// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: taken from Pink-shadPS4 0.4.0 (luizgustavs), cp_record_thread; scheduler hooks in vk_scheduler.cpp.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perf_stats.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "bbport_threads.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_parallel_record.h" // sotc: SOTC_N5_PARALLEL counters

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define RECORDER_PAUSE() _mm_pause()
#else
#define RECORDER_PAUSE()
#endif

namespace Vulkan {

/// sotc: payload of a BeginRendering block. The copied VkRenderingInfo points at the attachments
/// stored right after this struct (the colors, then depth and stencil when present), so the block is
/// self-contained and stays valid while its chunk is pinned.
struct BeginPayload {
    VkCommandBuffer cmdbuf;
    VkRenderingInfo info;
    PassFormats formats;
};

namespace {

using Dispatch = std::remove_cvref_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>;
using BlockCategory = CommandRecorder::BlockCategory;
// sotc: the SOTC_CMD_STATS counters in sotc_stats.h hold one slot per category
static_assert(static_cast<size_t>(BlockCategory::Count) <= 16);

constexpr u32 NumFakeHandles = CommandRecorder::NumFakeHandles;
alignas(64) u8 g_fake_handles[NumFakeHandles];
CommandRecorder* g_recorder{};

// sotc: on the pass worker thread, the secondary command buffer it is recording stands for the
// fake handle of the pass (CommandRecorder::Real); both are null on every other thread
thread_local VkCommandBuffer tls_pass_fake{};
thread_local VkCommandBuffer tls_pass_real{};

// sotc: SOTC_PASS_SEC=1 turns on the secondary command buffers of the dynamic rendering passes
bool PassSecEnabled() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_PASS_SEC");
        return v && v[0] == '1';
    }();
    return value;
}

// sotc: SOTC_PASS_SEC_MIN: passes with more draws than this go to a secondary command buffer
u32 PassSecMin() {
    static const u32 value = [] {
        const char* v = std::getenv("SOTC_PASS_SEC_MIN");
        const long n = v ? std::strtol(v, nullptr, 10) : 64;
        return n > 0 ? static_cast<u32>(n) : 64U;
    }();
    return value;
}

// sotc: a distinct tag per template instance, for the StateKey of the state blocks it records. The
// anchor is writable so the linker cannot merge two of them.
template <auto Member>
u32 TagOf() {
    static u8 anchor;
    return static_cast<u32>(reinterpret_cast<uintptr_t>(&anchor));
}

// sotc: the state key of a generic command: its tag, and the first argument when that selects
// the state (vkCmdBindPipeline's bind point, the stencil face mask)
template <typename... A>
StateKey GenericKey(u32 tag, bool keyed, const A&... args) {
    StateKey key{tag, 0, 0, 0};
    if constexpr (sizeof...(A) > 0) {
        using First = std::tuple_element_t<0, std::tuple<A...>>;
        if constexpr (std::is_integral_v<First> || std::is_enum_v<First>) {
            if (keyed) {
                key.a = static_cast<u32>(std::get<0>(std::forward_as_tuple(args...)));
            }
        }
    }
    return key;
}

StateKey KeyOf(u32 tag, u32 a = 0, u32 b = 0, u32 c = 0) {
    return StateKey{tag, a, b, c};
}

bool KeyedFirst(std::string_view name) {
    return name == "vkCmdBindPipeline" || name == "vkCmdSetStencilOp" ||
           name == "vkCmdSetStencilOpEXT" || name == "vkCmdSetStencilReference" ||
           name == "vkCmdSetStencilWriteMask" || name == "vkCmdSetStencilCompareMask";
}

// sotc: SOTC_WAIT_NOSPIN=1: the consumer spins at most ~20 us, then blocks on the wake_seq doorbell
// instead of the timed wait. Off by default.
bool WaitNoSpin() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_WAIT_NOSPIN");
        return v && v[0] == '1';
    }();
    return value;
}

// sotc: SOTC_FUTEX_WAIT=1 (Windows only, off by default): the consumer's idle wait (on wake_seq),
// Sync (sync_done) and PassSecondary::WaitJob (done_gen) sleep in WaitOnAddress, and the producer
// side wakes them after it stores the word they wait on. The sleep compares the value the waiter
// read, so a store that came first is seen at once: no wake is lost. The timeout is a safety net,
// and timed-out sleeps whose condition already held are counted (safety_met).
bool FutexWaitOn() {
#ifdef _WIN32
    static const bool value = [] {
        const char* v = std::getenv("SOTC_FUTEX_WAIT");
        return v && v[0] == '1';
    }();
    return value;
#else
    return false;
#endif
}

constexpr u32 kFutexSpin = 128;
constexpr u32 kFutexTimeoutMs = 16;

// Sleeps while word == observed, at most kFutexTimeoutMs. Returns false on timeout.
template <typename T>
bool FutexSleepRec(std::atomic<T>& word, T observed) {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    SotcFutexRecCounters& c = g_futex_rec;
    const auto t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
    const bool woken = WaitOnAddress(&word, &observed, sizeof(T), kFutexTimeoutMs) != FALSE;
#else
    const bool woken = true; // not reached: SOTC_FUTEX_WAIT is Windows-only
#endif
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    c.sleeps.fetch_add(1, std::memory_order_relaxed);
    c.sleep_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 seen = c.sleep_max_ns.load(std::memory_order_relaxed);
    while (ns > seen && !c.sleep_max_ns.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
    return woken;
}

// Wakes one (or every) thread sleeping in FutexSleepRec on word.
template <typename T>
void FutexWakeRec(std::atomic<T>& word, bool all) {
    g_futex_rec.wakes.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    if (all) {
        WakeByAddressAll(&word);
    } else {
        WakeByAddressSingle(&word);
    }
#else
    (void)word;
    (void)all;
#endif
}

// Waits until word >= target: a short spin, then sleeps (the publisher stores, then wakes).
void FutexWaitAtLeast(std::atomic<u64>& word, u64 target) {
    SotcFutexRecCounters& c = g_futex_rec;
    c.waits.fetch_add(1, std::memory_order_relaxed);
    u64 seen = word.load(std::memory_order_acquire);
    for (u32 spin = 0; seen < target && spin < kFutexSpin; ++spin) {
        RECORDER_PAUSE();
        seen = word.load(std::memory_order_acquire);
    }
    if (seen >= target) {
        c.spins.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    while ((seen = word.load(std::memory_order_seq_cst)) < target) {
        if (!FutexSleepRec(word, seen) && word.load(std::memory_order_seq_cst) >= target) {
            c.safety_met.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

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

/// Category of a generic command, from the prefix of its Vulkan name; other commands are Other
constexpr BlockCategory CategoryOfName(std::string_view name) {
    const auto starts_with = [name](std::string_view prefix) {
        return name.substr(0, prefix.size()) == prefix;
    };
    if (starts_with("vkCmdDispatch")) {
        return BlockCategory::Dispatch;
    }
    if (starts_with("vkCmdDraw")) {
        return BlockCategory::Draw;
    }
    if (starts_with("vkCmdBeginRendering") || starts_with("vkCmdBeginRenderPass")) {
        return BlockCategory::BeginRendering;
    }
    if (starts_with("vkCmdEndRendering") || starts_with("vkCmdEndRenderPass")) {
        return BlockCategory::EndRendering;
    }
    if (starts_with("vkCmdPipelineBarrier")) {
        return BlockCategory::Barrier;
    }
    if (starts_with("vkCmdCopy") || starts_with("vkCmdBlit") || starts_with("vkCmdClear") ||
        starts_with("vkCmdFill") || starts_with("vkCmdUpdate") || starts_with("vkCmdResolve")) {
        return BlockCategory::Copy;
    }
    if (starts_with("vkCmdBindPipeline")) {
        return BlockCategory::BindPipeline;
    }
    // Descriptor buffer offsets use a vkCmdSet name but bind descriptors, so this check comes
    // before the vkCmdSet one
    if (starts_with("vkCmdPushDescriptorSet") || starts_with("vkCmdBindDescriptorSets") ||
        starts_with("vkCmdBindDescriptorBuffer") || starts_with("vkCmdSetDescriptorBufferOffsets") ||
        starts_with("vkCmdBindSamplerHeap") || starts_with("vkCmdBindResourceHeap")) {
        return BlockCategory::BindDescriptors;
    }
    if (starts_with("vkCmdPushConstants")) {
        return BlockCategory::PushConstants;
    }
    if (starts_with("vkCmdSet")) {
        return BlockCategory::DynamicState;
    }
    if (starts_with("vkCmdBindVertexBuffers") || starts_with("vkCmdBindIndexBuffer")) {
        return BlockCategory::VertexIndex;
    }
    // sotc: labels are valid inside a secondary command buffer, so they do not end a pass
    if (starts_with("vkCmdBeginDebugUtilsLabel") || starts_with("vkCmdEndDebugUtilsLabel") ||
        starts_with("vkCmdInsertDebugUtilsLabel")) {
        return BlockCategory::Label;
    }
    return BlockCategory::Other;
}

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
    static inline BlockCategory category{};
    static inline bool keyed{}; ///< sotc: the first argument selects part of the state (GenericKey)

    static R VKAPI_CALL Call(VkCommandBuffer cmdbuf, Args... args) {
        if (!CommandRecorder::IsFake(cmdbuf)) {
            return real(cmdbuf, args...);
        }
        // Commands returning a result, such as VK_INTEL_performance_query calls, need an immediate
        // driver result and cannot be queued here
        if constexpr (!std::is_void_v<R> || (IsInputPointer<Args>::value || ...)) {
            FailUncovered(name);
        } else {
            g_recorder->CountCommand();
            auto closure = [cmdbuf, args...] { real(g_recorder->Real(cmdbuf), args...); };
            if (CommandRecorder::IsStateCategory(category)) {
                // sotc: state blocks carry their key so a secondary command buffer can replay the
                // latest of each
                g_recorder->RunState(category, cmdbuf,
                                     GenericKey(TagOf<Member>(), keyed, args...), closure);
            } else {
                g_recorder->Run(category, closure);
            }
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
    Thunk::category = CategoryOfName(name);
    Thunk::keyed = KeyedFirst(name);
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
    auto& rec = *g_recorder;
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
    g_recorder->CountCommand();
    rec.Run(BlockCategory::Barrier,
            [cmdbuf, copy] { RealOf<Member>()(g_recorder->Real(cmdbuf), copy); });
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
    auto& rec = *g_recorder;
    rec.Reserve(num_memory * sizeof(VkMemoryBarrier) + num_buffer * sizeof(VkBufferMemoryBarrier) +
                num_image * sizeof(VkImageMemoryBarrier) + 256);
    const auto* memory_copy = rec.Copy(memory, num_memory);
    const auto* buffer_copy = rec.Copy(buffer, num_buffer);
    const auto* image_copy = rec.Copy(image, num_image);
    g_recorder->CountCommand();
    rec.Run(BlockCategory::Barrier, [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), src_stage, dst_stage, flags, num_memory,
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
    RealOf<Member>()(g_recorder->Real(push->cmdbuf), push->bind_point, push->layout, push->set,
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
    auto& rec = *g_recorder;
    auto* push = static_cast<PushDescriptorPayload*>(
        rec.AllocateCommand(bytes, BlockCategory::BindDescriptors,
                            &InvokePushDescriptorSet<Member>, cmdbuf,
                            KeyOf(TagOf<Member>(), static_cast<u32>(bind_point), set)));
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
    auto& rec = *g_recorder;
    rec.Reserve(size + 256);
    const auto* copy = rec.Copy(static_cast<const u8*>(values), size);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::PushConstants, cmdbuf,
                 KeyOf(TagOf<Member>(), offset, size, static_cast<u32>(stages)), [=] {
                     RealOf<Member>()(g_recorder->Real(cmdbuf), layout, stages, offset, size,
                                      copy);
                 });
}

// sotc: SOTC_DESCRIPTOR_HEAP. vkCmdBind{Resource,Sampler}HeapEXT take their range by pointer: the info is copied
// into the stream. A bind is descriptor state, one key per heap kind (the command's tag).
template <auto Member>
void VKAPI_CALL CmdBindHeap(VkCommandBuffer cmdbuf, const VkBindHeapInfoEXT* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, info);
    }
    auto& rec = *g_recorder;
    rec.Reserve(sizeof(VkBindHeapInfoEXT) + 256);
    CheckNoChain(info->pNext, "VkBindHeapInfoEXT");
    const auto* copy = rec.Copy(info);
    rec.CountCommand();
    rec.RunState(BlockCategory::BindDescriptors, cmdbuf, KeyOf(TagOf<Member>()),
                 [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), copy); });
}

// sotc: SOTC_DESCRIPTOR_HEAP. vkCmdPushDataEXT carries a host pointer to the pushed bytes: the info and the bytes
// are copied. Push data is push-constant state, keyed by its byte range as vkCmdPushConstants is.
template <auto Member>
void VKAPI_CALL CmdPushData(VkCommandBuffer cmdbuf, const VkPushDataInfoEXT* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, info);
    }
    auto& rec = *g_recorder;
    const size_t size = info->data.size;
    rec.Reserve(sizeof(VkPushDataInfoEXT) + size + 256);
    CheckNoChain(info->pNext, "VkPushDataInfoEXT");
    auto* copy = rec.Copy(info);
    copy->data.address = rec.Copy(static_cast<const u8*>(info->data.address), size);
    rec.CountCommand();
    rec.RunState(BlockCategory::PushConstants, cmdbuf,
                 KeyOf(TagOf<Member>(), info->offset, static_cast<u32>(size)),
                 [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), copy); });
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
    const auto* copy = CopyLabel(*g_recorder, label);
    g_recorder->CountCommand();
    g_recorder->Run(BlockCategory::Label,
                    [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), copy); });
}

