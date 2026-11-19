// Graceful device loss: the first VK_ERROR_DEVICE_LOST owns the root cause and the crash report, every later report
// is a bounded consequence, nothing is submitted to or waited on the lost device and no tick counts as completed.
// No Vulkan device, window or game files.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "common/assert.h"
#include "common/device_lost.h"
#include "common/gpu_trace.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

extern "C" void alngpu_set_fatal_stop(void (*stop)(const char* what, int status));

namespace {

using namespace Common::DeviceLost;
using Common::GpuTrace::Timeline;

std::atomic<int> g_checks{0};
std::atomic<int> g_failures{0};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
        }                                                                                          \
    } while (0)

constexpr s32 OutOfDeviceMemory = -2;
constexpr s32 InitializationFailed = -3;
constexpr s32 Suboptimal = 1000001003;

struct Stopped {};
struct Parked {};
struct Failed {
    std::string site;
    s32 result;
};

struct Recorder {
    std::string output;
    std::atomic<int> captures{};
    std::atomic<int> fault_queries{};
    std::atomic<int> shutdowns{};
    std::atomic<int> parks{};
    std::string summary;
    int status = 0;
    FaultInfo fault{};
    std::atomic<bool> hold_capture{};
    std::atomic<bool> capturing{};
} rec;

void ResetRecorder() {
    rec.output.clear();
    rec.captures = rec.fault_queries = rec.shutdowns = rec.parks = 0;
    rec.summary.clear();
    rec.status = 0;
    rec.fault = FaultInfo{};
    rec.hold_capture = false;
    rec.capturing = false;
}

std::mutex output_mutex;

void TestPrint(const char* line) {
    std::scoped_lock lock{output_mutex};
    rec.output += line;
    rec.output += '\n';
}

void TestCapture(const RootCause&) {
    ++rec.captures;
    rec.capturing = true;
    while (rec.hold_capture.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TestQuery(FaultInfo& info) {
    ++rec.fault_queries;
    info = rec.fault;
}

void TestShutdown(const char* summary, int status) {
    ++rec.shutdowns;
    rec.summary = summary;
    rec.status = status;
    throw Stopped{};
}

void TestPark() {
    ++rec.parks;
    throw Parked{};
}

constexpr Hooks TestHooks{TestPrint, TestCapture, TestQuery, TestShutdown, TestPark};

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// The call or wait a production site makes, with `result` as the driver's answer.
enum class Outcome { Returned, Stopped, Parked };

template <typename F>
Outcome Run(F&& f) {
    try {
        f();
        return Outcome::Returned;
    } catch (const Stopped&) {
        return Outcome::Stopped;
    } catch (const Parked&) {
        return Outcome::Parked;
    }
}

Outcome CallSite(Monitor& monitor, Operation operation, s32 result, int* calls = nullptr) {
    return Run([&] {
        const char* site = OperationName(operation);
        if (operation == Operation::WaitSemaphores || operation == Operation::WaitForFences) {
            WaitWhileHealthy(monitor, operation, site, 7, Timeline::Draw, [&] {
                if (calls) {
                    ++*calls;
                }
                return result;
            });
            return;
        }
        CallWhileHealthy(monitor, operation, site, 7, Timeline::Draw, [&] {
            if (calls) {
                ++*calls;
            }
            return result;
        });
    });
}

void LoseDevice(Monitor& monitor, Operation operation = Operation::WaitSemaphores) {
    CHECK(CallSite(monitor, operation, DeviceLostResult) == Outcome::Stopped);
}

void HealthyAcceptsNormalResults() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    int calls = 0;
    for (const s32 result : {Success, Suboptimal, 1 /* VK_NOT_READY */}) {
        CHECK(CallSite(monitor, Operation::QueueSubmit, result, &calls) == Outcome::Returned);
        monitor.OnResult(Operation::QueuePresent, "present", result, 0, Timeline::Present);
    }
    CHECK(calls == 3);
    CHECK(!monitor.IsLost());
    CHECK(monitor.CurrentPhase() == Phase::Healthy);
    CHECK(rec.output.empty() && rec.captures == 0 && rec.shutdowns == 0);
}

void FirstDeviceLostTransitionsOnce() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    LoseDevice(monitor);
    CHECK(monitor.IsLost());
    CHECK(monitor.CurrentPhase() == Phase::ShutdownRequested);
    CHECK(rec.captures == 1);
    CHECK(rec.shutdowns == 1);
    CHECK(rec.status == ExitStatus);
    const RootCause& root = monitor.Root();
    CHECK(root.operation == Operation::WaitSemaphores);
    CHECK(root.result == DeviceLostResult);
    CHECK(root.thread_id == Common::GpuTrace::CurrentThreadId());
    CHECK(root.tick == 7);
    CHECK(!root.simulated);
    CHECK(Has(monitor.RootLine(), "GPU device lost: first detected in vkWaitSemaphores"));
    CHECK(Has(monitor.RootLine(), "thread " + std::to_string(Common::GpuTrace::CurrentThreadId())));
}

