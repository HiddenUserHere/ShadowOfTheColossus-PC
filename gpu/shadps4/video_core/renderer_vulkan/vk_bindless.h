// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

/// sotc: SOTC_N5_BINDLESS: 0 off, 1 on, 2 on and verify (the heap mirror is compared with the view
/// the descriptor path would bind). Read once. Off unless SOTC_NATIVE is on as well.
u32 BindlessImagesMode();

/// sotc: N5 bindless images: views per heap array. Slot 0 of every array is the null slot.
inline constexpr u32 BindlessImageCapacity = 4096;
/// sotc: the heap arrays are bindings 1 + key of the bindless set (set 1); binding 0 is the sampler
/// array of BindlessSamplers. The set layout (sampler.cpp) appends BindlessImageLayoutBindings().
inline constexpr u32 BindlessImageFirstBinding = 1;

/// sotc: descriptor type of the heap array of `key`: sampled for the sampled keys, storage for the
/// storage keys (Shader::BindlessImageKeyOf).
vk::DescriptorType BindlessImageDescriptorType(u32 key);

/// sotc: layout bindings of the heap arrays (one per key), for the bindless set layout.
std::array<vk::DescriptorSetLayoutBinding, Shader::NUM_BINDLESS_IMAGE_KEYS>
BindlessImageLayoutBindings();

/// sotc: one heap slot a bind uses: its array (key), its index (the push word value) and the view
/// the slot held when the bind used it. A stored bind (SOTC_BIND_CACHE, SOTC_TEX_BLOCK) keeps these
/// and replays them while BindlessImages::Generation is unchanged.
struct BindlessRef {
    u32 key{};
    u32 slot{};
    vk::ImageView view{};
};

/// sotc: SOTC_N5_BINDLESS: the image views of the bindless set. A view takes a slot of its key's
/// array when a bindless stage first binds it (ImageView::bindless_slots) and keeps it while it
/// holds it. A slot is given back when its view releases it (a layout change, or the view dies),
/// or taken away when the heap is full (the least recently used slot the GPU has finished with).
/// A released slot is cleared and reused only after the GPU passed the tick of its release, and a
/// slot is evicted only after the GPU passed the last tick that used it, so no submitted draw can
/// read a slot that changes under it.
class BindlessImages {
public:
    /// `set` is the bindless set (BindlessSamplers::Set()), with the heap arrays in its layout.
    BindlessImages(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                   vk::DescriptorSet set, u32 mode);
    ~BindlessImages();

    BindlessImages(const BindlessImages&) = delete;
    BindlessImages& operator=(const BindlessImages&) = delete;

    /// The live heap of the session, null when N5 bindless images are off. ImageView uses it to
    /// give its slots back.
    [[nodiscard]] static BindlessImages* Active() noexcept;

    [[nodiscard]] u32 Mode() const noexcept {
        return mode;
    }

    /// Bumped whenever a slot stops holding the view it held (a release, an eviction). A bind
    /// stored with the generation it was made at is replayed only while it is unchanged.
    [[nodiscard]] u64 Generation() const noexcept {
        return generation.load(std::memory_order_acquire);
    }

    /// Slot of `view` in the array of `key`, written with `layout`: the descriptor fixes the layout
    /// at write time, so a view bound in another layout takes a new slot (the caller releases the
    /// old one). When the heap is full, the least recently used slot the GPU has finished with is
    /// taken (counted as an eviction). Returns the null slot of the key when none is free.
    /// The new slot is marked used by the current tick. Called with the texture cache lock.
    [[nodiscard]] u32 Acquire(u32 key, vk::ImageView view, vk::ImageLayout layout);

    /// True when `slot` of `key` is live and holds `view` in `layout`.
    [[nodiscard]] bool Holds(u32 key, u32 slot, vk::ImageView view, vk::ImageLayout layout);

    /// Marks a live `slot` of `key` used by the current tick: it is not evicted before that tick
    /// has finished on the GPU. Every bind that names a slot calls it when it names it.
    void Use(u32 key, u32 slot);

    /// Gives `slot` of `key` back when it is live and holds `view` (otherwise a no-op: the view
    /// lost the slot to an eviction). Slot 0 (null) is never released. The slot is cleared after the
    /// frame ticket (see above).
    void Release(u32 key, u32 slot, vk::ImageView view);

    /// The null slot of every key: holds no view, reads as zero (nullDescriptor).
    [[nodiscard]] static constexpr u32 NullSlot(u32 /*key*/) noexcept {
        return 0;
    }

    /// Verify mode: the view the descriptor path would bind at `slot` (`expected`) against the
    /// mirror of the heap. Counts a mismatch, returns false on one.
    bool Verify(u32 key, u32 slot, vk::ImageView expected);

    /// One line for the 5 s report (counters are reset on read):
    /// "SOTC N5 BINDLESS acquire N writes N releases N reclaims N evict N full N verify N mismatch N
    /// pending N served N". `writes` are the descriptor writes the heap made; `served` counts the
    /// heap binds (fresh or replayed), each one a descriptor write the classic path would have made.
    /// Returns the length written.
    u32 Report(char* out, u32 size);

private:
    enum class SlotState : u8 {
        Free,    ///< no view; in the free list or never used
        Live,    ///< holds `view` for its owner
        Pending, ///< released: still holds the old view until its release tick is free
    };

    struct Slot {
        vk::ImageView view{};
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
        u64 last_use{}; ///< tick of the last bind that named the slot
        SlotState state{SlotState::Free};
    };

    struct ReleasedSlot {
        u32 key;
        u32 slot;
        u64 tick;
    };

    void Reclaim();
    u32 Evict(u32 key);
    void WriteSlot(u32 key, u32 slot, vk::ImageView view, vk::ImageLayout layout);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    vk::DescriptorSet set;
    u32 mode;

    std::mutex mutex;
    std::atomic<u64> generation{1};
    // Mirror of the heap, one entry per slot of each array (index 0 is the null slot).
    std::array<std::vector<Slot>, Shader::NUM_BINDLESS_IMAGE_KEYS> slots{};
    std::array<std::vector<u32>, Shader::NUM_BINDLESS_IMAGE_KEYS> free_slots{};
    std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> next_slot{};
    std::deque<ReleasedSlot> pending;
    bool logged_full{};

    u64 acquires{};
    u64 writes{};
    u64 releases{};
    u64 reclaims{};
    u64 evictions{};
    u64 full{};
    u64 verifies{};
    u64 mismatches{};
    u64 served{};
};

} // namespace VideoCore
