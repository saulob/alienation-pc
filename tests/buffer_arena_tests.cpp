// Buffer arenas under the report's guest memory layout (ALN-4KPCU7): arena extents against the
// driver's buffer size limit, residency seen by every arena that aliases it, barrier tracking
// across aliasing arenas and a timeline payload past every submission. Fully resident arenas for
// devices without sparse buffers: layout, migration, BDA entries, stale bindings and readback on
// fake arenas, plus the sources that must agree. Production helpers only: no Vulkan device.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// The vendored interval_set.h compares signed and unsigned sizes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "video_core/buffer_cache/arena_layout.h"
#pragma GCC diagnostic pop
#include "video_core/buffer_cache/arena_map.h"
#include "video_core/buffer_cache/backing_memory.h"
#include "video_core/buffer_cache/texel_image_copies.h"
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

// ---------------------------------------------------------------------------------------------
// Fully resident arenas (no sparse buffers)
// ---------------------------------------------------------------------------------------------

namespace Layout = VideoCore::ArenaLayout;

constexpr u64 MiB = u64{1} << 20;
constexpr u64 GiB = u64{1} << 30;

// The constructor's sparse sizing before the fully resident arenas existed.
u64 OriginalSparsePageBits(u64 max_buffer_size) {
    u64 bits = 32;
    while (bits > 30 && (u64{2} << bits) > max_buffer_size) {
        --bits;
    }
    return bits;
}

u64 OriginalSparseBlockSize(u64 alignment) {
    return Common::AlignUp(std::max<u64>(alignment, 16_KB), alignment);
}

bool IsPow2(u64 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

void TestSparseLayoutUnchanged() {
    for (const u64 max_buffer : {GiB, 2 * GiB, 3 * GiB, 4 * GiB, 8 * GiB, 16 * GiB, u64{1} << 40, ~u64{0}}) {
        for (const u64 max_allocation : {64 * MiB, GiB, 4 * GiB, ~u64{0}}) {
            CHECK(Layout::PageBits(true, max_buffer, max_allocation) == OriginalSparsePageBits(max_buffer));
        }
    }
    CHECK(Layout::PageBits(true, kMaxBufferSize, ~u64{0}) == kPageBits);
    CHECK(Layout::PageBits(true, ~u64{0}, ~u64{0}) == 32);
    for (u64 alignment = 1_KB; alignment <= 2_MB; alignment <<= 1) {
        CHECK(Layout::BlockSize(true, alignment) == OriginalSparseBlockSize(alignment));
    }
    CHECK(Layout::NumPages(32) == 256 && Layout::NumPages(30) == kNumPages);
}

void TestResidentLayoutRespectsDriverLimits() {
    CHECK(Layout::PageBits(false, ~u64{0}, ~u64{0}) == Layout::RESIDENT_PAGE_BITS);
    // maxBufferSize alone.
    CHECK(Layout::PageBits(false, 8 * MiB, ~u64{0}) == 23);
    CHECK(Layout::PageBits(false, 3 * MiB, ~u64{0}) == 21);
    // maxMemoryAllocationSize alone.
    CHECK(Layout::PageBits(false, ~u64{0}, 4 * MiB) == 22);
    CHECK(Layout::PageBits(false, 4 * GiB, 12 * MiB) == 23);
    for (u64 max_buffer = MiB; max_buffer <= 8 * GiB; max_buffer += 3 * MiB + 4096) {
        for (const u64 max_allocation : {MiB, 5 * MiB, 64 * MiB, 2 * GiB, ~u64{0}}) {
            const u64 bits = Layout::PageBits(false, max_buffer, max_allocation);
            const u64 page = u64{1} << bits;
            CHECK(IsPow2(page));
            CHECK(bits >= Layout::MIN_RESIDENT_PAGE_BITS && bits <= Layout::RESIDENT_PAGE_BITS);
            CHECK(page <= max_buffer && page <= max_allocation);
            CHECK(Layout::ResidentPageFits(bits, max_buffer, max_allocation));
        }
    }
    for (u64 alignment = 1; alignment <= 64_KB; alignment <<= 1) {
        CHECK(Layout::BlockSize(false, alignment) == 64_KB);
    }
    CHECK(Layout::BlockSize(false, 256_KB) == 256_KB);
    for (u64 bits = Layout::MIN_RESIDENT_PAGE_BITS; bits <= Layout::RESIDENT_PAGE_BITS; ++bits) {
        const u64 block = Layout::BlockSize(false, 4096);
        CHECK(IsPow2(block) && block <= (u64{1} << bits));
    }
}

// A driver whose limits are below the smallest fully resident page is refused, never wrapped.
void TestImpossibleResidentLimitsAreRefused() {
    CHECK(Layout::PageBits(false, 64_KB, 64_KB) == Layout::MIN_RESIDENT_PAGE_BITS);
    CHECK(!Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, 64_KB, ~u64{0}));
    CHECK(!Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, ~u64{0}, MiB - 1));
    CHECK(!Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, 0, 0));
    CHECK(Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, MiB, MiB));
    CHECK(Layout::ResidentArenaLimit(2 * GiB, 4 * GiB) == 2 * GiB);
    CHECK(Layout::ResidentArenaLimit(~u64{0}, GiB) == GiB);
}

void SetForceEnvironment(const char* value) {
#ifdef _WIN32
    _putenv_s("ALN_FORCE_NON_SPARSE_ARENAS", value ? value : "");
#else
    if (value) {
        setenv("ALN_FORCE_NON_SPARSE_ARENAS", value, 1);
    } else {
        unsetenv("ALN_FORCE_NON_SPARSE_ARENAS");
    }
#endif
}

void TestModeSelection() {
    using Layout::ArenaMode;
    const struct {
        bool supported;
        const char* env;
        ArenaMode expected;
    } cases[] = {
        {true, nullptr, ArenaMode::Sparse},
        {true, "", ArenaMode::Sparse},
        {true, "0", ArenaMode::Sparse},
        {true, "1", ArenaMode::FullyResidentForced},
        {true, "true", ArenaMode::FullyResidentForced},
        {true, "yes", ArenaMode::FullyResidentForced},
        {true, "00", ArenaMode::FullyResidentForced},
        {true, " ", ArenaMode::FullyResidentForced},
        {false, nullptr, ArenaMode::FullyResidentUnsupported},
        {false, "", ArenaMode::FullyResidentUnsupported},
        {false, "0", ArenaMode::FullyResidentUnsupported},
        {false, "1", ArenaMode::FullyResidentForced},
    };
    for (const auto& c : cases) {
        CHECK(Layout::SelectArenaMode(c.supported, c.env) == c.expected);
        CHECK(Layout::IsFullyResident(c.expected) == (c.expected != ArenaMode::Sparse));
    }

    // The helper BufferCache calls reads the real variable.
    const char* saved = std::getenv("ALN_FORCE_NON_SPARSE_ARENAS");
    const std::string saved_copy = saved ? saved : "";
    for (const auto& c : cases) {
        SetForceEnvironment(c.env);
        const char* seen = std::getenv("ALN_FORCE_NON_SPARSE_ARENAS");
        CHECK(Layout::SelectArenaModeFromEnvironment(c.supported) == Layout::SelectArenaMode(c.supported, seen));
        if (c.env && *c.env) {
            CHECK(Layout::SelectArenaModeFromEnvironment(c.supported) == c.expected);
        }
    }
    SetForceEnvironment(nullptr);
    CHECK(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::Sparse);
    SetForceEnvironment("1");
    CHECK(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::FullyResidentForced);
    SetForceEnvironment("0");
    CHECK(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::Sparse);
    SetForceEnvironment(saved ? saved_copy.c_str() : nullptr);

    // The device and its graphics queue both have to offer sparse binding and residency.
    CHECK(Layout::SparseBuffersSupported(true, true, true));
    CHECK(!Layout::SparseBuffersSupported(false, true, true));
    CHECK(!Layout::SparseBuffersSupported(true, false, true));
    CHECK(!Layout::SparseBuffersSupported(true, true, false));
    CHECK(Layout::SelectArenaMode(Layout::SparseBuffersSupported(true, true, false), nullptr) ==
          ArenaMode::FullyResidentUnsupported);

    // The startup lines name the mode, the reason and the arena size.
    CHECK(Layout::FullyResidentStartupLine(ArenaMode::FullyResidentUnsupported, 16) ==
          "Vulkan buffer cache: sparse buffers unavailable, using 16 MiB fully resident arenas");
    CHECK(Layout::FullyResidentStartupLine(ArenaMode::FullyResidentForced, 4) ==
          "Vulkan buffer cache: ALN_FORCE_NON_SPARSE_ARENAS enabled, using 4 MiB fully resident arenas");
}