// Mutation A: a later report must not take over the root cause, from any thread.
void LaterReportsKeepTheRootCause() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    LoseDevice(monitor, Operation::QueueSubmit);
    const u32 owner = monitor.Root().thread_id;
    Outcome other = Outcome::Returned;
    std::thread present([&] {
        other = Run([&] {
            monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 9, Timeline::Present);
        });
    });
    present.join();
    CHECK(other == Outcome::Parked);
    CHECK(CallSite(monitor, Operation::AcquireNextImage, DeviceLostResult) == Outcome::Parked);
    CHECK(monitor.Root().operation == Operation::QueueSubmit);
    CHECK(monitor.Root().thread_id == owner);
    CHECK(monitor.Root().tick == 7);
    CHECK(rec.captures == 1);
    CHECK(rec.shutdowns == 1);
    CHECK(Has(rec.summary, "vkQueueSubmit"));
    CHECK(!Has(rec.summary, "vkQueuePresentKHR"));

    const u32 secondary = monitor.SecondaryReports();
    monitor.OnResult(Operation::QueueSubmit, "submit", OutOfDeviceMemory, 99, Timeline::Draw);
    monitor.OnResult(Operation::QueuePresent, "present", Suboptimal, 99, Timeline::Present);
    CHECK(monitor.Root().operation == Operation::QueueSubmit);
    CHECK(monitor.Root().result == DeviceLostResult);
    CHECK(monitor.SecondaryReports() == secondary);
}

// Mutation B: no submission of any kind reaches a lost device.
void SubmissionsRejectedAfterLoss() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    LoseDevice(monitor);
    for (const Operation operation : {Operation::QueueSubmit, Operation::QueueBindSparse,
                                      Operation::AcquireNextImage, Operation::QueuePresent,
                                      Operation::GetSemaphoreCounterValue, Operation::DeviceWaitIdle}) {
        int calls = 0;
        CHECK(CallSite(monitor, operation, Success, &calls) == Outcome::Parked);
        CHECK(calls == 0);
    }
    CHECK(rec.shutdowns == 1);
}

// Mutation C: a wait leaves at once when the loss is already known.
void WaitLeavesWhenLossIsKnown() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    LoseDevice(monitor, Operation::QueueSubmit);
    for (const Operation operation : {Operation::WaitSemaphores, Operation::WaitForFences}) {
        int calls = 0;
        const auto start = std::chrono::steady_clock::now();
        const Outcome outcome = Run([&] {
            WaitWhileHealthy(monitor, operation, "wait", 7, Timeline::Draw, [&] {
                return ++calls < 1000 ? Timeout : Success;
            });
        });
        CHECK(outcome == Outcome::Parked);
        CHECK(calls == 0);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    }
}

void WaitReportsItsOwnDeviceLoss() {
    for (const Operation operation : {Operation::WaitSemaphores, Operation::WaitForFences}) {
        ResetRecorder();
        Monitor monitor{TestHooks};
        int calls = 0;
        const Outcome outcome = Run([&] {
            WaitWhileHealthy(monitor, operation, "wait", 12, Timeline::Draw,
                             [&] { return ++calls < 3 ? Timeout : DeviceLostResult; });
        });
        CHECK(outcome == Outcome::Stopped);
        CHECK(calls == 3);
        CHECK(monitor.Root().operation == operation);
        CHECK(monitor.Root().tick == 12);
    }
}

