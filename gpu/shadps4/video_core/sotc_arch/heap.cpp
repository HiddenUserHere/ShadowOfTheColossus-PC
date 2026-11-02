// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_HEAP (ARCH-DRAW.md part 3). The shader-side view of the N5 bindless arrays: image views in the
// per-key arrays of set 1 (binding 1 + key, vk_bindless.h) and samplers in the bindless sampler array
// (texture_cache/sampler.h). The heap owns no descriptors: BindlessImages and BindlessSamplers do, and their
// slots are released only after the GPU passed the tick of the release (BindlessImages::Reclaim), so a slot a
// submitted frame can reference is never reused. VK_EXT_descriptor_heap is not used (runtime-array form only).

#include <atomic>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"

namespace SotcArch::Heap {

bool On() {
    static const bool on = SwitchMode("SOTC_HEAP") != 0;
    return on;
}

bool UsesDescriptorHeap() {
    // VK_EXT_descriptor_heap is left for later: the emitted SPIR-V is the runtime-array form only.
    return false;
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
    if (VideoCore::BindlessImagesMode() == 0) {
        LOG_WARNING(Render_Vulkan, "SOTC_HEAP=1 needs SOTC_NATIVE=1 and SOTC_N5_BINDLESS=1: image indices are null");
    }
    if (!instance.IsBindlessSamplersEnabled()) {
        LOG_WARNING(Render_Vulkan, "SOTC_HEAP=1 needs SOTC_BINDLESS_SAMP=1: sampler indices are null");
    }
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
