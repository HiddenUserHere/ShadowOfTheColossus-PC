// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: taken from Pink-shadPS4 0.4.0 (luizgustavs), cp_record_thread; scheduler hooks in vk_scheduler.cpp.

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <stop_token>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perf_stats.h"
#include "common/thread.h"
#include "native/native_switches.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/sotc_stats.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define RECORDER_PAUSE() _mm_pause()
#else
#define RECORDER_PAUSE()
#endif

namespace Vulkan {

namespace {

using Dispatch = std::remove_cvref_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>;

constexpr u32 NumFakeHandles = CommandRecorder::NumFakeHandles;
alignas(64) u8 g_fake_handles[NumFakeHandles];


u32 CurrentThreadId() {
#ifdef _WIN32
    return static_cast<u32>(GetCurrentThreadId());
#else
    return static_cast<u32>(gettid());
#endif
}

[[noreturn]] void FailUncovered(const char* name) {
    UNREACHABLE_MSG(
        "cp_record_thread: {} was called on a recorded command buffer, but it has pointer "
        "arguments and no deep-copy thunk. Add one in vk_recorder.cpp",
        name);
}

template <typename T>
struct IsInputPointer : std::false_type {};
template <typename T>
struct IsInputPointer<const T*> : std::true_type {};

template <auto Member>
using MemberPfn = std::remove_cvref_t<decltype(std::declval<Dispatch&>().*Member)>;

/// Handle one vkCmd entry by passing real command buffers straight to the driver and queuing calls
/// made with fake handles
/// Copy ordinary arguments by value, but use a dedicated deep-copy handler when pointers refer to
/// data needed later
/// Call must not keep a pointer into memory the producer can reuse before the consumer reaches it
template <auto Member, typename Pfn = MemberPfn<Member>>
struct GenericThunk;

template <auto Member, typename R, typename... Args>
struct GenericThunk<Member, R(VKAPI_PTR*)(VkCommandBuffer, Args...)> {
    using Pfn = R(VKAPI_PTR*)(VkCommandBuffer, Args...);
    static inline Pfn real{};
    static inline const char* name{};

    static R VKAPI_CALL Call(VkCommandBuffer cmdbuf, Args... args) {
        if (!CommandRecorder::IsFake(cmdbuf)) {
            return real(cmdbuf, args...);
        }
        // Commands returning a result, such as VK_INTEL_performance_query calls, need an immediate
        // driver result and cannot be queued here
        if constexpr (!std::is_void_v<R> || (IsInputPointer<Args>::value || ...)) {
            FailUncovered(name);
        } else {
            TheRecorder()->CountCommand();
            TheRecorder()->Run([cmdbuf, args...] { real(TheRecorder()->Real(cmdbuf), args...); });
        }
    }
};

template <auto Member>
void InstallGeneric(MemberPfn<Member>& slot, const char* name) {
    if (slot == nullptr) {
        return;
    }
    using Thunk = GenericThunk<Member>;
    Thunk::real = slot;
    Thunk::name = name;
    slot = &Thunk::Call;
}

template <auto Member>
auto RealOf() {
    return GenericThunk<Member>::real;
}

// Copy pointer arguments before sending a command to the recording thread so the caller can reuse
// its memory after returning
// Real handles still go straight to the driver, while fake handles store the arrays beside the
// queued command
// pNext chains are not expected here, so stop on a non-null chain instead of silently losing its
// data
// Reserve room for all pointed-to arrays before copying them and queue the call with references to
// those owned copies

void CheckNoChain(const void* next, const char* what) {
    if (next != nullptr) {
        UNREACHABLE_MSG("cp_record_thread: {} with a pNext chain has no deep copy", what);
    }
}

template <auto Member>
void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cmdbuf, const VkDependencyInfo* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, info);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(sizeof(VkDependencyInfo) + info->memoryBarrierCount * sizeof(VkMemoryBarrier2) +
                info->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2) +
                info->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2) + 256);
    CheckNoChain(info->pNext, "VkDependencyInfo");
    auto* copy = rec.Copy(info);
    copy->pMemoryBarriers = rec.Copy(info->pMemoryBarriers, info->memoryBarrierCount);
    copy->pBufferMemoryBarriers =
        rec.Copy(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount);
    copy->pImageMemoryBarriers =
        rec.Copy(info->pImageMemoryBarriers, info->imageMemoryBarrierCount);
    for (u32 i = 0; i < info->imageMemoryBarrierCount; ++i) {
        CheckNoChain(info->pImageMemoryBarriers[i].pNext, "VkImageMemoryBarrier2");
    }
    TheRecorder()->CountCommand();
    rec.Run([cmdbuf, copy] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), copy); });
}

template <auto Member>
void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cmdbuf, VkPipelineStageFlags src_stage,
                                   VkPipelineStageFlags dst_stage, VkDependencyFlags flags,
                                   u32 num_memory, const VkMemoryBarrier* memory, u32 num_buffer,
                                   const VkBufferMemoryBarrier* buffer, u32 num_image,
                                   const VkImageMemoryBarrier* image) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, src_stage, dst_stage, flags, num_memory, memory, num_buffer,
                                buffer, num_image, image);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(num_memory * sizeof(VkMemoryBarrier) + num_buffer * sizeof(VkBufferMemoryBarrier) +
                num_image * sizeof(VkImageMemoryBarrier) + 256);
    const auto* memory_copy = rec.Copy(memory, num_memory);
    const auto* buffer_copy = rec.Copy(buffer, num_buffer);
    const auto* image_copy = rec.Copy(image, num_image);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), src_stage, dst_stage, flags, num_memory,
                         memory_copy, num_buffer, buffer_copy, num_image, image_copy);
    });
}

