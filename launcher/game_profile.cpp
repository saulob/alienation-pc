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

// The executable's own localizations, by the SCE system language ids that name them.
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
    {"portuguese", "Portuguese (Portugal)", 7},
    {"portuguese_brazil", "Portuguese (Brazil)", 17},
    {"russian", "Russian", 8},
    {"spanish", "Spanish (Spain)", 3},
    {"spanish_latin_america", "Spanish (Latin America)", 20},
    {"swedish", "Swedish", 13},
};

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
        {},
        kAlienationLanguages,
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
        {},
        kAlienationLanguages,
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
