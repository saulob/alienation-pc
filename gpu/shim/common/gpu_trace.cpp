// aln: see gpu_trace.h.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "aln_win_compat.h"
#include "common/gpu_trace.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Common::GpuTrace {
namespace {

constexpr u32 Capacity = 512;
constexpr s32 DeviceLostResult = -4; // VK_ERROR_DEVICE_LOST

struct Entry {
    std::atomic<u64> seq{};
    std::atomic<u32> event{};
    std::atomic<u32> thread{};
    std::atomic<u64> args[4]{};
};

std::array<Entry, Capacity> ring;
std::atomic<u64> next_seq{1};
std::atomic<u64> submitted_tick{};
std::atomic<u64> completed_tick{};
std::atomic<u64> pending_operations{}, queued_record_chunks{};
std::atomic<u32> recording_owner{};
std::atomic<u64> ownership_changes{}, ownership_takeovers{};
std::atomic<u64> last_vs{}, last_gs{}, last_ps{}, last_cs{};
std::atomic<u64> last_dispatch{};
std::atomic<const char*> replay_file{};
std::atomic<u32> replay_line{};
std::atomic<bool> dumped{};
std::atomic<void (*)()> fault_reporter{};
std::atomic<bool> fault_reported{};

char device_identity[256];
char layer_summary[512];
char dispatch_summary[384];

u32 ThreadId() noexcept {
    static thread_local const u32 tid = static_cast<u32>(gettid());
    return tid;
}

void PrintThreadNames(u64 first, u64 last) noexcept {
    constexpr u32 MaxThreads = 16;
    u32 seen[MaxThreads];
    u32 count = 0;
    for (u64 seq = first; seq <= last && count < MaxThreads; ++seq) {
        const u32 tid = ring[seq % Capacity].thread.load(std::memory_order_relaxed);
        bool known = tid == 0;
        for (u32 i = 0; i < count && !known; ++i) {
            known = seen[i] == tid;
        }
        if (!known) {
            seen[count++] = tid;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        char name[64];
        ThreadName(seen[i], name, sizeof(name));
        std::fprintf(stderr, "GPU trace: thread %u is %s\n", seen[i], name);
    }
}

void CopyBounded(char* dst, size_t size, const char* src) noexcept {
    std::strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

void PrintTag(char* out, u64 tag) noexcept {
    std::memcpy(out, &tag, 8);
    out[8] = '\0';
}

void PrintEntry(u64 seq, Event event, u32 thread, const u64 (&a)[4]) noexcept {
    char tag[9];
    switch (event) {
    case Event::Submit:
        std::fprintf(stderr, "  #%llu tid %u submit tick %llu, %llu waits (first %llu), result %lld\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], (long long)(s32)a[3]);
        break;
    case Event::SparseBind:
        std::fprintf(stderr, "  #%llu tid %u sparse bind memory tick %llu, %llu buffers, result %lld\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (long long)(s32)a[2]);
        break;
    case Event::BlockingWait:
        std::fprintf(stderr, "  #%llu tid %u blocking wait for tick %llu (completed %llu)\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1]);
        break;
    case Event::ShaderCompile:
        PrintTag(tag, a[1]);
        std::fprintf(stderr, "  #%llu tid %u compile %s shader %#018llx, permutation %#llx\n",
                     (unsigned long long)seq, thread, tag, (unsigned long long)a[0],
                     (unsigned long long)a[2]);
        break;
    case Event::PipelineFirstUse: {
        const char* what = "first use of";
        if (a[0] == u64(PipelineKind::Compute)) {
            std::fprintf(stderr,
                         "  #%llu tid %u %s compute pipeline cs %#010llx, groups %llux%llux%llu\n",
                         (unsigned long long)seq, thread, what, (unsigned long long)a[1],
                         (unsigned long long)(a[2] & 0x1FFFFF),
                         (unsigned long long)((a[2] >> 21) & 0x1FFFFF),
                         (unsigned long long)((a[2] >> 42) & 0x1FFFFF));
        } else {
            std::fprintf(stderr,
                         "  #%llu tid %u %s graphics pipeline vs %#010llx gs %#010llx ps %#010llx\n",
                         (unsigned long long)seq, thread, what, (unsigned long long)a[1],
                         (unsigned long long)a[2], (unsigned long long)a[3]);
        }
        break;
    }
    case Event::ArenaMigration:
        std::fprintf(stderr,
                     "  #%llu tid %u buffer arena migration at %#llx, %llu bytes (from %llu + %llu)\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
        break;
    case Event::CopyLayersCoerced:
        std::fprintf(stderr,
                     "  #%llu tid %u image copy layers coerced: source %llu, destination %llu\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1]);
        break;
    case Event::GeometryOutputClamp:
        std::fprintf(stderr,
                     "  #%llu tid %u gs %#010llx output vertices %llu -> %llu (%llu components each)\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
        break;
    case Event::RenderingMismatch:
        std::fprintf(stderr, "  #%llu tid %u unpaired dynamic rendering (kind %llu), corrected\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0]);
        break;
    case Event::NonUniformSubgroupOp:
        PrintTag(tag, a[1]);
        std::fprintf(stderr, "  #%llu tid %u shader %#018llx: %s in non-uniform control flow\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0], tag);
        break;
    case Event::AllocationFallback:
        std::fprintf(stderr, "  #%llu tid %u image %llux%llu allocated %s\n", (unsigned long long)seq,
                     thread, (unsigned long long)a[0], (unsigned long long)a[1],
                     a[2] == 1 ? "over the memory budget" : "in system memory");
        break;
    case Event::ForeignRecording:
        std::fprintf(stderr, "  #%llu tid %u recorded while thread %llu owns recording, at %s:%llu\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[1],
                     a[2] ? reinterpret_cast<const char*>(a[2]) : "?", (unsigned long long)a[3]);
        break;
    case Event::VulkanFailure:
        PrintTag(tag, a[0]);
        std::fprintf(stderr, "  #%llu tid %u %s at %s, result %lld, tick %llu\n",
                     (unsigned long long)seq, thread,
                     s32(a[1]) == DeviceLostResult ? "DEVICE LOST" : "Vulkan failure", tag,
                     (long long)(s32)a[1], (unsigned long long)a[2]);
        break;
    }
}

} // namespace

void Record(Event event, u64 a, u64 b, u64 c, u64 d) noexcept {
    const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = ring[seq % Capacity];
    entry.seq.store(0, std::memory_order_relaxed);
    entry.event.store(u32(event), std::memory_order_relaxed);
    entry.thread.store(ThreadId(), std::memory_order_relaxed);
    entry.args[0].store(a, std::memory_order_relaxed);
    entry.args[1].store(b, std::memory_order_relaxed);
    entry.args[2].store(c, std::memory_order_relaxed);
    entry.args[3].store(d, std::memory_order_relaxed);
    entry.seq.store(seq, std::memory_order_release);
}

u32 CurrentThreadId() noexcept {
    return ThreadId();
}

void ThreadName(u32 tid, char* out, size_t size) noexcept {
    CopyBounded(out, size, "unnamed");
#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (!get_description || tid == 0) {
        return;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) {
        return;
    }
    PWSTR description = nullptr;
    if (SUCCEEDED(get_description(thread, &description)) && description) {
        if (description[0] &&
            WideCharToMultiByte(CP_UTF8, 0, description, -1, out, int(size), nullptr, nullptr) == 0) {
            CopyBounded(out, size, "unnamed");
        }
        LocalFree(description);
    }
    CloseHandle(thread);
#endif
}

void SetSubmittedTick(u64 tick) noexcept {
    submitted_tick.store(tick, std::memory_order_relaxed);
}

void SetCompletedTick(u64 tick) noexcept {
    completed_tick.store(tick, std::memory_order_relaxed);
}

void SetPendingWork(u64 operations, u64 record_chunks) noexcept {
    pending_operations.store(operations, std::memory_order_relaxed);
    queued_record_chunks.store(record_chunks, std::memory_order_relaxed);
}

void SetRecordingOwner(u32 owner, u64 changes, u64 takeovers) noexcept {
    recording_owner.store(owner, std::memory_order_relaxed);
    ownership_changes.store(changes, std::memory_order_relaxed);
    ownership_takeovers.store(takeovers, std::memory_order_relaxed);
}

void SetLastGraphics(u64 vs, u64 gs, u64 ps) noexcept {
    last_vs.store(vs, std::memory_order_relaxed);
    last_gs.store(gs, std::memory_order_relaxed);
    last_ps.store(ps, std::memory_order_relaxed);
}

void SetLastDispatch(u64 cs, u32 x, u32 y, u32 z) noexcept {
    last_cs.store(cs, std::memory_order_relaxed);
    last_dispatch.store(PackGroups(x, y, z), std::memory_order_relaxed);
}

void SetDispatchSummary(const char* summary) noexcept {
    CopyBounded(dispatch_summary, sizeof(dispatch_summary), summary);
}

void SetReplaySite(const char* file, u32 line) noexcept {
    replay_line.store(line, std::memory_order_relaxed);
    replay_file.store(file, std::memory_order_relaxed);
}

void SetDeviceIdentity(const char* identity) noexcept {
    CopyBounded(device_identity, sizeof(device_identity), identity);
}

void SetLayerSummary(const char* summary) noexcept {
    CopyBounded(layer_summary, sizeof(layer_summary), summary);
}

void SetFaultReporter(void (*reporter)()) noexcept {
    fault_reporter.store(reporter, std::memory_order_release);
}

void VulkanFailure(const char* where, s32 vk_result, u64 tick) noexcept {
    Record(Event::VulkanFailure, Tag(where), u64(u32(vk_result)), tick);
    Dump(vk_result == DeviceLostResult ? "Vulkan device lost" : "Vulkan operation failed");
    if (auto* reporter = fault_reporter.load(std::memory_order_acquire);
        reporter && !fault_reported.exchange(true)) {
        reporter();
    }
}

void Dump(const char* reason) noexcept {
    if (dumped.exchange(true)) {
        return;
    }
    const u64 last = next_seq.load(std::memory_order_acquire) - 1;
    const u64 dims = last_dispatch.load(std::memory_order_relaxed);
    char detecting[64];
    ThreadName(ThreadId(), detecting, sizeof(detecting));
    std::fprintf(stderr, "GPU trace: %s, detected on thread %u (%s)\n", reason, ThreadId(),
                 detecting);
    std::fprintf(stderr, "GPU trace: device %s\n", device_identity[0] ? device_identity : "unknown");
    std::fprintf(stderr, "GPU trace: Vulkan layers %s\n",
                 layer_summary[0] ? layer_summary : "unknown");
    std::fprintf(stderr, "GPU trace: Vulkan dispatch %s\n",
                 dispatch_summary[0] ? dispatch_summary : "unknown");
    std::fprintf(stderr, "GPU trace: last submitted tick %llu, last completed tick %llu\n",
                 (unsigned long long)submitted_tick.load(std::memory_order_relaxed),
                 (unsigned long long)completed_tick.load(std::memory_order_relaxed));
    std::fprintf(stderr, "GPU trace: at the last submission %llu operations waited for GPU ticks, "
                         "%llu recorded chunks were queued\n",
                 (unsigned long long)pending_operations.load(std::memory_order_relaxed),
                 (unsigned long long)queued_record_chunks.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "GPU trace: last recorded draw vs %#010llx gs %#010llx ps %#010llx; dispatch cs "
                 "%#010llx groups %llux%llux%llu\n",
                 (unsigned long long)last_vs.load(std::memory_order_relaxed),
                 (unsigned long long)last_gs.load(std::memory_order_relaxed),
                 (unsigned long long)last_ps.load(std::memory_order_relaxed),
                 (unsigned long long)last_cs.load(std::memory_order_relaxed),
                 (unsigned long long)(dims & 0x1FFFFF), (unsigned long long)((dims >> 21) & 0x1FFFFF),
                 (unsigned long long)((dims >> 42) & 0x1FFFFF));
    if (const char* file = replay_file.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "GPU trace: command buffer replay was inside a command recorded at %s:%u\n",
                     file, replay_line.load(std::memory_order_relaxed));
    } else {
        std::fprintf(stderr, "GPU trace: no recorded command was being replayed\n");
    }
    const u64 first = last >= Capacity ? last - Capacity + 1 : 1;
    {
        char owner[64];
        const u32 owner_tid = recording_owner.load(std::memory_order_relaxed);
        ThreadName(owner_tid, owner, sizeof(owner));
        std::fprintf(stderr,
                     "GPU trace: recording owner at the last submission thread %u (%s), %llu ownership "
                     "changes, %llu takeovers\n",
                     owner_tid, owner, (unsigned long long)ownership_changes.load(std::memory_order_relaxed),
                     (unsigned long long)ownership_takeovers.load(std::memory_order_relaxed));
    }
    PrintThreadNames(first, last);
    std::fprintf(stderr, "GPU trace: events %llu..%llu (oldest first)\n", (unsigned long long)first,
                 (unsigned long long)last);
    for (u64 seq = first; seq <= last; ++seq) {
        const Entry& entry = ring[seq % Capacity];
        if (entry.seq.load(std::memory_order_acquire) != seq) {
            continue;
        }
        const u64 args[4] = {entry.args[0].load(std::memory_order_relaxed),
                             entry.args[1].load(std::memory_order_relaxed),
                             entry.args[2].load(std::memory_order_relaxed),
                             entry.args[3].load(std::memory_order_relaxed)};
        PrintEntry(seq, Event(entry.event.load(std::memory_order_relaxed)),
                   entry.thread.load(std::memory_order_relaxed), args);
    }
    std::fflush(stderr);
}

} // namespace Common::GpuTrace