struct FakeArena {
    u64 cpu_addr;
    u64 size;
    u64 device_address;
    std::map<u64, u8> bytes; // written offsets only
    u64 CpuAddr() const {
        return cpu_addr;
    }
    u64 SizeBytes() const {
        return size;
    }
};

// Like BufferCache::Backing in fully resident arenas: adjacent ranges stay separate.
struct ResidentRange : Interval {
    constexpr bool CanMergeWith(const ResidentRange&) const noexcept {
        return false;
    }
    constexpr ResidentRange SubRange(u64 a, u64 b) const noexcept {
        return {{a, b}};
    }
};

// BufferCache's fully resident mode on fake arenas: GetResidentArena, EnsureResident,
// ObtainBuffer with FollowMigration, RefreshBinding, DownloadOwnedRange and RetireMigratedArenas.
struct World {
    static constexpr u64 PageBits = Layout::RESIDENT_PAGE_BITS;
    static constexpr u64 BlockShift = 16;
    static constexpr u64 PageSize = u64{1} << PageBits;

    std::list<FakeArena> arenas;
    std::vector<FakeArena*> owners = std::vector<FakeArena*>(Layout::NumPages(PageBits));
    IntervalList<ResidentRange> resident;
    std::unordered_map<u64, u64> bda;
    std::vector<FakeArena*> retired;
    std::vector<FakeArena*> destroyed;
    std::vector<std::string> events;
    std::vector<std::pair<u64, u64>> copied_blocks;
    u64 next_device_address = 0x7000'0000'0000;
    u64 generation = 0;
    u64 limit = ~u64{0};
    u64 over_limit = 0;
    u64 sparse_binds = 0;
    // Runs once inside the next Obtain, between residency and synchronization (a fault processed
    // while that ObtainBuffer waits).
    std::function<void()> nested;

    FakeArena* Create(u64 cpu_addr, u64 size) {
        over_limit += size > limit;
        arenas.push_back({cpu_addr, size, next_device_address, {}});
        next_device_address += size + GiB;
        events.push_back("create");
        return &arenas.back();
    }
    void Copy(FakeArena& from, FakeArena& to, u64 start, u64 end) {
        const u64 first = (start << BlockShift) - from.cpu_addr;
        const u64 last = (end << BlockShift) - from.cpu_addr;
        for (auto it = from.bytes.lower_bound(first); it != from.bytes.end() && it->first < last; ++it) {
            to.bytes[it->first + from.cpu_addr - to.cpu_addr] = it->second;
        }
        copied_blocks.emplace_back(start, end);
        events.push_back("copy");
    }
    void Publish(FakeArena& arena, u64 start, u64 end) {
        for (u64 block = start; block < end; ++block) {
            bda[block] = Layout::BlockDeviceAddress(arena.device_address, arena.cpu_addr, block, BlockShift);
        }
        events.push_back("publish");
    }
    void Retire(FakeArena& arena) {
        retired.push_back(&arena);
        events.push_back("retire");
    }

    FakeArena* GetArena(u64 first_block, u64 last_block) {
        const size_t before = retired.size();
        FakeArena* arena = Layout::ResidentArenaFor<FakeArena>(owners, resident, PageBits, BlockShift,
                                                               first_block, last_block, *this);
        generation += retired.size() != before;
        return arena;
    }

    void EnsureResident(FakeArena* arena, u64 first_block, u64 last_block) {
        std::vector<std::pair<u64, u64>> gaps;
        resident.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) { gaps.emplace_back(start, end); });
        for (const auto& [start, end] : gaps) {
            resident.Add(ResidentRange{{start, end}});
            Publish(*arena, start, end);
        }
    }

    std::pair<FakeArena*, u64> Obtain(u64 addr, u64 size, u8 write = 0) {
        const u64 first = addr >> BlockShift;
        const u64 last = (addr + size - 1) >> BlockShift;
        FakeArena* arena = GetArena(first, last);
        u64 seen = generation;
        EnsureResident(arena, first, last);
        if (auto hook = std::exchange(nested, nullptr)) {
            hook();
        }
        if (write) {
            Write(*arena, addr, size, write); // an upload recorded into the arena held
        }
        while (seen != generation) {
            seen = generation;
            arena = Follow(arena, addr, size, first, last);
        }
        return {arena, addr - arena->cpu_addr};
    }

    FakeArena* Follow(FakeArena* stale, u64 addr, u64 size, u64 first, u64 last) {
        FakeArena* current = GetArena(first, last);
        if (current == stale) {
            return current;
        }
        EnsureResident(current, first, last);
        for (u64 a = addr; a < addr + size; ++a) {
            if (const auto it = stale->bytes.find(a - stale->cpu_addr); it != stale->bytes.end()) {
                current->bytes[a - current->cpu_addr] = it->second;
            }
        }
        Publish(*current, first, last + 1);
        return current;
    }

    static u8 ByteAt(const FakeArena& arena, u64 addr) {
        const auto it = arena.bytes.find(addr - arena.cpu_addr);
        return it == arena.bytes.end() ? 0 : it->second;
    }

    static void Write(FakeArena& arena, u64 addr, u64 size, u8 value) {
        for (u64 a = addr; a < addr + size; ++a) {
            arena.bytes[a - arena.cpu_addr] = value;
        }
    }

    // What a shader reads through the BDA page table, or 0 when the block has no entry.
    u8 ReadThroughBda(u64 addr) const {
        const auto entry = bda.find(addr >> BlockShift);
        if (entry == bda.end()) {
            return 0;
        }
        const u64 device = entry->second + (addr & ((u64{1} << BlockShift) - 1));
        for (const auto& arena : arenas) {
            if (device >= arena.device_address && device < arena.device_address + arena.size) {
                const auto it = arena.bytes.find(device - arena.device_address);
                return it == arena.bytes.end() ? 0 : it->second;
            }
        }
        return 0xEE; // an address into no arena
    }

    std::pair<const FakeArena*, u64> Refresh(const FakeArena* arena, u64 offset) const {
        return Layout::CurrentBinding<FakeArena>(owners, retired, PageBits, arena, offset);
    }

    void RetireMigrated() {
        destroyed.insert(destroyed.end(), retired.begin(), retired.end());
        retired.clear();
    }
};

void TestRequestInsideOneArenaReusesIt() {
    World w;
    const u64 base = 0x10'0000'0000;
    auto [a, a_offset] = w.Obtain(base + 0x1000, 0x100);
    auto [b, b_offset] = w.Obtain(base + 0x20000, 0x4000);
    CHECK(a == b && w.arenas.size() == 1 && w.generation == 0);
    CHECK(a->size == World::PageSize && a_offset == 0x1000 && b_offset == 0x20000);
    CHECK(w.copied_blocks.empty() && w.retired.empty());
}

