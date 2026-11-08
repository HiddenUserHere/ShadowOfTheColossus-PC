// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h" // sotc: SOTC_HEAP (UsesHeapImages)
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/sotc_pipe_feedback.h" // sotc: SOTC_PIPE_FEEDBACK / COMPILE_CHECK
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "../../../sotc/block_ledger.h" // sotc: SOTC_BLOCK_LEDGER
#include "../../../sotc/nonblocking.h"  // sotc: SOTC_NB_GPL

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

namespace {
/// sotc: SOTC_PIPE_GPL. The exact bytes a library is keyed by (compared in full, no hash).
class GplKey {
public:
    template <typename T>
    void Add(const T& value) {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    std::string bytes;
};
} // namespace

namespace {
thread_local bool t_gpl_lookup_only = false; // sotc: SOTC_NB_GPL (GplLibraryCache::SetLookupOnly)
thread_local bool t_gpl_libraries_only = false; // sotc: SOTC_GPL_NODROP (GplLibraryCache::SetLibrariesOnly)
thread_local u64 t_gpl_thread_built = 0;        // sotc: SOTC_GPL_NODROP (GplLibraryCache::ThreadBuilt)
thread_local u32 t_gpl_parts = 0;               // sotc: SOTC_GPL_PREDICT (GplLibraryCache::SetLibraryParts)
thread_local u8 t_gpl_origin = 0;               // sotc: SOTC_GPL_PREDICT (GplLibraryCache::SetOrigin)
thread_local bool t_gpl_draw_use = false;       // sotc: SOTC_GPL_PREDICT (GplLibraryCache::SetDrawUse)
thread_local u32 t_gpl_missing = 0;
thread_local bool t_gpl_discard = false;        // sotc: SOTC_GPL_PACK_LIBS (GplLibraryCache::SetDiscard)
thread_local u64 t_gpl_found = 0;               // sotc: SOTC_GPL_PACK_LIBS (GplLibraryCache::ThreadFound)
thread_local u64 t_gpl_failed = 0;              // sotc: SOTC_GPL_PACK_LIBS (GplLibraryCache::ThreadFailed)
thread_local u64 t_gpl_lib_id = 0;              // sotc: SOTC_GPL_PREDICT (GplLibraryCache::SetLibraryId)
u64 GplNowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}             // sotc: SOTC_GPL_PARALLEL_LIBS (GplLibraryCache::Missing)
} // namespace

void GplLibraryCache::SetLookupOnly(bool lookup_only) {
    t_gpl_lookup_only = lookup_only;
}

bool GplLibraryCache::LookupOnly() {
    return t_gpl_lookup_only;
}

void GplLibraryCache::SetLibrariesOnly(bool libraries_only) {
    t_gpl_libraries_only = libraries_only;
}

bool GplLibraryCache::LibrariesOnly() {
    return t_gpl_libraries_only;
}

u64 GplLibraryCache::ThreadBuilt() {
    return t_gpl_thread_built;
}

void GplLibraryCache::SetLibraryParts(u32 mask) {
    t_gpl_parts = mask;
}

bool GplLibraryCache::PartWanted(Part part) {
    return t_gpl_parts == 0 || (t_gpl_parts & (1u << part)) != 0;
}

void GplLibraryCache::SetOrigin(Origin origin) {
    t_gpl_origin = origin;
}

void GplLibraryCache::SetLibraryId(u64 id) {
    t_gpl_lib_id = id;
}

void GplLibraryCache::Touch(u64 id, u64 now_ns) {
    std::scoped_lock lock{refs_mutex};
    Ref& ref = refs[id];
    ++ref.count;
    ref.last_ns = now_ns;
}

size_t GplLibraryCache::Referenced() {
    std::scoped_lock lock{refs_mutex};
    return refs.size();
}

void GplLibraryCache::SetLibraryCache(vk::PipelineCache cache) {
    library_cache = cache;
}

vk::PipelineCache GplLibraryCache::LibraryCache() {
    return library_cache;
}

u64 GplLibraryCache::LibraryBuilds() {
    return library_builds.load(std::memory_order_relaxed);
}

size_t GplLibraryCache::EvictStale(u64 now_ns, u64 max_age_ns, const std::unordered_set<u64>& in_flight,
                                   Evicted& out) {
    std::scoped_lock lock{mutex, refs_mutex};
    size_t evicted = 0;
    for (const Part part : {PreRaster, Fragment}) { // the shader parts are the predicted ones
        auto& map = parts[part];
        for (auto it = map.begin(); it != map.end();) {
            const Entry& e = it->second;
            if ((e.flags & FlagPredicted) == 0 || (e.flags & FlagManifest) != 0 || e.lib_id == 0 ||
                in_flight.contains(e.lib_id)) {
                ++it;
                continue;
            }
            u64 last = e.last_use_ns;
            if (const auto ref = refs.find(e.lib_id); ref != refs.end()) {
                last = std::max(last, ref->second.last_ns);
            }
            if (now_ns - std::min(now_ns, last) <= max_age_ns) {
                ++it;
                continue;
            }
            out.pipes.push_back(std::move(it.value().pipe));
            out.ids.push_back(e.lib_id);
            it = map.erase(it);
            ++evicted;
        }
    }
    if (evicted != 0) {
        g_gpl_counters.predicted_evicted.fetch_add(evicted, std::memory_order_relaxed);
        g_gpl_counters.held.fetch_sub(evicted, std::memory_order_relaxed);
    }
    return evicted;
}

void GplLibraryCache::SetDrawUse(bool draw_use) {
    t_gpl_draw_use = draw_use;
}

void GplLibraryCache::SetDiscard(bool discard) {
    t_gpl_discard = discard;
}

u64 GplLibraryCache::ThreadFound() {
    return t_gpl_found;
}

u64 GplLibraryCache::ThreadFailed() {
    return t_gpl_failed;
}

void GplLibraryCache::ResetMissing() {
    t_gpl_missing = 0;
}

u32 GplLibraryCache::Missing() {
    return t_gpl_missing;
}

size_t GplLibraryCache::Count(Part part) {
    std::scoped_lock lock{mutex};
    return parts[part].size();
}

