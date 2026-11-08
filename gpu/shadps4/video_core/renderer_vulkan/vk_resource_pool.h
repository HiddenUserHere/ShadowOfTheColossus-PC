// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <span>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Semaphore;
class Scheduler;
class DescriptorBufferRing;
class DescriptorHeapBuffers; // sotc: SOTC_DESCRIPTOR_HEAP=1 only (vk_descriptor_heap.h)

/**
 * Handles a pool of resources protected by fences. Manages resource overflow allocating more
 * resources.
 */
class ResourcePool {
public:
    explicit ResourcePool() = default;
    explicit ResourcePool(Semaphore* work_semaphore, std::size_t grow_step);
    virtual ~ResourcePool() = default;

    ResourcePool& operator=(ResourcePool&&) noexcept = default;
    ResourcePool(ResourcePool&&) noexcept = default;

    ResourcePool& operator=(const ResourcePool&) = default;
    ResourcePool(const ResourcePool&) = default;

protected:
    std::size_t CommitResource();

    /// Called when a chunk of resources have to be allocated.
    virtual void Allocate(std::size_t begin, std::size_t end) = 0;

private:
    /// Manages pool overflow allocating new resources.
    std::size_t ManageOverflow();

protected:
    Semaphore* work_semaphore{};
    std::size_t grow_step = 0;     ///< Number of new resources created after an overflow
    std::size_t hint_iterator = 0; ///< Hint to where the next free resources is likely to be found
    std::vector<u64> ticks;        ///< Ticks for each resource
};

class CommandPool final : public ResourcePool {
public:
    /// sotc: level is ePrimary for the command processor's pool, eSecondary for the pass secondaries
    explicit CommandPool(const Instance& instance, Semaphore* work_semaphore,
                         vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);
    ~CommandPool() override;

    void Allocate(std::size_t begin, std::size_t end) override;

    vk::CommandBuffer Commit();

    /// sotc: keeps cmd out of Commit until Release gives it a tick. Not thread safe: the caller locks.
    void Hold(vk::CommandBuffer cmd);
    /// sotc: cmd may be committed again once the GPU reaches tick. Not thread safe: the caller locks.
    void Release(vk::CommandBuffer cmd, u64 tick);

private:
    [[nodiscard]] std::size_t IndexOf(vk::CommandBuffer cmd) const;

    const Instance& instance;
    vk::CommandBufferLevel level;
    vk::UniqueCommandPool cmd_pool;
    std::vector<vk::CommandBuffer> cmd_buffers;
};

/// sotc: SOTC_BIND_DESC_REUSE (gpu/sotc/nonblocking.h). The descriptor sets a Pipeline::BindResources committed and
/// wrote, kept by the frame bind-cache entry of that bind so a later hit binds them without writing. Valid while the
/// heap's pool serial is unchanged (DescriptorHeap::PoolSerial: a roll resets or replaces the pool the sets came from)
/// and the descriptors a hit would write are the entry's own (checked by the rasterizer).
/// sotc: with persistent sets (DescriptorHeap::PersistentSets) the sets come from the never-rolled persistent pools
/// instead: the entry owns them until DescriptorHeap::RetirePersistent hands them back after `last_tick`, and the
/// pool serial is not checked.
struct DescriptorSetReuse {
    static constexpr u32 MaxSets = 5; ///< Shader::MaxStageDescriptorSets
    std::array<vk::DescriptorSet, MaxSets> sets{};
    std::array<vk::DescriptorSetLayout, MaxSets> layouts{}; ///< sotc: the layout of each set (persistent sets)
    u32 count{};          ///< sets kept (0: the bind path keeps none)
    u64 pool_serial{};    ///< DescriptorHeap::PoolSerial() when the sets were committed
    u64 last_tick{};      ///< sotc: the scheduler tick of the last command buffer that bound the sets
    bool reuse{};         ///< in: bind `sets` as they are, write nothing; out: false when the path could not
    bool keep{};          ///< sotc: in: a bind-cache entry will own the sets (persistent sets may be taken)
    bool persistent{};    ///< sotc: out: the sets are persistent (RetirePersistent frees them)
};

/// sotc: SOTC_BIND_DESC_REUSE persistent set counters (DescriptorHeap::TakePersistentStats).
struct PersistentSetStats {
    u64 alive{};     ///< sets owned by bind-cache entries now
    u64 retiring{};  ///< sets waiting for their last tick
    u64 free{};      ///< sets in the per-layout free lists
    u64 allocated{}; ///< sets allocated from the pools (since the last take)
    u64 recycled{};  ///< sets taken from a free list (since the last take)
    u64 freed{};     ///< sets back in a free list after their last tick (since the last take)
    u64 pools{};     ///< persistent pools
    u64 bytes{};     ///< estimate of the pools' descriptor memory
};

class DescriptorHeap final {
    static constexpr u32 DescriptorSetBatch = 32;

public:
    explicit DescriptorHeap(const Instance& instance, Scheduler& scheduler,
                            std::span<const vk::DescriptorPoolSize> pool_sizes,
                            u32 descriptor_heap_count = 1024);
    ~DescriptorHeap();

