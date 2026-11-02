// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: VK_EXT_descriptor_heap, SOTC_DESCRIPTOR_HEAP (phases 1 and 2). See vk_descriptor_heap.h.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/renderer_vulkan/vk_descriptor_heap.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/sampler.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

// sotc: the heaps hold the bindless arrays (phase 1, unused by heap pipelines) before the rings.
constexpr u64 HeapImageSlots = Shader::NUM_BINDLESS_IMAGE_KEYS * VideoCore::BindlessImageCapacity;
constexpr u64 HeapSamplerSlots = VideoCore::BindlessSamplers::Capacity;

static_assert(sizeof(Shader::PushData) == HeapPushResourceWord,
              "the ring base words follow Shader::PushData");
static_assert(HeapPushSamplerWord == HeapPushResourceWord + 4);
static_assert(HeapPushBytes == HeapPushSamplerWord + 4);

constexpr bool IsPow2(u64 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr u64 AlignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::atomic<bool> g_active{false};
std::atomic<u64> g_pipelines{};
std::atomic<u64> g_outcomes[static_cast<u32>(HeapFallback::Count)]{};
std::atomic<u64> g_draws{};
std::atomic<u64> g_descriptors{};
std::atomic<u64> g_refused{};
std::atomic<u64> g_ring_waits{};
std::atomic<u64> g_heap_binds{};
std::atomic<u64> g_pushes{};

/// sotc: the create info of an image view as the heap writes need it. `info.pNext` points at `usage` when set.
struct RegisteredView {
    VkImageViewCreateInfo info{};
    VkImageViewUsageCreateInfo usage{};
    bool has_usage{};
};
/// sotc: the create info of a sampler, with its custom border color when it has one.
struct RegisteredSampler {
    VkSamplerCreateInfo info{};
    VkSamplerCustomBorderColorCreateInfoEXT custom{};
    bool has_custom{};
};

std::mutex g_registry_mutex;
std::unordered_map<u64, RegisteredView> g_views;
std::unordered_map<u64, RegisteredSampler> g_samplers;

const RegisteredView* FindView(VkImageView view) {
    std::scoped_lock lock{g_registry_mutex};
    const auto it = g_views.find(reinterpret_cast<u64>(view));
    return it == g_views.end() ? nullptr : &it->second;
}

const RegisteredSampler* FindSampler(VkSampler sampler) {
    std::scoped_lock lock{g_registry_mutex};
    const auto it = g_samplers.find(reinterpret_cast<u64>(sampler));
    return it == g_samplers.end() ? nullptr : &it->second;
}

/// Creates a host-visible buffer for a descriptor heap, aligned to `alignment` (the heap alignment of the spec).
bool CreateHeapBuffer(VmaAllocator allocator, vk::DeviceSize size, vk::DeviceSize alignment,
                      VkBuffer& buffer, VmaAllocation& allocation, u8*& mapped) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    // Same placement as the descriptor buffer ring: host visible, device local when the heap allows it.
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VmaAllocationInfo info{};
    const VkResult result = vmaCreateBufferWithAlignment(allocator, &buffer_ci, &alloc_ci, alignment,
                                                         &buffer, &allocation, &info);
    if (result != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        allocation = nullptr;
        mapped = nullptr;
        return false;
    }
    mapped = static_cast<u8*>(info.pMappedData);
    return mapped != nullptr;
}

} // Anonymous namespace

u32 DescriptorHeapRequest() {
    static const u32 request = [] {
        const char* env = std::getenv("SOTC_DESCRIPTOR_HEAP");
        if (!env || env[0] == '\0' || (env[0] == '0' && env[1] == '\0')) {
            return 0u;
        }
        if (env[0] == '1' && env[1] == '\0') {
            return 1u;
        }
        LOG_WARNING(Render_Vulkan, "SOTC_DESCRIPTOR_HEAP={} not understood (use 1 or 0): off", env);
        return 0u;
    }();
    return request;
}

