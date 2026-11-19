// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include "common/device_lost.h"
#include "common/gpu_trace.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// Ticks are handed out before they are submitted, so a payload at or past the next one was never
/// signalled: drivers report such values once the device is lost.
[[nodiscard]] constexpr bool IsPayloadNeverSignalled(u64 payload, u64 next_tick) noexcept {
    return payload >= next_tick;
}

struct TimelineTicks {
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
};

/// The known GPU tick only takes values the device reported, so nothing counts as completed on a lost device.
/// `read_counter(u64&)` and `fail(site, result)` stand for the Vulkan query and the caller's fatal path.
template <typename ReadCounter, typename Fail>
void RefreshKnownTick(TimelineTicks& ticks, Common::DeviceLost::Monitor& monitor,
                      Common::GpuTrace::Timeline timeline, ReadCounter&& read_counter, Fail&& fail) {
    using namespace Common::DeviceLost;
    u64 known{};
    u64 counter{};
    do {
        known = ticks.gpu_tick.load(std::memory_order_acquire);
        const s32 result =
            CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, "semread", known, timeline,
                             [&] { return read_counter(counter); });
        if (result != Success) {
            fail("semread", result, known);
        }
        if (counter < known) {
            return;
        }
        if (IsPayloadNeverSignalled(counter, ticks.current_tick.load(std::memory_order_acquire))) {
            monitor.Report(Operation::GetSemaphoreCounterValue, "semread", DeviceLostResult, known,
                           timeline);
        }
        Common::GpuTrace::SetCompletedTick(timeline, counter);
    } while (!ticks.gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
                                                   std::memory_order_relaxed));
}

/// Returns once the device has reached `tick`. `wait_once()` is one bounded wait for it.
template <typename WaitOnce, typename ReadCounter, typename Fail>
void WaitForTick(TimelineTicks& ticks, Common::DeviceLost::Monitor& monitor,
                 Common::GpuTrace::Timeline timeline, u64 tick, WaitOnce&& wait_once,
                 ReadCounter&& read_counter, Fail&& fail) {
    using namespace Common::DeviceLost;
    if (ticks.gpu_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    RefreshKnownTick(ticks, monitor, timeline, read_counter, fail);
    if (ticks.gpu_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    Common::GpuTrace::Record(Common::GpuTrace::Event::BlockingWait, tick,
                             ticks.gpu_tick.load(std::memory_order_relaxed), u64(timeline));
    const s32 result =
        WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", tick, timeline, wait_once);
    if (result != Success) {
        fail("semwait", result, tick);
    }
    RefreshKnownTick(ticks, monitor, timeline, read_counter, fail);
}

class Semaphore {
public:
    explicit Semaphore(const Instance& instance_,
                       Common::GpuTrace::Timeline timeline_ = Common::GpuTrace::Timeline::Draw);
    ~Semaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return ticks.current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return ticks.gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return ticks.current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    [[nodiscard]] Common::GpuTrace::Timeline GetTimeline() const noexcept {
        return timeline;
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

protected:
    vk::Result ReadCounter(u64& counter) const;
    [[noreturn]] void Fail(const char* site, s32 result, u64 tick) const;

    const Instance& instance;
    Common::GpuTrace::Timeline timeline;
    vk::UniqueSemaphore semaphore; ///< Timeline semaphore.
    TimelineTicks ticks;
};

} // namespace Vulkan
