// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic> // sotc: BindSets guard counter
#include <bit>
#include <chrono> // sotc: SOTC_DESC_ON_REC report
#include <memory> // sotc: SOTC_DESC_ON_REC
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h> // sotc: SOTC_DESC_ON_REC verify

#include "common/assert.h"
#include "common/logging/log.h" // sotc: BindSets guard
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_heap.h" // sotc: SOTC_DESCRIPTOR_HEAP
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_checkpoints.h" // sotc: SOTC_GPU_CHECKPOINTS
#include "video_core/sotc_stats.h"
#include "../../../sotc/nonblocking.h" // sotc: SOTC_BIND_DESC_REUSE

namespace Vulkan {

// sotc: SOTC_DEDUP_PUSH=1 skips pushes of unchanged constants (the cache in DynamicState,
// vk_scheduler.h). Off by default: host passes (FSR, DLSS-G, detiler, fault manager) push their
// own constants on the same command buffer and must void the cache (InvalidatePushConstants);
// a missed one would leave a draw with another pass's constants. SOTC_DEDUP_STATE=0 also
// turns it off.
static const bool kDedupState = [] {
    const char* all = std::getenv("SOTC_DEDUP_STATE");
    const char* push = std::getenv("SOTC_DEDUP_PUSH");
    return !(all && all[0] == '0') && push && push[0] == '1';
}();

// sotc: off by default (new, untested in game): SOTC_LAYOUT_CACHE=1 turns it on, and
// SOTC_STAGE_BINDINGS=1 turns it on too (per-stage set layouts are meant to be shared), unless
// SOTC_LAYOUT_CACHE=0 says otherwise.
static bool LayoutCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_LAYOUT_CACHE");
        if (v) {
            return v[0] == '1';
        }
        const char* stage = std::getenv("SOTC_STAGE_BINDINGS");
        return stage && stage[0] == '1';
    }();
    return enabled;
}

LayoutCache::LayoutCache(const Instance& instance_)
    : instance{instance_}, enabled{LayoutCacheEnabled()} {}

LayoutCache::~LayoutCache() {
    if (SotcStats::kCmdStats) {
        std::fprintf(stderr, "SOTC layouts: %llu created, %llu reused%s\n",
                     static_cast<unsigned long long>(num_created),
                     static_cast<unsigned long long>(num_reused),
                     enabled ? "" : " (cache off)");
    }
}

LayoutCache::Key LayoutCache::MakeSetKey(const SetRequest& set) {
    Key key;
    key.reserve(2 + set.bindings.size() * 4);
    key.push_back(static_cast<u64>(static_cast<VkFlags>(set.flags)));
    key.push_back(set.bindings.size());
    for (const auto& binding : set.bindings) {
        key.push_back(binding.binding);
        key.push_back(static_cast<u64>(binding.descriptorType));
        key.push_back(binding.descriptorCount);
        key.push_back(static_cast<u64>(static_cast<VkFlags>(binding.stageFlags)));
    }
    return key;
}

vk::UniqueDescriptorSetLayout LayoutCache::CreateSetLayout(const SetRequest& set) {
    const vk::DescriptorSetLayoutCreateInfo layout_ci = {
        .flags = set.flags,
        .bindingCount = static_cast<u32>(set.bindings.size()),
        .pBindings = set.bindings.data(),
    };
    auto [result, layout] = instance.GetDevice().createDescriptorSetLayoutUnique(layout_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create descriptor set layout: {}",
               vk::to_string(result));
    return std::move(layout);
}

vk::UniquePipelineLayout LayoutCache::CreatePipelineLayout(
    std::span<const vk::DescriptorSetLayout> sets, std::span<const vk::PushConstantRange> push,
    vk::PipelineLayoutCreateFlags flags) {
    const vk::PipelineLayoutCreateInfo layout_info = {
        .flags = flags,
        .setLayoutCount = static_cast<u32>(sets.size()),
        .pSetLayouts = sets.data(),
        .pushConstantRangeCount = static_cast<u32>(push.size()),
        .pPushConstantRanges = push.data(),
    };
    auto [result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(result));
    return std::move(layout);
}

vk::DescriptorSetLayout LayoutCache::FindSetLayout(const SetRequest& set) {
    if (set.null_set) {
        return {}; // sotc: SOTC_NB_GPL, a set the library does not use
    }
    auto [it, is_new] = set_entries.try_emplace(MakeSetKey(set));
    if (is_new) {
        it.value() = CreateSetLayout(set);
    }
    return *it.value();
}

LayoutCache::Layouts LayoutCache::Get(const Request& request, Owned& owned) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(request.sets.size() <= Shader::MaxStageDescriptorSets,
               "Pipeline layout with {} sets", request.sets.size());
    Layouts result;
    // sotc: the bindless slots are a second range after PushData (SOTC_BINDLESS_SAMP only).
    const std::array<vk::PushConstantRange, 2> ranges{request.push_constants, request.slot_constants};
    const std::span<const vk::PushConstantRange> push_ranges{
        ranges.data(), request.slot_constants.size != 0 ? 2u : 1u};
    if (!enabled) {
        owned.sets.clear();
        for (const auto& set : request.sets) {
            if (set.null_set) {
                result.sets.push_back(vk::DescriptorSetLayout{});
                continue;
            }
            owned.sets.push_back(CreateSetLayout(set));
            result.sets.push_back(*owned.sets.back());
        }
        if (request.trailing_set) {
            result.sets.push_back(request.trailing_set);
        }
        owned.pipeline_layout = CreatePipelineLayout(result.sets, push_ranges, request.layout_flags);
        result.pipeline_layout = *owned.pipeline_layout;
        result.created = true;
        ++num_created;
        return result;
    }
    // The pipeline layout is keyed by its set layouts' handles, so equal sets are shared too.
    Key key;
    key.reserve(request.sets.size() + 4);
    for (const auto& set : request.sets) {
        result.sets.push_back(FindSetLayout(set));
        key.push_back(std::bit_cast<u64>(result.sets.back()));
    }
    if (request.trailing_set) {
        result.sets.push_back(request.trailing_set);
        key.push_back(std::bit_cast<u64>(request.trailing_set));
    }
    key.push_back(static_cast<u64>(static_cast<VkFlags>(request.push_constants.stageFlags)));
    key.push_back(request.push_constants.offset);
    key.push_back(request.push_constants.size);
    key.push_back(static_cast<u64>(static_cast<VkFlags>(request.layout_flags))); // sotc: SOTC_NB_GPL
    if (request.slot_constants.size != 0) {
        key.push_back(static_cast<u64>(static_cast<VkFlags>(request.slot_constants.stageFlags)));
        key.push_back(request.slot_constants.offset);
        key.push_back(request.slot_constants.size);
    }
    auto [it, is_new] = pipeline_entries.try_emplace(std::move(key));
    if (is_new) {
        it.value() = CreatePipelineLayout(result.sets, push_ranges, request.layout_flags);
        ++num_created;
    } else {
        ++num_reused;
    }
    result.pipeline_layout = *it.value();
    result.created = is_new;
    return result;
}