// Every independent first-failure site reaches the same state and shutdown, naming itself.
void EveryFirstFailureSite() {
    for (const Operation operation : {Operation::QueueSubmit, Operation::QueueBindSparse,
                                      Operation::WaitSemaphores, Operation::WaitForFences,
                                      Operation::AcquireNextImage, Operation::QueuePresent}) {
        ResetRecorder();
        Monitor monitor{TestHooks};
        CHECK(CallSite(monitor, operation, DeviceLostResult) == Outcome::Stopped);
        CHECK(monitor.CurrentPhase() == Phase::ShutdownRequested);
        CHECK(monitor.Root().operation == operation);
        CHECK(rec.shutdowns == 1);
        CHECK(rec.status == ExitStatus);
        CHECK(Has(rec.summary, std::string{"first detected in "} + OperationName(operation)));
        CHECK(Count(rec.output, "GPU device lost: first detected") == 1);
    }
}

// The diagnostic injection drives the same handler without the driver call it replaces.
void SimulationUsesTheProductionHandler() {
    for (const Operation operation : {Operation::QueueSubmit, Operation::QueueBindSparse,
                                      Operation::WaitSemaphores, Operation::WaitForFences,
                                      Operation::AcquireNextImage, Operation::QueuePresent}) {
        ResetRecorder();
        rec.fault.status = FaultStatus::Captured;
        Monitor monitor{TestHooks};
        monitor.ArmSimulation(operation, 2);
        int calls = 0;
        CHECK(CallSite(monitor, operation, Success, &calls) == Outcome::Returned);
        CHECK(CallSite(monitor, Operation::DeviceWaitIdle, Success, &calls) == Outcome::Returned);
        CHECK(CallSite(monitor, operation, Success, &calls) == Outcome::Stopped);
        CHECK(calls == 2);
        CHECK(monitor.Root().operation == operation);
        CHECK(monitor.Root().simulated);
        CHECK(Has(rec.summary, "simulated by ALN_SIMULATE_DEVICE_LOST, diagnostic only"));
        CHECK(rec.fault_queries == 0);
        CHECK(Has(rec.summary, "device fault not queried (simulated)"));
    }
    Operation operation{};
    u64 calls = 0;
    CHECK(ParseSimulation("present", operation, calls) && operation == Operation::QueuePresent && calls == 1);
    CHECK(ParseSimulation("submit:600", operation, calls) && operation == Operation::QueueSubmit && calls == 600);
    CHECK(ParseSimulation("sparse:3", operation, calls) && operation == Operation::QueueBindSparse);
    CHECK(ParseSimulation("semwait", operation, calls) && operation == Operation::WaitSemaphores);
    CHECK(ParseSimulation("fence", operation, calls) && operation == Operation::WaitForFences);
    CHECK(ParseSimulation("acquire", operation, calls) && operation == Operation::AcquireNextImage);
    CHECK(!ParseSimulation("", operation, calls));
    CHECK(!ParseSimulation("present:", operation, calls));
    CHECK(!ParseSimulation("present:0", operation, calls));
    CHECK(!ParseSimulation("present:x", operation, calls));
    CHECK(!ParseSimulation("present:12345678901", operation, calls));
    CHECK(!ParseSimulation("vkQueueSubmit", operation, calls));

    ResetRecorder();
    Monitor unarmed{TestHooks};
    for (u32 i = 0; i < u32(Operation::Count); ++i) {
        CHECK(!unarmed.Simulate(Operation(i)));
    }
}

// Mutation F: VK_EXT_device_fault is asked once, whatever reports follow.
void FaultQueriedOnce() {
    ResetRecorder();
    rec.fault.status = FaultStatus::Captured;
    Monitor monitor{TestHooks};
    LoseDevice(monitor);
    monitor.QueryFaultOnce();
    CHECK(CallSite(monitor, Operation::QueuePresent, DeviceLostResult) == Outcome::Parked);
    monitor.QueryFaultOnce();
    CHECK(rec.fault_queries == 1);
}

