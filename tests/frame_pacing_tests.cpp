// Focused tests of the Frame rate setting: the mode table, which flips reach the window, the repeat cadence and its
// limiter (aln_frame_pacing.h), the title, and a model of the game's own timing driven through them at every setting.
// No window, no GPU and no game files.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aln_frame_pacing.h"
#include "aln_title.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

namespace Pacing = AlnFramePacing;
using Clock = std::chrono::steady_clock;

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const std::size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

Clock::time_point At(double seconds) {
    return Clock::time_point{} + std::chrono::nanoseconds(std::llround(seconds * 1e9));
}

void TestModeTable() {
    const int expected[] = {30, 60, 90, 120, 144, 165, 240, 0};
    CHECK(Pacing::kFrameRateModes.size() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        CHECK(Pacing::kFrameRateModes[i] == expected[i]);
        CHECK(Pacing::IsFrameRateMode(expected[i]));
    }
    CHECK(!Pacing::IsFrameRateMode(-1) && !Pacing::IsFrameRateMode(45) && !Pacing::IsFrameRateMode(1000));
    CHECK(Pacing::RenderFps(45) == 60 && Pacing::RenderFps(-1) == 60);
    CHECK(Pacing::RenderFps(0) == 0 && Pacing::RenderFps(144) == 144);
}

void TestParseFrameRate() {
    for (const int mode : Pacing::kFrameRateModes) {
        CHECK(Pacing::ParseFrameRate(std::to_string(mode)) == mode);
    }
    for (const char* invalid : {"", "abc", "45", "-1", "090", "+90", "90.0", "90fps", "0x5a", "00", " 90", "1000"}) {
        CHECK(Pacing::ParseFrameRate(invalid) == 60);
    }
    CHECK(Pacing::ModeName(0) == "Unlimited" && Pacing::ModeName(144) == "144");
}

void TestOriginalRateAddsNothing() {
    CHECK(!Pacing::IsHighRate(60) && !Pacing::IsHighRate(30) && !Pacing::IsHighRate(-1));
    for (const bool vsync : {false, true}) {
        for (const double display : {0.0, 59.94, 60.0, 144.0, 240.0}) {
            CHECK(Pacing::RepeatCadenceHz(60, vsync, display) == 0);
            CHECK(Pacing::RepeatCadenceHz(30, vsync, display) == 0);
        }
    }
    for (std::uint64_t flip = 0; flip < 8; ++flip) {
        CHECK(Pacing::PresentsFlip(60, flip));
    }
}

void TestThirtyShowsEverySecondFlip() {
    int shown = 0;
    for (std::uint64_t flip = 0; flip < 100; ++flip) {
        shown += Pacing::PresentsFlip(30, flip) ? 1 : 0;
    }
    CHECK(shown == 50);
    // A game that flips every second vblank by itself is already at 30: every flip is shown.
    for (std::uint64_t flip = 0; flip < 8; ++flip) {
        CHECK(Pacing::PresentsFlip(30, flip, 1) && Pacing::PresentsFlip(30, flip, 2));
    }
}

