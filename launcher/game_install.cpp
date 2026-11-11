#include "game_install.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "boot_image.h"
#include "crypto.h"
#include "localization/ui_text.h"
#include "pkg_extract.h"
#include "utf8.h"

namespace aln {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSpaceMargin = 256ull << 20;
constexpr int kRenameAttempts = 10;

InstallResult Failure(InstallStatus status, std::string detail = {}) {
    InstallResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

fs::path WithSuffix(const fs::path& dir, const char* suffix) {
    return dir.parent_path() / (dir.filename().native() + PathFromUtf8(suffix).native());
}

bool IsWithin(const fs::path& child, const fs::path& parent) {
    const fs::path relative = child.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path Normal(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    return (ec ? path : absolute).lexically_normal();
}

// The antivirus scanner can hold a freshly written folder for a moment.
bool RenameWithRetry(const fs::path& from, const fs::path& to) {
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

// Hard links need both paths on one volume: the drive on Windows, the device of the nearest existing folder elsewhere.
bool SameVolume(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return a.root_name() == b.root_name();
#else
    const auto device = [](fs::path path) {
        std::error_code ec;
        while (!path.empty() && !fs::exists(path, ec)) {
            path = path.parent_path();
        }
        struct stat info;
        return !path.empty() && stat(path.c_str(), &info) == 0 ? std::optional<dev_t>(info.st_dev) : std::nullopt;
    };
    const auto first = device(a), second = device(b);
    return first && second && *first == *second;
#endif
}

// An existing folder is only replaced when it is empty or already holds what the installer writes there.
bool IsReplaceable(const fs::path& dir, const char* own_file) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return true;
    }
    return fs::is_directory(dir, ec) && (fs::is_empty(dir, ec) || fs::exists(dir / own_file, ec));
}

class StagingGuard {
public:
    explicit StagingGuard(fs::path dir) : dir_(std::move(dir)) {}
    ~StagingGuard() {
        if (armed_) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }
    void Release() { armed_ = false; }

private:
    fs::path dir_;
    bool armed_ = true;
};

struct SourceFile {
    fs::path relative;
    uint64_t size;
};

// Skipped as scripts/game_view.py skips them: extraction leftovers and an earlier preparation's marker.
bool IsSkipped(const fs::path& name) {
    const std::string text = Utf8(name);
    return text == ".pkg_extracted" || text == kPreparedMarker || (text.size() > 5 && text.ends_with(".part"));
}

std::string JsonQuote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

struct CopyContext {
    const InstallHooks* hooks;
    uint64_t done, total;
    bool cancelled;
};

enum class Transfer { kLinked, kCopied, kCancelled, kFailed };

#ifdef _WIN32
DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                            HANDLE, HANDLE, LPVOID data) {
    auto* context = static_cast<CopyContext*>(data);
    if (context->hooks->progress &&
        !context->hooks->progress(context->done + uint64_t(transferred.QuadPart), context->total)) {
        context->cancelled = true;
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}

// A hard link costs no space; another drive gets a copy.
Transfer TransferFile(const fs::path& from, const fs::path& to, CopyContext& copy, std::string& error) {
    if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) {
        return Transfer::kLinked;
    }
    if (CopyFileExW(from.c_str(), to.c_str(), CopyProgress, &copy, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
        return Transfer::kCopied;
    }
    error = "error " + std::to_string(GetLastError());
    return copy.cancelled ? Transfer::kCancelled : Transfer::kFailed;
}
#else
Transfer TransferFile(const fs::path& from, const fs::path& to, CopyContext& copy, std::string& error) {
    std::error_code ec;
    fs::create_hard_link(from, to, ec);
    if (!ec) {
        return Transfer::kLinked;
    }
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        error = "cannot open the file";
        return Transfer::kFailed;
    }
    std::vector<char> block(1u << 20);
    uint64_t transferred = 0;
    while (in.read(block.data(), std::streamsize(block.size())).gcount() > 0) {
        out.write(block.data(), in.gcount());
        transferred += uint64_t(in.gcount());
        if (!out) {
            error = "cannot write the file";
            return Transfer::kFailed;
        }
        if (copy.hooks->progress && !copy.hooks->progress(copy.done + transferred, copy.total)) {
            copy.cancelled = true;
            return Transfer::kCancelled;
        }
    }
    if (!in.eof() || !out.flush()) {
        error = "cannot copy the file";
        return Transfer::kFailed;
    }
    return Transfer::kCopied;
}
#endif

InstallStatus FromCheck(const GameCheck& check) {
    switch (check.state) {
    case GameFileState::kValid: return InstallStatus::kInstalled;
    case GameFileState::kMissing: return InstallStatus::kIncomplete;
    case GameFileState::kReadError: return InstallStatus::kReadFailed;
    case GameFileState::kDifferentVersion:
        return check.detail.starts_with("param.sfo TITLE_ID") || check.detail.starts_with("param.sfo CONTENT_ID")
                   ? InstallStatus::kWrongGame
                   : InstallStatus::kWrongVersion;
    }
    return InstallStatus::kIncomplete;
}

std::string HexDigest(const uint8_t (&digest)[32]) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

std::string PackageEbootSha256(Package& package) {
    std::vector<uint8_t> eboot;
    uint8_t digest[32];
    if (!package.ReadFile("eboot.bin", 512ull << 20, eboot).ok() || !crypto::Sha256(eboot.data(), eboot.size(), digest)) {
        return {};
    }
    return HexDigest(digest);
}

bool IsAlienation(const std::string& sfo) {
    std::string title = SfoString(sfo, "TITLE");
    for (char& c : title) {
        c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return title.find("ALIENATION") != std::string::npos;
}

// An Alienation build the port does not support is refused with its identity in the log; never run as a known build.
InstallResult UnsupportedOrFailure(const GameCheck& check, const std::string& sfo, const std::string& eboot_sha256) {
    if (check.state != GameFileState::kDifferentVersion) {
        return Failure(FromCheck(check), check.detail);
    }
    if (!IsAlienation(sfo)) {
        return Failure(InstallStatus::kWrongGame, check.detail);
    }
    const std::string sha = eboot_sha256.empty() ? "unreadable" : eboot_sha256;
    InstallResult result = Failure(InstallStatus::kUnsupportedBuild, check.detail);
    result.diagnostics = {
        "Unsupported Alienation build: TITLE_ID " + SfoString(sfo, "TITLE_ID") + ", APP_VER " + SfoString(sfo, "APP_VER") +
            ", VERSION " + SfoString(sfo, "VERSION") + ", CATEGORY " + SfoString(sfo, "CATEGORY") + ", eboot.bin sha256 " +
            sha,
        "Supported builds: " + SupportedBuildsText(),
    };
    return result;
}

InstallResult UnsupportedOrFailure(const GameCheck& check, const fs::path& folder) {
    std::ifstream in(folder / "sce_sys" / "param.sfo", std::ios::binary);
    const std::string sfo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return UnsupportedOrFailure(check, sfo, check.eboot_sha256.empty() ? Sha256Hex(folder / "eboot.bin") : check.eboot_sha256);
}

// The folders the installer writes: the prepared game and the boot image, each with a staging and a backup folder.
struct Layout {
    explicit Layout(const GamePaths& target)
        : app0(Normal(target.app0)), image(Normal(target.image_dir)), app0_stage(WithSuffix(app0, ".install-temp")),
          app0_backup(WithSuffix(app0, ".install-backup")), image_stage(WithSuffix(image, ".install-temp")),
          image_backup(WithSuffix(image, ".install-backup")) {}

    bool Overlaps(const fs::path& source) const {
        for (const fs::path& own : {app0, app0_stage, app0_backup, image, image_stage, image_backup}) {
            if (source == own || IsWithin(source, own) || IsWithin(own, source)) {
                return true;
            }
        }
        return false;
    }

    fs::path app0, image, app0_stage, app0_backup, image_stage, image_backup;
};

// A source inside, or holding, the folders of any supported build is refused before it is read.
bool OverlapsAnyBuild(const fs::path& source, const GameRoots& roots, std::span<const GameBuild> builds) {
    for (const GameBuild& build : builds) {
        if (Layout(BuildPaths(roots, build)).Overlaps(source)) {
            return true;
        }
    }
    return false;
}

// Creates the parent folders, checks the free space and clears old staging folders.
InstallResult PrepareStaging(const Layout& layout, uint64_t needed) {
    std::error_code ec;
    for (const fs::path& dir : {layout.app0.parent_path(), layout.image.parent_path()}) {
        fs::create_directories(dir, ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(dir) + ": " + ec.message());
        }
    }
    const fs::space_info space = fs::space(layout.app0.parent_path(), ec);
    if (!ec && needed && space.available < needed + kSpaceMargin) {
        return Failure(InstallStatus::kNoSpace,
                       "needs " + std::to_string(needed) + " bytes, " + std::to_string(space.available) + " available");
    }
    for (const fs::path& dir : {layout.app0_stage, layout.image_stage}) {
        fs::remove_all(dir, ec);
        if (ec || fs::exists(dir, ec)) {
            return Failure(InstallStatus::kWriteFailed, "cannot clear " + Utf8(dir));
        }
    }
    return {};
}

// The marker scripts/game_view.py writes, so the development tools see an installed game as prepared. A marker that is
// a link into an installed game is removed, not written through.
bool WriteMarker(const fs::path& app0, const fs::path& source, size_t linked, size_t copied, const GameBuild& build,
                 const char* scope) {
    std::error_code ec;
    fs::remove(app0 / kPreparedMarker, ec);
    std::ofstream marker(app0 / kPreparedMarker, std::ios::binary | std::ios::trunc);
    marker << "{\n  \"layers\": [\n    " << JsonQuote(Utf8(source)) << "\n  ],\n  \"description\": \""
           << build.title_id << ' ' << build.version << "\",\n  \"link\": " << linked << ",\n  \"copy\": " << copied
           << ",\n  \"replaced\": 0,\n  \"version\": \"" << build.version << "\",\n  \"scope\": \"" << scope << "\"\n}";
    return bool(marker.flush());
}

InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard);

