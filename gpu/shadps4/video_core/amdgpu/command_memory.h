// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include "common/types.h"

namespace AmdGpu {

/// The guest command words an indirect buffer packet names; nullopt when the range is at address 0, wraps, is
/// impossibly large or is not wholly mapped (`mapped_bytes(address, bytes)` returns fewer than `bytes`).
template <typename MappedBytes>
std::optional<std::span<const u32>> GuestCommandWords(u64 address, u64 words, MappedBytes&& mapped_bytes) {
    if (words == 0) {
        return std::span<const u32>{};
    }
    constexpr u64 max_words = static_cast<u64>(std::numeric_limits<ptrdiff_t>::max()) / sizeof(u32);
    if (address == 0 || words > max_words) {
        return std::nullopt;
    }
    const u64 bytes = words * sizeof(u32);
    if (address > std::numeric_limits<uintptr_t>::max() - bytes || mapped_bytes(address, bytes) != bytes) {
        return std::nullopt;
    }
    return std::span<const u32>{reinterpret_cast<const u32*>(static_cast<uintptr_t>(address)),
                                static_cast<size_t>(words)};
}

/// The first word of `unread` (command words not read yet) that a write of `bytes` at `address` changes.
inline std::optional<size_t> FirstUnreadWordWritten(std::span<const u32> unread, u64 address, u64 bytes) {
    const u64 begin = reinterpret_cast<u64>(unread.data());
    const u64 end = begin + unread.size_bytes();
    if (unread.empty() || bytes == 0 || address >= end) {
        return std::nullopt;
    }
    const u64 last = bytes - 1 > std::numeric_limits<u64>::max() - address ? std::numeric_limits<u64>::max()
                                                                            : address + (bytes - 1);
    if (last < begin) {
        return std::nullopt;
    }
    return static_cast<size_t>(((address > begin ? address : begin) - begin) / sizeof(u32));
}

} // namespace AmdGpu
