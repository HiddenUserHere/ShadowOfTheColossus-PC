// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <string>
#include <boost/container/small_vector.hpp>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h" // sotc: SOTC_HEAP (UsesHeapImages)
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/sotc_pipe_feedback.h" // sotc: SOTC_PIPE_FEEDBACK / COMPILE_CHECK
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "../../../sotc/block_ledger.h" // sotc: SOTC_BLOCK_LEDGER
#include "../../../sotc/nonblocking.h"  // sotc: SOTC_COMPUTE_FAST

namespace Vulkan {

namespace {
// sotc: SOTC_COMPUTE_FAST counters (totals; ReportComputeFast prints the 5 s window). Fast builds and their time,
// optimized pipelines the driver cache had at the fast path (probe hits), optimized builds that replaced a fast
// handle and their time, recorder waits on a fast-path build and their time, evidence lines printed.
struct ComputeFastStats {
    std::atomic<u64> fast_builds{0}, fast_ns{0};
    std::atomic<u64> probe_hits{0};
    std::atomic<u64> swaps{0}, opt_ns{0};
    std::atomic<u64> rec_waits{0}, rec_wait_ns{0};
    std::atomic<u32> logged{0};
} g_cfast;

u64 SinceNs(std::chrono::steady_clock::time_point start) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

// sotc: SOTC_COMPUTE_FAST: an optimized build finished. Counted when it replaced a fast handle; the first 32 print the
// evidence line (does the driver honour DISABLE_OPTIMIZATION: fast vs optimized build time).
void NoteOptimized(u64 key, u64 fast_ns, u64 opt_ns, bool swapped) {
    if (!swapped) {
        return;
    }
    g_cfast.swaps.fetch_add(1, std::memory_order_relaxed);
    g_cfast.opt_ns.fetch_add(opt_ns, std::memory_order_relaxed);
    if (g_cfast.logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::fprintf(stderr, "SOTC COMPUTE FAST: %016llx fast build %.1f ms, optimized %.1f ms\n",
                     static_cast<unsigned long long>(key), static_cast<double>(fast_ns) / 1e6,
                     static_cast<double>(opt_ns) / 1e6);
    }
}
} // namespace

ComputeSpecFreeStats g_specfree; // sotc: SOTC_COMPUTE_SPECFREE

// sotc: SOTC_COMPUTE_SPECFREE, the 5 s report line: dispatches, prebuilt hits and fallbacks of the window; prebuilt
// programs, variants, compile time and first-dispatch translations are totals. Nothing while the switch is off or
// nothing moved. Call from one thread.
void ReportComputeSpecFree(std::string& out, double seconds) {
    (void)seconds;
    if (!Shader::ComputeSpecFree()) {
        return;
    }
    static u64 prev[9]{};
    const u64 now[9] = {
        g_specfree.dispatches.load(std::memory_order_relaxed),
        g_specfree.hits.load(std::memory_order_relaxed),
        g_specfree.fallback_dim.load(std::memory_order_relaxed),
        g_specfree.fallback_int.load(std::memory_order_relaxed),
        g_specfree.fallback_written_fmt.load(std::memory_order_relaxed),
        g_specfree.fallback_atomic.load(std::memory_order_relaxed),
        g_specfree.fallback_other.load(std::memory_order_relaxed),
        g_specfree.programs.load(std::memory_order_relaxed),
        g_specfree.not_prebuilt.load(std::memory_order_relaxed),
    };
    u64 d[9];
    bool any = false;
    for (int i = 0; i < 9; ++i) {
        d[i] = now[i] - prev[i];
        prev[i] = now[i];
        any = any || d[i] != 0;
    }
    if (!any) {
        return;
    }
    const u64 fallback = d[2] + d[3] + d[4] + d[5] + d[6];
    char line[640];
    std::snprintf(line, sizeof(line),
                  "SOTC COMPUTE SPECFREE: dispatches %llu, prebuilt hits %llu, fallback %llu (dim %llu, int %llu, "
                  "written-fmt %llu, atomic %llu, other %llu), prebuilt programs %llu (variants %llu), compile at load "
                  "%.1f s, first-dispatch programs %llu; perms per program 1:%llu 2:%llu 4:%llu 8:%llu over-cap:%llu "
                  "(queued %llu), int fallbacks of over-cap programs %llu\n",
                  static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]),
                  static_cast<unsigned long long>(fallback), static_cast<unsigned long long>(d[2]),
                  static_cast<unsigned long long>(d[3]), static_cast<unsigned long long>(d[4]),
                  static_cast<unsigned long long>(d[5]), static_cast<unsigned long long>(d[6]),
                  static_cast<unsigned long long>(now[7]),
                  static_cast<unsigned long long>(g_specfree.variants.load(std::memory_order_relaxed)),
                  static_cast<double>(g_specfree.compile_ns.load(std::memory_order_relaxed)) / 1e9,
                  static_cast<unsigned long long>(now[8]),
                  // sotc: the int/float variant combinations (totals, ComputeSpecFreeStats::perms_hist)
                  static_cast<unsigned long long>(g_specfree.perms_hist[0].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.perms_hist[1].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.perms_hist[2].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.perms_hist[3].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.perms_hist[4].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.perms_queued.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_specfree.fallback_int_capped.load(std::memory_order_relaxed)));
    out += line;
}