// Builds the boot image from the staged game, checks both and swaps them in. Until the swap the current installation
// is untouched; a failed swap puts it back.
InstallResult BuildAndActivate(const Layout& layout, StagingGuard& app0_guard, StagingGuard& image_guard,
                               const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build,
                               uint64_t total) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    phase("Preparing the game...");
    if (const std::string error = BuildBootImage(layout.app0_stage, layout.image_stage); !error.empty()) {
        return Failure(InstallStatus::kPrepareFailed, error);
    }

    phase("Verifying the installation...");
    const GameCheck installed = CheckGameFiles({layout.app0_stage, layout.image_stage}, builds);
    if (installed.state != GameFileState::kValid || installed.build != &build) {
        return Failure(InstallStatus::kPrepareFailed, installed.detail);
    }
    if (hooks.progress && !hooks.progress(total, total)) {
        return Failure(InstallStatus::kCancelled);
    }
    phase("Finishing...");
    return Activate(layout, app0_guard, &image_guard);
}

// Puts the staged folders in place of the installed ones; without an image the installed image is removed, since it
// was built from another installation. A failed step puts the installed folders back.
InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard) {
    std::error_code ec;
    const fs::path &app0 = layout.app0, &image = layout.image;
    const bool had_app0 = fs::exists(app0, ec), had_image = fs::exists(image, ec);
    fs::remove_all(layout.app0_backup, ec);
    fs::remove_all(layout.image_backup, ec);
    if (had_app0 && !RenameWithRetry(app0, layout.app0_backup)) {
        return Failure(InstallStatus::kWriteFailed, "cannot move aside " + Utf8(app0));
    }
    if (!RenameWithRetry(layout.app0_stage, app0)) {
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(app0));
    }
    const auto restore_app0 = [&] {
        RenameWithRetry(app0, layout.app0_stage);
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
    };
    if (had_image && !RenameWithRetry(image, layout.image_backup)) {
        restore_app0();
        return Failure(InstallStatus::kWriteFailed, "cannot move aside " + Utf8(image));
    }
    if (image_guard && !RenameWithRetry(layout.image_stage, image)) {
        if (had_image) {
            RenameWithRetry(layout.image_backup, image);
        }
        restore_app0();
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(image));
    }
    app0_guard.Release();
    if (image_guard) {
        image_guard->Release();
    }
    fs::remove_all(layout.app0_backup, ec);
    fs::remove_all(layout.image_backup, ec);
    return {};
}