std::string DescriptorHeapLimitRefusal(const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props,
                                       DescriptorHeapSizes& sizes) {
    if (!IsPow2(props.resourceHeapAlignment) || !IsPow2(props.samplerHeapAlignment)) {
        return fmt::format("heap alignment is not a power of two (resource {}, sampler {})",
                           static_cast<u64>(props.resourceHeapAlignment),
                           static_cast<u64>(props.samplerHeapAlignment));
    }
    if (props.imageDescriptorSize == 0 || props.bufferDescriptorSize == 0 ||
        props.samplerDescriptorSize == 0) {
        return "a descriptor size is zero";
    }
    const u64 resource_align = props.resourceHeapAlignment;
    const u64 sampler_align = props.samplerHeapAlignment;

    // A slot is one descriptor of either kind, padded to the alignment of the kinds it holds.
    const u64 slot_align = std::max<u64>({props.imageDescriptorAlignment,
                                          props.bufferDescriptorAlignment, 1});
    if (!IsPow2(slot_align)) {
        return "a resource descriptor alignment is not a power of two";
    }
    const u64 slot_size = AlignUp(std::max<u64>(props.imageDescriptorSize, props.bufferDescriptorSize),
                                  slot_align);
    const u64 sampler_slot_align = std::max<u64>(props.samplerDescriptorAlignment, 1);
    if (!IsPow2(sampler_slot_align)) {
        return "the sampler descriptor alignment is not a power of two";
    }
    const u64 sampler_slot_size = AlignUp(props.samplerDescriptorSize, sampler_slot_align);
    // The ring base words are slot indices: each ring must start on a slot boundary of its heap.
    if (resource_align % slot_size != 0 || sampler_align % sampler_slot_size != 0) {
        return "a descriptor slot does not divide the heap alignment";
    }

    // Layout: [bindless arrays][per-op ring][reserved range], each part aligned to the heap alignment.
    const u64 resource_usable = AlignUp(HeapImageSlots * slot_size, resource_align);
    const u64 resource_reserved =
        AlignUp(resource_usable + u64{HeapRingSlots} * slot_size, resource_align);
    const u64 resource_total =
        AlignUp(resource_reserved + props.minResourceHeapReservedRange, resource_align);
    // sotc: the bindless sampler array is not mirrored into the heap yet (no heap pipeline reads it), and
    // maxSamplerHeapSize can be as low as 128 KiB (NVIDIA: about 4000 samplers), so the sampler heap holds only
    // the per-op ring, sized to what the limit leaves (whole chunks, at most HeapSamplerRingSlots).
    const u64 sampler_usable = 0;
    const u64 sampler_room = props.maxSamplerHeapSize > props.minSamplerHeapReservedRange + sampler_align
                                 ? props.maxSamplerHeapSize - props.minSamplerHeapReservedRange - sampler_align
                                 : 0;
    const u32 sampler_ring_slots = static_cast<u32>(
        std::min<u64>(HeapSamplerRingSlots, sampler_room / sampler_slot_size / HeapChunkSlots * HeapChunkSlots));
    if (sampler_ring_slots < 4 * HeapChunkSlots) {
        return fmt::format("the sampler heap limit ({} KiB) leaves {} ring slots",
                           static_cast<u64>(props.maxSamplerHeapSize) >> 10, sampler_ring_slots);
    }
    const u64 sampler_reserved =
        AlignUp(sampler_usable + u64{sampler_ring_slots} * sampler_slot_size, sampler_align);
    const u64 sampler_total =
        AlignUp(sampler_reserved + props.minSamplerHeapReservedRange, sampler_align);

    if (resource_total > props.maxResourceHeapSize) {
        return fmt::format("resource heap needs {} KiB, the limit is {} KiB", resource_total >> 10,
                           static_cast<u64>(props.maxResourceHeapSize) >> 10);
    }
    if (sampler_total > props.maxSamplerHeapSize) {
        return fmt::format("sampler heap needs {} KiB, the limit is {} KiB", sampler_total >> 10,
                           static_cast<u64>(props.maxSamplerHeapSize) >> 10);
    }

    sizes = DescriptorHeapSizes{
        .resource_usable = resource_usable,
        .resource_reserved = resource_reserved,
        .resource_total = resource_total,
        .sampler_usable = sampler_usable,
        .sampler_reserved = sampler_reserved,
        .sampler_total = sampler_total,
        .slot_size = slot_size,
        .sampler_slot_size = sampler_slot_size,
        .sampler_ring_slots = sampler_ring_slots,
    };
    return {};
}

struct DescriptorHeapBuffers::Memory {
    VmaAllocator allocator{};
    VmaAllocation resource_allocation{};
    VkBuffer resource_buffer{};
    VmaAllocation sampler_allocation{};
    VkBuffer sampler_buffer{};
};

