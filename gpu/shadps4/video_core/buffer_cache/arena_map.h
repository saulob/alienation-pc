// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <vector>

#include "common/assert.h"
#include "common/types.h"

namespace VideoCore {

// Sparse buffer arenas indexed by their first page. Each arena covers its page and the next one, so a
// request spanning at most two pages fits one arena and no arena outgrows the driver's buffer size
// limit. A page is covered by up to two arenas, which alias the same resident memory.
template <typename Arena>
class ArenaMap {
public:
    static constexpr u64 PagesPerArena = 2;

    void Configure(u64 num_pages, u32 blocks_per_page_shift) {
        arenas.assign(num_pages, Arena{});
        page_shift = blocks_per_page_shift;
    }

    [[nodiscard]] u64 PageFor(u64 first_block, u64 last_block) const {
        const u64 first_page = first_block >> page_shift;
        ASSERT_MSG((last_block >> page_shift) - first_page < PagesPerArena,
                   "Buffer request cannot span more than two VA arena pages");
        return first_page;
    }

    [[nodiscard]] Arena At(u64 page) const {
        return arenas[page];
    }

    void Set(u64 page, Arena arena) {
        arenas[page] = arena;
    }

    [[nodiscard]] u64 FirstBlock(u64 page) const {
        return page << page_shift;
    }

    [[nodiscard]] u64 EndBlock(u64 page) const {
        return (page + PagesPerArena) << page_shift;
    }

    /// Calls fn(arena, start, end) for each existing arena covering part of blocks [start, end),
    /// with the range clipped to that arena.
    template <typename Fn>
    void ForEachCovering(u64 start, u64 end, Fn&& fn) const {
        if (start >= end) {
            return;
        }
        const u64 first_page = start >> page_shift;
        const u64 last_page = std::min<u64>((end - 1) >> page_shift, arenas.size() - 1);
        for (u64 page = first_page > 0 ? first_page - 1 : 0; page <= last_page; ++page) {
            if (!arenas[page]) {
                continue;
            }
            const u64 begin = std::max(start, FirstBlock(page));
            const u64 stop = std::min(end, EndBlock(page));
            if (begin < stop) {
                fn(arenas[page], begin, stop);
            }
        }
    }

private:
    std::vector<Arena> arenas;
    u32 page_shift{};
};

} // namespace VideoCore