namespace DrawPhase { // sotc: SOTC_DRAW_PHASES laps defined in vk_rasterizer.cpp
void SubPipe(unsigned which);
}

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {
    stage_sets = !is_compute && profile.stage_bindings;
    // sotc: SOTC_PACKET: packet_range is not set here. It is decided by the stages of the pipeline when its layout is
    // built (a stage reads the DrawPacket when it has a shader-addressed buffer or is the heap stage).
}

Pipeline::~Pipeline() {
    // sotc: SOTC_GPU_CHECKPOINTS: a pipeline destroyed while a recorded command still uses it shows in the report.
    if (pipeline) {
        SotcCheckpoints::NoteFreed(reinterpret_cast<uint64_t>(static_cast<VkPipeline>(*pipeline)),
                                   SotcCheckpoints::kPipeline);
    }
}

void Pipeline::QueryDescriptorBufferLayout(
    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    if (!instance.IsDescriptorBufferEnabled() &&
        !DescriptorBufferN5Layout(instance, desc_heap, stage_sets, uses_bindless_samplers)) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    desc_layout_size = device.getDescriptorSetLayoutSizeEXT(desc_layout);
    u32 num_slots = 0;
    for (const auto& binding : bindings) {
        num_slots = std::max(num_slots, binding.binding + 1);
    }
    desc_binding_offsets.assign(num_slots, 0);
    for (const auto& binding : bindings) {
        desc_binding_offsets[binding.binding] =
            device.getDescriptorSetLayoutBindingOffsetEXT(desc_layout, binding.binding);
    }
}

namespace {

/// sotc: size in bytes of one descriptor of this type in a descriptor buffer.
size_t DescriptorBufferSize(const vk::PhysicalDeviceDescriptorBufferPropertiesEXT& props,
                            vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eSampler:
        return props.samplerDescriptorSize;
    case vk::DescriptorType::eSampledImage:
        return props.sampledImageDescriptorSize;
    case vk::DescriptorType::eStorageImage:
        return props.storageImageDescriptorSize;
    case vk::DescriptorType::eStorageBuffer:
        return props.storageBufferDescriptorSize;
    default:
        UNREACHABLE_MSG("Descriptor type {} has no descriptor buffer layout",
                        vk::to_string(type));
    }
}

/// sotc: encodes element `element` of a write into dst through vkGetDescriptorEXT. A null buffer
/// becomes a null descriptor (address 0), which nullDescriptor makes valid.
void WriteDescriptorElement(vk::Device device, const vk::WriteDescriptorSet& write, u32 element,
                            void* dst, size_t size) {
    VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = static_cast<VkDescriptorType>(write.descriptorType),
    };
    VkDescriptorAddressInfoEXT address{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
    };
    VkDescriptorImageInfo image{};
    switch (write.descriptorType) {
    case vk::DescriptorType::eStorageBuffer: {
        const vk::DescriptorBufferInfo& buffer = write.pBufferInfo[element];
        if (buffer.buffer != vk::Buffer{}) {
            address.address =
                device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer.buffer}) +
                buffer.offset;
            address.range = buffer.range;
        }
        info.data.pStorageBuffer = &address;
        break;
    }
    case vk::DescriptorType::eSampler:
        image = write.pImageInfo[element];
        info.data.pSampler = &image.sampler;
        break;
    case vk::DescriptorType::eSampledImage:
        image = write.pImageInfo[element];
        info.data.pSampledImage = &image;
        break;
    case vk::DescriptorType::eStorageImage:
        image = write.pImageInfo[element];
        info.data.pStorageImage = &image;
        break;
    default:
        UNREACHABLE_MSG("Descriptor type {} has no descriptor buffer encoding",
                        vk::to_string(write.descriptorType));
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDescriptorEXT(static_cast<VkDevice>(device), &info, size,
                                                     dst);
}

} // Anonymous namespace

void Pipeline::BindDescriptorBuffer(const DescriptorWrites& set_writes,
                                    vk::PipelineBindPoint bind_point) const {
    DescriptorBufferRing& ring = *desc_heap.GetDescriptorBuffer();
    const auto& props = instance.GetDescriptorBufferProperties();
    const vk::Device device = instance.GetDevice();

    // One set per draw, written at its binding offsets inside the ring region.
    const auto alloc = ring.Allocate(desc_layout_size, props.descriptorBufferOffsetAlignment);
    for (const auto& write : set_writes) {
        ASSERT_MSG(write.dstBinding < desc_binding_offsets.size(),
                   "Descriptor binding {} is not in the set layout", write.dstBinding);
        const size_t size = DescriptorBufferSize(props, write.descriptorType);
        u8* base = alloc.mapped + desc_binding_offsets[write.dstBinding] +
                   write.dstArrayElement * size;
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            WriteDescriptorElement(device, write, i, base + i * size, size);
        }
    }
    ring.Flush(alloc.offset, desc_layout_size);

    const auto cmdbuf = scheduler.CommandBuffer();
    // The buffer is bound once per command buffer, before its first offset is set.
    if (ring.NeedsBinding(scheduler.SessionSerial())) {
        const vk::DescriptorBufferBindingInfoEXT binding_info = {
            .address = ring.Address(),
            .usage = vk::BufferUsageFlagBits::eResourceDescriptorBufferEXT |
                     vk::BufferUsageFlagBits::eSamplerDescriptorBufferEXT,
        };
        cmdbuf.bindDescriptorBuffersEXT(1, &binding_info);
    }
    const u32 buffer_index = 0;
    cmdbuf.setDescriptorBufferOffsetsEXT(bind_point, pipeline_layout, 0, 1, &buffer_index,
                                         &alloc.offset);
}

