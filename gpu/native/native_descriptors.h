// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_descriptors: the bindless descriptor layer. The game is
// bindless by construction (docs/engine/native-render-abi.md §3): shaders read V#/T#/S# through
// s_load from guest pointers passed in user SGPRs, so the native backend does not build per-draw
// descriptors on the CPU. This layer holds the guest-memory buffer views and the root-table
// pointers, and hands them to the port's existing descriptor heap / flatbuffer path.
#pragma once

#include "common/types.h"
#include "native/native_device.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class DescriptorHeap;
} // namespace Vulkan

namespace Native {

/// A descriptor-set-like table of N qwords built from the guest's masked root-table window
/// (0x95320 / 0xae150). The native backend reuses the guest's own table pointers.
struct RootTable {
    VAddr pointer{0}; ///< guest address of the table (0x95320 returns this)
    u32 mask{0};      ///< the low-10-bit mask selecting which slots are present
};

class Descriptors {
public:
    explicit Descriptors(const Device& device) : device{device} {}

    /// Bind the per-draw user-data block (the guest's 16-dword user SGPR window,
    /// docs/engine/native-render-abi.md §1) as push constants for a stage. This is the native
    /// equivalent of the guest's 0x86d550 / 0x86d4c0 / 0x86d5e0 writers.
    void PushUserData(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                      vk::ShaderStageFlags stages, const u32* user_data, u32 dwords) const;

    /// Record a root-table binding (0x95320 + 0xae150) for the draw.
    void BindRootTable(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                       vk::ShaderStageFlags stages, const RootTable& table) const;

private:
    const Device& device;
};

} // namespace Native
