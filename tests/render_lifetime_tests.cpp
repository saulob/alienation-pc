// Renderer resource lifetime under graphics load: the texture cache's eviction pass and LRU list, the image
// allocation fallback and the swapchain acquire rule. Production code with stand-in images: no Vulkan device.
#include <atomic>
#include <cstdio>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "common/lru_cache.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_gc.h"

#include <vk_mem_alloc.h>

// Normally the loader's: this test links the video core without it.
extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using namespace VideoCore;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

using Lru = Common::LeastRecentlyUsedCache<u32, u64>;

struct StandInImage {
    u64 bytes;
    bool pinned;
    bool alive;
    size_t lru_id;
};

// Images as the texture cache keeps them: an LRU entry each, usage the sum of the live ones.
struct ImageCache {
    Lru lru;
    std::vector<StandInImage> images;
    u64 usage = 0;

    u32 Add(u64 bytes, bool pinned, u64 tick) {
        const u32 id = static_cast<u32>(images.size());
        images.push_back({bytes, pinned, true, lru.Insert(id, tick)});
        usage += bytes;
        return id;
    }

    GcRun Collect(u64 gc_tick, const GcLimits& limits) {
        return CollectImages(
            lru, gc_tick, limits, [this] { return usage; },
            [this](u32 id) { return images[id].pinned ? GcCandidate::Pinned : GcCandidate::Free; },
            [this](u32 id, bool) {
                StandInImage& image = images[id];
                image.alive = false;
                usage -= image.bytes;
                lru.Free(image.lru_id);
            });
    }

    size_t Live(bool pinned) const {
        size_t count = 0;
        for (const StandInImage& image : images) {
            count += image.alive && image.pinned == pinned;
        }
        return count;
    }
};

void AgesShrinkWithPressure() {
    CHECK(GcMinimumAge(false, false) > GcMinimumAge(true, false));
    CHECK(GcMinimumAge(true, false) > GcMinimumAge(true, true));
}

// ALN-XXJJ7R: over the critical mark, images unused for 50 ticks were never evicted (the aggressive pass
// waited for 160).
void CriticalPressureEvictsRecentImages() {
    ImageCache cache;
    for (int i = 0; i < 20; ++i) {
        cache.Add(100, false, 50);
    }
    const GcRun run = cache.Collect(100, {.pressure = 1000, .critical = 1500});
    CHECK(run.evicted > 0);
    CHECK(cache.usage < 2000);
}

// ALN-XXJJ7R: render targets the GPU wrote and the cache cannot download stay at the head of the LRU list.
// Each one used to spend the pass's budget, so behind ten of them nothing was ever evicted again and the
// evictable images grew without bound. A render target costs more at a higher resolution.
void PinnedImagesDoNotStopEviction() {
    ImageCache cache;
    const GcLimits limits{.pressure = 20000, .critical = 24000};
    constexpr u64 trigger = 16000;
    constexpr u64 ticks = 4000;
    size_t worst = 0;
    for (u64 tick = 1; tick <= ticks; ++tick) {
        cache.Add(15, true, tick);
        for (int i = 0; i < 3; ++i) {
            cache.Add(30, false, tick);
        }
        if (cache.usage >= trigger) {
            cache.Collect(tick, limits);
        }
        if (tick > ticks / 2) {
            worst = std::max(worst, cache.Live(false));
        }
    }
    std::printf("  stress: %llu ticks, %zu images created, %zu evictable live at most in the second half, "
                "%zu pinned live\n",
                (unsigned long long)ticks, cache.images.size(), worst, cache.Live(true));
    CHECK(worst <= 3 * (GcMinimumAge(false, false) + 8));
    CHECK(cache.Live(true) == ticks);
    CHECK(cache.lru.Size() == cache.Live(true) + cache.Live(false));
}

void FreedEntriesStayFreed() {
    Lru lru;
    const size_t a = lru.Insert(1, 1);
    const size_t b = lru.Insert(2, 2);
    lru.Free(a);
    lru.Touch(a, 10);
    lru.Free(a);
    CHECK(lru.Size() == 1);
    const size_t c = lru.Insert(3, 3);
    const size_t d = lru.Insert(4, 4);
    CHECK(c == a);
    CHECK(d != a && d != b);
    std::vector<u32> seen;
    lru.ForEachItemBelow(100, [&](u32 obj) { seen.push_back(obj); });
    CHECK((seen == std::vector<u32>{2, 3, 4}));
}

