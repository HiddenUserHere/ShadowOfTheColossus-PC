// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_descriptor_heap.h" // sotc: SOTC_DESCRIPTOR_HEAP (heap plan of a pipeline)
#include "video_core/sotc_arch/sotc_arch.h"

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
    };

    /// What makes a pipeline layout: its sets in order (set 0 first), then its push constants.
    struct Request {
        std::span<const SetRequest> sets;
        vk::PushConstantRange push_constants{};
        /// sotc: an existing set layout placed after `sets` (the bindless sampler set, set 1).
        /// Null for none.
        vk::DescriptorSetLayout trailing_set{};
        /// sotc: a second push constant range (the bindless sampler slots, after PushData), used
        /// when its size is not zero.
        vk::PushConstantRange slot_constants{};
    };

    using SetLayouts = boost::container::static_vector<vk::DescriptorSetLayout,
                                                       Shader::MaxStageDescriptorSets>;

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
                                                  std::span<const vk::PushConstantRange> push);
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
        return *pipeline;
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
    /// sotc: SOTC_DESCRIPTOR_HEAP: true when the pipeline was built with the heap flag and no layout. Its
    /// BindResources writes the descriptors into the heap rings and pushes the push block with vkCmdPushDataEXT.
    [[nodiscard]] bool UsesDescriptorHeap() const noexcept {
        return heap_mode;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    /// `write_sets[i]` is the descriptor set of `set_writes[i]`, used only with UsesStageSets().
    /// sotc: `packet` is required when HasPacketRange() and ignored otherwise. `heap_block` is the heap block of the
    /// draw (sotc_arch.h, HeapBlock): read when the profile has the heap (heap_images), zeros when null.
    void BindResources(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                       const Shader::PushData& push_data,
                       const Shader::BindlessSamplerSlots& bindless_slots,
                       const SotcArch::DrawPacket* packet = nullptr,
                       const SotcArch::HeapBlock* heap_block = nullptr) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    /// sotc: SOTC_DESCRIPTOR_HEAP: plans the mapping of `bindings` (the layout's set 0, in order) when the mode is on
    /// and `pre` (the reason the caller found, Heap when none) allows it. Counts the outcome. True when the pipeline is
    /// a heap pipeline: its layout must then not be created (heap_plan holds the mappings).
    bool PlanHeapMode(std::span<const HeapBinding> bindings, HeapFallback pre);
    /// sotc: SOTC_DESCRIPTOR_HEAP draw: writes the set's descriptors into the rings, binds both heaps, and pushes the
    /// push block (PushData, then the two ring base slots) with vkCmdPushDataEXT.
    void BindHeapResources(const Shader::PushData& push_data, const DescriptorWrites& set_writes) const;

    /// sotc: SOTC_STAGE_BINDINGS: commits one descriptor set per stage layout, writes each
    /// stage's descriptors into its own set and binds them all together.
    void BindStageSets(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                       vk::PipelineBindPoint bind_point) const;

    /// sotc: with descriptor buffers (SOTC_DESC_BUFFER=1) the set layout is queried once after its
    /// creation: the size of a set and the offset of each binding number inside it.
    void QueryDescriptorBufferLayout(std::span<const vk::DescriptorSetLayoutBinding> bindings);

    /// sotc: writes the set into the descriptor buffer ring and binds it at its offset.
    void BindDescriptorBuffer(const DescriptorWrites& set_writes,
                              vk::PipelineBindPoint bind_point) const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    LayoutCache::Owned owned_layouts; ///< sotc: only when the layout cache is off
    vk::PipelineLayout pipeline_layout; ///< sotc: from LayoutCache, not owned here
    vk::DescriptorSetLayout desc_layout; ///< sotc: set 0's layout, from LayoutCache
    LayoutCache::SetLayouts set_layouts; ///< sotc: all sets of the layout, in set order
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    /// sotc: SOTC_BINDLESS_SAMP: some stage reads its samplers from the global heap, so the layout
    /// has set 1 (the heap) and BindResources binds it.
    bool uses_bindless_samplers{};
    /// sotc: SOTC_HEAP: some stage reads bindless slot words from the push constants (BindlessSamplerSlots). A heap
    /// stage reads its indices from the index table instead, so it needs the heap set but no slot words.
    bool uses_slot_words{};
    bool stage_sets{}; ///< sotc: a graphics pipeline with one descriptor set per stage
    /// sotc: SOTC_DESCRIPTOR_HEAP: the pipeline is a heap pipeline (see UsesDescriptorHeap) and its mapping plan.
    bool heap_mode{};
    DescriptorHeapPlan heap_plan{};
    /// sotc: SOTC_PACKET: the push range carries the DrawPacket (PacketPushBytes). Set when the layout is built: true
    /// when a stage has a shader-addressed buffer (UsesShaderAddr) or is the heap stage. Never true together with
    /// uses_slot_words (the packet and the slot words share bytes 56..128).
    bool packet_range{};
    vk::DeviceSize desc_layout_size{};             ///< sotc: descriptor buffer size of one set
    std::vector<vk::DeviceSize> desc_binding_offsets; ///< sotc: by binding number, in the set
};

} // namespace Vulkan
