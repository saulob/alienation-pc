// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

/// The memory types a buffer arena's resident range may be bound to: the preferred device-local
/// type, the other device-local types the arena accepts, then its system memory types.
inline std::vector<u32> BackingMemoryTypes(const vk::PhysicalDeviceMemoryProperties& properties,
                                           u32 allowed_bits, u32 preferred) {
    std::vector<u32> types{preferred};
    for (const bool device_local : {true, false}) {
        for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
            const bool is_local = static_cast<bool>(properties.memoryTypes[i].propertyFlags &
                                                    vk::MemoryPropertyFlagBits::eDeviceLocal);
            if (i != preferred && ((allowed_bits >> i) & 1) != 0 && is_local == device_local) {
                types.push_back(i);
            }
        }
    }
    return types;
}

struct BackingAllocation {
    vk::Result result;
    u32 type;
    u32 attempts;
};

/// Each type is tried once, in order; only a full device memory moves on to the next type.
template <typename Allocate>
BackingAllocation AllocateBacking(std::span<const u32> types, Allocate&& allocate) {
    BackingAllocation allocation{vk::Result::eErrorOutOfDeviceMemory, 0, 0};
    for (const u32 type : types) {
        ++allocation.attempts;
        allocation.type = type;
        allocation.result = allocate(type);
        if (allocation.result != vk::Result::eErrorOutOfDeviceMemory) {
            break;
        }
    }
    return allocation;
}

} // namespace VideoCore