// Two arenas on neighbouring pages and a buffer across their boundary.
void TestSpanningTwoArenasMigrates() {
    World w;
    const u64 boundary = 0x10'0100'0000;
    auto [low, low_offset] = w.Obtain(boundary - 0x30000, 0x10000);
    auto [high, high_offset] = w.Obtain(boundary + 0x50000, 0x10000);
    World::Write(*low, boundary - 0x30000, 0x10, 0xA1);
    World::Write(*high, boundary + 0x50000, 0x10, 0xB2);
    CHECK(low != high && w.arenas.size() == 2);
    const u64 bda_low_before = w.bda[(boundary - 0x30000) >> World::BlockShift];
    CHECK(bda_low_before == low->device_address + low_offset);

    w.events.clear();
    w.GetArena((boundary - 0x8000) >> World::BlockShift, (boundary + 0x7FFF) >> World::BlockShift);
    const std::vector<std::string> migration_events = w.events;
    auto [merged, merged_offset] = w.Obtain(boundary - 0x8000, 0x10000);
    CHECK(merged != low && merged != high && w.arenas.size() == 3 && w.generation == 1);
    CHECK(merged->cpu_addr == boundary - World::PageSize && merged->size == 2 * World::PageSize);
    CHECK(merged_offset == World::PageSize - 0x8000);
    CHECK(w.owners[(boundary - 1) >> World::PageBits] == merged);
    CHECK(w.owners[boundary >> World::PageBits] == merged);

    // Resident data of both old arenas moved; the new block across the boundary was never data.
    CHECK(World::ByteAt(*merged, boundary - 0x30000) == 0xA1);
    CHECK(World::ByteAt(*merged, boundary + 0x50000) == 0xB2);
    CHECK(w.ReadThroughBda(boundary - 0x30000 + 0xF) == 0xA1);
    CHECK(w.ReadThroughBda(boundary + 0x50000 + 0x7) == 0xB2);

    // Every moved block points at the new arena, at its guest-relative offset.
    w.resident.ForEachInRange(0, ~u64{0} >> World::BlockShift, [&](const ResidentRange& range) {
        for (u64 block = range.start; block < range.end; ++block) {
            CHECK(w.bda.at(block) == merged->device_address + (block << World::BlockShift) - merged->cpu_addr);
        }
    });

    // Old arenas are retired only after the replacement exists and is published.
    CHECK(w.retired.size() == 2);
    const auto& ev = migration_events;
    size_t first_retire = ev.size(), last_publish = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (ev[i] == "retire") {
            first_retire = std::min(first_retire, i);
        } else if (ev[i] == "publish") {
            last_publish = i;
        }
    }
    CHECK(!ev.empty() && ev.front() == "create");
    CHECK(first_retire < ev.size() && last_publish > 0 && last_publish < first_retire);
    CHECK(std::count(ev.begin(), ev.end(), "copy") == 2);
}

// Pages never made resident are never copied as data.
void TestHolesAreNotCopied() {
    World w;
    const u64 boundary = 0x10'0300'0000;
    w.Obtain(boundary - 0x400000, 0x10000);
    w.Obtain(boundary + 0x200000, 0x20000);
    w.Obtain(boundary - 0x8000, 0x10000);
    u64 copied = 0;
    for (const auto& [start, end] : w.copied_blocks) {
        copied += end - start;
        for (u64 block = start; block < end; ++block) {
            CHECK(w.resident.Contains(block));
        }
    }
    CHECK(copied == 1 + 2);
}

// A command resolved arena A, then a fault processed while it waited merged A away.
void TestNestedMigrationFollowsStaleBinding() {
    World w;
    const u64 boundary = 0x10'0700'0000;
    const u64 addr = boundary - 0x200000;
    auto [first, first_offset] = w.Obtain(addr, 0x100);
    w.Obtain(boundary + 0x100000, 0x100);

    // The caller still holds `first`; another ObtainBuffer of the command (or a fault) merges.
    w.Obtain(boundary - 0x10, 0x20);
    const auto [current, offset] = w.Refresh(first, first_offset);
    CHECK(current != first && current == w.owners[addr >> World::PageBits]);
    CHECK(current->cpu_addr + offset == addr);
    CHECK(offset == addr - current->cpu_addr);
    // Bindings into arenas no migration replaced are left alone.
    const FakeArena* other = w.Obtain(0x20'0000'0000, 0x100).first;
    CHECK((w.Refresh(other, 0x40) == std::pair<const FakeArena*, u64>{other, 0x40}));

    // A fault inside ObtainBuffer merges the arena it resolved: the upload it records into the
    // stale arena reaches the arena returned, and the shaders' BDA entries.
    World n;
    const u64 b2 = 0x10'0900'0000;
    n.Obtain(b2 + 0x10000, 0x100);
    n.nested = [&] { n.Obtain(b2 - 0x10, 0x20); };
    const auto [result, result_offset] = n.Obtain(b2 - 0x40000, 0x80, 0x5C);
    CHECK(n.generation == 1);
    CHECK(result == n.owners[(b2 - 1) >> World::PageBits] && result->size == 2 * World::PageSize);
    CHECK(result->cpu_addr + result_offset == b2 - 0x40000);
    CHECK(World::ByteAt(*result, b2 - 0x40000) == 0x5C);
    CHECK(n.ReadThroughBda(b2 - 0x40000 + 0x7F) == 0x5C);

    // Chains: A into B into C still resolve to C.
    World c;
    const u64 b3 = 0x10'0B00'0000;
    auto [a, a_offset] = c.Obtain(b3 - 0x100000, 0x100);
    c.Obtain(b3 + 0x100000, 0x100);
    c.Obtain(b3 - 0x10, 0x20);
    c.Obtain(b3 + World::PageSize + 0x100, 0x100);
    c.Obtain(b3 + World::PageSize - 0x10, 0x20);
    const auto [chained, chained_offset] = c.Refresh(a, a_offset);
    CHECK(c.generation == 2 && chained == c.owners[b3 >> World::PageBits]);
    CHECK(chained->cpu_addr + chained_offset == b3 - 0x100000);
}

void TestRetiredArenasLiveUntilRetirement() {
    World w;
    const u64 boundary = 0x10'0D00'0000;
    auto [low, low_offset] = w.Obtain(boundary - 0x10000, 0x100);
    w.Obtain(boundary + 0x10000, 0x100);
    w.Obtain(boundary - 0x10, 0x20);
    CHECK(w.destroyed.empty() && w.retired.size() == 2);
    // Until the command list ends the stale arena still resolves to its replacement.
    CHECK(w.Refresh(low, low_offset).first != low);
    w.RetireMigrated();
    CHECK(w.destroyed.size() == 2 && w.retired.empty());
}

// Readback windows crossing pages without an arena and several owned arenas.
void TestOwnedRunsForReadback() {
    World w;
    const u64 base = 0x10'1000'0000;
    w.Obtain(base + 0x100, 0x100);                         // page P
    w.Obtain(base + 2 * World::PageSize + 0x100, 0x100);   // page P+2, P+1 has no arena
    w.Obtain(base + 3 * World::PageSize + 0x100, 0x100);   // page P+3, its own arena
    std::span<FakeArena* const> owners{w.owners};
    std::vector<std::tuple<const FakeArena*, u64, u64>> runs;
    u64 start = base + 0x80000;
    const u64 end = base + 3 * World::PageSize + 0x2000;
    for (;;) {
        const auto run = Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, start, end);
        if (!run.arena) {
            break;
        }
        CHECK(run.start >= run.arena->cpu_addr && run.end <= run.arena->cpu_addr + run.arena->size);
        runs.emplace_back(run.arena, run.start, run.end);
        start = run.end;
    }
    CHECK(runs.size() == 3);
    CHECK(runs[0] == std::make_tuple(w.owners[base >> World::PageBits], base + 0x80000, base + World::PageSize));
    CHECK(std::get<1>(runs[1]) == base + 2 * World::PageSize);
    CHECK(std::get<2>(runs[1]) == base + 3 * World::PageSize);
    CHECK(std::get<1>(runs[2]) == base + 3 * World::PageSize && std::get<2>(runs[2]) == end);

    // Nothing owned at all.
    const auto none = Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, 0x30'0000'0000, 0x30'0400'0000);
    CHECK(none.arena == nullptr);
    // The top of the address space, and past it.
    const u64 top = u64{1} << Layout::ADDRESS_SPACE_BITS;
    CHECK(Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, top - 0x1000, top).arena == nullptr);
    CHECK(Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, top - 0x1000, top + 0x1000).arena == nullptr);
}

// The highest guest pages: no wrapped arena size or page index.
void TestTopOfAddressSpace() {
    World w;
    const u64 top = u64{1} << Layout::ADDRESS_SPACE_BITS;
    auto [high, high_offset] = w.Obtain(top - 0x1000, 0x1000);
    auto [low, low_offset] = w.Obtain(top - World::PageSize - 0x1000, 0x1000);
    auto [merged, offset] = w.Obtain(top - World::PageSize - 0x10, 0x20);
    CHECK(high != low && merged->cpu_addr == top - 2 * World::PageSize);
    CHECK(merged->cpu_addr + merged->size == top && offset == World::PageSize - 0x10);
    CHECK(w.bda.at((top - 0x1000) >> World::BlockShift) ==
          merged->device_address + (((top - 0x1000) >> World::BlockShift) << World::BlockShift) - merged->cpu_addr);
    (void)high_offset;
    (void)low_offset;
}

