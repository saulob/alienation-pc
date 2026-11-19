// Focused tests of the Frame rate setting: the mode table, which flips reach the window, the repeat cadence and its
// limiter (aln_frame_pacing.h), the frame counter, title and log line (aln_frame_report.h, aln_title.h), and a model of
// the game's own timing driven through them at every setting.
// No window, no GPU and no game files.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aln_frame_pacing.h"
#include "aln_frame_report.h"
#include "aln_title.h"
#include "stepper_diag.h"

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
    for (const double display : {0.0, 59.94, 60.0, 144.0, 240.0}) {
        CHECK(Pacing::RepeatCadenceHz(60, display) == 0);
        CHECK(Pacing::RepeatCadenceHz(30, display) == 0);
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
    // Unknown refresh: the setting, Unlimited up to 480.
    CHECK(Pacing::RepeatCadenceHz(0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
    CHECK(Pacing::RepeatCadenceHz(240, 0.0) == 240);
    // A known refresh bounds the cadence; a display at the game's rate leaves nothing to repeat.
    CHECK(Pacing::RepeatCadenceHz(120, 60.0) == 0 && Pacing::RepeatCadenceHz(0, 59.94) == 0);
    CHECK(Pacing::RepeatCadenceHz(120, 144.0) == 120);
    CHECK(Pacing::RepeatCadenceHz(240, 143.856) == 144);
    CHECK(Pacing::RepeatCadenceHz(0, 165.0) == 165);
    CHECK(Pacing::RepeatCadenceHz(240, 144.0) == 144);
    CHECK(Pacing::RepeatCadenceHz(0, 1000.0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
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
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = true, .fps = 59.99, .presented_fps = 143.98}) ==
          "Alienation | FPS: 59.99 (presented 143.98)");
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = true, .fps = 59.99, .presented_fps = 59.6}) ==
          "Alienation | FPS: 59.99");
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = false, .fps = 60.0, .presented_fps = 144.0}) ==
          "Alienation");
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
    StepperWindow stepper{}; ///< every frame through the Issue5Diag accounting
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
        const long long dropped_before = game.dropped;
        const int steps = game.Tick(start, s.step_cost);
        stepper_window_add(&out.stepper, steps, static_cast<int>(game.dropped - dropped_before),
                           static_cast<float>(steps * s.step_cost * 1000.0), static_cast<float>(game.step));
        ++out.ticks;
        ++out.pump_calls; // sceNpCheckCallback: once per game frame
        out.max_steps = std::max<long long>(out.max_steps, steps);
        out.steps_per_tick.push_back(steps);
        return start + s.work(frame++);
    };
    double submit = run_frame(0.0);
    int setting = s.setting;
    Pacing::FrameLimiter limiter{Pacing::RepeatCadenceHz(setting, s.display_hz), At(0.0)};
    std::uint64_t consumed = 0;
    for (long long k = 1;; ++k) {
        const double vblank = k * period + s.jitter(k);
        if (vblank >= s.seconds) {
            break;
        }
        if (s.setting_at && s.setting_at(vblank) != setting) {
            setting = s.setting_at(vblank);
            limiter = Pacing::FrameLimiter{Pacing::RepeatCadenceHz(setting, s.display_hz), At(vblank)};
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
        for (const double display : {0.0, 60.0, 119.95, 144.0, 240.0}) {
            const Outcome run = Simulate({.setting = setting, .display_hz = display});
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
    const Outcome display165 = Simulate({.setting = 0, .display_hz = 165.0});
    CHECK(std::abs(display165.shown + display165.repeats - 1650) <= 61);
    const Outcome display60 = Simulate({.setting = 240, .display_hz = 60.0});
    CHECK(display60.repeats == 0);
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
    CHECK(Contains(driver, "unsigned RepeatCadence(int frame_rate, double display_hz) {"));
    CHECK(InOrder(driver, "if (!presenter->CanRepeatFrames()) {",
                  "return AlnFramePacing::RepeatCadenceHz(frame_rate, display_hz);"));
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

// The Steam Deck under Gamescope: VSync off in the settings, but the compositor shows one image per refresh.
struct GamescopeRun {
    long long vblanks = 0, flips = 0, delayed_flips = 0, presents = 0, repeats = 0, dropped_repeats = 0;
    long long max_pending = 0;
};

// The present thread (driver.cpp SwapThread) on Gamescope's Mailbox swapchain, where a present returns at once: new
// frames queue, a repeat only starts on an idle presenter (RunPresenter if_idle), and a due flip waits while a new
// frame is still queued (FlipWaitsForPresenter).
GamescopeRun SimulateGamescope(unsigned cadence, double seconds) {
    GamescopeRun run;
    const double vblank = 1.0 / Pacing::kGameHz;
    const double present_time = 0.001;
    const double dt = 1e-5;
    Pacing::FrameLimiter limiter{cadence, At(0.0)};
    double busy_until = 0.0;
    double frame_ready = 0.008;
    long long pending = 0;
    long long next_vblank = 1;
    for (double t = 0.0; t < seconds; t += dt) {
        if (t >= busy_until && pending > 0) {
            --pending;
            busy_until = t + present_time;
            ++run.presents;
        }
        if (t >= next_vblank * vblank) {
            ++run.vblanks;
            if (t >= frame_ready) {
                if (pending > 0) {
                    ++run.delayed_flips;
                } else {
                    limiter.Release(At(t));
                    ++run.flips;
                    ++pending;
                    frame_ready = t + 0.008;
                }
            }
            ++next_vblank;
        }
        if (const auto slot = limiter.SlotBefore(At(next_vblank * vblank)); slot && At(t) >= *slot) {
            limiter.Release(At(t));
            if (t >= busy_until && pending == 0) {
                busy_until = t + present_time;
                ++run.presents;
                ++run.repeats;
            } else {
                ++run.dropped_repeats;
            }
        }
        run.max_pending = std::max(run.max_pending, pending);
    }
    return run;
}

// ALN-U3T5UQ: Steam Deck, Gamescope, 59.81 Hz, Frame rate 120, VSync off, Mailbox. Gamescope paces presents like VSync,
// so the repeat cadence is bounded by the display: no repeats on a 60 Hz panel, the difference on 90 and 120 Hz ones.
void TestGamescopePresentsFollowTheDisplay() {
    const auto cadence = [](int setting, double display) { return Pacing::RepeatCadenceHz(setting, display); };
    CHECK(cadence(60, 59.81) == 0 && cadence(120, 59.81) == 0 && cadence(0, 59.81) == 0);
    CHECK(cadence(120, 90.0) == 90 && cadence(120, 120.0) == 120);
    // The schedule before the fix: VSync off on a 59.81 Hz Deck still asked for 120 presents a second.
    const unsigned uncapped_120 = Pacing::RepeatCadenceHz(120);
    CHECK(uncapped_120 == 120);

    struct Case {
        int setting;
        double display;
        long long presents_per_second;
    };
    for (const Case c : {Case{60, 59.81, 60}, Case{120, 59.81, 60}, Case{120, 90.0, 90}, Case{120, 120.0, 120}}) {
        const Outcome run = Simulate({.setting = c.setting, .display_hz = c.display});
        const long long presents = run.shown + run.repeats;
        CHECK(std::abs(presents - c.presents_per_second * 10) <= 61);
        CHECK(run.shown == run.flips);
    }

    const GamescopeRun before = SimulateGamescope(uncapped_120, 60.0);
    const GamescopeRun after = SimulateGamescope(cadence(120, 59.81), 60.0);
    std::printf("  Gamescope 59.81 Hz, 60 s: cadence 120: %lld presents (%lld repeats, %lld dropped), %lld flips of "
                "%lld vblanks, %lld delayed; display cadence: %lld presents, %lld flips, %lld delayed\n",
                before.presents, before.repeats, before.dropped_repeats, before.flips, before.vblanks,
                before.delayed_flips, after.presents, after.flips, after.delayed_flips);
    CHECK(before.presents > 59.81 * 60.0 * 1.9);
    CHECK(after.repeats == 0 && after.dropped_repeats == 0);
    CHECK(after.presents == after.flips && after.presents <= 59.81 * 60.0 + 61);
    CHECK(after.flips == before.flips && after.delayed_flips == 0 && after.max_pending <= 1);
    for (const double display : {90.0, 120.0}) {
        const GamescopeRun run = SimulateGamescope(cadence(120, display), 60.0);
        CHECK(std::abs(run.presents - static_cast<long long>(display * 60.0)) <= 61);
        CHECK(run.delayed_flips == 0 && run.max_pending <= 1);
    }
}


// Issue #5: a guest making `fps` frames a second. It waits for each flip, which completes on the 60 Hz vblank, so a
// frame lasts whole vblanks: `extra` of every `fps` frames take one vblank more than the rest.
std::function<double(long long)> WorkForFlipRate(int fps) {
    const int vblanks = Pacing::kGameHz / fps, extra = Pacing::kGameHz - vblanks * fps;
    return [=](long long frame) {
        const int k = static_cast<int>(frame % fps) < extra ? vblanks + 1 : vblanks;
        return (k - 0.5) / Pacing::kGameHz;
    };
}

long long Presents(const Outcome& run) {
    return run.shown + run.repeats;
}

constexpr double kReportDisplayHz = 119.95; // Virtual-sunshine-vmon in the Issue #5 logs

// The player's numbers: a game making about 34 frames a second shows ~17 at 30, 34 at 60, and Unlimited adds repeats
// of the same frames. An overlay counting presents (MangoHud) counts those repeats as frames.
void TestIssue5PlayerNumbers() {
    std::printf("  Issue #5 model, %.2f Hz display, 10 s: presents (new frames + repeats) by game FPS and setting\n",
                kReportDisplayHz);
    for (const int fps : {16, 30, 34, 60}) {
        const Outcome base = Simulate({.setting = 60, .work = WorkForFlipRate(fps)});
        CHECK(std::llabs(base.flips - fps * 10) <= 1);
        std::string row = "    game " + std::to_string(fps) + " FPS:";
        for (const int setting : {30, 60, 90, 120, 0}) {
            const Outcome run = Simulate({.setting = setting, .display_hz = kReportDisplayHz, .work = WorkForFlipRate(fps)});
            CHECK(run.flips == base.flips && run.steps == base.steps && run.steps_per_tick == base.steps_per_tick);
            if (setting == 30) {
                CHECK(std::llabs(run.shown * 2 - run.flips) <= 1);
            } else {
                CHECK(run.shown == run.flips);
            }
            const unsigned cadence = Pacing::RepeatCadenceHz(setting, kReportDisplayHz);
            if (cadence == 0) {
                CHECK(run.repeats == 0);
            } else {
                CHECK(Presents(run) <= cadence * 10 + 61);
            }
            row += "  " + Pacing::ModeName(setting) + " " + std::to_string(run.shown) + "+" + std::to_string(run.repeats);
        }
        std::printf("%s\n", row.c_str());
    }

    const auto work = WorkForFlipRate(34);
    const Outcome at30 = Simulate({.setting = 30, .display_hz = kReportDisplayHz, .work = work});
    const Outcome at60 = Simulate({.setting = 60, .display_hz = kReportDisplayHz, .work = work});
    CHECK(std::llabs(at30.shown - 170) <= 1 && at30.flips == at60.flips && at30.repeats == 0);
    CHECK(std::llabs(at60.shown - 340) <= 1 && at60.repeats == 0);
    // The report's run: VSync off, Mailbox, no Gamescope, so the old rule asked for 480 a second.
    const Outcome before = Simulate({.setting = 0, .work = work});
    const Outcome after = Simulate({.setting = 0, .display_hz = kReportDisplayHz, .work = work});
    CHECK(before.shown == at60.shown && after.shown == at60.shown);
    CHECK(before.flips == at60.flips && after.flips == at60.flips && after.steps == at60.steps);
    CHECK(Presents(before) > 4000 && std::llabs(Presents(after) - 1200) <= 61);
    std::printf("    game 34 FPS, Unlimited: old schedule %lld presents (%lld repeats), display cap %lld (%lld repeats); "
                "unique frames %lld in both\n",
                Presents(before), before.repeats, Presents(after), after.repeats, after.shown);
}

// 30 shows every second flip and takes all of them: 60 completions a second, 30 new frames, one step per flip.
void TestIssue5ThirtyKeepsSixtyFlips() {
    const double one_second = 1.0 + 0.5 / Pacing::kGameHz;
    const Outcome sixty = Simulate({.setting = 60, .seconds = one_second});
    const Outcome thirty = Simulate({.setting = 30, .seconds = one_second});
    CHECK(sixty.flips == 60 && sixty.shown == 60);
    CHECK(thirty.flips == 60 && thirty.shown == 30 && thirty.repeats == 0);
    CHECK(thirty.steps == sixty.steps && thirty.max_steps == 1 && thirty.steps_per_tick == sixty.steps_per_tick);
    CHECK(thirty.ticks == 61);
}

// The counters behind the frame counter and the title: the game's flips first, presents only when they differ.
void TestIssue5CounterSemantics() {
    const auto counters = [](const Outcome& run) {
        return AlnFrameReport::Counters{.guest_flips = std::uint64_t(run.flips),
                                        .presents = std::uint64_t(Presents(run)),
                                        .repeats = std::uint64_t(run.repeats)};
    };
    const auto work = WorkForFlipRate(34);
    const AlnFrameReport::Counters zero{};
    const auto rates = [&](int setting) {
        return AlnFrameReport::RatesBetween(
            zero, counters(Simulate({.setting = setting, .display_hz = kReportDisplayHz, .work = work})), 10.0);
    };
    const AlnFrameReport::Rates at60 = rates(60), at30 = rates(30), unlimited = rates(0);
    CHECK(std::abs(at60.game_fps - 34.0) < 0.2 && std::abs(at30.game_fps - 34.0) < 0.2);
    CHECK(std::abs(unlimited.game_fps - 34.0) < 0.2 && std::abs(unlimited.new_fps - 34.0) < 0.2);
    CHECK(std::abs(unlimited.presented_fps - 120.0) < 6.2 && unlimited.repeat_fps > 80.0);
    CHECK(!AlnFrameReport::PresentationDiffers(at60) && AlnFrameReport::PresentationDiffers(at30));
    CHECK(AlnFrameReport::PresentationDiffers(unlimited));
    CHECK(AlnFrameReport::CounterText(AlnFrameReport::RatesBetween(zero, {34, 34, 0}, 1.0)) == "34 FPS  29.4 ms");
    CHECK(AlnFrameReport::CounterText(AlnFrameReport::RatesBetween(zero, {34, 17, 0}, 1.0)) ==
          "34 FPS  29.4 ms  (17 presented)");

    // What the report's overlay saw: 34 game frames and ~170 presents.
    const AlnFrameReport::Rates mango = AlnFrameReport::RatesBetween(zero, {34, 170, 136}, 1.0);
    CHECK(mango.game_fps == 34.0 && mango.new_fps == 34.0 && mango.repeat_fps == 136.0);
    CHECK(AlnFrameReport::CounterText(mango) == "34 FPS  29.4 ms  (170 presented)");
    CHECK(AlnTitle::Compose({.base = "Alienation", .show_fps = true, .fps = mango.game_fps,
                             .presented_fps = mango.presented_fps}) == "Alienation | FPS: 34.00 (presented 170.00)");
    const AlnFrameReport::Rates thirty = AlnFrameReport::RatesBetween(zero, {60, 30, 0}, 1.0);
    CHECK(AlnFrameReport::CounterText(thirty) == "60 FPS  16.7 ms  (30 presented)");

    const AlnFrameReport::Rates none = AlnFrameReport::RatesBetween({10, 10, 0}, {10, 10, 0}, 1.0);
    CHECK(none.game_fps == 0.0 && AlnFrameReport::CounterText(none) == "0 FPS  0.0 ms");
    const AlnFrameReport::Rates backwards = AlnFrameReport::RatesBetween({10, 10, 5}, {5, 5, 9}, 1.0);
    CHECK(backwards.game_fps == 0.0 && backwards.presented_fps == 0.0 && backwards.repeat_fps == 0.0);
    CHECK(AlnFrameReport::RatesBetween(zero, {34, 34, 0}, 0.0).game_fps == 0.0);
}

// Repeats are capped at a known refresh in every present mode; guest flips and new frames never are.
void TestIssue5DisplayCap() {
    const Outcome base = Simulate({.setting = 60, .seconds = 5.0});
    for (const double display : {60.0, 59.94, 75.0, 90.0, 119.95, 120.0, 143.856, 144.0, 165.0, 240.0, 360.0}) {
        const int refresh = Pacing::NominalHz(display);
        for (const int setting : kSettings) {
            const unsigned cadence = Pacing::RepeatCadenceHz(setting, display);
            const int wanted = Pacing::IsUnlimited(setting) ? refresh : std::min(Pacing::RenderFps(setting), refresh);
            const unsigned expected = Pacing::IsHighRate(setting) && wanted > Pacing::kGameHz ? unsigned(wanted) : 0u;
            CHECK(cadence == expected && cadence <= unsigned(refresh));
            const Outcome run = Simulate({.setting = setting, .display_hz = display, .seconds = 5.0});
            CHECK(run.flips == base.flips && run.steps == base.steps);
            CHECK(run.shown == (setting == 30 ? (base.flips + 1) / 2 : base.flips));
            CHECK(Presents(run) <= std::max<long long>(refresh, Pacing::kGameHz) * 5 + 31);
        }
    }
    CHECK(Pacing::RepeatCadenceHz(0, 240.0) == 240 && Pacing::RepeatCadenceHz(0, 360.0) == 360);
    CHECK(Pacing::RepeatCadenceHz(165, 144.0) == 144 && Pacing::RepeatCadenceHz(144, 165.0) == 144);
    CHECK(Pacing::RepeatCadenceHz(0, 75.0) == 75 && Pacing::RepeatCadenceHz(90, 75.0) == 75);

    // VSync on or off, Gamescope, other Linux sessions and Windows reach the same rule: the driver asks only the
    // presenter and the refresh.
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    const std::size_t at = driver.find("unsigned RepeatCadence(int frame_rate, double display_hz) {");
    const std::string body = at == std::string::npos ? "" : driver.substr(at, driver.find("\n}\n", at) - at);
    CHECK(!body.empty() && !Contains(body, "vsync") && !Contains(body, "Gamescope") && !Contains(body, "_WIN32"));
    CHECK(InOrder(driver, "settings.display_refresh_hz.load() != display_hz", "start_pacing(tick_start);"));
}

// Unlimited on the report's 119.95 Hz display: at most ~120 presentation slots a second, not 480; a 240 Hz display
// still gets ~240; every game frame is shown in both.
void TestIssue5UnlimitedLoad() {
    const auto work = WorkForFlipRate(34);
    const double seconds = 60.0;
    const Outcome old_rule = Simulate({.setting = 0, .seconds = seconds, .work = work});
    const Outcome capped = Simulate({.setting = 0, .display_hz = kReportDisplayHz, .seconds = seconds, .work = work});
    const Outcome high = Simulate({.setting = 0, .display_hz = 240.0, .seconds = seconds, .work = work});
    const Outcome none = Simulate({.setting = 60, .display_hz = kReportDisplayHz, .seconds = seconds, .work = work});
    CHECK(Pacing::RepeatCadenceHz(0) == 480 && Pacing::RepeatCadenceHz(0, kReportDisplayHz) == 120);
    CHECK(std::abs(Presents(capped) / seconds - 120.0) <= 1.1);
    CHECK(std::abs(Presents(high) / seconds - 240.0) <= 1.1);
    CHECK(Presents(old_rule) / seconds > 470.0 && none.repeats == 0);
    for (const Outcome* run : {&old_rule, &capped, &high}) {
        CHECK(run->shown == none.shown && run->flips == none.flips && run->steps == none.steps);
    }
    const double unseen = (Presents(old_rule) - Presents(capped)) / seconds;
    std::printf("  Issue #5 Unlimited, game 34 FPS, presents a second: old rule %.1f, 119.95 Hz cap %.1f, 240 Hz %.1f, "
                "none %.1f; %.1f a second beyond the refresh removed\n",
                Presents(old_rule) / seconds, Presents(capped) / seconds, Presents(high) / seconds,
                Presents(none) / seconds, unseen);
    CHECK(unseen > 340.0);
}

// Slow motion is the game's own rule: it stops catching up after a fixed step slower than real time. A host that is
// only slow to render keeps game time on wall time; a step slower than 1 / 59.94 makes game time one step a frame.
void TestIssue5SlowMotionNeedsASlowStep() {
    const double seconds = 20.0;
    for (const int fps : {30, 34}) {
        const auto work = WorkForFlipRate(fps);
        const Outcome fast_steps = Simulate({.setting = 60, .seconds = seconds, .step_cost = 0.005, .work = work});
        const Outcome slow_steps = Simulate({.setting = 60, .seconds = seconds, .step_cost = 0.017, .work = work});
        const double step = 1.0 / 59.94;
        const double fast_speed = fast_steps.steps * step / seconds, slow_speed = slow_steps.steps * step / seconds;
        CHECK(std::abs(fast_steps.lead) < 2.0 * step && fast_speed > 0.99 && fast_steps.dropped == 0);
        CHECK(slow_steps.steps == slow_steps.flips + 1 && slow_steps.max_steps == 1);
        CHECK(std::abs(slow_speed - fps / 59.94) < 0.01);
        for (const int setting : kSettings) {
            const Outcome run = Simulate({.setting = setting, .display_hz = kReportDisplayHz, .seconds = seconds,
                                          .step_cost = 0.017, .work = work});
            CHECK(run.steps == slow_steps.steps && run.flips == slow_steps.flips);
        }
        std::printf("  Issue #5 game %d FPS: game speed %.0f%% with 5 ms steps, %.0f%% with 17 ms steps\n", fps,
                    fast_speed * 100.0, slow_speed * 100.0);
    }
}

// The default log line: once a minute, not while nothing flips, and not with ALN_FRAME_STATS.
void TestIssue5SummaryLine() {
    const AlnFrameReport::Rates rates = AlnFrameReport::RatesBetween({}, {2040, 7200, 5160}, 60.0);
    const AlnFrameReport::GpuThreadLoad load{.cpu = 91.6, .idle = 3.2, .gpu_wait = 4.9, .recorder = 0.4,
                                             .host_copies = 0.0, .draws_per_frame = 2950.4};
    CHECK(AlnFrameReport::SummaryLine(rates, load) ==
          "Frame rate: game 34.0 FPS; presented 120.0 a second (34.0 new, 86.0 repeats); GPU command thread: CPU 92%, "
          "idle 3%, waiting for the GPU 5%, recorder 0%, host copies 0%; 2950 draws a frame");

    AlnFrameReport::SummaryClock clock{At(0.0)};
    int lines = 0;
    for (long long vblank = 1; vblank <= 600 * Pacing::kGameHz; ++vblank) {
        const auto now = At(double(vblank) / Pacing::kGameHz);
        if (clock.Due(now)) {
            CHECK(std::abs(clock.Seconds(now) - 60.0) < 1e-6);
            clock.Restart(now);
            ++lines;
        }
    }
    CHECK(lines == 10);
    CHECK(!AlnFrameReport::SummaryClock{At(5.0)}.Due(At(64.999)) && AlnFrameReport::SummaryClock{At(5.0)}.Due(At(65.0)));

    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(InOrder(driver, "if (!AlnStats::enabled) {", "PrintFrameRate(seconds);"));
    CHECK(InOrder(driver, "if (rates.game_fps > 0.0) {", "AlnFrameReport::SummaryLine(rates, load)"));
    CHECK(InOrder(driver, "main_port.vblank_cv.notify_all();", "summary.Tick(tick_start);"));
    CHECK(InOrder(driver, "summary.Tick(tick_start);", "limiter.SlotBefore(tick_deadline)"));
}

// The counters are taken where the game's flips and the window's presents happen.
void TestIssue5CounterWiring() {
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(InOrder(driver, "flips_consumed++,", "AlnOverlay::LogGuestFlip();"));
    CHECK(InOrder(driver, "AlnOverlay::LogGuestFlip();", "if (!present) {"));
    const std::string overlay = ReadSource("gpu/shim/aln_overlay.cpp");
    CHECK(Contains(overlay, "AlnFrameReport::CounterText(AlnFrameReport::RatesBetween(from, to, seconds))"));
    CHECK(InOrder(overlay, "bool BeginPresent() {", "UpdateCounterText(now);"));
    CHECK(!Contains(overlay, "frame_ms_avg"));
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Contains(window, "fps = rates.game_fps;") && Contains(window, "presented_fps = rates.presented_fps;"));
    CHECK(Contains(window, ".presented_fps = presented_fps,"));
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp"),
                   "AlnOverlay::LogPresent(is_reusing_frame);"));
}