// sotc: SOTC_COMPUTE_FAST, the 5 s report line (window counts). Nothing while the switch is off or the window was empty.
// Call from one thread.
void ReportComputeFast(std::string& out, double seconds) {
    (void)seconds;
    if (!SotcNonBlocking::ComputeFast()) {
        return;
    }
    static u64 prev[7]{};
    const u64 now[7] = {
        g_cfast.fast_builds.load(std::memory_order_relaxed), g_cfast.fast_ns.load(std::memory_order_relaxed),
        g_cfast.swaps.load(std::memory_order_relaxed),       g_cfast.opt_ns.load(std::memory_order_relaxed),
        g_cfast.rec_waits.load(std::memory_order_relaxed),   g_cfast.rec_wait_ns.load(std::memory_order_relaxed),
        g_cfast.probe_hits.load(std::memory_order_relaxed),
    };
    u64 d[7];
    bool any = false;
    for (int i = 0; i < 7; ++i) {
        d[i] = now[i] - prev[i];
        prev[i] = now[i];
        any = any || d[i] != 0;
    }
    if (!any) {
        return;
    }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "SOTC COMPUTE FAST (5 s): fast builds %llu (%.1f ms total), optimized swaps %llu (%.1f ms total), "
                  "recorder waits on fast builds %llu (%.1f ms), driver-cache hits %llu\n",
                  static_cast<unsigned long long>(d[0]), static_cast<double>(d[1]) / 1e6,
                  static_cast<unsigned long long>(d[2]), static_cast<double>(d[3]) / 1e6,
                  static_cast<unsigned long long>(d[4]), static_cast<double>(d[5]) / 1e6,
                  static_cast<unsigned long long>(d[6]));
    out += line;
}

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
    boost::container::small_vector<u32, 2> flat; // sotc: SOTC_BIND_DESC_REUSE: the flat buffer bindings
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        if (buffer.buffer_type == Shader::BufferType::Flatbuf) { // sotc: SOTC_BIND_DESC_REUSE
            flat.push_back(static_cast<u32>(bindings.size()));
        }
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
    // sotc: SOTC_BIND_DESC_REUSE: a pool-allocated set gets dynamic flat buffers (see GraphicsPipeline).
    ResetDynamicFlat();
    if (!heap_mode && !uses_push_descriptors && !descriptor_buffer) {
        MarkDynamicFlat(bindings, flat, 0, instance.MaxDescriptorSetStorageBuffersDynamic());
    } else {
        flat_bindings = static_cast<u32>(flat.size());
    }
    FinishDynamicFlat();
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
    auto [result, built] = MakePipeline(BuildMode::Normal);
    Install(std::move(built), true);
}

