// aln: always-on bounded record of significant GPU operations, printed when the device is lost
// or a GPU assertion fails. Recording is lock-free and allocation-free.
#pragma once
#include <cstddef>
#include "common/types.h"

namespace Common::GpuTrace {

enum class Event : u32 {
    Submit,
    SparseBind,
    BlockingWait,
    ShaderCompile,
    PipelineFirstUse,
    ArenaMigration,
    CopyLayersCoerced,
    GeometryOutputClamp,
    RenderingMismatch,
    NonUniformSubgroupOp,
    AllocationFallback,
    ForeignRecording,
    VulkanFailure,
};

enum class PipelineKind : u64 { Graphics = 1, Compute = 2 };

void Record(Event event, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;

u32 CurrentThreadId() noexcept;
// The thread's description (host threads name themselves), or "unnamed".
void ThreadName(u32 tid, char* out, size_t size) noexcept;

void SetSubmittedTick(u64 tick) noexcept;
void SetCompletedTick(u64 tick) noexcept;
void SetPendingWork(u64 pending_operations, u64 queued_record_chunks) noexcept;
// Recording owner thread at the last submission, how often ownership changed, and how many
// changes were takeovers by the GPU command thread with the draw pipeline idle.
void SetRecordingOwner(u32 owner, u64 changes, u64 takeovers) noexcept;
void SetLastGraphics(u64 vs, u64 gs, u64 ps) noexcept;
void SetLastDispatch(u64 cs, u32 x, u32 y, u32 z) noexcept;
// Where the command being replayed into the command buffer was recorded; null when idle.
void SetReplaySite(const char* file, u32 line) noexcept;

void SetDeviceIdentity(const char* identity) noexcept;
void SetLayerSummary(const char* summary) noexcept;
void SetDispatchSummary(const char* summary) noexcept;
// Called once after a fatal Vulkan result, after the trace (driver fault report).
void SetFaultReporter(void (*reporter)()) noexcept;

// Records a fatal VkResult and prints the trace once.
void VulkanFailure(const char* where, s32 vk_result, u64 tick = 0) noexcept;
void Dump(const char* reason) noexcept;

constexpr u64 PackGroups(u32 x, u32 y, u32 z) noexcept {
    return u64(x & 0x1FFFFF) | (u64(y & 0x1FFFFF) << 21) | (u64(z & 0x1FFFFF) << 42);
}

// First eight bytes of a short name, for stage and site tags in event payloads.
constexpr u64 Tag(const char* text) noexcept {
    u64 value = 0;
    for (u32 i = 0; i < 8 && text[i]; ++i) {
        value |= u64(u8(text[i])) << (i * 8);
    }
    return value;
}

} // namespace Common::GpuTrace
