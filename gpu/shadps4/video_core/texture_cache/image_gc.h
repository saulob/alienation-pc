// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include "common/types.h"

namespace VideoCore {

enum class GcCandidate { Free, WriteBackThenFree, Pinned };

struct GcLimits {
    u64 pressure;
    u64 critical;
};

struct GcRun {
    bool pressured;
    bool aggressive;
    size_t evicted;
};

/// The GC ticks an image must stay unused before it can be evicted: the more pressure, the younger.
constexpr u64 GcMinimumAge(bool pressured, bool aggressive) {
    return aggressive ? 16 : pressured ? 80 : 160;
}

/// One collection over `lru`, oldest first. `usage()` is read again after every eviction,
/// `classify(id)` says what evicting the image takes and `evict(id, write_back)` frees it. Only
/// evictions spend a pass's budget, so pinned images cannot hide younger ones behind them.
template <typename Lru, typename Usage, typename Classify, typename Evict>
GcRun CollectImages(Lru& lru, u64 gc_tick, const GcLimits& limits, Usage&& usage, Classify&& classify,
                    Evict&& evict) {
    GcRun run{};
    u64 min_age = 0;
    size_t budget = 0;
    const auto configure = [&](bool allow_aggressive) {
        run.pressured = usage() >= limits.pressure;
        run.aggressive = allow_aggressive && usage() >= limits.critical;
        min_age = std::min(GcMinimumAge(run.pressured, run.aggressive), gc_tick);
        budget = run.aggressive ? 40 : run.pressured ? 20 : 10;
    };
    const auto clean_up = [&](auto id) {
        if (budget == 0) {
            return true;
        }
        const GcCandidate candidate = classify(id);
        if (candidate == GcCandidate::Pinned ||
            (candidate == GcCandidate::WriteBackThenFree && !run.pressured)) {
            return false;
        }
        --budget;
        evict(id, candidate == GcCandidate::WriteBackThenFree);
        ++run.evicted;
        if (usage() < limits.critical) {
            if (run.aggressive) {
                budget >>= 2;
                run.aggressive = false;
                return false;
            }
            if (run.pressured && usage() < limits.pressure) {
                budget >>= 1;
                run.pressured = false;
            }
        }
        return false;
    };

    configure(false);
    lru.ForEachItemBelow(gc_tick - min_age, clean_up);
    if (usage() >= limits.critical) {
        configure(true);
        lru.ForEachItemBelow(gc_tick - min_age, clean_up);
    }
    return run;
}

} // namespace VideoCore