std::pair<vk::Result, vk::UniquePipeline> ComputePipeline::MakePipeline(BuildMode mode) {
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
    // sotc: SOTC_COMPUTE_FAST: with the flags-2 struct chained the legacy flags are ignored, so the mode's bit goes here
    // too. The bits are part of the create info, so the driver cache keeps the fast and the optimized builds apart.
    const VkPipelineCreateFlags2 mode_flags2 =
        mode == BuildMode::Fast    ? VkPipelineCreateFlags2{VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT}
        : mode == BuildMode::Probe ? VkPipelineCreateFlags2{VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT}
                                   : VkPipelineCreateFlags2{0};
    const VkPipelineCreateFlags2CreateInfo heap_flags{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT | mode_flags2,
    };
    // sotc: pipelines whose layouts are descriptor buffer layouts carry this flag.
    const bool n5_layout = DescriptorBufferN5Layout(instance, desc_heap, false, uses_bindless_samplers);
    vk::PipelineCreateFlags pipeline_flags =
        instance.IsDescriptorBufferEnabled() || n5_layout
            ? vk::PipelineCreateFlags(vk::PipelineCreateFlagBits::eDescriptorBufferEXT)
            : vk::PipelineCreateFlags{};
    if (mode == BuildMode::Fast) { // sotc: SOTC_COMPUTE_FAST
        pipeline_flags |= vk::PipelineCreateFlagBits::eDisableOptimization;
    } else if (mode == BuildMode::Probe) {
        pipeline_flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
    }
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .pNext = heap_mode ? static_cast<const void*>(&heap_flags) : nullptr,
        .flags = pipeline_flags,
        .stage = shader_ci,
        .layout = heap_mode ? vk::PipelineLayout{} : pipeline_layout, // sotc: heap pipelines have none
    };
    if (mode == BuildMode::Probe) {
        // sotc: SOTC_COMPUTE_FAST: the optimized pipeline when the driver cache has it; compile-required otherwise.
        auto [result, handle] = instance.GetDevice().createComputePipelineUnique(cache, compute_pipeline_ci);
        if (result != vk::Result::eSuccess) {
            return {result, vk::UniquePipeline{}};
        }
        SetObjectName(instance.GetDevice(), *handle, "Compute Pipeline {}", GetDebugString());
        return {result, std::move(handle)};
    }
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
    // sotc: SOTC_COMPUTE_FAST: a refused fast build is not fatal (the caller makes the normal build instead).
    if (pipeline_result != vk::Result::eSuccess && (from_cache || mode == BuildMode::Fast)) {
        // sotc: as for graphics pipelines, a cached one the driver refuses is dropped.
        LOG_WARNING(Render_Vulkan, "Cached compute pipeline {} failed to build: {}", GetDebugString(),
                    vk::to_string(pipeline_result));
        return {pipeline_result, vk::UniquePipeline{}};
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    SetObjectName(instance.GetDevice(), *pipe, "Compute Pipeline {}", GetDebugString());
    return {pipeline_result, std::move(pipe)};
}

bool ComputePipeline::Install(vk::UniquePipeline built, bool optimized) {
    if (!built) {
        return false;
    }
    vk::UniquePipeline retired;
    {
        std::scoped_lock lock{sotc_swap_mutex};
        if (!pipeline) {
            pipeline = std::move(built);
            sotc_fast_live.store(!optimized, std::memory_order_release);
            compute_handle.store(static_cast<VkPipeline>(*pipeline), std::memory_order_release);
            return false;
        }
        if (!optimized || !sotc_fast_live.load(std::memory_order_relaxed)) {
            return false; // a second build of the same kind, or a fast one after the optimized: never bound, freed here
        }
        retired = std::move(pipeline);
        pipeline = std::move(built);
        compute_handle.store(static_cast<VkPipeline>(*pipeline), std::memory_order_release);
        sotc_fast_live.store(false, std::memory_order_release);
    }
    // sotc: SOTC_COMPUTE_FAST: every bind that read the fast handle was recorded into a tick up to the command
    // processor's current one (CurrentTick advances on the command processor at each Flush; a deferred bind reads the
    // handle when the recorder runs it, which was before this swap when it read the fast one, and it was queued in a
    // tick no later than the current one). The fast pipeline is freed once the GPU finished that tick; later binds read
    // the optimized handle. The async compute queue never records a fast handle (the rasterizer checks OnFastBuild).
    scheduler.DeferOperation([old = std::move(retired)] {});
    return true;
}

