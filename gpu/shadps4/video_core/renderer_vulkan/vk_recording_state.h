// SPDX-License-Identifier: GPL-2.0-or-later
// aln: replay-side state of the deferred command recording (vk_scheduler.h).

#pragma once

#include <atomic>
#include <utility>
#include "common/types.h"

namespace Vulkan {

/// Whether the command buffer being replayed has an open dynamic rendering instance. Only the
/// thread replaying into the command buffer uses it.
class RenderingScope {
public:
    /// True when an instance is still open and must be ended before this begin.
    bool Begin() noexcept {
        return std::exchange(open, true);
    }

    /// True when an instance is open, i.e. the end is valid.
    bool End() noexcept {
        return std::exchange(open, false);
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        return open;
    }

    void Reset() noexcept {
        open = false;
    }

private:
    bool open = false;
};

/// The one thread that may add commands to the current recording chunk: the draw recording
/// thread while it runs packets, the GPU command thread after it drained the draw pipeline.
class RecordingOwner {
public:
    void Claim(u32 tid) noexcept {
        if (owner.exchange(tid, std::memory_order_relaxed) != tid) {
            changes.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// True when `tid` may record. The first thread to record claims an unowned scheduler.
    bool Allows(u32 tid) noexcept {
        u32 current = owner.load(std::memory_order_relaxed);
        if (current == tid) {
            return true;
        }
        if (current == 0 && owner.compare_exchange_strong(current, tid, std::memory_order_relaxed)) {
            changes.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return current == tid;
    }

    [[nodiscard]] u32 Owner() const noexcept {
        return owner.load(std::memory_order_relaxed);
    }

    [[nodiscard]] u64 Changes() const noexcept {
        return changes.load(std::memory_order_relaxed);
    }

private:
    std::atomic<u32> owner{0};
    std::atomic<u64> changes{0};
};

} // namespace Vulkan
