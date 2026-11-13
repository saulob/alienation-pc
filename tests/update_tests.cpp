// Updater tests: semantic versions, release parsing, SHA-256, ZIP path safety, staging validation and rollback.
// Run: aln-update-tests

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "update_restart_arguments.h"
#include "updater/github_release.h"
#include "updater/install_transaction.h"
#include "updater/sha256.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_job.h"
#include "updater/update_paths.h"
#include "updater/update_zip.h"
#include "updater/version.h"

using namespace trg::update;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("aln-update-tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), std::streamsize(content.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& entries) {
  mz_zip_archive archive{};
  if (mz_zip_writer_init_heap(&archive, 0, 0) == MZ_FALSE) {
    return {};
  }
  for (const auto& [name, content] : entries) {
    if (mz_zip_writer_add_mem(&archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION) ==
        MZ_FALSE) {
      mz_zip_writer_end(&archive);
      return {};
    }
  }
  void* data = nullptr;
  size_t size = 0;
  if (mz_zip_writer_finalize_heap_archive(&archive, &data, &size) == MZ_FALSE) {
    mz_zip_writer_end(&archive);
    return {};
  }
  std::string result(static_cast<const char*>(data), size);
  mz_free(data);
  mz_zip_writer_end(&archive);
  return result;
}

void TestVersions() {
  std::printf("semantic versions\n");
  const auto v060 = ParseVersion("v0.6.0");
  const auto v060_plain = ParseVersion("0.6.0");
  const auto v0610 = ParseVersion("0.6.10");
  const auto v070 = ParseVersion("0.7.0");
  const auto v100 = ParseVersion("1.0.0");
  CHECK(v060 && v060_plain && v0610 && v070 && v100);
  CHECK(*v060 == *v060_plain);
  CHECK(!(*v0610 < *v060));
  CHECK(*v060 < *v0610);
  CHECK(*v0610 < *v070);
  CHECK(*v070 < *v100);
  CHECK(v060->CoreString() == "0.6.0");
  CHECK(v100->ToString() == "1.0.0");

  const auto rc = ParseVersion("0.3.2-rc1");
  const auto rc2 = ParseVersion("0.3.2-rc.2");
  const auto rc10 = ParseVersion("0.3.2-rc.10");
  const auto stable = ParseVersion("0.3.2");
  CHECK(rc && rc2 && rc10 && stable);
  CHECK(*rc < *stable);
  CHECK(*rc2 < *rc10);
  const auto with_build = ParseVersion("0.3.2+abc");
  CHECK(with_build && with_build->build == "abc");
  CHECK(with_build && SemanticVersion::Compare(*with_build, *stable) == 0);
  CHECK(ParseVersion("0.3.2-rc1+build.5")->prerelease.size() == 1);

  CHECK(!ParseVersion("1.0"));
  CHECK(!ParseVersion("1.0.0.0"));
  CHECK(!ParseVersion("01.0.0"));
  CHECK(!ParseVersion("1.0.0-"));
  CHECK(!ParseVersion("v"));
  CHECK(ParseVersion("V0.6.1").has_value());
}

