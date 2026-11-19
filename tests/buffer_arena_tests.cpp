// Buffer arenas under the report's guest memory layout (ALN-4KPCU7): arena extents against the
// driver's buffer size limit, residency seen by every arena that aliases it, barrier tracking
// across aliasing arenas and a timeline payload past every submission. Production helpers only:
// no Vulkan device.
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "video_core/buffer_cache/arena_map.h"
#include "video_core/buffer_cache/backing_memory.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using VideoCore::ArenaMap;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                      \
        }                                                                                          \
    } while (0)

// AMD's Windows driver: 2 GiB buffers, so 1 GiB arena pages; 64 KiB sparse blocks.
constexpr u64 kBlockShift = 16;
constexpr u64 kPageBits = 30;
constexpr u64 kMaxBufferSize = u64{2} << 30;
constexpr u32 kBlocksPerPageShift = kPageBits - kBlockShift;
constexpr u64 kNumPages = u64{1} << (40 - kPageBits);

// The report build's direct memory: one GPU-visible block from 0x10'0000'0000 crossing four
// arena page boundaries at 1 GiB pages.
constexpr u64 kGuestFirst = 0x10'0000'0000;
constexpr u64 kGuestEnd = 0x11'2800'0000;

struct Arena {
    u64 first_block;
    u64 end_block;
    std::vector<std::pair<u64, u64>> binds;
};

// The arena bookkeeping BufferCache keeps: arenas created on first use of their page, each
// receiving the residency of every range it covers.
struct Cache {
    ArenaMap<Arena*> map;
    std::vector<std::unique_ptr<Arena>> arenas;
    std::set<std::pair<u64, u64>> resident;

    Cache() {
        map.Configure(kNumPages, kBlocksPerPageShift);
    }

    Arena* Obtain(u64 address, u64 size) {
        const u64 first = address >> kBlockShift;
        const u64 last = (address + size - 1) >> kBlockShift;
        const u64 page = map.PageFor(first, last);
        Arena* arena = map.At(page);
        if (!arena) {
            arenas.push_back(std::make_unique<Arena>(Arena{map.FirstBlock(page), map.EndBlock(page), {}}));
            arena = arenas.back().get();
            map.Set(page, arena);
            for (const auto& [start, end] : resident) {
                if (start < arena->end_block && end > arena->first_block) {
                    arena->binds.emplace_back(std::max(start, arena->first_block), std::min(end, arena->end_block));
                }
            }
        }
        MakeResident(first, last + 1);
        return arena;
    }

    void MakeResident(u64 first, u64 end) {
        for (u64 block = first; block < end; ++block) {
            if (IsResident(block)) {
                continue;
            }
            resident.emplace(block, block + 1);
            map.ForEachCovering(block, block + 1, [](Arena* covering, u64 start, u64 stop) {
                covering->binds.emplace_back(start, stop);
            });
        }
    }

    bool IsResident(u64 block) const {
        return resident.contains({block, block + 1});
    }

    static bool IsBound(const Arena& arena, u64 block) {
        for (const auto& [start, end] : arena.binds) {
            if (block >= start && block < end) {
                return true;
            }
        }
        return false;
    }
};

u64 ArenaBytes(const Arena& arena) {
    return (arena.end_block - arena.first_block) << kBlockShift;
}

bool Contains(const Arena& arena, u64 address, u64 size) {
    return (address >> kBlockShift) >= arena.first_block &&
           ((address + size - 1) >> kBlockShift) < arena.end_block;
}

// Buffers straddling each 1 GiB boundary of the report's direct memory, in the order a chain of
// migrations grew one arena to 3, 4 and 5 GiB.
void TestReportBoundariesStayWithinBufferLimit() {
    Cache cache;
    const u64 boundaries[] = {0x10'4000'0000, 0x10'8000'0000, 0x10'C000'0000, 0x11'0000'0000};
    for (const u64 boundary : boundaries) {
        const u64 address = boundary - 0x8000;
        const Arena* arena = cache.Obtain(address, 0x10000);
        CHECK(ArenaBytes(*arena) <= kMaxBufferSize);
        CHECK(Contains(*arena, address, 0x10000));
    }
    for (auto it = std::rbegin(boundaries); it != std::rend(boundaries); ++it) {
        const Arena* arena = cache.Obtain(*it - 0x40, 0x80);
        CHECK(ArenaBytes(*arena) <= kMaxBufferSize);
        CHECK(Contains(*arena, *it - 0x40, 0x80));
    }
    CHECK(cache.arenas.size() == 4);
    for (const auto& arena : cache.arenas) {
        CHECK(ArenaBytes(*arena) == kMaxBufferSize);
    }
}

