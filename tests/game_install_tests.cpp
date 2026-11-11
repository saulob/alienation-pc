// Install game and Install update without a window: package identity and kind, the title and version gates, staging,
// failure rollback, version detection and update removal, on synthetic folders and metadata-only packages in a temporary
// folder this test owns. ALN_TEST_PACKAGES names a folder with the player's real packages, which are only read.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "aln_image_format.h"
#include "aln_launcher.h"
#include "crypto.h"
#include "game_files.h"
#include "game_install.h"
#include "pkg_extract.h"
#include "utf8.h"

using namespace aln;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

void Append32(std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); }
void Append16(std::string& out, uint16_t value) { out.append(reinterpret_cast<const char*>(&value), 2); }
void AppendBe32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char(value >> shift));
}

std::string Sfo(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string keys, values, index;
    for (const auto& [key, value] : fields) {
        Append16(index, uint16_t(keys.size()));
        Append16(index, 0x0204);
        Append32(index, uint32_t(value.size() + 1));
        Append32(index, uint32_t(value.size() + 1));
        Append32(index, uint32_t(values.size()));
        keys += key + '\0';
        values += value + '\0';
    }
    const uint32_t key_table = uint32_t(20 + index.size());
    std::string out("\0PSF", 4);
    Append32(out, 0x101);
    Append32(out, key_table);
    Append32(out, uint32_t(key_table + keys.size()));
    Append32(out, uint32_t(fields.size()));
    return out + index + keys + values;
}

constexpr char kEurope[] = "CUSA00062";
constexpr char kEuropeContent[] = "EP9000-CUSA00062_00-ALIENATIONGAMEEU";

std::string PackageSfo(const char* title, const char* content, const char* category, const char* app_ver,
                       const char* version) {
    return Sfo({{"TITLE", "ALIENATION"},
                {"TITLE_ID", title},
                {"CONTENT_ID", content},
                {"CATEGORY", category},
                {"APP_VER", app_ver},
                {"VERSION", version}});
}

std::string FinishedSfo(const GameBuild& build) {
    const PackageLayer& layer = build.layers.back();
    return PackageSfo(build.title_id, build.content_id, layer.update ? "gp" : "gd", layer.app_ver, layer.sfo_version);
}

const GameBuild& Build(std::string_view folder, std::span<const GameBuild> builds = SupportedBuilds()) {
    for (const GameBuild& build : builds) {
        if (folder == build.folder) return build;
    }
    std::printf("FAIL: no build %.*s\n", int(folder.size()), folder.data());
    std::exit(1);
}

void Write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Sha256(const std::string& data) {
    uint8_t digest[32];
    crypto::Sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

// Every file and folder with its own time: equal trees mean nothing was written.
std::map<std::string, fs::file_time_type> Tree(const fs::path& dir) {
    std::map<std::string, fs::file_time_type> entries;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        entries[it->path().lexically_relative(dir).generic_string()] = fs::last_write_time(it->path(), ec);
    }
    return entries;
}

bool AnyStaging(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = Utf8(it->path().filename());
        if (name.find(".install-") != std::string::npos || name.ends_with(".removing")) return true;
    }
    return false;
}

// A package that carries only its param.sfo: enough for everything Install does before it unpacks.
void WriteSfoOnlyPackage(const fs::path& file, const std::string& sfo, uint32_t declared_extra = 0) {
    std::string pkg(0x1000, '\0');
    const auto put32 = [&](size_t at, uint32_t value) {
        std::string be;
        AppendBe32(be, value);
        pkg.replace(at, 4, be);
    };
    const uint32_t sfo_offset = 0x1020, total = uint32_t(sfo_offset + sfo.size());
    put32(0x00, 0x7F434E54);
    put32(0x10, 1);
    put32(0x18, 0x1000);
    put32(0x434, total + declared_extra);
    std::string entry;
    AppendBe32(entry, 0x1000);
    entry.append(12, '\0');
    AppendBe32(entry, sfo_offset);
    AppendBe32(entry, uint32_t(sfo.size()));
    entry.append(8, '\0');
    Write(file, pkg + entry + sfo);
}

// A stand-in installation: only what the active-build and title checks read.
void InstallStandIn(const GameRoots& roots, const GameBuild& build) {
    const GamePaths paths = BuildPaths(roots, build);
    Write(paths.app0 / "sce_sys" / "param.sfo", FinishedSfo(build));
    Write(paths.app0 / kPreparedMarker, "{\"version\": \"" + std::string(build.version) + "\", \"scope\": \"full\"}");
    Write(paths.image_dir / "boot.bin", "image");
}

void MakeActive(const GameRoots& roots, const GameBuild& build, int step) {
    const auto when = fs::file_time_type::clock::now() - std::chrono::hours(1) + std::chrono::minutes(step);
    fs::last_write_time(BuildPaths(roots, build).app0 / kPreparedMarker, when);
}

