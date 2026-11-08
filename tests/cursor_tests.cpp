// Cursor visibility tests: the last-device state machine and the controller activity edges. No window, no SDL.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "aln_cursor.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

using AlnCursor::ControllerState;

constexpr std::uint32_t Bit(int button) {
    return 1u << button;
}

constexpr std::uint32_t kCross = Bit(0);
constexpr std::uint32_t kCircle = Bit(1);
constexpr std::uint32_t kDpadUp = Bit(2);

int TriggerAt(int units) {
    return units * 128;
}

bool Apply(AlnCursor::State& state, bool gameplay = true, bool focused = true, bool overlay = false) {
    state.Update(gameplay, focused, overlay);
    return state.Visible();
}

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

int Count(const std::string& text, const std::string& needle) {
    int count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

void TestInitialVisible() {
    AlnCursor::State state;
    CHECK(state.Visible());
    CHECK(state.LastDevice() == AlnCursor::Device::Mouse);
    CHECK(!state.Update(true, true, false).has_value());
    CHECK(state.Visible());
}

void TestMouseActivityShows() {
    AlnCursor::State state;
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseButtonPressed();
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseWheelMoved();
    CHECK(Apply(state));
}

void TestKeyboardActivityHides() {
    AlnCursor::State state;
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));
}

void TestKeyRepeatAndHostShortcuts() {
    CHECK(AlnCursor::IsGameplayKey(false, false));
    CHECK(!AlnCursor::IsGameplayKey(true, false));
    CHECK(!AlnCursor::IsGameplayKey(false, true));
    CHECK(!AlnCursor::IsGameplayKey(true, true));

    AlnCursor::State state;
    state.KeyboardActivity();
    CHECK(!Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));
    state.MouseMoved();
    CHECK(Apply(state));
}

void TestControllerButtonHides() {
    AlnCursor::State state;
    AlnCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.buttons = kCross;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    CHECK(!filter.Update(pad));
    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.buttons |= kCircle;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    pad.buttons = 0;
    CHECK(!filter.Update(pad));
    pad.buttons = kDpadUp;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestStickCrossing() {
    AlnCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_x = AlnCursor::kStickDeadzone - 1;
    CHECK(!filter.Update(pad));
    pad.left_x = AlnCursor::kStickDeadzone;
    CHECK(filter.Update(pad));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.left_x = -AlnCursor::kStickDeadzone;
    CHECK(filter.Update(pad));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.right_y = AlnCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
}

void TestHeldStickDoesNotOverrideMouse() {
    AlnCursor::State state;
    AlnCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_x = AlnCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.left_x = AlnCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestTriggerCrossing() {
    AlnCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_trigger = TriggerAt(AlnCursor::kTriggerThreshold - 1);
    CHECK(!filter.Update(pad));
    pad.left_trigger = TriggerAt(AlnCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));

    pad.left_trigger = 0;
    CHECK(!filter.Update(pad));
    pad.right_trigger = TriggerAt(AlnCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
}

void TestHeldTriggerDoesNotOverrideMouse() {
    AlnCursor::State state;
    AlnCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_trigger = TriggerAt(AlnCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.left_trigger = TriggerAt(AlnCursor::kTriggerThreshold - 1);
    CHECK(!filter.Update(pad));
    pad.left_trigger = TriggerAt(AlnCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestOverlayForcesVisible() {
    AlnCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, true, true));
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
    CHECK(state.LastDevice() == AlnCursor::Device::Controller);
}

void TestUiInputKeepsLastDevice() {
    AlnCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, true, true));

    state.MouseMoved();
    state.MouseButtonPressed();
    state.KeyboardActivity();
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
    CHECK(state.LastDevice() == AlnCursor::Device::Controller);

    AlnCursor::State keyboard;
    keyboard.KeyboardActivity();
    CHECK(!Apply(keyboard));
    CHECK(Apply(keyboard, true, true, true));
    keyboard.MouseMoved();
    CHECK(Apply(keyboard, true, true, true));
    CHECK(!Apply(keyboard, true, true, false));
    CHECK(keyboard.LastDevice() == AlnCursor::Device::Keyboard);
}

void TestFocus() {
    AlnCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, false, false));
    CHECK(!Apply(state, true, true, false));

    CHECK(Apply(state, true, false, true));
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
}

void TestWindowedAndBorderless() {
    // Both modes share the same path.
    AlnCursor::State windowed;
    windowed.ControllerActivity();
    CHECK(!Apply(windowed));
    windowed.MouseMoved();
    CHECK(Apply(windowed));

    AlnCursor::State borderless;
    borderless.ControllerActivity();
    CHECK(!Apply(borderless));
    borderless.MouseMoved();
    CHECK(Apply(borderless));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "SDL_WINDOW_FULLSCREEN"));
}

void TestLauncherUnaffected() {
    AlnCursor::State state;
    state.KeyboardActivity();
    CHECK(Apply(state, false, true, false));
    state.ControllerActivity();
    CHECK(Apply(state, false, true, false));
    CHECK(state.LastDevice() == AlnCursor::Device::Mouse);

    const std::string launcher = ReadSource("launcher/host.cpp");
    CHECK(!launcher.empty());
    CHECK(!Contains(launcher, "AlnCursor"));
}

void TestControllersIndependent() {
    AlnCursor::ControllerActivityFilter first;
    AlnCursor::ControllerActivityFilter second;
    ControllerState a;
    ControllerState b;
    a.buttons = kCross;
    CHECK(first.Update(a));
    CHECK(!first.Update(a));

    b.buttons = kCircle;
    CHECK(second.Update(b));
    CHECK(!second.Update(b));

    a.buttons |= kCircle;
    CHECK(first.Update(a));
    CHECK(!second.Update(b));
}

void TestNoTimeout() {
    AlnCursor::State state;
    state.MouseMoved();
    for (int i = 0; i < 10000; ++i) {
        state.Update(true, true, false);
    }
    CHECK(state.Visible());

    state.KeyboardActivity();
    CHECK(!Apply(state));
    for (int i = 0; i < 10000; ++i) {
        state.Update(true, true, false);
    }
    CHECK(!state.Visible());

    const std::string header = ReadSource("gpu/shim/aln_cursor.h");
    CHECK(!header.empty());
    CHECK(!Contains(header, "chrono"));
    CHECK(!Contains(header, "kHideDelay"));
    CHECK(!Contains(header, "last_motion"));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "kHideDelay"));
    CHECK(!Contains(window, "last_motion"));
}

void TestNoSecondGamepadPath() {
    const std::string header = ReadSource("gpu/shim/aln_cursor.h");
    CHECK(!header.empty());
    CHECK(!Contains(header, "SDL_"));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "SDL_GetGamepads"));
    CHECK(!Contains(window, "SDL_OpenGamepad"));
    CHECK(Count(window, "SDL_InitSubSystem") == 1);
    CHECK(Count(window, "SDL_INIT_GAMEPAD") == 1);
}

} // namespace

int main() {
    TestInitialVisible();
    TestMouseActivityShows();
    TestKeyboardActivityHides();
    TestKeyRepeatAndHostShortcuts();
    TestControllerButtonHides();
    TestStickCrossing();
    TestHeldStickDoesNotOverrideMouse();
    TestTriggerCrossing();
    TestHeldTriggerDoesNotOverrideMouse();
    TestOverlayForcesVisible();
    TestUiInputKeepsLastDevice();
    TestFocus();
    TestWindowedAndBorderless();
    TestLauncherUnaffected();
    TestControllersIndependent();
    TestNoTimeout();
    TestNoSecondGamepadPath();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