namespace {

/// sotc: the most layout slots the N5 memo keeps. Past it the memo starts over (the ring keeps
/// the bytes of the regions in use until their ticks complete).
constexpr size_t MaxN5Slots = 8192;

/// sotc: the source key of a set: everything its bytes are encoded from. It holds the layout handle,
/// the layout size and binding offsets, and per write its header and the handles of every element's
/// info. Equal keys in one session give equal bytes.
void BuildSourceKey(const std::vector<vk::WriteDescriptorSet>& writes, vk::PipelineLayout layout,
                    vk::DeviceSize size, std::span<const vk::DeviceSize> offsets,
                    std::vector<u64>& key) {
    key.clear();
    key.push_back(std::bit_cast<u64>(layout));
    key.push_back(size);
    key.push_back(offsets.size());
    key.insert(key.end(), offsets.begin(), offsets.end());
    for (const auto& write : writes) {
        key.push_back(u64{write.dstBinding} | (u64{write.dstArrayElement} << 32));
        key.push_back(u64{write.descriptorCount} |
                      (static_cast<u64>(write.descriptorType) << 32));
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
                const auto& info = write.pBufferInfo[i];
                key.push_back(std::bit_cast<u64>(info.buffer));
                key.push_back(info.offset);
                key.push_back(info.range);
            } else {
                const auto& info = write.pImageInfo[i];
                key.push_back(std::bit_cast<u64>(info.sampler));
                key.push_back(std::bit_cast<u64>(info.imageView));
                key.push_back(static_cast<u64>(info.imageLayout));
            }
        }
    }
}

/// sotc: SOTC_N5_DESCBUF=1. Writes the set into the ring at its binding offsets and binds it, or
/// reuses the bytes written earlier in this session for the same layout and sources.
void BindN5Set(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
               const std::vector<vk::WriteDescriptorSet>& set_writes,
               vk::PipelineLayout pipeline_layout, vk::DeviceSize layout_size,
               std::span<const vk::DeviceSize> binding_offsets, vk::PipelineBindPoint bind_point) {
    DescriptorBufferRing& ring = *desc_heap.GetDescriptorBuffer();
    const auto& props = instance.GetDescriptorBufferProperties();
    const vk::Device device = instance.GetDevice();
    const u64 session = scheduler.SessionSerial();
    static thread_local std::vector<u64> key;
    BuildSourceKey(set_writes, pipeline_layout, layout_size, binding_offsets, key);

    // A region is reused only inside its session: its tick is still open, so the ring keeps it.
    auto& slots = ring.Slots();
    if (slots.size() >= MaxN5Slots) {
        slots.clear();
    }
    auto [it, is_new] = slots.try_emplace(std::bit_cast<u64>(pipeline_layout));
    DescriptorBufferRing::SetSlot& slot = it.value();
    vk::DeviceSize offset{};
    if (!is_new && slot.session == session && slot.key == key) {
        DescBufN5::sets_skipped.fetch_add(1, std::memory_order_relaxed);
        offset = slot.offset;
    } else {
        const auto alloc = ring.Allocate(layout_size, props.descriptorBufferOffsetAlignment);
        for (const auto& write : set_writes) {
            ASSERT_MSG(write.dstBinding < binding_offsets.size(),
                       "Descriptor binding {} is not in the set layout", write.dstBinding);
            const size_t size = DescriptorBufferSize(props, write.descriptorType);
            u8* base = alloc.mapped + binding_offsets[write.dstBinding] +
                       write.dstArrayElement * size;
            for (u32 i = 0; i < write.descriptorCount; ++i) {
                WriteDescriptorElement(device, write, i, base + i * size, size);
            }
        }
        ring.Flush(alloc.offset, layout_size);
        slot.key = key;
        slot.session = session;
        slot.offset = alloc.offset;
        offset = alloc.offset;
        DescBufN5::bytes_written.fetch_add(layout_size, std::memory_order_relaxed);
    }
    DescBufN5::Report();

    const auto cmdbuf = scheduler.CommandBuffer();
    // The buffer is bound once per command buffer, before its offset is set.
    if (ring.NeedsBinding(session)) {
        const vk::DescriptorBufferBindingInfoEXT binding_info = {
            .address = ring.Address(),
            .usage = vk::BufferUsageFlagBits::eResourceDescriptorBufferEXT |
                     vk::BufferUsageFlagBits::eSamplerDescriptorBufferEXT,
        };
        cmdbuf.bindDescriptorBuffersEXT(1, &binding_info);
    }
    const u32 buffer_index = 0;
    cmdbuf.setDescriptorBufferOffsetsEXT(bind_point, pipeline_layout, 0, 1, &buffer_index,
                                         &offset);
}

/// sotc: SOTC_N5_DESCBUF=2. Encodes the classic writes of a set, and in one session compares the
/// bytes of every set whose source key was seen before. The classic bind still runs; this only
/// counts. A reference for the bytes cannot be made without a descriptor buffer layout, so the
/// check is that equal sources encode to equal bytes.
void VerifyN5Set(const Instance& instance, DescriptorHeap& desc_heap,
                 const std::vector<vk::WriteDescriptorSet>& set_writes,
                 vk::PipelineLayout pipeline_layout, u64 session) {
    static thread_local std::vector<u64> key;
    static thread_local std::vector<u8> encoded;
    const auto& props = instance.GetDescriptorBufferProperties();
    const vk::Device device = instance.GetDevice();
    BuildSourceKey(set_writes, pipeline_layout, 0, {}, key);
    encoded.clear();
    for (const auto& write : set_writes) {
        const size_t size = DescriptorBufferSize(props, write.descriptorType);
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            const size_t at = encoded.size();
            encoded.resize(at + size);
            WriteDescriptorElement(device, write, i, encoded.data() + at, size);
        }
    }

    auto& slots = desc_heap.GetDescriptorBuffer()->Slots();
    if (slots.size() >= MaxN5Slots) {
        slots.clear();
    }
    auto [it, is_new] = slots.try_emplace(std::bit_cast<u64>(pipeline_layout));
    DescriptorBufferRing::SetSlot& slot = it.value();
    if (!is_new && slot.session == session && slot.key == key) {
        DescBufN5::verify_checks.fetch_add(1, std::memory_order_relaxed);
        if (slot.encoded != encoded) {
            DescBufN5::verify_mismatches.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        slot.key = key;
        slot.session = session;
        slot.encoded = encoded;
    }
    DescBufN5::Report();
}

} // namespace

