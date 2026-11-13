// Installs the game from the player's own fake PKG (or an already extracted game folder): checks it, unpacks or lays
// it out in staging folders next to the prepared game, builds the boot image, checks the result and only then swaps it
// in. Settings, saves, logs and the shader cache live elsewhere and are never touched.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "game_files.h"

namespace aln {

enum class InstallStatus {
    kInstalled,
    kActivated,
    kCancelled,
    kNotPackage,
    kUnsupportedPackage,
    kNotGameFolder,
    kWrongGame,
    kWrongVersion,
    kIncomplete,
    kReadFailed,
    kOverlapping,
    kForeignFolder,
    kNoSpace,
    kWriteFailed,
    kPrepareFailed,
    kUpdateNeedsBase,
    kUpToDate,
    kUnsupportedBuild,
    kExpectedBasePackage,
    kExpectedUpdatePackage,
    kUpdateForOtherTitle,
    kOlderUpdate,
    kAlreadyContainsUpdate,
};

// What the player asked to install. An update and a base package are refused for each other's action.
enum class PackageKind { kAny, kBase, kUpdate };

struct InstallResult {
    InstallStatus status = InstallStatus::kInstalled;
    std::string detail; // for the log, not the UI
    std::vector<std::string> diagnostics; // kUnsupportedBuild: the package's identity, for the log
    std::string recognition; // a recognized package: its file, build, kind and layer, for the log
    std::string update_title_id, installed_title_id; // kUpdateForOtherTitle
    std::string installed_label; // kOlderUpdate: the newer installed build
    std::string contained_version; // kAlreadyContainsUpdate: the version the installed complete package already is
    bool complete_package = false;         // the package is one of the build's PackageAlias complete packages
    uint64_t bytes = 0;
    size_t files = 0, linked = 0;
    const GameBuild* build = nullptr; // the build that was installed
    bool ok() const {
        return status == InstallStatus::kInstalled || status == InstallStatus::kActivated || status == InstallStatus::kUpToDate;
    }
};

struct InstallHooks {
    std::function<void(const char* phase)> phase;
    // Returns false to cancel. Cancelling is possible until the new files are activated.
    std::function<bool(uint64_t done, uint64_t total)> progress;
    bool replace_active = false; // install the package even when its build is the active, ready one
    PackageKind expected = PackageKind::kAny;
};

// The build is the one the source identifies as; it is installed into BuildPaths(roots, build). An update is laid over the
// installed base build of its title (kUpdateNeedsBase when that is not installed). Install update (expected kUpdate)
// refuses, before anything is written, an update of another title than the active build's (kUpdateForOtherTitle), one
// older than the active update of its title (kOlderUpdate) and one a complete package installed for its build or active
// for its title already contains (kAlreadyContainsUpdate); the active update itself is up to date, never re-applied. On
// any failure or cancellation that build's current installation is left as it was. The source is a .pkg file or a game
// folder.
InstallResult InstallGame(const std::filesystem::path& source, const GameRoots& roots, const InstallHooks& hooks = {},
                          std::span<const GameBuild> builds = SupportedBuilds());
InstallResult InstallFromPackage(const std::filesystem::path& package, const GameRoots& roots,
                                 const InstallHooks& hooks = {}, std::span<const GameBuild> builds = SupportedBuilds());
InstallResult InstallFromFolder(const std::filesystem::path& source, const GameRoots& roots,
                                const InstallHooks& hooks = {}, std::span<const GameBuild> builds = SupportedBuilds());

// Lays an unpacked update over an installed base game in `staged_app0`, which must not exist yet. The base game is not
// changed: its files are linked, and the update's files replace the links.
InstallResult StageUpdate(const std::filesystem::path& base_app0, const std::filesystem::path& update_dir,
                          const std::filesystem::path& staged_app0);

enum class RemoveStatus { kRemoved, kNotUpdate, kNotInstalled, kForeignFolder, kInUse };

struct RemoveResult {
    RemoveStatus status = RemoveStatus::kRemoved;
    std::string detail; // for the log, not the UI
    bool ok() const { return status == RemoveStatus::kRemoved; }
};

// Removes an installed update build: its game folder and boot image, nothing else. The base build, saves, trophies and
// settings live elsewhere and are not touched; the update's files that are links into the base only lose that link. Both
// folders are first renamed aside, so a removal that cannot start changes nothing and one that stops halfway leaves no
// folder that passes as installed.
RemoveResult RemoveInstalledUpdate(const GameRoots& roots, const GameBuild& build,
                                   std::span<const GameBuild> builds = SupportedBuilds());

// The prepared updates of one title, in table order.
std::vector<const GameBuild*> InstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                                 std::span<const GameBuild> builds = SupportedBuilds());

struct RemoveAllResult {
    size_t removed = 0;
    size_t failed = 0;
    std::string detail;         // for the log, not the UI
    bool base_selected = false; // the title's base build is the active build afterwards
    bool ok() const { return failed == 0; }
};

// Removes every installed update of one title, never the base and never another title, then makes the title's prepared
// base the active build. A failure on one update does not stop the others.
RemoveAllResult RemoveInstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                         std::span<const GameBuild> builds = SupportedBuilds());

// Deletes what an interrupted removal left aside. True when nothing is left.
bool FinishInterruptedRemovals(const GameRoots& roots, std::span<const GameBuild> builds = SupportedBuilds());

// True when the package was chosen in the wrong action: an update in Install game, or a base in Install update.
bool RefusedPackageKind(InstallStatus status);

struct InstallMessage {
    std::string title;
    std::string text;
};
InstallMessage DescribeInstall(InstallStatus status);
InstallMessage DescribeInstall(const InstallResult& result);

} // namespace aln
