#include "addon_install.h"
#include "pkg_extract.h"
#include "runtime.h"
#include "utf8.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;
using namespace aln;
static std::vector<std::string> registered_addons;
extern "C" int runtime_content_addon(const char *label, const char *) {
    registered_addons.emplace_back(label);
    return 0;
}
static int checks, failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++failures;                                                                                                \
            std::cout << "FAIL " << __LINE__ << ": " << #c << '\n';                                                    \
        }                                                                                                              \
    } while (0)
#ifdef _WIN32
static bool Junction(const fs::path &link, const fs::path &target) {
    fs::create_directory(link);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    struct {
        DWORD tag;
        WORD length, reserved, sub_offset, sub_length, print_offset, print_length;
        wchar_t text[2048];
    } data{};
    const auto print = fs::absolute(target).wstring(), sub = std::wstring(L"\\??\\") + print;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.sub_length = WORD(sub.size() * 2);
    data.print_offset = WORD((sub.size() + 1) * 2);
    data.print_length = WORD(print.size() * 2);
    memcpy(data.text, sub.c_str(), (sub.size() + 1) * 2);
    memcpy(reinterpret_cast<char *>(data.text) + data.print_offset, print.c_str(), (print.size() + 1) * 2);
    data.length = WORD(8 + data.print_offset + data.print_length + 2);
    DWORD done;
    const bool ok =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, DWORD(8 + data.length), nullptr, 0, &done, nullptr);
    CloseHandle(handle);
    return ok;
}
#endif
static std::string Sfo(const std::map<std::string, std::string> &fields) {
    std::string keys, values, index;
    const auto append = [&](auto value) { index.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
    for (const auto &[key, value] : fields) {
        append(uint16_t(keys.size()));
        append(uint16_t(0x0204));
        append(uint32_t(value.size() + 1));
        append(uint32_t(value.size() + 1));
        append(uint32_t(values.size()));
        keys += key + '\0';
        values += value + '\0';
    }
    std::string out("\0PSF", 4);
    for (uint32_t n :
         {0x101u, uint32_t(20 + index.size()), uint32_t(20 + index.size() + keys.size()), uint32_t(fields.size())})
        out.append(reinterpret_cast<const char *>(&n), 4);
    return out + index + keys + values;
}
static void Write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << text;
}
int main(int argc, char **argv) {
    auto fields = std::map<std::string, std::string>{{"CATEGORY", "ac"},
                                                     {"TITLE_ID", "CUSA00062"},
                                                     {"CONTENT_ID", "EP9000-CUSA00062_00-ANALLWEAPONS0001"},
                                                     {"TITLE", "Test DLC"}};
    Addon addon;
    CHECK(AddonIdentity(Sfo(fields), "CUSA00062", addon));
    CHECK(addon.label == "ANALLWEAPONS0001");
    CHECK(!AddonIdentity(Sfo(fields), "CUSA04430", addon));
    CHECK(!AddonIdentity(Sfo(fields), "../escape", addon));
    for (const auto &id : {"EP9000-CUSA00062_00-../evil000000000", "EP9000-CUSA00062_00-ANALLWEAPONS000/",
                           "EP9000-CUSA00062_00-ANALLWEAPONS0001X"}) {
        auto bad = fields;
        bad["CONTENT_ID"] = id;
        CHECK(!AddonIdentity(Sfo(bad), "CUSA00062", addon));
    }
    auto bad = fields;
    bad["CATEGORY"] = "gp";
    CHECK(!AddonIdentity(Sfo(bad), "CUSA00062", addon));
    CHECK(!AddonIdentity("not an sfo", "CUSA00062", addon));
    auto publisher = fields;
    publisher["CONTENT_ID"] = "UP9999-CUSA00062_00-ANALLWEAPONS0001";
    CHECK(!AddonIdentity(Sfo(publisher), "CUSA00062", addon));
    const auto scratch =
        fs::temp_directory_path() /
        ("aln-addon-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const GameRoots roots{scratch / "game" / "prepared", scratch / "game" / "image"};
#ifdef _WIN32
    // Exercise real Windows sharing violations, not only successful cleanup.
    for (bool release_early : {true, false}) {
        const auto staged = scratch / (release_early ? ".install-retry" : ".install-pending");
        const auto locked = staged / "sce_sys" / "held.bin";
        Write(locked, "held");
        HANDLE held = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        std::thread release;
        if (release_early)
            release = std::thread([held] {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                CloseHandle(held);
            });
        const auto note = CleanupAddonStaging(staged);
        if (release_early) {
            release.join();
            CHECK(note.empty());
            CHECK(!fs::exists(staged));
        } else {
            CHECK(note.find(Utf8(staged)) != std::string::npos);
            CHECK(fs::exists(locked));
            InstallResult pending;
            pending.status = InstallStatus::kCancelled;
            pending.detail = note;
            CHECK(DescribeAddonInstall(pending).text.find(Utf8(staged)) != std::string::npos);
            CloseHandle(held);
            CHECK(CleanupAddonStaging(staged).empty());
        }
    }
#endif
    CHECK(AddonRootsForApp0(roots.prepared / "CUSA00062-1.06").prepared ==
          AddonRootsForApp0(roots.prepared / "CUSA00062-1.06" / "").prepared);
    const auto root = AddonRoot(roots, "CUSA00062"), dir = root / "ANALLWEAPONS0001";
    Write(dir / "sce_sys" / "param.sfo", Sfo(fields));
    CHECK(InstalledAddons(roots, "CUSA00062").empty()); /* interrupted installs are not exposed */
    Write(dir / ".aln_addon", fields["CONTENT_ID"] + "\n");
    CHECK(InstalledAddons(roots, "CUSA00062").size() == 1);
    const auto app0 = roots.prepared / "CUSA00062-1.06";
    auto game = fields;
    game["CATEGORY"] = "gp";
    game["CONTENT_ID"] = "EP9000-CUSA00062_00-ALIENATIONGAMEEU";
    game["APP_VER"] = "01.06";
    game["VERSION"] = "01.00";
    Write(app0 / "sce_sys" / "param.sfo", Sfo(game));
    runtime_content_load_addons(Utf8(app0 / "").c_str());
    CHECK(registered_addons.size() == 1 && registered_addons[0] == "ANALLWEAPONS0001");
    registered_addons.clear();
    game["CATEGORY"] = "gd";
    game["APP_VER"] = "01.00";
    game["VERSION"] = "01.06";
    Write(app0 / "sce_sys" / "param.sfo", Sfo(game));
    runtime_content_load_addons(Utf8(app0).c_str());
    CHECK(registered_addons.size() == 1);
    registered_addons.clear();
    game["VERSION"] = "01.00";
    game["APP_VER"] = "01.00";
    Write(app0 / "sce_sys" / "param.sfo", Sfo(game));
    runtime_content_load_addons(Utf8(app0).c_str());
    CHECK(registered_addons.empty());
#ifdef _WIN32
    const auto owner = scratch / "owner-games-junction";
    CHECK(Junction(owner, scratch / "game"));
    CHECK(InstalledAddons({owner / "prepared", {}}, "CUSA00062").size() == 1);
    CHECK(RemoveDirectoryW(owner.c_str()));
    const auto moved = scratch / "parked-addcont", managed = root.parent_path();
    fs::rename(managed, moved);
    CHECK(Junction(managed, moved));
    CHECK(InstalledAddons(roots, "CUSA00062").empty());
    CHECK(RemoveDirectoryW(managed.c_str()));
    fs::rename(moved, managed);
#endif
    Write(dir / ".aln_addon", "wrong\n");
    CHECK(InstalledAddons(roots, "CUSA00062").empty());
    Write(dir / ".aln_addon", fields["CONTENT_ID"] + "\n");
    fs::rename(dir, root / "WRONGDIRECTORY00");
    CHECK(InstalledAddons(roots, "CUSA00062").empty());
    fs::rename(root / "WRONGDIRECTORY00", dir);
    CHECK(InstalledAddons(roots, "CUSA04430").empty());
    CHECK(!InstallAddon(scratch / "missing.pkg", roots, "CUSA00062").ok());
    CHECK(!InstallAddon(scratch / "missing.pkg", roots, "../escape").ok());
    const auto error = DescribeAddonInstall(InstallAddon(scratch / "missing.pkg", roots, "CUSA00062"));
    CHECK(error.title == "DLC installation failed" && !error.text.empty());
    InstallResult success;
    CHECK(DescribeAddonInstall(success).text.find("restart") != std::string::npos);
    std::error_code link_ec;
    fs::create_directory_symlink(dir, root / "LINKEDADDON00000", link_ec);
    if (!link_ec) {
        CHECK(InstalledAddons(roots, "CUSA00062").size() == 1);
        fs::remove(root / "LINKEDADDON00000");
    } else
        std::cout << "skipped: directory symlink test unavailable: " << link_ec.message() << '\n';
    const char *environment = std::getenv("ALN_TEST_DLC_PACKAGES");
    if (argc == 3 || (environment && *environment)) {
        const auto packages = PathFromUtf8(argc == 3 ? argv[1] : environment),
                   output = argc == 3 ? PathFromUtf8(argv[2]) : scratch / "real";
        const GameRoots real{output / "game" / "prepared", output / "game" / "image"};
        size_t count = 0;
        std::error_code packages_ec;
        if (!fs::is_directory(packages, packages_ec) || packages_ec) {
            std::cerr << "DLC package folder is missing or unreadable: " << Utf8(packages) << '\n';
            fs::remove_all(scratch);
            return 1;
        }
        for (const auto &item : fs::directory_iterator(packages)) {
            auto extension = Utf8(item.path().extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (extension != ".pkg")
                continue;
            const auto file = item.path();
            const auto before = fs::file_size(file),
                       time = uint64_t(fs::last_write_time(file).time_since_epoch().count());
            CHECK(!InstallAddon(file, real, "CUSA04430").ok());
            const GameRoots cancelled{scratch / "cancel" / "prepared", scratch / "cancel" / "image"};
            InstallHooks cancel;
            cancel.progress = [](uint64_t, uint64_t) { return false; };
            CHECK(InstallAddon(file, cancelled, "CUSA00062", cancel).status == InstallStatus::kCancelled);
            CHECK(InstalledAddons(cancelled, "CUSA00062").empty());
            CHECK(fs::is_empty(AddonRoot(cancelled, "CUSA00062")));
            int calls = 0;
            cancel.progress = [&](uint64_t, uint64_t) { return ++calls == 1; };
            CHECK(InstallAddon(file, cancelled, "CUSA00062", cancel).status == InstallStatus::kCancelled);
            CHECK(fs::is_empty(AddonRoot(cancelled, "CUSA00062")));
            if (count == 0) {
                // An incomplete existing destination is refused and preserved.
                // The actual first file can be any package: get its ID directly.
                Package p;
                CHECK(p.Open(file).ok());
                Addon identity;
                CHECK(AddonIdentity(p.sfo(), "CUSA00062", identity));
                const auto destination = AddonRoot(cancelled, "CUSA00062") / identity.label;
                Write(destination / "keep.txt", "untouched");
                const auto refused = InstallAddon(file, cancelled, "CUSA00062");
                CHECK(refused.status == InstallStatus::kForeignFolder);
                CHECK(fs::exists(destination / "keep.txt"));
                CHECK(refused.detail.find(Utf8(destination)) != std::string::npos);
                fs::remove_all(destination);
                CHECK(p.LoadFiles().ok());
                CHECK(p.extracted_bytes() > p.total_bytes());
            }
            auto result = InstallAddon(file, real, "CUSA00062");
            if (!result.ok())
                std::cout << "Install failed: " << Utf8(file) << ": " << result.detail << '\n';
            CHECK(result.ok());
            std::cout << result.recognition << '\n';
            CHECK(InstallAddon(file, real, "CUSA00062").status == InstallStatus::kUpToDate);
            CHECK(fs::file_size(file) == before &&
                  uint64_t(fs::last_write_time(file).time_since_epoch().count()) == time);
            ++count;
        }
        CHECK(count > 0);
        CHECK(InstalledAddons(real, "CUSA00062").size() == count);
        CHECK(!fs::exists(output / "user"));
    } else
        std::cout << "skipped: ALN_TEST_DLC_PACKAGES is not set (real DLC packages)\n";
    fs::remove_all(scratch); /* exclusive temporary directory created by this test */
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