// Lays the files of `from` into `to` as links (copies across volumes); the installed game is not changed.
InstallResult LinkTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    std::vector<SourceFile> files;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            const uint64_t size = it->file_size(ec);
            files.push_back({it->path().lexically_relative(from), size});
            total += size;
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(from) + ": " + ec.message());
    }
    const InstallHooks no_progress;
    CopyContext copy{&no_progress, 0, total, false};
    for (const SourceFile& file : files) {
        fs::create_directories((to / file.relative).parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to / file.relative.parent_path()));
        }
        std::string error;
        if (TransferFile(from / file.relative, to / file.relative, copy, error) == Transfer::kFailed) {
            return Failure(InstallStatus::kWriteFailed, "cannot copy " + Utf8(file.relative) + " (" + error + ")");
        }
        copy.done += file.size;
    }
    return {};
}

// Lays the files of an unpacked update over the staged game. A staged file is replaced, never written through: it may
// be a link into the installed game.
InstallResult OverlayUpdate(const fs::path& update, const fs::path& staged) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(update, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(update) + ": " + ec.message());
    }
    for (const fs::path& from : files) {
        const fs::path to = staged / from.lexically_relative(update);
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to.parent_path()));
        }
        fs::remove(to, ec);
        fs::rename(from, to, ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot place " + Utf8(to));
        }
    }
    return {};
}

InstallStatus FromPackage(PackageResult::Code code) {
    switch (code) {
    case PackageResult::kOk: return InstallStatus::kInstalled;
    case PackageResult::kNotPackage: return InstallStatus::kNotPackage;
    case PackageResult::kUnsupported: return InstallStatus::kUnsupportedPackage;
    case PackageResult::kIncomplete: return InstallStatus::kIncomplete;
    case PackageResult::kReadFailed: return InstallStatus::kReadFailed;
    case PackageResult::kWriteFailed: return InstallStatus::kWriteFailed;
    case PackageResult::kCancelled: return InstallStatus::kCancelled;
    }
    return InstallStatus::kReadFailed;
}

// The build the launcher runs, when its files still pass the readiness check.
const GameBuild* ActiveValidBuild(const GameRoots& roots, std::span<const GameBuild> builds) {
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (!active) {
        return nullptr;
    }
    const GameCheck check = CheckGameFiles(BuildPaths(roots, *active), builds);
    return check.state == GameFileState::kValid && check.build == active ? active : nullptr;
}

// Makes a complete build the newest prepared one, which is how ActiveBuildPaths picks the active build. Only the small
// marker is written.
bool ActivatePrepared(const fs::path& app0) {
    std::error_code ec;
    fs::last_write_time(app0 / kPreparedMarker, fs::file_time_type::clock::now(), ec);
    return !ec;
}

