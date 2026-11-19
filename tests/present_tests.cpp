// Focused tests of the swapchain image count, the present mode choice, Gamescope detection, the Gamescope-only flip
// pacing, the latency trace switch and the swapchain health rules (suboptimal acquire and present, recreation).
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "aln_latency_trace.h"
#include "aln_present.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using AlnPresent::ImageCount;
using AlnPresent::Mode;

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

bool InOrder(const std::string& text, const char* first, const char* second) {
    const std::size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a) != std::string::npos;
}

void TestDefaultImageCount() {
    CHECK(ImageCount(2, 0, false) == 3);
    CHECK(ImageCount(3, 0, false) == 4); // the reporter's Gamescope surface, before
    CHECK(ImageCount(3, 8, false) == 4);
    CHECK(ImageCount(3, 3, false) == 3);
    CHECK(ImageCount(1, 0, false) == 2);
}

void TestGamescopeImageCount() {
    CHECK(ImageCount(3, 0, true) == 3);
    CHECK(ImageCount(4, 0, true) == 4);
    CHECK(ImageCount(2, 8, true) == 2);
    CHECK(ImageCount(1, 0, true) == 2);
    CHECK(ImageCount(1, 1, true) == 1);
    for (unsigned min = 1; min <= 8; ++min) {
        for (unsigned max = 0; max <= 8; ++max) {
            if (max && max < min) {
                continue;
            }
            for (const bool gamescope : {false, true}) {
                const unsigned count = ImageCount(min, max, gamescope);
                CHECK(count >= min);
                CHECK(!max || count <= max);
                CHECK(count <= ImageCount(min, max, false));
            }
        }
    }
}

void TestPresentMode() {
    for (const bool mailbox : {false, true}) {
        for (const bool immediate : {false, true}) {
            CHECK(AlnPresent::ChooseMode(true, mailbox, immediate) == Mode::Fifo);
        }
    }
    CHECK(AlnPresent::ChooseMode(false, true, true) == Mode::Mailbox);
    CHECK(AlnPresent::ChooseMode(false, true, false) == Mode::Mailbox);
    CHECK(AlnPresent::ChooseMode(false, false, true) == Mode::Immediate);
    CHECK(AlnPresent::ChooseMode(false, false, false) == Mode::Fifo);
    CHECK(int(Mode::Immediate) == 0 && int(Mode::Mailbox) == 1 && int(Mode::Fifo) == 2);
}

void TestGamescopeDetection() {
    CHECK(AlnPresent::IsGamescope("gamescope-0", false));
    CHECK(!AlnPresent::IsGamescope("gamescope-0", true));
    CHECK(!AlnPresent::IsGamescope("", false));
    CHECK(!AlnPresent::IsGamescope(nullptr, false));
    if (AlnPresent::kWindows) {
        CHECK(!AlnPresent::GamescopeSession());
    }
}

void TestLatencyTraceSwitch() {
    CHECK(!AlnLatencyTrace::Requested(nullptr));
    CHECK(!AlnLatencyTrace::Requested(""));
    CHECK(!AlnLatencyTrace::Requested("0"));
    CHECK(AlnLatencyTrace::Requested("1"));
}

const char* FakeEnv(const char* name) {
    static const struct {
        const char* name;
        const char* value;
    } env[] = {
        {"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"},
        {"XDG_CURRENT_DESKTOP", "gamescope"},
        {"SteamDeck", "1"},
        {"GAMESCOPE_LIMITER_FILE", "/tmp/gamescope-limiter.123"},
        {"HOME", "/home/deck"},
        {"USER", "deck"},
        {"MESA_VK_WSI_PRESENT_MODE", "fifo"},
    };
    for (const auto& e : env) {
        if (!std::strcmp(e.name, name)) {
            return e.value;
        }
    }
    return nullptr;
}

void TestSessionDescription() {
    const std::string session = AlnPresent::DescribeSession(true, FakeEnv);
    CHECK(session.rfind("Gamescope yes", 0) == 0);
    CHECK(Contains(session, "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0"));
    CHECK(Contains(session, "SteamDeck=1"));
    CHECK(Contains(session, "MESA_VK_WSI_PRESENT_MODE=fifo"));
    CHECK(Contains(session, "GAMESCOPE_LIMITER_FILE=(set)"));
    CHECK(session.find('/') == std::string::npos);
    CHECK(!Contains(session, "HOME") && !Contains(session, "USER") && !Contains(session, "deck"));
    CHECK(AlnPresent::DescribeSession(false, [](const char*) -> const char* { return nullptr; }) == "Gamescope no");
}

