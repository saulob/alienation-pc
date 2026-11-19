// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <array>
#include <map>
#include <tuple>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "aln_copy_pool.h"
#include "aln_toggles.h"
#include "common/alignment.h"
#include "common/device_lost.h"
#include "common/gpu_trace.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/backing_memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/texel_image_copies.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance, Common::GpuTrace::Timeline::SparseBind} {
    const auto arena_mode =
        ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferSupported());
    sparse_arenas = !ArenaLayout::IsFullyResident(arena_mode);
    const u64 max_buffer_size = instance.GetMaxBufferSize();
    const u64 max_allocation_size = instance.MaxMemoryAllocationSize();
    arena_page_bits = ArenaLayout::PageBits(sparse_arenas, max_buffer_size, max_allocation_size);
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = ArenaLayout::NumPages(arena_page_bits);
    if (!sparse_arenas) {
        ASSERT_MSG(ArenaLayout::ResidentPageFits(arena_page_bits, max_buffer_size,
                                                 max_allocation_size),
                   "Vulkan buffer cache: sparse buffers unavailable and the driver allows only "
                   "{} MiB buffers and {} MiB allocations, below the {} MiB fully resident arena",
                   max_buffer_size >> 20, max_allocation_size >> 20, arena_page_size >> 20);
        std::printf("%s\n",
                    ArenaLayout::FullyResidentStartupLine(arena_mode, arena_page_size >> 20).c_str());
    } else if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, max_buffer_size >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags = sparse_arenas ? vk::BufferCreateFlagBits::eSparseBinding |
                                     vk::BufferCreateFlagBits::eSparseResidency
                               : vk::BufferCreateFlags{},
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    const u64 arena_block_size = ArenaLayout::BlockSize(sparse_arenas, reqs.alignment);
    ASSERT_MSG(std::popcount(arena_block_size) == 1 && arena_block_size <= arena_page_size,
               "Arena block size {} is not a power of 2 within a {} MiB arena page",
               arena_block_size, arena_page_size >> 20);
    block_size = static_cast<u32>(arena_block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    if (sparse_arenas) {
        arena_map.Configure(num_arena_pages, blocks_per_arena_page_shift);
        arena_memory_type_index =
            FindMemoryType(instance.GetMemoryProperties(),
                           vk::MemoryPropertyFlagBits::eDeviceLocal, reqs.memoryTypeBits)
                .value();
        backing_memory_types = BackingMemoryTypes(
            instance.GetMemoryProperties(), reqs.memoryTypeBits, arena_memory_type_index);
    } else {
        resident_owners.assign(num_arena_pages, nullptr);
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                 MemoryType::DeviceLocal, "Null Index Buffer");
    runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// aln: games fill their per-frame buffers (constants, skinning output) sequentially, and a
// protection fault per 4 KiB page is expensive. A fault unprotects the aligned window around it
// instead; pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const AlnCopyPool::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const AlnCopyPool::Item& item) {
    // Only the recording thread may Record() or use its copy batch. A draw preparation worker
    // obtaining a buffer copies at once, before the buffer is returned: recording from a worker
    // while the GPU thread handed its chunk to the recorder writes into a null chunk, and a
    // worker's thread-local copy batch could stay unflushed.
    if (!scheduler.OwnsRecording() && scheduler.IsRecordingDeferred()) {
        item.run(item);
        return;
    }
    if (scheduler.IsRecordingDeferred() && !AlnToggle::Disabled(AlnToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    AlnCopyPool::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static constexpr u64 window = 256 * 1024;
    if (window <= TRACKER_BYTES_PER_PAGE || AlnToggle::Disabled(AlnToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        if (!sparse_arenas) {
            const VAddr window_start = Common::AlignDown(device_addr, WindowSize);
            DownloadOwnedRange(window_start,
                               std::max<VAddr>(window_start + WindowSize, device_addr + size));
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
            return;
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

// GPU-modified data only lives in pages that already have an arena. Owners are looked up again
// for every run: a download waits for the GPU, and faults processed then can migrate arenas.
void BufferCache::DownloadOwnedRange(VAddr start, VAddr end) {
    for (;;) {
        const auto run =
            ArenaLayout::NextOwnedRun<Buffer>(resident_owners, arena_page_bits, start, end);
        if (!run.arena) {
            return;
        }
        DownloadMemory(run.arena, run.start, run.end - run.start);
        start = run.end;
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        // aln: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !AlnToggle::Disabled(AlnToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    u64 generation = arena_generation;
    EnsureResident(arena, first_block, last_block);
    const bool image_written =
        SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    while (generation != arena_generation) {
        generation = arena_generation;
        arena = FollowMigration(arena, device_addr, size, first_block, last_block);
        // The image write can reach past the requested range, which FollowMigration does not
        // move; written again after that copy, it lands in the current arena on top of it.
        if (image_written) {
            SynchronizeMemoryFromImage(arena, device_addr, size);
        }
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!AlnToggle::Disabled(AlnToggle::DeferredUploads)) {
        // aln: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const AlnCopyPool::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                AlnCopyPool::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(GetArena(range.start, range.end - 1), device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    if (!sparse_arenas) {
        return GetResidentArena(first_block, last_block);
    }
    const u64 page = arena_map.PageFor(first_block, last_block);
    if (const Buffer* arena = arena_map.At(page)) {
        return arena;
    }
    const u64 base_block = arena_map.FirstBlock(page);
    const u64 end_block = arena_map.EndBlock(page);
    const Buffer* arena = &arenas.emplace_back(instance, base_block << block_shift,
                                               (end_block - base_block) << block_shift,
                                               MemoryType::Sparse);
    arena_map.Set(page, arena);

    u64 aliased_ranges = 0;
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        BindsForArena(arena)->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
        ++aliased_ranges;
    });
    Common::GpuTrace::Record(Common::GpuTrace::Event::ArenaCreated, arena->cpu_addr,
                             arena->size_bytes, aliased_ranges);
    return arena;
}

const Buffer* BufferCache::GetResidentArena(u64 first_block, u64 last_block) {
    struct Ops {
        BufferCache& cache;
        const Buffer* source = nullptr;
        boost::container::small_vector<vk::BufferCopy, 16> copies;
        boost::container::small_vector<Interval, 16> published;
        bool created = false;

        Buffer* Create(VAddr cpu_addr, u64 size) {
            const u64 limit = ArenaLayout::ResidentArenaLimit(
                cache.instance.GetMaxBufferSize(), cache.instance.MaxMemoryAllocationSize());
            ASSERT_MSG(size <= limit,
                       "Fully resident buffer arena of {} MiB at {:#x} exceeds the driver limit "
                       "of {} MiB",
                       size >> 20, cpu_addr, limit >> 20);
            created = true;
            return &cache.arenas.emplace_back(cache.instance, cpu_addr, size,
                                              MemoryType::DeviceLocal);
        }
        void Copy(const Buffer& from, const Buffer& to, u64 start, u64 end) {
            if (&from != source) {
                FlushCopies(to);
                source = &from;
                // Shaders also write arenas through the BDA page table, unseen by the tracker.
                cache.runtime.AccessBuffer(source, 0, source->SizeBytes(),
                                           vk::PipelineStageFlagBits2::eAllCommands,
                                           vk::AccessFlagBits2::eMemoryWrite);
            }
            const VAddr addr = start << cache.block_shift;
            const u64 size = (end - start) << cache.block_shift;
            if (!copies.empty() && copies.back().srcOffset + copies.back().size == from.Offset(addr)) {
                copies.back().size += size;
                return;
            }
            copies.push_back({from.Offset(addr), to.Offset(addr), size});
        }
        void FlushCopies(const Buffer& to) {
            if (!copies.empty()) {
                cache.runtime.CopyBuffer(source, &to, copies);
                copies.clear();
            }
        }
        void Publish(const Buffer&, u64 start, u64 end) {
            published.push_back({start, end});
        }
        void Retire(Buffer& arena) {
            cache.retired_arenas.push_back(&arena);
        }
    } ops{*this};

    const size_t retired = retired_arenas.size();
    const Buffer* arena = ArenaLayout::ResidentArenaFor<Buffer>(
        resident_owners, resident_ranges, arena_page_bits, block_shift, first_block, last_block,
        ops);
    if (ops.created) {
        ops.FlushCopies(*arena);
        if (!ops.published.empty()) {
            PublishArenaBda(arena, {ops.published.data(), ops.published.size()});
            runtime.FlushBarriers();
        }
        Common::GpuTrace::Record(Common::GpuTrace::Event::ArenaCreated, arena->cpu_addr,
                                 arena->size_bytes, ops.published.size());
    }
    if (retired_arenas.size() != retired) {
        ++arena_generation;
    }
    return arena;
}

// A migration while the caller waited replaced `stale` after the caller got it; what the caller
// wrote into it since is copied over.
const Buffer* BufferCache::FollowMigration(const Buffer* stale, VAddr device_addr, u32 size,
                                           u64 first_block, u64 last_block) {
    const Buffer* current = GetArena(first_block, last_block);
    if (current == stale) {
        return current;
    }
    EnsureResident(current, first_block, last_block);
    const vk::BufferCopy copy{stale->Offset(device_addr), current->Offset(device_addr), size};
    runtime.CopyBuffer(stale, current, {&copy, 1});
    const Interval blocks{first_block, last_block + 1};
    PublishArenaBda(current, {&blocks, 1});
    runtime.FlushBarriers();
    return current;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    AlnStats::Timer timer{AlnStats::t_resident};

    if (!sparse_arenas) {
        for (const auto& range : bind_ranges) {
            resident_ranges.Add(Backing{{range.start, range.end}, {}, 0});
        }
        PublishArenaBda(arena, {bind_ranges.begin(), bind_ranges.end()});
        return;
    }

    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    vk::MemoryAllocateInfo alloc_info = {
        .pNext = &alloc_flags,
        .allocationSize = resident_blocks << block_shift,
    };
    vk::DeviceMemory device_memory{};
    const BackingAllocation allocation = AllocateBacking(backing_memory_types, [&](u32 type) {
        alloc_info.memoryTypeIndex = type;
        const auto [result, memory] = instance.GetDevice().allocateMemory(alloc_info);
        device_memory = memory;
        return result;
    });
    Vulkan::Check(allocation.result);
    if (allocation.type != arena_memory_type_index) {
        Common::GpuTrace::Record(Common::GpuTrace::Event::AllocationFallback, alloc_info.allocationSize,
                                 allocation.type,
                                 u64(Common::GpuTrace::Fallback::SparseBackingInSystemMemory));
        LOG_WARNING(Render_Vulkan, "Buffer arena backing of {} KiB placed in memory type {}: device memory is full",
                    alloc_info.allocationSize >> 10, allocation.type);
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        // Every arena covering the range must see it, not only the one that asked.
        arena_map.ForEachCovering(range.start, range.end,
                                  [&](const Buffer* covering, u64 start, u64 end) {
                                      BindsForArena(covering)->binds.push_back(vk::SparseMemoryBind{
                                          .resourceOffset = (start << block_shift) - covering->cpu_addr,
                                          .size = (end - start) << block_shift,
                                          .memory = device_memory,
                                          .memoryOffset =
                                              memory_offset + ((start - range.start) << block_shift),
                                      });
                                  });
        const u64 range_offset = (range.start << block_shift) - arena->cpu_addr;
        const u64 range_size = (range.end - range.start) << block_shift;
        memory_offset += range_size;

        for (u64 block = 0; block < range_size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + range_offset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    AlnStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // aln: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!AlnToggle::Disabled(AlnToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!AlnToggle::Disabled(AlnToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            AlnCopyPool::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        AlnCopyPool::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else if (*type == TextureCache::MetaType::CMask) {
            // aln: color images are always fully expanded on the host, so every 4-bit CMask
            // tile code reads as 0xF (uncompressed: the color in memory is valid). A game reading
            // CMask through buffers would otherwise treat stale tiles as fast-cleared.
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, 0xFFFFFFFFu);
            return true;
        } else {
            // Once per type: a game can read metadata through buffers every frame.
            static std::atomic<u32> warned{0};
            const u32 bit = 1u << static_cast<u32>(*type);
            if (!(warned.fetch_or(bit) & bit)) {
                LOG_WARNING(Render_Vulkan, "Unhandled metadata type {} (reported once)",
                            magic_enum::enum_name(*type));
            }
        }
    }
    // aln: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !AlnToggle::Disabled(AlnToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // aln: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    auto buffer_copies =
        TexelImageCopiesFor(image.info, image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                            arena_offset, arena->size_bytes);
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::PublishArenaBda(const Buffer* arena, std::span<const Interval> ranges) {
    u64 num_blocks = 0;
    for (const auto& range : ranges) {
        num_blocks += range.end - range.start;
    }
    if (num_blocks == 0) {
        return;
    }
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(num_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    const vk::DeviceAddress arena_address = arena->BufferDeviceAddress();
    u64 offset = staging.offset;
    for (const auto& range : ranges) {
        for (u64 block = range.start; block < range.end; ++block) {
            *(bda_addrs++) = ArenaLayout::BlockDeviceAddress(arena_address, arena->cpu_addr, block,
                                                             block_shift);
        }
        const u64 copy_size = (range.end - range.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, range.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }
    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

void BufferCache::RefreshBinding(const Buffer*& buffer, u64& offset) const {
    std::tie(buffer, offset) = ArenaLayout::CurrentBinding<Buffer>(
        resident_owners, retired_arenas, arena_page_bits, buffer, offset);
}

void BufferCache::RefreshBinding(vk::Buffer& handle, vk::DeviceSize& offset) const {
    if (!handle) {
        return;
    }
    for (const Buffer* arena : retired_arenas) {
        if (arena->Handle() == handle) {
            const Buffer* current = arena;
            u64 current_offset = offset;
            RefreshBinding(current, current_offset);
            handle = current->Handle();
            offset = current_offset;
            return;
        }
    }
}

void BufferCache::RetireMigratedArenas() {
    if (retired_arenas.empty()) {
        return;
    }
    // The deque entries stay: their addresses are never reused by a new arena.
    scheduler.DeferOperation([retired = std::move(retired_arenas)] {
        for (Buffer* arena : retired) {
            arena->buffer.Destroy();
        }
    });
    retired_arenas.clear();
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    const s32 submit_result = Common::DeviceLost::CallWhileHealthy(
        Common::DeviceLost::Global(), Common::DeviceLost::Operation::QueueBindSparse, "sparse",
        signal_tick, Common::GpuTrace::Timeline::SparseBind, [&] {
            const vk::Result result = instance.GetGraphicsQueue().bindSparse(sparse_info);
            Common::GpuTrace::Record(Common::GpuTrace::Event::SparseBind, signal_tick,
                                     buffer_binds.size(), u32(result));
            Common::GpuTrace::SetSubmittedTick(Common::GpuTrace::Timeline::SparseBind, signal_tick);
            return result;
        });
    if (submit_result != Common::DeviceLost::Success) {
        Common::GpuTrace::VulkanFailure("sparse", submit_result, signal_tick,
                                        Common::GpuTrace::Timeline::SparseBind);
    }
    ASSERT_MSG(submit_result == Common::DeviceLost::Success, "Sparse bind failed: {}",
               vk::to_string(vk::Result(submit_result)));

    pending_binds.clear();
}

} // namespace VideoCore