    vk::DescriptorSet Commit(vk::DescriptorSetLayout set_layout);

    /// sotc: SOTC_BIND_DESC_REUSE. Serial of the pool Commit allocates from, bumped at every roll (reset or new pool).
    /// A set committed under the current serial stays as written until this pool is retired, and the pool is retired
    /// with the tick being recorded when it fills, so a set reused at any tick before that is covered by its wait.
    [[nodiscard]] u64 PoolSerial() const noexcept {
        return pool_serial;
    }

    /// sotc: SOTC_BIND_DESC_REUSE with the layout cache on (stable set layout handles): the sets a bind-cache entry
    /// keeps come from persistent pools that are never reset. Set once by PipelineCache before any bind.
    void EnablePersistentSets(bool on) noexcept {
        persistent_on = on;
    }
    [[nodiscard]] bool PersistentSets() const noexcept {
        return persistent_on;
    }
    /// sotc: a persistent set of `set_layout`: a free one whose last tick the GPU finished, else a new one.
    vk::DescriptorSet AcquirePersistent(vk::DescriptorSetLayout set_layout);
    /// sotc: hands the record's persistent sets back; they are reused once the GPU finished record.last_tick.
    /// Nothing happens for a record without persistent sets.
    void RetirePersistent(const DescriptorSetReuse& record);
    /// sotc: the persistent set counters; the per-interval ones are reset.
    PersistentSetStats TakePersistentStats();

    /// sotc: the descriptor buffer ring when SOTC_DESC_BUFFER=1 (vk_descriptor_buffer.h), else null.
    [[nodiscard]] DescriptorBufferRing* GetDescriptorBuffer() const noexcept {
        return descriptor_buffer.get();
    }

    /// sotc: SOTC_BINDLESS_SAMP: the global sampler heap (set 1, texture_cache/sampler.h). Set once
    /// by the rasterizer; the pipelines that use bindless samplers bind it.
    void SetBindlessSamplers(vk::DescriptorSetLayout layout, vk::DescriptorSet set) noexcept {
        bindless_layout = layout;
        bindless_set = set;
    }
    /// sotc: SOTC_DESCRIPTOR_HEAP=1: the resource and sampler heaps (vk_descriptor_heap.h), else null.
    [[nodiscard]] DescriptorHeapBuffers* GetDescriptorHeapBuffers() const noexcept {
        return descriptor_heap_buffers.get();
    }
    [[nodiscard]] vk::DescriptorSetLayout BindlessSamplerLayout() const noexcept {
        return bindless_layout;
    }
    [[nodiscard]] vk::DescriptorSet BindlessSamplerSet() const noexcept {
        return bindless_set;
    }

private:
    void CreateDescriptorPool();
    void CreatePersistentPool(); // sotc: SOTC_BIND_DESC_REUSE
    void CollectPersistent();    // sotc: retired sets whose tick is free go back to their free list

private:
    vk::Device device;
    Semaphore* work_semaphore;
    u32 descriptor_heap_count;
    std::span<const vk::DescriptorPoolSize> pool_sizes;
    vk::DescriptorPool curr_pool;
    u64 pool_serial{1}; ///< sotc: SOTC_BIND_DESC_REUSE (PoolSerial)
    std::vector<vk::DescriptorPoolSize> scaled_sizes; ///< sotc: SOTC_BIND_DESC_REUSE: pool_sizes x8 (CreateDescriptorPool)
    std::deque<std::pair<vk::DescriptorPool, u64>> pending_pools;
    using DescSetBatch = boost::container::static_vector<vk::DescriptorSet, DescriptorSetBatch>;
    tsl::robin_map<u64, DescSetBatch> descriptor_sets;
    std::unique_ptr<DescriptorBufferRing> descriptor_buffer; // sotc: SOTC_DESC_BUFFER=1 only
    std::unique_ptr<DescriptorHeapBuffers> descriptor_heap_buffers; // sotc: SOTC_DESCRIPTOR_HEAP=1 only
    vk::DescriptorSetLayout bindless_layout{}; // sotc: SOTC_BINDLESS_SAMP, null otherwise
    vk::DescriptorSet bindless_set{};
    // sotc: SOTC_BIND_DESC_REUSE persistent sets: pools never reset (destroyed with the heap), per-layout free lists,
    // and the retired sets with the tick after which they are free.
    struct RetiredSet {
        u64 tick;
        vk::DescriptorSetLayout layout;
        vk::DescriptorSet set;
    };
    bool persistent_on{};
    std::vector<vk::DescriptorPool> persistent_pools;
    tsl::robin_map<u64, std::vector<vk::DescriptorSet>> persistent_free;
    std::deque<RetiredSet> persistent_retired;
    std::vector<vk::DescriptorPoolSize> persistent_sizes;
    u64 persistent_alive{}, persistent_free_count{}, persistent_allocated{}, persistent_recycled{},
        persistent_freed{};
};

} // namespace Vulkan
