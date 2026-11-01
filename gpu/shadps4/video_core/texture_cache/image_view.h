// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
struct ColorBuffer;
}

namespace Shader {
struct ImageResource;
}

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

struct ImageViewInfo {
    ImageViewInfo() = default;
    ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept;
    ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept;
    ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl);

    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    SubresourceRange range;
    vk::ComponentMapping mapping{};
    u32 min_lod = 0;
    bool is_storage = false;

    auto operator<=>(const ImageViewInfo&) const = default;
};

struct Image;

struct ImageView {
    ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info, const Image& image);
    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    // sotc: moves hand the heap slots over: the moved-from view releases nothing.
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;

    ImageViewInfo info;
    vk::UniqueImageView image_view;

    /// sotc: SOTC_N5_BINDLESS: the heap slot of this view per heap key (NO_BINDLESS_IMAGE_SLOT
    /// when none), and the image layout that slot was written with (BindlessImages::Acquire).
    std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> bindless_slots = NoBindlessSlots();
    std::array<vk::ImageLayout, Shader::NUM_BINDLESS_IMAGE_KEYS> bindless_layouts{};

    /// Gives every heap slot of this view back to the live heap (if any).
    void ReleaseBindless() noexcept;

private:
    static std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> NoBindlessSlots() {
        std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> slots{};
        slots.fill(Shader::NO_BINDLESS_IMAGE_SLOT);
        return slots;
    }
};

} // namespace VideoCore