void ComputePipeline::Build() {
    if (claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    sotc_normal_claim.store(true, std::memory_order_release); // sotc: SOTC_COMPUTE_FAST (BindWhenReady)
    const auto start = std::chrono::steady_clock::now();
    auto [result, built] = MakePipeline(BuildMode::Normal);
    // sotc: SOTC_COMPUTE_FAST: the recorder may have bound a fast build of it meanwhile (BuildFastBeside): swapped.
    const bool swapped = Install(std::move(built), true);
    NoteOptimized(compute_key.value, sotc_fast_ns.load(std::memory_order_relaxed), SinceNs(start), swapped);
    ready.store(true, std::memory_order_release);
    ready.notify_all();
}

bool ComputePipeline::BuildFast() {
    if (claimed.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    sotc_fast_running.store(true, std::memory_order_release);
    const auto start = std::chrono::steady_clock::now();
    bool follow = false;
    // The optimized pipeline when the driver cache has it (a later launch, or another build of the same create info).
    // Heap pipelines are not probed (as in SotcPipeFeedback::Create).
    if (!heap_mode && SotcPipeFeedback::g_compile_control.load(std::memory_order_relaxed)) {
        auto [probe_result, hit] = MakePipeline(BuildMode::Probe);
        if (hit) {
            Install(std::move(hit), true);
            g_cfast.probe_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!compute_handle.load(std::memory_order_acquire)) {
        auto [fast_result, fast] = MakePipeline(BuildMode::Fast);
        if (fast) {
            const u64 ns = SinceNs(start);
            sotc_fast_ns.store(ns, std::memory_order_relaxed);
            Install(std::move(fast), false);
            g_cfast.fast_builds.fetch_add(1, std::memory_order_relaxed);
            g_cfast.fast_ns.fetch_add(ns, std::memory_order_relaxed);
            follow = true;
        } else {
            // Refused with DISABLE_OPTIMIZATION: the normal build here, as without the switch.
            sotc_normal_claim.store(true, std::memory_order_release);
            auto [normal_result, normal] = MakePipeline(BuildMode::Normal);
            Install(std::move(normal), true);
        }
    }
    sotc_fast_running.store(false, std::memory_order_release);
    ready.store(true, std::memory_order_release);
    ready.notify_all();
    return follow;
}

void ComputePipeline::BuildFastBeside() {
    if (sotc_fast_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto [result, fast] = MakePipeline(BuildMode::Fast);
    if (fast) {
        const u64 ns = SinceNs(start);
        sotc_fast_ns.store(ns, std::memory_order_relaxed);
        Install(std::move(fast), false); // dropped when the worker's optimized build landed first
        g_cfast.fast_builds.fetch_add(1, std::memory_order_relaxed);
        g_cfast.fast_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    sotc_fast_running.store(false, std::memory_order_release);
    if (compute_handle.load(std::memory_order_acquire)) {
        ready.store(true, std::memory_order_release);
        ready.notify_all();
    }
}

void ComputePipeline::BuildOptimized() {
    const auto start = std::chrono::steady_clock::now();
    auto [result, built] = MakePipeline(BuildMode::Normal);
    const bool swapped = Install(std::move(built), true);
    NoteOptimized(compute_key.value, sotc_fast_ns.load(std::memory_order_relaxed), SinceNs(start), swapped);
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
        // sotc: SOTC_COMPUTE_FAST: the current handle (fast or optimized; see Install).
        scheduler.CommandBuffer().bindPipeline(vk::PipelineBindPoint::eCompute, Handle());
        return;
    }
    scheduler.RecordDeferred([this](vk::CommandBuffer cmdbuf) {
        if (!ready.load(std::memory_order_acquire)) {
            // sotc: SOTC_PIPE_COMPILE_CHECK: the recorder waits for this pipeline (logged once).
            SotcPipeFeedback::NoteWait(SotcPipeFeedback::Kind::Compute, &sotc_logged,
                                       [this] { return GetDebugString(); });
            const auto start = std::chrono::steady_clock::now();
            // sotc: demand includes a build taken by the recorder, not only ready.wait below.
            const SotcBlockLedger::Scope ledger{SotcBlockLedger::ComputeWait, compute_key.value};
            bool self_built = false;
            auto* self = const_cast<ComputePipeline*>(this);
            // sotc: SOTC_COMPUTE_FAST: the recorder waits only for a fast build: its own when no worker claimed the
            // pipeline (the optimized build is queued behind it), or one beside a worker's optimized build. A pack
            // listed (inline, a cache hit) or pack prebuild pipeline keeps the normal build.
            const bool fast = SotcNonBlocking::ComputeFast() && !sotc_inline && !sotc_pack;
            bool fast_wait = false;
            if (!claimed.load(std::memory_order_acquire)) {
                if (fast) {
                    if (self->BuildFast() && QueueOptimizedHook) {
                        QueueOptimizedHook(self);
                    }
                    fast_wait = !sotc_normal_claim.load(std::memory_order_acquire);
                } else {
                    self->Build();
                }
                self_built = true;
                SotcStats::async_compute_self_built.fetch_add(1, std::memory_order_relaxed);
            }
            if (fast && !ready.load(std::memory_order_acquire)) {
                if (sotc_normal_claim.load(std::memory_order_acquire)) {
                    self->BuildFastBeside(); // a worker's optimized build is running (or just finished)
                }
                fast_wait = true; // waiting for a fast build (this one, or a worker's from the Urgent lane)
            }
            ready.wait(false, std::memory_order_acquire);
            const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
            if (fast_wait) {
                g_cfast.rec_waits.fetch_add(1, std::memory_order_relaxed);
                g_cfast.rec_wait_ns.fetch_add(static_cast<u64>(spent), std::memory_order_relaxed);
            }
            // sotc: SOTC_COMPUTE_PACK: a listed pipeline (pack cache hit) built here by design is an inline build,
            // not a wait for a worker.
            if (sotc_inline && self_built) {
                SotcStats::async_compute_inline_ns.fetch_add(spent, std::memory_order_relaxed);
                SotcStats::async_compute_inline_built.fetch_add(1, std::memory_order_relaxed);
            } else {
                SotcStats::async_compute_wait_ns.fetch_add(spent, std::memory_order_relaxed);
                SotcStats::async_compute_waits.fetch_add(1, std::memory_order_relaxed);
            }
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, Handle()); // sotc: SOTC_COMPUTE_FAST (Install)
    });
}

} // namespace Vulkan
