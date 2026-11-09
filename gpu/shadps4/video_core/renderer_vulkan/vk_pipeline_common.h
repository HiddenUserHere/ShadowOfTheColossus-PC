// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_descriptor_heap.h" // sotc: SOTC_DESCRIPTOR_HEAP (heap plan of a pipeline)
#include "video_core/renderer_vulkan/vk_resource_pool.h"    // sotc: SOTC_BIND_DESC_REUSE (DescriptorSetReuse)
#include "video_core/sotc_arch/sotc_arch.h"

#include <atomic> // sotc: SOTC_GCN_COMMIT (Pipeline::sotc_gcn_commit)
#include <mutex>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include <xxhash.h>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

/// sotc: SOTC_PACKET (ARCH-DRAW.md part 4). A pipeline with a stage that reads the DrawPacket (a shader-addressed
/// buffer, or the heap stage; Pipeline::packet_range) has its push range extended: PushData at offset 0, then the
/// packet at PacketPushOffset. The offset is sizeof(PushData) because such a pipeline has no bindless slot words.
/// 56 + 72 = 128 bytes, the push constant budget Vulkan guarantees.
inline constexpr u32 PacketPushOffset = sizeof(Shader::PushData);
inline constexpr u32 PacketPushBytes = PacketPushOffset + sizeof(SotcArch::DrawPacket);
static_assert(PacketPushBytes <= 128, "the packet push range must fit the guaranteed 128 bytes");

/// sotc: shares descriptor set layouts and pipeline layouts between the pipelines that need the
/// same ones (many pipelines differ only in their shaders). Owned by PipelineCache, so it outlives
/// every pipeline. Off by default (SOTC_LAYOUT_CACHE=1 or SOTC_STAGE_BINDINGS=1 turn it on).
class LayoutCache {
public:
    /// One descriptor set of a pipeline layout. Its bindings are copied only when created.
    /// The immutable samplers of the bindings are not part of the key (none are used).
    struct SetRequest {
        std::span<const vk::DescriptorSetLayoutBinding> bindings;
        vk::DescriptorSetLayoutCreateFlags flags{};
        /// sotc: SOTC_NB_GPL: a VK_NULL_HANDLE set (allowed in a layout with independent sets): the part of a
        /// pipeline library that does not use this set.
        bool null_set{};
    };

    /// What makes a pipeline layout: its sets in order (set 0 first), then its push constants.
    struct Request {
        std::span<const SetRequest> sets;
        vk::PushConstantRange push_constants{};
        /// sotc: an existing set layout placed after `sets` (the bindless sampler set: set 1, or
        /// Shader::BindlessDescriptorSet with a set per stage). Null for none.
        vk::DescriptorSetLayout trailing_set{};
        /// sotc: a second push constant range (the bindless sampler slots, after PushData), used
        /// when its size is not zero.
        vk::PushConstantRange slot_constants{};
        /// sotc: SOTC_NB_GPL: VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT for the per-shader libraries.
        vk::PipelineLayoutCreateFlags layout_flags{};
    };

    // sotc: the stage sets plus the trailing bindless set (Shader::BindlessDescriptorSet).
    using SetLayouts = boost::container::static_vector<vk::DescriptorSetLayout,
                                                       Shader::MaxStageDescriptorSets + 1>;

    /// Non-owning handles, valid while the cache lives. `created` tells whether this call made
    /// the pipeline layout (the caller names it then).
    struct Layouts {
        SetLayouts sets;
        vk::PipelineLayout pipeline_layout;
        bool created{};
    };

    /// Layouts a pipeline owns itself, used only when the cache is off.
    struct Owned {
        std::vector<vk::UniqueDescriptorSetLayout> sets;
        vk::UniquePipelineLayout pipeline_layout;
    };

    explicit LayoutCache(const Instance& instance);
    ~LayoutCache();

    LayoutCache(const LayoutCache&) = delete;
    LayoutCache& operator=(const LayoutCache&) = delete;

    /// Returns the layouts for the request, creating them unless an equal request was seen. With
    /// the cache off, they are created into `owned` instead. Thread-safe.
    Layouts Get(const Request& request, Owned& owned);