// A selected build that needs no copying. The active build itself is up to date. A complete folder of any other build
// that passes the readiness check becomes the active one, whether it is older or newer. Nothing is returned when the
// build has to be installed.
std::optional<InstallResult> ReuseInstalledBuild(const GameRoots& roots, std::span<const GameBuild> builds,
                                                 const GameBuild& build, bool replace_active) {
    if (ActiveValidBuild(roots, builds) == &build) {
        if (replace_active) {
            return std::nullopt;
        }
        return Failure(InstallStatus::kUpToDate, build.label);
    }
    const GamePaths target = BuildPaths(roots, build);
    const GameCheck prepared = CheckGameFiles(target, builds);
    if (prepared.state != GameFileState::kValid || prepared.build != &build) {
        return std::nullopt;
    }
    if (!ActivatePrepared(target.app0)) {
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(target.app0));
    }
    InstallResult result;
    result.status = InstallStatus::kActivated;
    result.build = &build;
    return result;
}

// Install update upgrades the active game: an update of another title is refused, whatever other title is installed.
std::optional<InstallResult> RefuseUpdateOfOtherTitle(const GameRoots& roots, const InstallHooks& hooks,
                                                      std::span<const GameBuild> builds, const GameBuild& update) {
    if (hooks.expected != PackageKind::kUpdate || !IsUpdateBuild(update)) {
        return std::nullopt;
    }
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (!active || std::string_view(active->title_id) == update.title_id) {
        return std::nullopt;
    }
    InstallResult result = Failure(InstallStatus::kUpdateForOtherTitle, std::string(update.label) + " is an update of " +
                                                                             update.title_id + ", the active game is " +
                                                                             active->label);
    result.update_title_id = update.title_id;
    result.installed_title_id = active->title_id;
    return result;
}

// Install update never goes back: an update older than the active update of its title is refused. Going back is Remove
// update, or Install game with the base package.
std::optional<InstallResult> RefuseOlderUpdate(const GameRoots& roots, const InstallHooks& hooks,
                                               std::span<const GameBuild> builds, const GameBuild& update) {
    if (hooks.expected != PackageKind::kUpdate || !IsUpdateBuild(update)) {
        return std::nullopt;
    }
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (!active || active == &update || std::string_view(active->title_id) != update.title_id || !IsUpdateBuild(*active) ||
        std::string_view(FinishedLayer(update).app_ver) >= FinishedLayer(*active).app_ver) {
        return std::nullopt;
    }
    InstallResult result = Failure(InstallStatus::kOlderUpdate,
                                   std::string(update.label) + " is older than the active " + active->label);
    result.installed_label = active->label;
    return result;
}

// An update is laid over its base package only. A build installed from a complete repack already contains every update
// up to its own version and has no base layer to lay one over, whether it is active or its folder is the update's.
std::optional<InstallResult> RefuseUpdateOverRepack(const GameRoots& roots, const InstallHooks& hooks,
                                                    std::span<const GameBuild> builds, const GameBuild& update) {
    if (hooks.expected != PackageKind::kUpdate || !IsUpdateBuild(update)) {
        return std::nullopt;
    }
    const GameBuild* repack = nullptr;
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (InstalledProvenance(BuildPaths(roots, update).app0, update) == InstallProvenance::kRepack) {
        repack = &update;
    } else if (active && std::string_view(active->title_id) == update.title_id &&
               InstalledProvenance(BuildPaths(roots, *active).app0, *active) == InstallProvenance::kRepack &&
               std::string_view(FinishedLayer(update).app_ver) <= FinishedLayer(*active).app_ver) {
        repack = active;
    }
    if (!repack) {
        return std::nullopt;
    }
    InstallResult result = Failure(InstallStatus::kAlreadyContainsUpdate,
                                   std::string(update.label) + " is part of the complete package installed as " +
                                       repack->label);
    result.contained_version = repack->version;
    result.installed_label = BuildDisplayName(*repack);
    return result;
}

// The action's package kind, the active title and the active update's version, checked before anything is written.
std::optional<InstallResult> RefuseForAction(const GameRoots& roots, const InstallHooks& hooks,
                                             std::span<const GameBuild> builds, const GameBuild& build, bool update) {
    if (hooks.expected == PackageKind::kBase && update) {
        return Failure(InstallStatus::kExpectedBasePackage, std::string("update package for ") + build.label);
    }
    if (hooks.expected == PackageKind::kUpdate && !update) {
        return Failure(InstallStatus::kExpectedUpdatePackage, std::string("base package for ") + build.label);
    }
    if (std::optional<InstallResult> refused = RefuseUpdateOfOtherTitle(roots, hooks, builds, build)) {
        return refused;
    }
    if (std::optional<InstallResult> refused = RefuseUpdateOverRepack(roots, hooks, builds, build)) {
        return refused;
    }
    return RefuseOlderUpdate(roots, hooks, builds, build);
}

// Reinstalling the active, ready build repairs it with Install game; Install update never re-applies the active update.
bool RepairActive(const InstallHooks& hooks) {
    return hooks.replace_active && hooks.expected != PackageKind::kUpdate;
}

