// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_HEAP (ARCH-DRAW.md part 3). The shader-side view of the N5 bindless arrays: image views in the
// per-key arrays of set 1 (binding 1 + key, vk_bindless.h) and samplers in the bindless sampler array
// (texture_cache/sampler.h). The heap owns no descriptors: BindlessImages and BindlessSamplers do, and their
// slots are released only after the GPU passed the tick of the release (BindlessImages::Reclaim), so a slot a
// submitted frame can reference is never reused. VK_EXT_descriptor_heap is not used (runtime-array form only).

#include <atomic>
#include <cstdio>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"

namespace SotcArch::Heap {
namespace {
// Set by CheckRequirements when a requirement is missing on this device (process lifetime).
std::atomic<bool> g_refused{false};
} // namespace

bool On() {
    // The environment part is read once; the device part is final before any pipeline is built (the instance).
    static const bool env_on = SwitchMode("SOTC_HEAP") != 0 && SwitchMode("SOTC_N5_BINDLESS") != 0 &&
                               SwitchMode("SOTC_BINDLESS_SAMP") != 0 &&
                               SwitchMode("SOTC_LAYOUT_UNIFIED") != 0;
    return env_on && !g_refused.load(std::memory_order_relaxed);
}

bool UsesDescriptorHeap() {
    // VK_EXT_descriptor_heap is left for later: the emitted SPIR-V is the runtime-array form only.
    return false;
}

void CheckRequirements(bool bindless_images, bool bindless_samplers) {
    if (SwitchMode("SOTC_HEAP") == 0) {
        return;
    }
    const bool n5 = SwitchMode("SOTC_N5_BINDLESS") != 0;
    const bool samp = SwitchMode("SOTC_BINDLESS_SAMP") != 0;
    const bool layout = SwitchMode("SOTC_LAYOUT_UNIFIED") != 0;
    if (n5 && samp && layout && bindless_images && bindless_samplers) {
        return;
    }
    g_refused.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "SOTC HEAP refused: needs SOTC_N5_BINDLESS=1 (%s), SOTC_BINDLESS_SAMP=1 (%s), "
                 "SOTC_LAYOUT_UNIFIED=1 (%s) and a device with the bindless arrays (%s); the run uses the "
                 "descriptor path\n",
                 n5 && bindless_images ? "ok" : "missing", samp && bindless_samplers ? "ok" : "missing",
                 layout ? "ok" : "missing", bindless_images && bindless_samplers ? "ok" : "missing");
}

void Init(const Vulkan::Instance& instance) {
    if (!On()) {
        return;
    }
    // Once per process: the heap is process-global, and every TextureCache calls Init.
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    LOG_INFO(Render_Vulkan, "SOTC HEAP: VK_EXT_descriptor_heap not used (runtime-array form)");
    (void)instance;
}

u32 NullImageIndex(u32 key) {
    return VideoCore::BindlessImages::NullSlot(key);
}

u32 NullSamplerIndex() {
    return VideoCore::BindlessSamplers::DefaultSlot(false);
}

u32 IndexOf(VideoCore::ImageView& view, u32 key, vk::ImageLayout layout) {
    VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
    if (!heap || key >= Shader::NUM_BINDLESS_IMAGE_KEYS || !view.image_view) {
        return NullImageIndex(key);
    }
    // The same slot the rasterizer binds (its BindlessImageIndex): the view keeps it per key while the heap
    // still holds the view in this layout, and takes a new one otherwise.
    const vk::ImageView handle = *view.image_view;
    u32& slot = view.bindless_slots[key];
    if (slot == Shader::NO_BINDLESS_IMAGE_SLOT || !heap->Holds(key, slot, handle, layout)) {
        heap->Release(key, slot, handle);
        slot = heap->Acquire(key, handle, layout);
    }
    heap->Use(key, slot);
    if (heap->Mode() == 2 && slot != NullImageIndex(key)) {
        heap->Verify(key, slot, handle);
    }
    return slot;
}

u32 IndexOfSampler(VideoCore::Sampler& sampler) {
    // The sampler's slot in the bindless sampler array, given by the texture cache when the sampler was created
    // (TextureCache::GetSampler under SOTC_HEAP). The GC keeps samplers that hold a slot.
    if (sampler.bindless_slot == VideoCore::BindlessSamplers::NoSlot) {
        return NullSamplerIndex();
    }
    return sampler.bindless_slot;
}

} // namespace SotcArch::Heap