void TestProfile() {
    const auto builds = SupportedBuilds();
    CHECK(builds.size() == 3);
    const GameBuild& eu100 = Build("CUSA00062-1.00");
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    const GameBuild& kr100 = Build("CUSA04430-1.00");
    CHECK(&builds[0] == &eu100);
    CHECK(!IsUpdateBuild(eu100) && !IsUpdateBuild(kr100) && IsUpdateBuild(eu106));
    CHECK(BaseBuildOf(eu106) == &eu100);
    CHECK(BaseBuildOf(eu100) == nullptr && BaseBuildOf(kr100) == nullptr);
    CHECK(BuildKey(eu106) == "CUSA00062-1.06");
    CHECK(std::string(eu106.label) == "CUSA00062 1.06");
    CHECK(std::string(eu106.content_id) == eu100.content_id);
    CHECK(std::string(eu106.folder) != eu100.folder);
    CHECK(std::string(eu106.eboot_sha256) == "c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243");
    CHECK(std::string(eu106.eboot_sha256) != eu100.eboot_sha256);
    CHECK(BuildMetadataText(eu106) == "APP_VER 1.06 \xC2\xB7 VERSION 1.00");
    CHECK(BuildMetadataText(eu100) == "APP_VER 1.00 \xC2\xB7 VERSION 1.00");
    // 1.06 has its own verified sites, never 1.00's offsets.
    CHECK(eu106.resolution_sites.size() == 4 && eu106.frame_rate != nullptr);
    CHECK(eu106.frame_rate->offset != eu100.frame_rate->offset);
    CHECK(eu106.resolution_sites[2].offset != eu100.resolution_sites[2].offset);
    CHECK(!RestrictedToNativeResolution(&eu106));
    CHECK(!RestrictedToNativeResolution(&eu100) && eu100.frame_rate != nullptr);
    CHECK(!RestrictedToNativeResolution(&kr100) && kr100.frame_rate != nullptr);
    CHECK(SupportedBuildsText() == "CUSA00062 1.00 \xC2\xB7 EUR, CUSA00062 1.06 \xC2\xB7 EUR and CUSA04430 1.00 \xC2\xB7 ASIA/KR");
    CHECK(BuildDisplayName(kr100) == "CUSA04430 1.00 \xC2\xB7 ASIA/KR");
    for (const GameBuild& build : builds) {
        CHECK(std::string_view(build.region).find("USA") == std::string_view::npos);
    }
    CHECK(SupportedBuildsText().find(" USA") == std::string::npos);
    CHECK(SupportedBuildsText().find("CUSA04181") == std::string::npos);
    // Only 1.06 has a repack, and it carries no executable of its own: the build's eboot.bin is the proof.
    CHECK(eu100.package_aliases.empty() && kr100.package_aliases.empty() && eu106.package_aliases.size() == 1);
    CHECK(BuildMetadataText(eu106, &eu106.package_aliases[0]) == "APP_VER 1.00 \xC2\xB7 VERSION 1.06");
}

void TestIdentity() {
    const GameBuild& eu100 = Build("CUSA00062-1.00");
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    const GameCheck update = CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gp", "01.06", "01.00"));
    CHECK(update.state == GameFileState::kValid && update.build == &eu106 && update.layer == 1);
    const GameCheck base = CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gd", "01.00", "01.00"));
    CHECK(base.state == GameFileState::kValid && base.build == &eu100 && base.layer == 0);
    // An update must not pass as a base with the same numbers, nor a base as an update.
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gd", "01.06", "01.00")).state != GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gp", "01.00", "01.00")).state != GameFileState::kValid);
    const GameCheck unknown = CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gp", "01.07", "01.00"));
    CHECK(unknown.state == GameFileState::kDifferentVersion && unknown.build == &eu100);
    CHECK(unknown.detail.find("is not part of a supported build") != std::string::npos);
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, "HP9000-CUSA04430_00-ALIENATIONGAMEKR", "gp", "01.06", "01.00")).state !=
          GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo("CUSA04430", "HP9000-CUSA04430_00-ALIENATIONGAMEKR", "gp", "01.06", "01.00"))
              .state != GameFileState::kValid);

    // The installed game's version comes from its param.sfo, never from a file name.
    CHECK(CheckSfoIdentity(FinishedSfo(eu106)).build == &eu106);
    CHECK(CheckSfoIdentity(FinishedSfo(eu100)).build == &eu100);
    CHECK(CheckSfoIdentity(FinishedSfo(Build("CUSA04430-1.00"))).build == &Build("CUSA04430-1.00"));
    CHECK(CheckSfoIdentity(PackageSfo(kEurope, kEuropeContent, "gp", "01.05", "01.00")).state != GameFileState::kValid);
    CHECK(CheckSfoIdentity(FinishedSfo(eu106)).alias == nullptr);
}

// The DUPLEX repack (gd, APP_VER 01.00, VERSION 01.06) is a complete package of 1.06, recognized by its metadata only as
// a candidate: the installer and the readiness check then require 1.06's eboot.bin.
void TestRepackIdentity() {
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    const std::string duplex = PackageSfo(kEurope, kEuropeContent, "gd", "01.00", "01.06");
    const GameCheck package = CheckPackageIdentity(duplex);
    CHECK(package.state == GameFileState::kValid && package.build == &eu106 && package.layer == 0);
    CHECK(package.alias == &eu106.package_aliases[0]);
    const GameCheck installed = CheckSfoIdentity(duplex);
    CHECK(installed.build == &eu106 && installed.alias == &eu106.package_aliases[0]);
    // Only that exact identity: other categories, versions or titles stay unknown.
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gp", "01.00", "01.06")).state != GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gd", "01.00", "01.07")).state != GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, kEuropeContent, "gd", "01.06", "01.06")).state != GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo("CUSA04430", "HP9000-CUSA04430_00-ALIENATIONGAMEKR", "gd", "01.00", "01.06"))
              .state != GameFileState::kValid);
    CHECK(CheckPackageIdentity(PackageSfo(kEurope, "EP9000-CUSA00062_00-SOMEOTHERCONTENT", "gd", "01.00", "01.06")).state !=
          GameFileState::kValid);
    CHECK(BuildKey(*package.build) == "CUSA00062-1.06");
}