// A base package that names a supported build but carries another executable is refused before anything is unpacked;
// nothing is returned when the installation should go on. An update's executable is checked once it is laid over its base.
std::optional<InstallResult> RejectForeignExecutable(Package& package, const GameCheck& identity) {
    if (identity.layer > 0) {
        return std::nullopt;
    }
    const std::string sha = PackageEbootSha256(package);
    if (sha == identity.build->eboot_sha256 || (sha.empty() && !identity.alias)) {
        return std::nullopt;
    }
    GameCheck check;
    check.state = GameFileState::kDifferentVersion;
    check.detail =
        identity.alias ? "the repack's eboot.bin is not the supported build's" : "eboot.bin is not the supported build";
    check.build = identity.build;
    return UnsupportedOrFailure(check, package.sfo(), sha);
}

// A package that finishes its build by itself: the base package of a build without updates.
InstallResult InstallBasePackage(const fs::path& source, Package& package, const PackageResult& loaded, const Layout& layout,
                                 const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (!loaded.ok()) {
        return Failure(FromPackage(loaded.code), loaded.detail);
    }
    const uint64_t total = package.total_bytes();
    if (InstallResult prepared = PrepareStaging(layout, total); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    phase("Unpacking the game files...");
    if (const PackageResult extracted = package.Extract(layout.app0_stage, hooks.progress); !extracted.ok()) {
        return Failure(FromPackage(extracted.code), extracted.detail);
    }
    if (!WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    phase("Checking the game files...");
    const GameCheck unpacked = CheckGameTree(layout.app0_stage, builds);
    if (unpacked.state != GameFileState::kValid || unpacked.build != &build) {
        return UnsupportedOrFailure(unpacked, layout.app0_stage);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

// An update laid over the installed base build of its title, staged and checked like any other installation.
InstallResult InstallUpdatePackage(const fs::path& source, Package& package, const PackageResult& loaded, const Layout& layout,
                                   const GameRoots& roots, const InstallHooks& hooks, std::span<const GameBuild> builds,
                                   const GameBuild& build) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const GameBuild* base_build = BaseBuildOf(build, builds);
    if (!base_build) {
        return Failure(InstallStatus::kWrongVersion, std::string("no base package for ") + build.label);
    }
    const GamePaths base = BuildPaths(roots, *base_build);
    if (const GameCheck base_check = CheckGameTree(base.app0, builds);
        base_check.state != GameFileState::kValid || base_check.build != base_build) {
        return Failure(InstallStatus::kUpdateNeedsBase, base_check.detail);
    }
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (!loaded.ok()) {
        return Failure(FromPackage(loaded.code), loaded.detail);
    }
    const uint64_t total = package.total_bytes();
    // The base's files are linked; the update's are unpacked in full.
    if (InstallResult prepared = PrepareStaging(layout, total); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    const fs::path update_dir = WithSuffix(layout.app0, ".install-update");
    std::error_code ec;
    fs::remove_all(update_dir, ec);
    phase("Unpacking the update...");
    if (const PackageResult extracted = package.Extract(update_dir, hooks.progress); !extracted.ok()) {
        fs::remove_all(update_dir, ec);
        return Failure(FromPackage(extracted.code), extracted.detail);
    }
    phase("Copying the game files...");
    InstallResult staged = StageUpdate(base.app0, update_dir, layout.app0_stage);
    fs::remove_all(update_dir, ec);
    if (!staged.ok()) {
        return staged;
    }
    if (!WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    phase("Checking the game files...");
    const GameCheck merged = CheckGameTree(layout.app0_stage, builds);
    if (merged.state != GameFileState::kValid || merged.build != &build) {
        return UnsupportedOrFailure(merged, layout.app0_stage);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

} // namespace

InstallResult StageUpdate(const fs::path& base_app0, const fs::path& update_dir, const fs::path& staged_app0) {
    if (InstallResult linked = LinkTree(base_app0, staged_app0); !linked.ok()) {
        return linked;
    }
    return OverlayUpdate(update_dir, staged_app0);
}

namespace {

constexpr char kRemovingSuffix[] = ".removing";

bool IsLinkOrJunction(const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec))) {
        return true;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

// The build's own two folders, directly in the roots and named after the build; nothing else is ever removed.
bool OwnedFolders(const GameRoots& roots, const GameBuild& build, const Layout& layout) {
    const auto owned = [&](const fs::path& dir, const fs::path& root) {
        return dir.parent_path() == Normal(root) && dir.filename() == PathFromUtf8(build.folder) && !IsLinkOrJunction(dir) &&
               !IsLinkOrJunction(root);
    };
    return owned(layout.app0, roots.prepared) && owned(layout.image, roots.image);
}

bool RemoveAside(const fs::path& aside, std::string& detail) {
    std::error_code ec;
    if (!fs::exists(aside, ec)) {
        return true;
    }
    fs::remove_all(aside, ec);
    if (ec) {
        detail += (detail.empty() ? "" : "; ") + Utf8(aside) + " left for later: " + ec.message();
        return false;
    }
    return true;
}

} // namespace

RemoveResult RemoveInstalledUpdate(const GameRoots& roots, const GameBuild& build, std::span<const GameBuild> builds) {
    RemoveResult result;
    const auto fail = [&](RemoveStatus status, std::string detail) {
        result.status = status;
        result.detail = std::move(detail);
        return result;
    };
    if (!BaseBuildOf(build, builds)) {
        return fail(RemoveStatus::kNotUpdate, std::string(build.label) + " is not an update");
    }
    const Layout layout(BuildPaths(roots, build));
    if (!OwnedFolders(roots, build, layout)) {
        return fail(RemoveStatus::kForeignFolder, Utf8(layout.app0) + " is not the update's own folder");
    }
    std::error_code ec;
    if (!fs::exists(layout.app0, ec) && !fs::exists(layout.image, ec)) {
        return fail(RemoveStatus::kNotInstalled, std::string(build.label) + " is not installed");
    }
    std::string sfo;
    {
        std::ifstream sfo_file(layout.app0 / "sce_sys" / "param.sfo", std::ios::binary);
        sfo.assign(std::istreambuf_iterator<char>(sfo_file), std::istreambuf_iterator<char>());
    }
    if (fs::exists(layout.app0, ec) && CheckSfoIdentity(sfo, builds).build != &build) {
        return fail(RemoveStatus::kForeignFolder, Utf8(layout.app0) + " does not hold " + build.label);
    }
    if (const PackageAlias* alias = PackageAliasOf(build, sfo)) {
        return fail(RemoveStatus::kNotUpdate, std::string(build.label) + " was installed from a " + alias->name +
                                                   ", a complete game: no update to remove");
    }
    const fs::path app0_aside = WithSuffix(layout.app0, kRemovingSuffix);
    const fs::path image_aside = WithSuffix(layout.image, kRemovingSuffix);
    RemoveAside(app0_aside, result.detail);
    RemoveAside(image_aside, result.detail);
    if (fs::exists(app0_aside, ec) || fs::exists(image_aside, ec)) {
        return fail(RemoveStatus::kInUse, "an earlier removal is still in use: " + result.detail);
    }
    if (fs::exists(layout.app0, ec) && !RenameWithRetry(layout.app0, app0_aside)) {
        return fail(RemoveStatus::kInUse, Utf8(layout.app0) + " is in use");
    }
    if (fs::exists(layout.image, ec) && !RenameWithRetry(layout.image, image_aside)) {
        if (fs::exists(app0_aside, ec)) {
            RenameWithRetry(app0_aside, layout.app0);
        }
        return fail(RemoveStatus::kInUse, Utf8(layout.image) + " is in use");
    }
    RemoveAside(app0_aside, result.detail);
    RemoveAside(image_aside, result.detail);
    return result;
}

std::vector<const GameBuild*> InstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                                 std::span<const GameBuild> builds) {
    std::vector<const GameBuild*> updates;
    for (const GameBuild* build : PreparedBuilds(roots, builds)) {
        if (title_id == build->title_id &&
            InstalledProvenance(BuildPaths(roots, *build).app0, *build) == InstallProvenance::kUpdateLayers) {
            updates.push_back(build);
        }
    }
    return updates;
}

RemoveAllResult RemoveInstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                         std::span<const GameBuild> builds) {
    RemoveAllResult result;
    for (const GameBuild& build : builds) {
        if (!IsUpdateBuild(build) || title_id != build.title_id ||
            InstalledProvenance(BuildPaths(roots, build).app0, build) == InstallProvenance::kRepack) {
            continue;
        }
        const RemoveResult removed = RemoveInstalledUpdate(roots, build, builds);
        if (removed.status == RemoveStatus::kRemoved) {
            ++result.removed;
        } else if (removed.status != RemoveStatus::kNotInstalled) {
            ++result.failed;
        }
        if (!removed.detail.empty() && removed.status != RemoveStatus::kNotInstalled) {
            result.detail += (result.detail.empty() ? "" : "; ") + removed.detail;
        }
    }
    for (const GameBuild& build : builds) {
        if (IsUpdateBuild(build) || title_id != build.title_id) {
            continue;
        }
        const fs::path base_app0 = BuildPaths(roots, build).app0;
        if (result.removed > 0 && PreparationScope(base_app0) == "full") {
            ActivatePrepared(base_app0);
        }
        result.base_selected = LastPreparedBuild(roots, builds) == &build;
    }
    return result;
}

bool FinishInterruptedRemovals(const GameRoots& roots, std::span<const GameBuild> builds) {
    bool clean = true;
    std::string detail;
    for (const GameBuild& build : builds) {
        if (!IsUpdateBuild(build)) {
            continue;
        }
        const Layout layout(BuildPaths(roots, build));
        if (!OwnedFolders(roots, build, layout)) {
            continue;
        }
        clean = RemoveAside(WithSuffix(layout.app0, kRemovingSuffix), detail) && clean;
        clean = RemoveAside(WithSuffix(layout.image, kRemovingSuffix), detail) && clean;
    }
    return clean;
}

bool RefusedPackageKind(InstallStatus status) {
    return status == InstallStatus::kExpectedBasePackage || status == InstallStatus::kExpectedUpdatePackage;
}

InstallResult InstallGame(const fs::path& source, const GameRoots& roots, const InstallHooks& hooks,
                          std::span<const GameBuild> builds) {
    std::error_code ec;
    return fs::is_directory(source, ec) ? InstallFromFolder(source, roots, hooks, builds)
                                        : InstallFromPackage(source, roots, hooks, builds);
}

// The package as recognized from its param.sfo: its build, whether it is the base or an update, and the base an update
// goes over.
static std::string PackageRecognition(const fs::path& file, const GameCheck& identity, std::span<const GameBuild> builds) {
    const GameBuild& build = *identity.build;
    if (identity.alias) {
        return "package recognized: " + Utf8(file.filename()) + " is " + BuildDisplayName(build) + " (" +
               identity.alias->name + ", " + BuildMetadataText(build, identity.alias) + ", CATEGORY " +
               identity.alias->category + ", accepted only with the eboot.bin " + build.eboot_sha256 + ")";
    }
    std::string text = "package recognized: " + Utf8(file.filename()) + " is " + build.label + " (" +
                       (identity.layer > 0 ? "update" : "base") + ", " + BuildMetadataText(build) + ", layer " +
                       std::to_string(identity.layer + 1) + " of " + std::to_string(build.layers.size());
    if (const GameBuild* base = BaseBuildOf(build, builds); base && identity.layer > 0) {
        text += std::string(", laid over ") + base->label;
    }
    return text + ")";
}

InstallResult InstallFromPackage(const fs::path& package_file, const GameRoots& roots, const InstallHooks& hooks,
                                 std::span<const GameBuild> builds) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const fs::path source = Normal(package_file);

    phase("Checking the package...");
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return Failure(InstallStatus::kNotPackage, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    Package package;
    if (const PackageResult opened = package.Open(source); !opened.ok()) {
        return Failure(FromPackage(opened.code), opened.detail);
    }
    const GameCheck identity = CheckPackageIdentity(package.sfo(), builds);
    if (identity.state != GameFileState::kValid) {
        if (identity.state == GameFileState::kDifferentVersion && package.LoadFiles().ok()) {
            return UnsupportedOrFailure(identity, package.sfo(), PackageEbootSha256(package));
        }
        return Failure(FromCheck(identity), identity.detail);
    }
    const GameBuild& build = *identity.build;
    const std::string recognition = PackageRecognition(source, identity, builds);
    const auto recognized = [&](InstallResult result) {
        result.recognition = recognition;
        result.repack = identity.alias != nullptr;
        return result;
    };
    if (std::optional<InstallResult> refused = RefuseForAction(roots, hooks, builds, build, identity.layer > 0)) {
        return recognized(*refused);
    }
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, RepairActive(hooks))) {
        return recognized(*reused);
    }
    const PackageResult loaded = package.LoadFiles();
    if (loaded.ok()) {
        if (std::optional<InstallResult> foreign = RejectForeignExecutable(package, identity)) {
            return recognized(*foreign);
        }
    }
    const Layout layout(BuildPaths(roots, build));
    if (identity.layer > 0) {
        return recognized(InstallUpdatePackage(source, package, loaded, layout, roots, hooks, builds, build));
    }
    return recognized(InstallBasePackage(source, package, loaded, layout, hooks, builds, build));
}

InstallResult InstallFromFolder(const fs::path& source_dir, const GameRoots& roots, const InstallHooks& hooks,
                                std::span<const GameBuild> builds) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    std::error_code ec;
    const fs::path source = Normal(source_dir);

    phase("Checking the game folder...");
    if (!fs::is_regular_file(source / "eboot.bin", ec) || !fs::is_regular_file(source / "sce_sys" / "param.sfo", ec)) {
        return Failure(InstallStatus::kNotGameFolder, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    const GameCheck check = CheckGameTree(source, builds);
    if (check.state != GameFileState::kValid) {
        return UnsupportedOrFailure(check, source);
    }
    const GameBuild& build = *check.build;
    if (std::optional<InstallResult> refused =
            RefuseForAction(roots, hooks, builds, build, IsUpdateBuild(build) && !check.alias)) {
        refused->repack = check.alias != nullptr;
        return *refused;
    }
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, RepairActive(hooks))) {
        return *reused;
    }
    const Layout layout(BuildPaths(roots, build));
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }

    std::vector<SourceFile> files;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (IsSkipped(it->path().filename())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(ec)) {
            const uint64_t size = it->file_size(ec);
            files.push_back({it->path().lexically_relative(source), size});
            total += size;
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(source) + ": " + ec.message());
    }

    // The same drive gets hard links, which need no space.
    if (InstallResult prepared = PrepareStaging(layout, SameVolume(source, layout.app0) ? 0 : total);
        !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    phase("Copying the game files...");
    CopyContext copy{&hooks, 0, total, false};
    size_t linked = 0;
    for (const SourceFile& file : files) {
        const fs::path from = source / file.relative, to = layout.app0_stage / file.relative;
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to.parent_path()) + ": " + ec.message());
        }
        std::string error;
        switch (TransferFile(from, to, copy, error)) {
        case Transfer::kLinked: ++linked; break;
        case Transfer::kCopied: break;
        case Transfer::kCancelled: return Failure(InstallStatus::kCancelled);
        case Transfer::kFailed:
            return Failure(InstallStatus::kWriteFailed, "cannot copy " + Utf8(file.relative) + " (" + error + ")");
        }
        copy.done += file.size;
        if (hooks.progress && !hooks.progress(copy.done, total)) {
            return Failure(InstallStatus::kCancelled);
        }
    }
    if (!WriteMarker(layout.app0_stage, source, linked, files.size() - linked, build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = files.size();
    result.linked = linked;
    result.build = &build;
    return result;
}

InstallMessage DescribeInstall(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled:
        return {Tr("Game installed"), Tr("Alienation is ready to play.")};
    case InstallStatus::kCancelled:
        return {Tr("Installation cancelled"), Tr("The installed game was not changed.")};
    case InstallStatus::kNotPackage:
        return {Tr("Not a PS4 package"), Tr("Select the Alienation .pkg file.")};
    case InstallStatus::kUnsupportedPackage:
        return {Tr("This package cannot be unpacked"),
                Tr("It is encrypted for a PS4 console. Select a decrypted (fake) PKG of the game.")};
    case InstallStatus::kNotGameFolder:
        return {Tr("Not a game folder"),
                Tr("Select the folder that contains eboot.bin, sce_module and sce_sys.")};
    case InstallStatus::kWrongGame:
        return {Tr("Wrong game"), Tr("This is not Alienation.")};
    case InstallStatus::kWrongVersion:
        return {Tr("Different version of Alienation"),
                Tr("This port supports ") + SupportedBuildsText() + "."};
    case InstallStatus::kIncomplete:
        return {Tr("Game files missing"),
                Tr("The package is incomplete or some of the game's files are missing. Copy or download it again.")};
    case InstallStatus::kReadFailed:
        return {Tr("Could not read the game files"),
                Tr("The package could not be read or is damaged. Check the file, then try again.")};
    case InstallStatus::kOverlapping:
        return {Tr("Choose another file"),
                Tr("Select your game package, not a file inside the port's own game folder.")};
    case InstallStatus::kForeignFolder:
        return {Tr("Could not install the game"),
                Tr("The port's game folder holds other files. Move them out and try again.")};
    case InstallStatus::kNoSpace:
        return {Tr("Not enough free space"),
                Tr("The game needs about 2.5 GB and an update up to 1.7 GB more. Free up disk space and try again.")};
    case InstallStatus::kWriteFailed:
        return {Tr("Could not install the game"), Tr("The game files could not be written next to the port.")};
    case InstallStatus::kPrepareFailed:
        return {Tr("Could not prepare the game"),
                Tr("The game's code could not be prepared. The log has the details.")};
    case InstallStatus::kUpdateNeedsBase:
        return {Tr("Base game required"),
                Tr("This package is an update. Install the base game package of the same title ID (CUSA number) first, "
                   "then install the update.")};
    case InstallStatus::kActivated:
        return {Tr("Game activated"), Tr("The selected build is now the active game.")};
    case InstallStatus::kUpToDate:
        return {Tr("Already up to date"), Tr("This version is already the active game.")};
    case InstallStatus::kUnsupportedBuild:
        return {Tr("Alienation version not supported yet"),
                Tr("This port supports ") + SupportedBuildsText() +
                    Tr(". The package's identity was written to the latest log; share it when reporting this version.")};
    case InstallStatus::kExpectedBasePackage:
        return {Tr("This is an update package"),
                Tr("Install game needs the base game. Use Install update for an update package.")};
    case InstallStatus::kExpectedUpdatePackage:
        return {Tr("This is the base game package"),
                Tr("Install update needs an update package. Use Install game for the base game.")};
    case InstallStatus::kUpdateForOtherTitle:
        return {Tr("Update for another game version"),
                Tr("Install the matching game version before applying this update.")};
    case InstallStatus::kAlreadyContainsUpdate:
        return {Tr("This game already contains this update"),
                Tr("It was installed from a complete package that includes the update, so there is nothing to apply. "
                   "Nothing was changed.")};
    case InstallStatus::kOlderUpdate:
        return {Tr("A newer update is installed"),
                Tr("This update is older than the installed one. Remove the installed update first to go back.")};
    }
    return {Tr("Could not install the game"), Tr("An unexpected error occurred.")};
}

InstallMessage DescribeInstall(const InstallResult& result) {
    InstallMessage message = DescribeInstall(result.status);
    if (result.status == InstallStatus::kUpdateForOtherTitle) {
        message.title = Tr("This update is for ") + result.update_title_id + Tr(", but the installed game is ") +
                        result.installed_title_id;
    } else if (result.status == InstallStatus::kOlderUpdate) {
        message.title = Tr("A newer update is installed: ") + result.installed_label;
    } else if (result.status == InstallStatus::kAlreadyContainsUpdate) {
        message.title = Tr("This game already contains version ") + result.contained_version;
    } else if (result.status == InstallStatus::kExpectedUpdatePackage && result.repack) {
        message = {Tr("This is a complete game package"),
                   Tr("It holds the base game and its update together. Use Install game for it.")};
    }
    return message;
}

} // namespace aln
