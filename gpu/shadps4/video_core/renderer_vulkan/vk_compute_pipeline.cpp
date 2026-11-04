// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <boost/container/small_vector.hpp>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h" // sotc: SOTC_HEAP (UsesHeapImages)
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/sotc_pipe_feedback.h" // sotc: SOTC_PIPE_FEEDBACK / COMPILE_CHECK
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, LayoutCache& layout_cache,
                                 const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                                 ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module_,
                                 SerializationSupport& sdata, bool preloading /*=false*/,
                                 bool deferred /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_}, cache{pipeline_cache}, module{module_}, from_cache{preloading} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (u32 image_index = 0; image_index < info->images.size(); ++image_index) {
        const auto& image = info->images[image_index];
        // sotc: SOTC_N5_BINDLESS: a sampled image of the heap has no binding (AddBindings).
        if (Shader::BindlessImageSlotOf(*info, image_index) != Shader::NO_BINDLESS_IMAGE_SLOT) {
            continue;
        }
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    // sotc: SOTC_BINDLESS_SAMP: the samplers come from the global heap (set 1) instead.
    const bool bindless_images = Shader::UsesBindlessImages(*info);
    uses_bindless_samplers = Shader::UsesBindlessSamplers(*info) || bindless_images;
    // sotc: SOTC_HEAP: a heap stage reads its indices from the index table (the packet's): the heap set, no slot words,
    // and the packet range. Otherwise the bindless stage reads slot words, as before.
    const bool heap_stage = Shader::Backend::SPIRV::UsesHeapImages(*info, profile);
    // sotc: SOTC_PACKET: the packet range only when the stage reads the packet (heap stage or shader-addressed buffer).
    packet_range = heap_stage || Shader::Backend::SPIRV::UsesShaderAddr(profile, *info);
    uses_slot_words = uses_bindless_samplers && !heap_stage;
    for (const auto& sampler : info->samplers) {
        if (uses_bindless_samplers) {
            break;
        }
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    // sotc: SOTC_PACKET: a packet pipeline's range holds the DrawPacket after PushData (no slot words).
    ASSERT_MSG(!(packet_range && uses_slot_words),
               "packet pipeline with bindless slot words (the heap replaces them)");
    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        // sotc: SOTC_HEAP: a heap profile's packet range also holds the heap block (HeapPushBytes).
        .size = packet_range ? (profile.heap_images != 0 ? Shader::HeapPushBytes : PacketPushBytes)
                             : static_cast<u32>(sizeof(Shader::PushData)),
    };

    // sotc: descriptor buffers replace push descriptors (the two layout flags are exclusive).
    // sotc: SOTC_N5_DESCBUF=1 adds its pipelines (DescriptorBufferN5Layout) to the same layout kind.
    const bool descriptor_buffer =
        instance.IsDescriptorBufferEnabled() ||
        DescriptorBufferN5Layout(instance, desc_heap, false, uses_bindless_samplers);
    // sotc: SOTC_DESCRIPTOR_HEAP (vk_descriptor_heap.h): all bindings map to the heaps: no layout, the heap flag.
    {
        HeapFallback pre = HeapFallback::Heap;
        if (descriptor_buffer) {
            pre = HeapFallback::DescriptorBuffer;
        } else if (uses_bindless_samplers) {
            pre = HeapFallback::Bindless;
        } else if (uses_slot_words) {
            pre = HeapFallback::SlotWords;
        } else if (packet_range) {
            pre = HeapFallback::PacketRange;
        }
        boost::container::small_vector<HeapBinding, 32> heap_bindings;
        for (const auto& b : bindings) {
            heap_bindings.push_back(HeapBinding{
                .binding = b.binding,
                .count = b.descriptorCount,
                .sampler = b.descriptorType == vk::DescriptorType::eSampler,
                .stage = vk::ShaderStageFlagBits::eCompute,
            });
        }
        PlanHeapMode(heap_bindings, pre);
    }
    uses_push_descriptors = !heap_mode && !descriptor_buffer && binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                       : descriptor_buffer
                           ? vk::DescriptorSetLayoutCreateFlagBits::eDescriptorBufferEXT
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    // Compute has one set, set 0, whatever SOTC_STAGE_BINDINGS says.
    const LayoutCache::SetRequest set_request{
        .bindings = bindings,
        .flags = flags,
    };
    const LayoutCache::Request request{
        .sets = std::span{&set_request, 1},
        .push_constants = push_constants,
        .trailing_set = uses_bindless_samplers ? desc_heap.BindlessSamplerLayout()
                                               : vk::DescriptorSetLayout{},
        .slot_constants = uses_slot_words
                              ? vk::PushConstantRange{
                                    .stageFlags = vk::ShaderStageFlagBits::eCompute,
                                    .offset = sizeof(Shader::PushData),
                                    .size = sizeof(Shader::BindlessSamplerSlots),
                                }
                              : vk::PushConstantRange{},
    };
    // sotc: a heap pipeline (heap_mode) has no layout: none is created.
    if (!heap_mode) {
        const auto layouts = layout_cache.Get(request, owned_layouts);
        set_layouts = layouts.sets;
        desc_layout = set_layouts[0];
        pipeline_layout = layouts.pipeline_layout;
        // sotc: the offsets are per set layout, so every pipeline queries them from the shared handle.
        QueryDescriptorBufferLayout(bindings);
        if (layouts.created) {
            SetObjectName(instance.GetDevice(), pipeline_layout, "Compute PipelineLayout {}",
                          debug_str);
        }
    }

    if (deferred) {
        ready.store(false, std::memory_order_relaxed);
        return;
    }
    CreatePipeline();
}