// The same guest page reached through either arena covering it reads the same resident memory.
void TestResidencyReachesEveryAliasingArena() {
    Cache cache;
    const u64 page = u64{1} << kPageBits;
    Arena* low = cache.Obtain(0x10'4000'0000 - 0x8000, 0x10000);
    Arena* high = cache.Obtain(0x10'4000'0000 + 0x100000, 0x1000);
    CHECK(low != high);
    cache.Obtain(0x10'4000'0000 + 0x200000, 0x30000);
    for (const auto& [start, end] : cache.resident) {
        for (const auto& arena : cache.arenas) {
            if (start >= arena->first_block && start < arena->end_block) {
                CHECK(Cache::IsBound(*arena, start));
            }
        }
    }
    // A page that only becomes an arena's second page later still sees memory bound before.
    const u64 before = (0x10'C000'0000 + 0x40000) >> kBlockShift;
    cache.Obtain(0x10'C000'0000 + 0x40000, 0x100);
    const Arena* straddling = cache.Obtain(0x10'C000'0000 - 0x100, 0x200);
    CHECK(Cache::IsBound(*straddling, before));
    CHECK(ArenaBytes(*straddling) == 2 * page);
}

void TestCoveringArenasAreClipped() {
    ArenaMap<Arena*> map;
    map.Configure(kNumPages, kBlocksPerPageShift);
    Arena a{}, b{}, c{};
    const u64 blocks = u64{1} << kBlocksPerPageShift;
    map.Set(3, &a);
    map.Set(4, &b);
    map.Set(5, &c);
    std::vector<std::tuple<Arena*, u64, u64>> seen;
    map.ForEachCovering(5 * blocks - 2, 5 * blocks + 3, [&](Arena* arena, u64 start, u64 end) {
        seen.emplace_back(arena, start, end);
    });
    CHECK(seen.size() == 3);
    CHECK(seen[0] == std::make_tuple(&a, 5 * blocks - 2, 5 * blocks));
    CHECK(seen[1] == std::make_tuple(&b, 5 * blocks - 2, 5 * blocks + 3));
    CHECK(seen[2] == std::make_tuple(&c, 5 * blocks, 5 * blocks + 3));
    seen.clear();
    map.Set(0, &a);
    map.ForEachCovering(0, 1, [&](Arena* arena, u64 start, u64 end) { seen.emplace_back(arena, start, end); });
    CHECK(seen.size() == 1);
    seen.clear();
    map.Set(kNumPages - 1, &c);
    map.ForEachCovering((kNumPages - 1) * blocks, kNumPages * blocks,
                        [&](Arena* arena, u64 start, u64 end) { seen.emplace_back(arena, start, end); });
    CHECK(seen.size() == 1);
    CHECK(map.FirstBlock(4) == 4 * blocks && map.EndBlock(4) == 6 * blocks);
}

// Explosion-heavy play: thousands of buffers anywhere in the direct memory, many straddling.
void TestRandomBuffersStayBounded() {
    Cache cache;
    std::mt19937_64 rng{0x4C9C07};
    std::uniform_int_distribution<u64> where(kGuestFirst, kGuestEnd - 0x100000);
    std::uniform_int_distribution<u64> size(4, 0x40000);
    const u64 boundaries[] = {0x10'4000'0000, 0x10'8000'0000, 0x10'C000'0000, 0x11'0000'0000};
    u64 largest = 0;
    u64 outside = 0;
    for (int i = 0; i < 20000; ++i) {
        u64 address = where(rng) & ~u64{3};
        if (i % 4 == 0) {
            address = boundaries[i / 4 % 4] - (size(rng) & ~u64{3});
        }
        const u64 bytes = size(rng);
        const Arena* arena = cache.Obtain(address, bytes);
        largest = std::max(largest, ArenaBytes(*arena));
        outside += Contains(*arena, address, bytes) ? 0 : 1;
    }
    std::printf("random buffers: %zu arenas, largest %llu MiB, %llu outside their arena\n", cache.arenas.size(),
                (unsigned long long)(largest >> 20), (unsigned long long)outside);
    CHECK(largest <= kMaxBufferSize);
    CHECK(outside == 0);
    CHECK(cache.arenas.size() <= ((kGuestEnd - kGuestFirst) >> kPageBits) + 1);
}