/// Keep the vkCmdPushDescriptorSet call, its writes and the descriptor information in one command
/// block
/// This call happens on every draw and dispatch with about ten writes, so filling the block in
/// place avoids separate allocations
struct PushDescriptorPayload {
    VkCommandBuffer cmdbuf;
    VkPipelineLayout layout;
    VkPipelineBindPoint bind_point;
    u32 set;
    u32 num_writes;
    // Place VkWriteDescriptorSet[num_writes] and the referenced descriptor information after this
    // header, keeping each part aligned to 8 bytes
};
static_assert(sizeof(PushDescriptorPayload) % alignof(VkWriteDescriptorSet) == 0);

/// Choose the descriptor information array used by this write
/// Vulkan ignores the other two pointers for that descriptor type, so they do not need to contain
/// valid data
enum class WriteArray { Image, Buffer, TexelView };
WriteArray ArrayOf(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return WriteArray::Image;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return WriteArray::TexelView;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return WriteArray::Buffer;
    default:
        UNREACHABLE_MSG("cp_record_thread: push descriptor write of type {} has no deep copy",
                        static_cast<u32>(type));
    }
}

size_t ArrayBytes(const VkWriteDescriptorSet& write) {
    switch (ArrayOf(write.descriptorType)) {
    case WriteArray::Image:
        return write.descriptorCount * sizeof(VkDescriptorImageInfo);
    case WriteArray::Buffer:
        return write.descriptorCount * sizeof(VkDescriptorBufferInfo);
    case WriteArray::TexelView:
        return write.descriptorCount * sizeof(VkBufferView);
    }
    return 0;
}

template <auto Member>
void InvokePushDescriptorSet(void* payload) {
    const auto* push = static_cast<const PushDescriptorPayload*>(payload);
    const auto* writes = reinterpret_cast<const VkWriteDescriptorSet*>(push + 1);
    RealOf<Member>()(TheRecorder()->Real(push->cmdbuf), push->bind_point, push->layout, push->set,
                     push->num_writes, writes);
}

template <auto Member>
void VKAPI_CALL CmdPushDescriptorSet(VkCommandBuffer cmdbuf, VkPipelineBindPoint bind_point,
                                     VkPipelineLayout layout, u32 set, u32 num_writes,
                                     const VkWriteDescriptorSet* writes) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, bind_point, layout, set, num_writes, writes);
    }
    size_t bytes = sizeof(PushDescriptorPayload) + num_writes * sizeof(VkWriteDescriptorSet);
    for (u32 i = 0; i < num_writes; ++i) {
        CheckNoChain(writes[i].pNext, "VkWriteDescriptorSet");
        bytes += ArrayBytes(writes[i]);
    }
    auto& rec = *TheRecorder();
    auto* push = static_cast<PushDescriptorPayload*>(
        rec.AllocateCommand(bytes, &InvokePushDescriptorSet<Member>));
    *push = {cmdbuf, layout, bind_point, set, num_writes};
    auto* copy = reinterpret_cast<VkWriteDescriptorSet*>(push + 1);
    std::memcpy(copy, writes, num_writes * sizeof(VkWriteDescriptorSet));
    u8* data = reinterpret_cast<u8*>(copy + num_writes);
    for (u32 i = 0; i < num_writes; ++i) {
        auto& write = copy[i];
        const WriteArray array = ArrayOf(write.descriptorType);
        const size_t size = ArrayBytes(write);
        const void* src = array == WriteArray::Image    ? static_cast<const void*>(write.pImageInfo)
                          : array == WriteArray::Buffer ? static_cast<const void*>(write.pBufferInfo)
                                                        : static_cast<const void*>(write.pTexelBufferView);
        std::memcpy(data, src, size);
        write.pImageInfo = array == WriteArray::Image
                               ? reinterpret_cast<const VkDescriptorImageInfo*>(data)
                               : nullptr;
        write.pBufferInfo = array == WriteArray::Buffer
                                ? reinterpret_cast<const VkDescriptorBufferInfo*>(data)
                                : nullptr;
        write.pTexelBufferView =
            array == WriteArray::TexelView ? reinterpret_cast<const VkBufferView*>(data) : nullptr;
        data += size;
    }
    rec.CountCommand();
    rec.Commit();
}

template <auto Member>
void VKAPI_CALL CmdPushConstants(VkCommandBuffer cmdbuf, VkPipelineLayout layout,
                                 VkShaderStageFlags stages, u32 offset, u32 size,
                                 const void* values) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, layout, stages, offset, size, values);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(size + 256);
    const auto* copy = rec.Copy(static_cast<const u8*>(values), size);
    TheRecorder()->CountCommand();
    rec.Run(
        [=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), layout, stages, offset, size, copy); });
}

const VkDebugUtilsLabelEXT* CopyLabel(CommandRecorder& rec, const VkDebugUtilsLabelEXT* label) {
    const size_t name_size = label->pLabelName ? std::strlen(label->pLabelName) + 1 : 0;
    rec.Reserve(sizeof(VkDebugUtilsLabelEXT) + name_size + 256);
    CheckNoChain(label->pNext, "VkDebugUtilsLabelEXT");
    auto* copy = rec.Copy(label);
    copy->pLabelName = rec.Copy(label->pLabelName, name_size);
    return copy;
}