vk::Pipeline GplLibraryCache::GetOrBuild(Part part, const std::string& key, bool& built,
                                         const std::function<vk::UniquePipeline()>& build) {
    // sotc: SOTC_NB_GPL: the lock only covers the map, never a build. It was held while a worker compiled a library
    // (~17 ms each), so the command processor's lookups waited for whole builds (14.7 s in one 5 s window of a cold
    // run). Two threads may now build the same library; the second copy is dropped.
    {
        std::optional<SotcBlockLedger::Scope> lock_wait{std::in_place, SotcBlockLedger::GplLockWait};
        std::scoped_lock lock{mutex};
        lock_wait.reset();
        auto& map = parts[part];
        if (auto it = map.find(key); it != map.end()) {
            built = false;
            ++t_gpl_found;
            // sotc: SOTC_GPL_PREDICT: the first draw that finds a predicted library.
            if (t_gpl_draw_use) {
                it.value().last_use_ns = GplNowNs(); // the eviction keeps what draws use
                if ((it->second.flags & FlagUsed) == 0) {
                    it.value().flags |= FlagUsed;
                    if ((it->second.flags & FlagPredicted) != 0) {
                        g_gpl_counters.predicted_used.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((it->second.flags & FlagPrebuilt) != 0) {
                        g_gpl_counters.prebuilt_used.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else if (t_gpl_origin == OriginPredicted && PartWanted(part) && (it->second.flags & FlagUsed) != 0 &&
                       (it->second.flags & FlagPredicted) == 0) {
                // sotc: SOTC_GPL_PREDICT: a predicted job whose library a draw built first: built after the draw. The
                // library takes the predicted flag (its material references it from now on; evictable by age).
                it.value().flags |= FlagPredicted;
                it.value().lib_id = t_gpl_lib_id;
                g_gpl_counters.predicted_late.fetch_add(1, std::memory_order_relaxed);
            }
            return *it->second.pipe;
        }
    }
    // sotc: SOTC_GPL_PREDICT / SOTC_GPL_PARALLEL_LIBS: a part outside this thread's mask is only looked up.
    if (t_gpl_lookup_only || !PartWanted(part)) {
        built = false;
        t_gpl_missing |= 1u << part;
        return {}; // sotc: SOTC_NB_GPL, built by a worker's link job instead
    }
    vk::UniquePipeline library;
    {
        const SotcBlockLedger::Scope ledger{SotcBlockLedger::GplBuild};
        library = build();
    }
    if (!library) {
        built = false;
        ++t_gpl_failed; // sotc: SOTC_GPL_PACK_LIBS
        return {};
    }
    ++t_gpl_thread_built; // sotc: SOTC_GPL_NODROP, a library build on this thread (kept or not)
    library_builds.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_PLAY_QUIET: the pack cache grew
    if (t_gpl_discard) {
        // sotc: SOTC_GPL_PACK_LIBS: compiled into the driver's pipeline cache only; destroyed here.
        built = true;
        const vk::Pipeline handle = *library;
        library.reset();
        g_gpl_counters.pack_discarded.fetch_add(1, std::memory_order_relaxed);
        return handle;
    }
    std::scoped_lock lock{mutex};
    auto& map = parts[part];
    auto [it, inserted] = map.try_emplace(key, Entry{std::move(library), 0});
    built = inserted; // a library another thread stored first wins; this copy is destroyed with `library`
    if (inserted) {
        // sotc: SOTC_GPL_PREDICT counters (a draw that builds a library uses it at once).
        u8 flags = t_gpl_draw_use ? u8{FlagUsed} : u8{0};
        if (t_gpl_origin == OriginPredicted) {
            flags |= FlagPredicted;
            g_gpl_counters.predicted_built.fetch_add(1, std::memory_order_relaxed);
        } else if (t_gpl_origin == OriginUrgent) {
            g_gpl_counters.urgent_built.fetch_add(1, std::memory_order_relaxed);
        } else if (t_gpl_origin == OriginManifest) {
            flags |= FlagManifest; // sotc: SOTC_GPL_PREDICT: never evicted
        }
        it.value().lib_id = t_gpl_lib_id; // sotc: SOTC_GPL_PREDICT
        if (t_gpl_draw_use) {
            it.value().last_use_ns = GplNowNs();
        }
        // sotc: SOTC_GPL_DYN_STATE: built ahead of any draw (not for a waiting draw).
        if (!t_gpl_draw_use && t_gpl_origin != OriginUrgent) {
            flags |= FlagPrebuilt;
            g_gpl_counters.prebuilt_built.fetch_add(1, std::memory_order_relaxed);
        }
        it.value().flags = flags;
        g_gpl_counters.held.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_GPL_PACK_LIBS report
    }
    return *it->second.pipe;
}

namespace {
/// sotc: SOTC_GPL_DYN_STATE. A canonical topology of the class of t: with a dynamic topology a pipeline (or vertex
/// input library) only fixes the class, and the topology a draw sets must be of the same class.
vk::PrimitiveTopology TopologyClassOf(vk::PrimitiveTopology t) {
    switch (t) {
    case vk::PrimitiveTopology::ePointList:
        return vk::PrimitiveTopology::ePointList;
    case vk::PrimitiveTopology::eLineList:
    case vk::PrimitiveTopology::eLineStrip:
    case vk::PrimitiveTopology::eLineListWithAdjacency:
    case vk::PrimitiveTopology::eLineStripWithAdjacency:
        return vk::PrimitiveTopology::eLineList;
    case vk::PrimitiveTopology::ePatchList:
        return vk::PrimitiveTopology::ePatchList;
    default:
        return vk::PrimitiveTopology::eTriangleList;
    }
}
} // namespace

bool GraphicsPipeline::FastLinkable(const GraphicsPipelineKey& key,
                                    std::span<const Shader::Info*, MaxShaderStages> infos,
                                    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                                    const Shader::Profile& profile) {
    // Rect and quad lists take auxiliary tessellation shaders made from the fragment shader inputs,
    // which no library key here covers.
    if (key.prim_type == AmdGpu::PrimitiveType::RectList ||
        key.prim_type == AmdGpu::PrimitiveType::QuadList) {
        return false;
    }
    const Shader::Info* fs_info = infos[u32(Shader::SwStage::Fragment)];
    const auto& fs_runtime = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
    // Without a fragment shader, the constructor emits a discard shader from the vertex outputs.
    if (!fs_info && fs_runtime.clip_distance_emulation) {
        return false;
    }
    // The barycentric geometry shader (same test as the constructor below).
    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    const bool triangles = topology == vk::PrimitiveTopology::eTriangleList ||
                           topology == vk::PrimitiveTopology::eTriangleStrip ||
                           topology == vk::PrimitiveTopology::eTriangleFan;
    if (fs_info && profile.barycentric_via_gs && triangles &&
        !infos[u32(Shader::SwStage::Geometry)] && !infos[u32(Shader::SwStage::TessellationEval)] &&
        Shader::Backend::SPIRV::NeedsBarycentricGs(fs_runtime, *fs_info)) {
        return false;
    }
    return true;
}

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    LayoutCache& layout_cache, const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    const Shader::Gcn::FetchShaderData* fetch_shader_, std::span<const vk::ShaderModule> modules,
    SerializationSupport& sdata, bool preloading, bool async_build, GplLibraryCache* gpl)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_} {
    if (fetch_shader_) {
        fetch_shader = *fetch_shader_;
    }

    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    // sotc: built on a pipeline worker (SOTC_ASYNC_SHADERS), the guest registers already belong
    // to later draws: skip the buffer sharp reads, whose result is unused (as when preloading).
    const bool layout_created = BuildDescSetLayout(layout_cache, preloading || async_build);
    const auto debug_str = GetDebugString();
    if (layout_created) {
        SetObjectName(device, pipeline_layout, "Graphics PipelineLayout {}", debug_str);
    }

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }

    // sotc: SOTC_GPL_DYN_STATE. The same values as the static state above, set per draw where they are dynamic.
    const auto& kd = instance.KeyDynamic();
    key_statics = DynamicState::KeyStatics{
        .topology = topology,
        .polygon_mode = raster_chain.get().polygonMode,
        .depth_clamp_enable = raster_chain.get().depthClampEnable != VK_FALSE,
        .depth_clip_enable = key.depth_clip_enable != 0,
        .provoking_vertex = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                ? vk::ProvokingVertexModeEXT::eFirstVertex
                                : vk::ProvokingVertexModeEXT::eLastVertex,
        .clip_negative_one_to_one = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
        .rasterization_samples = LiverpoolToVK::NumSamples(
            key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
    };

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }
    // sotc: SOTC_GPL_DYN_STATE (Instance::KeyDynamic): the same list for every pipeline of the device, so the libraries
    // of one link always agree. Recorded per draw by DynamicState::Commit from key_statics.
    if (kd.topology) {
        dynamic_states.push_back(vk::DynamicState::ePrimitiveTopology);
    }
    if (kd.polygon_mode) {
        dynamic_states.push_back(vk::DynamicState::ePolygonModeEXT);
    }
    if (kd.depth_clamp) {
        dynamic_states.push_back(vk::DynamicState::eDepthClampEnableEXT);
    }
    if (kd.depth_clip) {
        dynamic_states.push_back(vk::DynamicState::eDepthClipEnableEXT);
    }
    if (kd.provoking) {
        dynamic_states.push_back(vk::DynamicState::eProvokingVertexModeEXT);
    }
    if (kd.clip_negative) {
        dynamic_states.push_back(vk::DynamicState::eDepthClipNegativeOneToOneEXT);
    }
    if (kd.samples) {
        dynamic_states.push_back(vk::DynamicState::eRasterizationSamplesEXT);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    // sotc: Profile::barycentric_via_gs: the geometry shader that hands the pixel shader its
    // per-vertex inputs and barycentrics, on triangles without a guest geometry/tess stage.
    const Shader::Info* fs_stage_info = infos[u32(Shader::SwStage::Fragment)];
    const bool triangles = topology == vk::PrimitiveTopology::eTriangleList ||
                           topology == vk::PrimitiveTopology::eTriangleStrip ||
                           topology == vk::PrimitiveTopology::eTriangleFan || is_rect_list ||
                           is_quad_list;
    // Preloaded pipelines take it from the cache: the interpolation data it needs is not stored.
    if (!preloading && profile.barycentric_via_gs && fs_stage_info && triangles &&
        !infos[u32(Shader::SwStage::Geometry)] &&
        !infos[u32(Shader::SwStage::TessellationEval)]) {
        const auto& fs_runtime = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        if (Shader::Backend::SPIRV::NeedsBarycentricGs(fs_runtime, *fs_stage_info)) {
            sdata.gs = Shader::Backend::SPIRV::EmitBarycentricGeometryShader(fs_runtime,
                                                                             *fs_stage_info);
        }
    }
    if (!sdata.gs.empty()) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = CompileSPV(sdata.gs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    // sotc: 64 where every stage can be pinned to it; on Intel the recompiler's lane count
    // (Instance::RequiredSubgroupSize).
    std::array<vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo, MaxShaderStages>
        subgroup_size_cis{};
    // sotc: SOTC_DESCRIPTOR_HEAP: a heap pipeline chains each stage's mapping list ahead of its subgroup size.
    std::array<VkShaderDescriptorSetAndBindingMappingInfoEXT, MaxShaderStages> heap_mapping_infos{};
    for (size_t i = 0; i < shader_stages.size(); ++i) {
        auto& stage = shader_stages[i];
        const u32 size = instance.RequiredSubgroupSize(stage.stage);
        subgroup_size_cis[i].requiredSubgroupSize = size;
        stage.pNext = size != 0 ? &subgroup_size_cis[i] : nullptr;
        if (heap_mode) {
            const auto& mappings = heap_plan.MappingsOf(stage.stage);
            heap_mapping_infos[i] = VkShaderDescriptorSetAndBindingMappingInfoEXT{
                .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
                .pNext = stage.pNext,
                .mappingCount = static_cast<u32>(mappings.size()),
                .pMappings = mappings.data(),
            };
            stage.pNext = &heap_mapping_infos[i];
        }
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    // sotc: pipelines whose layouts are descriptor buffer layouts carry this flag.
    const bool n5_layout =
        DescriptorBufferN5Layout(instance, desc_heap, stage_sets, uses_bindless_samplers);
    const vk::PipelineCreateFlags pipeline_flags =
        instance.IsDescriptorBufferEnabled() || n5_layout
            ? vk::PipelineCreateFlags(vk::PipelineCreateFlagBits::eDescriptorBufferEXT)
            : vk::PipelineCreateFlags{};
    // sotc: SOTC_DESCRIPTOR_HEAP: the heap flag is a VkPipelineCreateFlags2 bit, chained ahead of the rendering info.
    const VkPipelineCreateFlags2CreateInfo heap_flags{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &pipeline_rendering_ci,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const void* pipeline_next = heap_mode ? static_cast<const void*>(&heap_flags)
                                          : static_cast<const void*>(&pipeline_rendering_ci);
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = pipeline_next,
        .flags = pipeline_flags,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = heap_mode ? vk::PipelineLayout{} : pipeline_layout, // sotc: heap pipelines have none
    };

    // sotc: SOTC_PIPE_GPL. The pipeline as four libraries, each shared by every pipeline whose state
    // for that part is equal (the key is the exact bytes of that state, with the layout and the
    // dynamic states), then fast-linked without link-time optimization. A refused library or link
    // leaves the pipeline unbuilt (linked stays false): the caller keeps the draw skipped.
    // sotc: SOTC_DESCRIPTOR_HEAP pipelines are not linked from libraries (the heap flag and the mappings are per pipeline).
    if (gpl && !preloading && !heap_mode && FastLinkable(key, infos, runtime_infos, profile)) {
        const vk::PipelineLayout layout = pipeline_layout;
        const vk::PipelineCreateFlags lib_flags =
            pipeline_flags | vk::PipelineCreateFlagBits::eLibraryKHR;

        // Every part has the layout and the dynamic states: the libraries of one link must agree.
        // sotc: SOTC_NB_GPL: per-shader parts carry their own stage's layout (independent sets), the interface
        // parts none.
        const bool per_shader = gpl_vs_layout && gpl_fs_layout;
        const vk::PipelineLayout pre_layout = per_shader ? gpl_vs_layout : layout;
        const vk::PipelineLayout fs_layout = per_shader ? gpl_fs_layout : layout;
        const vk::PipelineLayout interface_layout = per_shader ? vk::PipelineLayout{} : layout;
        const auto add_common_with = [&](GplKey& k, vk::PipelineLayout part_layout) {
            k.Add(static_cast<VkPipelineLayout>(part_layout));
            k.Add(static_cast<u32>(dynamic_states.size()));
            for (const auto state : dynamic_states) {
                k.Add(static_cast<u32>(state));
            }
        };
        const auto add_common = [&](GplKey& k) { add_common_with(k, layout); };
        // The rendering info (attachment formats and samples) of the fragment and output parts.
        const auto add_render = [&](GplKey& k) {
            k.Add(pipeline_rendering_ci.colorAttachmentCount);
            for (u32 i = 0; i < pipeline_rendering_ci.colorAttachmentCount; ++i) {
                k.Add(static_cast<u32>(color_formats[i]));
                k.Add(static_cast<u32>(color_samples[i]));
            }
            k.Add(static_cast<u32>(pipeline_rendering_ci.depthAttachmentFormat));
            k.Add(static_cast<u32>(pipeline_rendering_ci.stencilAttachmentFormat));
            k.Add(static_cast<u32>(mixed_samples.depthStencilAttachmentSamples));
        };
        // Vertex input interface: the vertex attributes (when not dynamic) and the topology.
        GplKey vi_key;
        add_common_with(vi_key, interface_layout);
        // sotc: SOTC_GPL_DYN_STATE: with a dynamic topology only its class is baked (the library takes a canonical
        // topology of that class; the draw sets its own).
        const vk::PrimitiveTopology lib_topology = kd.topology ? TopologyClassOf(topology) : topology;
        vi_key.Add(static_cast<u32>(lib_topology));
        vi_key.Add(static_cast<u32>(instance.IsVertexInputDynamicState()));
        vi_key.Add(static_cast<u32>(sdata.vertex_attributes.size()));
        for (const auto& attr : sdata.vertex_attributes) {
            vi_key.Add(attr);
        }
        vi_key.Add(static_cast<u32>(sdata.vertex_bindings.size()));
        for (const auto& binding : sdata.vertex_bindings) {
            vi_key.Add(binding);
        }
        vi_key.Add(static_cast<u32>(sdata.divisors.size()));
        for (const auto& divisor : sdata.divisors) {
            vi_key.Add(divisor);
        }

        // Pre-rasterization: the vertex, tessellation and geometry modules, and the rasterization
        // state, a function of these key fields and of the instance.
        GplKey pre_key;
        add_common_with(pre_key, pre_layout);
        // sotc: SOTC_GPL_NODROP: a stage is keyed by its stage hash (program and permutation, as the pipeline map
        // keys it), not by its module handle. A program that leaves memory (the eviction at the end of the pretranslate
        // hold) comes back with new module handles, which made the libraries built at the boot hold unreachable, and a
        // destroyed module may give its handle to another one, which could reach a library of other code.
        for (const auto stage : {Shader::SwStage::Vertex, Shader::SwStage::Geometry,
                                 Shader::SwStage::TessellationControl,
                                 Shader::SwStage::TessellationEval}) {
            pre_key.Add(infos[u32(stage)] ? key.stage_hashes[u32(stage)] : size_t{0});
        }
        // sotc: SOTC_GPL_DYN_STATE: the key holds exactly what pre_ci holds besides the stages and layout: the
        // tessellation state (only read with tessellation stages), and each rasterization / clip state the device keeps
        // static (a dynamic one is given a canonical value below, so the library bakes no draw state). The topology is
        // not part of this state (input assembly belongs to the vertex input library; pre_ci has none).
        const bool has_tess = infos[u32(Shader::SwStage::TessellationControl)] ||
                              infos[u32(Shader::SwStage::TessellationEval)];
        pre_key.Add(has_tess ? static_cast<u32>(tessellation_state.patchControlPoints) : 0u);
        if (!kd.polygon_mode) {
            pre_key.Add(static_cast<u32>(key.polygon_mode));
        }
        if (!kd.provoking) {
            pre_key.Add(static_cast<u32>(key.provoking_vtx_last));
        }
        if (!kd.depth_clamp) {
            pre_key.Add(static_cast<u32>(key.depth_clamp_enable));
            pre_key.Add(static_cast<u32>(key.depth_clip_enable)); // the static clamp expression reads it
        }
        if (!kd.depth_clip) {
            pre_key.Add(static_cast<u32>(key.depth_clip_enable));
        }
        if (!kd.clip_negative) {
            pre_key.Add(static_cast<u32>(key.clip_space));
        }
        // The library's own copies of that state: canonical values for the dynamic parts.
        auto lib_raster_chain = raster_chain;
        {
            auto& r = lib_raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
            if (kd.polygon_mode) {
                r.polygonMode = vk::PolygonMode::eFill;
            }
            if (kd.depth_clamp) {
                r.depthClampEnable = VK_FALSE;
            }
            if (kd.provoking) {
                lib_raster_chain.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>()
                    .provokingVertexMode = vk::ProvokingVertexModeEXT::eFirstVertex;
            }
            if (kd.depth_clip) {
                lib_raster_chain.get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>().depthClipEnable =
                    VK_TRUE;
            }
        }
        const vk::PipelineViewportDepthClipControlCreateInfoEXT lib_clip_control = {
            .negativeOneToOne = kd.clip_negative ? VK_FALSE : clip_control.negativeOneToOne,
        };
        const vk::PipelineViewportStateCreateInfo lib_viewport_info = {
            .pNext = instance.IsDepthClipControlSupported() ? &lib_clip_control : nullptr,
        };
        const vk::PipelineInputAssemblyStateCreateInfo lib_input_assembly = {
            .topology = lib_topology,
        };
        // sotc: SOTC_GPL_DYN_STATE: with dynamic rasterization samples the fragment and output libraries take one
        // canonical multisample state (equal in both, as a link requires); the sample shading comes from the pixel
        // shader (in both keys).
        vk::PipelineMultisampleStateCreateInfo lib_multisampling = sdata.multisampling;
        if (kd.samples) {
            lib_multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        }

        // Fragment: the fragment module, the multisampling and the depth/stencil formats.
        GplKey fs_key;
        add_common_with(fs_key, fs_layout);
        fs_key.Add(infos[u32(Shader::SwStage::Fragment)] ? key.stage_hashes[u32(Shader::SwStage::Fragment)]
                                                         : size_t{0}); // sotc: the stage hash, as pre_key
        fs_key.Add(static_cast<u32>(lib_multisampling.rasterizationSamples));
        fs_key.Add(static_cast<u32>(lib_multisampling.sampleShadingEnable));
        // sotc: SOTC_NB_GPL: fragment shader state ignores the attachment formats (only the view mask, 0 here), so a
        // per-shader fragment library serves every render-target set. SOTC_GPL_DYN_STATE: a per-shader fragment library
        // is created without the rendering info (the view mask defaults to 0), so the depth/stencil formats are not
        // part of it either.
        if (!per_shader) {
            fs_key.Add(static_cast<u32>(key.z_format));
            fs_key.Add(static_cast<u32>(key.stencil_format));
            add_render(fs_key);
        }

        // Fragment output: the blend state, the multisampling and the rendering info.
        GplKey fo_key;
        add_common_with(fo_key, interface_layout);
        fo_key.Add(static_cast<u32>(lib_multisampling.rasterizationSamples));
        fo_key.Add(static_cast<u32>(lib_multisampling.sampleShadingEnable));
        fo_key.Add(static_cast<u32>(color_blending.logicOpEnable));
        fo_key.Add(static_cast<u32>(color_blending.logicOp));
        fo_key.Add(color_blending.attachmentCount);
        for (u32 i = 0; i < color_blending.attachmentCount; ++i) {
            fo_key.Add(attachments[i]);
        }
        add_render(fo_key);

        // Stages split by part: the fragment module is the fragment part, the rest pre-rasterization.
        boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
            pre_stages, fs_stages;
        for (const auto& stage : shader_stages) {
            (stage.stage == vk::ShaderStageFlagBits::eFragment ? fs_stages : pre_stages)
                .push_back(stage);
        }

        const auto get_library = [&](GplLibraryCache::Part part, const std::string& lib_key,
                                     const vk::GraphicsPipelineCreateInfo& ci) -> vk::Pipeline {
            bool built = false;
            const vk::Pipeline lib = gpl->GetOrBuild(part, lib_key, built, [&] {
                // sotc: SOTC_PIPE_FEEDBACK / SOTC_PIPE_COMPILE_CHECK (sotc_pipe_feedback.h)
                auto [result, handle] = SotcPipeFeedback::Create(
                    SotcPipeFeedback::Kind::GplLibrary, ci.stageCount, ci.pNext, true,
                    [&](const void* next, bool fail_on_compile) {
                        vk::GraphicsPipelineCreateInfo c = ci;
                        c.pNext = next;
                        if (fail_on_compile) {
                            c.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
                        }
                        // sotc: SOTC_PLAY_QUIET: libraries live in the pack cache (read-mostly, saved by the pack
                        // publication), not in the live cache the periodic saver reads.
                        const vk::PipelineCache lib_cache = GplLibraryCache::LibraryCache();
                        auto [r, h] = device.createGraphicsPipelineUnique(lib_cache ? lib_cache : pipeline_cache, c);
                        return std::pair{r, std::move(h)};
                    },
                    [&] { return debug_str; });
                SotcPipeFeedback::NoteBuilt(SotcPipeFeedback::Kind::GplLibrary, nullptr, result,
                                            [&] { return debug_str; });
                if (result != vk::Result::eSuccess) {
                    LOG_WARNING(Render_Vulkan, "Pipeline library {} failed to build: {}",
                                static_cast<u32>(part), vk::to_string(result));
                    return vk::UniquePipeline{};
                }
                return std::move(handle);
            });
            if (lib && built) {
                g_gpl_counters.lib_built[part].fetch_add(1, std::memory_order_relaxed);
            } else if (lib) {
                g_gpl_counters.lib_reused[part].fetch_add(1, std::memory_order_relaxed);
            }
            return lib;
        };

        vk::GraphicsPipelineLibraryCreateInfoEXT vi_lib{
            .flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface,
        };
        const vk::GraphicsPipelineCreateInfo vi_ci = {
            .pNext = &vi_lib,
            .flags = lib_flags,
            .pVertexInputState =
                !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
            .pInputAssemblyState = &lib_input_assembly,
            .pDynamicState = &dynamic_info,
            .layout = interface_layout,
        };
        const vk::Pipeline vi_handle =
            get_library(GplLibraryCache::VertexInput, vi_key.bytes, vi_ci);

        vk::GraphicsPipelineLibraryCreateInfoEXT pre_lib{
            .flags = vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
        };
        const vk::GraphicsPipelineCreateInfo pre_ci = {
            .pNext = &pre_lib,
            .flags = lib_flags,
            .stageCount = static_cast<u32>(pre_stages.size()),
            .pStages = pre_stages.data(),
            .pTessellationState = &tessellation_state,
            .pViewportState = &lib_viewport_info,
            .pRasterizationState = &lib_raster_chain.get(),
            .pDynamicState = &dynamic_info,
            .layout = pre_layout,
        };
        const vk::Pipeline pre_handle =
            get_library(GplLibraryCache::PreRaster, pre_key.bytes, pre_ci);

        vk::GraphicsPipelineLibraryCreateInfoEXT fs_lib{
            .pNext = per_shader ? nullptr : &pipeline_rendering_ci, // sotc: SOTC_GPL_DYN_STATE, see fs_key
            .flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
        };
        const vk::GraphicsPipelineCreateInfo fs_ci = {
            .pNext = &fs_lib,
            .flags = lib_flags,
            .stageCount = static_cast<u32>(fs_stages.size()),
            .pStages = fs_stages.data(),
            .pMultisampleState = &lib_multisampling,
            .pDepthStencilState =
                !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
            .pDynamicState = &dynamic_info,
            .layout = fs_layout,
        };
        const vk::Pipeline fs_handle =
            get_library(GplLibraryCache::Fragment, fs_key.bytes, fs_ci);

        vk::GraphicsPipelineLibraryCreateInfoEXT fo_lib{
            .pNext = &pipeline_rendering_ci,
            .flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
        };
        const vk::GraphicsPipelineCreateInfo fo_ci = {
            .pNext = &fo_lib,
            .flags = lib_flags,
            .pMultisampleState = &lib_multisampling,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = interface_layout,
        };
        const vk::Pipeline fo_handle =
            get_library(GplLibraryCache::FragmentOutput, fo_key.bytes, fo_ci);

        const std::array<vk::Pipeline, 4> libraries = {vi_handle, pre_handle, fs_handle,
                                                       fo_handle};
        // sotc: SOTC_GPL_NODROP boot libraries / SOTC_GPL_PREDICT library jobs: only the parts were wanted, no link.
        // A part outside the job's mask (GplLibraryCache::SetLibraryParts) may be missing; a wanted one that is missing
        // was refused by the driver.
        if (GplLibraryCache::LibrariesOnly()) {
            bool refused = false;
            for (u32 part = 0; part < GplLibraryCache::NumParts; ++part) {
                refused |= !libraries[part] && GplLibraryCache::PartWanted(static_cast<GplLibraryCache::Part>(part));
            }
            if (refused && !GplLibraryCache::LookupOnly()) {
                LOG_WARNING(Render_Vulkan, "Pipeline libraries of {} not built", debug_str);
                g_gpl_counters.link_failed.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (std::ranges::any_of(libraries, [](vk::Pipeline lib) { return !lib; })) {
            if (GplLibraryCache::LookupOnly()) {
                return; // sotc: SOTC_NB_GPL, a part is not built yet (a worker's link job builds it)
            }
            LOG_WARNING(Render_Vulkan, "Pipeline libraries of {} not built, not linked",
                        debug_str);
            g_gpl_counters.link_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const vk::PipelineLibraryCreateInfoKHR link_libraries = {
            .libraryCount = static_cast<u32>(libraries.size()),
            .pLibraries = libraries.data(),
        };
        const vk::GraphicsPipelineCreateInfo link_ci = {
            .pNext = &link_libraries,
            .flags = pipeline_flags,
            .pDynamicState = &dynamic_info,
            .layout = layout,
        };
        const auto link_start = std::chrono::steady_clock::now();
        // sotc: SOTC_PIPE_FEEDBACK / SOTC_PIPE_COMPILE_CHECK (sotc_pipe_feedback.h)
        auto [link_result, link_pipe] = SotcPipeFeedback::Create(
            SotcPipeFeedback::Kind::GplLink, link_ci.stageCount, link_ci.pNext, true,
            [&](const void* next, bool fail_on_compile) {
                vk::GraphicsPipelineCreateInfo c = link_ci;
                c.pNext = next;
                if (fail_on_compile) {
                    c.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
                }
                auto [r, h] = device.createGraphicsPipelineUnique(pipeline_cache, c);
                return std::pair{r, std::move(h)};
            },
            [&] { return debug_str; });
        SotcPipeFeedback::NoteBuilt(SotcPipeFeedback::Kind::GplLink, nullptr, link_result,
                                    [&] { return debug_str; });
        g_gpl_counters.link_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - link_start)
                .count(),
            std::memory_order_relaxed);
        if (link_result != vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "Fast link of graphics pipeline {} failed: {}", debug_str,
                        vk::to_string(link_result));
            g_gpl_counters.link_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_gpl_counters.links.fetch_add(1, std::memory_order_relaxed);
        pipeline = std::move(link_pipe);
        linked = true;
        SetObjectName(device, *pipeline, "Graphics Pipeline (fast linked) {}", debug_str);
        return;
    }

    // sotc: SOTC_PIPE_FEEDBACK / SOTC_PIPE_COMPILE_CHECK (sotc_pipe_feedback.h). Heap pipelines are not probed.
    auto [pipeline_result, pipe] = SotcPipeFeedback::Create(
        SotcPipeFeedback::Kind::Graphics, pipeline_info.stageCount, pipeline_info.pNext,
        !heap_mode,
        [&](const void* next, bool fail_on_compile) {
            vk::GraphicsPipelineCreateInfo c = pipeline_info;
            c.pNext = next;
            if (fail_on_compile) {
                c.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
            }
            auto [r, h] = device.createGraphicsPipelineUnique(pipeline_cache, c);
            return std::pair{r, std::move(h)};
        },
        [&] { return debug_str; });
    SotcPipeFeedback::NoteBuilt(SotcPipeFeedback::Kind::Graphics, nullptr, pipeline_result,
                                [&] { return debug_str; });
    if (pipeline_result != vk::Result::eSuccess && preloading) {
        // sotc: the driver can refuse a pipeline rebuilt from the cache (ErrorUnknown on AMD).
        // Left without a handle, the preload drops it and the game compiles it when it is drawn.
        LOG_WARNING(Render_Vulkan, "Cached graphics pipeline {} failed to build: {}", debug_str,
                    vk::to_string(pipeline_result));
        return;
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (fetch_shader.Empty() || fetch_shader.attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (u32 semantic = 0; semantic < fetch_shader.attributes.size(); ++semantic) {
        const auto& attrib = fetch_shader.attributes[semantic];
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = semantic,
            .binding = semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

bool GraphicsPipeline::BuildDescSetLayout(LayoutCache& layout_cache, bool preloading) {
    // sotc: SOTC_STAGE_BINDINGS: each stage numbers its bindings from 0 in its own set. Otherwise
    // every stage shares set 0 and the numbers run on across the stages.
    std::array<boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32>,
               Shader::MaxStageDescriptorSets>
        set_bindings;
    u32 num_sets{1};
    u32 binding{};
    u32 layout_buffers{}; // sotc: the buffer slots of the stages before (SOTC_VS_BINDING_BASE)
    uses_bindless_samplers = false;
    uses_slot_words = false; // sotc: SOTC_HEAP
    packet_range = false;    // sotc: SOTC_PACKET, set by the stages below
    // sotc: SOTC_BIND_DESC_REUSE: the flat buffer bindings of each set (indices into its bindings).
    ResetDynamicFlat();
    std::array<boost::container::small_vector<u32, 2>, Shader::MaxStageDescriptorSets> set_flat;

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const u32 set = stage_sets ? Shader::StageDescriptorSet(stage->sw_stage) : 0u;
        auto& bindings = set_bindings[set];
        if (stage_sets) {
            binding = 0;
            num_sets = std::max(num_sets, set + 1);
        }
        if (stage->sw_stage == Shader::SwStage::Vertex) { // sotc: SOTC_VS_BINDING_BASE, as the bind loop numbers it
            binding = Shader::Backend::VsStartBindings({binding, layout_buffers}, stage_sets).unified;
        }
        layout_buffers += static_cast<u32>(stage->buffers.size());
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            if (buffer.buffer_type == Shader::BufferType::Flatbuf) { // sotc: SOTC_BIND_DESC_REUSE
                set_flat[set].push_back(static_cast<u32>(bindings.size()));
            }
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (u32 image_index = 0; image_index < stage->images.size(); ++image_index) {
            const auto& image = stage->images[image_index];
            // sotc: SOTC_N5_BINDLESS: a sampled image of the heap has no binding (AddBindings).
            if (Shader::BindlessImageSlotOf(*stage, image_index) != Shader::NO_BINDLESS_IMAGE_SLOT) {
                continue;
            }
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        // sotc: SOTC_BINDLESS_SAMP: the samplers of this stage come from the global heap instead.
        const bool bindless = Shader::UsesBindlessSamplers(*stage);
        // sotc: SOTC_N5_BINDLESS: the heap image arrays are in the bindless set too.
        const bool bindless_images = Shader::UsesBindlessImages(*stage);
        uses_bindless_samplers |= bindless || bindless_images;
        // sotc: SOTC_HEAP: a heap stage reads its indices from the index table (the packet's), so it needs the heap set
        // but no slot words, and the pipeline has the packet range. Every other bindless stage reads slot words.
        const bool heap_stage = Shader::Backend::SPIRV::UsesHeapImages(*stage, profile);
        // sotc: SOTC_PACKET: a stage that addresses a buffer from its V# words reads the window words of the packet, and
        // so does the heap stage (its index table). Only then does the pipeline get the packet range.
        if (heap_stage || Shader::Backend::SPIRV::UsesShaderAddr(profile, *stage)) {
            packet_range = true;
        }
        uses_slot_words |= (bindless || bindless_images) && !heap_stage;
        for (const auto& sampler : stage->samplers) {
            if (bindless) {
                break;
            }
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    // sotc: descriptor buffers replace push descriptors (the two layout flags are exclusive).
    // Neither is used with a set per stage: their sets are bound by BindStageSets.
    // sotc: SOTC_N5_DESCBUF=1 adds its pipelines (DescriptorBufferN5Layout) to the same layout kind.
    const bool descriptor_buffer =
        !stage_sets && (instance.IsDescriptorBufferEnabled() ||
                        DescriptorBufferN5Layout(instance, desc_heap, stage_sets, uses_bindless_samplers));
    // sotc: SOTC_DESCRIPTOR_HEAP (vk_descriptor_heap.h): a pipeline whose set 0 bindings all map to the heaps is built
    // with the heap flag and no layout. Any other pipeline keeps the classic layout below.
    {
        HeapFallback pre = HeapFallback::Heap;
        if (stage_sets) {
            pre = HeapFallback::StageSets;
        } else if (descriptor_buffer) {
            pre = HeapFallback::DescriptorBuffer;
        } else if (uses_bindless_samplers) {
            pre = HeapFallback::Bindless;
        } else if (uses_slot_words) {
            pre = HeapFallback::SlotWords;
        } else if (packet_range) {
            pre = HeapFallback::PacketRange;
        }
        boost::container::small_vector<HeapBinding, 32> heap_bindings;
        for (const auto& b : set_bindings[0]) {
            heap_bindings.push_back(HeapBinding{
                .binding = b.binding,
                .count = b.descriptorCount,
                .sampler = b.descriptorType == vk::DescriptorType::eSampler,
                .stage = static_cast<vk::ShaderStageFlagBits>(
                    static_cast<VkShaderStageFlags>(b.stageFlags)),
            });
        }
        if (PlanHeapMode(heap_bindings, pre)) {
            uses_push_descriptors = false;
            return false;
        }
    }
    // sotc: maxPushDescriptors bounds the descriptors of the set, not its binding numbers. SOTC_VS_BINDING_BASE leaves a
    // gap below the vertex stage's first binding (48), so `binding` (the next free number) passed the limit (32 on
    // NVIDIA) for almost every draw, and they all fell to pool-allocated descriptor sets: the path behind the device
    // lost of 4 Nov (a draw reading an invalid descriptor, the same one in every crash). The count without the gap is
    // the one compared; with contiguous bindings it equals `binding`, as before.
    u32 set0_descriptors = 0;
    for (const auto& b : set_bindings[0]) {
        set0_descriptors += b.descriptorCount;
    }
    uses_push_descriptors =
        !stage_sets && !descriptor_buffer && set0_descriptors < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                       : descriptor_buffer
                           ? vk::DescriptorSetLayoutCreateFlagBits::eDescriptorBufferEXT
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    // sotc: SOTC_PACKET: a packet pipeline's range holds the DrawPacket after PushData (no slot words).
    if (packet_range && uses_slot_words) {
        for (const auto* stage : stages) {
            if (stage) {
                LOG_CRITICAL(Render_Vulkan,
                             "packet+slot words: stage {} pgm {:#x} variant {} heap {} bindless samplers {} "
                             "images {} samplers {} images bindless {} preloading {}",
                             u32(stage->sw_stage), stage->pgm_hash, u32(stage->variant),
                             Shader::Backend::SPIRV::UsesHeapImages(*stage, profile),
                             Shader::UsesBindlessSamplers(*stage), stage->images.size(),
                             stage->samplers.size(), Shader::UsesBindlessImages(*stage), preloading);
            }
        }
    }
    ASSERT_MSG(!(packet_range && uses_slot_words),
               "packet pipeline with bindless slot words (the heap replaces them)");
    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        // sotc: SOTC_HEAP: a heap profile's packet range also holds the heap block (HeapPushBytes).
        .size = packet_range ? (profile.heap_images != 0 ? Shader::HeapPushBytes : PacketPushBytes)
                             : static_cast<u32>(sizeof(Shader::PushData)),
    };
    // sotc: SOTC_BINDLESS_SAMP: the sampler slots follow PushData, in a layout with a bindless stage.
    const vk::PushConstantRange slot_push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = sizeof(Shader::PushData),
        .size = sizeof(Shader::BindlessSamplerSlots),
    };
    // sotc: SOTC_BIND_DESC_REUSE: in pool-allocated sets (not push descriptors or descriptor buffers, which cannot
    // hold dynamic descriptors) the flat buffers become dynamic storage buffers. With a set per stage the device limit
    // is split between the sets a layout can have, so a stage's set layout never depends on the other stages.
    if (!uses_push_descriptors && !descriptor_buffer) {
        const u32 limit = instance.MaxDescriptorSetStorageBuffersDynamic();
        const u32 cap = stage_sets ? limit / Shader::MaxStageDescriptorSets : limit;
        for (u32 set = 0; set < num_sets; ++set) {
            MarkDynamicFlat(set_bindings[set], set_flat[set], set, cap);
        }
    } else {
        for (u32 set = 0; set < num_sets; ++set) {
            flat_bindings += static_cast<u32>(set_flat[set].size());
        }
    }
    FinishDynamicFlat();
    boost::container::small_vector<LayoutCache::SetRequest, Shader::MaxStageDescriptorSets>
        requests;
    for (u32 set = 0; set < num_sets; ++set) {
        requests.push_back({
            .bindings = set_bindings[set],
            .flags = flags,
        });
    }
    // sotc: SOTC_NB_GPL: per-shader libraries need a layout with independent sets, on every such graphics pipeline
    // (linked or optimized, so both bind their sets the same way): one set per stage, vertex + fragment only.
    // sotc: SOTC_GPL_NODROP too (GplPerShader).
    const bool per_shader_gpl = stage_sets && SotcNonBlocking::GplPerShader() &&
                                instance.IsGraphicsPipelineLibrary() &&
                                num_sets == 2 && !uses_bindless_samplers && !uses_slot_words && !packet_range;
    const LayoutCache::Request request{
        .sets = requests,
        .push_constants = push_constants,
        .trailing_set = uses_bindless_samplers ? desc_heap.BindlessSamplerLayout()
                                               : vk::DescriptorSetLayout{},
        .slot_constants = uses_slot_words ? slot_push_constants : vk::PushConstantRange{},
        .layout_flags = per_shader_gpl ? vk::PipelineLayoutCreateFlags{vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT}
                                       : vk::PipelineLayoutCreateFlags{},
    };
    const auto layouts = layout_cache.Get(request, owned_layouts);
    set_layouts = layouts.sets;
    desc_layout = set_layouts[0];
    pipeline_layout = layouts.pipeline_layout;
    // sotc: SOTC_NB_GPL: the per-shader library layouts. Same push range and flags; the fragment one holds only the
    // fragment set (set 0), the vertex one a null set 0 and the vertex set (set 1).
    gpl_vs_layout = vk::PipelineLayout{};
    gpl_fs_layout = vk::PipelineLayout{};
    if (per_shader_gpl) {
        const std::array<LayoutCache::SetRequest, 1> fs_sets{requests[0]};
        const LayoutCache::Request fs_request{
            .sets = fs_sets,
            .push_constants = push_constants,
            .layout_flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT,
        };
        gpl_fs_layout = layout_cache.Get(fs_request, owned_gpl_layouts[0]).pipeline_layout;
        const std::array<LayoutCache::SetRequest, 2> vs_sets{LayoutCache::SetRequest{.null_set = true},
                                                             requests[1]};
        const LayoutCache::Request vs_request{
            .sets = vs_sets,
            .push_constants = push_constants,
            .layout_flags = vk::PipelineLayoutCreateFlagBits::eIndependentSetsEXT,
        };
        gpl_vs_layout = layout_cache.Get(vs_request, owned_gpl_layouts[1]).pipeline_layout;
    }
    // sotc: the offsets are per set layout, so every pipeline queries them from the shared handle.
    if (!stage_sets) {
        QueryDescriptorBufferLayout(set_bindings[0]);
    }
    return layouts.created;
}

} // namespace Vulkan
