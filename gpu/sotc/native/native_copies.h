// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU result copies, the engine of option 3 of docs/re-stutters/native/PLAN.md ("the consumer
// reads the last completed value without waiting"). A client registers a guest range the GPU is about to write;
// the draw thread copies every GPU write of that range into a host-readable ring right after the dispatch; the CPU
// serves the copy once the GPU finished the frame the write belongs to (a poll of the native frame ticket, never a
// wait). Clients: native_particles.cpp (ProcessStats counts), native_masks.cpp (XPR visibility masks).
#pragma once

#include <cstdint>
#include <string>

#include "video_core/renderer_vulkan/vk_common.h" // sotc: vk::CommandBuffer, vk::Buffer

namespace VideoCore {
struct Buffer;
}

namespace SotcNativeCopies {

/// Bytes of the host-readable copy ring the rasterizer creates (Attach).
inline constexpr uint64_t kRingBytes = 4ull << 20;
/// The largest range one registration may cover.
inline constexpr uint64_t kMaxBytes = 64ull << 10;

/// Key namespaces of the clients (the low bits hold a guest address or an object id).
inline constexpr uint64_t kKeyParticles = 1ull << 62;
inline constexpr uint64_t kKeyMasks = 2ull << 62;

/// True when a client is armed (the rasterizer creates the ring then). Read once.
bool Armed();

/// The frame a registration made now belongs to (SotcNative::FrameSubmitted() + 1).
uint64_t CurrentFrame();

/// Any thread. The GPU is about to write [base, base + bytes) for (key, seq). A registration whose seq is not
/// above the key's newest one first forgets the key (a new object at the same address). False when the range is
/// too large, the ring has no room or the native mode is off.
bool Register(uint64_t key, uint64_t seq, uint64_t base, uint64_t bytes);

/// Any thread. The key and seq of the newest registration at base. False when none is live.
bool KeyOf(uint64_t base, uint64_t* key, uint64_t* seq);

/// Any thread. Copies `need` bytes of the newest complete registration of key with seq in [min_seq, max_seq] into
/// dst. Returns its seq, or 0 when none is complete and large enough. `uncopied` (optional) is set when the
/// registration of max_seq exists but the GPU write was never seen.
uint64_t CopyOut(uint64_t key, uint64_t min_seq, uint64_t max_seq, uint64_t need, void* dst,
                 bool* uncopied = nullptr);

/// Draw thread. The rasterizer's host-readable ring (nullptr detaches it).
void Attach(VideoCore::Buffer* staging);
/// Draw thread, after a dispatch: a buffer range the dispatch wrote. Records the GPU copy of every overlap with
/// a registered range.
void NoteWrittenBind(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr, uint64_t size);
/// True while registrations wait (a cheap check before walking the written binds).
bool Watching();

/// "SOTC NATIVE COPIES" 5 s line (counters reset). Nothing when not armed.
/// sotc: SOTC_N5_ASYNC_COMPUTE. The async queue tick the GPU copies recorded now belong to (0: the
/// graphics queue). A copy is complete only once that async submit completed (see CopyOut).
void SetRecordingTick(uint64_t tick);

/// sotc: SOTC_N5_ASYNC_COMPUTE. Completion test of an async queue tick, set by the async compute
/// queue (null when there is none). Non-blocking.
void SetAsyncDone(bool (*done)(uint64_t tick));

void Report(std::string& out, double seconds);

} // namespace SotcNativeCopies
