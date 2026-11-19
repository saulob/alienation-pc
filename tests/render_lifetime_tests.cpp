// Renderer resource lifetime under graphics load: the texture cache's eviction pass and LRU list, sampler lifetime,
// the image allocation fallback and the swapchain acquire rule. Production code with stand-in images: no Vulkan device.
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
        return CollectUnused(
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

// ALN-MKL8FP: the report's thresholds (trigger 0, pressure 2608 MiB, critical 5216 MiB) with 1.9 GiB of
// non-image usage keep every pass pressured and never critical. Forty render targets of an earlier area sit
// at the head of the LRU list, more than a pressured pass's budget, while streamed textures keep arriving.
// Every pressured pass that has an idle evictable image behind them must evict, and the cache must shrink.
void PressuredPassesReachPastPinnedPrefix() {
    constexpr u64 MiB = 1ull << 20;
    constexpr u64 base = 1910 * MiB;
    const GcLimits limits{.pressure = 2608 * MiB - base, .critical = 5216 * MiB - base};
    constexpr u64 ticks = 6000;
    ImageCache cache;
    std::vector<u64> used_until;
    std::mt19937_64 random{0x4D4B4C38};
    const auto add = [&](u64 bytes, bool pinned, u64 tick, u64 use_for) {
        cache.Add(bytes, pinned, tick);
        used_until.push_back(tick + use_for);
    };
    for (int i = 0; i < 40; ++i) {
        add(4 * MiB, true, 0, 0);
    }
    for (int i = 0; i < 979; ++i) {
        add(MiB, false, 1, 50 + random() % 400);
    }
    const u64 initial_usage = cache.usage;
    size_t stalled_passes = 0, critical_passes = 0, evictions = 0, late_peak = 0;
    for (u64 tick = 2; tick <= ticks; ++tick) {
        if (tick % 4 == 0) {
            add(MiB + (random() % 512) * 1024, false, tick, 30 + random() % 600);
        }
        if (tick % 250 == 0) {
            add(4 * MiB, true, tick, 0);
        }
        for (u32 id = 0; id < cache.images.size(); ++id) {
            const StandInImage& image = cache.images[id];
            if (image.alive && !image.pinned && used_until[id] >= tick && (random() & 7) == 0) {
                cache.lru.Touch(image.lru_id, tick);
            }
        }
        const bool pressured = cache.usage >= limits.pressure;
        critical_passes += cache.usage >= limits.critical;
        bool idle_evictable = false;
        cache.lru.ForEachItemBelow(tick - GcMinimumAge(true, false), [&](u32 id) {
            idle_evictable = !cache.images[id].pinned;
            return idle_evictable;
        });
        const GcRun run = cache.Collect(tick, limits);
        evictions += run.evicted;
        stalled_passes += pressured && idle_evictable && run.evicted == 0;
        if (tick > ticks / 2) {
            late_peak = std::max(late_peak, cache.Live(false));
        }
    }
    std::printf("  ALN-MKL8FP: %zu evictions, %zu stalled pressured passes, %zu evictable live at most in the second "
                "half, %zu pinned live, %llu MiB cached at the end (%llu at the start)\n",
                evictions, stalled_passes, late_peak, cache.Live(true), (unsigned long long)(cache.usage / MiB),
                (unsigned long long)(initial_usage / MiB));
    CHECK(critical_passes == 0);
    CHECK(stalled_passes == 0);
    CHECK(cache.usage < limits.pressure);
    CHECK(late_peak < 979);
    CHECK(cache.Live(true) == 40 + ticks / 250);
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

// Samplers as the texture cache keeps them: each draw that binds one stamps it with the draw tick being recorded;
// the collector may only destroy one whose last draw the GPU has finished.
struct SamplerCache {
    Lru lru;
    std::vector<u64> last_use;
    std::vector<size_t> lru_ids;
    std::vector<bool> alive;
    u64 live = 0;
    u64 gpu_done = 0;
    size_t destroyed_in_flight = 0;

    u32 Create(u64 draw_tick, u64 gc_tick) {
        const u32 id = static_cast<u32>(last_use.size());
        last_use.push_back(draw_tick);
        lru_ids.push_back(lru.Insert(id, gc_tick));
        alive.push_back(true);
        ++live;
        return id;
    }

    void Use(u32 id, u64 draw_tick, u64 gc_tick) {
        last_use[id] = draw_tick;
        lru.Touch(lru_ids[id], gc_tick);
    }

    void Collect(u64 gc_tick, const GcLimits& limits) {
        CollectUnused(
            lru, gc_tick, limits, [this] { return live; },
            [this](u32 id) { return gpu_done >= last_use[id] ? GcCandidate::Free : GcCandidate::Pinned; },
            [this](u32 id, bool) {
                destroyed_in_flight += last_use[id] > gpu_done;
                alive[id] = false;
                lru.Free(lru_ids[id]);
                --live;
            });
    }
};

// ALN-GKAQG9: samplers used to be destroyed on LRU age alone, while a recorded draw could still use them.
void SamplersOutliveTheirDraws() {
    SamplerCache cache;
    const GcLimits limits{.pressure = 5, .critical = 8};
    const u32 in_flight = cache.Create(100, 0);
    for (int i = 0; i < 9; ++i) {
        cache.Create(50, 0);
    }
    cache.gpu_done = 99;
    cache.Collect(1000, limits);
    CHECK(cache.alive[in_flight]);
    CHECK(cache.destroyed_in_flight == 0);
    cache.gpu_done = 100;
    cache.Collect(2000, limits);
    CHECK(!cache.alive[in_flight]);
}

void SamplerStressStaysBoundedAndSafe() {
    SamplerCache cache;
    const GcLimits limits{.pressure = 3500, .critical = 3750};
    constexpr u64 trigger = 3000;
    std::mt19937 random{3};
    std::vector<u32> hot;
    for (int i = 0; i < 64; ++i) {
        hot.push_back(cache.Create(1, 0));
    }
    u64 worst = 0;
    for (u64 gc_tick = 1; gc_tick <= 6000; ++gc_tick) {
        const u64 draw_tick = gc_tick * 3;
        for (u32 id : hot) {
            cache.Use(id, draw_tick, gc_tick);
        }
        for (int i = 0; i < 8; ++i) {
            cache.Create(draw_tick - random() % 3, gc_tick);
        }
        cache.gpu_done = draw_tick - 4;
        if (cache.live >= trigger) {
            cache.Collect(gc_tick, limits);
        }
        worst = std::max(worst, cache.live);
    }
    std::printf("  samplers: %zu created, %llu live at most, %zu destroyed while in use\n", cache.last_use.size(),
                (unsigned long long)worst, cache.destroyed_in_flight);
    CHECK(cache.destroyed_in_flight == 0);
    CHECK(worst < 4000);
    for (u32 id : hot) {
        CHECK(cache.alive[id]);
    }
}

} // namespace

int main() {
    AgesShrinkWithPressure();
    CriticalPressureEvictsRecentImages();
    PinnedImagesDoNotStopEviction();
    PressuredPassesReachPastPinnedPrefix();
    FreedEntriesStayFreed();
    ScanStopsWhenAsked();
    ConcurrentTouchAndFreeKeepTheListWhole();
    FrameImagesFallBackLikeOtherImages();
    SuboptimalImagesArePresented();
    SamplersOutliveTheirDraws();
    SamplerStressStaysBoundedAndSafe();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
