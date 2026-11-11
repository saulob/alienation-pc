// The supported game builds. scripts/game_profiles.py mirrors the identity fields.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aln_frame_rate.h"
#include "aln_resolution.h"

namespace aln {

inline constexpr char kGameTitle[] = "Alienation";
inline constexpr char kPortRepository[] = "github.com/saulob/alienation-pc";
inline constexpr char kAuthorUrl[] = "https://github.com/saulob";
inline constexpr char kSponsorUrl[] = "https://github.com/sponsors/saulob";

// A package of a build, by its param.sfo APP_VER and VERSION. An update (CATEGORY gp) is applied over the packages
// before it.
struct PackageLayer {
    const char* app_ver;
    const char* sfo_version;
    bool update;
};

struct GameBuild {
    const char* title_id;
    const char* content_id;
    const char* version;          // the profile version: the preparation marker's "version"
    const char* folder;           // under game\prepared and game\image; distinct for each build
    const char* eboot_sha256;     // lower-case hex of the finished game's eboot.bin
    std::span<const char* const> data_files; // game data the build needs besides kSharedGameFiles
    const char* label;            // the name the log, the launcher and the loader show
    const char* region;           // from the content id's publisher prefix
    std::span<const PackageLayer> layers; // base package first; the last layer is the finished game
    std::span<const AlnResolutionSite> resolution_sites; // empty until verified in this build's executable
    const AlnFrameRatePatch* frame_rate;                 // the verified engine_set_fps binding
};

// The supported builds: each title's base first, then its updates.
std::span<const GameBuild> SupportedBuilds();

// The finished game's layer: its param.sfo is the one of the installed game.
const PackageLayer& FinishedLayer(const GameBuild& build);

// The finished package's APP_VER and VERSION, formatted for display (01.00 becomes 1.00).
std::string BuildMetadataText(const GameBuild& build);

// True for a build finished by an update package.
inline bool IsUpdateBuild(const GameBuild& build) {
    return build.layers.size() > 1;
}

// The build whose base package a build's update is laid over, or nullptr when the build has no update.
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());

// True when the build's executable has no verified render resolution sites: it then runs only at its native size.
// The launcher hides and the loader refuses the custom Render resolution and Aspect Ratio for such a build.
bool RestrictedToNativeResolution(const GameBuild* build);

std::string SupportedBuildsText();

// Files every complete game folder has, whatever the build.
inline constexpr const char* kSharedGameFiles[] = {
    "eboot.bin",
    "sce_discmap.plt",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "sce_sys/param.sfo",
};

// Linked after the eboot in this order: libc.prx needs libSceFios2.prx.
inline constexpr const char* kBundledModules[] = {"libc.prx", "libSceFios2.prx"};

// Written last into a prepared game folder (scripts/game_view.py writes the same one).
inline constexpr char kPreparedMarker[] = ".aln_view.json";

} // namespace aln