template <auto Member>
void InvokeBeginRendering(void* payload) {
    auto* begin = static_cast<BeginPayload*>(payload);
    RealOf<Member>()(g_recorder->Real(begin->cmdbuf), &begin->info);
}

template <auto Member>
void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cmdbuf, const VkRenderingInfo* info) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, info);
    }
    CheckNoChain(info->pNext, "VkRenderingInfo");
    const u32 num_colors = info->colorAttachmentCount;
    for (u32 i = 0; i < num_colors; ++i) {
        CheckNoChain(info->pColorAttachments[i].pNext, "VkRenderingAttachmentInfo");
    }
    size_t bytes = sizeof(BeginPayload) + num_colors * sizeof(VkRenderingAttachmentInfo);
    if (info->pDepthAttachment) {
        bytes += sizeof(VkRenderingAttachmentInfo);
    }
    if (info->pStencilAttachment) {
        bytes += sizeof(VkRenderingAttachmentInfo);
    }
    auto& rec = *g_recorder;
    auto* begin = static_cast<BeginPayload*>(rec.AllocateCommand(
        bytes, BlockCategory::BeginRendering, &InvokeBeginRendering<Member>, nullptr, {},
        CommandRecorder::kBeginPassAux));
    begin->cmdbuf = cmdbuf;
    begin->info = *info;
    begin->formats = rec.TakePassFormats();
    auto* tail = reinterpret_cast<VkRenderingAttachmentInfo*>(begin + 1);
    begin->info.pColorAttachments = num_colors ? tail : nullptr;
    if (num_colors) {
        std::memcpy(tail, info->pColorAttachments, num_colors * sizeof(*tail));
    }
    tail += num_colors;
    begin->info.pDepthAttachment = nullptr;
    if (info->pDepthAttachment) {
        *tail = *info->pDepthAttachment;
        begin->info.pDepthAttachment = tail++;
    }
    begin->info.pStencilAttachment = nullptr;
    if (info->pStencilAttachment) {
        *tail = *info->pStencilAttachment;
        begin->info.pStencilAttachment = tail;
    }
    g_recorder->CountCommand();
    rec.Commit();
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
        auto& rec = *g_recorder;
        rec.Reserve(count * sizeof(Region) + 256);
        const auto* copy = rec.Copy(regions, count);
        g_recorder->CountCommand();
        rec.Run(BlockCategory::Copy,
                [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), pre..., count, copy); });
    }
};

template <auto Member>
void VKAPI_CALL CmdBlitImage(VkCommandBuffer cmdbuf, VkImage src, VkImageLayout src_layout,
                             VkImage dst, VkImageLayout dst_layout, u32 count,
                             const VkImageBlit* regions, VkFilter filter) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, src, src_layout, dst, dst_layout, count, regions, filter);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * sizeof(VkImageBlit) + 256);
    const auto* copy = rec.Copy(regions, count);
    g_recorder->CountCommand();
    rec.Run(BlockCategory::Copy, [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), src, src_layout, dst, dst_layout, count, copy,
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
    auto& rec = *g_recorder;
    rec.Reserve(sizeof(Value) + count * sizeof(VkImageSubresourceRange) + 256);
    const auto* value_copy = rec.Copy(value);
    const auto* ranges_copy = rec.Copy(ranges, count);
    g_recorder->CountCommand();
    rec.Run(BlockCategory::Copy, [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), image, layout, value_copy, count, ranges_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cmdbuf, VkBuffer buffer, VkDeviceSize offset,
                                VkDeviceSize size, const void* data) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, buffer, offset, size, data);
    }
    auto& rec = *g_recorder;
    rec.Reserve(size + 256);
    const auto* copy = rec.Copy(static_cast<const u8*>(data), size);
    g_recorder->CountCommand();
    rec.Run(BlockCategory::Copy,
            [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), buffer, offset, size, copy); });
}

/// Copy viewport and scissor arrays for both the first-and-count commands and the WithCount
/// variants before queuing them
template <auto Member, typename Element>
void VKAPI_CALL CmdSetFirstArray(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                 const Element* elements) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, elements);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * sizeof(Element) + 256);
    const auto* copy = rec.Copy(elements, count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::DynamicState, cmdbuf, KeyOf(TagOf<Member>(), first, count),
                 [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), first, count, copy); });
}

template <auto Member, typename Element>
void VKAPI_CALL CmdSetArray(VkCommandBuffer cmdbuf, u32 count, const Element* elements) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, count, elements);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * sizeof(Element) + 256);
    const auto* copy = rec.Copy(elements, count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::DynamicState, cmdbuf, KeyOf(TagOf<Member>(), count),
                 [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), count, copy); });
}

template <auto Member>
void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cmdbuf, const float constants[4]) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, constants);
    }
    std::array<float, 4> copy;
    std::memcpy(copy.data(), constants, sizeof(copy));
    g_recorder->CountCommand();
    g_recorder->RunState(BlockCategory::DynamicState, cmdbuf, KeyOf(TagOf<Member>()),
                         [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), copy.data()); });
}

template <auto Member>
void VKAPI_CALL CmdSetVertexInput(VkCommandBuffer cmdbuf, u32 num_bindings,
                                  const VkVertexInputBindingDescription2EXT* bindings,
                                  u32 num_attributes,
                                  const VkVertexInputAttributeDescription2EXT* attributes) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, num_bindings, bindings, num_attributes, attributes);
    }
    auto& rec = *g_recorder;
    rec.Reserve(num_bindings * sizeof(*bindings) + num_attributes * sizeof(*attributes) + 256);
    const auto* bindings_copy = rec.Copy(bindings, num_bindings);
    const auto* attributes_copy = rec.Copy(attributes, num_attributes);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::DynamicState, cmdbuf, KeyOf(TagOf<Member>()), [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), num_bindings, bindings_copy, num_attributes,
                         attributes_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                     const VkBuffer* buffers, const VkDeviceSize* offsets) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, buffers, offsets);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * (sizeof(VkBuffer) + sizeof(VkDeviceSize)) + 256);
    const auto* buffers_copy = rec.Copy(buffers, count);
    const auto* offsets_copy = rec.Copy(offsets, count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::VertexIndex, cmdbuf, KeyOf(TagOf<Member>(), first, count), [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), first, count, buffers_copy, offsets_copy);
    });
}