// The loader's view of a boot image: each build gets its own patch sites; a repack's image is the build's.
void TestLoaderBuild(const fs::path& scratch) {
    const auto image = [&](const char* name, const char* title, const char* app_ver, const char* sha) {
        const fs::path dir = scratch / "images" / name;
        Write(dir / "image.json", std::string("{\"title_id\": \"") + title + "\", \"app_ver\": \"" + app_ver +
                                      "\", \"eboot_sha256\": \"" + sha + "\"}");
        return Utf8(dir);
    };
    const GameBuild& eu100 = Build("CUSA00062-1.00");
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    AlnGameBuild found{};
    CHECK(aln_game_build(image("106", kEurope, "01.06", eu106.eboot_sha256).c_str(), &found) == 1);
    CHECK(std::string(found.label) == "CUSA00062 1.06" && std::string(found.version) == "1.06");
    CHECK(found.resolution_site_count == 4 && found.frame_rate == eu106.frame_rate);
    CHECK(found.resolution_sites == eu106.resolution_sites.data());
    found = {};
    CHECK(aln_game_build(image("duplex", kEurope, "01.00", eu106.eboot_sha256).c_str(), &found) == 1);
    CHECK(std::string(found.version) == "1.06" && found.frame_rate == eu106.frame_rate);
    found = {};
    CHECK(aln_game_build(image("100", kEurope, "01.00", eu100.eboot_sha256).c_str(), &found) == 1);
    CHECK(std::string(found.version) == "1.00" && found.resolution_site_count == 4 && found.frame_rate != nullptr);
    CHECK(found.frame_rate == eu100.frame_rate);
    // A 1.00 executable named as 1.06 is no supported build, so nothing is patched; the executable decides.
    CHECK(aln_game_build(image("mixed", kEurope, "01.06", eu100.eboot_sha256).c_str(), &found) == 0);
    CHECK(aln_game_build(image("other", kEurope, "01.07", eu106.eboot_sha256).c_str(), &found) == 0);
}

void TestStageUpdateKeepsBase(const fs::path& scratch) {
    const fs::path base = scratch / "stage" / "base", update = scratch / "stage" / "update",
                   staged = scratch / "stage" / "staged";
    Write(base / "eboot.bin", "base eboot");
    Write(base / "video" / "intro.mp4", "base video");
    Write(base / "stream.dat", "base stream");
    Write(base / "sce_sys" / "param.sfo", "base sfo");
    Write(update / "eboot.bin", "update eboot");
    Write(update / "stream.dat", "update stream");
    Write(update / "Navmeshes" / "a.navmesh", "update navmesh");
    Write(update / "sce_sys" / "param.sfo", "update sfo");

    CHECK(StageUpdate(base, update, staged).ok());
    CHECK(Read(staged / "eboot.bin") == "update eboot");
    CHECK(Read(staged / "stream.dat") == "update stream");
    CHECK(Read(staged / "Navmeshes" / "a.navmesh") == "update navmesh");
    CHECK(Read(staged / "video" / "intro.mp4") == "base video");
    CHECK(Read(staged / "sce_sys" / "param.sfo") == "update sfo");
    // The staged files are links into the base: a replaced one must not have written through.
    CHECK(Read(base / "eboot.bin") == "base eboot");
    CHECK(Read(base / "stream.dat") == "base stream");
    CHECK(Read(base / "sce_sys" / "param.sfo") == "base sfo");
    CHECK(!fs::exists(base / "Navmeshes"));
}

