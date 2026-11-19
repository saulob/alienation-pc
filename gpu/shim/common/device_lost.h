// aln: the Vulkan device is dead after its first VK_ERROR_DEVICE_LOST. The first report owns the root cause, captures
// the diagnostics and ends the process through the crash report. Any other thread that meets the lost device stops
// there: it never submits to it, waits on it or takes its work as completed.
#pragma once
#include <atomic>
#include <cstddef>
#include <string_view>
#include "common/gpu_trace.h"
#include "common/types.h"

namespace Common {
// The runtime's fatal stop (crash report and minidump); exits with `status` when none is registered.
[[noreturn]] void StopProcess(const char* what, int status);
} // namespace Common

namespace Common::DeviceLost {

inline constexpr s32 Success = 0;
inline constexpr s32 Timeout = 2;
inline constexpr s32 DeviceLostResult = -4;
inline constexpr int ExitStatus = 25;
inline constexpr u32 MaxSecondaryLines = 4;

enum class Phase : u32 { Healthy, Detected, DiagnosticsCaptured, ShutdownRequested };

enum class Operation : u32 {
    QueueSubmit,
    QueueBindSparse,
    WaitSemaphores,
    GetSemaphoreCounterValue,
    WaitForFences,
    AcquireNextImage,
    QueuePresent,
    DeviceWaitIdle,
    Count,
};

const char* OperationName(Operation operation) noexcept;

enum class FaultStatus : u32 { NotQueried, Unsupported, Failed, Captured, Skipped };

inline constexpr u32 MaxFaultAddresses = 8;
inline constexpr u32 MaxVendorFaults = 4;

struct FaultInfo {
    FaultStatus status = FaultStatus::NotQueried;
    s32 result = 0;
    char description[256]{};
    // As the driver reported them; only the first entries are kept.
    u32 address_count = 0;
    u32 vendor_count = 0;
    u64 vendor_binary_size = 0;
    struct Address {
        u32 type;
        u64 address;
        u64 precision;
    } addresses[MaxFaultAddresses]{};
    struct Vendor {
        u64 code;
        u64 data;
        char description[128];
    } vendors[MaxVendorFaults]{};
};

const char* FaultAddressTypeName(u32 type) noexcept;
// Driver text without line breaks or control characters, always terminated.
void CopyDriverText(char* out, size_t size, const char* text, size_t text_size) noexcept;

struct RootCause {
    Operation operation{};
    const char* site = "";
    s32 result = 0;
    u64 tick = 0;
    GpuTrace::Timeline timeline{};
    u32 thread_id = 0;
    char thread_name[64]{};
    s64 unix_seconds = 0;
    bool simulated = false;
};

struct Hooks {
    void (*print)(const char* line);
    // The GPU trace and device state of the root cause, on the detecting thread.
    void (*capture)(const RootCause& root);
    void (*query_fault)(FaultInfo& info);
    // Ends the process with the crash report; `summary` is its reason.
    void (*shutdown)(const char* summary, int status);
    // Where a thread stays once shutdown is requested; never returns in production.
    void (*park)();
};

class Monitor {
public:
    constexpr explicit Monitor(const Hooks& hooks_) : hooks{hooks_}, query_fault{hooks_.query_fault} {}

    [[nodiscard]] bool IsLost() const noexcept {
        return phase.load(std::memory_order_acquire) != Phase::Healthy;
    }
    [[nodiscard]] Phase CurrentPhase() const noexcept {
        return phase.load(std::memory_order_acquire);
    }

    // VK_ERROR_DEVICE_LOST does not return; any other result is the caller's.
    void OnResult(Operation operation, const char* site, s32 result, u64 tick, GpuTrace::Timeline timeline) {
        if (result == DeviceLostResult) {
            Report(operation, site, result, tick, timeline);
        }
    }
    [[noreturn]] void Report(Operation operation, const char* site, s32 result, u64 tick,
                             GpuTrace::Timeline timeline);
    // A thread that found the device already lost.
    [[noreturn]] void Observe(const char* where);