template <auto Member>
void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cmdbuf, u32 first, u32 count,
                                      const VkBuffer* buffers, const VkDeviceSize* offsets,
                                      const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, first, count, buffers, offsets, sizes, strides);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * (sizeof(VkBuffer) + 3 * sizeof(VkDeviceSize)) + 256);
    const auto* buffers_copy = rec.Copy(buffers, count);
    const auto* offsets_copy = rec.Copy(offsets, count);
    const auto* sizes_copy = rec.Copy(sizes, count);
    const auto* strides_copy = rec.Copy(strides, count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::VertexIndex, cmdbuf, KeyOf(TagOf<Member>(), first, count), [=] {
        RealOf<Member>()(g_recorder->Real(cmdbuf), first, count, buffers_copy, offsets_copy,
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
    auto& rec = *g_recorder;
    rec.Reserve(count * sizeof(VkDescriptorSet) + num_dynamic * sizeof(u32) + 256);
    const auto* sets_copy = rec.Copy(sets, count);
    const auto* offsets_copy = rec.Copy(dynamic_offsets, num_dynamic);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::BindDescriptors, cmdbuf,
                 KeyOf(TagOf<Member>(), static_cast<u32>(bind_point), first, count), [=] {
                     RealOf<Member>()(g_recorder->Real(cmdbuf), bind_point, layout, first, count,
                                      sets_copy, num_dynamic, offsets_copy);
                 });
}

// sotc: descriptor buffer bindings and offsets (SOTC_DESC_BUFFER=1) have pointer arguments, so
// their arrays are copied into the stream like vkCmdBindDescriptorSets does.
template <auto Member>
void VKAPI_CALL CmdBindDescriptorBuffers(VkCommandBuffer cmdbuf, u32 count,
                                         const VkDescriptorBufferBindingInfoEXT* infos) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, count, infos);
    }
    auto& rec = *g_recorder;
    rec.Reserve(count * sizeof(VkDescriptorBufferBindingInfoEXT) + 256);
    for (u32 i = 0; i < count; ++i) {
        CheckNoChain(infos[i].pNext, "VkDescriptorBufferBindingInfoEXT");
    }
    const auto* infos_copy = rec.Copy(infos, count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::BindDescriptors, cmdbuf, KeyOf(TagOf<Member>(), count),
                 [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), count, infos_copy); });
}

template <auto Member>
void VKAPI_CALL CmdSetDescriptorBufferOffsets(VkCommandBuffer cmdbuf,
                                              VkPipelineBindPoint bind_point,
                                              VkPipelineLayout layout, u32 first_set,
                                              u32 set_count, const u32* buffer_indices,
                                              const VkDeviceSize* offsets) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, bind_point, layout, first_set, set_count, buffer_indices,
                                offsets);
    }
    auto& rec = *g_recorder;
    rec.Reserve(set_count * (sizeof(u32) + sizeof(VkDeviceSize)) + 256);
    const auto* indices_copy = rec.Copy(buffer_indices, set_count);
    const auto* offsets_copy = rec.Copy(offsets, set_count);
    g_recorder->CountCommand();
    rec.RunState(BlockCategory::BindDescriptors, cmdbuf,
                 KeyOf(TagOf<Member>(), static_cast<u32>(bind_point), first_set, set_count), [=] {
                     RealOf<Member>()(g_recorder->Real(cmdbuf), bind_point, layout, first_set,
                                      set_count, indices_copy, offsets_copy);
                 });
}

/// Keep the opaque marker value unchanged so the driver can return it after device loss, without
/// treating it as a pointer to dereference
template <auto Member>
void VKAPI_CALL CmdSetCheckpoint(VkCommandBuffer cmdbuf, const void* marker) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return RealOf<Member>()(cmdbuf, marker);
    }
    g_recorder->CountCommand();
    g_recorder->Run(BlockCategory::DynamicState,
                    [=] { RealOf<Member>()(g_recorder->Real(cmdbuf), marker); });
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
    g_recorder->Run(BlockCategory::Other, [cmdbuf] {
        const VkResult result = real_end(g_recorder->Real(cmdbuf));
        ASSERT_MSG(result == VK_SUCCESS, "cp_record_thread: vkEndCommandBuffer failed: {}",
                   vk::to_string(vk::Result{result}));
    });
    return VK_SUCCESS;
}

VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cmdbuf, VkCommandBufferResetFlags flags) {
    if (!CommandRecorder::IsFake(cmdbuf)) {
        return real_reset(cmdbuf, flags);
    }
    g_recorder->Run(BlockCategory::Other,
                    [cmdbuf, flags] { real_reset(g_recorder->Real(cmdbuf), flags); });
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
        OVERRIDE(vkCmdBindResourceHeapEXT, CmdBindHeap<&Dispatch::vkCmdBindResourceHeapEXT>);
        OVERRIDE(vkCmdBindSamplerHeapEXT, CmdBindHeap<&Dispatch::vkCmdBindSamplerHeapEXT>);
        OVERRIDE(vkCmdPushDataEXT, CmdPushData<&Dispatch::vkCmdPushDataEXT>);
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
        OVERRIDE(vkCmdBindDescriptorBuffersEXT,
                 CmdBindDescriptorBuffers<&Dispatch::vkCmdBindDescriptorBuffersEXT>);
        OVERRIDE(vkCmdSetDescriptorBufferOffsetsEXT,
                 CmdSetDescriptorBufferOffsets<&Dispatch::vkCmdSetDescriptorBufferOffsetsEXT>);
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

// sotc: SOTC_PASS_SEC. The consumer finds a dynamic rendering pass (BeginRendering .. EndRendering)
// with more than SOTC_PASS_SEC_MIN draws and no block a secondary cannot hold. The pass is recorded
// into a secondary command buffer by a worker thread while the consumer goes on with the blocks
// before it. The primary begins the pass with secondary contents and executes the secondary at the
// EndRendering, after the worker finished it.
//
// Several passes can be open at once (SOTC_PASS_WORKERS=N, ParallelRecord::Workers): open is the list
// of passes handed to the workers, in stream order, at most N. The consumer still walks the stream in
// order and is the only thread that touches the primary: it begins each pass, executes its secondary
// and ends it, and it runs barriers, copies and everything else itself, in order.
//
// The look-ahead (LookAhead) is what lets passes overlap. It walks the published part of the stream
// beyond the consumer position and prepares the next passes, so their workers start before the
// consumer reaches them. It cannot read the consumer's replay table at a later position, so it keeps
// a copy of that table (la_table) and advances the copy over the same blocks. It stops, and resumes
// later, where the copy cannot follow: at a block of category Other (BeginRecorded, submits and the
// other closures that can call Bind or PassSubmitted, which change the table), at an insert that
// would prune, at a pass whose end is not published yet, and when the open passes or the chunk ring
// are full. Visit drops the copy when the walk reaches its cursor; the next LookAhead rebuilds it.
//
// A secondary inherits no state, so the worker first replays the latest block of each state key
// (the replay table, one entry per command buffer and key). Those blocks are not copied: a table
// entry pins the chunk of its block and the chunk before it (its data comes before the command), so
// the block stays valid in place until the entry is replaced or dropped. Each chunk has two pin
// counts: table_pins, touched by the consumer only (la_table holds them too), and job_pins, atomic,
// released by the worker when a job is recorded. A chunk with a pin left is deferred
// (CommandRecorder::ReleaseChunk) instead of recycled.
//
// The table only holds command buffers that are still open: an entry goes when its command buffer is
// submitted (Submitted) or begins again (Forget). If the table fills up, Prune drops the entries of
// the other command buffers and marks them dropped; a pass on a dropped command buffer stays on the
// primary, so a partial replay never reaches the GPU.
//
// Executing a secondary leaves the primary's bound state undefined, so the table is replayed on the
// primary after each secondary pass too.
//
// Secondary command buffers come from one pool per worker. A secondary is held while its job is
// recorded and until the submit that executes it completes (its tick), so a pool never hands out a
// buffer the GPU still reads. Each pool has its own mutex; the consumer releases into it.
//
// SOTC_PASS_VERIFY=1 (ParallelRecord::Verify): every job is checked. At the start of the pass the
// consumer compares the job's state snapshot with its own replay table (same entries, same order),
// and the worker hashes each block it executes (identity, category, size, invoke, payload bytes),
// which the consumer compared with a reference hash taken before the job was handed out. The
// parallel result is always the one used; the checks only count mismatches.
class PassSecondary {
public:
    PassSecondary(CommandRecorder& rec, const Instance& instance, Semaphore* work_semaphore,
                  u32 min_draws);
    ~PassSecondary();

    PassSecondary(const PassSecondary&) = delete;
    PassSecondary& operator=(const PassSecondary&) = delete;

    /// Consumer thread. True when the block must not run on the primary: the block that starts a
    /// secondary pass (already run with secondary contents), the blocks the pass records, and the
    /// EndRendering that closes it (run here, after the secondary). False for every other block; a
    /// state block among those (meta != nullptr) is kept in the replay table.
    bool Handle(CommandRecorder::Chunk* chunk, u32 pos, CommandRecorder::Chunk* previous,
                CommandRecorder::BlockHeader* header, const CommandRecorder::StateMeta* meta,
                std::stop_token stop);

    /// Consumer thread, for every block the walk reaches. When the walk reaches the cursor of the
    /// look-ahead, its copy of the table is dropped (the next LookAhead rebuilds it).
    void Visit(CommandRecorder::Chunk* chunk, u32 pos);

    /// Consumer thread: the replay state of fake is no longer needed (its session was submitted, or
    /// it begins again)
    void Forget(VkCommandBuffer fake);

    /// Consumer thread: the submit of fakes completes at signal_tick. Their state goes, and the
    /// secondaries executed in them are released for reuse once that tick is reached.
    void Submitted(std::span<const vk::CommandBuffer> fakes, u64 signal_tick);

private:
    using Chunk = CommandRecorder::Chunk;
    using BlockHeader = CommandRecorder::BlockHeader;
    using BlockKind = CommandRecorder::BlockKind;
    using BlockCategory = CommandRecorder::BlockCategory;
    using StateMeta = CommandRecorder::StateMeta;

    static constexpr size_t kMaxEntries = 1024; ///< table size at which Prune drops entries
    static constexpr size_t kMaxScanChunks = 8; ///< a pass spanning more chunks stays on the primary
    static constexpr auto kMaxScanWait = std::chrono::milliseconds{5};
    /// sotc: the look-ahead and a new pass stop when the chunk ring is this close to full
    static constexpr u32 kRingHeadroom = 32;
    static constexpr u64 kHashSeed = 0xcbf29ce484222325ull;

    struct TableKey {
        VkCommandBuffer cmd{};
        StateKey key{};

        bool operator==(const TableKey& other) const noexcept = default;
    };

    struct TableKeyHash {
        size_t operator()(const TableKey& k) const noexcept {
            u64 h = reinterpret_cast<uintptr_t>(k.cmd);
            h = (h * 0x9E3779B97F4A7C15ull) ^ ((u64{k.key.tag} << 32) | k.key.a);
            h = (h * 0x9E3779B97F4A7C15ull) ^ ((u64{k.key.b} << 32) | k.key.c);
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    struct StateEntry {
        BlockHeader* header;
        const StateMeta* meta;
        Chunk* chunk;
        Chunk* prev;
        u64 seq;
    };

    using StateTable = std::unordered_map<TableKey, StateEntry, TableKeyHash>;

    struct Scan {
        bool found{};
        bool bad{}; ///< a block the secondary cannot hold, or the scan gave up
        bool incomplete{}; ///< sotc: non-blocking scan: the producer has not published the end yet
        // sotc: SOTC_N5_PARALLEL: why the scan gave up, and the blocking category (ScanBlock)
        ParallelRecord::Fallback reason{ParallelRecord::Fallback::ScanBlock};
        u8 block{0xFF};
        u32 draws{};
        u32 blocks{}; ///< sotc: N5 mode 2, the command blocks of the pass (state and draws)
        u32 descriptors{}; ///< sotc: SOTC_PASS_RECORD packet shape: descriptor binds in the pass
        Chunk* end_chunk{};
        u32 end_pos{};
        std::vector<Chunk*> chunks; ///< the chunks the pass spans, the first one holding BeginRendering
    };

    /// sotc: what the consumer or the look-ahead decided for a BeginRendering that starts a pass
    struct Decision {
        enum class Kind : u8 { Parallel, Inline, Incomplete, Stop, Dry };
        Kind kind{Kind::Inline};
        ParallelRecord::Fallback reason{ParallelRecord::Fallback::ScanBlock};
        u8 block{0xFF};
        Scan scan;
    };

    struct Job {
        VkCommandBuffer fake{};
        PassFormats formats{};
        Chunk* block_chunk{}; ///< the BeginRendering block, to find the job when the walk reaches it
        u32 block_pos{};
        Chunk* begin_chunk{}; ///< first block after the BeginRendering
        u32 begin_pos{};
        Chunk* end_chunk{}; ///< the EndRendering
        u32 end_pos{};
        std::vector<StateEntry> state; ///< replayed before the pass blocks, in sequence order
        std::vector<Chunk*> pinned;    ///< job_pins held, released by the worker
        u64 seq{};
        u32 worker{};             ///< the worker that recorded the job
        bool started{};           ///< consumer: the BeginRendering ran on the primary
        bool state_ok{true};      ///< verify: the snapshot equals the consumer table at the start
        u64 ref_hash{};           ///< verify: reference hash of the blocks, taken by the consumer
        u64 run_hash{};           ///< verify: hash of the blocks the worker executed
        std::atomic<bool> done{}; ///< set by the worker after its last access to the job
        VkCommandBuffer secondary{};
    };

    /// A secondary executed on the primary of fake, held until that primary is submitted
    struct Awaiting {
        VkCommandBuffer fake;
        VkCommandBuffer secondary;
        u32 worker;
    };

    struct Worker {
        std::unique_ptr<CommandPool> pool; ///< secondary command buffers of this worker
        std::mutex pool_mutex;
        std::thread thread;
    };

    Decision Decide(Chunk* chunk, u32 pos, BlockHeader* header, std::stop_token stop, bool walk);
    bool TryStart(Chunk* chunk, u32 pos, Chunk* previous, BlockHeader* header,
                  std::stop_token stop);
    bool ScanPass(Chunk* chunk, u32 pos, std::stop_token stop, Scan& scan, bool wait);
    Job* MakeJob(Chunk* chunk, u32 pos, BlockHeader* header, const Scan& scan,
                 const StateTable& src);
    void StartJob(Job* job, BlockHeader* header);
    void Enqueue(Job* job);
    void LookAhead(Chunk* chunk, u32 pos, Chunk* previous);
    void RebuildLookAhead(Chunk* chunk, u32 pos, Chunk* previous);
    void ClearLookAhead();
    bool LookAheadInsert(Chunk* chunk, Chunk* prev, BlockHeader* header, const StateMeta* meta);
    void Insert(Chunk* chunk, Chunk* previous, BlockHeader* header, const StateMeta* meta);
    void Prune(VkCommandBuffer keep);
    void ClearTable();
    void Finish(Chunk* chunk, u32 pos, Chunk* previous, BlockHeader* end);
    void Replay(VkCommandBuffer fake);
    void WaitJob(const Job* job);
    void WorkerLoop(u32 index);
    void Record(Job* job, u32 index);
    /// Calls fn(header) for the state blocks of the job, then for the pass blocks, in order
    template <typename Fn>
    static void ForEachBlock(const Job* job, Fn&& fn);
    static void MixBlock(u64& hash, const BlockHeader* header);
    static void Pin(Chunk* chunk, Chunk* prev);
    static void Unpin(Chunk* chunk, Chunk* prev);
    static void PinJob(Job* job, Chunk* chunk);

    CommandRecorder& rec;
    u32 min_draws;
    u32 workers_n; ///< sotc: SOTC_PASS_WORKERS: worker threads, and passes open at once
    bool verify_on;
    std::vector<std::unique_ptr<Worker>> workers;
    /// Jobs handed to the workers and not yet recorded, in stream order (guarded by mutex)
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job*> queue;
    bool stopping{};
    /// Counts every finished recording; WaitJob waits on it, and each job's done flag says which
    std::atomic<u64> done_gen{0};

    // Consumer thread: the replay table and the passes in flight
    StateTable table;
    /// command buffers Prune took entries from; their passes stay on the primary
    std::unordered_set<VkCommandBuffer> dropped;
    std::vector<Awaiting> awaiting; ///< secondaries waiting for their submit
    /// Passes handed out and not finished, in stream order. The front one is active when the walk is
    /// inside a pass; every job after it was prepared by the look-ahead and not started yet.
    std::deque<Job*> open;
    Job* active{}; ///< the pass the walk is inside (started, not finished)
    u64 seq{};
    u64 job_seq{};

    // Consumer thread: the look-ahead. Its copy la_table is the replay table at la_pos (the next block
    // to process), with the same pins as table; la_prev is the chunk before la_chunk.
    bool la_valid{};
    Chunk* la_chunk{};
    Chunk* la_prev{};
    u32 la_pos{};
    u64 la_seq{};
    StateTable la_table;
    bool la_incomplete{}; ///< the last LookAhead stopped at unpublished blocks
    u32 la_retry{};       ///< blocks walked inside a pass, to retry an incomplete look-ahead now and then
};

template <typename Fn>
void PassSecondary::ForEachBlock(const Job* job, Fn&& fn) {
    for (const StateEntry& entry : job->state) {
        fn(entry.header);
    }
    Chunk* c = job->begin_chunk;
    u32 p = job->begin_pos;
    while (c != job->end_chunk || p != job->end_pos) {
        auto* h = reinterpret_cast<BlockHeader*>(c->data + p);
        if (h->kind == BlockKind::End) {
            c = c->next.load(std::memory_order_acquire);
            p = 0;
            continue;
        }
        if (h->kind == BlockKind::Command) {
            fn(h);
        }
        p += h->size;
    }
}

void PassSecondary::MixBlock(u64& hash, const BlockHeader* header) {
    // FNV-1a over the block identity, its header fields and its payload bytes
    const auto mix = [&hash](u64 v) { hash = (hash ^ v) * 0x100000001B3ull; };
    mix(reinterpret_cast<uintptr_t>(header));
    mix(u64{header->size} | (u64{static_cast<u8>(header->kind)} << 32) |
        (u64{static_cast<u8>(header->category)} << 40) | (u64{header->aux} << 48));
    mix(reinterpret_cast<uintptr_t>(header->invoke));
    const u8* payload = reinterpret_cast<const u8*>(header + 1);
    const size_t bytes = header->size - sizeof(BlockHeader);
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ payload[i]) * 0x100000001B3ull;
    }
}

PassSecondary::PassSecondary(CommandRecorder& rec_, const Instance& instance,
                             Semaphore* work_semaphore, u32 min_draws_)
    : rec{rec_}, min_draws{min_draws_}, workers_n{static_cast<u32>(ParallelRecord::Workers())},
      verify_on{ParallelRecord::Verify()} {
    for (u32 i = 0; i < workers_n; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->pool = std::make_unique<CommandPool>(instance, work_semaphore,
                                                     vk::CommandBufferLevel::eSecondary);
        workers.push_back(std::move(worker));
    }
    // Started last: the workers read the members above
    for (u32 i = 0; i < workers_n; ++i) {
        workers[i]->thread = std::thread{[this, i] { WorkerLoop(i); }};
    }
}

PassSecondary::~PassSecondary() {
    {
        std::scoped_lock lk{mutex};
        stopping = true;
    }
    cv.notify_all();
    // The workers record what is queued, then stop
    for (auto& worker : workers) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    for (Job* job : open) {
        delete job;
    }
    open.clear();
    active = nullptr;
    ClearLookAhead();
    ClearTable();
}

void PassSecondary::Pin(Chunk* chunk, Chunk* prev) {
    if (chunk != nullptr) {
        chunk->table_pins++;
    }
    if (prev != nullptr) {
        prev->table_pins++;
    }
}

void PassSecondary::Unpin(Chunk* chunk, Chunk* prev) {
    if (chunk != nullptr) {
        chunk->table_pins--;
    }
    if (prev != nullptr) {
        prev->table_pins--;
    }
}

void PassSecondary::PinJob(Job* job, Chunk* chunk) {
    if (chunk != nullptr) {
        chunk->job_pins.fetch_add(1, std::memory_order_acq_rel);
        job->pinned.push_back(chunk);
    }
}

void PassSecondary::Insert(Chunk* chunk, Chunk* previous, BlockHeader* header,
                           const StateMeta* meta) {
    const TableKey table_key{meta->cmd, meta->key};
    if (const auto it = table.find(table_key); it != table.end()) {
        // The latest block of this key replaces the older one
        Unpin(it->second.chunk, it->second.prev);
        it->second = StateEntry{header, meta, chunk, previous, ++seq};
        Pin(chunk, previous);
        return;
    }
    if (table.size() >= kMaxEntries) {
        Prune(meta->cmd);
    }
    table.emplace(table_key, StateEntry{header, meta, chunk, previous, ++seq});
    Pin(chunk, previous);
}

void PassSecondary::Prune(VkCommandBuffer keep) {
    // Entries of the command buffer being inserted into stay. The others go, and their passes fall
    // back to the primary (Decide), because a partial replay would be wrong.
    for (auto it = table.begin(); it != table.end();) {
        if (it->first.cmd != keep) {
            Unpin(it->second.chunk, it->second.prev);
            dropped.insert(it->first.cmd);
            it = table.erase(it);
        } else {
            ++it;
        }
    }
}

void PassSecondary::Forget(VkCommandBuffer fake) {
    dropped.erase(fake);
    for (auto it = table.begin(); it != table.end();) {
        if (it->first.cmd == fake) {
            Unpin(it->second.chunk, it->second.prev);
            it = table.erase(it);
        } else {
            ++it;
        }
    }
}

void PassSecondary::Submitted(std::span<const vk::CommandBuffer> fakes, u64 signal_tick) {
    for (const vk::CommandBuffer fake : fakes) {
        Forget(static_cast<VkCommandBuffer>(fake));
    }
    for (size_t i = 0; i < awaiting.size();) {
        const Awaiting entry = awaiting[i];
        bool submitted = false;
        for (const vk::CommandBuffer fake : fakes) {
            if (static_cast<VkCommandBuffer>(fake) == entry.fake) {
                submitted = true;
                break;
            }
        }
        if (!submitted) {
            ++i;
            continue;
        }
        {
            Worker& worker = *workers[entry.worker];
            std::scoped_lock lk{worker.pool_mutex};
            worker.pool->Release(vk::CommandBuffer{entry.secondary}, signal_tick);
        }
        awaiting[i] = awaiting.back();
        awaiting.pop_back();
    }
}

void PassSecondary::ClearTable() {
    for (const auto& [key, entry] : table) {
        Unpin(entry.chunk, entry.prev);
    }
    table.clear();
}

void PassSecondary::ClearLookAhead() {
    for (const auto& [key, entry] : la_table) {
        Unpin(entry.chunk, entry.prev);
    }
    la_table.clear();
    la_valid = false;
    la_chunk = nullptr;
    la_prev = nullptr;
    la_pos = 0;
}

void PassSecondary::RebuildLookAhead(Chunk* chunk, u32 pos, Chunk* previous) {
    // The consumer table is exact at the walk position, so the copy starts equal to it
    ClearLookAhead();
    la_table.reserve(table.size());
    for (const auto& [key, entry] : table) {
        la_table.emplace(key, entry);
        Pin(entry.chunk, entry.prev);
    }
    la_seq = seq;
    la_chunk = chunk;
    la_prev = previous;
    la_pos = pos;
    la_valid = true;
}

bool PassSecondary::LookAheadInsert(Chunk* chunk, Chunk* prev, BlockHeader* header,
                                    const StateMeta* meta) {
    // The same rule as Insert, without the prune: a prune is left to the consumer (false)
    const TableKey table_key{meta->cmd, meta->key};
    if (const auto it = la_table.find(table_key); it != la_table.end()) {
        Unpin(it->second.chunk, it->second.prev);
        it->second = StateEntry{header, meta, chunk, prev, ++la_seq};
        Pin(chunk, prev);
        return true;
    }
    if (la_table.size() >= kMaxEntries) {
        return false;
    }
    la_table.emplace(table_key, StateEntry{header, meta, chunk, prev, ++la_seq});
    Pin(chunk, prev);
    return true;
}

void PassSecondary::Visit(Chunk* chunk, u32 pos) {
    if (la_valid && chunk == la_chunk && pos == la_pos) {
        ClearLookAhead();
    }
}

bool PassSecondary::Handle(Chunk* chunk, u32 pos, Chunk* previous, BlockHeader* header,
                           const StateMeta* meta, std::stop_token stop) {
    if (active != nullptr) {
        if (chunk == active->end_chunk && pos == active->end_pos) {
            Finish(chunk, pos, previous, header);
            return true;
        }
        // A block of the pass: the worker runs it, but a state block stays in the table
        if (meta != nullptr) {
            Insert(chunk, previous, header, meta);
        }
        // sotc: the producer may have published the next pass since the look-ahead last looked
        if (la_incomplete && (++la_retry & 63) == 0) {
            LookAhead(chunk, pos + header->size, previous);
        }
        return true;
    }
    if (header->category == BlockCategory::BeginRendering &&
        header->aux == CommandRecorder::kBeginPassAux) {
        return TryStart(chunk, pos, previous, header, stop);
    }
    if (meta != nullptr) {
        Insert(chunk, previous, header, meta);
    }
    return false;
}

bool PassSecondary::ScanPass(Chunk* chunk, u32 pos, std::stop_token stop, Scan& scan, bool wait) {
    // Reads the pass blocks as the producer publishes them, so the pass is complete before it is
    // handed over. The scan gives up (scan.bad, the pass stays on the primary) at a block the
    // secondary cannot hold, after kMaxScanChunks chunks, and after kMaxScanWait without a new block:
    // the producer may wait for the consumer (chunk recycling, a submit) before it ends the pass.
    // A scan that does not wait (look-ahead) returns with scan.incomplete at the first unpublished
    // block. Returns false when shutdown interrupts the wait.
    Chunk* c = chunk;
    u32 p = pos;
    scan.chunks.push_back(c);
    u32 spins = 0;
    bool waiting = false;
    std::chrono::steady_clock::time_point wait_start{};
    while (true) {
        const u32 committed = c->committed.load(std::memory_order_acquire);
        if (p >= committed) {
            if (!wait) {
                scan.incomplete = true;
                return true;
            }
            if (stop.stop_requested()) {
                return false;
            }
            if (++spins < 64) {
                RECORDER_PAUSE();
                continue;
            }
            spins = 0;
            const auto now = std::chrono::steady_clock::now();
            if (!waiting) {
                waiting = true;
                wait_start = now;
            } else if (now - wait_start > kMaxScanWait) {
                scan.bad = true;
                scan.reason = ParallelRecord::Fallback::ScanTimeout; // sotc: N5 counters
                return true;
            }
            std::this_thread::yield();
            continue;
        }
        waiting = false;
        auto* h = reinterpret_cast<BlockHeader*>(c->data + p);
        if (h->kind == BlockKind::End) {
            if (scan.chunks.size() >= kMaxScanChunks) {
                scan.bad = true;
                scan.reason = ParallelRecord::Fallback::ScanChunks; // sotc: N5 counters
                return true;
            }
            // NextChunk links the next chunk before it publishes the End block
            c = c->next.load(std::memory_order_acquire);
            scan.chunks.push_back(c);
            p = 0;
            continue;
        }
        if (h->kind == BlockKind::Data || h->kind == BlockKind::Meta) {
            p += h->size;
            continue;
        }
        if (h->category == BlockCategory::EndRendering) {
            scan.found = true;
            scan.end_chunk = c;
            scan.end_pos = p;
            return true;
        }
        switch (h->category) {
        case BlockCategory::Draw:
            ++scan.draws;
            break;
        case BlockCategory::BindDescriptors:
            ++scan.descriptors; // sotc: SOTC_PASS_RECORD packet shape (Decide), same handling as below
            break;
        case BlockCategory::BindPipeline:
        case BlockCategory::PushConstants:
        case BlockCategory::DynamicState:
        case BlockCategory::VertexIndex:
        case BlockCategory::Label:
            break;
        default:
            // Dispatch, barriers, copies, nested passes and submits stay on the primary
            scan.bad = true;
            scan.reason = ParallelRecord::Fallback::ScanBlock; // sotc: N5 counters
            scan.block = static_cast<u8>(h->category);
            return true;
        }
        ++scan.blocks;
        p += h->size;
    }
}

PassSecondary::Decision PassSecondary::Decide(Chunk* chunk, u32 pos, BlockHeader* header,
                                              std::stop_token stop, bool walk) {
    // Everything that decides whether a pass goes to a worker. The scan reads published blocks only:
    // with walk false (look-ahead) it returns Incomplete instead of waiting for the producer.
    Decision d;
    const auto* begin = reinterpret_cast<const BeginPayload*>(header + 1);
    if (!begin->formats.valid) {
        d.kind = Decision::Kind::Inline;
        d.reason = ParallelRecord::Fallback::FormatsInvalid;
        return d;
    }
    if (dropped.contains(begin->cmdbuf)) {
        d.kind = Decision::Kind::Inline;
        d.reason = ParallelRecord::Fallback::Dropped;
        return d;
    }
    if (!ScanPass(chunk, pos + header->size, stop, d.scan, walk)) {
        d.kind = Decision::Kind::Stop;
        d.reason = ParallelRecord::Fallback::ScanStopped;
        return d;
    }
    if (d.scan.incomplete) {
        d.kind = Decision::Kind::Incomplete;
        return d;
    }
    if (d.scan.bad) {
        d.kind = Decision::Kind::Inline;
        d.reason = d.scan.reason;
        d.block = d.scan.block;
        return d;
    }
    if (walk && ParallelRecord::PacketRecord()) { // sotc: SOTC_PASS_RECORD + SOTC_PACKET counters
        ParallelRecord::NotePacketPass(d.scan.descriptors == 0, d.scan.draws);
    }
    if (d.scan.draws <= min_draws) {
        d.kind = Decision::Kind::Inline;
        d.reason = ParallelRecord::Fallback::FewDraws;
        return d;
    }
    // sotc: SOTC_N5_PARALLEL=2 (dry run): the pass passed every check of the parallel path. It is
    // counted and left to the owner, which records it serially as in mode 0. The scan read the
    // blocks only, so nothing is consumed. The BeginRendering is not marked secondary.
    if (walk && ParallelRecord::Dry()) {
        d.kind = Decision::Kind::Dry;
        return d;
    }
    if (rec.chunks_in_flight.load(std::memory_order_acquire) + kRingHeadroom >=
        CommandRecorder::MaxChunksInFlight) {
        d.kind = Decision::Kind::Inline;
        d.reason = ParallelRecord::Fallback::Ring;
        return d;
    }
    if (open.size() >= workers_n) {
        d.kind = Decision::Kind::Inline;
        d.reason = ParallelRecord::Fallback::Workers;
        return d;
    }
    d.kind = Decision::Kind::Parallel;
    return d;
}

PassSecondary::Job* PassSecondary::MakeJob(Chunk* chunk, u32 pos, BlockHeader* header,
                                           const Scan& scan, const StateTable& src) {
    const auto* begin = reinterpret_cast<const BeginPayload*>(header + 1);
    auto* job = new Job;
    job->fake = begin->cmdbuf;
    job->formats = begin->formats;
    job->block_chunk = chunk;
    job->block_pos = pos;
    job->begin_chunk = chunk;
    job->begin_pos = pos + header->size;
    job->end_chunk = scan.end_chunk;
    job->end_pos = scan.end_pos;
    job->seq = ++job_seq;
    for (Chunk* c : scan.chunks) {
        PinJob(job, c);
    }
    for (const auto& [key, entry] : src) {
        if (key.cmd != job->fake) {
            continue;
        }
        job->state.push_back(entry);
        PinJob(job, entry.chunk);
        PinJob(job, entry.prev);
    }
    std::sort(job->state.begin(), job->state.end(),
              [](const StateEntry& a, const StateEntry& b) { return a.seq < b.seq; });
    if (verify_on) {
        // Taken before the job is queued: no worker reads the blocks yet
        u64 hash = kHashSeed;
        ForEachBlock(job, [&hash](BlockHeader* block) { MixBlock(hash, block); });
        job->ref_hash = hash;
    }
    return job;
}

void PassSecondary::Enqueue(Job* job) {
    {
        std::scoped_lock lk{mutex};
        queue.push_back(job);
    }
    cv.notify_one();
}

void PassSecondary::StartJob(Job* job, BlockHeader* header) {
    // The primary begins the pass with secondary contents and executes the secondary at its end
    auto* begin = reinterpret_cast<BeginPayload*>(header + 1);
    begin->info.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    header->invoke(header + 1);
    job->started = true;
    active = job;
    if (verify_on) {
        // The snapshot must be the consumer table at this point: same entries, same order
        std::vector<StateEntry> real;
        for (const auto& [key, entry] : table) {
            if (key.cmd == job->fake) {
                real.push_back(entry);
            }
        }
        std::sort(real.begin(), real.end(),
                  [](const StateEntry& a, const StateEntry& b) { return a.seq < b.seq; });
        bool same = real.size() == job->state.size();
        for (size_t i = 0; same && i < real.size(); ++i) {
            same = real[i].header == job->state[i].header && real[i].meta == job->state[i].meta &&
                   real[i].chunk == job->state[i].chunk && real[i].prev == job->state[i].prev;
        }
        job->state_ok = same;
    }
    if (ParallelRecord::Counted()) {
        ParallelRecord::NotePassParallel(); // sotc: SOTC_N5_PARALLEL counters
    }
    if (SotcStats::kCmdStats) {
        SotcStats::passes_secondary.fetch_add(1, std::memory_order_relaxed);
    }
}

bool PassSecondary::TryStart(Chunk* chunk, u32 pos, Chunk* previous, BlockHeader* header,
                             std::stop_token stop) {
    const bool counted = ParallelRecord::Counted(); // sotc: SOTC_N5_PARALLEL / pool counters
    // The look-ahead prepared this pass: its job is the first one still to start
    if (!open.empty()) {
        Job* job = open.front();
        if (!job->started && job->block_chunk == chunk && job->block_pos == pos) {
            StartJob(job, header);
            LookAhead(chunk, pos + header->size, previous);
            return true;
        }
        // sotc: the look-ahead prepared a later pass, so this one was passed over as ineligible (it
        // stays on the primary, the walk never reorders passes); counted as a slot fallback
        if (counted) {
            ParallelRecord::NoteFallback(ParallelRecord::Fallback::Workers);
        }
        return false;
    }

    const Decision d = Decide(chunk, pos, header, stop, /*walk=*/true);
    switch (d.kind) {
    case Decision::Kind::Stop:
        if (counted) {
            ParallelRecord::NoteFallback(ParallelRecord::Fallback::ScanStopped);
        }
        return false;
    case Decision::Kind::Dry:
        if (counted) {
            ParallelRecord::NoteWouldParallel(d.scan.draws, d.scan.blocks);
        }
        return false;
    case Decision::Kind::Inline:
        if (counted) {
            ParallelRecord::NoteFallback(d.reason, d.block);
        }
        if (SotcStats::kCmdStats && d.reason != ParallelRecord::Fallback::FewDraws &&
            d.reason != ParallelRecord::Fallback::Workers &&
            d.reason != ParallelRecord::Fallback::Ring) {
            SotcStats::passes_fallback.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    case Decision::Kind::Incomplete:
    case Decision::Kind::Parallel:
        break;
    }
    if (d.kind == Decision::Kind::Incomplete) {
        return false; // not reachable with wait, kept for the switch
    }

    Job* job = MakeJob(chunk, pos, header, d.scan, table);
    open.push_back(job);
    StartJob(job, header); // the primary begins the pass, then a worker may take the job
    Enqueue(job);
    LookAhead(chunk, pos + header->size, previous);
    return true;
}

void PassSecondary::LookAhead(Chunk* chunk, u32 pos, Chunk* previous) {
    // Walks the published blocks after the cursor and prepares the passes it finds, while there is a
    // free slot for each. Never waits: the first unpublished block ends the walk (la_incomplete).
    if (workers_n < 2 || ParallelRecord::Dry()) {
        return;
    }
    if (!la_valid) {
        // The copy is rebuilt only when no prepared pass is left to start (the walk is at the
        // cursor). An active pass is the walk's own; anything else prepared must start first.
        if (open.size() > (active != nullptr ? 1U : 0U)) {
            return;
        }
        RebuildLookAhead(chunk, pos, previous);
    }
    la_incomplete = false;
    const bool counted = ParallelRecord::Counted();
    while (true) {
        Chunk* c = la_chunk;
        const u32 start = la_pos;
        const u32 committed = c->committed.load(std::memory_order_acquire);
        if (start >= committed) {
            la_incomplete = true;
            return;
        }
        auto* h = reinterpret_cast<BlockHeader*>(c->data + start);
        if (h->kind == BlockKind::End) {
            // NextChunk links the next chunk before it publishes the End block
            la_prev = c;
            la_chunk = c->next.load(std::memory_order_acquire);
            la_pos = 0;
            continue;
        }
        if (h->kind == BlockKind::Data) {
            la_pos = start + h->size;
            continue;
        }
        const StateMeta* meta = nullptr;
        u32 p = start;
        if (h->kind == BlockKind::Meta) {
            // A state block: its Meta and command block are reserved together, in one chunk
            const u32 next = start + h->size;
            if (next >= committed) {
                la_incomplete = true;
                return;
            }
            meta = reinterpret_cast<const StateMeta*>(h + 1);
            p = next;
            h = reinterpret_cast<BlockHeader*>(c->data + p);
        }
        if (h->kind != BlockKind::Command) {
            la_incomplete = true;
            return;
        }
        if (h->category == BlockCategory::Other) {
            // Bind, submits and other closures change the table through Forget: the walk does them
            if (counted) {
                ParallelRecord::NoteLookahead(ParallelRecord::LaStop::Other);
            }
            return;
        }
        if (h->category == BlockCategory::BeginRendering &&
            h->aux == CommandRecorder::kBeginPassAux) {
            if (open.size() >= workers_n) {
                if (counted) {
                    ParallelRecord::NoteLookahead(ParallelRecord::LaStop::Queue);
                }
                return;
            }
            if (rec.chunks_in_flight.load(std::memory_order_acquire) + kRingHeadroom >=
                CommandRecorder::MaxChunksInFlight) {
                if (counted) {
                    ParallelRecord::NoteLookahead(ParallelRecord::LaStop::Ring);
                }
                return;
            }
            const Decision d = Decide(c, p, h, std::stop_token{}, /*walk=*/false);
            if (d.kind == Decision::Kind::Incomplete) {
                la_incomplete = true;
                if (counted) {
                    ParallelRecord::NoteLookahead(ParallelRecord::LaStop::Incomplete);
                }
                return;
            }
            if (d.kind == Decision::Kind::Parallel) {
                // The snapshot is the copy at the BeginRendering: the blocks before it are applied
                Job* job = MakeJob(c, p, h, d.scan, la_table);
                open.push_back(job);
                Enqueue(job);
                if (counted) {
                    ParallelRecord::NoteLookaheadPrepare(open.size());
                }
            }
            // Any other decision: the pass stays on the primary and the walk decides it again
        }
        if (meta != nullptr && !LookAheadInsert(c, la_prev, h, meta)) {
            // The consumer prunes at this insert; the copy stops here until the walk gets there
            if (counted) {
                ParallelRecord::NoteLookahead(ParallelRecord::LaStop::Prune);
            }
            return;
        }
        la_pos = p + h->size;
    }
}

void PassSecondary::Replay(VkCommandBuffer fake) {
    // Consumer thread: the table entries of this command buffer, latest last, run on the primary
    std::vector<StateEntry> latest;
    for (const auto& [key, entry] : table) {
        if (key.cmd == fake) {
            latest.push_back(entry);
        }
    }
    std::sort(latest.begin(), latest.end(),
              [](const StateEntry& a, const StateEntry& b) { return a.seq < b.seq; });
    for (const StateEntry& entry : latest) {
        entry.header->invoke(entry.header + 1);
    }
}

void PassSecondary::WaitJob(const Job* job) {
    // The worker stores job->done, then bumps done_gen and wakes the waiters. The consumer reads the
    // generation before it checks done, so a completion in between changes the value it sleeps on.
    while (!job->done.load(std::memory_order_acquire)) {
        const u64 gen = done_gen.load(std::memory_order_acquire);
        if (job->done.load(std::memory_order_acquire)) {
            break;
        }
        if (FutexWaitOn()) { // sotc: SOTC_FUTEX_WAIT
            FutexSleepRec(done_gen, gen);
        } else {
            done_gen.wait(gen, std::memory_order_acquire);
        }
    }
}

void PassSecondary::Finish(Chunk* chunk, u32 pos, Chunk* previous, BlockHeader* end) {
    Job* job = active;
    const u32 next_pos = pos + end->size;
    // sotc: SOTC_N5_PARALLEL: the owner's wait for the worker (clock reads only when counted)
    const bool counted = ParallelRecord::Counted();
    const auto wait_t0 =
        counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    WaitJob(job);
    if (counted) {
        ParallelRecord::NoteOwnerWait(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 wait_t0)
                .count()));
    }
    if (verify_on) {
        ParallelRecord::NoteVerify(job->state_ok, job->ref_hash == job->run_hash);
    }
    // sotc: SOTC_PASS_RECORD + SOTC_PACKET: the stitch (execute, EndRendering, replay), clock reads only then
    const bool stitch = ParallelRecord::PacketRecord();
    const u64 stitch_t0 = stitch ? ParallelRecord::NowNs() : 0;
    const VkCommandBuffer primary = rec.Real(job->fake);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdExecuteCommands(primary, 1, &job->secondary);
    awaiting.push_back(Awaiting{job->fake, job->secondary, job->worker});
    end->invoke(end + 1); // EndRendering on the primary
    Replay(job->fake);
    if (stitch) {
        ParallelRecord::NoteStitch(ParallelRecord::NowNs() - stitch_t0);
    }
    if (!open.empty() && open.front() == job) {
        open.pop_front();
    }
    delete job;
    active = nullptr;
    rec.SweepDeferred();
    // A slot is free: the look-ahead prepares the next pass now, while the walk goes on
    LookAhead(chunk, next_pos, previous);
}

