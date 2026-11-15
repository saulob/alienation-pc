#include "addon_install.h"
#include "pkg_extract.h"
#include "utf8.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aln {
namespace fs = std::filesystem;
namespace {
constexpr const char *kMarker = ".aln_addon";
constexpr size_t kMaxAddons = 256;
bool SafeLabel(std::string_view label) {
    return label.size() == 16 && std::all_of(label.begin(), label.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
           });
}
bool Title(std::string_view id) {
    return id.size() == 9 && id.substr(0, 4) == "CUSA" &&
           std::all_of(id.begin() + 4, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string ReadSmall(const fs::path &file) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec)) || fs::file_size(file, ec) > 65536 || ec)
        return {};
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
bool PlainDirectory(const fs::path &dir) {
#ifdef _WIN32
    const auto attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
#endif
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(dir, ec)) && !ec;
}
// Reject symlinks/junctions in all existing ancestors of the managed path.
bool PlainAncestors(fs::path dir) {
    // AddonRoot already resolves the game root. Check only our managed subtree;
    // a junction used by the owner to locate the port is legitimate.
    const auto boundary = dir.parent_path().parent_path();
    std::error_code ec;
    dir = fs::absolute(dir, ec).lexically_normal();
    if (ec)
        return false;
    for (; !dir.empty(); dir = dir.parent_path()) {
#ifdef _WIN32
        const auto attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
#endif
        auto status = fs::symlink_status(dir, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return false;
        if (!ec && fs::exists(status) && !fs::is_directory(status))
            return false;
        if (dir == boundary || dir == dir.parent_path())
            break;
    }
    return true;
}
InstallResult Failure(InstallStatus status, std::string detail) {
    InstallResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}
struct Stage {
    fs::path path;
    std::string *exception_note = nullptr;
    std::string Cleanup() {
        const auto current = path;
        path.clear();
        return current.empty() ? std::string() : CleanupAddonStaging(current);
    }
    ~Stage() {
        if (path.empty())
            return;
        const auto pending = Cleanup();
        if (!pending.empty()) {
            if (exception_note)
                *exception_note = pending;
            std::fprintf(stderr, "%s\n", pending.c_str());
        }
    }
};
} // namespace
std::string CleanupAddonStaging(const fs::path &directory) {
    if (!Utf8(directory.filename()).starts_with(".install-"))
        return "Refused cleanup of a non-staging directory: " + Utf8(directory);
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::error_code ec;
        fs::remove_all(directory, ec);
        if (!ec)
            return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return "DLC staging cleanup pending: " + Utf8(directory) +
           ". Close other programs and move this temporary folder aside before retrying.";
}
fs::path AddonRoot(const GameRoots &roots, std::string_view title_id) {
    if (!Title(title_id))
        return {};
    std::error_code ec;
    const auto game = fs::weakly_canonical(fs::absolute(roots.prepared.parent_path(), ec), ec);
    return ec ? fs::path() : game / "addcont" / std::string(title_id);
}
GameRoots AddonRootsForApp0(const fs::path &app0) {
    auto path = fs::absolute(app0).lexically_normal();
    while (path.filename().empty() && path != path.parent_path())
        path = path.parent_path();
    return {path.parent_path(), {}};
}
bool AddonIdentity(const std::string &sfo, std::string_view title_id, Addon &addon) {
    if (!Title(title_id) || SfoString(sfo, "CATEGORY") != "ac" || SfoString(sfo, "TITLE_ID") != title_id)
        return false;
    const auto id = SfoString(sfo, "CONTENT_ID");
    // PS4 content ID: EP9000-CUSA00062_00-<16 character label> (36 bytes).
    if (id.size() != 36 || id[6] != '-' || id.substr(7, 9) != title_id || id.substr(16, 4) != "_00-" ||
        !SafeLabel(std::string_view(id).substr(20)))
        return false;
    if (!std::all_of(id.begin(), id.begin() + 6,
                     [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
        return false;
    const GameBuild *supported = nullptr;
    for (const auto &build : SupportedBuilds())
        if (build.title_id == title_id) {
            supported = &build;
            break;
        }
    if (!supported || id.substr(0, 19) != std::string(supported->content_id).substr(0, 19))
        return false;
    addon.content_id = id;
    addon.label = id.substr(20);
    addon.title = SfoString(sfo, "TITLE");
    return true;
}
std::vector<Addon> InstalledAddons(const GameRoots &roots, std::string_view title_id,
                                   std::vector<std::string> *diagnostics) {
    std::vector<Addon> out;
    const auto root = AddonRoot(roots, title_id);
    const auto note = [&](const std::string &text) {
        if (diagnostics)
            diagnostics->push_back(text);
    };
    note("Scanning add-ons: " + Utf8(root));
    if (root.empty() || !PlainAncestors(root)) {
        note("Unsafe managed add-on root (links/cloud placeholders inside the managed subtree are unsupported): " +
             Utf8(root));
        return out;
    }
    if (!PlainDirectory(root)) {
        note("No add-on directory: " + Utf8(root));
        return out;
    }
    std::error_code ec;
    for (fs::directory_iterator i(root, ec), end; !ec && i != end; i.increment(ec)) {
        const auto dir = i->path();
        if (!PlainDirectory(dir) || !PlainDirectory(dir / "sce_sys")) {
            note("Skipping incomplete/unsafe add-on: " + Utf8(dir));
            continue;
        }
        Addon addon;
        if (!AddonIdentity(ReadSmall(dir / "sce_sys" / "param.sfo"), title_id, addon) ||
            dir.filename() != addon.label || ReadSmall(dir / kMarker) != addon.content_id + "\n") {
            note("Skipping invalid/incomplete add-on: " + Utf8(dir));
            continue;
        }
        // The extractor only writes regular files/directories. Refuse a later symlink anywhere in an add-on.
        bool safe = true;
        for (fs::recursive_directory_iterator j(dir, ec), last; !ec && j != last; j.increment(ec)) {
#ifdef _WIN32
            const auto attr = GetFileAttributesW(j->path().c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                safe = false;
                break;
            }
#endif
            const auto s = j->symlink_status(ec);
            if (ec || (!fs::is_regular_file(s) && !fs::is_directory(s))) {
                safe = false;
                break;
            }
        }
        if (!safe || ec) {
            note("Skipping unsafe/unreadable add-on: " + Utf8(dir));
            ec.clear();
            continue;
        }
        addon.directory = dir;
        out.push_back(std::move(addon));
        if (out.size() == kMaxAddons)
            break;
    }
    std::sort(out.begin(), out.end(), [](const Addon &a, const Addon &b) { return a.label < b.label; });
    return out;
}
InstallResult InstallAddon(const fs::path &source, const GameRoots &roots, std::string_view title_id,
                           const InstallHooks &hooks) {
    std::string cleanup_note;
    try {
        if (!Title(title_id))
            return Failure(InstallStatus::kWrongGame, "No supported active title");
        std::error_code source_ec;
        if (!fs::is_regular_file(source, source_ec) || fs::file_size(source, source_ec) > (512ull << 20) || source_ec)
            return Failure(InstallStatus::kNotPackage, "Expected a readable local DLC package, at most 512 MiB");
        Package pkg;
        auto result = pkg.Open(source);
        if (!result.ok())
            return Failure(InstallStatus::kNotPackage, result.detail);
        Addon addon;
        if (!AddonIdentity(pkg.sfo(), title_id, addon) || pkg.content_id() != addon.content_id)
            return Failure(InstallStatus::kWrongGame,
                           "Expected a DLC package (CATEGORY ac) for " + std::string(title_id));
        const auto installed = InstalledAddons(roots, title_id);
        for (const auto &old : installed)
            if (old.label == addon.label) {
                if (old.content_id != addon.content_id)
                    return Failure(InstallStatus::kWrongGame, "Conflicting add-on identity");
                return Failure(InstallStatus::kUpToDate, "DLC already installed: " + addon.title);
            }
        if (installed.size() >= kMaxAddons)
            return Failure(InstallStatus::kWriteFailed, "Add-on limit reached");
        result = pkg.LoadFiles();
        if (!result.ok())
            return Failure(InstallStatus::kUnsupportedPackage, result.detail);
        const auto total = pkg.extracted_bytes();
        if (total > (256ull << 20))
            return Failure(InstallStatus::kIncomplete, "DLC payload and metadata exceed 256 MiB local installer limit");
        const auto root = AddonRoot(roots, title_id), target = root / addon.label;
        if (root.empty() || !PlainAncestors(root))
            return Failure(InstallStatus::kForeignFolder, "Unsafe managed add-on directory (links/cloud placeholders "
                                                          "inside the managed subtree are unsupported): " +
                                                              Utf8(root));
        std::error_code ec;
        if (fs::exists(target, ec) || ec)
            return Failure(InstallStatus::kForeignFolder,
                           "DLC destination exists or cannot be checked: " + Utf8(target) +
                               ". Close the game, move this incomplete/conflicting folder aside, then retry. Nothing "
                               "was overwritten.");
        fs::create_directories(root, ec);
        if (ec)
            return Failure(InstallStatus::kWriteFailed, ec.message());
        const auto space = fs::space(root, ec);
        if (ec || space.available < total + (16ull << 20))
            return Failure(InstallStatus::kNoSpace, "Cannot verify sufficient free space at " + Utf8(root));
        Stage stage;
        stage.exception_note = &cleanup_note;
        const auto fail_staged = [&](InstallStatus status, std::string detail) {
            const auto pending = stage.Cleanup();
            if (!pending.empty())
                detail += (detail.empty() ? "" : " ") + pending;
            return Failure(status, std::move(detail));
        };
        // An exclusively created staging folder: concurrent installs never share extraction paths.
        for (int n = 0; n < 100; ++n) {
            auto candidate = root / (".install-" + addon.label + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                     std::to_string(n));
            if (fs::create_directory(candidate, ec)) {
                stage.path = candidate;
                break;
            }
            if (ec)
                return Failure(InstallStatus::kWriteFailed, ec.message());
        }
        if (stage.path.empty())
            return Failure(InstallStatus::kWriteFailed, "Cannot create exclusive DLC staging folder");
        if (hooks.phase)
            hooks.phase("Installing DLC...");
        if (hooks.progress && !hooks.progress(0, total))
            return fail_staged(InstallStatus::kCancelled, {});
        result = pkg.Extract(stage.path,
                             [&](uint64_t done, uint64_t) { return !hooks.progress || hooks.progress(done, total); });
        if (!result.ok())
            return fail_staged(result.code == PackageResult::kCancelled ? InstallStatus::kCancelled
                                                                        : InstallStatus::kWriteFailed,
                               result.detail);
        Addon extracted;
        if (!AddonIdentity(ReadSmall(stage.path / "sce_sys" / "param.sfo"), title_id, extracted) ||
            extracted.content_id != addon.content_id)
            return fail_staged(InstallStatus::kIncomplete, "Extracted DLC metadata differs from package");
        {
            std::ofstream marker(stage.path / kMarker, std::ios::binary);
            marker << addon.content_id << '\n';
            marker.close();
            if (!marker)
                return fail_staged(InstallStatus::kWriteFailed, "Cannot finish DLC marker");
        }
        if (hooks.progress && !hooks.progress(total, total))
            return fail_staged(InstallStatus::kCancelled, {});
        fs::rename(stage.path, target, ec);
        if (ec)
            return fail_staged(InstallStatus::kWriteFailed, ec.message());
        stage.path.clear();
        InstallResult out;
        out.files = pkg.file_count();
        out.bytes = pkg.total_bytes();
        out.recognition = "DLC installed: " + addon.title + " (" + addon.content_id + ")";
        return out;
    } catch (const std::exception &e) {
        return Failure(InstallStatus::kWriteFailed,
                       std::string(e.what()) + (cleanup_note.empty() ? "" : " " + cleanup_note));
    }
}
InstallMessage DescribeAddonInstall(const InstallResult &result) {
    if (result.ok())
        return {"DLC installed", (result.recognition.empty() ? result.detail : result.recognition) +
                                     ". Start or restart the game to use installed DLC."};
    if (result.status == InstallStatus::kCancelled)
        return {"DLC installation cancelled",
                "Existing files were kept." + (result.detail.empty() ? std::string() : " " + result.detail)};
    return {"DLC installation failed", result.detail};
}
} // namespace aln