// Install update and Install game refuse, from the package's param.sfo and before anything is written, the wrong kind,
// another title's update, a foreign or unknown package and malformed metadata.
void TestPackageGates(const fs::path& scratch) {
    const fs::path root = scratch / "gates";
    const GameRoots roots{root / "prepared", root / "image"};
    const fs::path packages = root / "packages";
    const GameBuild& eu100 = Build("CUSA00062-1.00");
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    const GameBuild& kr100 = Build("CUSA04430-1.00");
    InstallStandIn(roots, eu100);
    InstallStandIn(roots, kr100);
    Write(root / "user" / "savedata" / "CUSA00062" / "SAVE0000" / "data.bin", "save");
    Write(root / "user" / "alienation.ini", "[launcher]\n");
    MakeActive(roots, kr100, 1);
    MakeActive(roots, eu100, 2);

    // Package names say nothing: identity comes from the metadata alone.
    const fs::path update = packages / "random-name-a.pkg", base = packages / "random-name-b.pkg";
    WriteSfoOnlyPackage(update, FinishedSfo(eu106));
    WriteSfoOnlyPackage(base, FinishedSfo(eu100));
    const auto attempt = [&](const fs::path& file, PackageKind kind) {
        InstallHooks hooks;
        hooks.expected = kind;
        return InstallFromPackage(file, roots, hooks);
    };
    std::map<std::string, fs::file_time_type> prepared_before, image_before, user_before;
    const auto snapshot = [&] {
        prepared_before = Tree(roots.prepared);
        image_before = Tree(roots.image);
        user_before = Tree(root / "user");
    };
    snapshot();
    const auto unchanged = [&](const GameBuild* active) {
        CHECK(Tree(roots.prepared) == prepared_before);
        CHECK(Tree(roots.image) == image_before);
        CHECK(Tree(root / "user") == user_before);
        CHECK(LastPreparedBuild(roots) == active);
        CHECK(!fs::exists(BuildPaths(roots, eu106).app0));
        CHECK(!AnyStaging(roots.prepared) && !AnyStaging(roots.image));
    };

    // The matching update passes every gate and reaches the base check (the stand-in base is not a real game).
    InstallResult result = attempt(update, PackageKind::kUpdate);
    CHECK(result.status == InstallStatus::kUpdateNeedsBase);
    CHECK(result.recognition.find("is CUSA00062 1.06 (update, APP_VER 1.06") != std::string::npos);
    CHECK(result.recognition.find("laid over CUSA00062 1.00") != std::string::npos);
    CHECK(result.recognition.find("random-name-a.pkg") != std::string::npos);
    unchanged(&eu100);

    CHECK(attempt(base, PackageKind::kUpdate).status == InstallStatus::kExpectedUpdatePackage);
    CHECK(attempt(update, PackageKind::kBase).status == InstallStatus::kExpectedBasePackage);
    CHECK(attempt(update, PackageKind::kAny).status == InstallStatus::kUpdateNeedsBase);
    unchanged(&eu100);

    // The DUPLEX repack is a complete game: Install update refuses it as such, and Install game reaches its executable
    // check (this stand-in carries no files, so it stops there with nothing written).
    const fs::path repack = packages / "Alienation.PS4-DUPLEX.pkg";
    WriteSfoOnlyPackage(repack, PackageSfo(kEurope, kEuropeContent, "gd", "01.00", "01.06"));
    result = attempt(repack, PackageKind::kUpdate);
    CHECK(result.status == InstallStatus::kExpectedUpdatePackage && result.repack);
    CHECK(DescribeInstall(result).title == "This is a complete game package");
    CHECK(result.recognition.find("is CUSA00062 1.06 \xC2\xB7 EUR (single-package repack, APP_VER 1.00 \xC2\xB7 VERSION 1.06") !=
          std::string::npos);
    CHECK(result.recognition.find(eu106.eboot_sha256) != std::string::npos);
    result = attempt(repack, PackageKind::kBase);
    CHECK(!result.ok() && result.repack && result.build == nullptr);
    unchanged(&eu100);

    // An update of another title than the active game's is refused, even when its base is installed too.
    MakeActive(roots, kr100, 3);
    snapshot();
    result = attempt(update, PackageKind::kUpdate);
    CHECK(result.status == InstallStatus::kUpdateForOtherTitle);
    CHECK(result.update_title_id == "CUSA00062" && result.installed_title_id == "CUSA04430");
    CHECK(DescribeInstall(result).title == "This update is for CUSA00062, but the installed game is CUSA04430");
    unchanged(&kr100);
    MakeActive(roots, eu100, 4);
    snapshot();

    const fs::path other = packages / "other.pkg", unknown = packages / "unknown.pkg", truncated = packages / "short.pkg",
                   garbage = packages / "garbage.pkg", bad_sfo = packages / "bad-sfo.pkg";
    WriteSfoOnlyPackage(other, PackageSfo("CUSA00001", "UP0000-CUSA00001_00-OTHERGAME0000000", "gp", "01.06", "01.00"));
    WriteSfoOnlyPackage(unknown, PackageSfo(kEurope, kEuropeContent, "gp", "01.07", "01.00"));
    WriteSfoOnlyPackage(truncated, FinishedSfo(eu106), 4096);
    WriteSfoOnlyPackage(bad_sfo, "not a param.sfo");
    Write(garbage, std::string(0x2000, 'x'));
    CHECK(attempt(other, PackageKind::kUpdate).status == InstallStatus::kWrongGame);
    result = attempt(unknown, PackageKind::kUpdate);
    CHECK(result.status == InstallStatus::kWrongVersion);
    CHECK(DescribeInstall(result).text.find("CUSA00062 1.06") != std::string::npos);
    CHECK(attempt(truncated, PackageKind::kUpdate).status == InstallStatus::kIncomplete);
    CHECK(attempt(bad_sfo, PackageKind::kUpdate).status == InstallStatus::kWrongGame);
    CHECK(attempt(garbage, PackageKind::kUpdate).status == InstallStatus::kNotPackage);
    CHECK(attempt(packages / "absent.pkg", PackageKind::kUpdate).status == InstallStatus::kNotPackage);
    unchanged(&eu100);

    // A package inside the port's own game folders is refused before it is read.
    const fs::path inside = BuildPaths(roots, eu100).app0 / "copy.pkg";
    WriteSfoOnlyPackage(inside, FinishedSfo(eu106));
    CHECK(attempt(inside, PackageKind::kUpdate).status == InstallStatus::kOverlapping);
    fs::remove(inside);

    CHECK(RefusedPackageKind(InstallStatus::kExpectedBasePackage) && RefusedPackageKind(InstallStatus::kExpectedUpdatePackage));
    CHECK(!RefusedPackageKind(InstallStatus::kUpdateForOtherTitle) && !RefusedPackageKind(InstallStatus::kInstalled));
    CHECK(DescribeInstall(InstallStatus::kUpdateNeedsBase).text.find("same title ID") != std::string::npos);
    std::error_code ec;
    fs::remove_all(root, ec);
}

constexpr char kTestTitle[] = "CUSA99999";
constexpr char kTestContent[] = "EP9000-CUSA99999_00-ALIENATIONTEST00";
constexpr PackageLayer kTestBase[] = {{"01.00", "01.00", false}};
constexpr PackageLayer kTest106[] = {{"01.00", "01.00", false}, {"01.06", "01.00", true}};
constexpr PackageLayer kTest108[] = {{"01.00", "01.00", false}, {"01.08", "01.00", true}};
constexpr PackageAlias kTestRepack[] = {{"single-package repack", "gd", "01.00", "01.06"}};

// A build table of synthetic executables, so complete folders and boot images can be laid out without game files.
struct TestBuilds {
    std::string sha[3];
    GameBuild builds[3];
    TestBuilds() {
        sha[0] = Sha256(Eboot("1.00"));
        sha[1] = Sha256(Eboot("1.06"));
        sha[2] = Sha256(Eboot("1.08"));
        builds[0] = {kTestTitle, kTestContent, "1.00", "CUSA99999-1.00", sha[0].c_str(), {}, "CUSA99999 1.00", "Test",
                     kTestBase, {}, nullptr};
        builds[1] = {kTestTitle, kTestContent, "1.06", "CUSA99999-1.06", sha[1].c_str(), {}, "CUSA99999 1.06", "Test",
                     kTest106, {}, nullptr, kTestRepack};
        builds[2] = {kTestTitle, kTestContent, "1.08", "CUSA99999-1.08", sha[2].c_str(), {}, "CUSA99999 1.08", "Test",
                     kTest108, {}, nullptr};
    }
    static std::string Eboot(const char* version) { return std::string("synthetic eboot ") + version; }
    std::span<const GameBuild> span() const { return builds; }
};