// sotc: SOTC_DESC_ON_REC (off by default). The vkUpdateDescriptorSets of the classic set paths runs on the
// recording thread (Scheduler::RecordDeferred) instead of the draw thread. The set is still allocated here
// (Commit / AcquirePersistent): the bind-cache record keeps its handle. The writes and their info arrays are deep
// copied into one block the closure owns; the closure is queued before the bindDescriptorSets of the same op, so
// the replay order gives write-before-bind, also for a later bind-cache hit that rebinds the set unwritten.
// Pools roll and persistent sets recycle only after the GPU finished their tick, which is after the recorder ran.
//   0: inline write (as before).
//   1: write on the recorder.
//   2: verify: inline write (its result is the one used), and the recorder closure hashes its copy and compares
//      with the hash of the inline writes; on a match it writes the same contents again, on a mismatch it skips.
//      The draw thread also counts writes into a set whose previous write tick the GPU has not finished
//      ("in-flight"), which the precondition of mode 1 forbids.
// Report: "SOTC DESC ON REC" line every 5 s on stderr.
namespace DescOnRec {

static const int kMode = [] {
    const char* v = std::getenv("SOTC_DESC_ON_REC");
    return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
}();

std::atomic<u64> calls{};         ///< classic set writes seen (mode 1 or 2)
std::atomic<u64> deferred{};      ///< write batches queued on the recorder
std::atomic<u64> fallback{};      ///< batches written inline (no record thread, pNext or an unknown type)
std::atomic<u64> sets_bytes{};    ///< bytes copied for the recorder
std::atomic<u64> verify_checks{}; ///< mode 2: batches compared on the recorder
std::atomic<u64> verify_mismatches{}; ///< mode 2: copies whose hash differs from the inline writes
std::atomic<u64> inflight{};      ///< mode 2: writes into a set whose last write tick is still in flight
std::atomic<u64> total_mismatches{};
std::atomic<u64> total_inflight{};

enum class InfoKind { Image, Buffer, Texel, Unknown };

InfoKind KindOf(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return InfoKind::Image;
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return InfoKind::Buffer;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return InfoKind::Texel;
    default:
        return InfoKind::Unknown;
    }
}

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