template <auto Member>
void VKAPI_CALL CmdDebugLabel(VkCommandBuffer cmdbuf, const VkDebugUtilsLabelEXT* label) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, label);
    }
    const auto* copy = CopyLabel(*TheRecorder(), label);
    TheRecorder()->CountCommand();
    TheRecorder()->Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), copy); });
}

template <auto Member>
void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cmdbuf, const VkRenderingInfo* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, info);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(sizeof(VkRenderingInfo) +
                (info->colorAttachmentCount + 2) * sizeof(VkRenderingAttachmentInfo) + 256);
    CheckNoChain(info->pNext, "VkRenderingInfo");
    auto* copy = rec.Copy(info);
    copy->pColorAttachments = rec.Copy(info->pColorAttachments, info->colorAttachmentCount);
    copy->pDepthAttachment = rec.Copy(info->pDepthAttachment);
    copy->pStencilAttachment = rec.Copy(info->pStencilAttachment);
    for (u32 i = 0; i < info->colorAttachmentCount; ++i) {
        CheckNoChain(info->pColorAttachments[i].pNext, "VkRenderingAttachmentInfo");
    }
    TheRecorder()->CountCommand();
    rec.Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), copy); });
}

/// Queue buffer and image copy, resolve or blit commands together with the region arrays they need
/// when the recording thread runs them
template <auto Member, typename Region, typename... Pre>
struct RegionsThunk;

template <auto Member, typename Region, typename... Pre>
struct RegionsThunk<Member, Region, void(Pre...)> {
    static void VKAPI_CALL Call(VkCommandBuffer cmdbuf, Pre... pre, u32 count,
                                const Region* regions) {
        if (!CommandRecorder::IsFake(cmdbuf)) {
            return RealOf<Member>()(cmdbuf, pre..., count, regions);
        }
        auto& rec = *TheRecorder();
        rec.Reserve(count * sizeof(Region) + 256);
        const auto* copy = rec.Copy(regions, count);
        TheRecorder()->CountCommand();
        rec.Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), pre..., count, copy); });
    }
};

template <auto Member>
void VKAPI_CALL CmdBlitImage(VkCommandBuffer cmdbuf, VkImage src, VkImageLayout src_layout,
                             VkImage dst, VkImageLayout dst_layout, u32 count,
                             const VkImageBlit* regions, VkFilter filter) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, src, src_layout, dst, dst_layout, count, regions, filter);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * sizeof(VkImageBlit) + 256);
    const auto* copy = rec.Copy(regions, count);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), src, src_layout, dst, dst_layout, count, copy,
                         filter);
    });
}

template <auto Member, typename Value>
void VKAPI_CALL CmdClearImage(VkCommandBuffer cmdbuf, VkImage image, VkImageLayout layout,
                              const Value* value, u32 count,
                              const VkImageSubresourceRange* ranges) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, image, layout, value, count, ranges);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(sizeof(Value) + count * sizeof(VkImageSubresourceRange) + 256);
    const auto* value_copy = rec.Copy(value);
    const auto* ranges_copy = rec.Copy(ranges, count);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), image, layout, value_copy, count, ranges_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cmdbuf, VkBuffer buffer, VkDeviceSize offset,
                                VkDeviceSize size, const void* data) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, buffer, offset, size, data);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(size + 256);
    const auto* copy = rec.Copy(static_cast<const u8*>(data), size);
    TheRecorder()->CountCommand();
    rec.Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), buffer, offset, size, copy); });
}

/// Copy viewport and scissor arrays for both the first-and-count commands and the WithCount
/// variants before queuing them
template <auto Member, typename Element>
void VKAPI_CALL CmdSetFirstArray(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                 const Element* elements) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, elements);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * sizeof(Element) + 256);
    const auto* copy = rec.Copy(elements, count);
    TheRecorder()->CountCommand();
    rec.Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), first, count, copy); });
}

template <auto Member, typename Element>
void VKAPI_CALL CmdSetArray(VkCommandBuffer cmdbuf, u32 count, const Element* elements) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, count, elements);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * sizeof(Element) + 256);
    const auto* copy = rec.Copy(elements, count);
    TheRecorder()->CountCommand();
    rec.Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), count, copy); });
}

template <auto Member>
void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cmdbuf, const float constants[4]) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, constants);
    }
    std::array<float, 4> copy;
    std::memcpy(copy.data(), constants, sizeof(copy));
    TheRecorder()->CountCommand();
    TheRecorder()->Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), copy.data()); });
}