// A complete game folder of a synthetic build: every required file and its own executable.
void WriteGameFolder(const fs::path& app0, const GameBuild& build) {
    for (const char* name : kSharedGameFiles) {
        Write(app0 / name, std::string("file ") + name);
    }
    Write(app0 / "eboot.bin", TestBuilds::Eboot(build.version));
    Write(app0 / "sce_sys" / "param.sfo", FinishedSfo(build));
}

// A prepared, valid installation: the folder, its marker and a boot image that names it.
void WritePrepared(const GameRoots& roots, const GameBuild& build) {
    const GamePaths paths = BuildPaths(roots, build);
    WriteGameFolder(paths.app0, build);
    Write(paths.app0 / kPreparedMarker, "{\"version\": \"" + std::string(build.version) + "\", \"scope\": \"full\"}");
    unsigned char content[ALN_CONTENT_PROFILE_SIZE];
    const uint32_t values[ALN_CONTENT_VALUES] = {ALN_CONTENT_SKU_FULL, 0, 0, 0, 0};
    aln_content_profile_encode(content, values);
    Write(paths.image_dir / "boot.bin", "boot");
    Write(paths.image_dir / "content.bin", std::string(reinterpret_cast<const char*>(content), sizeof(content)));
    Write(paths.image_dir / "image.json", std::string("{\"title_id\": \"") + build.title_id + "\", \"app_ver\": \"" +
                                              FinishedLayer(build).app_ver + "\", \"eboot_sha256\": \"" +
                                              build.eboot_sha256 + "\"}");
}

