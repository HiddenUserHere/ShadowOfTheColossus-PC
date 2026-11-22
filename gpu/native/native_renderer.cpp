// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_renderer implementation.
#include "native/native_renderer.h"
#include "native/native_frame.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Native {

namespace {
Renderer* g_renderer = nullptr;
}

Renderer::Renderer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler)
    : instance{instance}, scheduler{scheduler}, device{instance}, pass{device}, draw{device},
      descriptors{device}, cull{device}, shadow{device, pass, draw} {}

Renderer::~Renderer() = default;

bool Renderer::RecordFrame(VAddr frame_object) {
    // Phase 2.11/2.12: the frame body lives in Native::RecordFrameNative (native_frame.cpp); it
    // derives the frame structure and returns false while any step is emulated (the caller then
    // runs the guest frame body). Phase 2 fills this in stage by stage. Each SOTC_RENDER stage
    // turns on more of the frame:
    //   native-clear   -> 2.1 (0x93690)
    //   native-draw    -> 2.2 (0x94ec0/0x94d00/0x94dd0)
    //   native-indirect-> 2.3 (0x10add0/0xdb620)
    //   native-bind    -> 2.4 (0x85fc10)
    //   native-rt      -> 2.5 (0x92140/0x85cbc0)
    //   native-pass    -> 2.6 (0x93960/0x94600/0x93ef0)
    //   native-cull    -> 2.7 (0x11d3d0 + predicate)
    //   native-shadow  -> 2.8 (0xcc9b0/0xc59d0)
    //   native-camera  -> 2.9 (0xcaed0)
    //   native-tables  -> 2.10 (0x95320/0xae150/0x123810/0x123cc0)
    //   native-frame   -> 2.11 (0x90ea0/0xc5560/0xd07c0/0x118470/0x956e0/0x1469d0)
    //   native         -> 2.12 (0xcefa0 body + thread path)
    return RecordFrameNative(static_cast<u64>(frame_object));
}

Renderer* Get(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    if (!Device::Enabled()) {
        return nullptr;
    }
    if (!g_renderer) {
        g_renderer = new Renderer(instance, scheduler);
    }
    return g_renderer;
}

Renderer* Current() {
    return g_renderer;
}

} // namespace Native