// A fully resident model of the 0x10'0000'0000.. direct memory under AMD-like limits: arenas
// that would pass the driver limit are visible to the cache (which refuses them), never silently
// created.
void TestResidentArenasAgainstDriverLimit() {
    World w;
    w.limit = Layout::ResidentArenaLimit(kMaxBufferSize, 4 * GiB);
    std::mt19937_64 rng{0x9C4F27};
    std::uniform_int_distribution<u64> where(kGuestFirst, kGuestEnd - 0x100000);
    std::uniform_int_distribution<u64> size(4, 0x40000);
    u64 largest = 0;
    for (int i = 0; i < 4000; ++i) {
        const u64 address = where(rng) & ~u64{3};
        const auto [arena, offset] = w.Obtain(address, size(rng));
        CHECK(arena->cpu_addr + offset == address);
        largest = std::max(largest, arena->size);
    }
    std::printf("fully resident random buffers: %zu arenas created, %llu migrations, largest %llu MiB, %llu over "
                "the %llu MiB limit\n",
                w.arenas.size(), (unsigned long long)w.generation, (unsigned long long)(largest >> 20),
                (unsigned long long)w.over_limit, (unsigned long long)(w.limit >> 20));
    CHECK((largest > w.limit) == (w.over_limit != 0));
    // Every resident block is published at its current owner.
    w.resident.ForEachInRange(0, ~u64{0} >> World::BlockShift, [&](const ResidentRange& range) {
        const FakeArena* owner = w.owners[(range.start << World::BlockShift) >> World::PageBits];
        CHECK(owner && w.bda.at(range.start) == owner->device_address + (range.start << World::BlockShift) - owner->cpu_addr);
    });
}

// ---------------------------------------------------------------------------------------------
// Source contracts: the parts that need a Vulkan device
// ---------------------------------------------------------------------------------------------