// Install update never goes back to an older update than the active one.
void TestOlderUpdate(const fs::path& scratch) {
    const TestBuilds test;
    const auto builds = test.span();
    const fs::path root = scratch / "older";
    const GameRoots roots{root / "prepared", root / "image"};
    for (const GameBuild& build : builds) InstallStandIn(roots, build);
    const auto attempt = [&](const GameBuild& build) {
        const fs::path file = root / "packages" / (std::string(build.folder) + ".pkg");
        WriteSfoOnlyPackage(file, FinishedSfo(build));
        InstallHooks hooks;
        hooks.expected = PackageKind::kUpdate;
        return InstallFromPackage(file, roots, hooks, builds);
    };
    MakeActive(roots, builds[0], 1);
    MakeActive(roots, builds[1], 2);
    MakeActive(roots, builds[2], 3);
    const auto before = Tree(roots.prepared);
    InstallResult result = attempt(builds[1]);
    CHECK(result.status == InstallStatus::kOlderUpdate);
    CHECK(result.installed_label == "CUSA99999 1.08");
    CHECK(DescribeInstall(result).title == "A newer update is installed: CUSA99999 1.08");
    CHECK(Tree(roots.prepared) == before && LastPreparedBuild(roots, builds) == &builds[2]);
    // Newer than or the same as the active update, or over the base, it passes on (to the base check here).
    CHECK(attempt(builds[2]).status == InstallStatus::kUpdateNeedsBase);
    MakeActive(roots, builds[1], 4);
    CHECK(attempt(builds[2]).status == InstallStatus::kUpdateNeedsBase);
    MakeActive(roots, builds[0], 5);
    CHECK(attempt(builds[1]).status == InstallStatus::kUpdateNeedsBase);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// A failed or cancelled installation leaves the installed game, its boot image and the user's files exactly as they
// were, and no staging folder behind.
void TestFailedInstallKeepsGame(const fs::path& scratch) {
    const TestBuilds test;
    const auto builds = test.span();
    const fs::path root = scratch / "failed";
    const GameRoots roots{root / "prepared", root / "image"};
    const GameBuild& base = builds[0];
    const GameBuild& update = builds[1];
    WritePrepared(roots, base);
    const GamePaths target = BuildPaths(roots, update);
    Write(target.app0 / kPreparedMarker, "{\"version\": \"1.06\", \"scope\": \"full\"}");
    Write(target.app0 / "keep.txt", "the installed game");
    Write(target.image_dir / "boot.bin", "the installed image");
    Write(root / "user" / "savedata" / "CUSA99999" / "SAVE0000" / "data.bin", "save");
    MakeActive(roots, base, 1);
    MakeActive(roots, update, 2);
    const fs::path source = root / "source";
    WriteGameFolder(source, update);
    const auto prepared_before = Tree(roots.prepared), image_before = Tree(roots.image), user_before = Tree(root / "user"),
               source_before = Tree(source);
    const auto unchanged = [&] {
        CHECK(Read(target.app0 / "keep.txt") == "the installed game");
        CHECK(Read(target.image_dir / "boot.bin") == "the installed image");
        CHECK(Tree(roots.prepared) == prepared_before);
        CHECK(Tree(roots.image) == image_before);
        CHECK(Tree(root / "user") == user_before);
        CHECK(Tree(source) == source_before);
        CHECK(!AnyStaging(roots.prepared) && !AnyStaging(roots.image));
    };

    InstallHooks hooks;
    hooks.expected = PackageKind::kUpdate;
    hooks.replace_active = true;
    // The synthetic executable is no real program, so the boot image cannot be built: the last step before the swap.
    InstallResult result = InstallFromFolder(source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kPrepareFailed);
    unchanged();

    hooks.progress = [](uint64_t, uint64_t) { return false; };
    result = InstallFromFolder(source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kCancelled);
    unchanged();

    // A folder of its own files that the installer did not write is never replaced.
    fs::remove(target.app0 / kPreparedMarker);
    hooks.progress = nullptr;
    CHECK(InstallFromFolder(source, roots, hooks, builds).status == InstallStatus::kForeignFolder);
    CHECK(Read(target.app0 / "keep.txt") == "the installed game");
    std::error_code ec;
    fs::remove_all(root, ec);
}

// Selecting an installed version activates it; the active build and its version come from the installed metadata; Remove
// update returns to the base without touching it or the user's files.
void TestVersionRefreshAndRemoval(const fs::path& scratch) {
    const TestBuilds test;
    const auto builds = test.span();
    const fs::path root = scratch / "refresh";
    const GameRoots roots{root / "prepared", root / "image"};
    const GameBuild& base = builds[0];
    const GameBuild& update = builds[1];
    WritePrepared(roots, base);
    WritePrepared(roots, update);
    const GamePaths base_paths = BuildPaths(roots, base), update_paths = BuildPaths(roots, update);
    Write(base_paths.app0 / "stream.dat", "base data");
    std::error_code ec;
    fs::create_hard_link(base_paths.app0 / "stream.dat", update_paths.app0 / "stream.dat", ec);
    const bool linked = !ec;
    Write(root / "user" / "savedata" / "CUSA99999" / "SAVE0000" / "data.bin", "save");
    Write(root / "user" / "alienation.ini", "[launcher]\n");
    const auto user_before = Tree(root / "user");
    MakeActive(roots, update, 1);
    MakeActive(roots, base, 2);
    CHECK(CheckGameFiles(base_paths, builds).state == GameFileState::kValid);
    CHECK(CheckGameFiles(update_paths, builds).state == GameFileState::kValid);
    CHECK(LastPreparedBuild(roots, builds) == &base);

    const fs::path update_source = root / "source-106", base_source = root / "source-100";
    WriteGameFolder(update_source, update);
    WriteGameFolder(base_source, base);
    InstallHooks hooks;
    hooks.expected = PackageKind::kUpdate;
    hooks.replace_active = true;
    InstallResult result = InstallFromFolder(update_source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kActivated && result.build == &update);
    CHECK(LastPreparedBuild(roots, builds) == &update);
    const GamePaths active = ActiveBuildPaths(roots, builds);
    const GameCheck check = CheckGameFiles(active, builds);
    CHECK(check.state == GameFileState::kValid && check.build == &update);
    CHECK(BuildMetadataText(*check.build) == "APP_VER 1.06 \xC2\xB7 VERSION 1.00");
    CHECK(InstalledGameName(check.state, check.build) == "PS4 CUSA99999 1.06 \xC2\xB7 Test");

    hooks.replace_active = false;
    CHECK(InstallFromFolder(update_source, roots, hooks, builds).status == InstallStatus::kUpToDate);
    // The base stays detectable and can be made active again with Install game.
    hooks.expected = PackageKind::kBase;
    result = InstallFromFolder(base_source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kActivated && result.build == &base);
    CHECK(LastPreparedBuild(roots, builds) == &base);
    hooks.expected = PackageKind::kUpdate;
    CHECK(InstallFromFolder(update_source, roots, hooks, builds).status == InstallStatus::kActivated);

    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).size() == 1);
    CHECK(RemoveInstalledUpdate(roots, base, builds).status == RemoveStatus::kNotUpdate);
    const RemoveAllResult removed = RemoveInstalledUpdatesOf(roots, kTestTitle, builds);
    CHECK(removed.ok() && removed.removed == 1 && removed.base_selected);
    CHECK(!fs::exists(update_paths.app0) && !fs::exists(update_paths.image_dir));
    CHECK(LastPreparedBuild(roots, builds) == &base);
    CHECK(CheckGameFiles(base_paths, builds).state == GameFileState::kValid);
    CHECK(Read(base_paths.app0 / "stream.dat") == "base data");
    CHECK(Tree(root / "user") == user_before);
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).empty());
    CHECK(RemoveInstalledUpdate(roots, update, builds).status == RemoveStatus::kNotInstalled);
    if (!linked) {
        std::printf("skipped: hard links unavailable in %s\n", Utf8(scratch).c_str());
    }

    // What an interrupted removal left aside never counts as installed and is finished later.
    const fs::path aside = roots.prepared / (std::string(update.folder) + ".removing");
    Write(aside / "sce_sys" / "param.sfo", FinishedSfo(update));
    Write(aside / kPreparedMarker, "{\"version\": \"1.06\", \"scope\": \"full\"}");
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).empty());
    CHECK(FinishInterruptedRemovals(roots, builds));
    CHECK(!fs::exists(aside));

    // A folder that holds another build is not removed as this update.
    WritePrepared(roots, update);
    Write(update_paths.app0 / "sce_sys" / "param.sfo", FinishedSfo(builds[2]));
    CHECK(RemoveInstalledUpdate(roots, update, builds).status == RemoveStatus::kForeignFolder);
    CHECK(fs::exists(update_paths.app0 / "eboot.bin"));
    fs::remove_all(root, ec);
}