void PassSecondary::WorkerLoop(u32 index) {
    Common::SetCurrentThreadName("shadPS4:PassSecondary");
    SotcStats::RegisterThreadRole(SotcStats::RolePassSecondary); // sotc: SOTC_FPS accounting
    while (true) {
        Job* job = nullptr;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                return;
            }
            job = queue.front();
            queue.pop_front();
        }
        Record(job, index);
    }
}

void PassSecondary::Record(Job* job, u32 index) {
    Worker& worker = *workers[index];
    // sotc: SOTC_N5_PARALLEL: worker time and commands replayed (clock reads only when counted)
    const bool counted = ParallelRecord::Counted();
    const auto job_t0 =
        counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    u64 ops = 0;
    u64 run_hash = kHashSeed;
    VkCommandBuffer secondary{};
    {
        std::scoped_lock lk{worker.pool_mutex};
        secondary = static_cast<VkCommandBuffer>(worker.pool->Commit());
        worker.pool->Hold(vk::CommandBuffer{secondary});
    }

    VkCommandBufferInheritanceRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    rendering.colorAttachmentCount = job->formats.num_colors;
    rendering.pColorAttachmentFormats = job->formats.colors.data();
    rendering.depthAttachmentFormat = job->formats.depth;
    rendering.stencilAttachmentFormat = job->formats.stencil;
    rendering.rasterizationSamples = job->formats.samples;
    VkCommandBufferInheritanceInfo inheritance{};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.pNext = &rendering;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inheritance;
    const VkResult begun = real_begin(secondary, &begin_info);
    ASSERT_MSG(begun == VK_SUCCESS, "SOTC_PASS_SEC: vkBeginCommandBuffer failed: {}",
               static_cast<u32>(begun));

    // Commands of the fake handle now go to this secondary (CommandRecorder::Real). The thread-local
    // override is per worker, so several passes of one command buffer can be recorded at once.
    tls_pass_fake = job->fake;
    tls_pass_real = secondary;
    ForEachBlock(job, [&](BlockHeader* header) {
        // The hash is taken before the block runs: running it may destroy its payload
        if (verify_on) {
            MixBlock(run_hash, header);
        }
        header->invoke(header + 1);
        ++ops;
    });
    tls_pass_fake = nullptr;
    tls_pass_real = nullptr;

    const VkResult ended = real_end(secondary);
    ASSERT_MSG(ended == VK_SUCCESS, "SOTC_PASS_SEC: vkEndCommandBuffer failed: {}",
               static_cast<u32>(ended));
    job->secondary = secondary;
    job->worker = index;
    job->run_hash = run_hash;
    if (counted) {
        ParallelRecord::NoteWorkerJob(
            ops, static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - job_t0)
                                      .count()));
    }

    // The consumer may recycle the chunks from here on
    for (Chunk* pinned : job->pinned) {
        pinned->job_pins.fetch_sub(1, std::memory_order_release);
    }
    job->pinned.clear();
    // Last access to job: the consumer may delete it once it sees done
    job->done.store(true, std::memory_order_release);
    done_gen.fetch_add(1, std::memory_order_seq_cst);
    if (FutexWaitOn()) { // sotc: SOTC_FUTEX_WAIT: WaitJob sleeps on done_gen
        FutexWakeRec(done_gen, true);
    } else {
        done_gen.notify_all();
    }
}