void FaultQueryFailureIsReported() {
    struct Case {
        FaultStatus status;
        s32 result;
        const char* summary;
        const char* line;
    };
    for (const Case& c : {Case{FaultStatus::Failed, InitializationFailed, "device fault query failed (result -3)",
                               "Device fault: query failed, result -3"},
                          Case{FaultStatus::Unsupported, 0, "VK_EXT_device_fault unavailable",
                               "Device fault report unavailable (no VK_EXT_device_fault)"},
                          Case{FaultStatus::NotQueried, 0, "device fault not queried",
                               "Device fault: not queried"}}) {
        ResetRecorder();
        rec.fault.status = c.status;
        rec.fault.result = c.result;
        Monitor monitor{TestHooks};
        LoseDevice(monitor);
        CHECK(rec.shutdowns == 1);
        CHECK(Has(rec.summary, c.summary));
        CHECK(Has(rec.output, c.line));
    }
    ResetRecorder();
    Monitor no_query{Hooks{TestPrint, TestCapture, nullptr, TestShutdown, TestPark}};
    LoseDevice(no_query);
    CHECK(rec.shutdowns == 1);
    CHECK(Has(rec.summary, "device fault not queried"));
}

// Mutation G: the crash report's reason is the root cause with the device and its fault.
void CrashReportCarriesTheRootCause() {
    ResetRecorder();
    Common::GpuTrace::SetDeviceIdentity("Test GPU (driver 9.9, Vulkan 1.3)");
    rec.fault.status = FaultStatus::Captured;
    CopyDriverText(rec.fault.description, sizeof(rec.fault.description), "page\nfault in\tshader", 64);
    rec.fault.address_count = 11;
    rec.fault.addresses[0] = {4, 0x7f0012340000ull, 0x1000};
    rec.fault.addresses[1] = {1, 0x1000, 0x100};
    rec.fault.vendor_count = 1;
    rec.fault.vendors[0].code = 0xdead;
    rec.fault.vendor_binary_size = 4096;
    Monitor monitor{TestHooks};
    LoseDevice(monitor, Operation::QueueSubmit);
    const std::string tid = std::to_string(Common::GpuTrace::CurrentThreadId());
    CHECK(Has(rec.summary, "GPU device lost: first detected in vkQueueSubmit (vkQueueSubmit), result -4, thread " +
                               tid));
    CHECK(Has(rec.summary, "draw tick 7"));
    CHECK(Has(rec.summary, "device Test GPU (driver 9.9, Vulkan 1.3)"));
    CHECK(Has(rec.summary, "device fault: page fault in shader, 11 addresses, first instruction pointer unknown "
                           "at 0x7f0012340000"));
    CHECK(Has(rec.summary, "1 vendor faults, first 0xdead"));
    CHECK(Has(rec.output, "Device fault: invalid read at 0x1000 (precision 0x100)"));
    CHECK(Has(rec.output, "Device fault: and 3 more addresses"));
    CHECK(Has(rec.output, "Device fault: vendor binary of 4096 bytes"));
    CHECK(rec.summary.find('\n') == std::string::npos);
    CHECK(std::string{monitor.Summary()} == rec.summary);
}

void SecondaryReportsAreBounded() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    LoseDevice(monitor);
    for (int i = 0; i < 10; ++i) {
        CHECK(CallSite(monitor, Operation::QueuePresent, Success) == Outcome::Parked);
    }
    CHECK(monitor.SecondaryReports() == 10);
    CHECK(Count(rec.output, "a consequence of the first report") == MaxSecondaryLines);
    CHECK(Count(rec.output, "further consequences on other threads are not shown") == 1);
    CHECK(Count(rec.output, "GPU device lost: first detected") == 1);
    CHECK(rec.shutdowns == 1);
    CHECK(rec.parks == 10);
}