void TestRepeatCadence() {
    for (const int fps : {90, 120, 144, 165, 240}) {
        CHECK(Pacing::RepeatCadenceHz(fps) == static_cast<unsigned>(fps));
        CHECK(Pacing::PresentsFlip(fps, 1));
    }
    CHECK(Pacing::RepeatCadenceHz(0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
    CHECK(Pacing::RepeatCadenceHz(0, false, 144.0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
    // VSync bounds the cadence to the display; a display at the game's rate leaves nothing to repeat.
    CHECK(Pacing::RepeatCadenceHz(120, true, 60.0) == 0 && Pacing::RepeatCadenceHz(0, true, 59.94) == 0);
    CHECK(Pacing::RepeatCadenceHz(120, true, 144.0) == 120);
    CHECK(Pacing::RepeatCadenceHz(240, true, 143.856) == 144);
    CHECK(Pacing::RepeatCadenceHz(0, true, 165.0) == 165);
    CHECK(Pacing::RepeatCadenceHz(240, false, 144.0) == 240);
    CHECK(Pacing::RepeatCadenceHz(240, true, 0.0) == 240);
    CHECK(Pacing::RepeatCadenceHz(0, true, 1000.0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
}

void TestLimiter() {
    const auto start = At(1.0);
    Pacing::FrameLimiter off{0, start};
    CHECK(!off.Enabled() && !off.SlotBefore(start + std::chrono::seconds(1)));

    Pacing::FrameLimiter limiter{120, start};
    const auto period = limiter.Period();
    CHECK(period == std::chrono::nanoseconds(8333333));
    limiter.Release(start + std::chrono::microseconds(100));
    CHECK(limiter.NextSlot() == start + period);
    // A late wake keeps the grid; more than a period late re-anchors without a burst.
    limiter.Release(start + period + std::chrono::milliseconds(3));
    CHECK(limiter.NextSlot() == start + 2 * period);
    const auto late = start + 10 * period;
    limiter.Release(late);
    CHECK(limiter.NextSlot() == late + period);
    // A repeat is only offered before the next vblank.
    CHECK(limiter.SlotBefore(late + period + std::chrono::nanoseconds(1)) == late + period);
    CHECK(!limiter.SlotBefore(late + period));
}

void TestTitle() {
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = true, .fps = 60.0}) == "Alienation | FPS: 60.00");
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = true, .fps = 143.98, .game_fps = 59.99}) ==
          "Alienation | FPS: 143.98 (game 59.99)");
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = false, .fps = 144.0, .game_fps = 60.0}) == "Alienation");
}

// The game's frame stepper (CUSA00062 1.00 0xf1b90, 1.06 0xf48f0, CUSA04430 0xf2750): one fixed step always, more while
// the wall-clock lag is a step or more, at most 1 + max frame skip, none after a step slower than real time; the rest
// of the lag is dropped. The step is 1 / 59.94 (SetFps at startup; engine_set_fps is a no-op in the port).
struct GameStepper {
    double step = 1.0 / 59.94;
    int max_frame_skip = 4;
    double sim_time = 0.0;
    long long steps = 0, dropped = 0;

    int Tick(double now, double step_cost) {
        int run = 0;
        bool stop = false, first = true;
        double lag = 0.0, t = now;
        do {
            if (stop) {
                ++dropped;
            } else {
                t += step_cost;
                stop = !(step_cost <= step && run < max_frame_skip);
                ++run;
            }
            if (first) {
                lag = t - sim_time;
                first = false;
            }
            sim_time += step;
            lag -= step;
        } while (lag >= step);
        steps += run;
        return run;
    }
};

struct Scenario {
    int setting = 60;
    bool vsync = false;
    double display_hz = 0.0;
    double seconds = 10.0;
    double vblank_hz = 60.0;
    double step_cost = 0.0005;
    std::function<double(long long frame)> work = [](long long) { return 0.008; }; // frame start to flip submit
    std::function<double(long long vblank)> jitter = [](long long) { return 0.0; };
    std::function<int(double now)> setting_at{};
};

struct Outcome {
    long long ticks = 0, steps = 0, dropped = 0, max_steps = 0, pump_calls = 0;
    long long flips = 0, shown = 0, repeats = 0, max_presents_per_vblank = 0;
    double lead = 0.0; ///< game time minus the wall time of the game's last frame
    std::vector<int> steps_per_tick;
};

