// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the draw-thread side of the native memory model (native.cpp). The contract is native.h; these
// are the hand-offs BufferCache uses at its submit start. Nothing else includes this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "native.h"

namespace SotcNative {

/// An announced range waiting for the draw thread (Announce, Persistent or PerFrame; at most 64 MiB each).
struct PendingUpload {
    uint64_t addr;
    uint64_t size;
    Kind kind;
    uint64_t seq = 0;      ///< SotcNative::AnnounceSeq() of the newest announce merged into this entry (0: unknown)
    /// sotc: the range lies in a run ring 8 imports and serves in place (SotcPcRing::HostServed when announced):
    /// the drain invalidates images only, no page mark and no upload (the binds read the guest bytes).
    bool served = false;
};

/// Draw thread. Pops the oldest pending upload. False when none is queued.
bool TakeUpload(PendingUpload& out);

/// sotc: SOTC_LEAN_SUBMIT=1. Draw thread. Pops up to max uploads, in the order repeated TakeUpload calls would,
/// under one lock. Returns the count popped.
size_t TakeUploads(std::vector<PendingUpload>& out, size_t max);
/// sotc: SOTC_LEAN_SUBMIT=2 (verify). Draw thread. Copies what TakeUploads would pop now into out; the queue is
/// not changed. Returns the queue version of that state.
uint64_t PeekUploads(std::vector<PendingUpload>& out);
/// sotc: SOTC_LEAN_SUBMIT=2 (verify). Draw thread. Compares a PeekUploads snapshot with the entries the per-entry
/// loop popped. Counts a race (not compared) when the queue changed since the snapshot (announce or release).
void VerifyTakeUploads(const std::vector<PendingUpload>& snapshot, const std::vector<PendingUpload>& popped,
                       uint64_t version);

/// The frame ticket's backend, set once by the rasterizer owner (BufferCache): the frame counter of the
/// guest (SubmitDone calls) and the GPU ticket recorded for each frame.
struct FrameBackend {
    void* ctx;
    uint64_t (*submitted)(void* ctx);
    void (*wait)(void* ctx, uint64_t frame); ///< waits for the GPU ticket of frame; never drains the CP
    uint64_t (*completed)(void* ctx);
};
void SetFrameBackend(const FrameBackend& backend);

/// sotc: SOTC_NATIVE_FREE_DISCARD=1. The buffer cache's handler for a block the game freed (pool_free_block): its
/// GPU-written state is forgotten as at an unmap (SOTC_DROP_NATIVE), so the memory's next contents upload from guest
/// memory. DiscardFreed calls it (any thread; no-op until registered or with the switch off).
void SetFreeDiscard(void* ctx, void (*fn)(void* ctx, uint64_t addr, uint64_t size));
void DiscardFreed(uint64_t addr, uint64_t size);

/// sotc: SOTC_NATIVE_OWNER_DISCARD=1 (read once; needs SOTC_NATIVE mode != 0). A 4 KiB-aligned or unaligned range
/// changes owner (a guest allocation or free, or a block the engine hands to new contents): the GPU-written state
/// of the range is forgotten byte-exactly, as an unmap does (BufferCache::NativeDiscardOwner). Pages that still hold
/// GPU-written bytes outside the range stay GPU-modified. DiscardOwner calls the handler (any thread; no-op until
/// registered or with the switch off). OwnerDiscardOn tells the callers whether it would do anything.
void SetOwnerDiscard(void* ctx, void (*fn)(void* ctx, uint64_t addr, uint64_t size));
void DiscardOwner(uint64_t addr, uint64_t size);
bool OwnerDiscardOn();
/// SOTC_NATIVE_OWNER_DISCARD=2: also at the engine's frees (=1: at allocations only).
bool OwnerDiscardAtFree();
/// The same handler regardless of SOTC_NATIVE_OWNER_DISCARD (SOTC_NATIVE on only): the GPU region's quarantine
/// (gpu_region.h) forgets a freed block's GPU-written state before the block is handed out again.
void DiscardOwnerForced(uint64_t addr, uint64_t size);
/// The GPU region's quarantine (SOTC_GPU_PLACE): forgets the GPU-written state at the command processor's point
/// without marking the range CPU-written (BufferCache::NativeDiscardRegion). SOTC_NATIVE on only.
void SetRegionDiscard(void* ctx, void (*fn)(void* ctx, uint64_t addr, uint64_t size));
void DiscardRegion(uint64_t addr, uint64_t size);
/// Diagnostic tag of the calling thread's next DiscardOwner (the caller's site key; bit 63 = a free), logged with the
/// first discards.
void SetOwnerDiscardTag(uint64_t tag);
uint64_t OwnerDiscardTag();
/// Counters of the owner discard (any thread, relaxed; reported in the native report window).
enum class OwnerDiscard : uint8_t {
    Call,            ///< DiscardOwner calls while the switch is on
    Discarded,       ///< calls that passed the filters (GPU bits set, no command buffer in the range)
    Bytes,           ///< bytes of those calls
    PagesUnprotected,///< 4 KiB pages unmarked GPU-modified and marked CPU-modified
    SharedPages,     ///< partial boundary pages left GPU-modified (their other bytes are still GPU-written)
    SkippedClean,    ///< calls with no GPU-modified page in the tracker
    SkippedCmd,      ///< calls with a command buffer still using the range
};
void NoteOwnerDiscard(OwnerDiscard what, uint64_t n = 1);

/// Counters (draw thread or any thread, relaxed).
void NoteUploaded(uint64_t bytes);     ///< a persistent range uploaded by the draw thread (0: already clean)
void NoteMarked(uint64_t bytes);       ///< a per-frame range marked CPU-modified (fallback)
void NoteExactUpload(uint64_t bytes);  ///< an announce over GPU-written memory copied byte-exact (mode 1)
void NoteDroppedUpload();              ///< a queue overflow: nothing else can be done for the range

/// Timing counters of one NativeDrain (draw thread). pickup_seq is SotcNative::AnnounceSeq() when the command
/// processor picked the submit up (0: unknown). An entry whose seq is above it was announced after the submit
/// reached the port, so that submit's binds can see a mark of CPU work newer than the submit.
void NoteDrainEntry(uint64_t seq, uint64_t pickup_seq, uint64_t size);
/// End of a drain: warm_deferred_bytes = Persistent uploads postponed by the budget in this drain (already
/// marked CPU-modified, so a bind uploads them anyway), warm_backlog_bytes = bytes still waiting for a drain.
void NoteDrainDone(uint64_t warm_deferred_bytes, uint64_t warm_backlog_bytes);
/// Cost of one NativeDrain (draw thread): wall time ns, 4 KiB pages given a CPU mark, texture invalidation calls,
/// queued entries that were served in place (no mark), and merged ranges the drain processed.
void NoteDrainWork(uint64_t ns, uint64_t pages_marked, uint64_t invalidations, uint64_t served_entries,
                   uint64_t ranges);

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