// A thread that meets the loss while the first is still capturing waits for its diagnostics; one shutdown.
void ConcurrentReportsWaitForTheOwner() {
    ResetRecorder();
    rec.fault.status = FaultStatus::Captured;
    CopyDriverText(rec.fault.description, sizeof(rec.fault.description), "owner fault", 32);
    rec.hold_capture = true;
    Monitor monitor{TestHooks};
    Outcome owner = Outcome::Returned;
    std::thread first([&] { owner = CallSite(monitor, Operation::QueueBindSparse, DeviceLostResult); });
    while (!rec.capturing.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<Outcome> others(4, Outcome::Returned);
    std::vector<std::thread> threads;
    for (Outcome& outcome : others) {
        Outcome* slot = &outcome;
        threads.emplace_back([&monitor, slot] { *slot = CallSite(monitor, Operation::QueuePresent, DeviceLostResult); });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(rec.shutdowns == 0);
    rec.hold_capture = false;
    first.join();
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(owner == Outcome::Stopped);
    for (const Outcome outcome : others) {
        CHECK(outcome == Outcome::Parked);
    }
    CHECK(rec.shutdowns == 1);
    CHECK(monitor.Root().operation == Operation::QueueBindSparse);
    CHECK(Has(rec.summary, "device fault: owner fault"));
}

void WorkersObserveTheLoss() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    std::stop_source running;
    CHECK(!ShouldStop(monitor, running.get_token()));
    std::stop_source stopped;
    stopped.request_stop();
    CHECK(ShouldStop(monitor, stopped.get_token()));
    LoseDevice(monitor);
    CHECK(ShouldStop(monitor, running.get_token()));
}

struct FakeTimeline {
    Vulkan::TimelineTicks ticks;
    u64 device_counter = 0;
    s32 read_result = Success;
    s32 wait_result = Success;
    int reads = 0;
    int waits = 0;
    std::vector<std::pair<std::string, s32>> failures;

    FakeTimeline(u64 known, u64 next) {
        ticks.gpu_tick = known;
        ticks.current_tick = next;
        device_counter = known;
    }
    auto Read() {
        return [this](u64& counter) {
            ++reads;
            counter = device_counter;
            return read_result;
        };
    }
    auto Fail() {
        return [this](const char* site, s32 result, u64) {
            failures.emplace_back(site, result);
            throw Failed{site, result};
        };
    }
    void Wait(Monitor& monitor, u64 tick) {
        Vulkan::WaitForTick(
            ticks, monitor, Timeline::Draw, tick,
            [&] {
                ++waits;
                if (wait_result == Success) {
                    device_counter = tick;
                }
                return wait_result;
            },
            Read(), Fail());
    }
    void Refresh(Monitor& monitor) {
        Vulkan::RefreshKnownTick(ticks, monitor, Timeline::Draw, Read(), Fail());
    }
};

void HealthyTimelineUnchanged() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    FakeTimeline timeline{10, 20};
    timeline.Wait(monitor, 8);
    CHECK(timeline.reads == 0 && timeline.waits == 0);
    timeline.device_counter = 12;
    timeline.Wait(monitor, 12);
    CHECK(timeline.reads == 1 && timeline.waits == 0);
    CHECK(timeline.ticks.gpu_tick == 12);
    timeline.Wait(monitor, 15);
    CHECK(timeline.waits == 1);
    CHECK(timeline.ticks.gpu_tick == 15);
    int calls = 0;
    CHECK(Run([&] {
              CHECK(WaitWhileHealthy(monitor, Operation::WaitForFences, "fence", 0, Timeline::Present,
                                     [&] { return ++calls < 4 ? Timeout : Success; }) == Success);
          }) == Outcome::Returned);
    CHECK(calls == 4);
    CHECK(!monitor.IsLost() && rec.output.empty());
}

