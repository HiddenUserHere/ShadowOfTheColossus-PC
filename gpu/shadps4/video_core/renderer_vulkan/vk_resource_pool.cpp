// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_heap.h" // sotc: SOTC_DESCRIPTOR_HEAP
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "../../../sotc/nonblocking.h" // sotc: SOTC_BIND_DESC_REUSE

namespace Vulkan {

ResourcePool::ResourcePool(Semaphore* work_semaphore_, std::size_t grow_step_)
    : work_semaphore{work_semaphore_}, grow_step{grow_step_} {}

std::size_t ResourcePool::CommitResource() {
    u64 gpu_tick = work_semaphore->KnownGpuTick();
    const auto search = [this, &gpu_tick](std::size_t begin,
                                         std::size_t end) -> std::optional<std::size_t> {
        for (std::size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = work_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };

    // Try to find a free resource from the hinted position to the end.
    auto found = search(hint_iterator, ticks.size());
    if (!found) {
        // Refresh semaphore to query updated results
        work_semaphore->Refresh();
        gpu_tick = work_semaphore->KnownGpuTick();
        found = search(hint_iterator, ticks.size());
    }
    if (!found) {
        // Search from beginning to the hinted position.
        found = search(0, hint_iterator);
        if (!found) {
            // Both searches failed, the pool is full; handle it.
            const std::size_t free_resource = ManageOverflow();

            ticks[free_resource] = work_semaphore->CurrentTick();
            found = free_resource;
        }
    }

    // Free iterator is hinted to the resource after the one that's been commited.
    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

std::size_t ResourcePool::ManageOverflow() {
    const std::size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
    return old_capacity;
}

constexpr std::size_t COMMAND_BUFFER_POOL_SIZE = 4;

CommandPool::CommandPool(const Instance& instance, Semaphore* work_semaphore,
                         vk::CommandBufferLevel level_)
    : ResourcePool{work_semaphore, COMMAND_BUFFER_POOL_SIZE}, instance{instance}, level{level_} {
    const vk::CommandPoolCreateInfo pool_create_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    const vk::Device device = instance.GetDevice();
    auto [pool_result, pool] = device.createCommandPoolUnique(pool_create_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create command pool: {}",
               vk::to_string(pool_result));
    cmd_pool = std::move(pool);
    SetObjectName(device, *cmd_pool, "CommandPool");
}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(std::size_t begin, std::size_t end) {
    cmd_buffers.resize(end);

    const vk::CommandBufferAllocateInfo buffer_alloc_info = {
        .commandPool = *cmd_pool,
        .level = level,
        .commandBufferCount = COMMAND_BUFFER_POOL_SIZE,
    };

    const vk::Device device = instance.GetDevice();
    const auto result =
        device.allocateCommandBuffers(&buffer_alloc_info, cmd_buffers.data() + begin);
    ASSERT(result == vk::Result::eSuccess);

    for (std::size_t i = begin; i < end; ++i) {
        SetObjectName(device, cmd_buffers[i], "CommandPool: Command Buffer {}", i);
    }
}

vk::CommandBuffer CommandPool::Commit() {
    const std::size_t index = CommitResource();
    return cmd_buffers[index];
}

std::size_t CommandPool::IndexOf(vk::CommandBuffer cmd) const {
    for (std::size_t i = 0; i < cmd_buffers.size(); ++i) {
        if (cmd_buffers[i] == cmd) {
            return i;
        }
    }
    ASSERT_MSG(false, "CommandPool: command buffer is not from this pool");
    return 0;
}

void CommandPool::Hold(vk::CommandBuffer cmd) {
    ticks[IndexOf(cmd)] = std::numeric_limits<u64>::max();
}

void CommandPool::Release(vk::CommandBuffer cmd, u64 tick) {
    ticks[IndexOf(cmd)] = tick;
}

DescriptorHeap::DescriptorHeap(const Instance& instance, Scheduler& scheduler,
                               std::span<const vk::DescriptorPoolSize> pool_sizes_,
                               u32 descriptor_heap_count_)
    : device{instance.GetDevice()}, work_semaphore{scheduler.GetWorkSemaphore()},
      descriptor_heap_count{descriptor_heap_count_}, pool_sizes{pool_sizes_} {
    CreateDescriptorPool();
    // sotc: with descriptor buffers the sets are not pooled; the ring replaces Commit.
    if (instance.IsDescriptorBufferEnabled() || instance.IsDescriptorBufferN5Enabled()) { // sotc: SOTC_N5_DESCBUF
        descriptor_buffer = std::make_unique<DescriptorBufferRing>(instance, scheduler);
    }
    // sotc: SOTC_DESCRIPTOR_HEAP=1: the heaps are allocated and probed here, and the probe logs the outcome. When
    // the probe fails the pointer is reset and the port stays on the classic path.
    if (instance.IsDescriptorHeapEnabled()) {
        auto heap_buffers =
            std::make_unique<DescriptorHeapBuffers>(instance, instance.GetDescriptorHeapProperties());
        if (heap_buffers->Ok()) {
            descriptor_heap_buffers = std::move(heap_buffers);
        }
    }
}

DescriptorHeap::~DescriptorHeap() {
    device.destroyDescriptorPool(curr_pool);
    for (const auto pool : persistent_pools) { // sotc: SOTC_BIND_DESC_REUSE
        device.destroyDescriptorPool(pool);
    }
    for (const auto [pool, tick] : pending_pools) {
        work_semaphore->Wait(tick);
        device.destroyDescriptorPool(pool);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout set_layout) {
    const u64 set_key = std::bit_cast<u64>(set_layout);
    const auto [it, _] = descriptor_sets.try_emplace(set_key);

    // Check if allocated sets exist and pick one.
    if (!it->second.empty()) {
        const auto desc_set = it->second.back();
        it.value().pop_back();
        return desc_set;
    }

    DescSetBatch desc_sets(DescriptorSetBatch);
    std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
    layouts.fill(set_layout);

    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = DescriptorSetBatch,
        .pSetLayouts = layouts.data(),
    };

    // Attempt to allocate the descriptor set batch.
    auto result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    if (result == vk::Result::eSuccess) {
        const auto desc_set = desc_sets.back();
        desc_sets.pop_back();
        it.value() = std::move(desc_sets);
        return desc_set;
    }

    // The pool has run out. Record current tick and place it in pending list.
    ASSERT_MSG(result == vk::Result::eErrorOutOfPoolMemory ||
                   result == vk::Result::eErrorFragmentedPool,
               "Unexpected error during descriptor set allocation: {}", vk::to_string(result));
    pending_pools.emplace_back(curr_pool, work_semaphore->CurrentTick());
    // sotc: the known GPU tick is refreshed less often now (PopPendingOperations' early-out), so
    // query it before creating another pool.
    if (!work_semaphore->IsFree(pending_pools.front().second)) {
        work_semaphore->Refresh();
    }
    if (const auto [pool, tick] = pending_pools.front(); work_semaphore->IsFree(tick)) {
        curr_pool = pool;
        pending_pools.pop_front();

        const auto reset_result = device.resetDescriptorPool(curr_pool);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting descriptor pool: {}", vk::to_string(reset_result));
    } else {
        CreateDescriptorPool();
    }

    // Attempt to allocate again with fresh pool.
    alloc_info.descriptorPool = curr_pool;
    result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    ASSERT_MSG(result == vk::Result::eSuccess,
               "Unexpected error during descriptor set allocation {}", vk::to_string(result));

    // We've changed pool so also reset descriptor batch cache.
    descriptor_sets.clear();
    ++pool_serial; // sotc: SOTC_BIND_DESC_REUSE: sets kept by bind-cache entries are no longer reusable
    const auto desc_set = desc_sets.back();
    desc_sets.pop_back();
    descriptor_sets[set_key] = std::move(desc_sets);
    return desc_set;
}

// sotc: SOTC_BIND_DESC_REUSE persistent sets. A pool holds 4096 sets and 4x the rolling pool's descriptors of each
// type; it is never reset, a full one is kept and another made. The sets go back to a per-layout free list only after
// the GPU finished the last tick that bound them, so a set is never written while a command buffer may read it.
void DescriptorHeap::CreatePersistentPool() {
    constexpr u32 kScale = 4;
    if (persistent_sizes.empty()) {
        persistent_sizes.assign(pool_sizes.begin(), pool_sizes.end());
        for (auto& size : persistent_sizes) {
            size.descriptorCount *= kScale;
        }
    }
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = descriptor_heap_count * kScale,
        .poolSizeCount = static_cast<u32>(persistent_sizes.size()),
        .pPoolSizes = persistent_sizes.data(),
    };
    auto [pool_result, pool] = device.createDescriptorPool(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create persistent descriptor pool: {}",
               vk::to_string(pool_result));
    persistent_pools.push_back(pool);
}

void DescriptorHeap::CollectPersistent() {
    while (!persistent_retired.empty() && work_semaphore->IsFree(persistent_retired.front().tick)) {
        const auto& r = persistent_retired.front();
        persistent_free[std::bit_cast<u64>(r.layout)].push_back(r.set);
        ++persistent_free_count;
        ++persistent_freed;
        persistent_retired.pop_front();
    }
}

vk::DescriptorSet DescriptorHeap::AcquirePersistent(vk::DescriptorSetLayout set_layout) {
    CollectPersistent();
    auto& list = persistent_free[std::bit_cast<u64>(set_layout)];
    if (list.empty() && !persistent_retired.empty() &&
        !work_semaphore->IsFree(persistent_retired.front().tick)) {
        // The known GPU tick is refreshed lazily: query it once before allocating a new set.
        work_semaphore->Refresh();
        CollectPersistent();
    }
    ++persistent_alive;
    if (!list.empty()) {
        const auto set = list.back();
        list.pop_back();
        --persistent_free_count;
        ++persistent_recycled;
        return set;
    }
    if (persistent_pools.empty()) {
        CreatePersistentPool();
    }
    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = persistent_pools.back(),
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    vk::DescriptorSet set{};
    auto result = device.allocateDescriptorSets(&alloc_info, &set);
    if (result != vk::Result::eSuccess) {
        ASSERT_MSG(result == vk::Result::eErrorOutOfPoolMemory ||
                       result == vk::Result::eErrorFragmentedPool,
                   "Unexpected error during persistent descriptor set allocation: {}", vk::to_string(result));
        CreatePersistentPool();
        alloc_info.descriptorPool = persistent_pools.back();
        result = device.allocateDescriptorSets(&alloc_info, &set);
        ASSERT_MSG(result == vk::Result::eSuccess,
                   "Unexpected error during persistent descriptor set allocation {}", vk::to_string(result));
    }
    ++persistent_allocated;
    return set;
}

void DescriptorHeap::RetirePersistent(const DescriptorSetReuse& record) {
    if (!record.persistent) {
        return;
    }
    for (u32 i = 0; i < record.count; ++i) {
        if (record.sets[i]) {
            persistent_retired.push_back({record.last_tick, record.layouts[i], record.sets[i]});
            --persistent_alive;
        }
    }
}

PersistentSetStats DescriptorHeap::TakePersistentStats() {
    PersistentSetStats st{};
    st.alive = persistent_alive;
    st.retiring = persistent_retired.size();
    st.free = persistent_free_count;
    st.allocated = std::exchange(persistent_allocated, 0);
    st.recycled = std::exchange(persistent_recycled, 0);
    st.freed = std::exchange(persistent_freed, 0);
    st.pools = persistent_pools.size();
    u64 descriptors = 0;
    for (const auto& size : persistent_sizes) {
        descriptors += size.descriptorCount;
    }
    // Rough: 32 bytes per descriptor slot of each pool (the driver's real cost is not visible).
    st.bytes = st.pools * descriptors * 32;
    return st;
}

void DescriptorHeap::CreateDescriptorPool() {
    // sotc: SOTC_BIND_DESC_REUSE: a pool 8x the size (sets and descriptors), so the sets kept by the bind cache survive
    // more frames between rolls (every roll makes every kept set unusable).
    u32 max_sets = descriptor_heap_count;
    std::span<const vk::DescriptorPoolSize> sizes = pool_sizes;
    if (SotcNonBlocking::BindDescReuse()) {
        constexpr u32 kScale = 8;
        max_sets *= kScale;
        if (scaled_sizes.empty()) {
            scaled_sizes.assign(pool_sizes.begin(), pool_sizes.end());
            for (auto& size : scaled_sizes) {
                size.descriptorCount *= kScale;
            }
        }
        sizes = scaled_sizes;
    }
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = max_sets,
        .poolSizeCount = static_cast<u32>(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    auto [pool_result, pool] = device.createDescriptorPool(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create descriptor pool: {}",
               vk::to_string(pool_result));
    curr_pool = pool;
}

} // namespace Vulkan