CommandRecorder::CommandRecorder(const Instance& instance, Semaphore* work_semaphore) {
    ASSERT_MSG(g_recorder == nullptr, "cp_record_thread: only one recorded scheduler is supported");
    InstallThunks();
    g_recorder = this;
    write_chunk = AcquireChunk();
    first_chunk = write_chunk;
    producer_tid = CurrentThreadId();
    if (PassSecEnabled()) {
        // sotc: created before the consumer starts, which reads pass
        pass = std::make_unique<PassSecondary>(*this, instance, work_semaphore, PassSecMin());
        state_meta = true;
        LOG_INFO(Render_Vulkan, "SOTC_PASS_SEC: passes over {} draws run on a secondary command buffer",
                 PassSecMin());
    }
    thread = std::jthread([this](std::stop_token stoken) { ConsumerLoop(stoken); });
}

CommandRecorder::~CommandRecorder() {
    Sync();
    thread.request_stop();
    WakeConsumer();
    thread.join();
    // sotc: stops the pass worker (it reads the chunks and calls the driver through g_recorder)
    pass.reset();
    g_recorder = nullptr;
    std::scoped_lock lk{free_mutex};
    for (Chunk* chunk : free_chunks) {
        delete chunk;
    }
    for (Chunk* chunk : deferred_chunks) {
        delete chunk;
    }
    deferred_chunks.clear();
    // The chunks still linked after the consumer position contain no remaining commands to run and
    // can be released during cleanup
}

