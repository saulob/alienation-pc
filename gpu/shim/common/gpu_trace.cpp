// aln: see gpu_trace.h.
#include <algorithm>
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
constexpr u32 NumTimelines = u32(Timeline::Count);
std::array<std::atomic<u64>, NumTimelines> submitted_ticks{};
std::array<std::atomic<u64>, NumTimelines> completed_ticks{};

const char* TimelineName(u64 timeline) noexcept {
    switch (Timeline(timeline)) {
    case Timeline::Draw:
        return "draw";
    case Timeline::Present:
        return "present";
    case Timeline::Flip:
        return "flip";
    case Timeline::SparseBind:
        return "sparse bind";
    default:
        return "unknown";
    }
}
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

constexpr u32 ComputePerSet = 256;
constexpr u32 GraphicsPerSet = 64;
constexpr u32 GraphicsPrinted = 16;
constexpr u32 DispatchesPerSet = 32;
constexpr u32 KeptSets = 4;
constexpr u32 TrackedHeaps = 16;

struct GraphicsUse {
    std::atomic<u64> vs{}, gs{}, ps{};
};

struct DispatchUse {
    std::atomic<u64> cs{}, groups{};
};

// Every distinct compute shader of a submission is kept: a hung dispatch is usually one of them.
struct ShaderSet {
    std::atomic<u64> tick{};
    std::atomic<u32> compute_count{};
    std::atomic<u32> graphics_count{};
    std::atomic<u64> dispatch_count{};
    std::array<std::atomic<u64>, ComputePerSet> compute{};
    std::array<GraphicsUse, GraphicsPerSet> graphics{};
    std::array<DispatchUse, DispatchesPerSet> dispatches{};
};

ShaderSet recording_set;
std::array<ShaderSet, KeptSets> submitted_sets;
std::atomic<u64> shader_generation{1};
std::array<std::atomic<bool>, TrackedHeaps> heap_over{};
std::array<std::atomic<bool>, TrackedHeaps> heap_reported{};

template <typename T>
void CopyRelaxed(std::atomic<T>& to, const std::atomic<T>& from) noexcept {
    to.store(from.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void KeepRecordedShaders(u64 tick) noexcept {
    ShaderSet& set = submitted_sets[tick % KeptSets];
    set.tick.store(0, std::memory_order_relaxed);
    const u32 compute = recording_set.compute_count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < std::min(compute, ComputePerSet); ++i) {
        CopyRelaxed(set.compute[i], recording_set.compute[i]);
    }
    const u32 graphics = recording_set.graphics_count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < std::min(graphics, GraphicsPerSet); ++i) {
        CopyRelaxed(set.graphics[i].vs, recording_set.graphics[i].vs);
        CopyRelaxed(set.graphics[i].gs, recording_set.graphics[i].gs);
        CopyRelaxed(set.graphics[i].ps, recording_set.graphics[i].ps);
    }
    for (u32 i = 0; i < DispatchesPerSet; ++i) {
        CopyRelaxed(set.dispatches[i].cs, recording_set.dispatches[i].cs);
        CopyRelaxed(set.dispatches[i].groups, recording_set.dispatches[i].groups);
    }
    set.compute_count.store(compute, std::memory_order_relaxed);
    set.graphics_count.store(graphics, std::memory_order_relaxed);
    CopyRelaxed(set.dispatch_count, recording_set.dispatch_count);
    set.tick.store(tick, std::memory_order_release);
    recording_set.compute_count.store(0, std::memory_order_relaxed);
    recording_set.graphics_count.store(0, std::memory_order_relaxed);
    recording_set.dispatch_count.store(0, std::memory_order_relaxed);
    shader_generation.fetch_add(1, std::memory_order_relaxed);
}

void PrintGroups(char* out, size_t size, u64 groups) noexcept {
    std::snprintf(out, size, "%llux%llux%llu", (unsigned long long)(groups & 0x1FFFFF),
                  (unsigned long long)((groups >> 21) & 0x1FFFFF),
                  (unsigned long long)((groups >> 42) & 0x1FFFFF));
}

