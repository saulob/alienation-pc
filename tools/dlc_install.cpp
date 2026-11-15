// Manual batch installer. Reads the player's packages and adds DLC beside a verified installed game.
#include "addon_install.h"
#include "game_process.h"
#include "utf8.h"
#include <algorithm>
#include <iostream>
namespace fs = std::filesystem;
static int Run(const std::vector<fs::path> &args) {
    using namespace aln;
    if (args.size() != 3) {
        std::cerr << "Usage: aln-dlc-install PORT_FOLDER DLC_PACKAGE_OR_FOLDER|--list\n";
        return 2;
    }
    try {
        const auto root = fs::absolute(args[1]);
        if (OtherProcessRunning(root / "alienation.exe") || OtherProcessRunning(root / "alienation")) {
            std::cerr << "Close Alienation before installing DLC.\n";
            return 2;
        }
        const GameRoots roots{root / "game" / "prepared", root / "game" / "image"};
        const auto pin = PinnedBuildFromEnvironment(roots);
        if (!pin.error.empty()) {
            std::cerr << pin.error << '\n';
            return 2;
        }
        const auto check = CheckGameFiles(pin.build ? BuildPaths(roots, *pin.build) : ActiveBuildPaths(roots));
        if (check.state != GameFileState::kValid || !check.build) {
            std::cerr << "No verified supported game installed.\n";
            return 2;
        }
        if (std::string_view(check.build->title_id) == "CUSA00062" &&
            std::string_view(check.build->version) != "1.06") {
            std::cerr << "Install European update 1.06 before DLC.\n";
            return 2;
        }
        std::cout << "DLC target: " << check.build->title_id << " " << check.build->version << '\n';
        if (args[2] == "--list") {
            const auto addons = InstalledAddons(roots, check.build->title_id);
            for (const auto &addon : addons)
                std::cout << addon.label << '\n';
            std::cout << addons.size() << " installed DLCs\n";
            return 0;
        }
        std::vector<fs::path> packages;
        if (fs::is_directory(args[2])) {
            for (const auto &entry : fs::directory_iterator(args[2])) {
                auto ext = Utf8(entry.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                if (entry.is_regular_file() && ext == ".pkg")
                    packages.push_back(entry.path());
            }
        } else
            packages.push_back(args[2]);
        std::sort(packages.begin(), packages.end());
        if (packages.empty()) {
            std::cerr << "No .pkg files found.\n";
            return 2;
        }
        size_t failures = 0;
        for (const auto &file : packages) {
            const auto result = InstallAddon(file, roots, check.build->title_id);
            if (!result.ok()) {
                ++failures;
                std::cerr << Utf8(file.filename()) << ": " << result.detail << '\n';
            } else
                std::cout << (result.recognition.empty() ? result.detail : result.recognition) << '\n';
        }
        std::cout << InstalledAddons(roots, check.build->title_id).size() << " installed DLCs, " << failures
                  << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<fs::path> args;
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);
    return Run(args);
}
#else
int main(int argc, char **argv) {
    std::vector<fs::path> args;
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);
    return Run(args);
}
#endif