vk::CommandBuffer CommandRecorder::NewFakeHandle() {
    // Fake handles wrap around after NumFakeHandles sessions, so confirm the old session was
    // submitted before using that handle again
    const SotcStats::SplitTimer recorder_split{SotcStats::SplitWaitRecorder};
    if (handles_issued - handles_retired.load(std::memory_order_acquire) >= NumFakeHandles / 2) {
        const SotcStats::DrawWait wait{SotcStats::WaitRecorder}; // sotc: SOTC_FPS accounting
        while (handles_issued - handles_retired.load(std::memory_order_acquire) >=
               NumFakeHandles / 2) {
            Kick();
            std::this_thread::yield();
        }
    }
    const u32 index = static_cast<u32>(handles_issued++ % NumFakeHandles);
    return vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(&g_fake_handles[index])};
}

void CommandRecorder::Bind(VkCommandBuffer fake, VkCommandBuffer real) noexcept {
    real_handles[reinterpret_cast<u8*>(fake) - g_fake_handles] = real;
    if (pass) {
        // sotc: a new session starts with no replay state (consumer thread)
        pass->Forget(fake);
    }
}

void CommandRecorder::PassSubmitted(std::span<const vk::CommandBuffer> fakes, u64 signal_tick) {
    if (pass) {
        pass->Submitted(fakes, signal_tick);
    }
}

VkCommandBuffer CommandRecorder::Real(VkCommandBuffer fake) const noexcept {
    // sotc: on the pass worker, its secondary command buffer records the commands of the pass
    if (fake != nullptr && fake == tls_pass_fake) {
        return tls_pass_real;
    }
    return real_handles[reinterpret_cast<u8*>(fake) - g_fake_handles];
}