std::string ReadSource(const char* relative) {
    std::ifstream in(std::string(ALN_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Body of the function whose definition starts with `signature`, up to its closing brace at column 0.
std::string Body(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = source.find("\n}\n", start);
    return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string Block(const std::string& body, const std::string& opening) {
    const auto start = body.find(opening);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = body.find("\n    }\n", start);
    return body.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

bool Ordered(const std::string& text, const std::string& first, const std::string& second) {
    const auto a = text.find(first);
    return a != std::string::npos && text.find(second, a) != std::string::npos;
}

void TestBufferCacheSources() {
    const std::string cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const std::string header = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.h");
    const std::string buffer = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer.cpp");
    const std::string instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.h");
    const std::string instance_cpp = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(!cache.empty() && !header.empty());

    // Capability, sizing and the startup line.
    CHECK(Has(instance, "bool IsSparseBufferSupported() const;"));
    CHECK(Has(instance_cpp, "ArenaLayout::SparseBuffersSupported(\n        features.sparseBinding, features.sparseResidencyBuffer, queue_sparse_binding)"));
    CHECK(Has(instance_cpp, "vk::QueueFlagBits::eSparseBinding"));
    const std::string ctor = Body(cache, "BufferCache::BufferCache(");
    CHECK(Has(ctor, "ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferSupported())"));
    CHECK(Has(ctor, "sparse_arenas = !ArenaLayout::IsFullyResident(arena_mode);"));
    CHECK(Count(cache, "std::getenv(\"ALN_FORCE_NON_SPARSE_ARENAS\")") == 0);
    const std::string layout = ReadSource("gpu/shadps4/video_core/buffer_cache/arena_layout.h");
    CHECK(Count(layout, "std::getenv(\"ALN_FORCE_NON_SPARSE_ARENAS\")") == 1);
    CHECK(Ordered(layout, "if (NonSparseArenasForced(force_env_value)) {\n        return ArenaMode::FullyResidentForced;",
                  "ArenaMode::FullyResidentUnsupported;"));
    // Forced and unsupported modes share the one fully resident implementation: every branch tests
    // sparse_arenas, never the reason, and the reason only picks the startup line.
    CHECK(Count(cache, "ArenaMode::FullyResidentForced") == 0 && Count(cache, "ArenaMode::FullyResidentUnsupported") == 0);
    CHECK(Count(cache, "arena_mode") == 3);
    CHECK(Has(ctor, "ArenaLayout::FullyResidentStartupLine(arena_mode, arena_page_size >> 20)"));
    CHECK(Has(ctor, "std::printf(\"%s\\n\","));
    CHECK(Has(Block(ctor, "if (!sparse_arenas) {"), "FullyResidentStartupLine("));
    CHECK(!Has(Block(ctor, "} else if (arena_page_bits != MAX_ARENA_PAGE_BITS) {"), "fully resident"));
    CHECK(Has(layout, "ALN_FORCE_NON_SPARSE_ARENAS enabled") && Has(layout, "sparse buffers unavailable"));
    CHECK(Count(layout, "fully resident arenas\"") == 1);
    CHECK(Has(ctor, "ArenaLayout::PageBits(sparse_arenas, max_buffer_size, max_allocation_size)"));
    CHECK(Has(ctor, "ArenaLayout::ResidentPageFits("));
    CHECK(Has(ctor, "sparse_arenas ? vk::BufferCreateFlagBits::eSparseBinding"));
    CHECK(Has(ctor, "ArenaLayout::BlockSize(sparse_arenas, reqs.alignment)"));
    CHECK(Has(Block(ctor, "if (sparse_arenas) {"), "BackingMemoryTypes("));

    // Sparse mode keeps the aliasing arena map; only the fully resident mode migrates.
    const std::string get_arena = Body(cache, "const Buffer* BufferCache::GetArena(");
    CHECK(Ordered(get_arena, "if (!sparse_arenas) {\n        return GetResidentArena(", "arena_map.PageFor("));
    CHECK(Has(get_arena, "MemoryType::Sparse"));
    size_t calls = 0;
    for (size_t at = cache.find("GetResidentArena("); at != std::string::npos; at = cache.find("GetResidentArena(", at + 1)) {
        ++calls;
    }
    CHECK(calls == 2); // the definition and GetArena's fully resident branch

    // Fully resident arenas: ordinary device-local buffers within the driver limit (the existing
    // buffer allocation policy: other memory types, then over budget), migrated before the BDA
    // page table and barriers, retired later.
    const std::string resident = Body(cache, "const Buffer* BufferCache::GetResidentArena(");
    CHECK(Has(resident, "MemoryType::DeviceLocal") && !Has(resident, "MemoryType::Sparse"));
    CHECK(Has(resident, "ArenaLayout::ResidentArenaLimit(") && Has(resident, "ASSERT_MSG(size <= limit"));
    CHECK(!Has(resident, "BindsForArena") && !Has(resident, "bindSparse"));
    CHECK(Ordered(resident, "ops.FlushCopies(*arena);", "PublishArenaBda(arena,"));
    CHECK(Ordered(resident, "PublishArenaBda(arena,", "runtime.FlushBarriers();"));
    CHECK(Has(resident, "eMemoryWrite"));
    CHECK(Has(buffer, "over budget"));
    const std::string retire = Body(cache, "void BufferCache::RetireMigratedArenas(");
    CHECK(Has(retire, "scheduler.DeferOperation(") && Has(retire, "buffer.Destroy()"));
    CHECK(!Has(retire, "arenas.erase") && !Has(retire, "remove_if"));
    CHECK(Has(header, "std::deque<Buffer> arenas;"));

    // Residency without sparse binding: bookkeeping and BDA entries only.
    const std::string ensure = Body(cache, "void BufferCache::EnsureResident(");
    const std::string non_sparse = Block(ensure, "if (!sparse_arenas) {");
    CHECK(Has(non_sparse, "resident_ranges.Add(") && Has(non_sparse, "PublishArenaBda(arena,"));
    CHECK(Has(non_sparse, "return;"));
    CHECK(Ordered(ensure, "if (!sparse_arenas) {", "AllocateBacking("));
    CHECK(Ordered(ensure, "if (!sparse_arenas) {", "BindsForArena("));
    CHECK(!Has(non_sparse, "allocateMemory") && !Has(non_sparse, "BindsForArena"));
    CHECK(Has(header, "ASSERT(sparse_arenas);"));
    // Sparse residency unchanged: backing with the Issue #9 fallback, binds into every covering arena.
    CHECK(Has(ensure, "AllocateBacking(backing_memory_types,"));
    CHECK(Has(ensure, "arena_map.ForEachCovering("));
    CHECK(Has(ensure, "Fallback::SparseBackingInSystemMemory"));
    const std::string submit = Body(cache, "void BufferCache::SubmitPendingArenaBinds(");
    CHECK(Ordered(submit, "if (pending_binds.empty()) {\n        return;", "bindSparse("));

    // Followed migrations and readback.
    const std::string obtain = Body(cache, "std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(");
    CHECK(Ordered(obtain, "u64 generation = arena_generation;", "SynchronizeMemory(arena,"));
    CHECK(Ordered(obtain, "SynchronizeMemory(arena,", "while (generation != arena_generation)"));
    CHECK(Ordered(obtain, "FollowMigration(", "gpu_modified_ranges.Add("));
    const std::string read = Body(cache, "void BufferCache::ReadMemory(");
    CHECK(Ordered(read, "if (!sparse_arenas) {", "DownloadOwnedRange("));
    CHECK(Ordered(read, "DownloadOwnedRange(", "GetArena("));
    const std::string owned = Body(cache, "void BufferCache::DownloadOwnedRange(");
    CHECK(Has(owned, "if (!run.arena) {") && Has(owned, "DownloadMemory(run.arena"));
    const std::string publish = Body(cache, "void BufferCache::PublishArenaBda(");
    CHECK(Has(publish, "ArenaLayout::BlockDeviceAddress(arena_address, arena->cpu_addr"));
}

void TestRasterizerRefreshesBindings() {
    const std::string r = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string hle = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp");
    CHECK(!r.empty() && !hle.empty());
    const std::string draw = Body(r, "void Rasterizer::DrawRecord(");
    CHECK(Ordered(draw, "RefreshArenaBindings();", "EmitVertexBuffers();"));
    CHECK(Ordered(draw, "ResolveVertexBuffers(", "RefreshArenaBindings();"));
    const std::string indirect = Body(r, "void Rasterizer::DrawIndirectRecord(");
    CHECK(!Has(indirect, "BindVertexBuffers(") && !Has(indirect, "BindIndexBuffer("));
    CHECK(Ordered(indirect, "ObtainBuffer(indirect.args", "RefreshArenaBindings();"));
    CHECK(Ordered(indirect, "buffer_cache.RefreshBinding(buffer, base);", "EmitVertexBuffers();"));
    CHECK(Has(indirect, "buffer_cache.RefreshBinding(count_buffer, count_offset);"));
    CHECK(Ordered(Body(r, "void Rasterizer::DispatchRecord("), "BindResources(pipeline)", "RefreshArenaBindings();"));
    const std::string dispatch_indirect = Body(r, "void Rasterizer::DispatchIndirectRecord(");
    CHECK(Ordered(dispatch_indirect, "ObtainBuffer(address + offset", "buffer_cache.RefreshBinding(buffer, base);"));
    CHECK(Ordered(dispatch_indirect, "buffer_cache.RefreshBinding(buffer, base);", "dispatchIndirect"));
    CHECK(Ordered(Body(r, "void Rasterizer::CopyBuffer("), "ObtainBuffer(dst", "RefreshBinding(src_buffer, src_offset)"));
    CHECK(Ordered(hle, "ObtainBuffer(\n            dst_buf_sharp", "RefreshBinding(src_buf, src_buf_offset)"));
    const std::string refresh = Body(r, "void Rasterizer::RefreshArenaBindings(");
    CHECK(Has(refresh, "buffer_infos") && Has(refresh, "host_buffers") && Has(refresh, "index_bind.handle") &&
          Has(refresh, "bound_buffers"));
    CHECK(Has(Body(r, "bool Rasterizer::BindResources("), "bind_generation = buffer_cache.ArenaGeneration();"));
    CHECK(Ordered(Body(r, "void Rasterizer::OnSubmit("), "DrainDrawPipe();", "buffer_cache.RetireMigratedArenas();"));
}

// TextureCache::RefreshImage on a GPU-modified range: ObtainBufferForImage resolves the arena,
// DetileImage's params copy may wait for the GPU (faults processed then), the binding is
// refreshed, and the tiling dispatch then reads the guest bytes at the binding.
struct Upload {
    const FakeArena* resolved;
    const FakeArena* source;
    u64 offset;
    u8 value;
};

Upload ModelDetileUpload(World& w, u64 addr, u64 size, const std::function<void()>& wait) {
    const auto [arena, offset] = w.Obtain(addr, size);
    wait();
    const auto [source, source_offset] = w.Refresh(arena, offset);
    return {arena, source, source_offset, World::ByteAt(*source, source->cpu_addr + source_offset)};
}

void TestTextureUploadSourceAcrossMigration() {
    const u64 boundary = 0x10'1100'0000;
    const u64 image = boundary - 0x80000;

    // No migration: the binding ObtainBufferForImage returned.
    {
        World w;
        const auto [arena, offset] = w.Obtain(image, 0x40000);
        World::Write(*arena, image, 0x40000, 0x11);
        const Upload up = ModelDetileUpload(w, image, 0x40000, [] {});
        CHECK(up.source == arena && up.offset == offset && up.value == 0x11);
        CHECK(up.source->cpu_addr + up.offset == image);
    }

    // A fault processed in the wait merges the image's arena with its neighbour and uploads new
    // guest data for the image range into the new arena only.
    World w;
    const auto [arena, offset] = w.Obtain(image, 0x40000);
    World::Write(*arena, image, 0x40000, 0x11);
    w.Obtain(boundary + 0x10000, 0x100);
    const Upload up = ModelDetileUpload(w, image, 0x40000, [&] {
        const auto [merged, merged_offset] = w.Obtain(boundary - 0x10, 0x20);
        World::Write(*merged, image, 0x40000, 0x22);
        (void)merged_offset;
    });
    CHECK(up.resolved == arena && up.source != arena);
    CHECK(up.source == w.owners[image >> World::PageBits]);
    CHECK(up.source->cpu_addr + up.offset == image);
    CHECK(up.offset == image - up.source->cpu_addr);
    CHECK(up.value == 0x22);
    // Read through the stale arena, the upload would have taken the old bytes.
    CHECK(World::ByteAt(*arena, image) == 0x11);
    // The source is never a retired arena; the stale one lives until the command list ends.
    CHECK(std::ranges::find(w.retired, up.source) == w.retired.end());
    CHECK(std::ranges::find(w.retired, arena) != w.retired.end() && w.destroyed.empty());
    w.RetireMigrated();
    CHECK(std::ranges::find(w.destroyed, arena) != w.destroyed.end());

    // The source in the upper arena: the merged arena starts a page lower, so the offset moves.
    {
        World u;
        const u64 upper = boundary + 0x30000;
        u.Obtain(boundary - 0x100000, 0x100);
        const auto [high, high_offset] = u.Obtain(upper, 0x2000);
        World::Write(*high, upper, 0x2000, 0x66);
        const Upload moved = ModelDetileUpload(u, upper, 0x2000, [&] {
            const auto [merged, merged_offset] = u.Obtain(boundary - 0x10, 0x20);
            World::Write(*merged, upper, 0x2000, 0x77);
            (void)merged_offset;
        });
        CHECK(moved.resolved == high && high_offset == 0x30000);
        CHECK(moved.source->cpu_addr == boundary - World::PageSize);
        CHECK(moved.offset == World::PageSize + 0x30000);
        CHECK(moved.source->cpu_addr + moved.offset == upper && moved.value == 0x77);
    }

    // A to B to C within one wait resolves to C.
    World c;
    const u64 b = 0x10'1300'0000;
    const auto [first, first_offset] = c.Obtain(b - 0x40000, 0x1000);
    World::Write(*first, b - 0x40000, 0x1000, 0x33);
    c.Obtain(b + 0x100, 0x100);
    c.Obtain(b + World::PageSize + 0x100, 0x100);
    const Upload chained = ModelDetileUpload(c, b - 0x40000, 0x1000, [&] {
        c.Obtain(b - 0x10, 0x20);
        const auto [last, last_offset] = c.Obtain(b + World::PageSize - 0x10, 0x20);
        World::Write(*last, b - 0x40000, 0x1000, 0x44);
        (void)last_offset;
    });
    CHECK(c.generation == 2 && chained.source == c.owners[b >> World::PageBits]);
    CHECK(chained.source->cpu_addr + chained.offset == b - 0x40000 && chained.value == 0x44);
    (void)first_offset;

    // Sparse arenas never migrate: nothing is ever replaced, so a binding is left as is.
    FakeArena sparse{0x10'0000'0000, 2 * GiB, 0x5000'0000'0000, {}};
    const std::vector<FakeArena*> none;
    const auto kept = Layout::CurrentBinding<FakeArena>(w.owners, none, World::PageBits, &sparse, 0x1234);
    CHECK(kept.first == &sparse && kept.second == 0x1234);
}

void TestTextureUploadSources() {
    const std::string tile = ReadSource("gpu/shadps4/video_core/texture_cache/tile_manager.cpp");
    const std::string tex = ReadSource("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    const std::string cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const std::string runtime = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp");
    const std::string detile = Body(tile, "std::pair<const Buffer*, u64> TileManager::DetileImage(");
    CHECK(Ordered(detile, "stream_buffer.Copy(", "buffer_cache.RefreshBinding(in_buffer, in_offset);"));
    CHECK(Ordered(detile, "GetStagingPool().Request(", "buffer_cache.RefreshBinding(in_buffer, in_offset);"));
    CHECK(Ordered(detile, "buffer_cache.RefreshBinding(in_buffer, in_offset);", ".buffer = in_buffer->Handle(),"));
    CHECK(Ordered(detile, "buffer_cache.RefreshBinding(in_buffer, in_offset);", "runtime.FlushBarriers();"));
    // Untiled images go straight to the upload, which records without waiting.
    CHECK(Ordered(detile, "if (!info.props.is_tiled) {\n        return {in_buffer, in_offset};", "stream_buffer.Copy("));
    const std::string upload = Body(runtime, "void Runtime::UploadImage(");
    CHECK(!upload.empty() && !Has(upload, "scheduler.Flush") && !Has(upload, "scheduler.Finish") &&
          !Has(upload, "scheduler.Wait"));
    const std::string refresh = Body(tex, "void TextureCache::RefreshImage(");
    CHECK(Ordered(refresh, "buffer_cache.ObtainBufferForImage(", "tile_manager.DetileImage(buffer_cache, in_buffer, in_offset"));
    CHECK(Ordered(refresh, "tile_manager.DetileImage(", "runtime.UploadImage("));
    // TileImage writes into the arena ObtainBuffer holds; ObtainBuffer's FollowMigration moves it.
    CHECK(!Has(Body(tile, "void TileManager::TileImage("), "RefreshBinding"));
    CHECK(Ordered(Body(cache, "std::pair<const Buffer*, u64> BufferCache::ObtainBuffer("), "SynchronizeMemory(arena,",
                  "FollowMigration("));
    // Only fully resident migrations retire arenas.
    CHECK(Count(cache, "retired_arenas.push_back(") == 1);
    CHECK(Has(Body(cache, "const Buffer* BufferCache::GetResidentArena("), "retired_arenas.push_back("));
}

// A command buffer as the scheduler records it: binds are per command buffer, a submit starts a
// new one with none, and a draw reads what is bound in its own command buffer.
struct Commands {
    struct Bind {
        const FakeArena* arena;
        u64 offset;
    };
    std::optional<Bind> vertex, index;
    u32 vertex_binds = 0, index_binds = 0;
    void Submit() {
        vertex.reset();
        index.reset();
    }
};

// DrawIndirectRecord: resolve vertex and index buffers, look up the indirect arguments (which may
// submit and process faults), then emit the binds (old order: before the lookup) and draw.
bool ModelIndirectDraw(bool emit_before_lookup, bool lookup_submits, bool lookup_migrates) {
    World w;
    Commands cmd;
    const u64 boundary = 0x10'1500'0000;
    const u64 vertices = boundary - 0x100000, indices = boundary - 0x200000;
    w.Obtain(boundary + 0x100, 0x100);
    auto [vb, vb_offset] = w.Obtain(vertices, 0x1000);
    auto [ib, ib_offset] = w.Obtain(indices, 0x100);
    const u64 generation = w.generation;
    const auto emit = [&] {
        const auto [v, vo] = w.Refresh(vb, vb_offset);
        const auto [i, io] = w.Refresh(ib, ib_offset);
        cmd.vertex = Commands::Bind{v, vo};
        cmd.index = Commands::Bind{i, io};
        ++cmd.vertex_binds;
        ++cmd.index_binds;
    };
    if (emit_before_lookup) {
        emit();
    }
    if (lookup_submits) {
        cmd.Submit();
    }
    if (lookup_migrates) {
        w.Obtain(boundary - 0x10, 0x20);
    }
    if (!emit_before_lookup) {
        emit();
    }
    const auto current = [&](u64 addr, const std::optional<Commands::Bind>& bound) {
        return bound && bound->arena == w.owners[addr >> World::PageBits] &&
               bound->arena->cpu_addr + bound->offset == addr;
    };
    const bool drawn = current(vertices, cmd.vertex) && current(indices, cmd.index);
    return drawn && cmd.vertex_binds == 1 && cmd.index_binds == 1 && (lookup_migrates == (w.generation != generation));
}

void TestIndirectDrawBindOrder() {
    for (const bool submits : {false, true}) {
        for (const bool migrates : {false, true}) {
            CHECK(ModelIndirectDraw(false, submits, migrates));
        }
    }
    // The old order loses the binds to a submit in the lookup, or keeps a migrated arena.
    CHECK(ModelIndirectDraw(true, false, false));
    CHECK(!ModelIndirectDraw(true, true, false));
    CHECK(!ModelIndirectDraw(true, false, true));

    const std::string r = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string tex = ReadSource("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    const std::string cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    // The lookup can submit: readback resolution waits on ticks, and a wait on the current tick flushes.
    CHECK(Has(Body(cache, "std::pair<const Buffer*, u64> BufferCache::ObtainBuffer("), "texture_cache.ResolveReadbacks("));
    CHECK(Has(Body(tex, "bool TextureCache::ResolveReadbacks("), "scheduler.Wait("));
    const std::string scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    CHECK(Ordered(Body(scheduler, "void Scheduler::Wait("), "if (tick >= work_semaphore.CurrentTick()) {", "Flush(info);"));

    const std::string indirect = Body(r, "void Rasterizer::DrawIndirectRecord(");
    CHECK(Count(indirect, "ResolveVertexBuffers(pipeline, nullptr);") == 1);
    CHECK(Count(indirect, "ResolveIndexBuffer(0, true);") == 1);
    CHECK(Count(indirect, "EmitVertexBuffers();") == 1 && Count(indirect, "EmitIndexBuffer();") == 1);
    CHECK(Ordered(indirect, "ResolveVertexBuffers(pipeline, nullptr);", "ObtainBuffer(indirect.args"));
    CHECK(Ordered(indirect, "ObtainBuffer(count_address", "EmitVertexBuffers();"));
    CHECK(Ordered(indirect, "EmitVertexBuffers();", "EmitIndexBuffer();"));
    CHECK(Ordered(indirect, "EmitIndexBuffer();", "scheduler.Record("));
    CHECK(Ordered(indirect, "EmitIndexBuffer();", "drawIndexedIndirect"));

    const std::string direct = Body(r, "void Rasterizer::DrawRecord(");
    CHECK(Count(direct, "EmitVertexBuffers();") == 1 && Count(direct, "EmitIndexBuffer();") == 1);
    CHECK(Count(direct, "RefreshArenaBindings();") == 1);
    CHECK(Ordered(direct, "BindResources(pipeline);", "ResolveVertexBuffers(pipeline, draw_prepared);"));
    CHECK(Ordered(direct, "ResolveIndexBuffer(index_offset);", "RefreshArenaBindings();\n    EmitVertexBuffers();"));
    CHECK(Ordered(direct, "EmitIndexBuffer();", "scheduler.Record("));
    // Nothing between the binds and the draw can submit.
    const auto after_binds = indirect.substr(indirect.find("EmitIndexBuffer();"));
    for (const char* call : {"ObtainBuffer(", "scheduler.Flush", "scheduler.Finish", "scheduler.Wait"}) {
        CHECK(!Has(after_binds, call));
        CHECK(!Has(direct.substr(direct.find("EmitIndexBuffer();")), call));
    }
    CHECK(!Has(r, "void Rasterizer::BindVertexBuffers(") && !Has(r, "void Rasterizer::BindIndexBuffer("));
}

// ---------------------------------------------------------------------------------------------
// TileImage write footprint (texel buffer reads of an image) and migration
// ---------------------------------------------------------------------------------------------

using VideoCore::ImageInfo;

ImageInfo MakeImage(u32 width, u32 height, u32 pitch, u32 bits, u32 levels, u32 layers, bool block = false,
                    bool tiled = false) {
    ImageInfo info{};
    info.size = {width, height, 1};
    info.pitch = pitch;
    info.num_bits = bits;
    info.resources = {levels, layers};
    info.props.is_block = block;
    info.props.is_tiled = tiled;
    info.array_mode = tiled ? AmdGpu::ArrayMode::Array1DTiledThin1 : AmdGpu::ArrayMode::ArrayLinearAligned;
    info.tile_mode = tiled ? AmdGpu::TileMode::Display1DThin : AmdGpu::TileMode::DisplayLinearAligned;
    info.UpdateSize();
    return info;
}

using Spans = std::vector<std::pair<u64, u64>>;

void AddSpan(Spans& spans, u64 start, u64 end) {
    if (!spans.empty() && spans.back().second == start) {
        spans.back().second = end;
    } else {
        spans.emplace_back(start, end);
    }
}

// Bytes vkCmdCopyImageToBuffer writes for the copies, relative to the image start (Vulkan buffer
// addressing: rows of the extent at bufferRowLength, layers and slices at bufferImageHeight).
Spans DownloadFootprint(const ImageInfo& info, std::span<const vk::BufferImageCopy> copies) {
    const u32 block = info.props.is_block ? 4 : 1;
    const u64 block_bytes = info.num_bits / 8;
    const auto blocks = [&](u32 texels) { return u64{(texels + block - 1) / block}; };
    Spans spans;
    for (const auto& c : copies) {
        const u64 row_blocks = blocks(c.bufferRowLength ? c.bufferRowLength : c.imageExtent.width);
        const u64 image_rows = blocks(c.bufferImageHeight ? c.bufferImageHeight : c.imageExtent.height);
        const u64 slices = u64{c.imageSubresource.layerCount} * c.imageExtent.depth;
        for (u64 z = 0; z < slices; ++z) {
            for (u64 y = 0; y < blocks(c.imageExtent.height); ++y) {
                const u64 start = c.bufferOffset + (z * image_rows + y) * row_blocks * block_bytes;
                AddSpan(spans, start, start + blocks(c.imageExtent.width) * block_bytes);
            }
        }
    }
    return spans;
}

// TileImage: untiled images go through DownloadImage, tiled ones through the tiler dispatch over
// [0, guest_size), every texel of each mip written.
Spans TileImageFootprint(const ImageInfo& info, std::span<const vk::BufferImageCopy> copies) {
    if (info.props.is_tiled) {
        return {{0, info.guest_size}};
    }
    return DownloadFootprint(info, copies);
}

VideoCore::TexelImageCopies CopiesOf(const ImageInfo& info) {
    return VideoCore::TexelImageCopiesFor(info, vk::ImageAspectFlagBits::eColor, 0x1000,
                                          0x1000 + info.guest_size);
}

u64 FootprintEnd(const Spans& spans) {
    return spans.empty() ? 0 : spans.back().second;
}

void TestTileImageFootprint() {
    // Linear, no padding: exactly the image.
    {
        const ImageInfo info = MakeImage(64, 64, 64, 32, 1, 1);
        const auto copies = CopiesOf(info);
        const Spans spans = TileImageFootprint(info, copies);
        CHECK(info.guest_size == 64 * 64 * 4);
        CHECK(spans.size() == 1 && spans[0] == std::make_pair(u64{0}, u64{info.guest_size}));
    }
    // Row pitch above the width: pitched rows with holes, the last row ends inside the image, and
    // past a texel buffer that covers only the visible bytes.
    {
        const ImageInfo info = MakeImage(100, 50, 100, 32, 1, 1);
        const auto copies = CopiesOf(info);
        const Spans spans = TileImageFootprint(info, copies);
        const u64 row = u64{info.mips_layout[0].pitch} * 4;
        CHECK(info.mips_layout[0].pitch > 100);
        CHECK(spans.size() == 50 && spans[1].first == row);
        CHECK(FootprintEnd(spans) == 49 * row + 100 * 4 && FootprintEnd(spans) <= info.guest_size);
        CHECK(FootprintEnd(spans) > u64{100} * 50 * 4);
    }
    // Several mips and layers: dense, the last layer's last row ends at the image end.
    {
        const ImageInfo info = MakeImage(64, 32, 64, 32, 2, 3);
        const auto copies = CopiesOf(info);
        const Spans spans = TileImageFootprint(info, copies);
        CHECK(copies.size() == 2);
        CHECK(spans.size() == 1 && FootprintEnd(spans) == info.guest_size);
    }
    // Block compressed (BC1, 8-byte 4x4 blocks): block-rounded rows, inside the image.
    {
        const ImageInfo exact = MakeImage(256, 256, 256, 64, 1, 1, true);
        CHECK(FootprintEnd(TileImageFootprint(exact, CopiesOf(exact))) == exact.guest_size);
        const ImageInfo info = MakeImage(250, 250, 250, 64, 1, 1, true);
        const Spans spans = TileImageFootprint(info, CopiesOf(info));
        const u64 row_blocks = info.mips_layout[0].pitch / 4;
        CHECK(FootprintEnd(spans) == (62 * row_blocks + 63) * 8);
        CHECK(FootprintEnd(spans) <= info.guest_size);
    }
    // Small block-compressed mips of an array: the 32-texel minimum image height strides layers
    // past the mip's own size, so the write ends past the image.
    {
        const ImageInfo info = MakeImage(16, 16, 16, 64, 1, 4, true);
        const Spans spans = TileImageFootprint(info, CopiesOf(info));
        CHECK(info.mips_layout[0].height == 32 && info.guest_size == 2048);
        CHECK(FootprintEnd(spans) > info.guest_size);
        std::printf("BC1 16x16 array of 4: TileImage writes up to byte %llu of a %u byte image\n",
                    (unsigned long long)FootprintEnd(spans), info.guest_size);
    }
    // Tiled: the padded tile layout, all of it, beyond the visible bytes.
    {
        const ImageInfo info = MakeImage(100, 50, 100, 32, 1, 1, false, true);
        const Spans spans = TileImageFootprint(info, CopiesOf(info));
        CHECK(spans.size() == 1 && FootprintEnd(spans) == info.guest_size);
        CHECK(info.guest_size > u64{100} * 50 * 4);
    }
    // The copies stop at the first mip past the arena end, never at a mip that fits.
    {
        const ImageInfo info = MakeImage(64, 64, 64, 32, 3, 1);
        const auto all = VideoCore::TexelImageCopiesFor(info, vk::ImageAspectFlagBits::eColor, 0x100,
                                                        0x100 + info.guest_size);
        CHECK(all.size() == 3 && all[2].imageSubresource.mipLevel == 2);
        CHECK(FootprintEnd(DownloadFootprint(info, all)) == info.guest_size);
        const auto clipped = VideoCore::TexelImageCopiesFor(info, vk::ImageAspectFlagBits::eColor, 0x100,
                                                            0x100 + info.guest_size - 1);
        CHECK(clipped.size() == 2);
    }
}

enum class TileOrder {
    Production,          // write into the arena held, FollowMigration, write again into the current one
    NoRewrite,           // FollowMigration only
    RefreshBeforeFollow, // destination refreshed after the wait, then FollowMigration
};

// BufferCache::ObtainBuffer of a texel buffer aliasing an image: residency, uploads (which may wait),
// TileImage (whose tiled path waits before recording its write), then the migration follow-up.
const FakeArena* ModelTexelObtain(World& w, u64 addr, u64 size, const Spans& footprint, u8 image,
                                  const std::function<void()>& during_uploads,
                                  const std::function<void()>& during_tile, TileOrder order) {
    const u64 first = addr >> World::BlockShift;
    const u64 last = (addr + size - 1) >> World::BlockShift;
    FakeArena* arena = w.GetArena(first, last);
    u64 seen = w.generation;
    w.EnsureResident(arena, first, last);
    if (during_uploads) {
        during_uploads();
    }
    const auto tile = [&](FakeArena* target, const std::function<void()>& wait) {
        if (wait) {
            wait();
        }
        if (order == TileOrder::RefreshBeforeFollow) {
            const auto [current, offset] = w.Refresh(target, addr - target->cpu_addr);
            target = const_cast<FakeArena*>(current);
            (void)offset;
        }
        for (const auto& [start, end] : footprint) {
            World::Write(*target, addr + start, end - start, image);
        }
    };
    tile(arena, during_tile);
    while (seen != w.generation) {
        seen = w.generation;
        arena = w.Follow(arena, addr, size, first, last);
        if (order == TileOrder::Production) {
            tile(arena, nullptr);
        }
    }
    return arena;
}

struct TexelScenario {
    u64 boundary;
    u64 addr;
    u64 size;
    Spans footprint;
    u64 window_start;
    u64 window_end;
};

// Arena bytes over the scenario window after the obtain: the image write reaches past the request,
// a fault processed in a wait uploads newer guest data around it.
std::vector<u8> RunTexelScenario(const TexelScenario& s, bool migrate_in_uploads, bool migrate_in_tile, bool chain,
                                 TileOrder order, const FakeArena** result_arena = nullptr, World* out = nullptr) {
    World w;
    const auto [setup, setup_offset] = w.Obtain(s.window_start, s.window_end - s.window_start);
    World::Write(*setup, s.window_start, s.window_end - s.window_start, 0x11);
    (void)setup_offset;
    w.Obtain(s.boundary + 0x100, 0x100);
    if (chain) {
        w.Obtain(s.boundary + World::PageSize + 0x100, 0x100);
    }
    const auto upload_newer = [&] {
        FakeArena* owner = w.owners[s.window_start >> World::PageBits];
        World::Write(*owner, s.window_start, s.addr - s.window_start, 0x22);
        World::Write(*owner, s.addr + s.size, s.window_end - s.addr - s.size, 0x22);
    };
    const auto uploads = [&] {
        if (migrate_in_uploads) {
            w.Obtain(s.boundary - 0x10, 0x20);
        }
    };
    const auto tile_wait = [&] {
        if (migrate_in_tile) {
            w.Obtain(chain ? s.boundary + World::PageSize - 0x10 : s.boundary - 0x10, 0x20);
        }
        upload_newer();
    };
    const FakeArena* arena = ModelTexelObtain(w, s.addr, s.size, s.footprint, 0x99, uploads, tile_wait, order);
    if (result_arena) {
        *result_arena = arena;
    }
    const FakeArena* owner = w.owners[s.addr >> World::PageBits];
    std::vector<u8> bytes;
    for (u64 a = s.window_start; a < s.window_end; ++a) {
        bytes.push_back(World::ByteAt(*owner, a));
    }
    if (out) {
        *out = std::move(w);
    }
    return bytes;
}

void TestTileImageWritesSurviveMigration() {
    // An untiled 16 KiB-pitch image whose texel buffer covers 32 KiB of its 44 KiB footprint, rows
    // with padding both inside and past the request.
    TexelScenario s;
    s.boundary = 0x10'1700'0000;
    s.addr = s.boundary - 0x40000;
    s.size = 0x8000;
    s.footprint = {{0, 0x3000}, {0x4000, 0x7000}, {0x8000, 0x9000}, {0xA000, 0xB000}};
    s.window_start = s.addr - 0x1000;
    s.window_end = s.addr + 0xC000;

    const auto baseline = RunTexelScenario(s, false, false, false, TileOrder::Production);
    const auto at = [&](const std::vector<u8>& bytes, u64 addr) { return bytes[addr - s.window_start]; };
    CHECK(at(baseline, s.addr) == 0x99 && at(baseline, s.addr + 0x8000) == 0x99);
    CHECK(at(baseline, s.addr + 0x9000) == 0x22 && at(baseline, s.addr + 0xB000) == 0x22);
    CHECK(at(baseline, s.addr - 1) == 0x22 && at(baseline, s.addr + 0x3000) == 0x11);

    // A migration in the tile's wait, in the uploads before it, or both (A to B to C): the current
    // arena ends exactly as without a migration, every written byte included.
    for (const auto& [in_uploads, in_tile, chain] :
         {std::tuple{false, true, false}, std::tuple{true, false, false}, std::tuple{true, true, true}}) {
        const FakeArena* arena = nullptr;
        World w;
        const auto bytes = RunTexelScenario(s, in_uploads, in_tile, chain, TileOrder::Production, &arena, &w);
        CHECK(w.generation == (chain ? 2u : 1u));
        CHECK(arena == w.owners[s.addr >> World::PageBits]);
        CHECK(std::ranges::find(w.retired, arena) == w.retired.end());
        CHECK(bytes == baseline);
        if (chain) {
            CHECK(arena->cpu_addr == s.boundary - World::PageSize && arena->size == 3 * World::PageSize);
        }
    }

    // Without the second write the bytes past the request stay in the retired arena; written into
    // the current arena before FollowMigration, the stale copy lands on top of them.
    CHECK(RunTexelScenario(s, false, true, false, TileOrder::NoRewrite) != baseline);
    CHECK(at(RunTexelScenario(s, false, true, false, TileOrder::NoRewrite), s.addr + 0x8000) == 0x22);
    const auto refreshed = RunTexelScenario(s, false, true, false, TileOrder::RefreshBeforeFollow);
    CHECK(refreshed != baseline && at(refreshed, s.addr) == 0x11);
}

void TestTileImageSources() {
    const std::string cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const std::string tile = ReadSource("gpu/shadps4/video_core/texture_cache/tile_manager.cpp");
    const std::string obtain = Body(cache, "std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(");
    CHECK(Ordered(obtain, "const bool image_written =\n        SynchronizeMemory(arena,", "while (generation != arena_generation)"));
    CHECK(Ordered(obtain, "arena = FollowMigration(", "if (image_written) {\n            SynchronizeMemoryFromImage(arena, device_addr, size);"));
    const std::string loop = Block(obtain, "while (generation != arena_generation) {");
    CHECK(Ordered(loop, "FollowMigration(", "SynchronizeMemoryFromImage(arena, device_addr, size);"));
    const std::string from_image = Body(cache, "bool BufferCache::SynchronizeMemoryFromImage(");
    CHECK(Has(from_image, "TexelImageCopiesFor(image.info, image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,"));
    CHECK(Has(from_image, "arena_offset, arena->size_bytes);"));
    CHECK(Ordered(from_image, "TexelImageCopiesFor(", "tile_manager.TileImage(image, buffer_copies, arena, arena_offset);"));
    CHECK(!Has(from_image, "RefreshBinding") && !Has(Body(tile, "void TileManager::TileImage("), "RefreshBinding"));
    // Sparse arenas never migrate, so the follow-up never runs there.
    CHECK(Has(Body(cache, "const Buffer* BufferCache::GetResidentArena("), "++arena_generation;"));
    CHECK(Count(cache, "++arena_generation;") == 1);
}

void TestNoDebugMarkers() {
    for (const char* file : {"gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp",
                             "gpu/shadps4/video_core/buffer_cache/buffer_cache.h",
                             "gpu/shadps4/video_core/buffer_cache/arena_layout.h",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_instance.h"}) {
        const std::string text = ReadSource(file);
        CHECK(!text.empty() && !Has(text, "DEBUG_" "SAULO"));
    }
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
    TestSparseLayoutUnchanged();
    TestResidentLayoutRespectsDriverLimits();
    TestImpossibleResidentLimitsAreRefused();
    TestModeSelection();
    TestRequestInsideOneArenaReusesIt();
    TestSpanningTwoArenasMigrates();
    TestHolesAreNotCopied();
    TestNestedMigrationFollowsStaleBinding();
    TestRetiredArenasLiveUntilRetirement();
    TestOwnedRunsForReadback();
    TestTopOfAddressSpace();
    TestResidentArenasAgainstDriverLimit();
    TestBufferCacheSources();
    TestRasterizerRefreshesBindings();
    TestTextureUploadSourceAcrossMigration();
    TestTextureUploadSources();
    TestIndirectDrawBindOrder();
    TestTileImageFootprint();
    TestTileImageWritesSurviveMigration();
    TestTileImageSources();
    TestNoDebugMarkers();
    std::printf("buffer arena tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
