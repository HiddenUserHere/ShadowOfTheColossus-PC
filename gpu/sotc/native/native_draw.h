// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the draw-thread side of the native memory model (native.cpp). The contract is native.h; these
// are the hand-offs BufferCache uses at its submit start. Nothing else includes this.
#pragma once

#include <cstdint>

#include "native.h"

namespace SotcNative {

/// An announced range waiting for the draw thread (Announce, Persistent or PerFrame; at most 64 MiB each).
struct PendingUpload {
    uint64_t addr;
    uint64_t size;
    Kind kind;
};

/// Draw thread. Pops the oldest pending upload. False when none is queued.
bool TakeUpload(PendingUpload& out);

/// The frame ticket's backend, set once by the rasterizer owner (BufferCache): the frame counter of the
/// guest (SubmitDone calls) and the GPU ticket recorded for each frame.
struct FrameBackend {
    void* ctx;
    uint64_t (*submitted)(void* ctx);
    void (*wait)(void* ctx, uint64_t frame); ///< waits for the GPU ticket of frame; never drains the CP
    uint64_t (*completed)(void* ctx);
};
void SetFrameBackend(const FrameBackend& backend);

/// Counters (draw thread or any thread, relaxed).
void NoteUploaded(uint64_t bytes);     ///< a persistent range uploaded by the draw thread (0: already clean)
void NoteMarked(uint64_t bytes);       ///< a per-frame range marked CPU-modified (fallback)
void NoteDroppedUpload();              ///< a queue overflow: nothing else can be done for the range

/// Ring depths of the engine's per-frame heaps (heaps.md section 7): the flip (FUN_0001d230) advances the
/// frame index mod depth and reuses a block after depth flips without waiting on the GPU, so the port fences.
enum class RingDepth : uint32_t {
    RenderSingleFrameGpu = 2,
    GameToRenderThread = 3,
    GameToGpu = 5,
    CpuRenderToGpuToGame = 6,
};

/// Flip site (engine 0x1d230 / 0x8c860, once per frame on the game thread). frame_now is the frame counter
/// the flip is entering (FrameSubmitted() count so far). Before the engine reuses the block of frame
/// (frame_now - depth), FrameWait(frame_now - depth) must have returned: this waits for it. No-op when
/// frame_now < depth or the mode is off. Never drains the command processor.
void FlipReuseFence(uint64_t frame_now, RingDepth depth);

} // namespace SotcNative