DescriptorHeapBuffers::DescriptorHeapBuffers(const Instance& instance,
                                             const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props_)
    : props{props_}, memory{std::make_unique<Memory>()} {
    reason = DescriptorHeapLimitRefusal(props, sizes);
    if (!reason.empty()) {
        LOG_WARNING(Render_Vulkan, "SOTC DESCRIPTOR_HEAP: off: {}", reason);
        return;
    }
    memory->allocator = instance.GetAllocator();
    u8* resource_ptr = nullptr;
    u8* sampler_ptr = nullptr;
    const bool resource_ok =
        CreateHeapBuffer(memory->allocator, sizes.resource_total, props.resourceHeapAlignment,
                         memory->resource_buffer, memory->resource_allocation, resource_ptr);
    const bool sampler_ok =
        CreateHeapBuffer(memory->allocator, sizes.sampler_total, props.samplerHeapAlignment,
                         memory->sampler_buffer, memory->sampler_allocation, sampler_ptr);
    if (!resource_ok || !sampler_ok) {
        // The buffers are destroyed by the destructor, which runs as the object is returned.
        reason = "heap buffers are not host visible or could not be created";
        return;
    }
    resource_mapped = resource_ptr;
    sampler_mapped = sampler_ptr;

    const vk::Device device = instance.GetDevice();
    const vk::Buffer resource{memory->resource_buffer};
    const vk::Buffer sampler{memory->sampler_buffer};
    SetObjectName(device, resource, "Descriptor Heap Resource");
    SetObjectName(device, sampler, "Descriptor Heap Sampler");
    resource_address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = resource});
    sampler_address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = sampler});
    // The bind address must be a multiple of the heap alignment (VUID-VkBindHeapInfoEXT-heapRange-11235).
    if (resource_address % props.resourceHeapAlignment != 0 ||
        sampler_address % props.samplerHeapAlignment != 0) {
        reason = fmt::format("heap device addresses are not aligned (resource {:#x}, sampler {:#x})",
                             static_cast<u64>(resource_address), static_cast<u64>(sampler_address));
        return;
    }

    // The rings: slot indices count from the start of each heap buffer.
    resource_ring.begin = static_cast<u32>(sizes.resource_usable / sizes.slot_size);
    resource_ring.end = resource_ring.begin + HeapRingSlots;
    resource_ring.cursor = resource_ring.begin;
    resource_ring.chunk_tick.assign(HeapRingSlots / HeapChunkSlots, 0);
    sampler_ring.begin = static_cast<u32>(sizes.sampler_usable / sizes.sampler_slot_size);
    sampler_ring.end = sampler_ring.begin + sizes.sampler_ring_slots;
    sampler_ring.cursor = sampler_ring.begin;
    sampler_ring.chunk_tick.assign(sizes.sampler_ring_slots / HeapChunkSlots, 0);

    ok = Probe(instance, props);
    if (ok) {
        g_active.store(true, std::memory_order_release);
        LOG_INFO(Render_Vulkan,
                 "SOTC DESCRIPTOR_HEAP: on (resource heap {} KiB, sampler heap {} KiB, rings {} and {} "
                 "slots, descriptor sizes image {} buffer {} sampler {} B, slot {} B)",
                 sizes.resource_total >> 10, sizes.sampler_total >> 10, HeapRingSlots,
                 sizes.sampler_ring_slots, static_cast<u64>(props.imageDescriptorSize),
                 static_cast<u64>(props.bufferDescriptorSize),
                 static_cast<u64>(props.samplerDescriptorSize), static_cast<u64>(sizes.slot_size));
    } else {
        LOG_WARNING(Render_Vulkan, "SOTC DESCRIPTOR_HEAP: off: {}", reason);
    }
}

DescriptorHeapBuffers::~DescriptorHeapBuffers() {
    if (memory && memory->allocator) {
        if (memory->resource_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(memory->allocator, memory->resource_buffer, memory->resource_allocation);
        }
        if (memory->sampler_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(memory->allocator, memory->sampler_buffer, memory->sampler_allocation);
        }
    }
    g_active.store(false, std::memory_order_release);
}

