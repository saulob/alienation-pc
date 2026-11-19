// aln: see device_lost.h.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include "common/device_lost.h"

namespace Common::DeviceLost {
namespace {

constexpr auto OwnerWait = std::chrono::seconds(15);
constexpr auto OwnerPoll = std::chrono::milliseconds(10);

struct Appender {
    char* out;
    size_t size;
    size_t used = 0;

    void Add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (used + 1 >= size) {
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(out + used, size - used, format, args);
        va_end(args);
        if (written > 0) {
            used = std::min(size - 1, used + size_t(written));
        }
    }
};

void PrintToStderr(const char* line) {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void CaptureGpuTrace(const RootCause& root) {
    GpuTrace::VulkanFailure(root.site, root.result, root.tick, root.timeline);
}

void StopWithCrashReport(const char* summary, int status) {
    StopProcess(summary, status);
}

constinit Monitor global_monitor{Hooks{
    .print = PrintToStderr,
    .capture = CaptureGpuTrace,
    .query_fault = nullptr,
    .shutdown = StopWithCrashReport,
    .park = nullptr,
}};

constexpr const char* SimulationNames[] = {"submit", "sparse",  "semwait", "semread",
                                           "fence",  "acquire", "present", "idle"};
static_assert(std::size(SimulationNames) == size_t(Operation::Count));

} // namespace

const char* OperationName(Operation operation) noexcept {
    switch (operation) {
    case Operation::QueueSubmit:
        return "vkQueueSubmit";
    case Operation::QueueBindSparse:
        return "vkQueueBindSparse";
    case Operation::WaitSemaphores:
        return "vkWaitSemaphores";
    case Operation::GetSemaphoreCounterValue:
        return "vkGetSemaphoreCounterValue";
    case Operation::WaitForFences:
        return "vkWaitForFences";
    case Operation::AcquireNextImage:
        return "vkAcquireNextImageKHR";
    case Operation::QueuePresent:
        return "vkQueuePresentKHR";
    case Operation::DeviceWaitIdle:
        return "vkDeviceWaitIdle";
    case Operation::Count:
        break;
    }
    return "unknown Vulkan operation";
}

const char* FaultAddressTypeName(u32 type) noexcept {
    constexpr const char* names[] = {
        "none", "invalid read", "invalid write", "invalid execute", "instruction pointer unknown",
        "instruction pointer invalid", "instruction pointer fault",
    };
    return type < std::size(names) ? names[type] : "unknown address type";
}

void CopyDriverText(char* out, size_t size, const char* text, size_t text_size) noexcept {
    if (size == 0) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; text && i < text_size && text[i] && used + 1 < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        out[used++] = c < 0x20 || c == 0x7f ? ' ' : char(c);
    }
    out[used] = '\0';
}

Monitor& Global() noexcept {
    return global_monitor;
}

void Monitor::AdvanceTo(Phase next) noexcept {
    Phase current = phase.load(std::memory_order_acquire);
    while (u32(current) < u32(next) &&
           !phase.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
    }
}

void Monitor::Print(const char* line) {
    if (hooks.print) {
        hooks.print(line);
    }
}

void Monitor::BuildRootLine() {
    Appender text{root_line, sizeof(root_line)};
    text.Add("GPU device lost: first detected in %s (%s), result %d, thread %u (%s), %s tick %llu",
             OperationName(root.operation), root.site, root.result, root.thread_id, root.thread_name,
             GpuTrace::TimelineName(root.timeline), (unsigned long long)root.tick);
    if (root.simulated) {
        text.Add(" [simulated by ALN_SIMULATE_DEVICE_LOST, diagnostic only]");
    }
}

void Monitor::PrintFault() {
    char line[512];
    switch (fault.status) {
    case FaultStatus::NotQueried:
        Print("Device fault: not queried (no Vulkan device)");
        return;
    case FaultStatus::Unsupported:
        Print("Device fault report unavailable (no VK_EXT_device_fault)");
        return;
    case FaultStatus::Skipped:
        Print("Device fault: not queried (simulated device loss)");
        return;
    case FaultStatus::Failed:
        std::snprintf(line, sizeof(line), "Device fault: query failed, result %d", fault.result);
        Print(line);
        return;
    case FaultStatus::Captured:
        break;
    }
    std::snprintf(line, sizeof(line), "Device fault: %s", fault.description[0] ? fault.description : "(no description)");
    Print(line);
    for (u32 i = 0; i < std::min(fault.address_count, MaxFaultAddresses); ++i) {
        const auto& address = fault.addresses[i];
        std::snprintf(line, sizeof(line), "Device fault: %s at %#llx (precision %#llx)",
                      FaultAddressTypeName(address.type), (unsigned long long)address.address,
                      (unsigned long long)address.precision);
        Print(line);
    }
    if (fault.address_count > MaxFaultAddresses) {
        std::snprintf(line, sizeof(line), "Device fault: and %u more addresses", fault.address_count - MaxFaultAddresses);
        Print(line);
    }
    for (u32 i = 0; i < std::min(fault.vendor_count, MaxVendorFaults); ++i) {
        const auto& vendor = fault.vendors[i];
        std::snprintf(line, sizeof(line), "Device fault: vendor fault %#llx data %#llx: %s",
                      (unsigned long long)vendor.code, (unsigned long long)vendor.data, vendor.description);
        Print(line);
    }
    if (fault.vendor_binary_size) {
        std::snprintf(line, sizeof(line), "Device fault: vendor binary of %llu bytes (not kept)",
                      (unsigned long long)fault.vendor_binary_size);
        Print(line);
    }
}

void Monitor::QueryFaultOnce() {
    if (fault_claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto* const query = query_fault.load(std::memory_order_acquire);
    if (simulated_hit.load(std::memory_order_relaxed)) {
        fault.status = FaultStatus::Skipped;
    } else if (query) {
        query(fault);
    }
    PrintFault();
    fault_ready.store(true, std::memory_order_release);
}

void Monitor::BuildSummary() {
    Appender text{summary, sizeof(summary)};
    text.Add("%s; device %s", root_line,
             GpuTrace::DeviceIdentity()[0] ? GpuTrace::DeviceIdentity() : "unknown");
    if (!fault_ready.load(std::memory_order_acquire)) {
        text.Add("; device fault not queried");
        return;
    }
    switch (fault.status) {
    case FaultStatus::NotQueried:
        text.Add("; device fault not queried");
        break;
    case FaultStatus::Unsupported:
        text.Add("; VK_EXT_device_fault unavailable");
        break;
    case FaultStatus::Skipped:
        text.Add("; device fault not queried (simulated)");
        break;
    case FaultStatus::Failed:
        text.Add("; device fault query failed (result %d)", fault.result);
        break;
    case FaultStatus::Captured:
        text.Add("; device fault: %s", fault.description[0] ? fault.description : "(no description)");
        if (fault.address_count) {
            const auto& first = fault.addresses[0];
            text.Add(", %u addresses, first %s at %#llx", fault.address_count,
                     FaultAddressTypeName(first.type), (unsigned long long)first.address);
        }
        if (fault.vendor_count) {
            text.Add(", %u vendor faults, first %#llx", fault.vendor_count,
                     (unsigned long long)fault.vendors[0].code);
        }
        break;
    }
}

void Monitor::Report(Operation operation, const char* site, s32 result, u64 tick,
                     GpuTrace::Timeline timeline) {
    Phase expected = Phase::Healthy;
    if (!phase.compare_exchange_strong(expected, Phase::Detected, std::memory_order_acq_rel)) {
        NoteSecondary(OperationName(operation));
        Park();
    }
    root.operation = operation;
    root.site = site ? site : "";
    root.result = result;
    root.tick = tick;
    root.timeline = timeline;
    root.thread_id = GpuTrace::CurrentThreadId();
    GpuTrace::ThreadName(root.thread_id, root.thread_name, sizeof(root.thread_name));
    root.unix_seconds = s64(std::time(nullptr));
    root.simulated = simulated_hit.load(std::memory_order_relaxed);
    BuildRootLine();
    root_ready.store(true, std::memory_order_release);
    Print(root_line);

    if (hooks.capture) {
        hooks.capture(root);
    }
    QueryFaultOnce();
    BuildSummary();
    summary_ready.store(true, std::memory_order_release);
    AdvanceTo(Phase::DiagnosticsCaptured);
    RequestShutdown();
    Park();
}

void Monitor::Observe(const char* where) {
    NoteSecondary(where);
    Park();
}

void Monitor::NoteSecondary(const char* where) {
    const u32 seen = secondary_reports.fetch_add(1, std::memory_order_relaxed);
    if (seen > MaxSecondaryLines) {
        return;
    }
    if (seen == MaxSecondaryLines) {
        Print("GPU device lost: further consequences on other threads are not shown");
        return;
    }
    char name[64];
    const u32 tid = GpuTrace::CurrentThreadId();
    GpuTrace::ThreadName(tid, name, sizeof(name));
    char line[256];
    std::snprintf(line, sizeof(line), "GPU device lost: %s on thread %u (%s) stopped, a consequence of the first report",
                  where, tid, name);
    Print(line);
}

// The detecting thread ends the process, so the crash report names it; another thread only takes over when the
// diagnostics hang.
void Monitor::WaitForOwner() {
    const auto deadline = std::chrono::steady_clock::now() + OwnerWait;
    while (phase.load(std::memory_order_acquire) != Phase::ShutdownRequested &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(OwnerPoll);
    }
}

void Monitor::RequestShutdown() {
    if (shutdown_issued.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    AdvanceTo(Phase::ShutdownRequested);
    const char* reason = summary_ready.load(std::memory_order_acquire) ? summary
                         : root_ready.load(std::memory_order_acquire)  ? root_line
                                                                       : "GPU device lost";
    if (hooks.shutdown) {
        hooks.shutdown(reason, ExitStatus);
    }
}

void Monitor::Park() {
    WaitForOwner();
    RequestShutdown();
    if (hooks.park) {
        hooks.park();
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void Monitor::ArmSimulation(Operation operation, u64 calls) noexcept {
    simulated_calls.store(calls ? calls : 1, std::memory_order_relaxed);
    simulated_operation.store(u32(operation), std::memory_order_release);
}

bool ParseSimulation(std::string_view value, Operation& operation, u64& calls) noexcept {
    const size_t colon = value.find(':');
    const std::string_view name = value.substr(0, colon);
    calls = 1;
    if (colon != std::string_view::npos) {
        u64 parsed = 0;
        const std::string_view digits = value.substr(colon + 1);
        if (digits.empty() || digits.size() > 9) {
            return false;
        }
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return false;
            }
            parsed = parsed * 10 + u64(c - '0');
        }
        if (parsed == 0) {
            return false;
        }
        calls = parsed;
    }
    for (u32 i = 0; i < u32(Operation::Count); ++i) {
        if (name == SimulationNames[i]) {
            operation = Operation(i);
            return true;
        }
    }
    return false;
}

void ArmSimulationFromEnvironment() {
    static std::atomic<bool> armed{};
    const char* value = std::getenv("ALN_SIMULATE_DEVICE_LOST");
    if (!value || !value[0] || armed.exchange(true)) {
        return;
    }
    Operation operation{};
    u64 calls = 0;
    if (!ParseSimulation(value, operation, calls)) {
        std::fprintf(stderr, "Diagnostics: ALN_SIMULATE_DEVICE_LOST=%s not understood; nothing simulated\n", value);
        return;
    }
    Global().ArmSimulation(operation, calls);
    std::fprintf(stderr,
                 "Diagnostics: ALN_SIMULATE_DEVICE_LOST armed: call %llu of %s reports a device loss without "
                 "reaching the driver (diagnostic only, no real device loss)\n",
                 (unsigned long long)calls, OperationName(operation));
}

} // namespace Common::DeviceLost
