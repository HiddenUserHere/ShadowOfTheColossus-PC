// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

/// sotc: SOTC_PIPE_GPL=1. Graphics pipeline libraries (VK_EXT_graphics_pipeline_library) that the
/// fast-linked pipelines of one PipelineCache share. A part is keyed by the exact bytes of the state
/// it was created from (layout included), so a library is reused only for an identical part.
/// Thread-safe: one mutex, held while a missing library is built.
class GplLibraryCache {
public:
    enum Part : u32 { VertexInput, PreRaster, Fragment, FragmentOutput, NumParts };

    /// Returns the library stored under key, or builds it with build (a null handle on failure).
    /// built tells whether build ran.
    vk::Pipeline GetOrBuild(Part part, const std::string& key, bool& built,
                            const std::function<vk::UniquePipeline()>& build);

    /// sotc: SOTC_NB_GPL. While set on the calling thread (the command processor's TryFastLink), GetOrBuild only looks
    /// up: a missing library is not built, and the link fails instead.
    static void SetLookupOnly(bool lookup_only);
    static bool LookupOnly();

private:
    std::mutex mutex;
    tsl::robin_map<std::string, vk::UniquePipeline> parts[NumParts];
};

/// sotc: SOTC_PIPE_GPL counters, summed since the start. Written by GraphicsPipeline and
/// PipelineCache, printed by ReportPipeGpl (vk_pipeline_cache.cpp).
struct GplCounters {
    std::atomic<u64> cold_misses{};       ///< draws that found no built pipeline for their key
    std::atomic<u64> unlinkable{};        ///< cold misses whose state the fast link does not cover
    std::atomic<u64> linked_served{};     ///< draws served by a fast-linked pipeline
    std::array<std::atomic<u64>, GplLibraryCache::NumParts> lib_built{}; ///< libraries created
    std::array<std::atomic<u64>, GplLibraryCache::NumParts> lib_reused{}; ///< libraries found
    std::atomic<u64> links{};             ///< fast links that succeeded
    std::atomic<u64> link_failed{};       ///< library or link creation refused by the driver
    std::atomic<u64> link_ns{};           ///< time in vkCreateGraphicsPipelines for the links
    std::atomic<u64> replaced{};          ///< optimized builds that replaced a linked pipeline
};
inline GplCounters g_gpl_counters;

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};
        /// sotc: Profile::barycentric_via_gs (EmitBarycentricGeometryShader). Stored only in the
        /// caches of that profile (with_gs), so the other caches keep their format.
        std::vector<u32> gs{};
        static inline bool with_gs{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     LayoutCache& layout_cache, const Shader::Profile& profile,
                     const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     const Shader::Gcn::FetchShaderData* fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, bool async_build = false,
                     GplLibraryCache* gpl = nullptr);
    ~GraphicsPipeline();

    /// sotc: true when this pipeline was fast-linked from pipeline libraries (SOTC_PIPE_GPL), and
    /// so is replaced by the optimized build once that one is collected.
    bool IsLinked() const noexcept {
        return linked;
    }

    /// sotc: SOTC_PIPE_GPL: whether the state of key, with these stages, can be fast-linked. The
    /// constructor links only when this is true (with a library cache).
    static bool FastLinkable(const GraphicsPipelineKey& key,
                             std::span<const Shader::Info*, MaxShaderStages> infos,
                             std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                             const Shader::Profile& profile);

    const Shader::Gcn::FetchShaderData& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// sotc: N5. Draws that bound this pipeline (command processor only), and the count its blob
    /// holds (saved_use_count), so SaveUseCounts rewrites only the blobs that changed.
    mutable u32 use_count{};
    u32 saved_use_count{};

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    /// sotc: sets desc_layout and pipeline_layout; true when this pipeline created them.
    bool BuildDescSetLayout(LayoutCache& layout_cache, bool preloading);

private:
    GraphicsPipelineKey key;
    Shader::Gcn::FetchShaderData fetch_shader{};
    bool linked{}; ///< sotc: SOTC_PIPE_GPL: built by fast-linking the libraries
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