bool DescriptorHeapBuffers::Probe(const Instance& instance,
                                  const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props_) {
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteResourceDescriptorsEXT ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteSamplerDescriptorsEXT ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindResourceHeapEXT ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindSamplerHeapEXT ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDataEXT) {
        reason = "a descriptor heap function was not loaded";
        return false;
    }
    const auto resource_size = static_cast<size_t>(props_.bufferDescriptorSize);
    const auto sampler_size = static_cast<size_t>(props_.samplerDescriptorSize);
    // The probe writes into host scratch, not into the heap: no heap slot is touched.
    std::vector<u8> scratch(std::max(resource_size, sampler_size), 0);
    const VkDevice device = static_cast<VkDevice>(instance.GetDevice());

    // A storage buffer descriptor for the heap's own address range (the bytes are never read).
    VkDeviceAddressRangeEXT range{
        .address = static_cast<VkDeviceAddress>(resource_address),
        .size = 16,
    };
    VkResourceDescriptorInfoEXT resource_info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };
    resource_info.data.pAddressRange = &range;
    VkHostAddressRangeEXT resource_out{
        .address = scratch.data(),
        .size = resource_size,
    };
    VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteResourceDescriptorsEXT(device, 1,
                                                                                  &resource_info,
                                                                                  &resource_out);
    if (result != VK_SUCCESS) {
        reason = fmt::format("vkWriteResourceDescriptorsEXT refused the probe ({})",
                             static_cast<int>(result));
        return false;
    }

    // A default sampler, encoded the same way.
    VkSamplerCreateInfo sampler_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    VkHostAddressRangeEXT sampler_out{
        .address = scratch.data(),
        .size = sampler_size,
    };
    result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteSamplerDescriptorsEXT(device, 1, &sampler_ci,
                                                                         &sampler_out);
    if (result != VK_SUCCESS) {
        reason = fmt::format("vkWriteSamplerDescriptorsEXT refused the probe ({})",
                             static_cast<int>(result));
        return false;
    }
    return true;
}

void DescriptorHeapBuffers::Flush(bool sampler, u64 offset, u64 size) const {
    if (!memory || !memory->allocator || size == 0) {
        return;
    }
    vmaFlushAllocation(memory->allocator,
                       sampler ? memory->sampler_allocation : memory->resource_allocation, offset,
                       size);
}

u32 DescriptorHeapBuffers::AllocRing(SlotRing& ring, u32 count, Scheduler& scheduler) {
    if (count == 0) {
        return ring.begin;
    }
    if (count > ring.end - ring.begin) {
        return HeapNoSlot;
    }
    // The ring is never split: a request that does not fit before the end starts at the beginning again.
    bool wrapped = false;
    if (ring.cursor + count > ring.end) {
        ring.cursor = ring.begin;
        wrapped = true;
    }
    const u32 base = ring.cursor;
    const u32 first = (base - ring.begin) / HeapChunkSlots;
    const u32 last = (base + count - 1 - ring.begin) / HeapChunkSlots;
    const u64 current = scheduler.CurrentTick();
    for (u32 chunk = first; chunk <= last; ++chunk) {
        // The last write to this chunk must be finished by the GPU before the host writes it again. Waiting on
        // the current tick flushes the recording first (Scheduler::Wait).
        // sotc: a chunk last written in the current tick, reached by moving forward, holds only slots behind the
        // cursor: this request writes other slots, so nothing the recording reads is overwritten and no wait is
        // needed. Waiting there submitted the recording at almost every draw (ring_waits ~ draws, and a submit
        // with a render pass open). Only a wrap back onto the current tick's chunks must wait.
        const u64 tick = ring.chunk_tick[chunk];
        if ((tick != current || wrapped) && !scheduler.IsFree(tick)) {
            scheduler.Wait(tick);
            g_ring_waits.fetch_add(1, std::memory_order_relaxed);
        }
        ring.chunk_tick[chunk] = scheduler.CurrentTick();
    }
    ring.cursor = base + count;
    return base;
}

u32 DescriptorHeapBuffers::AllocResourceSlots(u32 count, Scheduler& scheduler) {
    return AllocRing(resource_ring, count, scheduler);
}

u32 DescriptorHeapBuffers::AllocSamplerSlots(u32 count, Scheduler& scheduler) {
    return AllocRing(sampler_ring, count, scheduler);
}

bool DescriptorHeapActive() noexcept {
    return g_active.load(std::memory_order_acquire);
}

void DescriptorHeapNoteLayout(bool is_compute, HeapFallback outcome) noexcept {
    (void)is_compute;
    g_pipelines.fetch_add(1, std::memory_order_relaxed);
    g_outcomes[static_cast<u32>(outcome)].fetch_add(1, std::memory_order_relaxed);
}

