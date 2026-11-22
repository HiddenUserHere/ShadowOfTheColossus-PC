// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). native_device: the device/queue/
// capability facade the native renderer issues against. Per the Phase 0 decision
// (docs/engine/native-render-map.md §0) the native backend REUSES the port's Vulkan resource
// layer (Vulkan::Instance, buffer_cache, texture_cache, page_manager) — those already do guest
// VA -> VkImage/VkBuffer, page protection and readback, and they are not render logic. This class
// therefore wraps Vulkan::Instance rather than creating a second device.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Native {

/// Thin, typed view of the device the native renderer uses. It does not own the device.
class Device {
public:
    explicit Device(const Vulkan::Instance& instance) : instance{instance} {}

    const Vulkan::Instance& Instance() const noexcept {
        return instance;
    }
    vk::Device Handle() const;
    vk::PhysicalDevice Physical() const;
    vk::Queue GraphicsQueue() const;
    u32 GraphicsQueueFamily() const;

    // Capability predicates the native passes must honour (all backed by the reused instance).
    bool ConditionalRendering() const;   ///< VK_EXT_conditional_rendering (cull)
    bool HasDescriptorIndexing() const;  ///< VK_EXT_descriptor_indexing (bindless heap)
    bool HasBufferDeviceAddress() const; ///< VK_KHR_buffer_device_address (BDA for guest memory)
    bool HasExternalMemoryHost() const;  ///< VK_EXT_external_memory_host (guest memory in place)
    bool HasSynchronization2() const;    ///< VK_KHR_synchronization2 (barriers)
    bool HasDynamicRendering() const;    ///< VK_KHR_dynamic_rendering (render passes)

    /// Whether the physical device advertises an extension (by literal name).
    bool HasExtension(const char* name) const;

    /// Enables the native renderer. Off until Phase 2.12 passes; SOTC_RENDER selects the stage.
    static bool Enabled();
    /// The SOTC_RENDER value ("", "native", "native-clear", "native-draw", ...).
    static const char* Stage();

private:
    const Vulkan::Instance& instance;
};

} // namespace Native