struct Delay {
    int worst_ticks = 0; ///< flip submit -> present
    int presents = 0;
};

// A model of the flip path in vblank ticks: the guest flips once a display buffer is free (its flip before last
// completed) and a presenter frame is free; the vblank thread completes a flip by moving it to the swap queue, and
// with Gamescope pacing only while that queue is empty; the swap thread presents one frame every `present_ticks`.
Delay FlipDelay(bool gamescope, int frames, int present_ticks, int display_buffers) {
    std::deque<int> requests, swap_queue;
    int free_frames = frames, pending_flips = 0, busy_until = -1, presenting = -1;
    Delay delay;
    for (int t = 0; t < 600; ++t) {
        if (presenting >= 0 && t >= busy_until) {
            if (t > 300) {
                delay.worst_ticks = std::max(delay.worst_ticks, t - presenting);
                ++delay.presents;
            }
            ++free_frames;
            presenting = -1;
        }
        if (presenting < 0 && !swap_queue.empty()) {
            presenting = swap_queue.front();
            swap_queue.pop_front();
            busy_until = t + present_ticks;
        }
        if (!(gamescope && !swap_queue.empty()) && !requests.empty()) {
            swap_queue.push_back(requests.front());
            requests.pop_front();
            --pending_flips;
        }
        if (pending_flips < display_buffers - 1 && free_frames > 0) {
            --free_frames;
            ++pending_flips;
            requests.push_back(t);
        }
    }
    return delay;
}

void TestFlipPacingModel() {
    for (const int buffers : {2, 3}) {
        for (const int frames : {3, 4}) {
            const Delay fast_old = FlipDelay(false, frames, 1, buffers), fast_new = FlipDelay(true, frames, 1, buffers);
            CHECK(fast_new.worst_ticks == fast_old.worst_ticks && fast_new.presents == fast_old.presents);
            const Delay slow_old = FlipDelay(false, frames, 2, buffers), slow_new = FlipDelay(true, frames, 2, buffers);
            CHECK(slow_new.worst_ticks <= slow_old.worst_ticks && slow_new.presents == slow_old.presents);
        }
    }
    // The reporter's case: 4 swapchain images (presenter frames) before, 3 with Gamescope pacing after.
    const Delay before = FlipDelay(false, 4, 2, 2), after = FlipDelay(true, 3, 2, 2);
    std::printf("30 FPS display, 60 Hz vblank: worst flip -> present %d ticks before, %d after\n", before.worst_ticks,
                after.worst_ticks);
    CHECK(after.worst_ticks < before.worst_ticks && after.presents == before.presents);
}

void TestFlipPacingIsGamescopeOnly() {
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(Contains(driver, "bool VideoOutDriver::FlipWaitsForPresenter() {\n"
                           "    if (!AlnPresent::GamescopeSession()) {\n"
                           "        return false;\n"
                           "    }\n"));
    CHECK(Contains(driver, "&& !FlipWaitsForPresenter()) {"));
    // One Gamescope detection: nothing else reads its variable.
    const char* users[] = {"gpu/shadps4/core/libraries/videoout/driver.cpp",
                           "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp", "gpu/shim/window.cpp",
                           "src/runtime_pad.c"};
    for (const char* path : users) {
        CHECK(!Contains(ReadSource(path), "GAMESCOPE_WAYLAND_DISPLAY"));
    }
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp"),
                   "gamescope{AlnPresent::GamescopeSession()}"));
}

void TestSwapchainRecreation() {
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "AlnPresent::ImageCount(surface_min_images, surface_max_images, gamescope)"));
    // The returned images decide the count; the semaphores and frame_index follow it on every recreation.
    CHECK(Contains(swapchain, "image_count = static_cast<u32>(images.size());"));
    CHECK(InOrder(swapchain, "    SetupImages();\n    RefreshSemaphores();\n", "    frame_index = 0;"));
    CHECK(Contains(swapchain, "image_acquired.resize(image_count);"));
    CHECK(Contains(swapchain, "present_ready.resize(image_count);"));
    CHECK(Contains(swapchain, "frame_index = (frame_index + 1) % image_count;"));
    // VSync and HDR changes go through the same recreation.
    CHECK(InOrder(swapchain, "void Swapchain::UpdateVsync() {", "    FindPresentMode();\n    Recreate(width, height);"));
    CHECK(InOrder(swapchain, "void Swapchain::SetHDR(bool hdr) {", "    Recreate(width, height);"));
    CHECK(InOrder(swapchain, "void Swapchain::Recreate(u32 width_, u32 height_) {", "    Create(width_, height_);"));
}