const std::vector<VkDescriptorSetAndBindingMappingEXT>& DescriptorHeapPlan::MappingsOf(
    vk::ShaderStageFlagBits stage) const {
    static const std::vector<VkDescriptorSetAndBindingMappingEXT> none;
    for (const auto& entry : stages) {
        if (entry.stage == stage) {
            return entry.mappings;
        }
    }
    return none;
}

HeapFallback PlanDescriptorHeap(const DescriptorHeapBuffers& heaps, std::span<const HeapBinding> bindings,
                                DescriptorHeapPlan& plan) {
    const u32 resource_stride = static_cast<u32>(heaps.Sizes().slot_size);
    const u32 sampler_stride = static_cast<u32>(heaps.Sizes().sampler_slot_size);
    plan = DescriptorHeapPlan{};

    u32 max_binding = 0;
    for (const auto& binding : bindings) {
        max_binding = std::max(max_binding, binding.binding + 1);
    }
    plan.local_of_binding.assign(max_binding, HeapNoSlot);

    u32 resource_next = 0;
    u32 sampler_next = 0;
    for (const auto& binding : bindings) {
        const u32 local = binding.sampler ? sampler_next : resource_next;
        const u32 stride = binding.sampler ? sampler_stride : resource_stride;
        plan.local_of_binding[binding.binding] = local;
        (binding.sampler ? sampler_next : resource_next) += binding.count;

        // One mapping per binding number. The base of the op's region comes from push data (a slot index), so
        // the address is (base + local + element) * stride. All resource kinds are covered by the mask: binding
        // numbers are unique in the set, and overspecifying a mask is allowed by the spec.
        VkDescriptorSetAndBindingMappingEXT mapping{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .descriptorSet = 0,
            .firstBinding = binding.binding,
            .bindingCount = 1,
            .resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT,
            .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT,
        };
        auto& push = mapping.sourceData.pushIndex;
        push.heapOffset = local * stride;
        push.pushOffset = binding.sampler ? HeapPushSamplerWord : HeapPushResourceWord;
        push.heapIndexStride = stride;
        push.heapArrayStride = stride;
        push.pEmbeddedSampler = nullptr;
        push.useCombinedImageSamplerIndex = VK_FALSE;
        push.samplerHeapOffset = 0;
        push.samplerPushOffset = HeapPushSamplerWord;
        push.samplerHeapIndexStride = 0;
        push.samplerHeapArrayStride = 0;

        auto it = std::find_if(plan.stages.begin(), plan.stages.end(),
                               [&](const auto& entry) { return entry.stage == binding.stage; });
        if (it == plan.stages.end()) {
            plan.stages.push_back({binding.stage, {}});
            it = plan.stages.end() - 1;
        }
        it->mappings.push_back(mapping);
    }
    plan.resource_slots = resource_next;
    plan.sampler_slots = sampler_next;
    if (resource_next > HeapRingSlots || sampler_next > HeapSamplerRingSlots) {
        return HeapFallback::RingSize;
    }
    return HeapFallback::Heap;
}