template <auto Member>
void VKAPI_CALL CmdSetVertexInput(VkCommandBuffer cmdbuf, u32 num_bindings,
                                  const VkVertexInputBindingDescription2EXT* bindings,
                                  u32 num_attributes,
                                  const VkVertexInputAttributeDescription2EXT* attributes) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, num_bindings, bindings, num_attributes, attributes);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(num_bindings * sizeof(*bindings) + num_attributes * sizeof(*attributes) + 256);
    const auto* bindings_copy = rec.Copy(bindings, num_bindings);
    const auto* attributes_copy = rec.Copy(attributes, num_attributes);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), num_bindings, bindings_copy, num_attributes,
                         attributes_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                     const VkBuffer* buffers, const VkDeviceSize* offsets) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, buffers, offsets);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * (sizeof(VkBuffer) + sizeof(VkDeviceSize)) + 256);
    const auto* buffers_copy = rec.Copy(buffers, count);
    const auto* offsets_copy = rec.Copy(offsets, count);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), first, count, buffers_copy, offsets_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                      const VkBuffer* buffers, const VkDeviceSize* offsets,
                                      const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, buffers, offsets, sizes, strides);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * (sizeof(VkBuffer) + 3 * sizeof(VkDeviceSize)) + 256);
    const auto* buffers_copy = rec.Copy(buffers, count);
    const auto* offsets_copy = rec.Copy(offsets, count);
    const auto* sizes_copy = rec.Copy(sizes, count);
    const auto* strides_copy = rec.Copy(strides, count);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), first, count, buffers_copy, offsets_copy,
                         sizes_copy, strides_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cmdbuf, VkPipelineBindPoint bind_point,
                                      VkPipelineLayout layout, u32 first, u32 count,
                                      const VkDescriptorSet* sets, u32 num_dynamic,
                                      const u32* dynamic_offsets) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, bind_point, layout, first, count, sets, num_dynamic,
                                dynamic_offsets);
    }
    auto& rec = *TheRecorder();
    rec.Reserve(count * sizeof(VkDescriptorSet) + num_dynamic * sizeof(u32) + 256);
    const auto* sets_copy = rec.Copy(sets, count);
    const auto* offsets_copy = rec.Copy(dynamic_offsets, num_dynamic);
    TheRecorder()->CountCommand();
    rec.Run([=] {
        RealOf<Member>()(TheRecorder()->Real(cmdbuf), bind_point, layout, first, count, sets_copy,
                         num_dynamic, offsets_copy);
    });
}

/// Keep the opaque marker value unchanged so the driver can return it after device loss, without
/// treating it as a pointer to dereference
template <auto Member>
void VKAPI_CALL CmdSetCheckpoint(VkCommandBuffer cmdbuf, const void* marker) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, marker);
    }
    TheRecorder()->CountCommand();
    TheRecorder()->Run([=] { RealOf<Member>()(TheRecorder()->Real(cmdbuf), marker); });
}

// The scheduler begins recorded command buffers after the recording thread allocates them
// End and Reset calls using a fake handle must join the command stream so their lifetime operations
// stay in order

PFN_vkBeginCommandBuffer real_begin{};
PFN_vkEndCommandBuffer real_end{};
PFN_vkResetCommandBuffer real_reset{};

VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cmdbuf,
                                       const VkCommandBufferBeginInfo* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return real_begin(cmdbuf, info);
    }
    FailUncovered("vkBeginCommandBuffer");
}

VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cmdbuf) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return real_end(cmdbuf);
    }
    TheRecorder()->Run([cmdbuf] {
        const VkResult result = real_end(TheRecorder()->Real(cmdbuf));
        ASSERT_MSG(result == VK_SUCCESS, "cp_record_thread: vkEndCommandBuffer failed: {}",
                   vk::to_string(vk::Result{result}));
    });
    return VK_SUCCESS;
}

VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cmdbuf, VkCommandBufferResetFlags flags) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return real_reset(cmdbuf, flags);
    }
    TheRecorder()->Run([cmdbuf, flags] { real_reset(TheRecorder()->Real(cmdbuf), flags); });
    return VK_SUCCESS;
}

} // Anonymous namespace

void CommandRecorder::InstallThunks() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
        ASSERT_MSG(d.vkCmdDraw != nullptr, "cp_record_thread: device functions are not loaded yet");

#define VK_RECORDER_CMD(name) InstallGeneric<&Dispatch::name>(d.name, #name);
#include "video_core/renderer_vulkan/vk_recorder_cmds.inc"
#undef VK_RECORDER_CMD