// Mutation H: only VK_ERROR_DEVICE_LOST is a device loss; other failures keep their own fatal paths.
void OtherFailuresKeepTheirBehaviour() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    for (const s32 result : {OutOfDeviceMemory, InitializationFailed, -1000001004 /* out of date */,
                             -1000000000 /* surface lost */}) {
        int calls = 0;
        s32 returned = Success;
        CHECK(Run([&] {
                  returned = CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 1, Timeline::Draw, [&] {
                      ++calls;
                      return result;
                  });
              }) == Outcome::Returned);
        CHECK(returned == result && calls == 1);
        CHECK(Run([&] {
                  returned = WaitWhileHealthy(monitor, Operation::WaitForFences, "fence", 1, Timeline::Draw,
                                              [&] { return result; });
              }) == Outcome::Returned);
        CHECK(returned == result);
    }
    FakeTimeline wait{1, 5};
    wait.wait_result = OutOfDeviceMemory;
    try {
        wait.Wait(monitor, 3);
        CHECK(false);
    } catch (const Failed& failed) {
        CHECK(failed.site == "semwait" && failed.result == OutOfDeviceMemory);
    }
    FakeTimeline read{1, 5};
    read.read_result = InitializationFailed;
    try {
        read.Refresh(monitor);
        CHECK(false);
    } catch (const Failed& failed) {
        CHECK(failed.site == "semread" && failed.result == InitializationFailed);
    }
    CHECK(!monitor.IsLost());
    CHECK(rec.captures == 0 && rec.shutdowns == 0 && rec.output.empty());
}

// Mutation D: a lost device completes nothing; its counter past every submission is a loss, not progress.
void NoFakeCompletion() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    FakeTimeline timeline{3, 6};
    timeline.wait_result = DeviceLostResult;
    CHECK(Run([&] { timeline.Wait(monitor, 5); }) == Outcome::Stopped);
    CHECK(timeline.ticks.gpu_tick == 3);
    CHECK(monitor.Root().operation == Operation::WaitSemaphores);
    CHECK(monitor.Root().tick == 5);

    timeline.device_counter = 5;
    CHECK(Run([&] { timeline.Refresh(monitor); }) == Outcome::Parked);
    CHECK(Run([&] { timeline.Wait(monitor, 5); }) == Outcome::Parked);
    CHECK(timeline.ticks.gpu_tick == 3);

    ResetRecorder();
    Monitor garbage{TestHooks};
    FakeTimeline past{3, 6};
    past.device_counter = ~0ull;
    CHECK(Run([&] { past.Refresh(garbage); }) == Outcome::Stopped);
    CHECK(past.ticks.gpu_tick == 3);
    CHECK(garbage.Root().operation == Operation::GetSemaphoreCounterValue);
}

// Retirement runs on known GPU ticks only: work submitted before the loss is never taken as done.
void RetirementWaitsForRealCompletion() {
    ResetRecorder();
    Monitor monitor{TestHooks};
    FakeTimeline timeline{3, 7};
    std::vector<u64> pending{2, 3, 4, 5, 6};
    std::vector<u64> retired;
    const auto retire = [&] {
        while (!pending.empty() && timeline.ticks.gpu_tick >= pending.front()) {
            retired.push_back(pending.front());
            pending.erase(pending.begin());
        }
    };
    retire();
    timeline.wait_result = DeviceLostResult;
    CHECK(Run([&] { timeline.Wait(monitor, 6); }) == Outcome::Stopped);
    timeline.device_counter = 6;
    CHECK(Run([&] { timeline.Refresh(monitor); }) == Outcome::Parked);
    retire();
    CHECK((retired == std::vector<u64>{2, 3}));
    CHECK((pending == std::vector<u64>{4, 5, 6}));
}

void DriverTextIsBounded() {
    char out[8];
    CopyDriverText(out, sizeof(out), "abc\r\ndefghij", 12);
    CHECK(std::string{out} == "abc  de");
    const char unterminated[4] = {'a', 'b', 'c', 'd'};
    CopyDriverText(out, sizeof(out), unterminated, sizeof(unterminated));
    CHECK(std::string{out} == "abcd");
    CopyDriverText(out, sizeof(out), nullptr, 5);
    CHECK(out[0] == '\0');
    CHECK(std::string{FaultAddressTypeName(6)} == "instruction pointer fault");
    CHECK(std::string{FaultAddressTypeName(99)} == "unknown address type");
}

std::string ReadFile(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    std::stringstream text;
    text << file.rdbuf();
    std::string result = text.str();
    std::erase(result, '\r');
    return result;
}