bool SameStepper(const StepperWindow& a, const StepperWindow& b) {
    return a.frames == b.frames && a.steps == b.steps && a.skipped == b.skipped &&
           a.skipping_frames == b.skipping_frames && a.rejected == b.rejected && a.step_seconds == b.step_seconds;
}

double StepsPerSecond(const Outcome& run, double seconds) {
    return run.stepper.steps / seconds;
}

// Issue #5 Issue5Diag: game speed is the stepper's own steps run of the steps its clock advanced by, whatever the
// frame rate shown.
void TestIssue5StepperGameSpeed() {
    const double seconds = 20.0;
    const Outcome full = Simulate({.setting = 60, .seconds = seconds});
    CHECK(std::abs(stepper_window_game_speed(&full.stepper) - 100.0) < 1e-9 && full.stepper.skipped == 0);
    CHECK(std::abs(StepsPerSecond(full, seconds) - 60.0) < 0.2 && full.stepper.rejected == 0);
    CHECK(std::abs(1.0 / full.stepper.step_seconds - 59.94) < 1e-3);

    const auto slow = [&](int fps, int setting, double display) {
        return Simulate({.setting = setting, .display_hz = display, .seconds = seconds, .step_cost = 0.017,
                         .work = WorkForFlipRate(fps)});
    };
    const Outcome thirty = slow(30, 60, 0.0), thirty_four = slow(34, 60, 0.0);
    CHECK(std::abs(stepper_window_game_speed(&thirty.stepper) - 50.0) < 0.2);
    CHECK(std::abs(stepper_window_game_speed(&thirty_four.stepper) - 100.0 * 34 / 60) < 0.2);
    CHECK(std::abs(StepsPerSecond(thirty_four, seconds) - 34.0) < 0.1);
    CHECK(thirty_four.stepper.skipping_frames > 0 && thirty_four.stepper.steps == thirty_four.stepper.frames);

    // Only a step slower than the step itself drops lag: the same 34 frames with fast steps catch up in full.
    const Outcome caught_up = Simulate({.setting = 60, .seconds = seconds, .step_cost = 0.005, .work = WorkForFlipRate(34)});
    CHECK(caught_up.stepper.skipped == 0 && caught_up.stepper.skipping_frames == 0);
    CHECK(std::abs(stepper_window_game_speed(&caught_up.stepper) - 100.0) < 1e-9);
    CHECK(caught_up.stepper.steps > thirty_four.stepper.steps && caught_up.stepper.frames == thirty_four.stepper.frames);

    // Presentation never enters the accounting: every setting and display, Unlimited's 480 repeats included.
    const Outcome unlimited60 = Simulate({.setting = 0, .seconds = seconds});
    CHECK(unlimited60.repeats > 8000 && SameStepper(unlimited60.stepper, full.stepper));
    for (const int setting : kSettings) {
        for (const double display : {0.0, 60.0, 119.95, 240.0}) {
            CHECK(SameStepper(Simulate({.setting = setting, .display_hz = display, .seconds = seconds}).stepper,
                              full.stepper));
            CHECK(SameStepper(slow(34, setting, display).stepper, thirty_four.stepper));
        }
    }
    const auto schedule = [](double now) { return now < 4 ? 60 : now < 8 ? 144 : now < 12 ? 30 : now < 16 ? 0 : 240; };
    const Outcome switching = Simulate({.setting = 60, .seconds = seconds, .step_cost = 0.017,
                                        .work = WorkForFlipRate(34), .setting_at = schedule});
    CHECK(SameStepper(switching.stepper, thirty_four.stepper));

    char line[320];
    CHECK(stepper_window_format(&thirty_four.stepper, seconds, line, sizeof(line)));
    std::printf("  %s\n", line);
    CHECK(stepper_window_format(&unlimited60.stepper, seconds, line, sizeof(line)));
    std::printf("  60 FPS, Unlimited: %s\n", line);
}