// The port's vblank thread (driver.cpp) with the game behind it: each vblank takes a submitted flip and wakes the game,
// whose next frame starts there; then the repeats due before the next vblank are shown, as the present thread does.
Outcome Simulate(const Scenario& s) {
    Outcome out;
    GameStepper game;
    const double period = 1.0 / s.vblank_hz;
    long long frame = 0;
    double last_frame = 0.0;
    const auto run_frame = [&](double start) {
        last_frame = start;
        const int steps = game.Tick(start, s.step_cost);
        ++out.ticks;
        ++out.pump_calls; // sceNpCheckCallback: once per game frame
        out.max_steps = std::max<long long>(out.max_steps, steps);
        out.steps_per_tick.push_back(steps);
        return start + s.work(frame++);
    };
    double submit = run_frame(0.0);
    int setting = s.setting;
    Pacing::FrameLimiter limiter{Pacing::RepeatCadenceHz(setting, s.vsync, s.display_hz), At(0.0)};
    std::uint64_t consumed = 0;
    for (long long k = 1;; ++k) {
        const double vblank = k * period + s.jitter(k);
        if (vblank >= s.seconds) {
            break;
        }
        if (s.setting_at && s.setting_at(vblank) != setting) {
            setting = s.setting_at(vblank);
            limiter = Pacing::FrameLimiter{Pacing::RepeatCadenceHz(setting, s.vsync, s.display_hz), At(vblank)};
        }
        long long presents = 0;
        if (submit <= vblank) {
            limiter.Release(At(vblank));
            ++out.flips;
            if (Pacing::PresentsFlip(setting, consumed++)) {
                ++out.shown;
                ++presents;
            }
            submit = run_frame(vblank);
        }
        while (const auto slot = limiter.SlotBefore(At(k * period + period))) {
            limiter.Release(*slot);
            ++out.repeats;
            ++presents;
        }
        out.max_presents_per_vblank = std::max(out.max_presents_per_vblank, presents);
    }
    out.steps = game.steps;
    out.dropped = game.dropped;
    out.lead = game.sim_time - last_frame;
    return out;
}

constexpr int kSettings[] = {30, 60, 90, 120, 144, 165, 240, 0};

void TestGameSpeedIsTheSameAtEveryRate() {
    const Outcome base = Simulate({.setting = 60});
    CHECK(base.ticks >= 599 && base.ticks <= 601);
    CHECK(base.steps == base.ticks && base.max_steps == 1);
    CHECK(base.repeats == 0 && base.shown == base.flips);
    for (const int setting : kSettings) {
        for (const auto& [vsync, display] :
             {std::pair{false, 0.0}, std::pair{true, 60.0}, std::pair{true, 144.0}, std::pair{true, 240.0}}) {
            const Outcome run = Simulate({.setting = setting, .vsync = vsync, .display_hz = display});
            CHECK(run.steps == base.steps && run.ticks == base.ticks && run.flips == base.flips);
            CHECK(run.steps_per_tick == base.steps_per_tick);
            CHECK(std::abs(run.lead - base.lead) < 1e-9);
        }
    }
}

void TestPresentationRates() {
    for (const int setting : {90, 120, 144, 165, 240}) {
        const Outcome run = Simulate({.setting = setting});
        const long long presents = run.shown + run.repeats;
        CHECK(run.shown == run.flips);
        CHECK(presents >= setting * 10 - 61 && presents <= setting * 10 + 61);
    }
    const Outcome unlimited = Simulate({.setting = 0});
    CHECK(unlimited.shown + unlimited.repeats <= Pacing::kUnlimitedCadenceHz * 10 + 61);
    CHECK(unlimited.max_presents_per_vblank <= Pacing::kUnlimitedCadenceHz / 60 + 1);
    const Outcome vsync165 = Simulate({.setting = 0, .vsync = true, .display_hz = 165.0});
    CHECK(std::abs(vsync165.shown + vsync165.repeats - 1650) <= 61);
    const Outcome vsync60 = Simulate({.setting = 240, .vsync = true, .display_hz = 60.0});
    CHECK(vsync60.repeats == 0);
}

void TestThirtyKeepsGameSpeed() {
    const Outcome base = Simulate({.setting = 60});
    const Outcome thirty = Simulate({.setting = 30});
    CHECK(thirty.steps == base.steps && thirty.flips == base.flips);
    CHECK(thirty.shown * 2 >= thirty.flips - 1 && thirty.shown * 2 <= thirty.flips + 1);
    CHECK(thirty.repeats == 0);
}