void CommandRecorder::CheckProducer() {
    static thread_local const u32 tid = CurrentThreadId();
    if (tid == producer_tid) [[likely]] {
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
    UNREACHABLE_MSG("cp_record_thread: thread {} records on the scheduler of thread {}; the command "
                    "stream has one producer",
                    tid, producer_tid);
}

void CommandRecorder::ClaimProducer() {
    const u32 tid = CurrentThreadId();
    if (tid != producer_tid) {
        LOG_INFO(Render_Vulkan, "cp_record_thread: thread {} takes over recording from {}", tid,
                 producer_tid);
        producer_tid = tid;
    }
}

void* CommandRecorder::Allocate(size_t size, BlockKind kind, BlockCategory category,
                                void (*invoke)(void*), u16 aux) {
    const size_t total = (sizeof(BlockHeader) + size + BlockAlign - 1) & ~(BlockAlign - 1);
    ASSERT_MSG(total + sizeof(BlockHeader) <= Chunk::Size, "cp_record_thread: block of {} bytes",
               total);
    if (write_pos + total + sizeof(BlockHeader) > Chunk::Size) {
        NextChunk();
    }
    // sotc: SOTC_N5_PARALLEL_TRANSLATE census, the producer's header write only (not NextChunk)
    const bool census = ParallelRecord::TranslateCensus();
    const u64 census_t0 = census ? ParallelRecord::NowNs() : 0;
    auto* header = reinterpret_cast<BlockHeader*>(write_chunk->data + write_pos);
    header->size = static_cast<u32>(total);
    header->kind = kind;
    header->category = category;
    header->aux = aux;
    header->invoke = invoke;
    write_pos += static_cast<u32>(total);
    if (census) {
        ParallelRecord::NoteEmit(static_cast<u8>(category), static_cast<u32>(total),
                                 ParallelRecord::NowNs() - census_t0);
    }
    return header + 1;
}

void* CommandRecorder::AllocateState(size_t size, BlockCategory category, void (*invoke)(void*),
                                     VkCommandBuffer cmd, StateKey key, u16 aux) {
    // The Meta block and the command block are reserved together so that they share a chunk and
    // the consumer finds the meta right before the command it describes
    constexpr size_t meta_total =
        (sizeof(BlockHeader) + sizeof(StateMeta) + BlockAlign - 1) & ~(BlockAlign - 1);
    const size_t total = (sizeof(BlockHeader) + size + BlockAlign - 1) & ~(BlockAlign - 1);
    ASSERT_MSG(meta_total + total + sizeof(BlockHeader) <= Chunk::Size,
               "cp_record_thread: block of {} bytes", total);
    if (write_pos + meta_total + total + sizeof(BlockHeader) > Chunk::Size) {
        NextChunk();
    }
    auto* meta_header = reinterpret_cast<BlockHeader*>(write_chunk->data + write_pos);
    meta_header->size = static_cast<u32>(meta_total);
    meta_header->kind = BlockKind::Meta;
    meta_header->category = BlockCategory::Other;
    meta_header->aux = 0;
    meta_header->invoke = nullptr;
    auto* meta = reinterpret_cast<StateMeta*>(meta_header + 1);
    meta->cmd = cmd;
    meta->key = key;
    write_pos += static_cast<u32>(meta_total);

    // sotc: SOTC_N5_PARALLEL_TRANSLATE census; times the command block only (the Meta is not timed)
    const bool census = ParallelRecord::TranslateCensus();
    const u64 census_t0 = census ? ParallelRecord::NowNs() : 0;
    auto* header = reinterpret_cast<BlockHeader*>(write_chunk->data + write_pos);
    header->size = static_cast<u32>(total);
    header->kind = BlockKind::Command;
    header->category = category;
    header->aux = aux;
    header->invoke = invoke;
    write_pos += static_cast<u32>(total);
    if (census) {
        ParallelRecord::NoteEmit(static_cast<u8>(category), static_cast<u32>(meta_total + total),
                                 ParallelRecord::NowNs() - census_t0);
    }
    return header + 1;
}

void CommandRecorder::Reserve(size_t bytes) {
    if (write_pos + bytes + sizeof(BlockHeader) > Chunk::Size && bytes < Chunk::Size / 2) {
        NextChunk();
    }
}

void CommandRecorder::Publish() {
    CheckProducer();
    // Publish commands in batches so the producer and consumer do not keep competing over the
    // committed cache line
    // Every Kick publishes the whole pending batch before submits or drains need to observe it
    if (++unpublished >= PublishBatch) {
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
    end->category = BlockCategory::Other;
    end->aux = 0;
    end->invoke = nullptr;
    write_pos += sizeof(BlockHeader);
    write_chunk->committed.store(write_pos, std::memory_order_release);
    if (WaitNoSpin() || FutexWaitOn()) { // sotc: the futex consumer sleeps too, it needs the wake
        // sotc: the consumer may be blocked on the end of the old chunk. The fence orders the
        // committed store before the sleeping flag load, as in PublishAll and Kick (otherwise the
        // consumer can sleep on the old value until the next publish: a hitch).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        WakeIfSleeping();
    }
    write_chunk = next;
    write_pos = 0;
}

CommandRecorder::Chunk* CommandRecorder::AcquireChunk() {
    std::optional<SotcStats::SplitTimer> recorder_split;
    if (chunks_in_flight.load(std::memory_order_acquire) >= MaxChunksInFlight) {
        recorder_split.emplace(SotcStats::SplitWaitRecorder);
        const SotcStats::DrawWait wait{SotcStats::WaitRecorder}; // sotc: SOTC_FPS accounting
        while (chunks_in_flight.load(std::memory_order_acquire) >= MaxChunksInFlight) {
            // The recording thread is far enough behind to fill the queue, so let it catch up
            // before the producer adds more commands
            Kick();
            std::this_thread::yield();
        }
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
    chunk->job_pins.store(0, std::memory_order_relaxed);
    chunk->table_pins = 0;
    return chunk;
}

void CommandRecorder::RecycleChunk(Chunk* chunk) {
    {
        std::scoped_lock lk{free_mutex};
        free_chunks.push_back(chunk);
    }
    chunks_in_flight.fetch_sub(1, std::memory_order_release);
}

void CommandRecorder::ReleaseChunk(Chunk* chunk) {
    // sotc: a chunk the replay table or a pass job still reads waits in deferred_chunks
    if (chunk->table_pins == 0 && chunk->job_pins.load(std::memory_order_acquire) == 0) {
        RecycleChunk(chunk);
    } else {
        deferred_chunks.push_back(chunk);
    }
}

void CommandRecorder::SweepDeferred() {
    for (size_t i = 0; i < deferred_chunks.size();) {
        Chunk* chunk = deferred_chunks[i];
        if (chunk->table_pins == 0 && chunk->job_pins.load(std::memory_order_acquire) == 0) {
            RecycleChunk(chunk);
            deferred_chunks[i] = deferred_chunks.back();
            deferred_chunks.pop_back();
        } else {
            ++i;
        }
    }
}

void CommandRecorder::WakeConsumer() {
    if (FutexWaitOn()) { // sotc: SOTC_FUTEX_WAIT: the consumer sleeps on wake_seq only
        wake_seq.fetch_add(1, std::memory_order_seq_cst);
        FutexWakeRec(wake_seq, false);
        return;
    }
    {
        std::scoped_lock lk{wake_mutex};
        wake_flag = true;
    }
    wake_cv.notify_one();
    if (WaitNoSpin()) {
        // sotc: the doorbell the consumer blocks on; also what the destructor rings to stop it
        wake_seq.fetch_add(1, std::memory_order_seq_cst);
        wake_seq.notify_one();
    }
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
    Run(BlockCategory::Other, [this, target] {
        if (FutexWaitOn()) { // sotc: SOTC_FUTEX_WAIT: the producer sleeps on sync_done
            sync_done.store(target, std::memory_order_seq_cst);
            FutexWakeRec(sync_done, true);
            return;
        }
        sync_done.store(target, std::memory_order_release);
        sync_done.notify_all();
    });
    Kick();
    const SotcStats::DrawWait wait{SotcStats::WaitRecorder}; // sotc: SOTC_FPS accounting
    const SotcStats::SplitTimer recorder_split{SotcStats::SplitWaitRecorder};
    if (FutexWaitOn()) { // sotc: SOTC_FUTEX_WAIT
        FutexWaitAtLeast(sync_done, target);
    } else {
        u64 done = sync_done.load(std::memory_order_acquire);
        while (done < target) {
            sync_done.wait(done, std::memory_order_acquire);
            done = sync_done.load(std::memory_order_acquire);
        }
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
    SotcStats::RegisterThreadRole(SotcStats::RoleRecorder); // sotc: SOTC_FPS accounting
    BbThreads::PlaceThread(BbThreads::Role::Recorder);
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

    const bool nospin = WaitNoSpin();
    // sotc: SOTC_FUTEX_WAIT: the idle wait is a WaitOnAddress on wake_seq, after the same short
    // spin as SOTC_WAIT_NOSPIN (at most ~20 us)
    const bool futex_wait = FutexWaitOn();
    Chunk* chunk = first_chunk;
    Chunk* previous = nullptr;
    u32 pos = 0;
    // sotc: the StateMeta of the Meta block just read, used by the command block right after it
    const StateMeta* pending_meta = nullptr;
    while (true) {
        u32 committed = chunk->committed.load(std::memory_order_acquire);
        if (pos == committed) {
            // When there is no work, spin briefly before sleeping until the producer kicks the
            // queue or the short timeout expires
            if (nospin || futex_wait) {
                // sotc: at most ~20 us of spinning (deadline checked every 64 iterations)
                const auto spin_end =
                    Clock::now() + std::min(SpinTime, std::chrono::microseconds{20});
                for (u32 n = 1;
                     (committed = chunk->committed.load(std::memory_order_acquire)) == pos &&
                     !stoken.stop_requested();
                     ++n) {
                    if ((n & 63) == 0 && Clock::now() >= spin_end) {
                        break;
                    }
                    RECORDER_PAUSE();
                }
            } else {
                const auto spin_end = Clock::now() + SpinTime;
                while ((committed = chunk->committed.load(std::memory_order_acquire)) == pos &&
                       Clock::now() < spin_end && !stoken.stop_requested()) {
                    RECORDER_PAUSE();
                }
            }
            if (futex_wait) { // sotc: SOTC_FUTEX_WAIT: one idle wait, ended by the spin or the sleep
                g_futex_rec.waits.fetch_add(1, std::memory_order_relaxed);
                if (committed != pos) {
                    g_futex_rec.spins.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (committed == pos) {
                if (stoken.stop_requested()) {
                    break;
                }
                consumer_sleeping.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (futex_wait) {
                    // sotc: SOTC_FUTEX_WAIT: the snapshot is taken after the fence, before the
                    // re-check; a producer that publishes after the re-check sees consumer_sleeping
                    // and bumps wake_seq, so the sleep returns at once (see FutexWaitAtLeast).
                    const u32 seen = wake_seq.load(std::memory_order_acquire);
                    if (chunk->committed.load(std::memory_order_acquire) == pos &&
                        !stoken.stop_requested()) {
                        if (!FutexSleepRec(wake_seq, seen) &&
                            chunk->committed.load(std::memory_order_acquire) != pos) {
                            g_futex_rec.safety_met.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                } else if (nospin) {
                    // sotc: the snapshot is taken before the re-check; a producer that publishes
                    // after the re-check sees consumer_sleeping and rings the doorbell (or
                    // WakeConsumer on destruction), so the wait returns at once
                    const u32 seen = wake_seq.load(std::memory_order_acquire);
                    if (chunk->committed.load(std::memory_order_acquire) == pos &&
                        !stoken.stop_requested()) {
                        wake_seq.wait(seen, std::memory_order_acquire);
                    }
                } else if (chunk->committed.load(std::memory_order_acquire) == pos) {
                    std::unique_lock lk{wake_mutex};
                    wake_cv.wait_for(lk, SleepTime,
                                     [&] { return wake_flag || stoken.stop_requested(); });
                    wake_flag = false;
                }
                consumer_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        const bool perf = Common::PerfStats::Enabled();
        const auto busy_start = perf ? Clock::now() : Clock::time_point{};
        // sotc: SOTC_FPS: recording thread busy time (blocks run, not the wait for work)
        // sotc: SOTC_TRACE reads it too (its "rec busy" span)
        const u64 sotc_busy_start = (SotcStats::kFpsLog || SotcTrace::kOn) ? SotcStats::NowNs() : 0;
        while (pos < committed) {
            auto* header = reinterpret_cast<BlockHeader*>(chunk->data + pos);
            // sotc: SOTC_PASS_WORKERS: the look-ahead drops its copy of the table when the walk reaches it
            if (pass != nullptr) {
                pass->Visit(chunk, pos);
            }
            if (header->kind == BlockKind::End) {
                Chunk* next = chunk->next.load(std::memory_order_acquire);
                // Commands in next may still refer to data in chunk, so keep that chunk until the
                // consumer has moved beyond those commands
                if (previous != nullptr) {
                    ReleaseChunk(previous);
                }
                SweepDeferred();
                previous = chunk;
                chunk = next;
                pos = 0;
                committed = chunk->committed.load(std::memory_order_acquire);
                continue;
            }
            if (header->kind == BlockKind::Meta) {
                pending_meta = reinterpret_cast<const StateMeta*>(header + 1);
            } else if (header->kind == BlockKind::Command) {
                if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
                    SotcStats::recorder_blocks[static_cast<size_t>(header->category)].fetch_add(
                        1, std::memory_order_relaxed);
                }
                // sotc: with SOTC_PASS_SEC the pass hook may run this block itself or take it
                // over for a secondary command buffer
                const StateMeta* meta = std::exchange(pending_meta, nullptr);
                if (pass == nullptr || !pass->Handle(chunk, pos, previous, header, meta, stoken)) {
                    header->invoke(header + 1);
                }
            }
            pos += header->size;
        }
        if (SotcStats::kFpsLog) {
            SotcStats::rec_busy_ns.fetch_add(SotcStats::NowNs() - sotc_busy_start,
                                             std::memory_order_relaxed);
        }
        if (SotcTrace::kOn) { // sotc: SOTC_TRACE
            SotcTrace::Span("rec busy", sotc_busy_start, SotcStats::NowNs());
        }
        if (perf) {
            Common::PerfStats::Add(
                Common::PerfStats::Id::RecorderBusyNs,
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - busy_start)
                    .count());
        }
    }
}

} // namespace Vulkan