void PrintShaderSet(const ShaderSet& set) noexcept {
    const u32 compute = set.compute_count.load(std::memory_order_relaxed);
    const u32 compute_kept = std::min(compute, ComputePerSet);
    for (u32 i = 0; i < compute_kept; i += 8) {
        char line[160];
        int used = std::snprintf(line, sizeof(line), "GPU trace:   cs");
        for (u32 j = i; j < std::min(i + 8, compute_kept); ++j) {
            used += std::snprintf(line + used, sizeof(line) - size_t(used), " %#010llx",
                                  (unsigned long long)set.compute[j].load(std::memory_order_relaxed));
        }
        std::fprintf(stderr, "%s\n", line);
    }
    if (compute > compute_kept) {
        std::fprintf(stderr, "GPU trace:   and %u more compute pipelines\n", compute - compute_kept);
    }
    const u32 graphics = set.graphics_count.load(std::memory_order_relaxed);
    const u32 graphics_shown = std::min(graphics, GraphicsPrinted);
    for (u32 i = 0; i < graphics_shown; ++i) {
        const GraphicsUse& use = set.graphics[i];
        std::fprintf(stderr, "GPU trace:   vs %#010llx gs %#010llx ps %#010llx\n",
                     (unsigned long long)use.vs.load(std::memory_order_relaxed),
                     (unsigned long long)use.gs.load(std::memory_order_relaxed),
                     (unsigned long long)use.ps.load(std::memory_order_relaxed));
    }
    if (graphics > graphics_shown) {
        std::fprintf(stderr, "GPU trace:   and %u more graphics pipelines\n", graphics - graphics_shown);
    }
    const u64 dispatches = set.dispatch_count.load(std::memory_order_relaxed);
    if (dispatches == 0) {
        return;
    }
    const u64 first = dispatches > DispatchesPerSet ? dispatches - DispatchesPerSet : 0;
    std::fprintf(stderr, "GPU trace:   dispatches %llu..%llu of %llu (oldest first):\n",
                 (unsigned long long)first + 1, (unsigned long long)dispatches,
                 (unsigned long long)dispatches);
    for (u64 i = first; i < dispatches; i += 4) {
        char line[192];
        int used = std::snprintf(line, sizeof(line), "GPU trace:    ");
        for (u64 j = i; j < std::min(i + 4, dispatches); ++j) {
            const DispatchUse& use = set.dispatches[j % DispatchesPerSet];
            char groups[48];
            PrintGroups(groups, sizeof(groups), use.groups.load(std::memory_order_relaxed));
            used += std::snprintf(line + used, sizeof(line) - size_t(used), " cs %#010llx %s",
                                  (unsigned long long)use.cs.load(std::memory_order_relaxed), groups);
        }
        std::fprintf(stderr, "%s\n", line);
    }
}

u32 PipelineCount(const ShaderSet& set) noexcept {
    return set.compute_count.load(std::memory_order_relaxed) +
           set.graphics_count.load(std::memory_order_relaxed);
}