// A repack folder installs with Install game as the build it repacks, only with that build's executable; installed, it is
// the build itself, not an update laid over a base, so Remove update leaves it alone.
void TestRepackInstall(const fs::path& scratch) {
    const TestBuilds test;
    const auto builds = test.span();
    const fs::path root = scratch / "repack";
    const GameRoots roots{root / "prepared", root / "image"};
    const GameBuild& update = builds[1];
    const std::string repack_sfo = PackageSfo(kTestTitle, kTestContent, "gd", "01.00", "01.06");
    const fs::path source = root / "source";
    WriteGameFolder(source, update);
    Write(source / "sce_sys" / "param.sfo", repack_sfo);
    const GameCheck tree = CheckGameTree(source, builds);
    CHECK(tree.state == GameFileState::kValid && tree.build == &update && tree.alias == &update.package_aliases[0]);

    InstallHooks hooks;
    hooks.expected = PackageKind::kUpdate;
    InstallResult result = InstallFromFolder(source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kExpectedUpdatePackage && result.repack);
    // Install game takes it; the synthetic executable cannot become a boot image, the last step before the swap.
    hooks.expected = PackageKind::kBase;
    result = InstallFromFolder(source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kPrepareFailed);
    CHECK(!fs::exists(BuildPaths(roots, update).app0) && !AnyStaging(roots.prepared));

    // The same metadata with another executable is an unknown repack, refused with its identity.
    Write(source / "eboot.bin", TestBuilds::Eboot("1.00"));
    CHECK(CheckGameTree(source, builds).state == GameFileState::kDifferentVersion);
    result = InstallFromFolder(source, roots, hooks, builds);
    CHECK(result.status == InstallStatus::kUnsupportedBuild);
    CHECK(!result.diagnostics.empty() && result.diagnostics[0].find("APP_VER 01.00, VERSION 01.06, CATEGORY gd") !=
                                             std::string::npos);
    CHECK(!fs::exists(BuildPaths(roots, update).app0));

    // Installed from the repack: valid as the build, with the repack's own metadata in its boot image report.
    const GamePaths paths = BuildPaths(roots, update);
    WritePrepared(roots, update);
    Write(paths.app0 / "sce_sys" / "param.sfo", repack_sfo);
    Write(paths.image_dir / "image.json", std::string("{\"title_id\": \"") + kTestTitle +
                                              "\", \"app_ver\": \"01.00\", \"eboot_sha256\": \"" + update.eboot_sha256 + "\"}");
    const GameCheck installed = CheckGameFiles(paths, builds);
    CHECK(installed.state == GameFileState::kValid && installed.build == &update && installed.alias != nullptr);
    CHECK(InstalledPackageAlias(paths.app0, update) == &update.package_aliases[0]);
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).empty());
    CHECK(RemoveInstalledUpdate(roots, update, builds).status == RemoveStatus::kNotUpdate);
    const RemoveAllResult removed = RemoveInstalledUpdatesOf(roots, kTestTitle, builds);
    CHECK(removed.ok() && removed.removed == 0);
    CHECK(fs::exists(paths.app0 / "eboot.bin") && fs::exists(paths.image_dir / "boot.bin"));
    // Installed over the official packages instead, it is an update again.
    Write(paths.app0 / "sce_sys" / "param.sfo", FinishedSfo(update));
    CHECK(InstalledPackageAlias(paths.app0, update) == nullptr);
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).size() == 1);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The installation's provenance decides the update actions, whatever the runtime build: an update laid over its base is
// up to date when applied again and can be removed; a complete repack of the same build already contains it, refuses it
// before anything is written and has no update to remove. The launcher asks every installation to replace a ready game.
void TestInstallLineage(const fs::path& scratch) {
    const TestBuilds test;
    const auto builds = test.span();
    const fs::path root = scratch / "lineage";
    const GameRoots roots{root / "prepared", root / "image"};
    const GameBuild& base = builds[0];
    const GameBuild& update = builds[1];
    const std::string repack_sfo = PackageSfo(kTestTitle, kTestContent, "gd", "01.00", "01.06");
    const fs::path update_source = root / "source-106", repack_source = root / "source-repack";
    WriteGameFolder(update_source, update);
    WriteGameFolder(repack_source, update);
    Write(repack_source / "sce_sys" / "param.sfo", repack_sfo);
    const auto attempt = [&](const fs::path& source, PackageKind kind, bool replace) {
        InstallHooks hooks;
        hooks.expected = kind;
        hooks.replace_active = replace;
        return InstallFromFolder(source, roots, hooks, builds);
    };
    const auto snapshot = [&] { return std::make_pair(Tree(roots.prepared), Tree(roots.image)); };

    // Official base, then the official update: it passes every gate (the synthetic executable stops at the boot image).
    WritePrepared(roots, base);
    MakeActive(roots, base, 1);
    CHECK(InstalledProvenance(BuildPaths(roots, base).app0, base) == InstallProvenance::kBasePackage);
    CHECK(attempt(update_source, PackageKind::kUpdate, true).status == InstallStatus::kPrepareFailed);

    // The update laid over the base and active: the same update again is up to date, nothing is re-applied.
    const GamePaths paths = BuildPaths(roots, update);
    WritePrepared(roots, update);
    MakeActive(roots, update, 2);
    CHECK(InstalledProvenance(paths.app0, update) == InstallProvenance::kUpdateLayers);
    auto before = snapshot();
    InstallResult result = attempt(update_source, PackageKind::kUpdate, true);
    CHECK(result.status == InstallStatus::kUpToDate && snapshot() == before);
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).size() == 1);

    // The repack in the build's folder: the official update is already contained, refused as such and not as older.
    Write(paths.app0 / "sce_sys" / "param.sfo", repack_sfo);
    CHECK(InstalledProvenance(paths.app0, update) == InstallProvenance::kRepack);
    CHECK(CheckGameFiles(paths, builds).state == GameFileState::kValid);
    before = snapshot();
    result = attempt(update_source, PackageKind::kUpdate, true);
    CHECK(result.status == InstallStatus::kAlreadyContainsUpdate && result.contained_version == "1.06");
    CHECK(DescribeInstall(result).title == "This game already contains version 1.06");
    CHECK(snapshot() == before);
    MakeActive(roots, base, 3);
    CHECK(attempt(update_source, PackageKind::kUpdate, true).status == InstallStatus::kAlreadyContainsUpdate);
    CHECK(snapshot().first.size() == before.first.size());
    MakeActive(roots, update, 4);

    // No update layer to remove, so no way back to the base through Remove update: the repack stays the build.
    CHECK(InstalledUpdatesOf(roots, kTestTitle, builds).empty());
    const RemoveAllResult removed = RemoveInstalledUpdatesOf(roots, kTestTitle, builds);
    CHECK(removed.ok() && removed.removed == 0 && LastPreparedBuild(roots, builds) == &update);
    CHECK(CheckGameFiles(paths, builds).state == GameFileState::kValid);

    // Install game with the repack again: up to date, or the launcher's reinstall, which leaves it as it was on failure.
    CHECK(attempt(repack_source, PackageKind::kBase, false).status == InstallStatus::kUpToDate);
    CHECK(attempt(repack_source, PackageKind::kBase, true).status == InstallStatus::kPrepareFailed);
    CHECK(InstalledProvenance(paths.app0, update) == InstallProvenance::kRepack);
    CHECK(LastPreparedBuild(roots, builds) == &update && !AnyStaging(roots.prepared));

    // Another title's game is refused before anything is written.
    const fs::path foreign = root / "source-foreign";
    WriteGameFolder(foreign, update);
    Write(foreign / "sce_sys" / "param.sfo",
          PackageSfo("CUSA99998", "EP9000-CUSA99998_00-ALIENATIONTEST00", "gp", "01.06", "01.00"));
    before = snapshot();
    CHECK(!attempt(foreign, PackageKind::kUpdate, true).ok() && snapshot() == before);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The launcher drives both actions through these entry points.
