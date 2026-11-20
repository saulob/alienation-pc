// Focused tests of the launcher's renderer choice and frame presentation: renderer order, module pinning, teardown order,
// the frame cap, the loop monitor's telemetry and the source contracts of the launcher's loops. No window, no renderer.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "frame_pacer.h"
#include "launcher_renderer.h"

namespace fs = std::filesystem;
using namespace aln;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t start = text.find(from);
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = text.find(to, start + from.size());
    return end == std::string::npos ? std::string() : text.substr(start, end - start);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// Windows asks for Direct3D 11 first and only with its modules pinned; software is the fallback; Linux keeps SDL's choice.
void TestRendererPolicy() {
    const std::vector<std::string_view> windows = LauncherRendererOrder(true);
    CHECK(windows.size() == 2 && windows[0] == "direct3d11" && windows[1] == "software");
    const std::vector<std::string_view> other = LauncherRendererOrder(false);
    CHECK(other.size() == 2 && other[0].empty() && other[1] == "software");
    for (const std::string_view name : windows) {
        CHECK(!name.empty());
        CHECK(name == "software" || !RendererModulesToPin(name).empty());
    }
    const std::vector<std::string_view> pins = RendererModulesToPin("direct3d11");
    CHECK(pins.size() == 2 && pins[0] == "d3d11.dll" && pins[1] == "dxgi.dll");
    CHECK(RendererModulesToPin("software").empty() && RendererModulesToPin("").empty() &&
          RendererModulesToPin("opengl").empty() && RendererModulesToPin("vulkan").empty() &&
          RendererModulesToPin("direct3d12").empty());
}

void TestHostSource(const fs::path& source) {
    const std::string host = ReadAll(source / "launcher" / "host.cpp");
    const std::string create = Between(host, "SDL_Renderer* CreateLauncherRenderer(", "} // namespace");
    CHECK(!create.empty());
    CHECK(create.find("LauncherRendererOrder(kWindows)") != std::string::npos);
    CHECK(create.find("SDL_CreateRenderer(window, nullptr)") == std::string::npos);
    const size_t pin = create.find("PinRendererModules(name, pinned)");
    const size_t skip = create.find("continue;", pin);
    const size_t made = create.find("SDL_CreateRenderer(window,", pin);
    CHECK(pin != std::string::npos && skip != std::string::npos && made != std::string::npos && pin < skip && skip < made);
    CHECK(create.find("fallback = i > 0;") != std::string::npos);
    CHECK(Count(host, "SDL_CreateRenderer(") == 1);
    CHECK(host.find("SDL_HINT_RENDER_DRIVER") == std::string::npos);

    const std::string pin_module = Between(host, "bool PinModule(std::string_view name) {", "int64_t ThreadCpuNs()");
    CHECK(pin_module.find("LoadLibraryW(") != std::string::npos);
    CHECK(pin_module.find("GET_MODULE_HANDLE_EX_FLAG_PIN") != std::string::npos);
    CHECK(host.find("FreeLibrary") == std::string::npos && host.find("SDL_UnloadObject") == std::string::npos);
    CHECK(host.find("Sleep(") == std::string::npos);
    const std::string pin_all = Between(host, "bool PinRendererModules(", "SDL_Renderer* CreateLauncherRenderer(");
    CHECK(pin_all.find("RendererModulesToPin(renderer)") != std::string::npos && pin_all.find("return false;") != std::string::npos);

    CHECK(host.find("RendererSelectedLine(renderer_name_, fallback, pinned)") != std::string::npos);
    CHECK(host.find("Launcher: window %dx%d at %.2fx display scale, %s renderer") != std::string::npos);
    const std::string open = Between(host, "bool LauncherHost::Open(", "void LauncherHost::ConfigureVsync()");
    const size_t selected = open.find("RendererSelectedLine(");
    const size_t vsync = open.find("ConfigureVsync();");
    const size_t imgui = open.find("ImGui_ImplSDLRenderer3_Init(renderer_);");
    CHECK(selected != std::string::npos && selected < vsync && vsync < imgui);
    CHECK(open.find("CreateLauncherRenderer(window_, fallback, pinned)") != std::string::npos);

    // Textures, ImGui, renderer, window, SDL: the pinned modules are never part of the teardown.
    const std::string close = Between(host, "void LauncherHost::Close() {", "bool LauncherHost::PumpEvents()");
    const size_t textures = close.find("SDL_DestroyTexture(texture);");
    const size_t shutdown = close.find("ImGui::DestroyContext(imgui_);");
    const size_t renderer = close.find("SDL_DestroyRenderer(renderer_);");
    const size_t window = close.find("SDL_DestroyWindow(window_);");
    const size_t quit = close.find("SDL_Quit();");
    CHECK(textures != std::string::npos && textures < shutdown && shutdown < renderer && renderer < window && window < quit &&
          quit != std::string::npos);
}