void TestReleases() {
  std::printf("release JSON\n");
  constexpr const char* kJson = R"({
    "tag_name": "v0.7.0",
    "draft": false,
    "prerelease": false,
    "html_url": "https://github.com/saulob/alienation-pc/releases/tag/v0.7.0",
    "assets": [
      {"name": "AlienationPC-v0.7.0-windows-x64.zip",
       "browser_download_url": "https://github.com/example/release.zip",
       "size": 123456,
       "digest": "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
      {"name": "AlienationPC-v0.7.0-windows-x64.zip.sha256",
       "browser_download_url": "https://github.com/example/release.zip.sha256",
       "size": 107,
       "digest": "sha256:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"}
    ]
  })";

  std::string error;
  const std::optional<Release> release = ParseRelease(kJson, &error);
  CHECK(release.has_value());
  CHECK(error.empty());
  if (!release) {
    return;
  }
  CHECK(release->tag == "v0.7.0");
  CHECK(release->version.CoreString() == "0.7.0");
  CHECK(IsInstallableRelease(*release));
  CHECK(release->html_url.rfind("https://", 0) == 0);
  CHECK(release->assets.size() == 2);
  const ReleaseAsset* asset = FindAsset(*release, "AlienationPC-v0.7.0-windows-x64.zip");
  CHECK(asset != nullptr);
  CHECK(asset && asset->size == 123456);
  CHECK(asset && asset->sha256() ==
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(FindAsset(*release, "AlienationPC-v0.7.1-windows-x64.zip") == nullptr);
  CHECK(FindAsset(*release, "Alienation-v0.7.0-windows-x64.zip") == nullptr);
  CHECK(FindAsset(*release, "AlienationPC-v0.7.0-x86_64.AppImage") == nullptr);

  const std::optional<Release> draft = ParseRelease(R"({
    "tag_name":"v0.7.1","draft":true,"prerelease":false,"assets":[]
  })");
  CHECK(draft && !IsInstallableRelease(*draft));
  const std::optional<Release> prerelease = ParseRelease(R"({
    "tag_name":"v0.7.1-rc1","draft":false,"prerelease":true,"assets":[]
  })");
  CHECK(prerelease && !IsInstallableRelease(*prerelease));
  const std::optional<Release> prerelease_tag = ParseRelease(R"({
    "tag_name":"v0.7.1-rc1","draft":false,"prerelease":false,"assets":[]
  })");
  CHECK(prerelease_tag && !IsInstallableRelease(*prerelease_tag));

  CHECK(!ParseRelease(R"({"tag_name": 42})").has_value());
  CHECK(!ParseRelease(R"({"tag_name": "not-a-version"})").has_value());
  CHECK(!ParseRelease("[]").has_value());
  CHECK(!ParseRelease("{").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.7.0","assets":{}})").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.7.0","assets":[{"name":"x"}]})").has_value());

  CHECK(ParseSha256Digest("sha256:abc") == std::nullopt);
  CHECK(ParseSha256Digest("md5:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::nullopt);
  CHECK(ParseSha256Digest("SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") ==
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(ParseSha256File("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789  "
                        "AlienationPC-v0.7.0-windows-x64.zip\n") ==
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
  CHECK(ParseSha256File("not-a-hash  AlienationPC-v0.7.0-windows-x64.zip") == std::nullopt);
}

void TestSha256() {
  std::printf("SHA-256\n");
  CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_text(1000000, 'a');
  CHECK(Sha256Hex(long_text) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(Sha256Hex("The quick brown fox jumps over the lazy dog") ==
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK(Sha256Hex("tampered") != Sha256Hex("original"));

  TempDir temp;
  const std::filesystem::path file = temp.path / "hash.bin";
  WriteFile(file, "abc");
  std::string error;
  CHECK(Sha256FileHex(file, error) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(error.empty());
}

void TestZipAndStaging() {
  std::printf("ZIP extraction and staging\n");
  TempDir temp;
  std::string error;

  const std::filesystem::path wrapped_archive = temp.path / "wrapped.zip";
  const std::string wrapped = MakeZip({
      {"AlienationPC-v0.7.0-windows-x64/alienation.exe", "launcher"},
      {"AlienationPC-v0.7.0-windows-x64/assets/launcher/header-art.png", "art"},
      {"AlienationPC-v0.7.0-windows-x64/licenses/THIRD-PARTY-LICENSES.txt", "license"},
  });
  CHECK(!wrapped.empty());
  WriteFile(wrapped_archive, wrapped);
  const std::filesystem::path wrapped_staging = temp.path / "wrapped-staging";
  CHECK(ExtractZip(wrapped_archive, wrapped_staging, {}, error));
  std::filesystem::path wrapped_root;
  CHECK(ResolveStagingRoot(wrapped_staging, {"alienation.exe"}, wrapped_root, error));
  CHECK(wrapped_root == wrapped_staging / "AlienationPC-v0.7.0-windows-x64");
  CHECK(ReadFile(wrapped_root / "alienation.exe") == "launcher");
  CHECK(ValidateStaging(wrapped_root, {"alienation.exe", "assets/launcher/header-art.png"}, error));
  const std::vector<std::string> listed = ListStagingFiles(wrapped_root, error);
  CHECK(listed.size() == 3);
  CHECK(std::find(listed.begin(), listed.end(), "assets/launcher/header-art.png") != listed.end());
  CHECK(std::all_of(listed.begin(), listed.end(), [](const std::string& entry) { return entry.find('\\') == std::string::npos; }));

  const std::filesystem::path flat_archive = temp.path / "flat.zip";
  WriteFile(flat_archive, MakeZip({{"alienation.exe", "launcher"},
                                   {"assets/launcher/header-art.png", "art"},
                                   {"licenses/THIRD-PARTY-LICENSES.txt", "license"}}));
  const std::filesystem::path flat_staging = temp.path / "flat-staging";
  CHECK(ExtractZip(flat_archive, flat_staging, {}, error));
  std::filesystem::path flat_root;
  CHECK(ResolveStagingRoot(flat_staging, {"alienation.exe"}, flat_root, error));
  CHECK(flat_root == flat_staging);

  WriteFile(flat_root / "game" / "eboot.bin", "game");
  CHECK(!ValidateStaging(flat_root, {"alienation.exe"}, error));
  std::filesystem::remove_all(flat_root / "game");

  WriteFile(flat_root / "alienation.ini", "settings");
  CHECK(!ValidateStaging(flat_root, {"alienation.exe"}, error));
  std::filesystem::remove(flat_root / "alienation.ini");

  WriteFile(flat_root / "logs" / "a.log", "log");
  CHECK(!ValidateStaging(flat_root, {"alienation.exe"}, error));
  std::filesystem::remove_all(flat_root / "logs");

  for (const char* folder : {"game", "user", "logs", "cache", "settings", "saves", "profiles", "online-instances"}) {
    WriteFile(flat_root / folder / "nested" / "note.txt", "x");
    CHECK(!ValidateStaging(flat_root, {"alienation.exe"}, error));
    CHECK(ListStagingFiles(flat_root, error).empty() && !error.empty());
    std::filesystem::remove_all(flat_root / folder);
  }

  CHECK(!ValidateStaging(flat_root, {"missing.exe"}, error));

  const std::vector<std::string> unsafe_names = {
      "../evil",        "C:/evil",         "C:\\evil",       "\\\\server\\share", "a/../../b",
      "a\\..\\b",       "a//b",            "game/eboot.bin", "user/alienation.ini",      "user_data/settings",
      "logs/a.log",     "cache/a.spv",     "savedata/SAVE0.DAT", "CUSA04430/eboot.bin", "sce_sys/param.sfo",
      "alienation.ini",        "eboot.bin",       "param.sfo",      "game.zip",          "a/b.",
      "con",            "C:evil",
  };
  int rejected = 0;
  for (const std::string& name : unsafe_names) {
    const std::filesystem::path unsafe_archive = temp.path / "unsafe.zip";
    const std::string bytes = MakeZip({{name, "payload"}});
    CHECK(!bytes.empty());
    WriteFile(unsafe_archive, bytes);
    const std::filesystem::path unsafe_staging = temp.path / "unsafe-staging";
    std::filesystem::remove_all(unsafe_staging);
    if (!ExtractZip(unsafe_archive, unsafe_staging, {}, error)) {
      ++rejected;
    }
    CHECK(!std::filesystem::exists(temp.path / "evil"));
  }
  CHECK(rejected == int(unsafe_names.size()));
  CHECK(IsSymlinkZipEntry(0120000u << 16));
  CHECK(!IsSymlinkZipEntry(0100644u << 16));
  CHECK(!IsSymlinkZipEntry(0));

  const std::filesystem::path two_folders = temp.path / "two-folders.zip";
  WriteFile(two_folders, MakeZip({{"one/alienation.exe", "one"}, {"two/alienation.exe", "two"}}));
  const std::filesystem::path two_staging = temp.path / "two-staging";
  CHECK(ExtractZip(two_folders, two_staging, {}, error));
  std::filesystem::path two_root;
  CHECK(!ResolveStagingRoot(two_staging, {"alienation.exe"}, two_root, error));

  const std::filesystem::path dot_archive = temp.path / "dot.zip";
  WriteFile(dot_archive, MakeZip({{"./alienation.exe", "launcher"}}));
  const std::filesystem::path dot_staging = temp.path / "dot-staging";
  CHECK(ExtractZip(dot_archive, dot_staging, {}, error));
  CHECK(std::filesystem::exists(dot_staging / "alienation.exe"));
}

std::filesystem::path MakeJob(UpdateJob& job, const std::filesystem::path& temp, const std::string& manifest_text) {
  const std::filesystem::path install = temp / "install dir";
  const std::filesystem::path staging = temp / "staging dir";
  const std::filesystem::path backup = temp / "backup dir";
  const std::filesystem::path manifest = temp / "manifest.txt";
  std::filesystem::create_directories(install);
  std::filesystem::create_directories(staging);
  WriteFile(manifest, manifest_text);
  job = {};
  job.parent_pid = 1234;
  job.install_dir = install;
  job.staging_dir = staging;
  job.backup_dir = backup;
  job.manifest_file = manifest;
  job.restart_executable = install / "alienation.exe";
  job.log_file = temp / "updater.log";
  return install;
}

void TestJobs() {
  std::printf("update jobs\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "alienation.exe\n");
  WriteFile(install / "alienation.exe", "old launcher");
  WriteFile(job.staging_dir / "alienation.exe", "launcher");
  job.restart_arguments = {"--user=with space", "--launcher", ""};

  std::string error;
  const std::filesystem::path job_file = temp.path / "job.txt";
  CHECK(WriteUpdateJob(job_file, job, error));
  const std::optional<UpdateJob> read = ReadUpdateJob(job_file, error);
  CHECK(read.has_value());
  if (read) {
    CHECK(read->parent_pid == 1234);
    CHECK(read->install_dir == install);
    CHECK(read->restart_arguments == job.restart_arguments);
  }

  CHECK(ValidateInstallJob(job, error));

  UpdateJob inside = job;
  inside.staging_dir = install / "staging";
  std::filesystem::create_directories(inside.staging_dir);
  CHECK(!ValidateInstallJob(inside, error));

  UpdateJob relative = job;
  relative.manifest_file = "manifest.txt";
  CHECK(!ValidateInstallJob(relative, error));

  UpdateJob outside = job;
  outside.restart_executable = temp.path / "elsewhere.exe";
  CHECK(!ValidateInstallJob(outside, error));

  UpdateJob no_manifest = job;
  no_manifest.manifest_file = temp.path / "missing.txt";
  CHECK(!ValidateInstallJob(no_manifest, error));
}

void TestManifest() {
  std::printf("manifest validation\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "../evil\n");
  WriteFile(install / "alienation.exe", "old launcher");
  WriteFile(job.staging_dir / "alienation.exe", "new launcher");
  std::string error;
  InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "alienation.exe") == "old launcher");

  WriteFile(job.manifest_file, "user/alienation.ini\n");
  WriteFile(job.staging_dir / "user" / "alienation.ini", "replacement");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "alienation.exe") == "old launcher");

  WriteFile(job.manifest_file, "missing.dll\n");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "alienation.exe\nrexruntime.dll\nalienation.exe\n");
  WriteFile(job.staging_dir / "rexruntime.dll", "new runtime");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "alienation.exe") == "new launcher");
  CHECK(ReadFile(install / "rexruntime.dll") == "new runtime");
}

void TestSameVersionReplacement() {
  std::printf("same-version replacement\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "app.exe\nrelease-version.txt\n");
  job.restart_executable = install / "app.exe";
  WriteFile(install / "app.exe", "DEVELOPMENT_BUILD");
  WriteFile(install / "release-version.txt", "0.6.0");
  WriteFile(install / "user" / "alienation.ini", "player settings");
  WriteFile(install / "notes.txt", "keep me");
  WriteFile(job.staging_dir / "app.exe", "PUBLIC_RELEASE");
  WriteFile(job.staging_dir / "release-version.txt", "0.6.0");

  const InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "app.exe") == "PUBLIC_RELEASE");
  CHECK(ReadFile(install / "release-version.txt") == "0.6.0");
  CHECK(ReadFile(install / "user" / "alienation.ini") == "player settings");
  CHECK(ReadFile(install / "notes.txt") == "keep me");
}