// The live window is reported on the minute summary's clock: nothing before a minute, ten lines in ten minutes.
void TestIssue5StepperReportRate() {
    stepper_diag_take();
    AlnFrameReport::SummaryClock clock{At(0.0)};
    int lines = 0;
    double first = -1.0;
    char line[320];
    for (long long vblank = 1; vblank <= 600 * Pacing::kGameHz; ++vblank) {
        const auto now = At(double(vblank) / Pacing::kGameHz);
        stepper_diag_record(1, vblank % 2 ? 1 : 0, 25.0f, static_cast<float>(1.0 / 59.94));
        if (clock.Due(now)) {
            if (stepper_diag_report(clock.Seconds(now), line, sizeof(line))) {
                ++lines;
                if (first < 0.0) {
                    first = double(vblank) / Pacing::kGameHz;
                }
            }
            clock.Restart(now);
        }
    }
    CHECK(lines == 10 && std::abs(first - 60.0) < 1e-9);
    CHECK(Contains(line, "game speed 66.7%"));

    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(InOrder(driver, "if (!clock_.Due(now)) {", "PrintFrameRate(seconds);"));
    CHECK(InOrder(driver, "PrintFrameRate(seconds);", "stepper_diag_report(seconds, stepper, sizeof(stepper))"));
    CHECK(InOrder(driver, "stepper_diag_report(seconds, stepper, sizeof(stepper))", "Restart(now);"));
    const std::string loader = ReadSource("src/loader.c");
    CHECK(InOrder(loader, "const size_t stepper_bytes = known_build ? stepper_diag_stub_bytes() : 0;",
                  "stepper_diag_install(image,size,build_key,game_build.label,"));
    CHECK(Contains(loader, "puts(\"Issue5Diag: stepper unavailable for an unknown game build: not a supported build\");"));
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
    TestGamescopePresentsFollowTheDisplay();
    TestIssue5PlayerNumbers();
    TestIssue5ThirtyKeepsSixtyFlips();
    TestIssue5CounterSemantics();
    TestIssue5DisplayCap();
    TestIssue5UnlimitedLoad();
    TestIssue5SlowMotionNeedsASlowStep();
    TestIssue5SummaryLine();
    TestIssue5CounterWiring();
    TestIssue5StepperGameSpeed();
    TestIssue5StepperReportRate();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("frame pacing tests passed\n");
    return 0;
}