// A write through one arena is a hazard for a read of the same guest bytes through the other.
void TestAliasingArenasShareBarrierTracking() {
    using namespace Vulkan;
    const u64 low_base = 0x10'0000'0000, high_base = 0x10'4000'0000;
    const u64 guest = 0x10'4000'1000;
    BarrierTracker tracker;
    tracker.InsertRange(BufferAccessRange(0x1111, true, low_base, guest - low_base, 64), Access::Write);
    CHECK(tracker.FindRange(BufferAccessRange(0x2222, true, high_base, guest - high_base, 16), Access::Write));
    CHECK(!tracker.FindRange(BufferAccessRange(0x2222, true, high_base, guest - high_base + 64, 16), Access::Write));

    // Keyed by handle, as before, the second arena never sees the first one's write.
    BarrierTracker by_handle;
    by_handle.InsertRange(BufferAccessRange(0x1111, false, low_base, guest - low_base, 64), Access::Write);
    CHECK(!by_handle.FindRange(BufferAccessRange(0x2222, false, high_base, guest - high_base, 16), Access::Write));

    // Ordinary buffers keep their own spaces even at equal offsets.
    BarrierTracker plain;
    plain.InsertRange(BufferAccessRange(0x3333, false, 0, 0x100, 64), Access::Write);
    CHECK(!plain.FindRange(BufferAccessRange(0x4444, false, 0, 0x100, 64), Access::Write));
    CHECK(plain.FindRange(BufferAccessRange(0x3333, false, 0, 0x120, 4), Access::Write));
}

// Issue #9 (6 GB NVIDIA laptop, Linux): the driver does not page device memory out, so a new
// resident range failed with VK_ERROR_OUT_OF_DEVICE_MEMORY and ended the game. The arena now takes
// another memory type it accepts before failing.
vk::PhysicalDeviceMemoryProperties NvidiaLikeMemory() {
    using Flag = vk::MemoryPropertyFlagBits;
    vk::PhysicalDeviceMemoryProperties props{};
    props.memoryTypeCount = 5;
    props.memoryTypes[0] = vk::MemoryType{{}, 1};
    props.memoryTypes[1] = vk::MemoryType{Flag::eDeviceLocal, 0};
    props.memoryTypes[2] = vk::MemoryType{Flag::eHostVisible | Flag::eHostCoherent, 1};
    props.memoryTypes[3] = vk::MemoryType{Flag::eHostVisible | Flag::eHostCoherent | Flag::eHostCached, 1};
    props.memoryTypes[4] = vk::MemoryType{Flag::eDeviceLocal | Flag::eHostVisible | Flag::eHostCoherent, 0};
    return props;
}

void TestBackingFallsBackWhenDeviceMemoryIsFull() {
    using VideoCore::AllocateBacking;
    using VideoCore::BackingMemoryTypes;
    const auto props = NvidiaLikeMemory();
    const std::vector<u32> types = BackingMemoryTypes(props, 0b10111, 1);
    CHECK((types == std::vector<u32>{1, 4, 0, 2}));
    CHECK((BackingMemoryTypes(props, 0b00010, 1) == std::vector<u32>{1}));

    std::vector<u32> tried;
    const auto device_full = [&](u32 type) {
        tried.push_back(type);
        return props.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal
                   ? vk::Result::eErrorOutOfDeviceMemory
                   : vk::Result::eSuccess;
    };
    const auto fallback = AllocateBacking(types, device_full);
    CHECK(fallback.result == vk::Result::eSuccess && fallback.type == 0 && fallback.attempts == 3);
    CHECK((tried == std::vector<u32>{1, 4, 0}));

    tried.clear();
    const auto nothing_fits = AllocateBacking(types, [&](u32 type) {
        tried.push_back(type);
        return vk::Result::eErrorOutOfDeviceMemory;
    });
    CHECK(nothing_fits.result == vk::Result::eErrorOutOfDeviceMemory && nothing_fits.attempts == types.size());
    CHECK(tried.size() == types.size());

    const auto host_failure = AllocateBacking(types, [](u32) { return vk::Result::eErrorOutOfHostMemory; });
    CHECK(host_failure.result == vk::Result::eErrorOutOfHostMemory && host_failure.attempts == 1);

    const auto preferred = AllocateBacking(types, [](u32) { return vk::Result::eSuccess; });
    CHECK(preferred.result == vk::Result::eSuccess && preferred.type == 1 && preferred.attempts == 1);
}

// The report's AMD driver read the draw timeline as UINT64_MAX after the reset.
void TestPayloadPastSubmissionsIsDeviceLoss() {
    using Vulkan::IsPayloadNeverSignalled;
    const u64 next_tick = 327554;
    CHECK(IsPayloadNeverSignalled(std::numeric_limits<u64>::max(), next_tick));
    CHECK(IsPayloadNeverSignalled(next_tick, next_tick));
    CHECK(!IsPayloadNeverSignalled(327553, next_tick));
    CHECK(!IsPayloadNeverSignalled(327551, next_tick));
}

} // namespace

int main() {
    TestReportBoundariesStayWithinBufferLimit();
    TestResidencyReachesEveryAliasingArena();
    TestCoveringArenasAreClipped();
    TestRandomBuffersStayBounded();
    TestAliasingArenasShareBarrierTracking();
    TestPayloadPastSubmissionsIsDeviceLoss();
    TestBackingFallsBackWhenDeviceMemoryIsFull();
    std::printf("buffer arena tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