using AlnPresent::SwapchainHealth;
using AlnPresent::SwapResult;
using AlnPresent::SwapStep;

bool Same(SwapStep step, bool usable, bool recreate) {
    return step.usable == usable && step.recreate == recreate;
}

void TestResultClassification() {
    using namespace AlnPresent;
    CHECK(ClassifyAcquire(0) == SwapResult::Success && ClassifyAcquire(1000001003) == SwapResult::Suboptimal);
    CHECK(ClassifyAcquire(-1000001004) == SwapResult::OutOfDate && ClassifyAcquire(-1000000000) == SwapResult::OutOfDate);
    CHECK(ClassifyAcquire(-13) == SwapResult::OutOfDate);
    // Everything else stays a failure: device lost, out of memory, timeout, not ready, incomplete.
    for (const int other : {-4, -1, -2, 1, 2, 5, -1000001001, -12345}) {
        CHECK(ClassifyAcquire(other) == SwapResult::Failure && ClassifyPresent(other) == SwapResult::Failure);
    }
    CHECK(ClassifyPresent(0) == SwapResult::Success && ClassifyPresent(1000001003) == SwapResult::Suboptimal);
    CHECK(ClassifyPresent(-1000001004) == SwapResult::OutOfDate);
    // A lost surface or an unknown error is a recreation condition on acquire, and stays fatal on present.
    CHECK(ClassifyPresent(-1000000000) == SwapResult::Failure && ClassifyPresent(-13) == SwapResult::Failure);
    // Suboptimal is no OutOfDate: it differs in the step it asks for.
    CHECK(ClassifyAcquire(1000001003) != ClassifyAcquire(-1000001004));
}

