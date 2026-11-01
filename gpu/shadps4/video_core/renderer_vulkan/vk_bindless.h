// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
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

/// sotc: layout bindings of the heap arrays (one per key), for the bindless set layout.
std::array<vk::DescriptorSetLayoutBinding, Shader::NUM_BINDLESS_IMAGE_KEYS>
BindlessImageLayoutBindings();

/// sotc: SOTC_N5_BINDLESS: the sampled image views of the bindless set. A view takes a slot of
/// a heap array the first time a bindless stage binds it for that key (ImageView::bindless_slots),
/// keeps it while the view lives, and gives it back when the view dies. A released slot is
/// cleared and reused only after the GPU passed the tick of its release (frame ticket), so no
/// submitted draw can read a slot that changes under it.
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

    /// Slot of `view` in the array of `key`, written with `layout`: the descriptor fixes the layout
    /// at write time, so a view bound in another layout takes a new slot (the caller releases the
    /// old one). Writes the heap on each Acquire, never per draw for a view that keeps its slot.
    /// Returns the null slot of the key when the heap is full. Called with the texture cache lock.
    [[nodiscard]] u32 Acquire(u32 key, vk::ImageView view, vk::ImageLayout layout);

    /// Gives `slot` of `key` back. Slot 0 (null) and NO_BINDLESS_IMAGE_SLOT are never released. The
    /// slot is cleared after the frame ticket (see above).
    void Release(u32 key, u32 slot);

    /// The null slot of every key: holds no view, reads as zero (nullDescriptor).
    [[nodiscard]] static constexpr u32 NullSlot(u32 /*key*/) noexcept {
        return 0;
    }

    /// Verify mode: the view the descriptor path would bind at `slot` (`expected`) against the
    /// mirror of the heap. Counts a mismatch, returns false on one.
    bool Verify(u32 key, u32 slot, vk::ImageView expected);

    /// One line for the 5 s report (counters are reset on read):
    /// "SOTC N5 BINDLESS acquire N writes N releases N reclaims N full N verify N mismatch N pending N".
    /// Returns the length written.
    u32 Report(char* out, u32 size);

private:
    void Reclaim();
    void WriteSlot(u32 key, u32 slot, vk::ImageView view, vk::ImageLayout layout);

    struct Pending {
        u32 key;
        u32 slot;
        u64 tick;
    };

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    vk::DescriptorSet set;
    u32 mode;

    std::mutex mutex;
    // Mirror of the heap: the view written at each slot (null handle when cleared).
    std::array<std::vector<vk::ImageView>, Shader::NUM_BINDLESS_IMAGE_KEYS> mirror{};
    std::array<std::vector<u32>, Shader::NUM_BINDLESS_IMAGE_KEYS> free_slots{};
    std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> next_slot{1, 1, 1, 1, 1, 1};
    std::deque<Pending> pending;
    bool logged_full{};

    u64 acquires{};
    u64 writes{};
    u64 releases{};
    u64 reclaims{};
    u64 full{};
    u64 verifies{};
    u64 mismatches{};
};

} // namespace VideoCore
