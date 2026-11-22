// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_renderer: orchestration. It owns the native sub-systems and
// drives one frame: per-pass secondary command buffers joined in pass order (the plan's threading
// model), replacing the guest's JobSystem_KickJobArray (0x35480) / WaitAndFreeCounter (0x35730) on
// the render path. The emulated PM4 path stays present until Phase 3, so any regression is one
// SOTC_RENDER switch away from isolation.
#pragma once

#include <memory>
#include <vector>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_camera.h"
#include "native/native_cull.h"
#include "native/native_descriptors.h"
#include "native/native_device.h"
#include "native/native_draw.h"
#include "native/native_pass.h"
#include "native/native_shadow.h"
#include "native/native_state.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
class Rasterizer;
} // namespace Vulkan

namespace Native {

/// The native renderer. One instance per process; created lazily the first time a native stage is
/// selected. It does not own the Vulkan device or the resource caches (reused from the port).
class Renderer {
public:
    Renderer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~Renderer();

    Device& Dev() noexcept {
        return device;
    }
    /// The scheduler the native draws are recorded on (reused from the port; owns the frame's
    /// command buffers and the submit order).
    Vulkan::Scheduler& Scheduler() noexcept {
        return scheduler;
    }
    Pass& Passes() noexcept {
        return pass;
    }
    Draw& Draws() noexcept {
        return draw;
    }
    Descriptors& Descs() noexcept {
        return descriptors;
    }
    Cull& Culling() noexcept {
        return cull;
    }
    Shadow& Shadows() noexcept {
        return shadow;
    }

    /// Per-frame entry (Phase 2.11 / 2.12): record one frame's passes. Returns false when the
    /// native path does not cover this frame (the caller falls back to the emulated path).
    bool RecordFrame(VAddr frame_object);

    /// The stage selected by SOTC_RENDER (empty when the native path is off).
    const char* Stage() const noexcept {
        return Device::Stage();
    }

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Device device;
    Pass pass;
    Draw draw;
    Descriptors descriptors;
    Cull cull;
    Shadow shadow;
};

/// The process-wide native renderer, or null when SOTC_RENDER is unset. Created on first use.
Renderer* Get(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
/// Null when the native path is off.
Renderer* Current();

} // namespace Native