// The launcher closes before the game starts in this process, and a separate game process is started only by Frame.
void TestLifecycle(const fs::path& source) {
    const std::string app = ReadAll(source / "launcher" / "launcher.cpp");
    const std::string run = Between(app, "bool Run(Outcome& outcome) {", "Outcome Frame() {");
    const size_t loop_end = run.find("aln_startup_phase(\"launcher loop ended\");");
    const size_t closed = run.find("host_.Close();");
    CHECK(loop_end != std::string::npos && closed != std::string::npos && loop_end < closed);
    CHECK(run.find("return true;", closed) != std::string::npos);
    CHECK(run.find("StartGameProcess") == std::string::npos);

    const std::string entry = Between(app, "extern \"C\" AlnLauncherResult aln_launcher_run(", "\n}\n");
    const size_t ran = entry.find("app.Run(outcome)");
    const size_t started = entry.find("app.game_process_started()");
    CHECK(ran != std::string::npos && started != std::string::npos && ran < started);
    const size_t result = entry.find("ALN_LAUNCHER_PLAY : ALN_LAUNCHER_QUIT");
    CHECK(result != std::string::npos && started < result);
    CHECK(Count(app, "host_.Close()") == 1);
    CHECK(Count(app, "StartGameProcess(config_, error)") == 1 && Count(app, "StartGameProcess(config, error)") == 1);

    const std::string explain = Between(app, "AlnLauncherResult ExplainInWindow(", "AlnLauncherResult PlayOrExplain(");
    CHECK(explain.find("LauncherHost host;") != std::string::npos && explain.find("host.Open(options)") != std::string::npos);
    CHECK(explain.find("SDL_CreateRenderer") == std::string::npos && explain.find("host.WaitForFrame()") != std::string::npos);
    CHECK(explain.find("PumpEvents") == std::string::npos && explain.find("SDL_Delay(") == std::string::npos);
    const std::string play = Between(app, "AlnLauncherResult PlayOrExplain(", "} // namespace\n} // namespace aln");
    CHECK(play.find("LauncherHost") == std::string::npos && play.find("StartGameProcess(config, error)") != std::string::npos);

    // The game's renderer is Vulkan: the launcher's choice never reaches the video core.
    const std::string shim = ReadAll(source / "gpu" / "shim" / "window.cpp");
    CHECK(!shim.empty());
    CHECK(shim.find("SDL_SOFTWARE_RENDERER") == std::string::npos && shim.find("direct3d11") == std::string::npos &&
          shim.find("SDL_CreateRenderer") == std::string::npos);
}

// ---- Frame presentation ----

struct LoopRun {
    int frames = 0;
    int64_t slept_ns = 0;
};

// The launcher's visible loop on a virtual clock: the frame cap's waits, a render of render_ns and a present that blocks
// until the next VSync of sdl_vsync_ns.
LoopRun SimulateVisibleLoop(int64_t interval_ns, int64_t render_ns, int64_t sdl_vsync_ns, int64_t oversleep_ns,
                            int64_t duration_ns) {
    FramePacer pacer(interval_ns);
    LoopRun run;
    int64_t now = 0, last_present = 0;
    while (now < duration_ns) {
        const FrameWait next = pacer.Next(now);
        if (!next.draw) {
            now += next.wait_ns + oversleep_ns;
            run.slept_ns += next.wait_ns + oversleep_ns;
            continue;
        }
        now += render_ns;
        if (sdl_vsync_ns > 0 && now < last_present + sdl_vsync_ns) {
            run.slept_ns += last_present + sdl_vsync_ns - now;
            now = last_present + sdl_vsync_ns;
        }
        last_present = now;
        ++run.frames;
    }
    return run;
}