#define OVERRIDE(name, ...)                                                                        \
    if (d.name != nullptr) {                                                                       \
        d.name = &__VA_ARGS__;                                                                     \
    }
        OVERRIDE(vkCmdPipelineBarrier2, CmdPipelineBarrier2<&Dispatch::vkCmdPipelineBarrier2>);
        OVERRIDE(vkCmdPipelineBarrier2KHR,
                 CmdPipelineBarrier2<&Dispatch::vkCmdPipelineBarrier2KHR>);
        OVERRIDE(vkCmdPipelineBarrier, CmdPipelineBarrier<&Dispatch::vkCmdPipelineBarrier>);
        OVERRIDE(vkCmdPushDescriptorSet, CmdPushDescriptorSet<&Dispatch::vkCmdPushDescriptorSet>);
        OVERRIDE(vkCmdPushDescriptorSetKHR,
                 CmdPushDescriptorSet<&Dispatch::vkCmdPushDescriptorSetKHR>);
        OVERRIDE(vkCmdPushConstants, CmdPushConstants<&Dispatch::vkCmdPushConstants>);
        OVERRIDE(vkCmdBeginDebugUtilsLabelEXT,
                 CmdDebugLabel<&Dispatch::vkCmdBeginDebugUtilsLabelEXT>);
        OVERRIDE(vkCmdInsertDebugUtilsLabelEXT,
                 CmdDebugLabel<&Dispatch::vkCmdInsertDebugUtilsLabelEXT>);
        OVERRIDE(vkCmdBeginRendering, CmdBeginRendering<&Dispatch::vkCmdBeginRendering>);
        OVERRIDE(vkCmdBeginRenderingKHR, CmdBeginRendering<&Dispatch::vkCmdBeginRenderingKHR>);
        OVERRIDE(
            vkCmdCopyBuffer,
            RegionsThunk<&Dispatch::vkCmdCopyBuffer, VkBufferCopy, void(VkBuffer, VkBuffer)>::Call);
        OVERRIDE(vkCmdCopyImage,
                 RegionsThunk<&Dispatch::vkCmdCopyImage, VkImageCopy,
                              void(VkImage, VkImageLayout, VkImage, VkImageLayout)>::Call);
        OVERRIDE(vkCmdCopyBufferToImage,
                 RegionsThunk<&Dispatch::vkCmdCopyBufferToImage, VkBufferImageCopy,
                              void(VkBuffer, VkImage, VkImageLayout)>::Call);
        OVERRIDE(vkCmdCopyImageToBuffer,
                 RegionsThunk<&Dispatch::vkCmdCopyImageToBuffer, VkBufferImageCopy,
                              void(VkImage, VkImageLayout, VkBuffer)>::Call);
        OVERRIDE(vkCmdResolveImage,
                 RegionsThunk<&Dispatch::vkCmdResolveImage, VkImageResolve,
                              void(VkImage, VkImageLayout, VkImage, VkImageLayout)>::Call);
        OVERRIDE(vkCmdBlitImage, CmdBlitImage<&Dispatch::vkCmdBlitImage>);
        OVERRIDE(vkCmdClearColorImage,
                 CmdClearImage<&Dispatch::vkCmdClearColorImage, VkClearColorValue>);
        OVERRIDE(vkCmdClearDepthStencilImage,
                 CmdClearImage<&Dispatch::vkCmdClearDepthStencilImage, VkClearDepthStencilValue>);
        OVERRIDE(vkCmdUpdateBuffer, CmdUpdateBuffer<&Dispatch::vkCmdUpdateBuffer>);
        OVERRIDE(vkCmdSetViewport, CmdSetFirstArray<&Dispatch::vkCmdSetViewport, VkViewport>);
        OVERRIDE(vkCmdSetScissor, CmdSetFirstArray<&Dispatch::vkCmdSetScissor, VkRect2D>);
        OVERRIDE(vkCmdSetViewportWithCount,
                 CmdSetArray<&Dispatch::vkCmdSetViewportWithCount, VkViewport>);
        OVERRIDE(vkCmdSetViewportWithCountEXT,
                 CmdSetArray<&Dispatch::vkCmdSetViewportWithCountEXT, VkViewport>);
        OVERRIDE(vkCmdSetScissorWithCount,
                 CmdSetArray<&Dispatch::vkCmdSetScissorWithCount, VkRect2D>);
        OVERRIDE(vkCmdSetScissorWithCountEXT,
                 CmdSetArray<&Dispatch::vkCmdSetScissorWithCountEXT, VkRect2D>);
        OVERRIDE(vkCmdSetColorWriteMaskEXT,
                 CmdSetFirstArray<&Dispatch::vkCmdSetColorWriteMaskEXT, VkColorComponentFlags>);
        OVERRIDE(vkCmdSetBlendConstants, CmdSetBlendConstants<&Dispatch::vkCmdSetBlendConstants>);
        OVERRIDE(vkCmdSetVertexInputEXT, CmdSetVertexInput<&Dispatch::vkCmdSetVertexInputEXT>);
        OVERRIDE(vkCmdBindVertexBuffers, CmdBindVertexBuffers<&Dispatch::vkCmdBindVertexBuffers>);
        OVERRIDE(vkCmdBindVertexBuffers2,
                 CmdBindVertexBuffers2<&Dispatch::vkCmdBindVertexBuffers2>);
        OVERRIDE(vkCmdBindVertexBuffers2EXT,
                 CmdBindVertexBuffers2<&Dispatch::vkCmdBindVertexBuffers2EXT>);
        OVERRIDE(vkCmdBindDescriptorSets,
                 CmdBindDescriptorSets<&Dispatch::vkCmdBindDescriptorSets>);
        OVERRIDE(vkCmdSetCheckpointNV, CmdSetCheckpoint<&Dispatch::vkCmdSetCheckpointNV>);
#undef OVERRIDE

        real_begin = d.vkBeginCommandBuffer;
        real_end = d.vkEndCommandBuffer;
        real_reset = d.vkResetCommandBuffer;
        d.vkBeginCommandBuffer = &BeginCommandBuffer;
        d.vkEndCommandBuffer = &EndCommandBuffer;
        d.vkResetCommandBuffer = &ResetCommandBuffer;
    });
}

bool CommandRecorder::IsFake(VkCommandBuffer cmdbuf) noexcept {
    const auto offset =
        reinterpret_cast<uintptr_t>(cmdbuf) - reinterpret_cast<uintptr_t>(g_fake_handles);
    return offset < NumFakeHandles;
}

CommandRecorder*& TheRecorder() {
    static CommandRecorder* recorder{};
    return recorder;
}

CommandRecorder::CommandRecorder() {
    ASSERT_MSG(TheRecorder() == nullptr, "cp_record_thread: only one recorded scheduler is supported");
    InstallThunks();
    TheRecorder() = this;
    write_chunk = AcquireChunk();
    first_chunk = write_chunk;
    producer_tid = CurrentThreadId();
    // sotc: N2-94 W4. Bigger publish batches only with the untimed consumer wait (SOTC_REC_BLOCK).
    publish_batch = Native::Switch::RecBlock() ? PublishBatchBlock : PublishBatchDefault;
    thread = std::jthread([this](std::stop_token stoken) { ConsumerLoop(stoken); });
}

CommandRecorder::~CommandRecorder() {
    Sync();
    thread.request_stop();
    WakeConsumer();
    thread.join();
    TheRecorder() = nullptr;
    std::scoped_lock lk{free_mutex};
    for (Chunk* chunk : free_chunks) {
        delete chunk;
    }
    // The chunks still linked after the consumer position contain no remaining commands to run and
    // can be released during cleanup
}