void TestLauncherWiring() {
    std::ifstream file(fs::path(ALN_SOURCE_DIR) / "launcher" / "launcher.cpp", std::ios::binary);
    const std::string launcher((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(launcher.find("BrowseAndInstall(PackageKind::kUpdate)") != std::string::npos);
    CHECK(launcher.find("BrowseAndInstall(PackageKind::kBase)") != std::string::npos);
    CHECK(launcher.find("hooks.expected = kind;") != std::string::npos);
    CHECK(launcher.find("const InstallMessage message = DescribeInstall(*outcome);") != std::string::npos);
    CHECK(launcher.find("DescribeInstall(outcome->status)") == std::string::npos);
    CHECK(launcher.find("RemoveInstalledUpdatesOf(") != std::string::npos);
    CHECK(launcher.find("FinishInterruptedRemovals(") != std::string::npos);
    CHECK(launcher.find("OtherProcessRunning(") != std::string::npos);
}

// The player's real packages, only read: each is recognized from its metadata whatever its file name, and the update's
// executable is the 1.06 one.
void TestRealPackages(const fs::path& scratch) {
    const char* folder = std::getenv("ALN_TEST_PACKAGES");
    if (!folder || !*folder) {
        std::printf("skipped: ALN_TEST_PACKAGES is not set (real packages)\n");
        return;
    }
    const fs::path root = scratch / "real";
    const GameRoots roots{root / "prepared", root / "image"};
    const GameBuild& eu100 = Build("CUSA00062-1.00");
    const GameBuild& eu106 = Build("CUSA00062-1.06");
    const GameBuild& kr100 = Build("CUSA04430-1.00");
    InstallStandIn(roots, kr100);
    size_t seen = 0;
    std::error_code ec;
    for (fs::directory_iterator it(PathFromUtf8(folder), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".pkg") continue;
        Package package;
        if (!package.Open(it->path()).ok()) continue;
        const GameCheck identity = CheckPackageIdentity(package.sfo());
        if (identity.state != GameFileState::kValid) continue;
        ++seen;
        std::printf("real package: %s is %s, layer %zu\n", Utf8(it->path().filename()).c_str(), identity.build->label,
                    identity.layer);
        if (identity.build == &eu106) {
            const bool repack = identity.alias != nullptr;
            CHECK(repack ? identity.layer == 0 && SfoString(package.sfo(), "CATEGORY") == "gd"
                         : identity.layer == 1 && SfoString(package.sfo(), "CATEGORY") == "gp");
            std::vector<uint8_t> eboot;
            CHECK(package.LoadFiles().ok() && package.ReadFile("eboot.bin", 512ull << 20, eboot).ok());
            CHECK(Sha256(std::string(eboot.begin(), eboot.end())) == eu106.eboot_sha256);
            InstallHooks hooks;
            hooks.expected = PackageKind::kUpdate;
            const auto before = Tree(root);
            const InstallResult refused = InstallFromPackage(it->path(), roots, hooks);
            CHECK(refused.status == (repack ? InstallStatus::kExpectedUpdatePackage : InstallStatus::kUpdateForOtherTitle));
            CHECK(Tree(root) == before);
            if (!repack) {
                // With the DUPLEX repack installed as 1.06, the official update is already contained.
                const GamePaths duplex = BuildPaths(roots, eu106);
                Write(duplex.app0 / "sce_sys" / "param.sfo", PackageSfo(kEurope, kEuropeContent, "gd", "01.00", "01.06"));
                Write(duplex.app0 / kPreparedMarker, "{\"version\": \"1.06\", \"scope\": \"full\"}");
                const auto with_repack = Tree(root);
                CHECK(InstallFromPackage(it->path(), roots, hooks).status == InstallStatus::kAlreadyContainsUpdate);
                CHECK(Tree(root) == with_repack);
                fs::remove_all(duplex.app0, ec);
            }
        } else {
            CHECK(identity.layer == 0 && (identity.build == &eu100 || identity.build == &kr100));
        }
    }
    CHECK(seen > 0);
    fs::remove_all(root, ec);
}

} // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / "aln-game-install-tests";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    TestProfile();
    TestIdentity();
    TestRepackIdentity();
    TestLoaderBuild(scratch);
    TestStageUpdateKeepsBase(scratch);
    TestPackageGates(scratch);
    TestOlderUpdate(scratch);
    TestFailedInstallKeepsGame(scratch);
    TestVersionRefreshAndRemoval(scratch);
    TestRepackInstall(scratch);
    TestInstallLineage(scratch);
    TestLauncherWiring();
    TestRealPackages(scratch);
    fs::remove_all(scratch, ec);
    std::printf("%s\n", failures ? "FAILED" : "game install tests passed");
    return failures ? 1 : 0;
}
