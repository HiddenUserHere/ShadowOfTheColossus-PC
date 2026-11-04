// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// sotc: SOTC_GPU_CHECKPOINTS=1, GPU crash breadcrumbs (VK_NV_device_diagnostic_checkpoints, NVIDIA). Each draw and
// dispatch is recorded in a ring (its kind, the shader hashes of its stages, its sizes or indirect arguments) and a
// checkpoint whose marker is the entry's sequence number goes into the command buffer right before it. After a device
// lost, Report reads the last checkpoint each queue reached at the top and at the bottom of the pipeline and prints the
// ring entries around them: the commands still in flight when the GPU faulted, with their shaders. Off by default (one
// more command per draw on the recorder); without the extension (other vendors) it stays off.

#include <cstdint>

namespace vk {
class Queue;
}

namespace SotcCheckpoints {

enum class Kind : uint8_t {
    Draw = 1,
    DrawIndexed,
    DrawIndirect,
    DrawIndexedIndirect,
    DrawIndirectCount,
    DrawIndexedIndirectCount,
    Dispatch,
    DispatchIndirect,
    DrawRun, ///< SOTC_DRAW_MDI: a run of joined draws recorded at its flush
};

/// A marker with this bit is the checkpoint recorded right after command (marker & ~kAfterBit).
inline constexpr uint64_t kAfterBit = uint64_t{1} << 62;

/// The switch (SOTC_GPU_CHECKPOINTS=1) and the device extension (SetAvailable). Cheap: one load.
bool On() noexcept;
/// vk_instance.cpp: the extension was enabled on the device.
void SetAvailable(bool available) noexcept;
/// The switch alone (vk_instance.cpp asks before enabling the extension).
bool Wanted() noexcept;

/// Draw thread: a ring entry for the next command; returns the marker to pass to setCheckpointNV (never 0).
/// hashes: the stage program hashes (up to 6, 0 for an unused stage); a, b, c: the command's sizes or
/// arguments (vertex/index count and instances, groups, or the indirect buffer's guest address and count).
/// perms: the permutation index of each stage's module (0xFFFF unknown), or null.
uint64_t Note(Kind kind, const uint64_t* hashes, uint32_t hash_count, uint64_t a, uint64_t b, uint64_t c,
              const uint16_t* perms = nullptr) noexcept;

/// A resource bound to a command: its guest range and its Vulkan handle.
struct Resource {
    uint64_t addr;   ///< guest address of the bound range (0: none, e.g. GDS)
    uint64_t size;   ///< bytes
    uint64_t handle; ///< VkBuffer / VkImage
    uint8_t type;    ///< ResourceType
    uint64_t gpu;      ///< buffers: the device address of the bound range (0: unknown)
    uint64_t gpu_size; ///< buffers: the whole VkBuffer's bytes from gpu_base (for a fault address match)
    uint64_t gpu_base; ///< buffers: the whole VkBuffer's device address
};
/// kDescriptor: one descriptor write of the command (addr: binding | descriptor type << 32, size: range, handle: the
/// VkBuffer or VkImageView, gpu: the buffer offset).
enum ResourceType : uint8_t { kBufferRead = 1, kBufferWritten, kVertexIndex, kImage, kDescriptor, kPipeline };

/// Draw thread, right after Note: the buffers, images and descriptor writes of command `seq` (at most 64 kept).
void NoteResources(uint64_t seq, const Resource* resources, uint32_t count) noexcept;
/// Any thread, when a VkBuffer / VkImage is destroyed (UniqueBuffer / UniqueImage): Report names a resource of a
/// command in flight that was destroyed after that command was recorded (a use after free on the GPU).
void NoteFreed(uint64_t handle, uint8_t type) noexcept;

/// vk_instance.cpp: the queues whose checkpoints Report reads (graphics, async compute, transfer).
void RegisterQueue(vk::Queue queue, const char* name) noexcept;

/// After a device lost (Instance::ReportDeviceFault): the last checkpoints of each registered queue and the ring entries
/// around them, to stderr; a bound buffer whose device range holds one of fault_addresses (VK_EXT_device_fault) is
/// marked. Once per session.
void Report(const uint64_t* fault_addresses, uint32_t fault_count) noexcept;

} // namespace SotcCheckpoints