    void SetFaultQuery(void (*query)(FaultInfo&)) noexcept {
        query_fault.store(query, std::memory_order_release);
    }
    void QueryFaultOnce();

    // ALN_SIMULATE_DEVICE_LOST: the `calls`th call of `operation` reports a device loss without reaching Vulkan.
    void ArmSimulation(Operation operation, u64 calls) noexcept;
    [[nodiscard]] bool Simulate(Operation operation) noexcept {
        if (simulated_operation.load(std::memory_order_relaxed) != u32(operation)) {
            return false;
        }
        if (simulated_calls.fetch_sub(1, std::memory_order_relaxed) != 1) {
            return false;
        }
        simulated_hit.store(true, std::memory_order_relaxed);
        return true;
    }

    // Valid once the root cause is recorded.
    [[nodiscard]] const RootCause& Root() const noexcept {
        return root;
    }
    [[nodiscard]] const FaultInfo& Fault() const noexcept {
        return fault;
    }
    [[nodiscard]] const char* RootLine() const noexcept {
        return root_line;
    }
    [[nodiscard]] const char* Summary() const noexcept {
        return summary;
    }
    [[nodiscard]] u32 SecondaryReports() const noexcept {
        return secondary_reports.load(std::memory_order_relaxed);
    }

private:
    void AdvanceTo(Phase next) noexcept;
    void NoteSecondary(const char* where);
    void WaitForOwner();
    void RequestShutdown();
    [[noreturn]] void Park();
    void Print(const char* line);
    void BuildRootLine();
    void BuildSummary();
    void PrintFault();

    Hooks hooks;
    std::atomic<void (*)(FaultInfo&)> query_fault;
    std::atomic<Phase> phase{Phase::Healthy};
    std::atomic<bool> root_ready{};
    std::atomic<bool> summary_ready{};
    std::atomic<bool> fault_claimed{};
    std::atomic<bool> fault_ready{};
    std::atomic<bool> shutdown_issued{};
    std::atomic<u32> secondary_reports{};
    std::atomic<u32> simulated_operation{u32(Operation::Count)};
    std::atomic<u64> simulated_calls{};
    std::atomic<bool> simulated_hit{};
    RootCause root{};
    FaultInfo fault{};
    char root_line[384]{};
    char summary[1024]{};
};

// The renderer's monitor: stderr, the GPU trace, the runtime's fatal stop.
Monitor& Global() noexcept;

[[nodiscard]] inline bool IsLost() noexcept {
    return Global().IsLost();
}

// "submit:600" arms the 600th queue submission; nothing when unset, empty or unknown.
bool ParseSimulation(std::string_view value, Operation& operation, u64& calls) noexcept;
void ArmSimulationFromEnvironment();

// A Vulkan call that must not reach a lost device. Its own VK_ERROR_DEVICE_LOST does not return.
template <typename Call>
s32 CallWhileHealthy(Monitor& monitor, Operation operation, const char* site, u64 tick,
                     GpuTrace::Timeline timeline, Call&& call) {
    if (monitor.IsLost()) {
        monitor.Observe(OperationName(operation));
    }
    const s32 result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<s32>(call());
    monitor.OnResult(operation, site, result, tick, timeline);
    return result;
}

// A wait with a timeout, repeated until it is no longer a timeout.
template <typename WaitOnce>
s32 WaitWhileHealthy(Monitor& monitor, Operation operation, const char* site, u64 tick,
                     GpuTrace::Timeline timeline, WaitOnce&& wait_once) {
    for (;;) {
        if (monitor.IsLost()) {
            monitor.Observe(OperationName(operation));
        }
        const s32 result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<s32>(wait_once());
        if (result != Timeout) {
            monitor.OnResult(operation, site, result, tick, timeline);
            return result;
        }
    }
}

// The command processor stops taking guest GPU work once the device is lost.
template <typename StopToken>
[[nodiscard]] bool ShouldStop(const Monitor& monitor, const StopToken& stop) noexcept {
    return stop.stop_requested() || monitor.IsLost();
}

} // namespace Common::DeviceLost