void PrintUnfinishedShaders(u64 submitted, u64 completed) noexcept {
    std::array<u64, KeptSets> ticks{};
    u32 found = 0;
    for (const ShaderSet& set : submitted_sets) {
        const u64 tick = set.tick.load(std::memory_order_acquire);
        if (tick > completed && tick <= submitted) {
            ticks[found++] = tick;
        }
    }
    std::sort(ticks.begin(), ticks.begin() + found);
    for (u32 i = 0; i < found; ++i) {
        const ShaderSet& set = submitted_sets[ticks[i] % KeptSets];
        std::fprintf(stderr,
                     "GPU trace: tick %llu was not completed; it recorded %u pipelines (%u compute, %u "
                     "graphics), %llu dispatches\n",
                     (unsigned long long)ticks[i], PipelineCount(set),
                     set.compute_count.load(std::memory_order_relaxed),
                     set.graphics_count.load(std::memory_order_relaxed),
                     (unsigned long long)set.dispatch_count.load(std::memory_order_relaxed));
        PrintShaderSet(set);
    }
    std::fprintf(stderr, "GPU trace: %u pipelines recorded since the last submission\n",
                 PipelineCount(recording_set));
    PrintShaderSet(recording_set);
}

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
        std::fprintf(stderr,
                     "  #%llu tid %u %s submit tick %llu, %llu waits (first %llu), result %lld\n",
                     (unsigned long long)seq, thread, TimelineName(a[3] >> 32), (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], (long long)(s32)a[3]);
        break;
    case Event::SparseBind:
        std::fprintf(stderr, "  #%llu tid %u sparse bind memory tick %llu, %llu buffers, result %lld\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (long long)(s32)a[2]);
        break;
    case Event::BlockingWait:
        std::fprintf(stderr, "  #%llu tid %u blocking wait for %s tick %llu (completed %llu)\n",
                     (unsigned long long)seq, thread, TimelineName(a[2]), (unsigned long long)a[0],
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
    case Event::ArenaCreated:
        std::fprintf(stderr,
                     "  #%llu tid %u buffer arena at %#llx, %llu bytes, aliasing %llu resident ranges\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2]);
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
        if (a[2] == u64(Fallback::BufferOverBudget)) {
            PrintTag(tag, a[1]);
            std::fprintf(stderr, "  #%llu tid %u %s buffer of %llu MiB allocated over the memory budget\n",
                         (unsigned long long)seq, thread, tag, (unsigned long long)(a[0] >> 20));
            break;
        }
        if (a[2] == u64(Fallback::SparseBackingInSystemMemory)) {
            std::fprintf(stderr, "  #%llu tid %u buffer arena backing of %llu KiB in memory type %llu\n",
                         (unsigned long long)seq, thread, (unsigned long long)(a[0] >> 10),
                         (unsigned long long)a[1]);
            break;
        }
        std::fprintf(stderr, "  #%llu tid %u image %llux%llu allocated %s\n", (unsigned long long)seq,
                     thread, (unsigned long long)a[0], (unsigned long long)a[1],
                     a[2] == u64(Fallback::ImageOverBudget) ? "over the memory budget"
                                                            : "in system memory");
        break;
    case Event::HeapBudget:
        std::fprintf(stderr, "  #%llu tid %u heap %llu %s: %llu MiB used of %llu MiB budget\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[0],
                     a[3] ? "went over its budget" : "is back within its budget",
                     (unsigned long long)(a[1] >> 20), (unsigned long long)(a[2] >> 20));
        break;
    case Event::ForeignRecording:
        std::fprintf(stderr, "  #%llu tid %u recorded while thread %llu owns recording, at %s:%llu\n",
                     (unsigned long long)seq, thread, (unsigned long long)a[1],
                     a[2] ? reinterpret_cast<const char*>(a[2]) : "?", (unsigned long long)a[3]);
        break;
    case Event::VulkanFailure:
        PrintTag(tag, a[0]);
        std::fprintf(stderr, "  #%llu tid %u %s at %s, result %lld, %s tick %llu\n",
                     (unsigned long long)seq, thread,
                     s32(a[1]) == DeviceLostResult ? "DEVICE LOST" : "Vulkan failure", tag,
                     (long long)(s32)a[1], TimelineName(a[3]), (unsigned long long)a[2]);
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

u64 ShaderGeneration() noexcept {
    return shader_generation.load(std::memory_order_relaxed);
}

void NoteShaders(PipelineKind kind, u64 first, u64 second, u64 third) noexcept {
    if (kind == PipelineKind::Compute) {
        const u32 index = recording_set.compute_count.fetch_add(1, std::memory_order_relaxed);
        if (index < ComputePerSet) {
            recording_set.compute[index].store(first, std::memory_order_relaxed);
        }
        return;
    }
    const u32 index = recording_set.graphics_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= GraphicsPerSet) {
        return;
    }
    GraphicsUse& use = recording_set.graphics[index];
    use.vs.store(first, std::memory_order_relaxed);
    use.gs.store(second, std::memory_order_relaxed);
    use.ps.store(third, std::memory_order_relaxed);
}

bool NoteHeapBudget(u32 heap, u64 used, u64 budget) noexcept {
    if (heap >= TrackedHeaps || budget == 0) {
        return false;
    }
    // Back within budget only below 90%, so a heap hovering at its budget does not flood the ring.
    const bool was_over = heap_over[heap].load(std::memory_order_relaxed);
    const bool over = was_over ? used > budget - budget / 10 : used > budget;
    if (over == was_over) {
        return false;
    }
    heap_over[heap].store(over, std::memory_order_relaxed);
    Record(Event::HeapBudget, heap, used, budget, over);
    return over && !heap_reported[heap].exchange(true, std::memory_order_relaxed);
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

void SetSubmittedTick(Timeline timeline, u64 tick) noexcept {
    if (timeline == Timeline::Draw) {
        KeepRecordedShaders(tick);
    }
    submitted_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);
}

void SetCompletedTick(Timeline timeline, u64 tick) noexcept {
    completed_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);
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
    const u64 index = recording_set.dispatch_count.fetch_add(1, std::memory_order_relaxed);
    DispatchUse& use = recording_set.dispatches[index % DispatchesPerSet];
    use.cs.store(cs, std::memory_order_relaxed);
    use.groups.store(PackGroups(x, y, z), std::memory_order_relaxed);
}

void SetDispatchSummary(const char* summary) noexcept {
    CopyBounded(dispatch_summary, sizeof(dispatch_summary), summary);
}

void SetReplaySite(const char* file, u32 line) noexcept {
    replay_line.store(line, std::memory_order_relaxed);
    replay_file.store(file, std::memory_order_relaxed);
}

const char* TimelineName(Timeline timeline) noexcept {
    return TimelineName(u64(timeline));
}

void SetDeviceIdentity(const char* identity) noexcept {
    CopyBounded(device_identity, sizeof(device_identity), identity);
}

const char* DeviceIdentity() noexcept {
    return device_identity;
}

void SetLayerSummary(const char* summary) noexcept {
    CopyBounded(layer_summary, sizeof(layer_summary), summary);
}

void SetFaultReporter(void (*reporter)()) noexcept {
    fault_reporter.store(reporter, std::memory_order_release);
}

void VulkanFailure(const char* where, s32 vk_result, u64 tick, Timeline timeline) noexcept {
    Record(Event::VulkanFailure, Tag(where), u64(u32(vk_result)), tick, u64(timeline));
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
    for (u32 timeline = 0; timeline < NumTimelines; ++timeline) {
        const u64 submitted = submitted_ticks[timeline].load(std::memory_order_relaxed);
        if (submitted == 0 && timeline != u32(Timeline::Draw)) {
            continue;
        }
        std::fprintf(stderr,
                     "GPU trace: %s timeline: last submitted tick %llu, last completed tick %llu\n",
                     TimelineName(timeline), (unsigned long long)submitted,
                     (unsigned long long)completed_ticks[timeline].load(std::memory_order_relaxed));
    }
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
    PrintUnfinishedShaders(submitted_ticks[u32(Timeline::Draw)].load(std::memory_order_relaxed),
                           completed_ticks[u32(Timeline::Draw)].load(std::memory_order_relaxed));
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
