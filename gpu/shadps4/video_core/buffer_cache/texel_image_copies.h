// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <boost/container/small_vector.hpp>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {

using TexelImageCopies = boost::container::small_vector<vk::BufferImageCopy, 8>;

/// The mips of an image a texel buffer read writes into its arena at `arena_offset`, relative to
/// the image's guest address: every mip up to the first that would end past the arena.
inline TexelImageCopies TexelImageCopiesFor(const ImageInfo& info, vk::ImageAspectFlags aspect,
                                            u64 arena_offset, u64 arena_size) {
    TexelImageCopies copies;
    for (u32 mip = 0; mip < info.resources.levels; mip++) {
        const auto& mip_info = info.mips_layout[mip];
        const u32 width = std::max(info.size.width >> mip, 1u);
        const u32 height = std::max(info.size.height >> mip, 1u);
        const u32 depth = std::max(info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena_size) {
            break;
        }
        copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = aspect,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    return copies;
}

} // namespace VideoCore