// Production wiring of the call sites: every Vulkan entry point that can report a device loss goes through the
// monitor (Mutation E: the presenter cannot acquire or present past it).
bool Wrapped(const std::string& text, const std::string& call, const std::string& helper) {
    size_t found = 0;
    for (size_t pos = text.find(call); pos != std::string::npos; pos = text.find(call, pos + 1)) {
        const size_t helper_pos = text.rfind(helper + "(", pos);
        if (helper_pos == std::string::npos || pos - helper_pos > 400 ||
            text.find("\n}\n", helper_pos) < pos) {
            return false;
        }
        ++found;
    }
    return found > 0;
}

void CallSitesGoThroughTheMonitor() {
    const std::string root = ALN_SOURCE_DIR "/gpu/shadps4/video_core/";
    const std::string swapchain = ReadFile(root + "renderer_vulkan/vk_swapchain.cpp");
    CHECK(Wrapped(swapchain, ".acquireNextImageKHR(", "CallWhileHealthy"));
    CHECK(Wrapped(swapchain, ".presentKHR(", "CallWhileHealthy"));
    CHECK(Wrapped(swapchain, ".waitIdle(", "CallWhileHealthy"));
    const std::string presenter = ReadFile(root + "renderer_vulkan/vk_presenter.cpp");
    CHECK(Wrapped(presenter, ".waitForFences(", "WaitWhileHealthy"));
    const std::string scheduler = ReadFile(root + "renderer_vulkan/vk_scheduler.cpp");
    CHECK(Wrapped(scheduler, ".submit(submit_info", "CallWhileHealthy"));
    const size_t submit = scheduler.find("void Scheduler::SubmitExecution(SubmitInfo& info) {");
    CHECK(submit != std::string::npos &&
          scheduler.find("Common::DeviceLost::IsLost()", submit) < scheduler.find("submit_mutex", submit));
    const std::string buffer_cache = ReadFile(root + "buffer_cache/buffer_cache.cpp");
    CHECK(Wrapped(buffer_cache, ".bindSparse(", "CallWhileHealthy"));
    const std::string semaphore = ReadFile(root + "renderer_vulkan/vk_semaphore.cpp");
    CHECK(Count(semaphore, ".waitSemaphores(") == 1 && Wrapped(semaphore, ".waitSemaphores(", "WaitForTick"));
    CHECK(Count(semaphore, ".getSemaphoreCounterValue(") == 1);
    CHECK(Has(semaphore, "RefreshKnownTick(") && Has(semaphore, "WaitForTick("));
    const std::string liverpool = ReadFile(root + "amdgpu/liverpool.cpp");
    CHECK(Has(liverpool, "while (!Common::DeviceLost::ShouldStop(device_lost, stoken))"));
    CHECK(Has(liverpool, "while ((num_submits || num_commands) && !device_lost.IsLost())"));
    for (const char* file : {"renderer_vulkan/vk_scheduler.cpp", "renderer_vulkan/vk_presenter.cpp",
                             "renderer_vulkan/vk_semaphore.cpp", "buffer_cache/buffer_cache.cpp"}) {
        CHECK(!Has(ReadFile(root + file), "eErrorDeviceLost"));
    }
}

void NoTemporaryDiagnostics() {
    for (const char* file : {"gpu/shim/common/device_lost.h", "gpu/shim/common/device_lost.cpp",
                             "gpu/shim/common/assert.cpp", "gpu/shim/common/gpu_trace.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.h",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp",
                             "gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp",
                             "gpu/shadps4/video_core/amdgpu/liverpool.cpp", "src/crash_guard.c",
                             "tests/crash_guard_tests.c"}) {
        const std::string text = ReadFile(std::string{ALN_SOURCE_DIR "/"} + file);
        CHECK(!text.empty());
        CHECK(!Has(text, std::string{"DEBUG_"} + "SAULO"));
    }
}

constexpr int StopMarkStatus = 40;