vk::CommandBuffer CommandRecorder::NewFakeHandle() {
    // Fake handles wrap around after NumFakeHandles sessions, so confirm the old session was
    // submitted before using that handle again
    const SotcStats::SplitTimer recorder_split{SotcStats::SplitWaitRecorder};
    while (handles_issued - handles_retired.load(std::memory_order_acquire) >= NumFakeHandles / 2) {
        Kick();
        std::this_thread::yield();
    }
    const u32 index = static_cast<u32>(handles_issued++ % NumFakeHandles);
    return vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(&g_fake_handles[index])};
}

void CommandRecorder::Bind(VkCommandBuffer fake, VkCommandBuffer real) noexcept {
    real_handles[reinterpret_cast<u8*>(fake) - g_fake_handles] = real;
}

VkCommandBuffer CommandRecorder::Real(VkCommandBuffer fake) const noexcept {
    return real_handles[reinterpret_cast<u8*>(fake) - g_fake_handles];
}

void CommandRecorder::CheckProducer() {
    static thread_local const u32 tid = CurrentThreadId();
    if (tid == producer_tid) [[likely]] {
        return;
    }
    // sotc: the thread of a batch (BeginBatch), and the recording thread while it runs one on
    // its ops that record for themselves (Scheduler::RunAfterRecorded: the native draws) own the
    // stream until the batch's next Publish. The stream goes back to the producer at that Publish.
    if (in_batch) {
        return;
    }
    if (IsRecordingThread() && !thread_command) {
        in_batch = true;
        LOG_INFO(Render_Vulkan, "cp_record_thread: recording thread takes the stream (producer {})",
                 tid);
        return;
    }
    // Let the command processor take over once from the thread that created the renderer, and let
    // shutdown call ClaimProducer if it changes threads
    // Any other producer change means two threads could record into the same stream, which this
    // queue does not support
    if (producer_switches++ == 0) {
        LOG_WARNING(Render_Vulkan, "cp_record_thread: producer thread changed {} -> {}", producer_tid,
                    tid);
        producer_tid = tid;
        return;
    }
    // sotc: the producer moving between the command processor and the recording thread is the
    // batched path above not having given the stream back yet (Scheduler::CommitRecordedCommands
    // and RunAfterRecorded are the points that do): let the batch's own Kick close it instead of
    // stopping the game, which is what a real second writer would have to be stopped for.
    if (std::this_thread::get_id() == thread.get_id() && !thread_command) {
        in_batch = true;
        LOG_INFO(Render_Vulkan, "cp_record_thread: recording thread takes the stream again (producer "
                                "{})",
                 tid);
        return;
    }
    // sotc: the batched producers move the stream between the command processor and the recording
    // thread (CommitRecordedCommands: a native batch hands over, the guest keeps recording). That
    // move is not worth stopping a game over; a real second writer is a different thread and still
    // stops here.
    producer_tid = tid;
    LOG_WARNING(Render_Vulkan, "cp_record_thread: producer thread moving {} -> {} (batched)",
                producer_tid, tid);
}

void CommandRecorder::BeginBatch() {
    if (IsRecordingThread()) {
        // The recording thread's own ops (Scheduler::RunAfterRecorded) take the stream in
        // CheckProducer.
        return;
    }
    ASSERT_MSG(!in_batch, "cp_record_thread: a batch started while another one is open");
    in_batch = true;
    const u32 tid = CurrentThreadId();
    LOG_INFO(Render_Vulkan, "cp_record_thread: thread {} records a batch for the stream (producer "
                            "{})",
             tid, producer_tid);
    producer_tid = tid;
}

void CommandRecorder::ClaimProducer() {
    const u32 tid = CurrentThreadId();
    if (tid != producer_tid) {
        LOG_INFO(Render_Vulkan, "cp_record_thread: thread {} takes over recording from {}", tid,
                 producer_tid);
        producer_tid = tid;
    }
}

void* CommandRecorder::Allocate(size_t size, BlockKind kind, void (*invoke)(void*)) {
    const size_t total = (sizeof(BlockHeader) + size + BlockAlign - 1) & ~(BlockAlign - 1);
    ASSERT_MSG(total + sizeof(BlockHeader) <= Chunk::Size, "cp_record_thread: block of {} bytes",
               total);
    if (write_pos + total + sizeof(BlockHeader) > Chunk::Size) {
        NextChunk();
    }
    auto* header = reinterpret_cast<BlockHeader*>(write_chunk->data + write_pos);
    header->size = static_cast<u32>(total);
    header->kind = kind;
    header->invoke = invoke;
    write_pos += static_cast<u32>(total);
    return header + 1;
}

void CommandRecorder::Reserve(size_t bytes) {
    if (write_pos + bytes + sizeof(BlockHeader) > Chunk::Size && bytes < Chunk::Size / 2) {
        NextChunk();
    }
}

void CommandRecorder::Publish() {
    // sotc: a batch (BeginBatch) and the ops the recording thread replays (Scheduler::
    // RunAfterRecorded: the native draws) own the stream while they record, whoever added them.
    // The batch's own Kick closes it; an op gives the stream back here, before the commands that
    // follow it queue, so the producer's next Publish -- the command processor adding its sets and
    // draws again -- finds the stream free instead of finding a second producer.
    if (in_batch && !thread_command) {
        in_batch = false;
        producer_tid = CurrentThreadId();
        LOG_INFO(Render_Vulkan, "cp_record_thread: the stream goes back to the producer ({})",
                 producer_tid);
    }
    CheckProducer();
    // Publish commands in batches so the producer and consumer do not keep competing over the
    // committed cache line
    // Every Kick publishes the whole pending batch before submits or drains need to observe it
    if (++unpublished >= publish_batch) {
        PublishAll();
    }
}

