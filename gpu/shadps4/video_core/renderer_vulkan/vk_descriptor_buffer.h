// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: VK_EXT_descriptor_buffer support. SOTC_DESC_BUFFER=1 writes each draw's descriptors straight
// into a ring of host-visible memory (vkGetDescriptorEXT) for every pipeline. SOTC_N5_DESCBUF=1|2 is
// the second scheme (the N5 bind): the same ring and encoder, with per-set reuse of the bytes and the
// layout flag only on the pipelines it covers (see DescriptorBufferN5Layout).

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class DescriptorHeap;
class Instance;
class Scheduler;

/// Size of the descriptor memory ring. A frame of Shadow of the Colossus draws needs a few MB.
inline constexpr u64 DescriptorBufferBytes = 128ULL << 20;

/// sotc: SOTC_N5_DESCBUF, read once. 0: off (default, nothing changes). 1: the N5 bind, for the
/// pipelines DescriptorBufferN5Layout accepts. 2: verify, the classic bind runs and the N5 source
/// keys are checked (see VerifyN5Set in vk_pipeline_common.cpp). SOTC_DESC_BUFFER=1 forces 0 (the
/// two descriptor buffer schemes are not combined), with one log line.
[[nodiscard]] int DescriptorBufferN5Mode();

/// sotc: true when the N5 mode can run here: it is on, VK_EXT_descriptor_buffer was probed in
/// vk_instance.cpp, and the resource pool created the ring (vk_resource_pool.cpp). Logs "not
/// complete" once when the mode is on and the ring is missing, and then does nothing.
[[nodiscard]] bool DescriptorBufferN5Active(const Instance& instance, const DescriptorHeap& heap);

/// sotc: true when a pipeline with these traits takes the N5 descriptor buffer layout (mode 1 only).
/// Graphics pipelines with a set per stage and pipelines with the bindless heap set keep the classic
/// layout. Layout creation and BindResources both call it, so they always agree.
[[nodiscard]] bool DescriptorBufferN5Layout(const Instance& instance, const DescriptorHeap& heap,
                                            bool stage_sets, bool bindless);

/// sotc: counters of the N5 path, cumulative. Printed by Report() as one "SOTC N5 DESCBUF" line
/// at most every 5 s, from the draw thread.
namespace DescBufN5 {
inline std::atomic<u64> calls{};             ///< BindN5Set and VerifyN5Set calls.
inline std::atomic<u64> sets_skipped{};      ///< Mode 1: sets whose bytes were still in the ring.
inline std::atomic<u64> bytes_written{};     ///< Mode 1: descriptor bytes written into the ring.
inline std::atomic<u64> verify_checks{};     ///< Mode 2: sets with an equal source key seen before.
inline std::atomic<u64> verify_mismatches{}; ///< Mode 2: equal keys whose encoded bytes differ.
void Report();
} // namespace DescBufN5

/// Ring allocator over one buffer that holds descriptor data. Regions are released once the GPU
/// has passed the tick they were allocated in, so the ring wraps without a per-draw wait.
class DescriptorBufferRing {
public:
    struct Allocation {
        u8* mapped{};         ///< Host pointer to the region.
        vk::DeviceSize offset{}; ///< Offset of the region inside the buffer.
    };

    explicit DescriptorBufferRing(const Instance& instance, Scheduler& scheduler);
    ~DescriptorBufferRing();

    DescriptorBufferRing(const DescriptorBufferRing&) = delete;
    DescriptorBufferRing& operator=(const DescriptorBufferRing&) = delete;

    /// Reserves size bytes at an offset that is a multiple of alignment (a power of two).
    [[nodiscard]] Allocation Allocate(vk::DeviceSize size, vk::DeviceSize alignment);

    /// Makes host writes into [offset, offset + size) visible to the GPU. No-op when coherent.
    void Flush(vk::DeviceSize offset, vk::DeviceSize size) const;

    /// Device address of the buffer start, for vkCmdBindDescriptorBuffersEXT.
    [[nodiscard]] vk::DeviceAddress Address() const noexcept {
        return address;
    }

    /// True the first time it is asked in a session: the buffer must be bound in that session's
    /// command buffer before its offsets are set. Sessions are the scheduler's command buffers.
    [[nodiscard]] bool NeedsBinding(u64 session) noexcept {
        if (bound_session == session) {
            return false;
        }
        bound_session = session;
        return true;
    }

    /// sotc: the last set written for one pipeline layout, keyed by the layout handle. The key is
    /// the source key of the bytes (BuildSourceKey), so a handle reused by another layout cannot
    /// match: the key holds the layout handle, its size and its binding offsets too.
    struct SetSlot {
        std::vector<u64> key;       ///< Source key of the bytes.
        u64 session{};              ///< Session (command buffer) the bytes were written in.
        vk::DeviceSize offset{};    ///< Mode 1: region of the bytes in the buffer.
        std::vector<u8> encoded;    ///< Mode 2: the encoded descriptors of every element.
    };
    [[nodiscard]] tsl::robin_map<u64, SetSlot>& Slots() noexcept {
        return slots;
    }

private:
    struct Memory;
    /// The GPU may read a region until the tick it was allocated in has completed.
    struct Mark {
        u64 tick;
        u64 end; ///< Logical end of the region (monotonic, not wrapped).
    };

    Scheduler& scheduler;
    std::unique_ptr<Memory> memory;
    u8* mapped{};
    vk::DeviceAddress address{};
    bool coherent{};
    // Logical positions: physical offset = position % DescriptorBufferBytes.
    u64 head{};
    u64 tail{};
    std::deque<Mark> marks;
    u64 bound_session{};
    tsl::robin_map<u64, SetSlot> slots;
};

} // namespace Vulkan