void TestAcquireSteps() {
    {
        SwapchainHealth health;
        CHECK(Same(health.Acquire(SwapResult::Success), true, false) && !health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        CHECK(Same(health.Acquire(SwapResult::Suboptimal), true, true) && health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        CHECK(Same(health.Acquire(SwapResult::OutOfDate), false, true) && health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        CHECK(Same(health.Acquire(SwapResult::Failure), false, false) && !health.NeedsRecreation());
    }
}

void TestPresentSteps() {
    {
        SwapchainHealth health;
        health.Acquire(SwapResult::Success);
        CHECK(Same(health.Present(SwapResult::Success), true, false) && !health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        health.Acquire(SwapResult::Success);
        CHECK(Same(health.Present(SwapResult::Suboptimal), true, true) && health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        health.Acquire(SwapResult::Success);
        CHECK(Same(health.Present(SwapResult::OutOfDate), false, true) && health.NeedsRecreation());
    }
    {
        SwapchainHealth health;
        health.Acquire(SwapResult::Success);
        CHECK(Same(health.Present(SwapResult::Failure), false, false) && !health.NeedsRecreation());
    }
}

// The presenter's frame, as Presenter::Present runs it: acquire, recreate once and retry when it handed out nothing,
// present, recreate after the present when asked. The results of each call come from the surface's generation.
struct FrameModel {
    SwapchainHealth health;
    int generation = 0;
    int recreations = 0, optional_recreations = 0;
    int presented = 0, skipped = 0, not_queued = 0;
    int unconsumed_acquires = 0;

    void Recreate() {
        if (health.NeedsRecreation()) {
            ++optional_recreations;
        }
        health.Recreated();
        ++generation;
        ++recreations;
    }

    using Source = std::function<SwapResult(int generation)>;

    void Frame(const Source& acquire, const Source& present, bool loading = false) {
        if (loading && health.NeedsRecreation()) {
            Recreate();
        }
        SwapStep step = health.Acquire(acquire(generation));
        if (!step.usable && !loading) {
            Recreate();
            step = health.Acquire(acquire(generation));
        }
        if (!step.usable) {
            ++skipped;
            return;
        }
        const SwapStep done = health.Present(present(generation));
        presented += done.usable;
        not_queued += !done.usable;
        if (!loading && health.NeedsRecreation()) {
            Recreate();
        }
    }
};

void TestPersistentSuboptimalIsKeptWithoutRecreatingEveryFrame() {
    FrameModel model;
    const auto always = [](int) { return SwapResult::Suboptimal; };
    for (int frame = 0; frame < 120; ++frame) {
        model.Frame(always, always);
    }
    std::printf("120 suboptimal frames: %d recreation, %d presented, %d skipped\n", model.recreations, model.presented,
                model.skipped);
    CHECK(model.recreations == 1 && model.optional_recreations == 1);
    CHECK(model.presented == 120 && model.skipped == 0 && model.not_queued == 0);
    CHECK(!model.health.NeedsRecreation());

    // The same run through the loading frame path.
    FrameModel loading;
    for (int frame = 0; frame < 120; ++frame) {
        loading.Frame(always, always, true);
    }
    CHECK(loading.recreations == 1 && loading.presented == 120 && loading.skipped == 0);
}

void TestSuboptimalRecoversWhenTheReplacementIsClean() {
    FrameModel model;
    const auto surface = [](int generation) { return generation == 0 ? SwapResult::Suboptimal : SwapResult::Success; };
    for (int frame = 0; frame < 60; ++frame) {
        model.Frame(surface, surface);
    }
    CHECK(model.recreations == 1 && model.presented == 60 && model.skipped == 0 && !model.health.NeedsRecreation());

    // A later suboptimal result is allowed one new recreation.
    const auto later = [](int) { return SwapResult::Suboptimal; };
    for (int frame = 0; frame < 40; ++frame) {
        model.Frame(later, later);
    }
    CHECK(model.recreations == 2 && model.presented == 100 && model.skipped == 0);
}

void TestSuppressionEndsOnlyAfterCleanOperation() {
    FrameModel model;
    const auto subopt = [](int) { return SwapResult::Suboptimal; };
    const auto clean = [](int) { return SwapResult::Success; };
    for (int frame = 0; frame < 50; ++frame) {
        model.Frame(subopt, subopt);
    }
    CHECK(model.recreations == 1);
    // Half clean frames: a clean acquire with a suboptimal present, or the reverse, change nothing.
    for (int frame = 0; frame < 20; ++frame) {
        model.Frame(clean, subopt);
        model.Frame(subopt, clean);
    }
    for (int frame = 0; frame < 20; ++frame) {
        model.Frame(subopt, subopt);
    }
    CHECK(model.recreations == 1 && model.skipped == 0);
    // A clean acquire and present ends it: the next suboptimal result recreates once more, and then is kept again.
    model.Frame(clean, clean);
    CHECK(model.recreations == 1);
    for (int frame = 0; frame < 50; ++frame) {
        model.Frame(subopt, subopt);
    }
    CHECK(model.recreations == 2 && model.presented == 50 + 40 + 20 + 1 + 50 && model.skipped == 0);
    // A suboptimal present after a clean acquire is the same event as a suboptimal acquire.
    FrameModel present_only;
    for (int frame = 0; frame < 30; ++frame) {
        present_only.Frame(clean, subopt);
    }
    CHECK(present_only.recreations == 1 && present_only.presented == 30);
}

void TestOutOfDateOverridesTheSuppression() {
    FrameModel model;
    const auto subopt = [](int) { return SwapResult::Suboptimal; };
    for (int frame = 0; frame < 30; ++frame) {
        model.Frame(subopt, subopt);
    }
    CHECK(model.recreations == 1);

    // Out of date at acquire: recreated at once, the retry gets an image, nothing is skipped.
    int calls = 0;
    model.Frame([&](int) { return calls++ == 0 ? SwapResult::OutOfDate : SwapResult::Success; },
                [](int) { return SwapResult::Success; });
    CHECK(model.recreations == 2 && model.presented == 31 && model.skipped == 0);

    // Suppressed again, then out of date at present.
    FrameModel at_present;
    for (int frame = 0; frame < 30; ++frame) {
        at_present.Frame(subopt, subopt);
    }
    at_present.Frame(subopt, [](int) { return SwapResult::OutOfDate; });
    CHECK(at_present.recreations == 2 && at_present.not_queued == 1 && at_present.presented == 30);
    at_present.Frame([](int) { return SwapResult::Success; }, [](int) { return SwapResult::Success; });
    CHECK(at_present.recreations == 2 && at_present.presented == 31);

    // A second out of date after the recreation skips that frame only, and the next one recovers.
    FrameModel stuck;
    stuck.Frame([](int) { return SwapResult::OutOfDate; }, [](int) { return SwapResult::Success; });
    CHECK(stuck.skipped == 1 && stuck.recreations == 1 && stuck.health.NeedsRecreation());
    stuck.Frame([](int) { return SwapResult::Success; }, [](int) { return SwapResult::Success; });
    CHECK(stuck.presented == 1 && stuck.skipped == 1);

    // Out of date is a required recreation whether or not suboptimal ones are being kept.
    SwapchainHealth health;
    health.Acquire(SwapResult::Suboptimal);
    health.Present(SwapResult::Suboptimal);
    health.Recreated();
    CHECK(Same(health.Acquire(SwapResult::Suboptimal), true, false));
    CHECK(Same(health.Acquire(SwapResult::OutOfDate), false, true));
}

void TestRequiredRecreationsAreNotHeldBack() {
    // A resize, VSync or HDR change calls Recreated() on its own: the swapchain is replaced whatever the state.
    SwapchainHealth health;
    health.Acquire(SwapResult::Suboptimal);
    health.Recreated();
    for (int frame = 0; frame < 10; ++frame) {
        CHECK(Same(health.Acquire(SwapResult::Suboptimal), true, false));
    }
    health.Recreated(); // resize
    CHECK(!health.NeedsRecreation());
    CHECK(Same(health.Acquire(SwapResult::Suboptimal), true, true));
}

void TestAcquiredImageIsAlwaysConsumed() {
    // ALN-XXJJ7R: an acquire that handed out an image signals its semaphore, so that image is presented. The
    // production decision for "the image is usable" is the same one for Success and Suboptimal, and only those.
    for (const int vk_result : {0, 1000001003, -1000001004, -1000000000, -13, -4, 2, 1, 5}) {
        const SwapResult kind = AlnPresent::ClassifyAcquire(vk_result);
        SwapchainHealth health;
        const bool usable = health.Acquire(kind).usable;
        CHECK(usable == AlnPresent::AcquireHandsOutImage(kind));
        CHECK(usable == (vk_result == 0 || vk_result == 1000001003));
    }
    for (int frame = 0; frame < 120; ++frame) {
        SwapchainHealth health;
        CHECK(health.Acquire(SwapResult::Suboptimal).usable);
    }

    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(Contains(swapchain, "return AlnPresent::AcquireHandsOutImage(AlnPresent::ClassifyAcquire(int(result)));"));
    CHECK(InOrder(swapchain, "bool Swapchain::AcquireNextImage() {", "AlnPresent::ClassifyAcquire(int(result))"));
    CHECK(InOrder(swapchain, "health.Acquire(kind)", "return step.usable;"));
    CHECK(!Contains(swapchain, "needs_recreation"));
    // The present path only follows an acquire that handed out an image; a failed one recreates or skips.
    CHECK(InOrder(presenter, "    if (!swapchain.AcquireNextImage()) {\n        swapchain.Recreate(", "Skipping frame!"));
    CHECK(InOrder(presenter, "Skipping frame!", "info.AddWait(swapchain.GetImageAcquiredSemaphore());"));
    CHECK(InOrder(presenter, "info.AddWait(swapchain.GetImageAcquiredSemaphore());", "presented = swapchain.Present();"));
    // The frame index moves after every present, suboptimal and out of date ones included.
    CHECK(InOrder(swapchain, "health.Present(kind)", "frame_index = (frame_index + 1) % image_count;"));
    // The kept-suboptimal line is printed once, and neither acquire nor present waits for the device.
    CHECK(Contains(swapchain, "Swapchain: the surface stays suboptimal after recreation; presenting with it"));
    CHECK(Contains(swapchain, "logged_kept_suboptimal) {\n        return;"));
    const std::size_t begin = swapchain.find("bool Swapchain::AcquireNextImage() {");
    const std::size_t end = swapchain.find("void Swapchain::FindPresentFormat() {");
    CHECK(begin != std::string::npos && end > begin);
    const std::string frame_path = swapchain.substr(begin, end - begin);
    CHECK(!Contains(frame_path, "WaitDeviceIdle") && !Contains(frame_path, "waitIdle") && !Contains(frame_path, "Recreate("));
}

void TestLoadingFrameHonorsPendingRecreation() {
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::size_t begin = presenter.find("void Presenter::PresentLoadingFrame() {");
    const std::size_t end = presenter.find("    present_scheduler.Finish();", begin);
    CHECK(begin != std::string::npos && end > begin);
    const std::string loading = presenter.substr(begin, end - begin);
    CHECK(InOrder(loading, "swapchain.NeedsRecreation()) {\n        swapchain.Recreate(", "if (!swapchain.AcquireNextImage()) {"));
    CHECK(Contains(loading, "window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||"));
    CHECK(Count(loading, "swapchain.Recreate(") == 1);

    // After an out of date acquire the loading screen recovers on its next frame instead of staying frozen.
    FrameModel model;
    model.Frame([](int) { return SwapResult::OutOfDate; }, [](int) { return SwapResult::Success; }, true);
    CHECK(model.skipped == 1 && model.recreations == 0);
    model.Frame([](int) { return SwapResult::Success; }, [](int) { return SwapResult::Success; }, true);
    CHECK(model.recreations == 1 && model.presented == 1);
}

void TestWindowAndVsyncRecreationsStayRequired() {
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    // The presenter recreates for a size mismatch before it acquires, and never asks the health state first.
    CHECK(InOrder(presenter, "    swapchain.UpdateVsync();\n    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {\n        swapchain.Recreate(",
                  "    if (!swapchain.AcquireNextImage()) {\n        swapchain.Recreate("));
    CHECK(Count(presenter, "NeedsRecreation()") == 1);
    CHECK(!Contains(presenter, "SwapchainHealth"));
    // Every recreation, wanted by the health state or not, goes through Create, which only resets it.
    CHECK(Count(swapchain, "health.Recreated();") == 1 && Count(swapchain, "health.NeedsRecreation()") == 1);
    CHECK(InOrder(swapchain, "void Swapchain::Create(u32 width_, u32 height_) {", "    health.Recreated();"));
    CHECK(InOrder(swapchain, "void Swapchain::UpdateVsync() {", "    FindPresentMode();\n    Recreate(width, height);"));
    CHECK(InOrder(swapchain, "void Swapchain::SetHDR(bool hdr) {", "    Recreate(width, height);"));
    // Persistent suboptimal handling is generic: no Gamescope branch in the helper or the swapchain's frame path.
    const std::string header = ReadSource("gpu/shim/aln_present.h");
    const std::size_t health = header.find("class SwapchainHealth {");
    CHECK(health != std::string::npos && !Contains(header.substr(health, header.find("/// Linux session facts") - health), "Gamescope()"));
}

void TestPadSamplingUntouched() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(!Contains(pad, "Gamescope") && !Contains(pad, "gamescope"));
    // The trace only reads the sampled state, after sampling, and only when switched on.
    CHECK(Contains(pad, "static void trace_read(const PadData *d) {"));
    CHECK(Contains(pad, "    if (!alngpu_latency_trace_enabled()) return;"));
    CHECK(Contains(pad, "    sample(slot,data); ++reads;\n    if (slot==0) trace_read(data);\n"));
    CHECK(!Contains(pad, "ALN_INPUT_LATENCY_TRACE\")"));
}

} // namespace

int main() {
    TestDefaultImageCount();
    TestGamescopeImageCount();
    TestPresentMode();
    TestGamescopeDetection();
    TestLatencyTraceSwitch();
    TestSessionDescription();
    TestFlipPacingModel();
    TestFlipPacingIsGamescopeOnly();
    TestSwapchainRecreation();
    TestResultClassification();
    TestAcquireSteps();
    TestPresentSteps();
    TestPersistentSuboptimalIsKeptWithoutRecreatingEveryFrame();
    TestSuboptimalRecoversWhenTheReplacementIsClean();
    TestSuppressionEndsOnlyAfterCleanOperation();
    TestOutOfDateOverridesTheSuppression();
    TestRequiredRecreationsAreNotHeldBack();
    TestAcquiredImageIsAlwaysConsumed();
    TestLoadingFrameHonorsPendingRecreation();
    TestWindowAndVsyncRecreationsStayRequired();
    TestPadSamplingUntouched();
    if (g_failures) {
        std::printf("%d present test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("present tests passed");
    return 0;
}