void CommandRecorder::PublishAll() {
    unpublished = 0;
    write_chunk->committed.store(write_pos, std::memory_order_release);
    // sotc: pairs with the consumer's "sleeping, fence, re-check": without it the store and the
    // sleeping-flag load could reorder and a wake be missed (2 ms on Windows, SleepTime).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    WakeIfSleeping();
}

void CommandRecorder::WakeIfSleeping() {
    // Send one wake-up for each timed sleep and keep the sleeping flag set until that wait ends
    // Otherwise every published batch during the same sleep would lock the mutex and notify again
    if (consumer_sleeping.load(std::memory_order_relaxed) &&
        consumer_sleeping.exchange(false, std::memory_order_acq_rel)) {
        WakeConsumer();
        Common::PerfStats::Add(Common::PerfStats::Id::RecorderWakes);
    }
}

void CommandRecorder::NextChunk() {
    Chunk* next = AcquireChunk();
    write_chunk->next.store(next, std::memory_order_release);
    auto* end = reinterpret_cast<BlockHeader*>(write_chunk->data + write_pos);
    end->size = sizeof(BlockHeader);
    end->kind = BlockKind::End;
    end->invoke = nullptr;
    write_pos += sizeof(BlockHeader);
    write_chunk->committed.store(write_pos, std::memory_order_release);
    write_chunk = next;
    write_pos = 0;
}

CommandRecorder::Chunk* CommandRecorder::AcquireChunk() {
    std::optional<SotcStats::SplitTimer> recorder_split;
    if (chunks_in_flight.load(std::memory_order_acquire) >= MaxChunksInFlight) {
        recorder_split.emplace(SotcStats::SplitWaitRecorder);
    }
    while (chunks_in_flight.load(std::memory_order_acquire) >= MaxChunksInFlight) {
        // The recording thread is far enough behind to fill the queue, so let it catch up before
        // the producer adds more commands
        Kick();
        std::this_thread::yield();
    }
    chunks_in_flight.fetch_add(1, std::memory_order_relaxed);
    Chunk* chunk = nullptr;
    {
        std::scoped_lock lk{free_mutex};
        if (!free_chunks.empty()) {
            chunk = free_chunks.back();
            free_chunks.pop_back();
        }
    }
    if (chunk == nullptr) {
        chunk = new Chunk;
    }
    chunk->committed.store(0, std::memory_order_relaxed);
    chunk->next.store(nullptr, std::memory_order_relaxed);
    return chunk;
}

void CommandRecorder::RecycleChunk(Chunk* chunk) {
    {
        std::scoped_lock lk{free_mutex};
        free_chunks.push_back(chunk);
    }
    chunks_in_flight.fetch_sub(1, std::memory_order_release);
}

void CommandRecorder::WakeConsumer() {
    {
        std::scoped_lock lk{wake_mutex};
        wake_flag = true;
    }
    wake_cv.notify_one();
}

void CommandRecorder::Kick() {
    unpublished = 0;
    write_chunk->committed.store(write_pos, std::memory_order_release);
    if (commands_counted != 0) {
        Common::PerfStats::Add(Common::PerfStats::Id::RecorderCommands, commands_counted);
        commands_counted = 0;
    }
    // Pair this with the consumer fence before its next work check so either it sees the newly
    // published position or the producer sees it sleeping
    std::atomic_thread_fence(std::memory_order_seq_cst);
    WakeIfSleeping();
}

void CommandRecorder::Sync() {
    using namespace Common::PerfStats;
    const bool perf = Enabled();
    const auto start =
        perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const u64 target = ++sync_requested;
    Run([this, target] {
        sync_done.store(target, std::memory_order_release);
        sync_done.notify_all();
    });
    Kick();
    const SotcStats::SplitTimer recorder_split{SotcStats::SplitWaitRecorder};
    u64 done = sync_done.load(std::memory_order_acquire);
    while (done < target) {
        sync_done.wait(done, std::memory_order_acquire);
        done = sync_done.load(std::memory_order_acquire);
    }
    if (perf) {
        Add(Id::RecorderDrains);
        Add(Id::RecorderDrainNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
    }
}

void CommandRecorder::ConsumerLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuRecorder");
    SotcStats::RegisterSplitThread(SotcStats::SplitThreadRecorder);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    using Clock = std::chrono::steady_clock;
    // The command processor publishes another batch every few microseconds while working
    // Keep the consumer spinning briefly between batches to avoid making the producer wake it for
    // every small update
    static const u32 logical = [] {
        u32 count = std::thread::hardware_concurrency();
#ifdef _WIN32
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) &&
            process_mask) {
            count = std::popcount(static_cast<u64>(process_mask));
        }