void TestRollback() {
  std::printf("install and rollback\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "");
  const std::filesystem::path staging = job.staging_dir;
  const std::filesystem::path manifest = job.manifest_file;
  WriteFile(install / "alienation.exe", "old launcher");
  WriteFile(install / "rexruntime.dll", "old runtime");
  WriteFile(install / "user" / "alienation.ini", "player settings");
  WriteFile(install / "user" / "savedata" / "1" / "CUSA04430" / "SAVE0.DAT", "save data");
  WriteFile(install / "game" / "eboot.bin", "game files");
  WriteFile(install / "logs" / "session.log", "log");
  WriteFile(install / "cache" / "CUSA04430" / "pipeline.bin", "shader cache");
  WriteFile(install / "custom-notes.txt", "keep me");

  auto stage = [&] {
    WriteFile(staging / "alienation.exe", "new launcher");
    WriteFile(staging / "trg-updater.exe", "new helper");
    WriteFile(staging / "rexruntime.dll", "new runtime");
    WriteFile(staging / "assets" / "new" / "data.png", "new nested file");
    WriteFile(manifest, "alienation.exe\ntrg-updater.exe\nrexruntime.dll\nassets/new/data.png\n");
  };
  stage();

  int replacements = 0;
  InstallOptions fail;
  fail.replace = [&replacements](const std::filesystem::path& from, const std::filesystem::path& to) {
    ++replacements;
    if (replacements == 3) {
      return false;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
  };
  InstallOutcome failed = InstallFiles(job, fail, false);
  CHECK(failed.status == InstallStatus::kInstallFailedRolledBack);
  CHECK(ReadFile(install / "alienation.exe") == "old launcher");
  CHECK(!std::filesystem::exists(install / "trg-updater.exe"));
  CHECK(ReadFile(install / "rexruntime.dll") == "old runtime");
  CHECK(ReadFile(install / "user" / "alienation.ini") == "player settings");
  CHECK(ReadFile(install / "user" / "savedata" / "1" / "CUSA04430" / "SAVE0.DAT") == "save data");
  CHECK(ReadFile(install / "game" / "eboot.bin") == "game files");
  CHECK(ReadFile(install / "logs" / "session.log") == "log");
  CHECK(ReadFile(install / "cache" / "CUSA04430" / "pipeline.bin") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");

  stage();
  replacements = 0;
  const InstallOutcome success = InstallFiles(job, {}, false);
  CHECK(success.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "alienation.exe") == "new launcher");
  CHECK(ReadFile(install / "trg-updater.exe") == "new helper");
  CHECK(ReadFile(install / "rexruntime.dll") == "new runtime");
  CHECK(ReadFile(install / "assets" / "new" / "data.png") == "new nested file");
  CHECK(ReadFile(install / "user" / "alienation.ini") == "player settings");
  CHECK(ReadFile(install / "user" / "savedata" / "1" / "CUSA04430" / "SAVE0.DAT") == "save data");
  CHECK(ReadFile(install / "game" / "eboot.bin") == "game files");
  CHECK(ReadFile(install / "logs" / "session.log") == "log");
  CHECK(ReadFile(install / "cache" / "CUSA04430" / "pipeline.bin") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");
}

void TestPackagingNames() {
  std::printf("packaging names\n");
  const std::string config = ReadFile(std::string(ALN_UPDATER_SRC_DIR) + "/update_config.cpp");
  const std::string windows_package = ReadFile(std::string(ALN_SOURCE_DIR) + "/tools/package.ps1");
  const std::string linux_package = ReadFile(std::string(ALN_SOURCE_DIR) + "/tools/package-linux.sh");
  CHECK(config.find("https://api.github.com/repos/saulob/alienation-pc/releases/latest") !=
        std::string::npos);
  CHECK(config.find("AlienationPC-v\" + version.CoreString() + \"-windows-x64.zip") !=
        std::string::npos);
  CHECK(config.find("AlienationPC-v\" + version.CoreString() + \"-windows-x64.zip.sha256") !=
        std::string::npos);
  CHECK(config.find("AlienationPC-v\" + version.CoreString() + \"-x86_64.AppImage") !=
        std::string::npos);
  CHECK(config.find("AlienationPC-v\" + version.CoreString() + \"-x86_64.AppImage.sha256") !=
        std::string::npos);
  CHECK(windows_package.find("AlienationPC-v$version-windows-x64") != std::string::npos);
  CHECK(windows_package.find("trg-updater.exe") != std::string::npos);
  CHECK(windows_package.find("project\\(alienation_pc VERSION") != std::string::npos);
  CHECK(linux_package.find("AlienationPC-v$version-x86_64") != std::string::npos);
  CHECK(linux_package.find("project\\(alienation_pc VERSION") != std::string::npos);
}

void TestPaths() {
  std::printf("path safety\n");
  CHECK(IsSafeRelativePath("alienation.exe"));
  CHECK(IsSafeRelativePath("trg-updater.exe"));
  CHECK(IsSafeRelativePath("assets/launcher/header-art.png"));
  CHECK(IsSafeRelativePath("./alienation.exe"));
  CHECK(IsSafeRelativePath("AlienationPC-v0.7.0-windows-x64/alienation.exe"));
  CHECK(!IsSafeRelativePath("../alienation.exe"));
  CHECK(!IsSafeRelativePath("/absolute"));
  CHECK(!IsSafeRelativePath("C:/alienation.exe"));
  CHECK(!IsSafeRelativePath("\\\\server\\share"));
  CHECK(!IsSafeRelativePath("a//b"));
  CHECK(!IsSafeRelativePath("a/./../b"));
  CHECK(IsProtectedRelativePath("game/eboot.bin"));
  CHECK(IsProtectedRelativePath("game/prepared/CUSA04430/eboot.bin"));
  CHECK(IsProtectedRelativePath("user/alienation.ini"));
  CHECK(IsProtectedRelativePath("user/savedata/1/CUSA04430/SAVE0.DAT"));
  CHECK(IsProtectedRelativePath("logs/a.log"));
  CHECK(IsProtectedRelativePath("cache/CUSA04430/a.spv"));
  CHECK(IsProtectedRelativePath("user/cache/CUSA04430/pipeline.bin"));
  CHECK(IsProtectedRelativePath("content/foo"));
  CHECK(IsProtectedRelativePath("screenshots/a.png"));
  CHECK(IsProtectedRelativePath("alienation.ini"));
  CHECK(IsProtectedRelativePath("keys.json"));
  CHECK(IsProtectedRelativePath("CUSA04430/eboot.bin"));
  CHECK(IsProtectedRelativePath("sce_sys/param.sfo"));
  CHECK(IsProtectedRelativePath("DLC/a.bin"));
  CHECK(IsProtectedRelativePath("game.pkg"));
  CHECK(IsProtectedRelativePath("saves/a.txt"));
  CHECK(IsProtectedRelativePath("settings/a.txt"));
  CHECK(IsProtectedRelativePath("profiles/a.txt"));
  CHECK(IsProtectedRelativePath("achievements/a.txt"));
  CHECK(IsProtectedRelativePath("online-instances/a/user/alienation.ini"));
  CHECK(IsProtectedRelativePath("game/prepared/CUSA04430/eboot.bin"));
  CHECK(IsProtectedRelativePath("USER/Alienation.INI"));
  CHECK(!IsProtectedRelativePath("alienation.exe"));
  CHECK(!IsProtectedRelativePath("trg-updater.exe"));
  CHECK(!IsProtectedRelativePath("rexruntime.dll"));
  CHECK(!IsProtectedRelativePath("SDL3.dll"));
  CHECK(!IsProtectedRelativePath("libGLESv2.dll"));
  CHECK(!IsProtectedRelativePath("avcodec-62.dll"));
  CHECK(!IsProtectedRelativePath("assets/launcher/header-art.png"));
  CHECK(!IsProtectedRelativePath("assets/launcher/sponsor-heart.png"));
  CHECK(!IsProtectedRelativePath("licenses/THIRD-PARTY-LICENSES.txt"));
  CHECK(!IsProtectedRelativePath("licenses/angleproject/LICENSE"));
  CHECK(!IsProtectedRelativePath("README.txt"));
  CHECK(!IsProtectedRelativePath("LICENSE.txt"));
}

void TestRestartArguments() {
  std::printf("restart arguments\n");
  const std::vector<std::string> cleaned = aln::UpdateRestartArguments(
      {"--user", "C:/with space", "--aln-skip-launcher", "--launcher", "--launcher=0", "--log", "a.log"});
  const std::vector<std::string> expected = {"--user", "C:/with space", "--log", "a.log", "--launcher"};
  CHECK(cleaned == expected);
  CHECK(aln::UpdateRestartArguments({}) == std::vector<std::string>{"--launcher"});
  CHECK(!aln::IsLauncherVisibilityArgument("--launcher-width"));
  CHECK(!aln::IsLauncherVisibilityArgument("--aln-skip-launcher-x"));
  CHECK(aln::IsLauncherVisibilityArgument("--aln-skip-launcher"));
}

void TestAppImageTarget() {
  std::printf("the updater replaces only the AppImage the running program was started from\n");
  TempDir dir;
  const std::filesystem::path root = dir.path;
  const std::filesystem::path appimage = root / "AlienationPC-v0.7.0-x86_64.AppImage";
  WriteFile(appimage, "appimage");
  const std::filesystem::path mount = root / "mount";
  const std::filesystem::path program = mount / "usr" / "bin" / "alienation";
  WriteFile(program, "program");
  CHECK(IsRunningAppImage(appimage, mount, program));

  const std::filesystem::path extracted = root / ".extract" / "squashfs-root";
  const std::filesystem::path extracted_program = extracted / "usr" / "bin" / "alienation";
  WriteFile(extracted_program, "program");
  CHECK(IsRunningAppImage(appimage, extracted, extracted_program));

  const std::filesystem::path build = root / "build" / "alienation";
  WriteFile(build, "program");
  CHECK(!IsRunningAppImage(appimage, mount, build));

  for (const char* name : {"Other-v1.0.0-x86_64.AppImage", "AlienationPC-v-x86_64.AppImage",
                           "AlienationPC-vx-x86_64.AppImage", "AlienationPC-v0.7.0-aarch64.AppImage"}) {
    const std::filesystem::path other = root / name;
    WriteFile(other, "other");
    CHECK(!IsRunningAppImage(other, mount, program));
  }

  const std::filesystem::path inside = mount / "AlienationPC-v0.7.0-x86_64.AppImage";
  WriteFile(inside, "appimage");
  CHECK(!IsRunningAppImage(inside, mount, program));

  CHECK(!IsRunningAppImage({}, mount, program));
  CHECK(!IsRunningAppImage(appimage, {}, program));
  CHECK(!IsRunningAppImage(appimage, mount, {}));
  CHECK(!IsRunningAppImage(appimage, root / "not-a-mount", program));
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestVersions();
  TestReleases();
  TestSha256();
  TestZipAndStaging();
  TestJobs();
  TestManifest();
  TestSameVersionReplacement();
  TestRollback();
  TestPackagingNames();
  TestPaths();
  TestRestartArguments();
  TestAppImageTarget();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