bool TelemetryTokensOnly(const std::string& line) {
    const std::string prefix = "Launcher telemetry: ";
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    for (size_t i = prefix.size(); i < line.size(); ++i) {
        const char c = line[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '=' || c == '.' || c == ' ')) {
            return false;
        }
    }
    return true;
}

struct MonitorRun {
    int samples = 0, first = 0, rate = 0, stall = 0;
    std::optional<LoopSample> last;
};

// Frames every period_ns with the given times; the UI thread's CPU clock runs at a quarter of the wall clock.
MonitorRun FeedMonitor(int64_t period_ns, const FrameTimes& times, int64_t seconds, int64_t wait_ns = 0) {
    LoopMonitor monitor;
    MonitorRun run;
    for (int64_t t = 0; t <= seconds * kNsPerSecond; t += period_ns) {
        monitor.Waited(wait_ns);
        if (std::optional<LoopSample> sample = monitor.FrameDrawn(t, times, t / 4)) {
            ++run.samples;
            run.first += sample->first ? 1 : 0;
            run.rate += sample->excessive_rate ? 1 : 0;
            run.stall += sample->present_stall ? 1 : 0;
            run.last = sample;
        }
    }
    return run;
}

void TestFrameCap() {
    const int64_t interval = kLauncherFrameNs;

    // The 60 Hz cap does not follow the monitor: a 240 Hz VSync does not rebuild the UI 240 times a second.
    CHECK(kLauncherTargetHz == 60 && interval == 16'666'666 && FramePacer().interval_ns() == interval);
    for (const int64_t vsync : {int64_t(16'666'666), int64_t(6'944'444), int64_t(4'166'666), int64_t(0)}) {
        const LoopRun run = SimulateVisibleLoop(interval, 2 * kNsPerMs, vsync, 300'000, 10 * kNsPerSecond);
        CHECK(run.frames >= 590 && run.frames <= 601);
    }
    const LoopRun uncapped = SimulateVisibleLoop(0, 2 * kNsPerMs, 4'166'666, 0, 10 * kNsPerSecond);
    CHECK(uncapped.frames >= 2390);

    // A wait is never longer than one frame and never zero, and a stall is one frame, not a burst.
    FramePacer sweep;
    sweep.Next(0);
    int64_t worst = 0;
    bool spun = false;
    for (int64_t t = 0; t < 4 * interval; t += 37'000) {
        const FrameWait next = sweep.Next(t);
        spun = spun || (!next.draw && next.wait_ns <= 0);
        worst = next.draw ? worst : std::max(worst, next.wait_ns);
    }
    CHECK(!spun && worst <= interval);
    FramePacer stall;
    stall.Next(0);
    CHECK(stall.Next(10 * kNsPerSecond).draw && !stall.Next(10 * kNsPerSecond + 1).draw);
    FramePacer late;
    late.Next(0);
    CHECK(late.Next(interval + 5 * kNsPerMs).draw && late.Next(2 * interval).draw);
    FramePacer backwards;
    backwards.Next(10 * kNsPerSecond);
    CHECK(backwards.Next(0).wait_ns <= interval);
    FramePacer hidden(0);
    bool always = true;
    for (int i = 0; i < 1000; ++i) {
        always = always && hidden.Next(i).draw;
    }
    CHECK(always);
    CHECK(kMinimizedSleepMs >= 30);
}

void TestMonitor() {
    const int64_t interval = kLauncherFrameNs;

    // The field run's pathological software present: 4 frames a second, about 245 ms each inside SDL_RenderPresent.
    const FrameTimes field{4 * kNsPerMs, 2 * kNsPerMs, 245 * kNsPerMs};
    const MonitorRun slow = FeedMonitor(251 * kNsPerMs, field, 30);
    CHECK(slow.samples >= 4 && slow.first == 1 && slow.stall == 1 && slow.rate == 0);
    CHECK(slow.last && slow.last->max_present_ns == 245 * kNsPerMs && slow.last->slow_presents == slow.last->frames);

    // A normal accelerated frame at 60 Hz: no anomaly, and the UI thread's CPU share is measured, not inferred.
    const FrameTimes normal{2 * kNsPerMs, kNsPerMs / 2, kNsPerMs};
    const MonitorRun steady = FeedMonitor(interval, normal, 30, interval - 3'500'000);
    CHECK(steady.samples >= 4 && steady.first == 1 && steady.stall == 0 && steady.rate == 0);
    CHECK(steady.last && steady.last->hz > 59.0 && steady.last->hz < 61.0);
    CHECK(steady.last && steady.last->cpu_pct >= 24 && steady.last->cpu_pct <= 25);

    // A VSync-blocked present at 30 Hz is not a stall, nor are occasional slow presents.
    const MonitorRun vsync30 = FeedMonitor(33'333'333, {2 * kNsPerMs, kNsPerMs, 30 * kNsPerMs}, 30);
    CHECK(vsync30.samples >= 4 && vsync30.stall == 0);
    LoopMonitor mixed;
    int mixed_stalls = 0, mixed_samples = 0;
    int frame = 0;
    for (int64_t t = 0; t <= 30 * kNsPerSecond; t += interval, ++frame) {
        const FrameTimes times{kNsPerMs, kNsPerMs, frame % 10 == 0 ? 120 * kNsPerMs : kNsPerMs};
        if (std::optional<LoopSample> sample = mixed.FrameDrawn(t, times)) {
            ++mixed_samples;
            mixed_stalls += sample->present_stall ? 1 : 0;
            CHECK(sample->cpu_pct == -1);
        }
    }
    CHECK(mixed_samples >= 4 && mixed_stalls == 0);
    // An uncapped loop still reports the excessive rate, once.
    const MonitorRun fast = FeedMonitor(4'166'666, normal, 30);
    CHECK(fast.rate == 1 && fast.stall == 0);
    // Nothing before the warm-up, and minimizing starts it again.
    LoopMonitor warmup;
    bool early = false;
    for (int64_t t = 0; t < LoopMonitor::kWarmupNs; t += 4'000'000) {
        early = early || warmup.FrameDrawn(t, normal).has_value();
    }
    CHECK(!early);
    warmup.Reset();
    CHECK(!warmup.FrameDrawn(100 * kNsPerSecond, normal).has_value());
}

void TestTelemetryLines() {
    LoopSample sample;
    sample.frames = 20;
    sample.elapsed_ns = 5 * kNsPerSecond;
    sample.hz = 4.0;
    sample.total = {80 * kNsPerMs, 40 * kNsPerMs, 4900 * kNsPerMs};
    sample.max_present_ns = 300 * kNsPerMs;
    sample.slow_presents = 20;
    const std::string sample_line = LoopSampleLine("software", sample);
    CHECK(sample_line ==
          "Launcher telemetry: loop_sample renderer=software frames=20 elapsed_ms=5000 effective_hz=4.0 avg_build_ms=4.0 "
          "avg_submit_ms=2.0 avg_present_ms=245.0 avg_wait_ms=0.0 present_pct=98 wait_pct=0");
    CHECK(sample_line.find("busy") == std::string::npos && sample_line.find("cpu") == std::string::npos);
    sample.cpu_pct = 12;
    CHECK(LoopSampleLine("direct3d11", sample).find(" ui_thread_cpu_pct=12") != std::string::npos);
    const std::string stall_line = PresentStallLine("software", sample);
    CHECK(stall_line.rfind("Launcher telemetry: event=launcher_present_stall renderer=software frames=20", 0) == 0);
    CHECK(stall_line.find("max_present_ms=300.0 slow_presents=20 threshold_ms=50") != std::string::npos);
    CHECK(ExcessiveRateLine("software", sample).find("event=launcher_frame_pacing_excessive target_hz=60") != std::string::npos);
    CHECK(PacingModeLine("direct3d11", {true, true}) ==
          "Launcher telemetry: frame_pacing renderer=direct3d11 requested_vsync=1 effective_vsync=1 pacing=vsync target_hz=60");
    CHECK(PacingModeLine("software", {true, false}) ==
          "Launcher telemetry: frame_pacing renderer=software requested_vsync=1 effective_vsync=0 pacing=fallback target_hz=60");
    CHECK(RendererSelectedLine("direct3d11", false, 2) ==
          "Launcher telemetry: renderer_selected renderer=direct3d11 fallback=0 pinned_modules=2");
    CHECK(RendererSelectedLine("software", true, 2) ==
          "Launcher telemetry: renderer_selected renderer=software fallback=1 pinned_modules=2");

    // Only tokens: no user input, path or free text reaches a telemetry line.
    const std::string hostile = "Direct3D C:\\Users\\someone\\game /home/x";
    CHECK(RendererLabel(hostile).find_first_of("\\/: ") == std::string::npos && RendererLabel("") == "unknown");
    for (const std::string& line : {sample_line, stall_line, ExcessiveRateLine(hostile, sample),
                                    PacingModeLine(hostile, {true, false}), RendererSelectedLine(hostile, true, 0),
                                    LoopSampleLine(hostile, sample)}) {
        CHECK(TelemetryTokensOnly(line));
    }
}

// The real loops: events before every frame, waits only between frames, one timed present per frame.
void TestLoopSource(const fs::path& source) {
    const std::string host = ReadAll(source / "launcher" / "host.cpp");
    const std::string wait = Between(host, "bool LauncherHost::WaitForFrame() {", "void LauncherHost::NoteFrameDrawn(");
    const size_t pump = wait.find("PumpEvents()");
    const size_t ready = wait.find("return true;");
    CHECK(pump != std::string::npos && ready != std::string::npos && pump < ready && ready < wait.find("SDL_DelayNS("));
    CHECK(wait.find("SDL_Delay(kMinimizedSleepMs)") != std::string::npos && wait.find("monitor_.Waited(") != std::string::npos);
    CHECK(wait.find("pacer_.Reset();") != std::string::npos && wait.find("monitor_.Reset();") != std::string::npos);
    CHECK(wait.find("SDL_RenderPresent") == std::string::npos && wait.find("BeginFrame") == std::string::npos);
    CHECK(Count(host, "SDL_RenderPresent(renderer_)") == 1);
    const std::string end_frame = Between(host, "void LauncherHost::EndFrame(", "bool LauncherHost::Fullscreen()");
    const size_t presenting = end_frame.find("const int64_t presenting = NowNs();");
    const size_t present = end_frame.find("SDL_RenderPresent(renderer_);");
    const size_t presented = end_frame.find("const int64_t presented = NowNs();");
    CHECK(presenting != std::string::npos && presenting < present && present < presented && presented != std::string::npos);
    CHECK(end_frame.find("presented - presenting") != std::string::npos);
    CHECK(host.find("pacer_ = FramePacer(hidden_ ? 0 : kLauncherFrameNs);") != std::string::npos);
    CHECK(host.find("if (pacer_.interval_ns() > 0) {") != std::string::npos);
    const std::string vsync = Between(host, "void LauncherHost::ConfigureVsync() {", "void LauncherHost::Show() {");
    CHECK(vsync.find("SDL_SetRenderVSync(renderer_, 1)") != std::string::npos &&
          vsync.find("SDL_SetRenderVSync(renderer_, 0)") != std::string::npos);
    CHECK(vsync.find("SDL_GetWindowSurfaceVSync(") != std::string::npos);

    const std::string header = ReadAll(source / "launcher" / "host.h");
    const size_t publics = header.find("public:");
    const size_t privates = header.find("private:");
    CHECK(publics != std::string::npos && privates != std::string::npos);
    CHECK(header.find("bool WaitForFrame();") < privates);
    CHECK(header.find("bool PumpEvents();") > privates && header.find("bool Minimized() const;") > privates);

    const std::string app = ReadAll(source / "launcher" / "launcher.cpp");
    const std::string loop = Between(app, "while (outcome == Outcome::kRunning) {", "aln_startup_phase(\"launcher loop ended\");");
    const size_t waited = loop.find("host_.WaitForFrame()");
    CHECK(waited != std::string::npos && waited < loop.find("host_.BeginFrame();"));
    CHECK(app.find("PumpEvents") == std::string::npos && app.find("SDL_Delay(") == std::string::npos);
    CHECK(app.find("Minimized()") == std::string::npos);
}

void TestPortableHeaders(const fs::path& source) {
    for (const char* header : {"frame_pacer.h", "launcher_renderer.h"}) {
        const std::string text = ReadAll(source / "launcher" / header);
        CHECK(!text.empty());
        for (const char* token : {"windows.h", "_WIN32", "SDL_", "<thread>"}) {
            CHECK(text.find(token) == std::string::npos);
        }
    }
}

} // namespace

int main() {
    const fs::path source(ALN_SOURCE_DIR);
    TestRendererPolicy();
    TestHostSource(source);
    TestLifecycle(source);
    TestFrameCap();
    TestMonitor();
    TestTelemetryLines();
    TestLoopSource(source);
    TestPortableHeaders(source);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