void RecordFatalStop(const char* what, int status) {
    std::fprintf(stderr, "fatal stop: %s, status %d\n", what, status);
    std::fflush(stderr);
    std::thread([] { ASSERT_MSG(false, "resource destruction after the loss"); }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::fflush(stderr);
    std::_Exit(status + StopMarkStatus);
}

void ChildQuery(FaultInfo& info) {
    info.status = FaultStatus::Captured;
    CopyDriverText(info.description, sizeof(info.description), "child fault", 32);
    info.address_count = 1;
    info.addresses[0] = {3, 0xabc000, 0x1000};
}

int DeviceLostChild(const std::string& path) {
    if (!std::freopen(path.c_str(), "w", stderr)) {
        return 2;
    }
    alngpu_set_fatal_stop(RecordFatalStop);
    Common::GpuTrace::SetDeviceIdentity("Child GPU (driver 2.0, Vulkan 1.3)");
    Global().SetFaultQuery(ChildQuery);
    CallWhileHealthy(Global(), Operation::QueueSubmit, "submit", 77, Timeline::Draw,
                     [] { return DeviceLostResult; });
    return 3;
}

// The production monitor: the GPU trace, the fault report and one fatal stop with the root cause; an assertion
// on another thread afterwards is a consequence, not a second crash reason.
void ProductionMonitorEndsThroughTheFatalStop(const char* self, const std::string& path) {
    const std::string command = std::string{"\""} + self + "\" --device-lost-child";
    int status = std::system(command.c_str());
#ifndef _WIN32
    status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    CHECK(status == ExitStatus + StopMarkStatus);
    const std::string text = ReadFile(path);
    const size_t root = text.find("GPU device lost: first detected in vkQueueSubmit (submit), result -4, thread ");
    const size_t trace = text.find("GPU trace: Vulkan device lost");
    const size_t fault = text.find("Device fault: child fault");
    const size_t stop = text.find("fatal stop: GPU device lost: first detected in vkQueueSubmit (submit), result -4");
    CHECK(root != std::string::npos);
    CHECK(trace != std::string::npos && trace > root);
    CHECK(Has(text, "DEVICE LOST at submit, result -4, draw tick 77"));
    CHECK(fault != std::string::npos && fault > trace);
    CHECK(stop != std::string::npos && stop > fault);
    CHECK(Has(text, "device Child GPU (driver 2.0, Vulkan 1.3); device fault: child fault, 1 addresses, first "
                    "invalid execute at 0xabc000, status 25"));
    CHECK(Count(text, "fatal stop:") == 1);
    CHECK(Has(text, "a GPU assertion (see the log line above) on thread "));
    CHECK(!Has(text, "STOP: GPU library assertion failed"));
    CHECK(Count(text, "GPU trace: ") > 0 && Count(text, "detected on thread") == 1);
    if (g_failures) {
        std::printf("---- child output ----\n%s\n", text.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string self = argc > 0 ? argv[0] : "device_lost_tests";
    const std::string child_path = self + ".child.txt";
    if (argc == 2 && std::string{argv[1]} == "--device-lost-child") {
        return DeviceLostChild(child_path);
    }
    HealthyAcceptsNormalResults();
    FirstDeviceLostTransitionsOnce();
    LaterReportsKeepTheRootCause();
    SubmissionsRejectedAfterLoss();
    WaitLeavesWhenLossIsKnown();
    WaitReportsItsOwnDeviceLoss();
    EveryFirstFailureSite();
    SimulationUsesTheProductionHandler();
    FaultQueriedOnce();
    FaultQueryFailureIsReported();
    CrashReportCarriesTheRootCause();
    SecondaryReportsAreBounded();
    ConcurrentReportsWaitForTheOwner();
    WorkersObserveTheLoss();
    HealthyTimelineUnchanged();
    OtherFailuresKeepTheirBehaviour();
    NoFakeCompletion();
    RetirementWaitsForRealCompletion();
    DriverTextIsBounded();
    CallSitesGoThroughTheMonitor();
    NoTemporaryDiagnostics();
    ProductionMonitorEndsThroughTheFatalStop(self.c_str(), child_path);
    std::remove(child_path.c_str());
    std::printf("%d checks, %d failures\n", g_checks.load(), g_failures.load());
    return g_failures == 0 ? 0 : 1;
}