void ScanStopsWhenAsked() {
    Lru lru;
    for (u32 i = 0; i < 10; ++i) {
        lru.Insert(i, i);
    }
    int calls = 0;
    lru.ForEachItemBelow(100, [&](u32) { return ++calls == 3; });
    CHECK(calls == 3);
}

// ALN-XXJJ7R: the GPU thread touches images while a guest thread unmaps (frees) others.
void ConcurrentTouchAndFreeKeepTheListWhole() {
    Lru lru;
    constexpr u32 count = 256;
    std::vector<size_t> ids(count);
    for (u32 i = 0; i < count; ++i) {
        ids[i] = lru.Insert(i, 0);
    }
    std::atomic<bool> stop{false};
    std::thread toucher([&] {
        std::mt19937 random{7};
        u64 tick = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            lru.Touch(ids[random() % count], tick++);
        }
    });
    std::mt19937 random{11};
    for (int round = 0; round < 200000; ++round) {
        const u32 slot = random() % count;
        lru.Free(ids[slot]);
        ids[slot] = lru.Insert(slot, 0);
    }
    stop = true;
    toucher.join();

    std::set<u32> seen;
    size_t walked = 0;
    lru.ForEachItemBelow(UINT64_MAX >> 1, [&](u32 obj) {
        ++walked;
        seen.insert(obj);
    });
    CHECK(walked == count);
    CHECK(seen.size() == count);
    CHECK(lru.Size() == count);
}

struct Attempt {
    bool within_budget;
    VmaMemoryUsage usage;
};

std::vector<Attempt> AllocateWith(int successful_attempt) {
    std::vector<Attempt> attempts;
    const vk::ImageCreateInfo info{.extent = {2560, 1440, 1}};
    const VkResult result = AllocateImageWithFallback(info, [&](const VmaAllocationCreateInfo& alloc) {
        attempts.push_back({(alloc.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) != 0, alloc.usage});
        return int(attempts.size()) == successful_attempt ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    CHECK((result == VK_SUCCESS) == (successful_attempt <= 3));
    return attempts;
}

// ALN-XXJJ7R: the presenter's frame images ended the game on the first budget refusal.
void FrameImagesFallBackLikeOtherImages() {
    const auto first = AllocateWith(1);
    CHECK(first.size() == 1 && first[0].within_budget && first[0].usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    const auto over_budget = AllocateWith(2);
    CHECK(over_budget.size() == 2 && !over_budget[1].within_budget &&
          over_budget[1].usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    const auto system = AllocateWith(3);
    CHECK(system.size() == 3 && system[2].usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    CHECK(AllocateWith(4).size() == 3);
}

// ALN-XXJJ7R: a suboptimal acquire hands out an image and signals its semaphore; recreating the swapchain
// before presenting it destroyed a semaphore with a pending signal and reused it on the next acquire.
void SuboptimalImagesArePresented() {
    CHECK(Vulkan::AcquireHandsOutImage(vk::Result::eSuccess));
    CHECK(Vulkan::AcquireHandsOutImage(vk::Result::eSuboptimalKHR));
    CHECK(!Vulkan::AcquireHandsOutImage(vk::Result::eErrorOutOfDateKHR));
    CHECK(!Vulkan::AcquireHandsOutImage(vk::Result::eErrorSurfaceLostKHR));
    CHECK(!Vulkan::AcquireHandsOutImage(vk::Result::eErrorDeviceLost));
    CHECK(!Vulkan::AcquireHandsOutImage(vk::Result::eTimeout));
}

} // namespace

int main() {
    AgesShrinkWithPressure();
    CriticalPressureEvictsRecentImages();
    PinnedImagesDoNotStopEviction();
    FreedEntriesStayFreed();
    ScanStopsWhenAsked();
    ConcurrentTouchAndFreeKeepTheListWhole();
    FrameImagesFallBackLikeOtherImages();
    SuboptimalImagesArePresented();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