// Online and local co-op run inside the game's frame: the determinism layer steps with the simulation and the NP
// callbacks (Matching2, sessions) are pumped once per frame. Equal wall time gives equal work at every rate.
void TestNetworkWorkFollowsGameFrames() {
    const Outcome base = Simulate({.setting = 60, .seconds = 30.0});
    for (const int setting : kSettings) {
        const Outcome run = Simulate({.setting = setting, .seconds = 30.0});
        CHECK(run.steps == base.steps && run.pump_calls == base.pump_calls);
    }
    CHECK(base.steps >= 1798 && base.steps <= 1801);
}

// A host that cannot reach 60: the game's own catch-up keeps game time on wall time, at every setting.
void TestSlowHostFollowsWallClock() {
    for (const double work : {0.020, 0.030, 0.045}) {
        const Outcome base = Simulate({.setting = 60, .work = [work](long long) { return work; }});
        CHECK(base.ticks < 600);
        CHECK(std::abs(base.lead) < 2.0 / 59.94);
        CHECK(base.max_steps <= 5);
        for (const int setting : kSettings) {
            const Outcome run = Simulate({.setting = setting, .work = [work](long long) { return work; }});
            CHECK(run.steps == base.steps && std::abs(run.lead - base.lead) < 1e-9);
        }
    }
    const Outcome uneven = Simulate({.setting = 144, .work = [](long long f) { return f % 3 ? 0.010 : 0.020; }});
    CHECK(std::abs(uneven.lead) < 2.0 / 59.94);
}

void TestStallIsBounded() {
    const Outcome run = Simulate({.setting = 144, .work = [](long long f) { return f == 300 ? 0.5 : 0.008; }});
    CHECK(run.max_steps == 5);
    CHECK(run.dropped > 20);
    for (std::size_t i = 302; i < run.steps_per_tick.size(); ++i) {
        CHECK(run.steps_per_tick[i] == 1);
    }
    // A step slower than real time stops the catch-up at once: no spiral.
    const Outcome slow_steps = Simulate({.setting = 60, .step_cost = 0.020, .work = [](long long) { return 0.025; }});
    CHECK(slow_steps.max_steps == 1);
}

void TestJitteredVblank() {
    const auto jitter = [](long long k) { return ((k * 7919) % 11 - 5) * 0.0002; };
    const Outcome base = Simulate({.setting = 60, .jitter = jitter});
    CHECK(base.max_steps == 1 && base.steps == base.ticks);
    for (const int setting : kSettings) {
        const Outcome run = Simulate({.setting = setting, .jitter = jitter});
        CHECK(run.steps_per_tick == base.steps_per_tick);
    }
}

void TestRateChangesWhilePlaying() {
    const Outcome base = Simulate({.setting = 60, .seconds = 20.0});
    const auto schedule = [](double now) { return now < 4 ? 60 : now < 8 ? 144 : now < 12 ? 30 : now < 16 ? 0 : 240; };
    const Outcome run = Simulate({.setting = 60, .seconds = 20.0, .setting_at = schedule});
    CHECK(run.steps_per_tick == base.steps_per_tick);
    CHECK(run.max_presents_per_vblank <= Pacing::kUnlimitedCadenceHz / 60 + 1);
}

// Why the vblank and the flips stay at 60 Hz: the game runs at least one step per frame, so frames flipped faster
// than its step make the whole game faster.
void TestFasterGuestFlipsWouldSpeedUpTheGame() {
    const Outcome flips120 = Simulate({.vblank_hz = 120.0, .work = [](long long) { return 0.004; }});
    CHECK(flips120.steps >= 1190 && flips120.max_steps == 1);
    CHECK(flips120.lead > 9.0);
}

// The port's 60 Hz vblank against the game's 59.94 step: game time runs 0.1% ahead of wall time. Pre-existing; the
// Frame rate setting neither causes nor changes it.
void TestVblankLeadIsTheSameAtEveryRate() {
    const Outcome base = Simulate({.setting = 60, .seconds = 600.0});
    CHECK(std::abs(base.lead - 600.0 * (60.0 / 59.94 - 1.0)) < 0.05);
    for (const int setting : {30, 144, 0}) {
        const Outcome run = Simulate({.setting = setting, .seconds = 600.0});
        CHECK(std::abs(run.lead - base.lead) < 1e-9 && run.steps == base.steps);
    }
}