bool WriteHeapDescriptors(const Instance& instance, DescriptorHeapBuffers& heaps,
                          const DescriptorHeapPlan& plan, std::span<const vk::WriteDescriptorSet> writes,
                          u32 resource_base, u32 sampler_base) {
    // Sized once per draw: the pointers handed to the write functions point into these vectors.
    static thread_local std::vector<VkResourceDescriptorInfoEXT> res_info;
    static thread_local std::vector<VkImageDescriptorInfoEXT> img_info;
    static thread_local std::vector<VkDeviceAddressRangeEXT> ranges;
    static thread_local std::vector<VkHostAddressRangeEXT> res_out;
    static thread_local std::vector<VkSamplerCreateInfo> smp_info;
    static thread_local std::vector<VkHostAddressRangeEXT> smp_out;

    u32 res_count = 0;
    u32 smp_count = 0;
    for (const auto& write : writes) {
        if (write.descriptorType == vk::DescriptorType::eSampler) {
            smp_count += write.descriptorCount;
        } else {
            res_count += write.descriptorCount;
        }
    }
    res_info.resize(res_count);
    img_info.resize(res_count);
    ranges.resize(res_count);
    res_out.resize(res_count);
    smp_info.resize(smp_count);
    smp_out.resize(smp_count);

    const vk::Device device = instance.GetDevice();
    const VkDevice vk_device = static_cast<VkDevice>(device);
    const auto& sizes = heaps.Sizes();
    u8* const res_mapped = heaps.ResourceMapped();
    u8* const smp_mapped = heaps.SamplerMapped();
    const auto& props = heaps.Props();
    const u64 res_desc_size = std::max(props.imageDescriptorSize, props.bufferDescriptorSize);

    u32 r = 0;
    u32 s = 0;
    for (const auto& write : writes) {
        if (write.descriptorCount == 0) {
            continue;
        }
        const bool is_sampler = write.descriptorType == vk::DescriptorType::eSampler;
        if (write.dstBinding >= plan.local_of_binding.size() ||
            plan.local_of_binding[write.dstBinding] == HeapNoSlot) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const u32 first = plan.local_of_binding[write.dstBinding] + write.dstArrayElement;
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            if (is_sampler) {
                const RegisteredSampler* reg =
                    FindSampler(static_cast<VkSampler>(write.pImageInfo[i].sampler));
                if (!reg) {
                    g_refused.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                smp_info[s] = reg->info;
                smp_info[s].pNext = reg->has_custom ? &reg->custom : nullptr;
                const u64 slot = u64{sampler_base} + first + i;
                smp_out[s] = VkHostAddressRangeEXT{
                    .address = smp_mapped + slot * sizes.sampler_slot_size,
                    .size = static_cast<size_t>(props.samplerDescriptorSize),
                };
                ++s;
                continue;
            }

            VkResourceDescriptorInfoEXT& info = res_info[r];
            info = VkResourceDescriptorInfoEXT{
                .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                .type = static_cast<VkDescriptorType>(write.descriptorType),
            };
            if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
                const vk::DescriptorBufferInfo& buffer = write.pBufferInfo[i];
                if (buffer.buffer != vk::Buffer{}) {
                    ranges[r] = VkDeviceAddressRangeEXT{
                        .address = static_cast<VkDeviceAddress>(
                            device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer.buffer}) +
                            buffer.offset),
                        .size = buffer.range,
                    };
                    info.data.pAddressRange = &ranges[r];
                } else {
                    info.data.pAddressRange = nullptr; // null descriptor (nullDescriptor)
                }
            } else {
                const vk::DescriptorImageInfo& image = write.pImageInfo[i];
                if (image.imageView != vk::ImageView{}) {
                    const RegisteredView* reg = FindView(static_cast<VkImageView>(image.imageView));
                    if (!reg) {
                        g_refused.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    VkImageDescriptorInfoEXT& desc = img_info[r];
                    desc = VkImageDescriptorInfoEXT{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
                        .pView = &reg->info,
                        .layout = static_cast<VkImageLayout>(image.imageLayout),
                    };
                    info.data.pImage = &desc;
                } else {
                    info.data.pImage = nullptr; // null descriptor (nullDescriptor)
                }
            }
            const u64 slot = u64{resource_base} + first + i;
            res_out[r] = VkHostAddressRangeEXT{
                .address = res_mapped + slot * sizes.slot_size,
                .size = static_cast<size_t>(res_desc_size),
            };
            ++r;
        }
    }
    ASSERT_MSG(r == res_count && s == smp_count, "heap write count mismatch");

    // The pointers in res_info reference img_info and ranges, which are complete now.
    if (r != 0) {
        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteResourceDescriptorsEXT(
            vk_device, r, res_info.data(), res_out.data());
        if (result != VK_SUCCESS) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        heaps.Flush(false, u64{resource_base} * sizes.slot_size, u64{r} * sizes.slot_size);
    }
    if (s != 0) {
        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWriteSamplerDescriptorsEXT(
            vk_device, s, smp_info.data(), smp_out.data());
        if (result != VK_SUCCESS) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        heaps.Flush(true, u64{sampler_base} * sizes.sampler_slot_size,
                    u64{s} * sizes.sampler_slot_size);
    }
    g_descriptors.fetch_add(r + s, std::memory_order_relaxed);
    g_draws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BindDescriptorHeaps(VkCommandBuffer cmdbuf, const DescriptorHeapBuffers& heaps) {
    const auto& sizes = heaps.Sizes();
    const VkBindHeapInfoEXT resource{
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = VkDeviceAddressRangeEXT{
            .address = static_cast<VkDeviceAddress>(heaps.ResourceAddress()),
            .size = sizes.resource_total,
        },
        .reservedRangeOffset = sizes.resource_reserved,
        .reservedRangeSize = sizes.resource_total - sizes.resource_reserved,
    };
    const VkBindHeapInfoEXT sampler{
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = VkDeviceAddressRangeEXT{
            .address = static_cast<VkDeviceAddress>(heaps.SamplerAddress()),
            .size = sizes.sampler_total,
        },
        .reservedRangeOffset = sizes.sampler_reserved,
        .reservedRangeSize = sizes.sampler_total - sizes.sampler_reserved,
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindResourceHeapEXT(cmdbuf, &resource);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindSamplerHeapEXT(cmdbuf, &sampler);
    g_heap_binds.fetch_add(1, std::memory_order_relaxed);
}

void PushDescriptorHeapData(VkCommandBuffer cmdbuf, const void* data, u32 size) {
    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .offset = 0,
        .data = VkHostAddressRangeConstEXT{
            .address = data,
            .size = size,
        },
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDataEXT(cmdbuf, &info);
    g_pushes.fetch_add(1, std::memory_order_relaxed);
}

void DescriptorHeapRegisterView(VkImageView view, const VkImageViewCreateInfo& info) {
    if (!view) {
        return;
    }
    std::scoped_lock lock{g_registry_mutex};
    RegisteredView& entry = g_views[reinterpret_cast<u64>(view)];
    entry.info = info;
    entry.has_usage = false;
    // Copy the usage struct the create info chains (ImageView passes its own on the stack).
    if (info.pNext && static_cast<const VkBaseInStructure*>(info.pNext)->sType ==
                          VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) {
        entry.usage = *static_cast<const VkImageViewUsageCreateInfo*>(info.pNext);
        entry.usage.pNext = nullptr;
        entry.has_usage = true;
    }
    entry.info.pNext = entry.has_usage ? &entry.usage : nullptr;
}

void DescriptorHeapRegisterSampler(VkSampler sampler, const VkSamplerCreateInfo& info) {
    if (!sampler) {
        return;
    }
    std::scoped_lock lock{g_registry_mutex};
    RegisteredSampler& entry = g_samplers[reinterpret_cast<u64>(sampler)];
    entry.info = info;
    entry.has_custom = false;
    if (info.pNext && static_cast<const VkBaseInStructure*>(info.pNext)->sType ==
                          VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT) {
        entry.custom = *static_cast<const VkSamplerCustomBorderColorCreateInfoEXT*>(info.pNext);
        entry.custom.pNext = nullptr;
        entry.has_custom = true;
    }
    entry.info.pNext = entry.has_custom ? &entry.custom : nullptr;
}

void DescriptorHeapForgetSampler(VkSampler sampler) {
    if (!sampler || !DescriptorHeapActive()) {
        return;
    }
    std::scoped_lock lock{g_registry_mutex};
    g_samplers.erase(reinterpret_cast<u64>(sampler));
}

u32 DescriptorHeapReport(char* out, u32 size) {
    if (!DescriptorHeapActive()) {
        return 0;
    }
    const auto take = [](std::atomic<u64>& counter) {
        return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed));
    };
    const u64 pipelines = g_pipelines.exchange(0, std::memory_order_relaxed);
    const auto outcome = [&](HeapFallback f) {
        return take(g_outcomes[static_cast<u32>(f)]);
    };
    const int n = std::snprintf(
        out, size,
        "SOTC DESCRIPTOR_HEAP pipelines %llu heap %llu classic (stage_sets %llu desc_buf %llu bindless %llu "
        "slot_words %llu packet %llu no_heap %llu ring %llu) draws %llu descs %llu refused %llu "
        "ring_waits %llu heap_binds %llu pushes %llu",
        static_cast<unsigned long long>(pipelines), outcome(HeapFallback::Heap),
        outcome(HeapFallback::StageSets), outcome(HeapFallback::DescriptorBuffer),
        outcome(HeapFallback::Bindless), outcome(HeapFallback::SlotWords),
        outcome(HeapFallback::PacketRange), outcome(HeapFallback::NoHeaps),
        outcome(HeapFallback::RingSize), take(g_draws), take(g_descriptors), take(g_refused),
        take(g_ring_waits), take(g_heap_binds), take(g_pushes));
    return n < 0 ? 0 : static_cast<u32>(n);
}

} // namespace Vulkan
