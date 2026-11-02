// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: VK_EXT_descriptor_heap, SOTC_DESCRIPTOR_HEAP=1. Phase 1: the switch, the runtime detection with its refusal
// reasons, the two heap buffers and one probe of the write functions. Phase 2 (this file): per-op rings in the heaps,
// the image and sampler create infos the heap writes need, the mapping plan of a heap pipeline, and the bind path.
// A pipeline that cannot be mapped keeps the classic path and is counted by its reason (HeapFallback).
// See docs/re-stutters/native/DESCRIPTOR-HEAP.md.
//
// Off (unset or 0) changes nothing: no extension, no log, no buffer, no registry entry.

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// sotc: SOTC_DESCRIPTOR_HEAP, read once. 0: off (unset, "0", or any other value, the last one with a
/// warning). 1: on when the device supports it (see DescriptorHeapLimitRefusal). "auto" is not implemented.
[[nodiscard]] u32 DescriptorHeapRequest();

/// sotc: slot counts of the per-op rings (one slot holds one descriptor of either kind, slot_size bytes).
inline constexpr u32 HeapRingSlots = 65536;
inline constexpr u32 HeapSamplerRingSlots = 16384;
/// sotc: the ring's write tracking unit: a chunk remembers the tick of its last write.
inline constexpr u32 HeapChunkSlots = 64;
/// sotc: no slot (a ring that cannot hold the request).
inline constexpr u32 HeapNoSlot = 0xFFFFFFFFu;
/// sotc: push data words after Shader::PushData (56 bytes): the resource ring base, then the sampler ring base.
inline constexpr u32 HeapPushResourceWord = 56;
inline constexpr u32 HeapPushSamplerWord = 60;
inline constexpr u32 HeapPushBytes = 64;

/// sotc: why a pipeline is (or is not) built in heap mode. Counted once per pipeline layout build.
enum class HeapFallback : u32 {
    Heap,          ///< built with the heap flag
    StageSets,     ///< SOTC_STAGE_BINDINGS: one set per stage
    DescriptorBuffer, ///< descriptor buffer layout (SOTC_DESC_BUFFER or SOTC_N5_DESCBUF)
    Bindless,      ///< a stage reads the bindless samplers or images (not mirrored into the heaps yet)
    SlotWords,     ///< bindless slot words in the push constants
    PacketRange,   ///< the DrawPacket (heap stage or shader-addressed buffer)
    NoHeaps,       ///< the heap buffers were not created or the probe failed
    RingSize,      ///< the pipeline needs more ring slots than a ring holds
    Count,
};

/// sotc: sizes of the two heaps, in bytes. Layout of the resource heap: [bindless arrays (unused in heap mode)]
/// [per-op ring] [reserved range]. The sampler heap is the same with its own ring.
struct DescriptorHeapSizes {
    vk::DeviceSize resource_usable{};   ///< bindless image arrays: 12 keys x 4096 slots x one slot size
    vk::DeviceSize resource_reserved{}; ///< offset of the reserved range (after the ring)
    vk::DeviceSize resource_total{};    ///< buffer size: usable + ring + reserved, aligned to resourceHeapAlignment
    vk::DeviceSize sampler_usable{};    ///< bindless sampler array: 4096 slots x sampler slot size
    vk::DeviceSize sampler_reserved{};  ///< offset of the reserved range (after the ring)
    vk::DeviceSize sampler_total{};     ///< buffer size: usable + ring + reserved, aligned to samplerHeapAlignment
    vk::DeviceSize slot_size{};         ///< one resource descriptor slot: max(image, buffer descriptor size)
    vk::DeviceSize sampler_slot_size{}; ///< one sampler slot: samplerDescriptorSize aligned to its alignment
    u32 sampler_ring_slots{};           ///< sampler ring slots: HeapSamplerRingSlots, less when the heap limit is lower
};

/// sotc: computes the sizes of the heaps from the device limits. Returns "" when they fit, else the reason
/// the heap mode is refused (alignment not a power of two, a heap larger than its maximum, zero sizes).
[[nodiscard]] std::string DescriptorHeapLimitRefusal(
    const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props, DescriptorHeapSizes& sizes);

/// sotc: the resource and sampler heaps: host-visible buffers with VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT and
/// a device address aligned to the heap alignment. Created by the resource pool when the instance enabled the mode.
class DescriptorHeapBuffers {
public:
    /// Computes the sizes from `props` (DescriptorHeapLimitRefusal); a refusal there is the reason.
    DescriptorHeapBuffers(const Instance& instance,
                          const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props);
    ~DescriptorHeapBuffers();

    DescriptorHeapBuffers(const DescriptorHeapBuffers&) = delete;
    DescriptorHeapBuffers& operator=(const DescriptorHeapBuffers&) = delete;

    [[nodiscard]] bool Ok() const noexcept {
        return ok;
    }
    [[nodiscard]] const std::string& Reason() const noexcept {
        return reason;
    }
    [[nodiscard]] const DescriptorHeapSizes& Sizes() const noexcept {
        return sizes;
    }
    [[nodiscard]] const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& Props() const noexcept {
        return props;
    }
    [[nodiscard]] vk::DeviceAddress ResourceAddress() const noexcept {
        return resource_address;
    }
    [[nodiscard]] vk::DeviceAddress SamplerAddress() const noexcept {
        return sampler_address;
    }
    /// Host pointers of the heaps (whole buffers). Writes go here; Flush makes them visible when not coherent.
    [[nodiscard]] u8* ResourceMapped() const noexcept {
        return resource_mapped;
    }
    [[nodiscard]] u8* SamplerMapped() const noexcept {
        return sampler_mapped;
    }