void TestWiring() {
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(InOrder(driver, "PresentsFlip(AlnSettings::Get().frame_rate.load(), flips_consumed++,",
                  "presenter->RetireFrame(frame);"));
    CHECK(InOrder(driver, "limiter.Release(tick_start);", "Flip(request);"));
    CHECK(Contains(driver, "if (vblank_status.count % (main_port.flip_rate + 1) == 0 && !FlipWaitsForPresenter()) {"));
    CHECK(InOrder(driver, "main_port.vblank_cv.notify_all();", "limiter.SlotBefore(tick_deadline)"));
    CHECK(InOrder(driver, "limiter.SlotBefore(tick_deadline)", "RepeatLastFrame();"));
    CHECK(Contains(driver, "RepeatLastFrame();\n        }\n\n        timer.End();"));
    CHECK(InOrder(driver, "if (!presenter->CanRepeatFrames()) {", "return 0u;"));
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h"),
                   "bool CanRepeatFrames() const override { return true; }"));
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_opengl/gl_presenter.h"),
                   "bool CanRepeatFrames() const override { return false; }"));
    // Flip completion (status, events, labels) is signalled whether or not the frame is shown.
    CHECK(InOrder(driver, "presenter->RetireFrame(frame);", "flip_status.count++;"));
    CHECK(InOrder(driver, "flip_status.count++;", "port->SignalVoLabel();"));

    const std::string emulator = ReadSource("gpu/shim/core/emulator_settings.h");
    CHECK(Contains(emulator, "u32 GetVblankFrequency() { return 60; }"));
    const std::string loader = ReadSource("src/loader.c");
    CHECK(!Contains(loader, "AlnFramePacing") && !Contains(loader, "\"frame_rate\""));

    const std::string settings = ReadSource("gpu/shim/aln_settings.cpp");
    CHECK(Contains(settings, "key == \"frame_rate\"") && Contains(settings, "ParseFrameRate(value)"));
    const std::string menu = ReadSource("gpu/shim/aln_options_menu.h");
    CHECK(Contains(menu, "{Id::FrameRate, Page::Gameplay, \"frame_rate\", \"Frame rate\","));
    CHECK(InOrder(menu, "{Id::FrameRate, Page::Gameplay", "Apply::Live, kFrameRates}"));
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(InOrder(presenter, "void Presenter::RetireFrame(Frame* frame) {", "free_queue.push_back(frame);"));
    CHECK(Contains(presenter, "AlnOverlay::LogPresent(is_reusing_frame);"));

    const std::string launcher_settings = ReadSource("launcher/settings.cpp");
    CHECK(Contains(launcher_settings, "{kFrameRateKey, \"60\"},"));
    CHECK(Contains(launcher_settings, "one_of({\"30\", \"60\", \"90\", \"120\", \"144\", \"165\", \"240\", \"0\"})"));
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    CHECK(Contains(launcher, "ui.Combo(\"Frame rate\", kFrameRateHelp, kFrameRateKey, \"60\", frame_rates);"));
}

} // namespace

int main() {
    TestModeTable();
    TestParseFrameRate();
    TestOriginalRateAddsNothing();
    TestThirtyShowsEverySecondFlip();
    TestRepeatCadence();
    TestLimiter();
    TestTitle();
    TestGameSpeedIsTheSameAtEveryRate();
    TestPresentationRates();
    TestThirtyKeepsGameSpeed();
    TestNetworkWorkFollowsGameFrames();
    TestSlowHostFollowsWallClock();
    TestStallIsBounded();
    TestJitteredVblank();
    TestRateChangesWhilePlaying();
    TestFasterGuestFlipsWouldSpeedUpTheGame();
    TestVblankLeadIsTheSameAtEveryRate();
    TestWiring();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("frame pacing tests passed\n");
    return 0;
}
