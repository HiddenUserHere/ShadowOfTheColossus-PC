// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: VK_EXT_descriptor_buffer ring, see vk_descriptor_buffer.h.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_queues.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

struct DescriptorBufferRing::Memory {
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    VkBuffer buffer{};
};

DescriptorBufferRing::DescriptorBufferRing(const Instance& instance, Scheduler& scheduler_)
    : scheduler{scheduler_}, memory{std::make_unique<Memory>()} {
    // Both descriptor usages: a set layout mixes sampler and resource descriptors.
    const vk::BufferCreateInfo buffer_ci = {
        .size = DescriptorBufferBytes,
        .usage = vk::BufferUsageFlagBits::eResourceDescriptorBufferEXT |
                 vk::BufferUsageFlagBits::eSamplerDescriptorBufferEXT |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const VkBufferCreateInfo buffer_ci_raw = static_cast<VkBufferCreateInfo>(buffer_ci);
    // Same placement as the stream buffers: device local when the heap is host visible too.
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    memory->allocator = instance.GetAllocator();
    VmaAllocationInfo alloc_info{};
    const VkResult result =
        vmaCreateBuffer(memory->allocator, &buffer_ci_raw, &alloc_ci, &memory->buffer,
                        &memory->allocation, &alloc_info);
    ASSERT_MSG(result == VK_SUCCESS, "Failed to create descriptor buffer: {}",
               vk::to_string(vk::Result{result}));
    mapped = static_cast<u8*>(alloc_info.pMappedData);
    ASSERT_MSG(mapped != nullptr, "Descriptor buffer memory is not host visible");

    VkMemoryPropertyFlags flags{};
    vmaGetAllocationMemoryProperties(memory->allocator, memory->allocation, &flags);
    coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::Device device = instance.GetDevice();
    const vk::Buffer buffer{memory->buffer};
    SetObjectName(device, buffer, "Descriptor Buffer Ring");
    address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer});
}

DescriptorBufferRing::~DescriptorBufferRing() {
    // The GPU may still read the ring: wait for every tick that was given a region.
    for (const Mark& mark : marks) {
        if (!scheduler.IsFree(mark.tick)) {
            scheduler.Wait(mark.tick);
        }
    }
    if (memory->buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(memory->allocator, memory->buffer, memory->allocation);
    }
}

DescriptorBufferRing::Allocation DescriptorBufferRing::Allocate(vk::DeviceSize size,
                                                                vk::DeviceSize alignment) {
    ASSERT_MSG(size <= DescriptorBufferBytes,
               "Descriptor set of {} bytes does not fit the descriptor buffer ring", size);
    const u64 align = std::max<u64>(alignment, 1);
    const u64 tick = scheduler.CurrentTick();

    // Release the regions of the ticks the GPU has already completed.
    while (!marks.empty() && scheduler.IsFree(marks.front().tick)) {
        tail = marks.front().end;
        marks.pop_front();
    }

    u64 start{};
    for (;;) {
        start = (head + align - 1) / align * align;
        // Keep a region contiguous in the buffer: start over at the buffer beginning instead.
        if (start % DescriptorBufferBytes + size > DescriptorBufferBytes) {
            start = (start / DescriptorBufferBytes + 1) * DescriptorBufferBytes;
        }
        if (marks.empty()) {
            // Nothing is in use, so the region can start anywhere.
            tail = start;
            break;
        }
        if (start + size - tail <= DescriptorBufferBytes) {
            break;
        }
        // The ring is full: wait for the oldest tick still using it.
        const Mark oldest = marks.front();
        ASSERT_MSG(oldest.tick != tick,
                   "Descriptor buffer ring exhausted within one submission ({} MB)",
                   DescriptorBufferBytes >> 20);
        if (!scheduler.IsFree(oldest.tick)) {
            scheduler.Wait(oldest.tick);
        }
        tail = oldest.end;
        marks.pop_front();
    }

    head = start + size;
    if (!marks.empty() && marks.back().tick == tick) {
        marks.back().end = head;
    } else {
        marks.push_back(Mark{tick, head});
    }
    const u64 offset = start % DescriptorBufferBytes;
    return Allocation{mapped + offset, offset};
}

void DescriptorBufferRing::Flush(vk::DeviceSize offset, vk::DeviceSize size) const {
    if (!coherent) {
        vmaFlushAllocation(memory->allocator, memory->allocation, offset, size);
    }
}

int DescriptorBufferN5Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_N5_DESCBUF");
        const int wanted = (v && v[0] == '1') ? 1 : (v && v[0] == '2') ? 2 : 0;
        const char* legacy = std::getenv("SOTC_DESC_BUFFER");
        if (wanted != 0 && legacy && legacy[0] == '1') {
            LOG_WARNING(Render_Vulkan, "SOTC_N5_DESCBUF ignored: SOTC_DESC_BUFFER=1 is set");
            return 0;
        }
        return wanted;
    }();
    return mode;
}

bool DescriptorBufferN5Active(const Instance& instance, const DescriptorHeap& heap) {
    if (DescriptorBufferN5Mode() == 0 || !instance.IsDescriptorBufferN5Enabled()) {
        return false;
    }
    if (heap.GetDescriptorBuffer() == nullptr) {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_WARNING(Render_Vulkan, "SOTC_N5_DESCBUF={} not complete: the resource pool made no "
                                       "descriptor buffer ring, classic descriptors",
                        DescriptorBufferN5Mode());
        }
        return false;
    }
    return true;
}

bool DescriptorBufferN5Layout(const Instance& instance, const DescriptorHeap& heap, bool stage_sets,
                              bool bindless) {
    if (DescriptorBufferN5Mode() != 1 || stage_sets || bindless) {
        return false;
    }
    // The async compute redirect records binds on its own timeline value, which the ring's tick
    // marks do not cover: the two are not combined (mode 2 keeps no region, so it is not affected).
    if (SotcQueues::AsyncMoveOn()) {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_WARNING(Render_Vulkan, "SOTC_N5_DESCBUF=1 not combined with SOTC_N5_ASYNC_COMPUTE=1, "
                                       "classic descriptors");
        }
        return false;
    }
    return DescriptorBufferN5Active(instance, heap);
}

namespace DescBufN5 {

void Report() {
    // Checked every 256 calls: the clock read stays off the draw path.
    static std::atomic<u64> last_report_ns{};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed);
    if ((n & 255) != 0) {
        return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    constexpr u64 period_ns = 5'000'000'000;
    u64 due = last_report_ns.load(std::memory_order_relaxed);
    if (due != 0 && static_cast<u64>(now) < due) {
        return;
    }
    if (!last_report_ns.compare_exchange_strong(due, static_cast<u64>(now) + period_ns,
                                                std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC N5 DESCBUF: mode %d, calls %llu, sets skipped %llu, bytes written %llu, "
                 "verify checks %llu, verify mismatches %llu\n",
                 DescriptorBufferN5Mode(), static_cast<unsigned long long>(calls.load()),
                 static_cast<unsigned long long>(sets_skipped.load()),
                 static_cast<unsigned long long>(bytes_written.load()),
                 static_cast<unsigned long long>(verify_checks.load()),
                 static_cast<unsigned long long>(verify_mismatches.load()));
}

} // namespace DescBufN5

} // namespace Vulkan