/// The contents a write gives its set: header and every element of the info array its type reads.
u64 HashWrites(const vk::WriteDescriptorSet* writes, u32 count) {
    u64 h = count;
    for (u32 w = 0; w < count; ++w) {
        const auto& write = writes[w];
        h = Mix(h, std::bit_cast<u64>(write.dstSet));
        h = Mix(h, u64{write.dstBinding} | (u64{write.dstArrayElement} << 32));
        h = Mix(h, u64{write.descriptorCount} | (static_cast<u64>(write.descriptorType) << 32));
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            switch (KindOf(write.descriptorType)) {
            case InfoKind::Image:
                if (write.pImageInfo) {
                    const auto& info = write.pImageInfo[i];
                    h = Mix(h, std::bit_cast<u64>(info.sampler));
                    h = Mix(h, std::bit_cast<u64>(info.imageView));
                    h = Mix(h, static_cast<u64>(info.imageLayout));
                }
                break;
            case InfoKind::Buffer:
                if (write.pBufferInfo) {
                    const auto& info = write.pBufferInfo[i];
                    h = Mix(h, std::bit_cast<u64>(info.buffer));
                    h = Mix(h, info.offset);
                    h = Mix(h, info.range);
                }
                break;
            case InfoKind::Texel:
                if (write.pTexelBufferView) {
                    h = Mix(h, std::bit_cast<u64>(write.pTexelBufferView[i]));
                }
                break;
            case InfoKind::Unknown:
                break;
            }
        }
    }
    return h;
}

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
    if (due == 0) {
        return; // the first call only starts the window
    }
    const u64 mism = verify_mismatches.exchange(0, std::memory_order_relaxed);
    const u64 infl = inflight.exchange(0, std::memory_order_relaxed);
    const u64 tot_mism = total_mismatches.fetch_add(mism, std::memory_order_relaxed) + mism;
    const u64 tot_infl = total_inflight.fetch_add(infl, std::memory_order_relaxed) + infl;
    std::fprintf(stderr,
                 "SOTC DESC ON REC: mode %d, deferred %llu, inline fallback %llu, copied %llu KiB, "
                 "verify checks %llu, mismatches %llu (total %llu), in-flight writes %llu (total %llu)\n",
                 kMode, static_cast<unsigned long long>(deferred.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long long>(fallback.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long long>(sets_bytes.exchange(0, std::memory_order_relaxed) >> 10),
                 static_cast<unsigned long long>(verify_checks.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long long>(mism), static_cast<unsigned long long>(tot_mism),
                 static_cast<unsigned long long>(infl), static_cast<unsigned long long>(tot_infl));
}

/// Mode 2, draw thread: counts a write into a set whose previous write tick the GPU has not finished.
void CheckInFlight(Scheduler& scheduler, const vk::WriteDescriptorSet* writes, u32 count) {
    static thread_local tsl::robin_map<u64, u64> last_write_tick;
    if (last_write_tick.size() >= (1u << 18)) {
        last_write_tick.clear();
    }
    const u64 tick = scheduler.CurrentTick();
    vk::DescriptorSet last_set{};
    for (u32 w = 0; w < count; ++w) {
        const vk::DescriptorSet set = writes[w].dstSet;
        if (set == last_set) {
            continue;
        }
        last_set = set;
        auto [it, is_new] = last_write_tick.try_emplace(std::bit_cast<u64>(set), tick);
        if (!is_new) {
            if (it->second != tick && !scheduler.IsFree(it->second)) {
                inflight.fetch_add(1, std::memory_order_relaxed);
            }
            it.value() = tick;
        }
    }
}

/// The writes of one batch, deep copied: the write array, then the buffer, image and texel view infos.
struct Batch {
    std::unique_ptr<u8[]> blob;
    u32 count{};
};

/// Copies writes and the info arrays they point to into one block. False when a write cannot be copied
/// (a pNext chain or a descriptor type without a known info array): the caller writes inline then.
bool CopyWrites(const vk::WriteDescriptorSet* writes, u32 count, Batch& out, size_t& bytes_out) {
    size_t num_buffer = 0, num_image = 0, num_texel = 0;
    for (u32 w = 0; w < count; ++w) {
        const auto& write = writes[w];
        if (write.pNext) {
            return false;
        }
        switch (KindOf(write.descriptorType)) {
        case InfoKind::Image:
            num_image += write.pImageInfo ? write.descriptorCount : 0;
            break;
        case InfoKind::Buffer:
            num_buffer += write.pBufferInfo ? write.descriptorCount : 0;
            break;
        case InfoKind::Texel:
            num_texel += write.pTexelBufferView ? write.descriptorCount : 0;
            break;
        case InfoKind::Unknown:
            return false;
        }
    }
    static_assert(alignof(vk::WriteDescriptorSet) <= 8 && alignof(vk::DescriptorBufferInfo) <= 8 &&
                  alignof(vk::DescriptorImageInfo) <= 8 && alignof(vk::BufferView) <= 8);
    static_assert(sizeof(vk::WriteDescriptorSet) % 8 == 0 && sizeof(vk::DescriptorBufferInfo) % 8 == 0 &&
                  sizeof(vk::DescriptorImageInfo) % 8 == 0);
    const size_t writes_bytes = sizeof(vk::WriteDescriptorSet) * count;
    const size_t buffer_bytes = sizeof(vk::DescriptorBufferInfo) * num_buffer;
    const size_t image_bytes = sizeof(vk::DescriptorImageInfo) * num_image;
    const size_t texel_bytes = sizeof(vk::BufferView) * num_texel;
    const size_t bytes = writes_bytes + buffer_bytes + image_bytes + texel_bytes;
    // operator new[] returns memory aligned for any fundamental type, which covers every array below.
    out.blob = std::make_unique_for_overwrite<u8[]>(bytes);
    out.count = count;
    u8* base = out.blob.get();
    auto* dst_writes = reinterpret_cast<vk::WriteDescriptorSet*>(base);
    auto* dst_buffer = reinterpret_cast<vk::DescriptorBufferInfo*>(base + writes_bytes);
    auto* dst_image = reinterpret_cast<vk::DescriptorImageInfo*>(base + writes_bytes + buffer_bytes);
    auto* dst_texel = reinterpret_cast<vk::BufferView*>(base + writes_bytes + buffer_bytes + image_bytes);
    std::memcpy(static_cast<void*>(dst_writes), writes, writes_bytes);
    for (u32 w = 0; w < count; ++w) {
        auto& write = dst_writes[w];
        const u32 n = write.descriptorCount;
        // Only the array the type reads is kept; the others are cleared (Vulkan ignores them).
        const vk::DescriptorImageInfo* image = nullptr;
        const vk::DescriptorBufferInfo* buffer = nullptr;
        const vk::BufferView* texel = nullptr;
        switch (KindOf(write.descriptorType)) {
        case InfoKind::Image:
            if (write.pImageInfo && n) {
                std::memcpy(static_cast<void*>(dst_image), write.pImageInfo, sizeof(*dst_image) * n);
                image = dst_image;
                dst_image += n;
            }
            break;
        case InfoKind::Buffer:
            if (write.pBufferInfo && n) {
                std::memcpy(static_cast<void*>(dst_buffer), write.pBufferInfo, sizeof(*dst_buffer) * n);
                buffer = dst_buffer;
                dst_buffer += n;
            }
            break;
        case InfoKind::Texel:
            if (write.pTexelBufferView && n) {
                std::memcpy(static_cast<void*>(dst_texel), write.pTexelBufferView, sizeof(*dst_texel) * n);
                texel = dst_texel;
                dst_texel += n;
            }
            break;
        case InfoKind::Unknown:
            break;
        }
        write.pImageInfo = image;
        write.pBufferInfo = buffer;
        write.pTexelBufferView = texel;
    }
    bytes_out = bytes;
    return true;
}

/// The classic set write of Pipeline::BindResources / BindStageSets. Mode 0 is the inline call as before.
void WriteSets(const Instance& instance, Scheduler& scheduler, const Pipeline::DescriptorWrites& set_writes) {
    if (kMode == 0) {
        instance.GetDevice().updateDescriptorSets(set_writes, {});
        return;
    }
    Report();
    const u32 count = static_cast<u32>(set_writes.size());
    if (kMode == 2) {
        instance.GetDevice().updateDescriptorSets(set_writes, {});
        CheckInFlight(scheduler, set_writes.data(), count);
    }
    Batch batch;
    size_t bytes = 0;
    if (!scheduler.HasRecordThread() || !CopyWrites(set_writes.data(), count, batch, bytes)) {
        if (kMode == 1) {
            instance.GetDevice().updateDescriptorSets(set_writes, {});
        }
        fallback.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    deferred.fetch_add(1, std::memory_order_relaxed);
    sets_bytes.fetch_add(bytes, std::memory_order_relaxed);
    const u64 expect = kMode == 2 ? HashWrites(set_writes.data(), count) : 0;
    const vk::Device device = instance.GetDevice();
    // Runs before the bindDescriptorSets queued next by the caller (same stream, in order). Without a record
    // thread at this point (a real command buffer), RecordDeferred runs it right away, which is the inline order.
    scheduler.RecordDeferred([device, expect, batch = std::move(batch)](vk::CommandBuffer) {
        const auto* writes = reinterpret_cast<const vk::WriteDescriptorSet*>(batch.blob.get());
        if (kMode == 2) {
            verify_checks.fetch_add(1, std::memory_order_relaxed);
            if (HashWrites(writes, batch.count) != expect) {
                verify_mismatches.fetch_add(1, std::memory_order_relaxed);
                return; // the inline write stands
            }
        }
        device.updateDescriptorSets(batch.count, writes, 0, nullptr);
    });
}

} // namespace DescOnRec

bool Pipeline::PlanHeapMode(std::span<const HeapBinding> bindings, HeapFallback pre) {
    // Off: nothing is planned or counted, the layout is built as before.
    if (!instance.IsDescriptorHeapEnabled()) {
        return false;
    }
    DescriptorHeapBuffers* heaps = desc_heap.GetDescriptorHeapBuffers();
    HeapFallback outcome = pre;
    if (outcome == HeapFallback::Heap && !heaps) {
        outcome = HeapFallback::NoHeaps;
    }
    if (outcome == HeapFallback::Heap) {
        outcome = PlanDescriptorHeap(*heaps, bindings, heap_plan);
    }
    DescriptorHeapNoteLayout(is_compute, outcome);
    heap_mode = outcome == HeapFallback::Heap;
    if (!heap_mode) {
        heap_plan = DescriptorHeapPlan{};
    }
    return heap_mode;
}

void Pipeline::BindHeapResources(const Shader::PushData& push_data,
                                 const DescriptorWrites& set_writes) const {
    DescriptorHeapBuffers* heaps = desc_heap.GetDescriptorHeapBuffers();
    ASSERT_MSG(heaps, "heap pipeline bound without the heap buffers");

    // This draw's region of each ring: its slots are reused only after the GPU finished the ticks that used them.
    // An allocation that waits may flush the recording (a new command buffer), so the command buffer is taken after.
    const u32 resource_base = heaps->AllocResourceSlots(heap_plan.resource_slots, scheduler);
    const u32 sampler_base = heaps->AllocSamplerSlots(heap_plan.sampler_slots, scheduler);
    ASSERT_MSG(resource_base != HeapNoSlot && sampler_base != HeapNoSlot,
               "heap ring cannot hold a pipeline's plan");
    // A refused write is counted by WriteHeapDescriptors; the draw then reads the slots as they were.
    static_cast<void>(
        WriteHeapDescriptors(instance, *heaps, heap_plan, set_writes, resource_base, sampler_base));

    const VkCommandBuffer cmdbuf = static_cast<VkCommandBuffer>(scheduler.CommandBuffer());
    // Heaps are bound on every heap draw: any classic descriptor state (this layer's or another pass's) voids them.
    BindDescriptorHeaps(cmdbuf, *heaps);
    std::array<u8, HeapPushBytes> block{};
    std::memcpy(block.data(), &push_data, sizeof(push_data));
    const std::array<u32, 2> words{resource_base, sampler_base};
    std::memcpy(block.data() + HeapPushResourceWord, words.data(), sizeof(words));
    PushDescriptorHeapData(cmdbuf, block.data(), HeapPushBytes);
    // vkCmdPushDataEXT voids the push constants of the classic layers, so their dedup cache starts over.
    scheduler.GetDynamicState().InvalidatePushConstants();
}

void Pipeline::MarkDynamicFlat(std::span<vk::DescriptorSetLayoutBinding> bindings, std::span<const u32> flat,
                               u32 set, u32 cap) {
    flat_bindings += static_cast<u32>(flat.size());
    if (!SotcNonBlocking::BindDescReuse() || flat.empty()) {
        return;
    }
    if (flat.size() > cap || dyn_flat.size() + flat.size() > MaxDynamicFlat) {
        SotcStats::pb_dyn_fallback_sets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (const u32 index : flat) {
        auto& b = bindings[index];
        b.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
        dyn_flat.push_back({set, b.binding});
    }
    SotcStats::pb_dyn_sets.fetch_add(1, std::memory_order_relaxed);
}

void Pipeline::FinishDynamicFlat() {
    // vkCmdBindDescriptorSets takes the dynamic offsets by set, then by binding number.
    std::sort(dyn_flat.begin(), dyn_flat.end(), [](const DynamicBinding& a, const DynamicBinding& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
}

u32 Pipeline::PrepareDynamicFlat(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                                 std::array<u32, MaxDynamicFlat>& offsets, DynamicPatch* patch) const {
    const u32 num = static_cast<u32>(dyn_flat.size());
    const bool by_set = write_sets.size() == set_writes.size();
    for (u32 k = 0; k < num; ++k) {
        offsets[k] = 0;
        for (u32 j = 0; j < static_cast<u32>(set_writes.size()); ++j) {
            auto& write = set_writes[j];
            if (write.dstBinding != dyn_flat[k].binding || (by_set ? write_sets[j] : 0u) != dyn_flat[k].set ||
                write.descriptorType != vk::DescriptorType::eStorageBuffer || !write.pBufferInfo) {
                continue;
            }
            offsets[k] = static_cast<u32>(write.pBufferInfo->offset);
            if (patch) {
                const u32 n = patch->count++;
                patch->infos[n] = *write.pBufferInfo;
                patch->infos[n].offset = 0;
                patch->saved[n] = write.pBufferInfo;
                patch->write[n] = j;
                write.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
                write.pBufferInfo = &patch->infos[n];
            }
            break;
        }
    }
    return num;
}

void Pipeline::RestoreDynamicWrites(DescriptorWrites& set_writes, const DynamicPatch& patch) {
    for (u32 n = 0; n < patch.count; ++n) {
        auto& write = set_writes[patch.write[n]];
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.pBufferInfo = patch.saved[n];
    }
}

void Pipeline::BindResources(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                             const Shader::PushData& push_data,
                             const Shader::BindlessSamplerSlots& bindless_slots,
                             const SotcArch::DrawPacket* packet,
                             const SotcArch::HeapBlock* heap_block,
                             DescriptorSetReuse* reuse) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    // sotc: SOTC_BIND_DESC_REUSE: until a set path below stores its sets, the caller's record holds none. A reuse
    // request is honoured only by the two classic set paths; any other path binds as before and clears it. The
    // request's sets are read from a copy taken first (the record is cleared before any path looks at it).
    const bool want_reuse = reuse && reuse->reuse;
    DescriptorSetReuse reuse_in{};
    if (want_reuse) {
        reuse_in = *reuse;
    }
    if (reuse) {
        reuse->count = 0;
        reuse->reuse = false;
        reuse->persistent = false;
    }

    // sotc: SOTC_DESCRIPTOR_HEAP: a heap pipeline has no layout: its descriptors and push block go to the heaps.
    if (heap_mode) {
        BindHeapResources(push_data, set_writes);
        return;
    }

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;

    // sotc: skip the push when this command buffer already holds these bytes for this layout and
    // stages. The cache is voided with the generation of DynamicState: BeginSession (a new
    // command buffer) and every Invalidate() call (other pipelines bound on it).
    auto& dynamic = scheduler.GetDynamicState();
    auto& cache = dynamic.push_constant_cache;
    static_assert(sizeof(Shader::PushData) <= DynamicState::PushConstantCache::MaxBytes);
    static_assert(PacketPushBytes <= DynamicState::PushConstantCache::MaxBytes);
    static_assert(Shader::HeapPushBytes <= DynamicState::PushConstantCache::MaxBytes);
    // sotc: SOTC_PACKET: a packet pipeline pushes PushData and the DrawPacket as one block. Other
    // pipelines push PushData alone, byte for byte as before.
    // sotc: SOTC_HEAP: a pipeline whose profile has the heap pushes the heap block too (HeapPushBytes, 192 bytes).
    // Left uninitialized on purpose: a packet block is written in full before any read.
    std::array<u8, Shader::HeapPushBytes> packet_block;
    const void* block = &push_data;
    u32 size = static_cast<u32>(sizeof(push_data));
    if (packet_range) {
        ASSERT_MSG(packet, "packet pipeline bound without a DrawPacket");
        std::memcpy(packet_block.data(), &push_data, sizeof(push_data));
        std::memcpy(packet_block.data() + PacketPushOffset, packet, sizeof(*packet));
        block = packet_block.data();
        size = PacketPushBytes;
        if (profile.heap_images != 0) {
            const SotcArch::HeapBlock empty{};
            std::memcpy(packet_block.data() + Shader::HeapBlockOffset,
                        heap_block ? heap_block : &empty, sizeof(SotcArch::HeapBlock));
            size = Shader::HeapPushBytes;
        }
    }
    const bool same = kDedupState && cache.generation == dynamic.generation &&
                      cache.layout == pipeline_layout && cache.stages == stage_flags &&
                      cache.size == size && std::memcmp(cache.bytes.data(), block, size) == 0;
    if (!same) {
        cmdbuf.pushConstants(pipeline_layout, stage_flags, 0u, size, block);
        if (kDedupState) {
            cache.generation = dynamic.generation;
            cache.layout = pipeline_layout;
            cache.stages = stage_flags;
            cache.size = size;
            std::memcpy(cache.bytes.data(), block, size);
        }
    }
    // sotc: SOTC_BINDLESS_SAMP: the sampler slots, after PushData (the second range of the layout).
    if (uses_slot_words) {
        cmdbuf.pushConstants(pipeline_layout, stage_flags, sizeof(push_data), sizeof(bindless_slots),
                             &bindless_slots);
    }
    static const bool phases = std::getenv("SOTC_DRAW_PHASES") != nullptr;
    if (phases) {
        DrawPhase::SubPipe(0);
    }

    // sotc: SOTC_BINDLESS_SAMP: the global sampler set is bound beside set 0, before any early out. Its index is the
    // one the SPIR-V names (BindlessSetIndex): set 1, or the set after the stage sets with SOTC_STAGE_BINDINGS (a
    // fixed set 1 there was the vertex stage's set: an invalid bind, and with dynamic flat buffers in that set NVIDIA
    // read the null pDynamicOffsets).
    if (uses_bindless_samplers) {
        const auto bindless_set = desc_heap.BindlessSamplerSet();
        BindSets(cmdbuf, bind_point, BindlessSetIndex(), 1, &bindless_set, 0, nullptr);
    }

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (stage_sets) {
        BindStageSets(set_writes, write_sets, bind_point, want_reuse ? &reuse_in : nullptr, reuse);
        return;
    }

    // sotc: SOTC_N5_DESCBUF=1: the set is written into the descriptor buffer ring (or reused from
    // this session) and bound at its offset. The layout is a descriptor buffer layout by construction.
    if (DescriptorBufferN5Layout(instance, desc_heap, stage_sets, uses_bindless_samplers)) {
        BindN5Set(instance, scheduler, desc_heap, set_writes, pipeline_layout, desc_layout_size,
                  desc_binding_offsets, bind_point);
        return;
    }
    // sotc: SOTC_N5_DESCBUF=2: the classic bind below runs, its sources are checked here first.
    if (DescriptorBufferN5Mode() == 2 && DescriptorBufferN5Active(instance, desc_heap)) {
        VerifyN5Set(instance, desc_heap, set_writes, pipeline_layout, scheduler.SessionSerial());
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, pipeline_layout, 0, set_writes);
        if (phases) {
            DrawPhase::SubPipe(1);
        }
        return;
    }

    // sotc: SOTC_DESC_BUFFER=1, the set is written into the descriptor buffer ring instead.
    if (instance.IsDescriptorBufferEnabled()) {
        BindDescriptorBuffer(set_writes, bind_point);
        return;
    }

    // sotc: SOTC_BIND_DESC_REUSE: the flat buffer bindings of a dynamic layout take their offsets at bind time.
    std::array<u32, MaxDynamicFlat> dyn_offsets{};
    // sotc: SOTC_BIND_DESC_REUSE: the set of the last write of this bind-cache entry, bound again unwritten.
    if (want_reuse && reuse_in.count == 1 && reuse_in.sets[0]) {
        const u32 num_dyn = PrepareDynamicFlat(set_writes, write_sets, dyn_offsets, nullptr);
        BindSets(cmdbuf, bind_point, 0, 1, &reuse_in.sets[0], num_dyn, dyn_offsets.data());
        *reuse = reuse_in;
        reuse->reuse = true;
        reuse->last_tick = scheduler.CurrentTick();
        return;
    }
    // sotc: SOTC_BIND_DESC_REUSE: a set a bind-cache entry keeps comes from the persistent pools when they are on.
    const bool persist = reuse && reuse->keep && desc_heap.PersistentSets();
    const auto desc_set = persist ? desc_heap.AcquirePersistent(desc_layout) : desc_heap.Commit(desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    DynamicPatch patch{};
    const u32 num_dyn = dyn_flat.empty() ? 0u : PrepareDynamicFlat(set_writes, write_sets, dyn_offsets, &patch);
    DescOnRec::WriteSets(instance, scheduler, set_writes); // sotc: SOTC_DESC_ON_REC
    RestoreDynamicWrites(set_writes, patch);
    BindSets(cmdbuf, bind_point, 0, 1, &desc_set, num_dyn, dyn_offsets.data());
    if (reuse) { // sotc: SOTC_BIND_DESC_REUSE: kept for the entry
        reuse->sets[0] = desc_set;
        reuse->layouts[0] = desc_layout;
        reuse->count = 1;
        reuse->pool_serial = desc_heap.PoolSerial();
        reuse->persistent = persist;
        reuse->last_tick = scheduler.CurrentTick();
    }
}

void Pipeline::BindStageSets(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                             vk::PipelineBindPoint bind_point, const DescriptorSetReuse* reuse_in,
                             DescriptorSetReuse* reuse_out) const {
    ASSERT_MSG(write_sets.size() == set_writes.size(), "Descriptor write without a set");
    const auto cmdbuf = scheduler.CommandBuffer();
    // sotc: only the stage sets: the layout's padding sets and its bindless set (BindlessSetIndex) are not committed
    // here; BindResources binds the global bindless set at its index.
    const u32 num_sets = num_stage_sets;
    ASSERT_MSG(num_sets != 0 && num_sets <= DescriptorSetReuse::MaxSets && num_sets <= set_layouts.size(),
               "Stage set count {} for a layout of {} sets", num_sets, set_layouts.size());
    static_assert(DescriptorSetReuse::MaxSets == Shader::MaxStageDescriptorSets);
    // sotc: SOTC_BIND_DESC_REUSE: the flat buffer bindings of a dynamic layout take their offsets at bind time.
    std::array<u32, MaxDynamicFlat> dyn_offsets{};
    // sotc: SOTC_BIND_DESC_REUSE: reuse_in is given only with a reuse request (BindResources). The entry's sets, one
    // per layout set, bound again without a write; a record of another shape is written below instead.
    if (reuse_in && reuse_out && reuse_in->count == num_sets) {
        const u32 num_dyn = PrepareDynamicFlat(set_writes, write_sets, dyn_offsets, nullptr);
        BindSets(cmdbuf, bind_point, 0, num_sets, reuse_in->sets.data(), num_dyn, dyn_offsets.data());
        *reuse_out = *reuse_in;
        reuse_out->reuse = true;
        reuse_out->last_tick = scheduler.CurrentTick();
        return;
    }
    // sotc: SOTC_BIND_DESC_REUSE: sets a bind-cache entry keeps come from the persistent pools when they are on.
    const bool persist = reuse_out && reuse_out->keep && desc_heap.PersistentSets();
    std::array<vk::DescriptorSet, Shader::MaxStageDescriptorSets> desc_sets{};
    for (u32 set = 0; set < num_sets; ++set) {
        desc_sets[set] = persist ? desc_heap.AcquirePersistent(set_layouts[set]) : desc_heap.Commit(set_layouts[set]);
    }
    if (reuse_out) { // sotc: SOTC_BIND_DESC_REUSE: these fresh sets are the entry's record
        reuse_out->sets = desc_sets;
        for (u32 set = 0; set < num_sets; ++set) {
            reuse_out->layouts[set] = set_layouts[set];
        }
        reuse_out->count = num_sets;
        reuse_out->pool_serial = desc_heap.PoolSerial();
        reuse_out->persistent = persist;
        reuse_out->last_tick = scheduler.CurrentTick();
    }
    for (size_t i = 0; i < set_writes.size(); ++i) {
        ASSERT_MSG(write_sets[i] < num_sets, "Descriptor set {} is not in the layout",
                   write_sets[i]);
        set_writes[i].dstSet = desc_sets[write_sets[i]];
    }
    DynamicPatch patch{};
    const u32 num_dyn = dyn_flat.empty() ? 0u : PrepareDynamicFlat(set_writes, write_sets, dyn_offsets, &patch);
    DescOnRec::WriteSets(instance, scheduler, set_writes); // sotc: SOTC_DESC_ON_REC
    RestoreDynamicWrites(set_writes, patch);
    BindSets(cmdbuf, bind_point, 0, num_sets, desc_sets.data(), num_dyn, dyn_offsets.data());
}

void Pipeline::BindSets(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point, u32 first, u32 count,
                        const vk::DescriptorSet* sets, u32 num_dyn, const u32* offsets) const {
    // sotc: the dynamic descriptors of the bound sets, one offset each (dyn_flat is sorted by set, then binding).
    u32 expected = 0;
    for (const auto& dyn : dyn_flat) {
        expected += dyn.set >= first && dyn.set - first < count ? 1u : 0u;
    }
    if (expected == num_dyn && (num_dyn == 0 || offsets)) [[likely]] {
        cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, first, count, sets, num_dyn, offsets);
        return;
    }
    static std::atomic<u64> mismatches{0};
    const u64 n = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0) {
        LOG_ERROR(Render_Vulkan,
                  "descriptor bind with wrong dynamic offsets (bind #{}): sets {}..{} of a {}-set layout need {}, "
                  "got {} (offsets {}), stage sets {} ({}), bindless {} at set {}, pipeline {}; zero offsets used",
                  n, first, first + count - 1, set_layouts.size(), expected, num_dyn, offsets ? "given" : "null",
                  stage_sets, num_stage_sets, uses_bindless_samplers, BindlessSetIndex(), GetDebugString());
    }
    // A valid bind (offset 0 stays inside each buffer) instead of the driver reading past or through null offsets.
    ASSERT_MSG(expected <= MaxDynamicFlat, "{} dynamic offsets in one bind", expected);
    const std::array<u32, MaxDynamicFlat> zeros{};
    cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, first, count, sets, expected, zeros.data());
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