    /// Reserves `count` consecutive resource slots of the ring and returns the absolute slot index of the first one
    /// (the heap's slot numbering: byte offset / slot_size). A chunk is reused only after the GPU finished its last
    /// write (Scheduler::IsFree), else the scheduler is waited on. HeapNoSlot when the ring cannot hold `count`.
    [[nodiscard]] u32 AllocResourceSlots(u32 count, Scheduler& scheduler);
    /// Same for the sampler ring (absolute index = byte offset / sampler_slot_size).
    [[nodiscard]] u32 AllocSamplerSlots(u32 count, Scheduler& scheduler);

    /// Flushes a host-written range of the resource or sampler heap (no-op when the memory is coherent).
    void Flush(bool sampler, u64 offset, u64 size) const;

private:
    struct Memory;
    struct SlotRing {
        u32 begin{};  ///< first slot of the ring (absolute)
        u32 end{};    ///< one past the last slot
        u32 cursor{}; ///< next free slot (absolute)
        std::vector<u64> chunk_tick; ///< per chunk: tick of its last write
    };
    u32 AllocRing(SlotRing& ring, u32 count, Scheduler& scheduler);
    bool Probe(const Instance& instance, const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& props);

    bool ok{};
    std::string reason;
    DescriptorHeapSizes sizes{};
    vk::PhysicalDeviceDescriptorHeapPropertiesEXT props{};
    vk::DeviceAddress resource_address{};
    vk::DeviceAddress sampler_address{};
    u8* resource_mapped{};
    u8* sampler_mapped{};
    SlotRing resource_ring{};
    SlotRing sampler_ring{};
    std::unique_ptr<Memory> memory;
};

/// sotc: true when a DescriptorHeapBuffers was created and passed its probe in this session.
[[nodiscard]] bool DescriptorHeapActive() noexcept;

/// sotc: one layout build of a pipeline while the mode is on, with its outcome. Counters reset on DescriptorHeapReport.
void DescriptorHeapNoteLayout(bool is_compute, HeapFallback outcome) noexcept;

/// sotc: one binding of a pipeline's set 0, in layout order. `binding` is the SPIR-V Binding number (the layout's),
/// `count` the descriptor count (array size), `sampler` true for a VK_DESCRIPTOR_TYPE_SAMPLER binding.
struct HeapBinding {
    u32 binding{};
    u32 count{};
    bool sampler{};
    vk::ShaderStageFlagBits stage{};
};

/// sotc: the mapping plan of a heap pipeline. Per stage: the VkDescriptorSetAndBindingMappingEXT list the stage's
/// VkPipelineShaderStageCreateInfo chains. local_of_binding: per binding number, the first ring slot of the binding
/// inside one op (relative to the op's base); HeapNoSlot for numbers that are not bindings of the pipeline.
struct DescriptorHeapPlan {
    struct StageMappings {
        vk::ShaderStageFlagBits stage{};
        std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
    };
    std::vector<StageMappings> stages;
    std::vector<u32> local_of_binding;
    u32 resource_slots{};
    u32 sampler_slots{};

    [[nodiscard]] const std::vector<VkDescriptorSetAndBindingMappingEXT>& MappingsOf(
        vk::ShaderStageFlagBits stage) const;
};

/// sotc: builds the plan of `bindings`. Returns Heap when every binding maps (plan filled), else RingSize.
[[nodiscard]] HeapFallback PlanDescriptorHeap(const DescriptorHeapBuffers& heaps,
                                              std::span<const HeapBinding> bindings,
                                              DescriptorHeapPlan& plan);

/// sotc: encodes the writes of one draw into the rings at the given bases (resource and sampler) and returns false when
/// one descriptor cannot be encoded (counted; the draw must not use the heap then). Images need their view's create
/// info, samplers their create info (the registries below).
[[nodiscard]] bool WriteHeapDescriptors(const Instance& instance, DescriptorHeapBuffers& heaps,
                                        const DescriptorHeapPlan& plan,
                                        std::span<const vk::WriteDescriptorSet> writes, u32 resource_base,
                                        u32 sampler_base);

/// sotc: binds both heaps on a command buffer (vkCmdBindResourceHeapEXT, vkCmdBindSamplerHeapEXT, no inheritance).
void BindDescriptorHeaps(VkCommandBuffer cmdbuf, const DescriptorHeapBuffers& heaps);

/// sotc: vkCmdPushDataEXT of `size` bytes at offset 0 (the push block of a heap pipeline).
void PushDescriptorHeapData(VkCommandBuffer cmdbuf, const void* data, u32 size);

/// sotc: the create info of an image view and of a sampler, kept while the mode is on (the heap writes need them).
/// Views overwrite their entry when their handle is created again; samplers are forgotten when destroyed.
void DescriptorHeapRegisterView(VkImageView view, const VkImageViewCreateInfo& info);
void DescriptorHeapRegisterSampler(VkSampler sampler, const VkSamplerCreateInfo& info);
void DescriptorHeapForgetSampler(VkSampler sampler);

/// sotc: one line for the 5 s report, counters reset on read: "SOTC DESCRIPTOR_HEAP ...". Returns the length
/// written, 0 when the mode is not active.
u32 DescriptorHeapReport(char* out, u32 size);

} // namespace Vulkan
