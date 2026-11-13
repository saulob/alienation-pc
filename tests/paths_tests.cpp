// The data folder: the game, saves, settings and logs sit beside the program; on Linux beside the AppImage file (never
// inside its read-only mount), or in $XDG_DATA_HOME/AlienationPC when that folder cannot be written. No window.
// Run: aln-paths-tests

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "aln_launcher.h"
#include "paths.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

#ifndef _WIN32
void SetEnv(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }
#endif

void UnsetEnv(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

bool Under(const fs::path& path, const fs::path& folder) {
  const std::string inner = path.lexically_normal().generic_string();
  const std::string outer = folder.lexically_normal().generic_string();
  return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 && inner[outer.size()] == '/';
}

fs::path MakeTemp(const char* name) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path path = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(now));
  fs::create_directories(path);
  return path;
}

void TestPortableDefaults(const fs::path& temp) {
  std::printf("portable defaults\n");
  const fs::path program = temp / "Alienation PC";
  const aln::Paths paths = aln::ResolvePaths({}, program);
  CHECK(paths.error.empty());
  CHECK(paths.user_dir == program / "user");
  CHECK(paths.logs_dir == program / "logs");
  CHECK(Under(paths.app0, program / "game"));
  CHECK(Under(paths.image_dir, program / "game"));

  aln::PathRequest request;
  request.user_dir = (temp / "elsewhere" / "user").string();
  request.logs_dir = (temp / "elsewhere" / "logs").string();
  const aln::Paths chosen = aln::ResolvePaths(request, program);
  CHECK(chosen.user_dir == temp / "elsewhere" / "user");
  CHECK(chosen.logs_dir == temp / "elsewhere" / "logs");
}

#ifndef _WIN32
bool Resolve(AlnPaths& out) {
  const AlnPathRequest request{};
  out = AlnPaths{};
  return aln_resolve_paths(&request, &out) != 0;
}

void TestAppImageDataFolder(const fs::path& temp) {
  std::printf("AppImage data folder\n");
  const fs::path games = temp / "Games";
  fs::create_directories(games);
  const fs::path image = games / "AlienationPC-v0.1.0-x86_64.AppImage";
  std::FILE* file = std::fopen(image.c_str(), "wb");
  if (file) {
    std::fclose(file);
  }
  std::error_code ec;
  const fs::path mount = fs::read_symlink("/proc/self/exe", ec).parent_path();

  UnsetEnv("ALN_PORTABLE_DIR");
  SetEnv("APPIMAGE", image.string());
  AlnPaths out;
  CHECK(Resolve(out));
  CHECK(Under(out.user_dir, games));
  CHECK(Under(out.logs_dir, games));
  CHECK(Under(out.app0, games));
  CHECK(!Under(out.user_dir, mount));

  SetEnv("ALN_PORTABLE_DIR", (temp / "portable").string());
  CHECK(Resolve(out));
  CHECK(Under(out.user_dir, temp / "portable"));
  UnsetEnv("ALN_PORTABLE_DIR");
}

// A read-only folder holding the AppImage: run as a user the folder's permissions apply to (root may write anywhere).
int ReadOnlyFolderChild() {
  const fs::path temp = MakeTemp("aln-paths-readonly");
  const fs::path games = temp / "Games";
  fs::create_directories(games);
  const fs::path image = games / "AlienationPC-v0.1.0-x86_64.AppImage";
  if (std::FILE* file = std::fopen(image.c_str(), "wb")) {
    std::fclose(file);
  }
  chmod(games.c_str(), 0555);
  SetEnv("APPIMAGE", image.string());
  SetEnv("XDG_DATA_HOME", (temp / "xdg").string());
  UnsetEnv("ALN_PORTABLE_DIR");
  AlnPaths out;
  CHECK(Resolve(out));
  CHECK(Under(out.user_dir, temp / "xdg" / "AlienationPC"));
  CHECK(Under(out.logs_dir, temp / "xdg" / "AlienationPC"));
  CHECK(!Under(out.user_dir, games));
  UnsetEnv("XDG_DATA_HOME");
  SetEnv("HOME", (temp / "home").string());
  CHECK(Resolve(out));
  CHECK(Under(out.user_dir, temp / "home" / ".local" / "share" / "AlienationPC"));
  chmod(games.c_str(), 0755);
  fs::remove_all(temp);
  return g_failures;
}

void TestReadOnlyFolder() {
  std::printf("read-only AppImage folder\n");
  if (geteuid() != 0) {
    const int before = g_failures;
    ReadOnlyFolderChild();
    CHECK(g_failures == before);
    return;
  }
  const pid_t child = fork();
  if (child == 0) {
    const gid_t nobody = 65534;
    if (setgroups(0, nullptr) != 0 || setgid(nobody) != 0 || setuid(nobody) != 0) {
      _exit(90);
    }
    _exit(ReadOnlyFolderChild() == 0 ? 0 : 1);
  }
  int status = 0;
  waitpid(child, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  UnsetEnv("ALN_GAME_BUILD");
  const fs::path temp = MakeTemp("aln-paths-tests");
  TestPortableDefaults(temp);
#ifndef _WIN32
  TestAppImageDataFolder(temp);
  TestReadOnlyFolder();
#endif
  std::error_code ec;
  fs::remove_all(temp, ec);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
