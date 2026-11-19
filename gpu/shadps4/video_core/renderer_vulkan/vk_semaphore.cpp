// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"
#include "common/gpu_trace.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

Semaphore::Semaphore(const Instance& instance_, Common::GpuTrace::Timeline timeline_)
    : instance{instance_}, timeline{timeline_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Fail(const char* site, s32 result, u64 tick) const {
    Common::GpuTrace::VulkanFailure(site, result, tick, timeline);
    UNREACHABLE_MSG("Timeline semaphore {} failed: {}", site, vk::to_string(vk::Result(result)));
}

vk::Result Semaphore::ReadCounter(u64& counter) const {
    const auto [result, value] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
    counter = value;
    return result;
}

void Semaphore::Refresh() {
    RefreshKnownTick(
        ticks, Common::DeviceLost::Global(), timeline,
        [this](u64& counter) { return ReadCounter(counter); },
        [this](const char* site, s32 result, u64 tick) { Fail(site, result, tick); });
}

void Semaphore::Wait(u64 tick) {
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };
    WaitForTick(
        ticks, Common::DeviceLost::Global(), timeline, tick,
        [&] { return instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT); },
        [this](u64& counter) { return ReadCounter(counter); },
        [this](const char* site, s32 result, u64 failed_tick) { Fail(site, result, failed_tick); });
}

} // namespace Vulkan
