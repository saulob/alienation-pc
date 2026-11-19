// SPDX-License-Identifier: GPL-2.0-or-later
// Buffer arena geometry and the fully resident arenas used without sparse buffers; free of Vulkan so
// it can be tested headless.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <utility>

#include "common/alignment.h"
#include "common/interval_set.h"
#include "common/types.h"

namespace VideoCore::ArenaLayout {

inline constexpr u64 ADDRESS_SPACE_BITS = 40;
// Sparse arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's Windows
// driver: 2 GiB); an arena spans two pages, so they shrink to half the limit.
inline constexpr u64 MAX_SPARSE_PAGE_BITS = 32;
inline constexpr u64 MIN_SPARSE_PAGE_BITS = 30;
inline constexpr u64 SPARSE_MIN_BLOCK_SIZE = 16_KB;
// Without sparse binding every arena page is allocated whole.
inline constexpr u64 RESIDENT_PAGE_BITS = 24;
inline constexpr u64 MIN_RESIDENT_PAGE_BITS = 20;
// Residency is only bookkeeping there; 64 KiB blocks keep the BDA page table at 128 MiB.
inline constexpr u64 RESIDENT_MIN_BLOCK_SIZE = 64_KB;

/// Sparse arenas need sparsely bound, partially resident buffers, bound on the queue that draws.
constexpr bool SparseBuffersSupported(bool sparse_binding, bool sparse_residency_buffer,
                                      bool queue_sparse_binding) {
    return sparse_binding && sparse_residency_buffer && queue_sparse_binding;
}

enum class ArenaMode {
    Sparse,
    FullyResidentUnsupported,
    FullyResidentForced,
};

constexpr bool IsFullyResident(ArenaMode mode) {
    return mode != ArenaMode::Sparse;
}

/// ALN_FORCE_NON_SPARSE_ARENAS: any non-empty value but "0" selects fully resident arenas.
inline bool NonSparseArenasForced(const char* env_value) {
    return env_value && *env_value && std::strcmp(env_value, "0") != 0;
}

inline ArenaMode SelectArenaMode(bool sparse_supported, const char* force_env_value) {
    if (NonSparseArenasForced(force_env_value)) {
        return ArenaMode::FullyResidentForced;
    }
    return sparse_supported ? ArenaMode::Sparse : ArenaMode::FullyResidentUnsupported;
}

inline ArenaMode SelectArenaModeFromEnvironment(bool sparse_supported) {
    return SelectArenaMode(sparse_supported, std::getenv("ALN_FORCE_NON_SPARSE_ARENAS"));
}

inline std::string FullyResidentStartupLine(ArenaMode mode, u64 arena_mib) {
    const char* reason = mode == ArenaMode::FullyResidentForced ? "ALN_FORCE_NON_SPARSE_ARENAS enabled"
                                                                 : "sparse buffers unavailable";
    return std::string{"Vulkan buffer cache: "} + reason + ", using " + std::to_string(arena_mib) +
           " MiB fully resident arenas";
}

constexpr u64 ResidentArenaLimit(u64 max_buffer_size, u64 max_allocation_size) {
    return std::min(max_buffer_size, max_allocation_size);
}

constexpr u64 PageBits(bool sparse, u64 max_buffer_size, u64 max_allocation_size) {
    if (sparse) {
        u64 bits = MAX_SPARSE_PAGE_BITS;
        while (bits > MIN_SPARSE_PAGE_BITS && (u64{2} << bits) > max_buffer_size) {
            --bits;
        }
        return bits;
    }
    const u64 limit = ResidentArenaLimit(max_buffer_size, max_allocation_size);
    u64 bits = RESIDENT_PAGE_BITS;
    while (bits > MIN_RESIDENT_PAGE_BITS && (u64{1} << bits) > limit) {
        --bits;
    }
    return bits;
}

/// False when even the smallest fully resident page exceeds the driver's limits.
constexpr bool ResidentPageFits(u64 page_bits, u64 max_buffer_size, u64 max_allocation_size) {
    return (u64{1} << page_bits) <= ResidentArenaLimit(max_buffer_size, max_allocation_size);
}

constexpr u64 BlockSize(bool sparse, u64 alignment) {
    const u64 minimum = sparse ? SPARSE_MIN_BLOCK_SIZE : RESIDENT_MIN_BLOCK_SIZE;
    return Common::AlignUp(std::max(alignment, minimum), alignment);
}

constexpr u64 NumPages(u64 page_bits) {
    return u64{1} << (ADDRESS_SPACE_BITS - page_bits);
}

constexpr u64 BlockDeviceAddress(u64 arena_address, u64 arena_cpu_addr, u64 block,
                                 u64 block_shift) {
    return arena_address + ((block << block_shift) - arena_cpu_addr);
}

struct PageRange {
    u64 first;
    u64 end;
};

template <typename Arena>
constexpr PageRange PagesOf(const Arena& arena, u64 page_bits) {
    return {arena.CpuAddr() >> page_bits, (arena.CpuAddr() + arena.SizeBytes()) >> page_bits};
}

/// The fully resident arena holding blocks [first_block, last_block]. When they span more than one
/// arena (or unowned pages), a new arena covering all of them is created through
/// `ops.Create(cpu_addr, size)`; the resident blocks of the arenas it replaces are copied into it
/// (`ops.Copy`), republished (`ops.Publish`, after every copy), and only then do the pages point at
/// it and the old arenas go to `ops.Retire`.
template <typename Arena, typename IV, typename Ops>
Arena* ResidentArenaFor(std::span<Arena*> owners, const IntervalList<IV>& resident, u64 page_bits,
                        u64 block_shift, u64 first_block, u64 last_block, Ops& ops) {
    const u64 blocks_per_page_shift = page_bits - block_shift;
    const u64 first_page = first_block >> blocks_per_page_shift;
    const u64 last_page = last_block >> blocks_per_page_shift;
    Arena* owner = owners[first_page];
    if (owner && owner == owners[last_page]) {
        return owner;
    }

    PageRange pages{first_page, last_page + 1};
    for (u64 page = first_page; page <= last_page; ++page) {
        if (const Arena* arena = owners[page]) {
            const PageRange owned = PagesOf(*arena, page_bits);
            pages.first = std::min(pages.first, owned.first);
            pages.end = std::max(pages.end, owned.end);
        }
    }
    Arena* target = ops.Create(pages.first << page_bits, (pages.end - pages.first) << page_bits);

    const u64 first_moved = pages.first << blocks_per_page_shift;
    const u64 end_moved = pages.end << blocks_per_page_shift;
    const auto for_each_moved = [&](auto&& func) {
        resident.ForEachInRange(first_moved, end_moved, [&](const IV& range) {
            func(std::max(first_moved, range.start), std::min(end_moved, range.end));
        });
    };
    for_each_moved([&](u64 start, u64 end) {
        for (u64 block = start; block < end;) {
            const u64 page = block >> blocks_per_page_shift;
            const u64 chunk_end = std::min(end, (page + 1) << blocks_per_page_shift);
            if (Arena* source = owners[page]; source && source != target) {
                ops.Copy(*source, *target, block, chunk_end);
            }
            block = chunk_end;
        }
    });
    for_each_moved([&](u64 start, u64 end) { ops.Publish(*target, start, end); });

    const Arena* previous = nullptr;
    for (u64 page = pages.first; page < pages.end; ++page) {
        Arena* arena = owners[page];
        if (arena && arena != previous) {
            ops.Retire(*arena);
        }
        previous = arena;
        owners[page] = target;
    }
    return target;
}

/// Where a binding into `arena` lives now: unchanged unless a migration replaced `arena`.
template <typename Arena>
std::pair<const Arena*, u64> CurrentBinding(std::span<Arena* const> owners,
                                            std::span<Arena* const> replaced, u64 page_bits,
                                            const Arena* arena, u64 offset) {
    if (std::find(replaced.begin(), replaced.end(), arena) == replaced.end()) {
        return {arena, offset};
    }
    const u64 addr = arena->CpuAddr() + offset;
    const Arena* owner = owners[addr >> page_bits];
    return {owner, addr - owner->CpuAddr()};
}

template <typename Arena>
struct OwnedRun {
    Arena* arena;
    u64 start;
    u64 end;
};

/// The first part of [start, end) that has an arena, clipped to it; arena is null when none has.
template <typename Arena>
OwnedRun<Arena> NextOwnedRun(std::span<Arena* const> owners, u64 page_bits, u64 start, u64 end) {
    while (start < end) {
        const u64 page = start >> page_bits;
        if (page >= owners.size()) {
            break;
        }
        if (Arena* arena = owners[page]) {
            return {arena, start, std::min(end, arena->CpuAddr() + arena->SizeBytes())};
        }
        start = std::min(end, (page + 1) << page_bits);
    }
    return {nullptr, end, end};
}

} // namespace VideoCore::ArenaLayout