ComputePipeline::~ComputePipeline() = default;

void ComputePipeline::CreatePipeline() {
    // sotc: 64 where every stage can be pinned to it, otherwise 32 when the device could pick a
    // wider size (AMD): the shaders were lowered for 32 lanes (vk_instance.h). On Intel the
    // recompiler's lane count (Instance::RequiredSubgroupSize).
    const u32 required_size = instance.RequiredSubgroupSize(vk::ShaderStageFlagBits::eCompute);
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = required_size,
    };
    const bool require_size = required_size != 0;
    // sotc: SOTC_DESCRIPTOR_HEAP: a heap pipeline chains its mapping list ahead of the subgroup size.
    const VkShaderDescriptorSetAndBindingMappingInfoEXT heap_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext = require_size ? &subgroup_size_ci : nullptr,
        .mappingCount = static_cast<u32>(heap_plan.MappingsOf(vk::ShaderStageFlagBits::eCompute).size()),
        .pMappings = heap_plan.MappingsOf(vk::ShaderStageFlagBits::eCompute).data(),
    };
    const void* stage_next = require_size ? static_cast<const void*>(&subgroup_size_ci) : nullptr;
    if (heap_mode) {
        stage_next = &heap_info;
    }
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = stage_next,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    // sotc: SOTC_DESCRIPTOR_HEAP: the heap flag is a VkPipelineCreateFlags2 bit.
    const VkPipelineCreateFlags2CreateInfo heap_flags{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    // sotc: pipelines whose layouts are descriptor buffer layouts carry this flag.
    const bool n5_layout = DescriptorBufferN5Layout(instance, desc_heap, false, uses_bindless_samplers);
    const vk::PipelineCreateFlags pipeline_flags =
        instance.IsDescriptorBufferEnabled() || n5_layout
            ? vk::PipelineCreateFlags(vk::PipelineCreateFlagBits::eDescriptorBufferEXT)
            : vk::PipelineCreateFlags{};
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .pNext = heap_mode ? static_cast<const void*>(&heap_flags) : nullptr,
        .flags = pipeline_flags,
        .stage = shader_ci,
        .layout = heap_mode ? vk::PipelineLayout{} : pipeline_layout, // sotc: heap pipelines have none
    };
    // sotc: SOTC_PIPE_FEEDBACK / SOTC_PIPE_COMPILE_CHECK: the build is measured, and with the driver
    // cache probe on, a success there is the pipeline (no second build). Heap pipelines are not probed.
    const auto debug_name = [this] { return GetDebugString(); };
    auto [pipeline_result, pipe] = SotcPipeFeedback::Create(
        SotcPipeFeedback::Kind::Compute, 1, compute_pipeline_ci.pNext, !heap_mode,
        [&](const void* next, bool fail_on_compile) {
            vk::ComputePipelineCreateInfo ci = compute_pipeline_ci;
            ci.pNext = next;
            if (fail_on_compile) {
                ci.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
            }
            auto [result, handle] = instance.GetDevice().createComputePipelineUnique(cache, ci);
            return std::pair{result, std::move(handle)};
        },
        debug_name);
    SotcPipeFeedback::NoteBuilt(SotcPipeFeedback::Kind::Compute, &sotc_logged, pipeline_result,
                                debug_name);
    if (pipeline_result != vk::Result::eSuccess && from_cache) {
        // sotc: as for graphics pipelines, a cached one the driver refuses is dropped.
        LOG_WARNING(Render_Vulkan, "Cached compute pipeline {} failed to build: {}", GetDebugString(),
                    vk::to_string(pipeline_result));
        return;
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(instance.GetDevice(), *pipeline, "Compute Pipeline {}", GetDebugString());
}

void ComputePipeline::Build() {
    if (claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    CreatePipeline();
    ready.store(true, std::memory_order_release);
    ready.notify_all();
}

void ComputePipeline::WaitBuilt() {
    if (!claimed.load(std::memory_order_acquire)) {
        Build();
    }
    // sotc: SOTC_PIPE_COMPILE_CHECK: logs the pipeline if it was not ready here.
    SotcPipeFeedback::NoteWait(SotcPipeFeedback::Kind::Compute, &sotc_logged,
                               [this] { return GetDebugString(); });
    ready.wait(false, std::memory_order_acquire);
}

void ComputePipeline::BindWhenReady() const {
    if (ready.load(std::memory_order_acquire)) {
        scheduler.CommandBuffer().bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        return;
    }
    scheduler.RecordDeferred([this](vk::CommandBuffer cmdbuf) {
        if (!ready.load(std::memory_order_acquire)) {
            // sotc: SOTC_PIPE_COMPILE_CHECK: the recorder waits for this pipeline (logged once).
            SotcPipeFeedback::NoteWait(SotcPipeFeedback::Kind::Compute, &sotc_logged,
                                       [this] { return GetDebugString(); });
            const auto start = std::chrono::steady_clock::now();
            if (!claimed.load(std::memory_order_acquire)) {
                const_cast<ComputePipeline*>(this)->Build();
                SotcStats::async_compute_self_built.fetch_add(1, std::memory_order_relaxed);
            }
            ready.wait(false, std::memory_order_acquire);
            SotcStats::async_compute_wait_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                std::memory_order_relaxed);
            SotcStats::async_compute_waits.fetch_add(1, std::memory_order_relaxed);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    });
}

} // namespace Vulkan