    /// sotc: true when layouts are shared and kept for the cache's lifetime (SOTC_BIND_DESC_REUSE persistent sets).
    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

private:
    /// Keys are compared in full (the hash only picks the bucket).
    using Key = std::vector<u64>;
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return XXH3_64bits(key.data(), key.size() * sizeof(u64));
        }
    };

    static Key MakeSetKey(const SetRequest& set);
    vk::UniqueDescriptorSetLayout CreateSetLayout(const SetRequest& set);
    vk::UniquePipelineLayout CreatePipelineLayout(std::span<const vk::DescriptorSetLayout> sets,
                                                  std::span<const vk::PushConstantRange> push,
                                                  vk::PipelineLayoutCreateFlags flags = {});
    vk::DescriptorSetLayout FindSetLayout(const SetRequest& set);

    const Instance& instance;
    bool enabled{};
    std::mutex mutex;
    tsl::robin_map<Key, vk::UniqueDescriptorSetLayout, KeyHash> set_entries;
    tsl::robin_map<Key, vk::UniquePipelineLayout, KeyHash> pipeline_entries;
    u64 num_created{};
    u64 num_reused{};
};

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        // sotc: SOTC_COMPUTE_FAST: a compute pipeline's handle is swapped (fast build, then optimized) on a worker while
        // other threads bind it, so compute pipelines read the atomic copy (ComputePipeline::Install keeps it).
        return is_compute ? vk::Pipeline{compute_handle.load(std::memory_order_acquire)} : *pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    /// sotc: true for graphics pipelines bound with one descriptor set per stage
    /// (SOTC_STAGE_BINDINGS): their writes name their set, see BindResources.
    bool UsesStageSets() const noexcept {
        return stage_sets;
    }

    /// sotc: the set index of the global bindless set in this pipeline's layout (Shader::BindlessDescriptorSet, the
    /// index its SPIR-V names): 1 for a single-set layout (compute included), the set after the stage sets otherwise.
    [[nodiscard]] u32 BindlessSetIndex() const noexcept {
        return Shader::BindlessDescriptorSet(stage_sets);
    }

    /// sotc: true when the layout has the packet push range (PacketPushBytes): the shaders read the
    /// DrawPacket, so BindResources needs `packet` (see below).
    /// sotc: the profile this pipeline was built with (the rasterizer asks for the heap stage and the shader
    /// address flags of the pipeline it binds).
    const Shader::Profile& GetProfile() const noexcept {
        return profile;
    }
    bool HasPacketRange() const noexcept {
        return packet_range;
    }
    /// sotc: SOTC_N5_BINDLESS / SOTC_BINDLESS_SAMP: true when some stage reads its slot words from the push constants
    /// (BindlessSamplerSlots). Only such a pipeline gets them filled and pushed; never with the packet range.
    bool HasSlotWords() const noexcept {
        return uses_slot_words;
    }
    /// sotc: SOTC_BIND_DESC_REUSE: true when every flat buffer binding of the layout is a dynamic storage buffer (also
    /// true without flat buffers). Then a bind-cache hit whose flat copies moved inside the same buffer can bind its
    /// kept sets with new dynamic offsets; otherwise the offsets are in the sets and a moved copy needs a write.
    [[nodiscard]] bool FlatDynamic() const noexcept {
        return dyn_flat.size() == flat_bindings;
    }
    /// sotc: SOTC_DESCRIPTOR_HEAP: true when the pipeline was built with the heap flag and no layout. Its
    /// BindResources writes the descriptors into the heap rings and pushes the push block with vkCmdPushDataEXT.
    [[nodiscard]] bool UsesDescriptorHeap() const noexcept {
        return heap_mode;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    /// `write_sets[i]` is the descriptor set of `set_writes[i]`, used only with UsesStageSets().
    /// sotc: `packet` is required when HasPacketRange() and ignored otherwise. `heap_block` is the heap block of the
    /// draw (sotc_arch.h, HeapBlock): read when the profile has the heap (heap_images), zeros when null.
    /// sotc: SOTC_BIND_DESC_REUSE: `reuse` (vk_resource_pool.h), when given, carries the committed sets. In with
    /// reuse->reuse: the sets are bound as they are and nothing is written (the caller checked they hold these
    /// descriptors). Otherwise the sets committed and written here are stored in it (count 0 when this bind path
    /// keeps no sets: heap pipelines, push descriptors, descriptor buffers, no writes).
    void BindResources(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                       const Shader::PushData& push_data,
                       const Shader::BindlessSamplerSlots& bindless_slots,
                       const SotcArch::DrawPacket* packet = nullptr,
                       const SotcArch::HeapBlock* heap_block = nullptr,
                       DescriptorSetReuse* reuse = nullptr) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    /// sotc: SOTC_BIND_DESC_REUSE. Called by the layout builders with the flat buffer bindings of one set (`flat`:
    /// indices into `bindings`) once the set is known to be a classic pool set (no push descriptors, descriptor buffer
    /// or heap). With the switch, they become dynamic storage buffers when there are at most `cap` of them (the
    /// device's dynamic storage buffer limit shared by the sets a layout can have), else the set keeps them static
    /// (counted). The decision depends on the stage's shader alone, so a per-shader GPL library and every pipeline
    /// that uses it agree on the set layout. ResetDynamicFlat first; FinishDynamicFlat after the last set.
    void ResetDynamicFlat() noexcept {
        dyn_flat.clear();
        flat_bindings = 0;
    }
    void MarkDynamicFlat(std::span<vk::DescriptorSetLayoutBinding> bindings, std::span<const u32> flat, u32 set,
                         u32 cap);
    void FinishDynamicFlat();

    /// sotc: SOTC_BIND_DESC_REUSE. The dynamic offsets of a bind, in dyn_flat order: each is the offset of the buffer
    /// info of the write to that (set, binding) (0 when the bind has no such write). With `patch`, those writes are
    /// changed to dynamic storage buffer writes at offset 0 (the offset moves to the dynamic offset) until
    /// RestoreDynamicWrites puts them back.
    static constexpr u32 MaxDynamicFlat = 16;
    struct DynamicPatch {
        std::array<vk::DescriptorBufferInfo, MaxDynamicFlat> infos{};
        std::array<const vk::DescriptorBufferInfo*, MaxDynamicFlat> saved{};
        std::array<u32, MaxDynamicFlat> write{};
        u32 count{};
    };
    u32 PrepareDynamicFlat(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                           std::array<u32, MaxDynamicFlat>& offsets, DynamicPatch* patch) const;
    static void RestoreDynamicWrites(DescriptorWrites& set_writes, const DynamicPatch& patch);

    /// sotc: SOTC_DESCRIPTOR_HEAP: plans the mapping of `bindings` (the layout's set 0, in order) when the mode is on
    /// and `pre` (the reason the caller found, Heap when none) allows it. Counts the outcome. True when the pipeline is
    /// a heap pipeline: its layout must then not be created (heap_plan holds the mappings).
    bool PlanHeapMode(std::span<const HeapBinding> bindings, HeapFallback pre);
    /// sotc: SOTC_DESCRIPTOR_HEAP draw: writes the set's descriptors into the rings, binds both heaps, and pushes the
    /// push block (PushData, then the two ring base slots) with vkCmdPushDataEXT.
    void BindHeapResources(const Shader::PushData& push_data, const DescriptorWrites& set_writes) const;

    /// sotc: SOTC_STAGE_BINDINGS: commits one descriptor set per stage layout, writes each
    /// stage's descriptors into its own set and binds them all together.
    /// sotc: SOTC_BIND_DESC_REUSE: reuse_in (null without a request) holds the sets to bind unwritten; reuse_out (may
    /// be null) receives the sets this call bound, fresh or reused.
    void BindStageSets(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                       vk::PipelineBindPoint bind_point, const DescriptorSetReuse* reuse_in,
                       DescriptorSetReuse* reuse_out) const;

    /// sotc: with descriptor buffers (SOTC_DESC_BUFFER=1) the set layout is queried once after its
    /// creation: the size of a set and the offset of each binding number inside it.
    void QueryDescriptorBufferLayout(std::span<const vk::DescriptorSetLayoutBinding> bindings);

    /// sotc: writes the set into the descriptor buffer ring and binds it at its offset.
    void BindDescriptorBuffer(const DescriptorWrites& set_writes,
                              vk::PipelineBindPoint bind_point) const;

    /// sotc: every vkCmdBindDescriptorSets of a pipeline goes through here. Guard: a bind of sets [first, first +
    /// count) must pass one dynamic offset per dynamic descriptor of those sets (the dyn_flat entries in the range).
    /// Another count is invalid (VUID-vkCmdBindDescriptorSets-dynamicOffsetCount-00359) and NVIDIA reads a null
    /// pDynamicOffsets (the recorder-thread crash of 9 Nov). Such a bind is counted, logged (first, then at powers of
    /// two), and recorded with zero offsets of the right count instead.
    void BindSets(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point, u32 first, u32 count,
                  const vk::DescriptorSet* sets, u32 num_dyn, const u32* offsets) const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    /// sotc: SOTC_COMPUTE_FAST: the handle compute pipelines bind (the one `pipeline` owns, stored after it).
    std::atomic<VkPipeline> compute_handle{};
    LayoutCache::Owned owned_layouts; ///< sotc: only when the layout cache is off
    vk::PipelineLayout pipeline_layout; ///< sotc: from LayoutCache, not owned here
    /// sotc: SOTC_NB_GPL: the layouts of the per-shader libraries (only the vertex set / only the fragment set), null
    /// when the pipeline's libraries use the full layout.
    vk::PipelineLayout gpl_vs_layout{};
    vk::PipelineLayout gpl_fs_layout{};
    LayoutCache::Owned owned_gpl_layouts[2]; ///< sotc: only when the layout cache is off
    vk::DescriptorSetLayout desc_layout; ///< sotc: set 0's layout, from LayoutCache
    LayoutCache::SetLayouts set_layouts; ///< sotc: all sets of the layout, in set order
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    /// sotc: SOTC_BINDLESS_SAMP: some stage reads its samplers from the global heap, so the layout
    /// has the heap set (BindlessSetIndex) and BindResources binds it.
    bool uses_bindless_samplers{};
    /// sotc: SOTC_HEAP: some stage reads bindless slot words from the push constants (BindlessSamplerSlots). A heap
    /// stage reads its indices from the index table instead, so it needs the heap set but no slot words.
    bool uses_slot_words{};
    bool stage_sets{}; ///< sotc: a graphics pipeline with one descriptor set per stage
    /// sotc: with stage_sets, the stage sets BindStageSets commits and binds (sets 0..n-1). The layout may hold more:
    /// empty padding sets up to the bindless set and the bindless set itself (BindlessSetIndex), bound apart.
    u32 num_stage_sets{};
    /// sotc: SOTC_DESCRIPTOR_HEAP: the pipeline is a heap pipeline (see UsesDescriptorHeap) and its mapping plan.
    bool heap_mode{};
    DescriptorHeapPlan heap_plan{};
    /// sotc: SOTC_PACKET: the push range carries the DrawPacket (PacketPushBytes). Set when the layout is built: true
    /// when a stage has a shader-addressed buffer (UsesShaderAddr) or is the heap stage. Never true together with
    /// uses_slot_words (the packet and the slot words share bytes 56..128).
    bool packet_range{};
    /// sotc: SOTC_BIND_DESC_REUSE: the dynamic flat buffer bindings, (set, binding) in the order of their dynamic
    /// offsets (set, then binding), and the count of flat buffer bindings in the layout (dynamic or not).
    struct DynamicBinding {
        u32 set;
        u32 binding;
    };
    boost::container::static_vector<DynamicBinding, MaxDynamicFlat> dyn_flat;
    u32 flat_bindings{};
    vk::DeviceSize desc_layout_size{};             ///< sotc: descriptor buffer size of one set
    std::vector<vk::DeviceSize> desc_binding_offsets; ///< sotc: by binding number, in the set
public:
    // sotc: SOTC_GCN_COMMIT: Commit::Eligible cached, -1 unknown
    mutable std::atomic<s8> sotc_gcn_commit{-1};
    // sotc: SOTC_GCN_SKIP: the pipeline part of Commit::SkipEligible cached (a SkipReason), -1 unknown
    mutable std::atomic<s8> sotc_gcn_skip{-1};
};

} // namespace Vulkan
