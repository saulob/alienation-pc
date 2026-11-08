#include "game_profile.h"

#include <string_view>

#include "localization/ui_text.h"

namespace aln {
namespace {

constexpr PackageLayer kBase100Layers[] = {{"01.00", "01.00", false}};

constexpr const char* kEurope100DataFiles[] = {
    "audio.pck", "stream.dat", "video/intro.mp4", "video/loading.mp4", "video/splash.mp4", "video/title.mp4",
};

constexpr const char* kAsia100DataFiles[] = {
    "audio.pck",       "stream.dat",          "video/intro.mp4", "video/loading.mp4",
    "video/splash.mp4", "video/splash_SIE.mp4", "video/title.mp4",
};

// The executable's own localizations, by the SCE system language ids that name them. English first, then by name.
constexpr TextLanguage kAlienationLanguages[] = {
    {"english", "English", 1},
    {"chinese_traditional", "Chinese (Traditional)", 10},
    {"danish", "Danish", 14},
    {"dutch", "Dutch", 6},
    {"finnish", "Finnish", 12},
    {"french", "French", 2},
    {"german", "German", 4},
    {"italian", "Italian", 5},
    {"japanese", "Japanese", 0},
    {"korean", "Korean", 9},
    {"norwegian", "Norwegian", 15},
    {"polish", "Polish", 16},
    {"portuguese_brazil", "Portuguese (Brazil)", 17},
    {"portuguese", "Portuguese (Portugal)", 7},
    {"russian", "Russian", 8},
    {"spanish_latin_america", "Spanish (Latin America)", 20},
    {"spanish", "Spanish (Spain)", 3},
    {"swedish", "Swedish", 13},
};

// The display configuration's default size: mov dword [rdi+0x34], 1080 and mov dword [rdi+0x30], 1920. The engine sizes
// its frontbuffers and every render script's RENDER_RESOLUTION_X/Y from it.
// The game's startup functions (two identical copies, the same offsets in both builds) set the size of the GPU heap that
// holds the render targets and textures: mov qword [rbp-0x78], 1 GiB. The heap's allocator returns a null block when it
// is full and the game never checks it, so a larger target needs a larger heap.
constexpr AlnResolutionSite kEurope100ResolutionSites[] = {
    {0x3bb, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
    {0x47b, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
    {0x1fa8da, 3, {0xc7, 0x47, 0x34}, 1080, 1, 0},
    {0x1fa8e1, 3, {0xc7, 0x47, 0x30}, 1920, 0, 0},
};
constexpr AlnResolutionSite kAsia100ResolutionSites[] = {
    {0x3bb, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
    {0x47b, 4, {0x48, 0xc7, 0x45, 0x88}, 0x40000000, 0, 1},
    {0x1fb4ea, 3, {0xc7, 0x47, 0x34}, 1080, 1, 0},
    {0x1fb4f1, 3, {0xc7, 0x47, 0x30}, 1920, 0, 0},
};

constexpr AlnFrameRatePatch kEurope100FrameRate = {0x2507b0, {0x48, 0x8b, 0x47, 0x28}, {0x31, 0xc0, 0xc3, 0x90}};
constexpr AlnFrameRatePatch kAsia100FrameRate = {0x2513c0, {0x48, 0x8b, 0x47, 0x28}, {0x31, 0xc0, 0xc3, 0x90}};

constexpr GameBuild kGameBuilds[] = {
    {
        "CUSA00062",
        "EP9000-CUSA00062_00-ALIENATIONGAMEEU",
        "1.00",
        "CUSA00062-1.00",
        "f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855",
        kEurope100DataFiles,
        "CUSA00062 1.00",
        "Europe",
        kBase100Layers,
        kEurope100ResolutionSites,
        kAlienationLanguages,
        &kEurope100FrameRate,
    },
    {
        "CUSA04430",
        "HP9000-CUSA04430_00-ALIENATIONGAMEKR",
        "1.00",
        "CUSA04430-1.00",
        "11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e",
        kAsia100DataFiles,
        "CUSA04430 1.00",
        "Asia",
        kBase100Layers,
        kAsia100ResolutionSites,
        kAlienationLanguages,
        &kAsia100FrameRate,
    },
};

bool SameLayer(const PackageLayer& a, const PackageLayer& b) {
    return std::string_view(a.app_ver) == b.app_ver && std::string_view(a.sfo_version) == b.sfo_version &&
           a.update == b.update;
}

// PS4 metadata is "NN.NN"; the launcher shows "N.NN".
std::string DisplayVersion(std::string_view version) {
    size_t first = 0;
    while (first + 1 < version.size() && version[first] == '0' && version[first + 1] != '.') {
        ++first;
    }
    return std::string(version.substr(first));
}

} // namespace

std::span<const GameBuild> SupportedBuilds() {
    return kGameBuilds;
}

const PackageLayer& FinishedLayer(const GameBuild& build) {
    return build.layers.back();
}

std::string BuildMetadataText(const GameBuild& build) {
    const PackageLayer& finished = FinishedLayer(build);
    return "APP_VER " + DisplayVersion(finished.app_ver) + " \xC2\xB7 VERSION " + DisplayVersion(finished.sfo_version);
}

const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds) {
    if (build.layers.size() < 2) {
        return nullptr;
    }
    for (const GameBuild& candidate : builds) {
        if (std::string_view(candidate.title_id) == build.title_id && candidate.layers.size() == 1 &&
            SameLayer(candidate.layers.front(), build.layers.front())) {
            return &candidate;
        }
    }
    return nullptr;
}

std::span<const TextLanguage> TextLanguagesOf(const GameBuild* build) {
    return build ? build->text_languages : std::span<const TextLanguage>(kAlienationLanguages, 1);
}

bool RestrictedToNativeResolution(const GameBuild* build) {
    return build && build->resolution_sites.empty();
}

const TextLanguage& ResolveTextLanguage(std::span<const TextLanguage> languages, std::string_view key) {
    for (const TextLanguage& language : languages) {
        if (language.key == key) {
            return language;
        }
    }
    return languages.front();
}

const TextLanguage& ResolveTextLanguage(const GameBuild* build, std::string_view key) {
    return ResolveTextLanguage(TextLanguagesOf(build), key);
}

std::string SupportedBuildsText() {
    const std::span<const GameBuild> builds = SupportedBuilds();
    std::string text;
    for (size_t i = 0; i < builds.size(); ++i) {
        if (i > 0) {
            text += i + 1 == builds.size() ? Tr(" and ") : ", ";
        }
        text += builds[i].label;
    }
    return text;
}

} // namespace aln
