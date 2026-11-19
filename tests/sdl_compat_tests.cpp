// Focused tests of the Proton/Wine SDL HIDAPI default (issue #42) and of the user gamepad mapping file and the
// unmapped joystick log (the pure rules; tests/local_pad_tests.c runs them against real SDL virtual joysticks).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

#define ALN_SDL_COMPAT_POLICY_ONLY
#include "aln_pad_mapping.h"
#include "aln_sdl_compat.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

std::string ReadSource(const char* path) {
    std::ifstream in(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

void TestPolicy() {
    CHECK(aln_hidapi_action(0, nullptr) == ALN_HIDAPI_LEAVE);
    CHECK(aln_hidapi_action(0, "") == ALN_HIDAPI_LEAVE);
    CHECK(aln_hidapi_action(1, nullptr) == ALN_HIDAPI_DISABLE);
    CHECK(aln_hidapi_action(1, "") == ALN_HIDAPI_DISABLE);
    CHECK(aln_hidapi_action(1, "0") == ALN_HIDAPI_PRESERVE);
    CHECK(aln_hidapi_action(1, "1") == ALN_HIDAPI_PRESERVE);
    CHECK(aln_hidapi_action(0, "0") == ALN_HIDAPI_PRESERVE);
    CHECK(aln_hidapi_action(0, "1") == ALN_HIDAPI_PRESERVE);
}

void TestProbeIsNotWineOnNative() {
#ifdef _WIN32
    CHECK(aln_running_under_wine() == 0 || GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version"));
#else
    CHECK(aln_running_under_wine() == 0);
#endif
}

static_assert(std::is_same_v<decltype(&aln_hidapi_action), AlnHidapiAction (*)(int, const char*)>);
static_assert(std::is_same_v<decltype(&aln_running_under_wine), int (*)(void)>);

size_t Pos(const std::string& text, const char* needle) {
    const size_t at = text.find(needle);
    if (at == std::string::npos) {
        std::printf("FAIL missing: %s\n", needle);
        ++g_failures;
    }
    return at;
}

size_t Count(const std::string& text, const char* needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

std::string LineBefore(const std::string& text, size_t at) {
    const size_t end = text.rfind('\n', at);
    const size_t start = text.rfind('\n', end - 1);
    return text.substr(start + 1, end - start - 1);
}

void TestNativeAndWineMatrix() {
    CHECK(aln_hidapi_action(0, nullptr) == ALN_HIDAPI_LEAVE);
    CHECK(aln_hidapi_action(1, nullptr) == ALN_HIDAPI_DISABLE);
    CHECK(aln_hidapi_action(1, "0") == ALN_HIDAPI_PRESERVE);
    CHECK(aln_hidapi_action(1, "1") == ALN_HIDAPI_PRESERVE);
}

void TestPolicyIsHostOnly() {
    const std::string header = ReadSource("src/aln_sdl_compat.h");
    CHECK(!header.empty());
    for (const char* forbidden : {"language", "LANGUAGE", "game_build", "GameBuild", "title_id", "CUSA", "region", "eboot",
                                  "getenv", "WINEPREFIX", "STEAM_COMPAT", "28de", "11ff", "Bazzite", "Deck"}) {
        if (header.find(forbidden) != std::string::npos) {
            std::printf("FAIL header mentions %s\n", forbidden);
            ++g_failures;
        }
    }
    CHECK(Pos(header, "SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)") != std::string::npos);
    CHECK(Pos(header, "wine_get_version") != std::string::npos);
    CHECK(Pos(header, "#ifdef _WIN32") < Pos(header, "wine_get_version"));
}

void TestGameProcessOrder() {
    const std::string loader = ReadSource("src/loader.c");
    const size_t prepare = Pos(loader, "aln_prepare_sdl_gamepad();");
    CHECK(Count(loader, "aln_prepare_sdl_gamepad();") == 1);
    CHECK(LineBefore(loader, prepare).find("if") == std::string::npos);
    CHECK(Pos(loader, "aln_launcher_run(&launcher)") < prepare);
    for (const char* later : {"aln_game_build(paths.image_dir", "aln_render_resolution(", "runtime_pad_set_settings(",
                              "alngpu_init("}) {
        CHECK(prepare < Pos(loader, later));
    }
    CHECK(Pos(loader, "if (game_child) {") < prepare);
    CHECK(Pos(loader, "if (!cpu_only && !check_imports && !game_child) {") < prepare);

    const std::string window = ReadSource("gpu/shim/window.cpp");
    const size_t window_prepare = Pos(window, "aln_prepare_sdl_gamepad();");
    CHECK(Count(window, "aln_prepare_sdl_gamepad();") == 1);
    CHECK(window_prepare < Pos(window, "SDL_INIT_GAMEPAD"));
    CHECK(window.find("aln_game_build") == std::string::npos && window.find("text_language") == std::string::npos);

    const std::string pad = ReadSource("src/runtime_pad.c");
    const size_t pad_prepare = Pos(pad, "aln_prepare_sdl_gamepad();");
    CHECK(Count(pad, "aln_prepare_sdl_gamepad();") == 1);
    CHECK(pad_prepare < Pos(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)"));
    CHECK(Count(pad, "aln_hidapi_action") == 0);
    CHECK(Count(pad, "aln_running_under_wine") == 1 && pad.find("PadDiag: platform=") != std::string::npos);
}

std::string DbPath(const char* settings, size_t size = 4300) {
    std::string buffer(size, 'x');
    const int ok = aln_gamepad_db_path(settings, buffer.data(), size);
    return ok ? std::string(buffer.c_str()) : std::string("<none>");
}

void TestGamepadDbPath() {
    CHECK(DbPath("user/alienation.ini") == "user/gamecontrollerdb.txt");
    CHECK(DbPath("C:\\path\\user\\alienation.ini") == "C:\\path\\user\\gamecontrollerdb.txt");
    CHECK(DbPath("C:/path/user/alienation.ini") == "C:/path/user/gamecontrollerdb.txt");
    CHECK(DbPath("C:\\path/user\\alienation.ini") == "C:\\path/user\\gamecontrollerdb.txt");
    CHECK(DbPath("C:/path\\user/alienation.ini") == "C:/path\\user/gamecontrollerdb.txt");
    CHECK(DbPath("alienation.ini") == "gamecontrollerdb.txt");
    CHECK(DbPath("/alienation.ini") == "/gamecontrollerdb.txt");
    CHECK(DbPath("user/") == "user/gamecontrollerdb.txt");
    CHECK(DbPath("/home/me/My Games/user/alienation.ini") == "/home/me/My Games/user/gamecontrollerdb.txt");
    CHECK(DbPath(nullptr) == "<none>");
    CHECK(DbPath("") == "<none>");

    // A buffer one byte short, exact, and absurdly small: no write past it, nothing named.
    const std::string full = "user/gamecontrollerdb.txt";
    for (size_t size = 0; size <= full.size() + 2; ++size) {
        std::string guarded(full.size() + 16, '#');
        const int ok = aln_gamepad_db_path("user/alienation.ini", guarded.data(), size);
        CHECK(ok == (size >= full.size() + 1));
        for (size_t i = size; i < guarded.size(); ++i) CHECK(guarded[i] == '#');
        if (!ok && size) CHECK(guarded[0] == 0);
    }
    const std::string long_folder(5000, 'd');
    CHECK(DbPath((long_folder + "/alienation.ini").c_str()) == "<none>");
    CHECK(aln_gamepad_db_path("user/alienation.ini", nullptr, 10) == 0);
}

std::string Line(const char* name, const char* guid, unsigned vendor, unsigned product) {
    char line[384];
    aln_unmapped_line(name, guid, vendor, product, line, sizeof(line));
    return line;
}

void TestUnmappedLine() {
    const char* guid = "030000005e040000d102000001000000";
    CHECK(Line("Pad", guid, 0x045e, 0x02d1) ==
          "Runtime: joystick without a gamepad mapping: Pad (guid 030000005e040000d102000001000000, 045e:02d1); add a line for "
          "this GUID to gamecontrollerdb.txt in the user folder");
    CHECK(Line("Pad", guid, 0x1, 0xabc).find("(guid 030000005e040000d102000001000000, 0001:0abc)") != std::string::npos);
    CHECK(Line("Pad", guid, 0, 0x10).find(", 0000:0010)") != std::string::npos);
    CHECK(Line("Pad", guid, 0, 0).find("(guid 030000005e040000d102000001000000);") != std::string::npos);
    CHECK(Line("Pad", guid, 0, 0).find(":0000") == std::string::npos);

    // No name from SDL: nothing is invented.
    for (const char* none : {static_cast<const char*>(nullptr), ""}) {
        const auto line = Line(none, guid, 0x045e, 0x02d1);
        CHECK(line.rfind("Runtime: joystick without a gamepad mapping: (guid ", 0) == 0);
        CHECK(line.find("  ") == std::string::npos && line.find("unknown") == std::string::npos);
    }
    const auto control = Line("Bad\tName\nwith\rcontrols", guid, 1, 2);
    CHECK(control.find_first_of("\t\n\r") == std::string::npos && control.find("Bad Name with controls") != std::string::npos);
    const auto huge = Line(std::string(5000, 'n').c_str(), guid, 1, 2);
    CHECK(huge.size() < 384 && huge.find("add a line for this GUID") != std::string::npos);
    char tiny[24];
    aln_unmapped_line("Pad", guid, 1, 2, tiny, sizeof(tiny));
    CHECK(std::strlen(tiny) < sizeof(tiny));

    // The line is built from name, GUID and VID:PID only: there is no way to hand it a serial number or a path.
    const std::string header = ReadSource("src/aln_pad_mapping.h");
    CHECK(!header.empty() && header.find("SDL_") == std::string::npos);
}

void TestUnmappedSeenIsBoundedAndForgetsAbsentJoysticks() {
    AlnUnmappedSeen seen{};
    CHECK(aln_unmapped_first_sight(&seen, 7) == 1);
    CHECK(aln_unmapped_first_sight(&seen, 7) == 0);
    CHECK(aln_unmapped_first_sight(&seen, 8) == 1);
    CHECK(seen.count == 2);

    // Gone and back: a fresh line. Still there: nothing.
    const uint32_t both[] = {7, 8};
    aln_unmapped_forget_absent(&seen, both, 2);
    CHECK(seen.count == 2 && aln_unmapped_first_sight(&seen, 7) == 0);
    const uint32_t only8[] = {8};
    aln_unmapped_forget_absent(&seen, only8, 1);
    CHECK(seen.count == 1 && aln_unmapped_first_sight(&seen, 7) == 1 && aln_unmapped_first_sight(&seen, 8) == 0);
    aln_unmapped_forget_absent(&seen, nullptr, 0);
    CHECK(seen.count == 0);

    // Many connect and disconnect cycles, and more simultaneous joysticks than it holds: the storage never grows.
    unsigned lines = 0;
    for (uint32_t id = 1; id <= 100000; ++id) {
        const uint32_t present[] = {id};
        aln_unmapped_forget_absent(&seen, present, 1);
        lines += static_cast<unsigned>(aln_unmapped_first_sight(&seen, id));
        CHECK(seen.count <= ALN_UNMAPPED_SEEN);
    }
    CHECK(lines == 100000 && seen.count == 1);
    uint32_t crowd[ALN_UNMAPPED_SEEN * 3];
    for (size_t i = 0; i < ALN_UNMAPPED_SEEN * 3; ++i) crowd[i] = 1000 + static_cast<uint32_t>(i);
    for (const uint32_t id : crowd) {
        aln_unmapped_forget_absent(&seen, crowd, ALN_UNMAPPED_SEEN * 3);
        CHECK(aln_unmapped_first_sight(&seen, id) == 1 && seen.count <= ALN_UNMAPPED_SEEN);
    }
    CHECK(static_cast<size_t>(sizeof(seen)) <= ALN_UNMAPPED_SEEN * sizeof(uint32_t) + sizeof(size_t));
}

void TestMappingFileIsAdditiveAndLoadedBeforeEnumeration() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const size_t init = Pos(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)");
    const size_t load_call = Pos(pad, "if (sdl_ready>0) load_user_gamepad_mappings();");
    const size_t enumerate = Pos(pad, "SDL_GetGamepads(&count)");
    const size_t unmapped_call = Pos(pad, "        log_unmapped_joysticks();");
    CHECK(Pos(pad, "aln_prepare_sdl_gamepad();") < init && init < load_call && load_call < unmapped_call && unmapped_call < enumerate);
    CHECK(Count(pad, "load_user_gamepad_mappings();") == 1 && Count(pad, "SDL_AddGamepadMappingsFromFile(") == 1);
    CHECK(Count(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)") == 1);

    // Nothing is guessed or overwritten: only the player's file is read, and SDL's own configuration is left alone.
    for (const char* forbidden : {"SDL_SetGamepadMapping", "SDL_AddGamepadMapping(", "SDL_AddGamepadMappingsFromIO",
                                  "\"SDL_GAMECONTROLLERCONFIG", "SDL_HINT_GAMECONTROLLERCONFIG", "SDL_ReloadGamepadMappings",
                                  "SDL_GetJoystickSerial", "SDL_GetJoystickPath", "SDL_GetGamepadSerial", "SDL_GetGamepadPath"}) {
        if (pad.find(forbidden) != std::string::npos) {
            std::printf("FAIL runtime_pad.c uses %s\n", forbidden);
            ++g_failures;
        }
    }
    CHECK(pad.find("SDL_JoystickID") != std::string::npos);

    // A missing file is silent: the load returns before reading when SDL finds nothing at the path.
    CHECK(Pos(pad, "!SDL_GetPathInfo(path,NULL)) return;") < Pos(pad, "SDL_AddGamepadMappingsFromFile("));
    // The log scan does not change when gamepads are enumerated.
    CHECK(Pos(pad, "if (free_slot()<0 || (last_scan && now-last_scan<250000)) return;") != std::string::npos);
}


void TestLauncherSeparate() {
    const std::string host = ReadSource("launcher/host.cpp");
    CHECK(host.find("aln_sdl_compat") == std::string::npos);
    CHECK(host.find("aln_prepare_sdl_gamepad") == std::string::npos);
    CHECK(host.find("SDL_Quit()") != std::string::npos);
}

void TestInputIgnoresBuildAndLanguage() {
    for (const char* file : {"src/runtime_pad.c", "launcher/host.cpp"}) {
        const std::string text = ReadSource(file);
        for (const char* key : {"CUSA", "ALN_LANGUAGE", "text_language", "aln_game_build"}) {
            CHECK(text.find(key) == std::string::npos);
        }
    }
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(pad.find("runtime_language") == std::string::npos && pad.find("language") == std::string::npos);
}

void TestEnumerationUnchanged() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Pos(pad, "#define MAX_PADS 4") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepads") != std::string::npos);
    CHECK(Pos(pad, "SDL_OpenGamepad") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadButton") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadAxis") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadTouchpadFinger") != std::string::npos);
    CHECK(Pos(pad, "ALN_PAD_MAP") != std::string::npos);
    CHECK(Pos(pad, "ALN_KEY_MAP") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetKeyboardState") != std::string::npos);
    CHECK(Pos(pad, "free_slot") != std::string::npos);
    CHECK(Pos(pad, "Runtime: gamepad %d disconnected") != std::string::npos);
    CHECK(Pos(pad, "Steam Input virtual gamepad") != std::string::npos);
}

} // namespace

int main() {
    TestPolicy();
    TestProbeIsNotWineOnNative();
    TestNativeAndWineMatrix();
    TestPolicyIsHostOnly();
    TestGameProcessOrder();
    TestLauncherSeparate();
    TestInputIgnoresBuildAndLanguage();
    TestEnumerationUnchanged();
    TestGamepadDbPath();
    TestUnmappedLine();
    TestUnmappedSeenIsBoundedAndForgetsAbsentJoysticks();
    TestMappingFileIsAdditiveAndLoadedBeforeEnumeration();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sdl compat tests passed\n");
    return 0;
}
