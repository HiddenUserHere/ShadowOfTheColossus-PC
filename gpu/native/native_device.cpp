// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_device implementation.
#include <cstdlib>
#include <cstring>

#include "native/native_device.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Native {

vk::Device Device::Handle() const {
    return instance.GetDevice();
}

vk::PhysicalDevice Device::Physical() const {
    return instance.GetPhysicalDevice();
}

vk::Queue Device::GraphicsQueue() const {
    return instance.GetGraphicsQueue();
}

u32 Device::GraphicsQueueFamily() const {
    return instance.GetGraphicsQueueFamilyIndex();
}

bool Device::HasExtension(const char* name) const {
    for (const auto& ext : instance.GetAvailableExtensions()) {
        if (ext == name) {
            return true;
        }
    }
    return false;
}

bool Device::ConditionalRendering() const {
    // VK_EXT_conditional_rendering: the native cull pass falls back to a CPU skip when absent.
    return HasExtension(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
}

bool Device::HasDescriptorIndexing() const {
    return HasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) ||
           instance.ApiVersion() >= VK_API_VERSION_1_2;
}

bool Device::HasBufferDeviceAddress() const {
    // Buffer device address is core in 1.2 and a prerequisite of the port's storage buffers.
    return instance.ApiVersion() >= VK_API_VERSION_1_2;
}

bool Device::HasExternalMemoryHost() const {
    return instance.IsExternalMemoryHostEnabled();
}

bool Device::HasSynchronization2() const {
    return instance.ApiVersion() >= VK_API_VERSION_1_3;
}

bool Device::HasDynamicRendering() const {
    return instance.ApiVersion() >= VK_API_VERSION_1_3;
}

const char* Device::Stage() {
    static const char* stage = [] {
        const char* v = std::getenv("SOTC_RENDER");
        return v ? v : "";
    }();
    return stage;
}

bool Device::Enabled() {
    // Native stages are named by SOTC_RENDER ("native" installs every stage); the native backend
    // is on when nothing is named, and SOTC_RENDER=off is the pre-cutover A/B.
    const char* s = Stage();
    return !(s[0] != '\0' && std::strcmp(s, "off") == 0);
}

} // namespace Native
