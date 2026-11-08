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

// A console language the game can be told at start (SCE system language id). The game keeps its own language option.
struct TextLanguage {
    std::string_view key;
    std::string_view name;
    int ps4_id;
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
    std::span<const TextLanguage> text_languages;        // the first is the default
    const AlnFrameRatePatch* frame_rate;                 // the verified engine_set_fps binding
};

// The supported builds.
std::span<const GameBuild> SupportedBuilds();

// The finished game's layer: its param.sfo is the one of the installed game.
const PackageLayer& FinishedLayer(const GameBuild& build);

// The finished package's APP_VER and VERSION, formatted for display (01.00 becomes 1.00).
std::string BuildMetadataText(const GameBuild& build);

// The build whose base package a build's update is laid over, or nullptr when the build has no update.
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());

// True when the build's executable has no verified render resolution sites: it then runs only at its native size.
// The launcher hides and the loader refuses the custom Render resolution and Aspect Ratio for such a build.
bool RestrictedToNativeResolution(const GameBuild* build);

// The languages offered for the build; English alone when the build is not known.
std::span<const TextLanguage> TextLanguagesOf(const GameBuild* build);

// The language of a saved key among `languages` (not empty); the first when the key is not one of them.
const TextLanguage& ResolveTextLanguage(std::span<const TextLanguage> languages, std::string_view key);
// The build's language for a saved key; the build's default when the key is not one of its languages.
const TextLanguage& ResolveTextLanguage(const GameBuild* build, std::string_view key);

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