#endif
        return count;
    }();
    static const bool few_cpus = logical && logical <= 8;
    // sotc: SOTC_REC_SPIN_US overrides the spin (A/B: on a power-limited laptop the spinning
    // recorder took a whole core, ~2/3 of it waiting, next to the command processor it waits on).
    // With 8 logical processors or fewer it does not spin: attract demo, Ryzen 7 5700X held to
    // 2 cores / 4 threads 66.9 FPS against 64.3 spinning 250 us, 4 cores 81.8 against 79.9, 4 cores
    // / 8 threads even (82.1 both).
    static const auto SpinTime = [] {
        const char* v = std::getenv("SOTC_REC_SPIN_US");
        return std::chrono::microseconds{v ? std::strtol(v, nullptr, 10) : (few_cpus ? 0 : 250)};
    }();
    // sotc: SOTC_REC_SLEEP_US sets the idle wait. On Windows the default 500 us rounds to 0 and the
    // thread spins: 61.1 vs 57.7 FPS on the attract demo (i9-13900K + RTX 2060 SUPER) against a
    // 2 ms sleep (PR #81, jownatan), which frees the core on laptops and APUs (set 2000 there).
    // With 8 logical processors or fewer the default is that 2 ms sleep: the spinning recorder took
    // a whole core the game needs. Attract demo on a Ryzen 7 5700X held to 4 logical processors:
    // stuck loading while it spun, 77 FPS sleeping; at 8 and 16 the two were even.
    static const auto SleepTime = [] {
        const char* v = std::getenv("SOTC_REC_SLEEP_US");
        const auto sleep =
            std::chrono::microseconds{v ? std::strtol(v, nullptr, 10) : (few_cpus ? 2000 : 500)};
        LOG_WARNING(Render_Vulkan, "GPU recorder: {} logical processors, spin {} us, idle wait {} us",
                    logical, SpinTime.count(), sleep.count());
        return sleep;
    }();

    // sotc: N2-94 W1. Wake protocol, re-verified for the untimed wait (SOTC_REC_BLOCK):
    //  - Producer (PublishAll / Kick): stores committed (release), seq_cst fence, then
    //    WakeIfSleeping loads consumer_sleeping and exchanges it to false; only the winner calls
    //    WakeConsumer, which sets wake_flag under wake_mutex and notifies.
    //  - Consumer: stores consumer_sleeping = true, seq_cst fence, re-checks committed, then takes
    //    wake_mutex and waits on (wake_flag || stop). The predicate is evaluated under the mutex and
    //    WakeConsumer sets wake_flag under the same mutex, so a notify cannot fall between the
    //    re-check and the wait: either the re-check sees the new committed, or the producer sees
    //    sleeping and its wake_flag is visible to the predicate.
    //  - A stale wake_flag left from an earlier wake only makes one extra loop pass (harmless).
    //  - Stop: request_stop() on the jthread is caught by the stop_callback below, which takes
    //    wake_mutex and notifies, so the untimed wait returns (the predicate also checks the token).
    // Without the switch the timed wait_for (SleepTime) stays exactly as before.
    const bool block = Native::Switch::RecBlock();
    std::optional<std::stop_callback<std::function<void()>>> wake_on_stop;
    if (block) {
        wake_on_stop.emplace(stoken, std::function<void()>{[this] {
            {
                std::scoped_lock lk{wake_mutex};
                wake_flag = true;
            }
            wake_cv.notify_all();
        }});
    }

    Chunk* chunk = first_chunk;
    Chunk* previous = nullptr;
    u32 pos = 0;
    while (true) {
        u32 committed = chunk->committed.load(std::memory_order_acquire);
        if (pos == committed) {
            // When there is no work, spin briefly before sleeping until the producer kicks the
            // queue or the short timeout expires
            const auto spin_end = Clock::now() + SpinTime;
            while ((committed = chunk->committed.load(std::memory_order_acquire)) == pos &&
                   Clock::now() < spin_end && !stoken.stop_requested()) {
                RECORDER_PAUSE();
            }
            if (committed == pos) {
                if (stoken.stop_requested()) {
                    break;
                }
                consumer_sleeping.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (chunk->committed.load(std::memory_order_acquire) == pos) {
                    std::unique_lock lk{wake_mutex};
                    if (block) {
                        wake_cv.wait(lk, [&] { return wake_flag || stoken.stop_requested(); });
                    } else {
                        wake_cv.wait_for(lk, SleepTime,
                                         [&] { return wake_flag || stoken.stop_requested(); });
                    }
                    wake_flag = false;
                }
                consumer_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        const bool perf = Common::PerfStats::Enabled();
        // sotc: with SOTC_FPS=1 the busy time (replaying commands, not waiting for them) also
        // feeds the "SOTC threads per frame" line (SotcStats::recorder_busy_ns).
        const bool fps = SotcStats::kFpsLog;
        const auto busy_start = perf || fps ? Clock::now() : Clock::time_point{};
        while (pos < committed) {
            auto* header = reinterpret_cast<BlockHeader*>(chunk->data + pos);
            if (header->kind == BlockKind::End) {
                Chunk* next = chunk->next.load(std::memory_order_acquire);
                // Commands in next may still refer to data in chunk, so keep that chunk until the
                // consumer has moved beyond those commands
                if (previous != nullptr) {
                    RecycleChunk(previous);
                }
                previous = chunk;
                chunk = next;
                pos = 0;
                committed = chunk->committed.load(std::memory_order_acquire);
                continue;
            }
            if (header->kind == BlockKind::Command) {
                header->invoke(header + 1);
            }
            pos += header->size;
        }
        if (perf || fps) {
            const u64 busy_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - busy_start)
                    .count());
            if (perf) {
                Common::PerfStats::Add(Common::PerfStats::Id::RecorderBusyNs, busy_ns);
            }
            if (fps) {
                SotcStats::recorder_busy_ns.fetch_add(busy_ns, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace Vulkan
