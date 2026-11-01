// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the PC-native memory model (docs/re-stutters/native/PLAN.md). The game, patched at its own write,
// read and wait points (gpu/sotc/hook_handlers.cpp, scripts/hooks.d/native*.py), tells the port what it did
// instead of the port detecting it with page protection, fault readbacks and region compares.
//
// Contract (every caller relies on it):
// - Announce(addr, size, kind): the engine finished writing [addr, addr + size) and the GPU may read it from
//   the next submit on. Persistent data is uploaded once (upload workers, budgeted per frame); per-frame data
//   lives in a host-shared window and needs no upload. A CPU write after the announce of persistent data is
//   a bug the verify mode counts (VerifyUnannouncedWrite).
// - Host-shared windows (RegisterWindow): guest ranges the GPU reads and writes in place, imported as host
//   memory block by block (ImportWindowBlock, when the pool commits the block). The engine's own double/triple
//   buffering plus the frame ticket order the reuse. Memory pinned by an import is only the committed blocks.
// - The CPU never reads GPU-written memory. GPU-result consumers are patched away (gpuresults.md); the
//   verify mode counts any CPU read of a GPU-written range (VerifyGpuRead).
// - The only CPU wait on the GPU is the frame ticket (FrameWait): the engine's frame-ring label spin
//   (0x90ea0) and the port's submit gates become FrameWait on the ticket of frame N - depth.
//
// Mode: SOTC_NATIVE (read once): 0 off (today's UMA emulation), 1 on, 2 on plus verify (the old page
// tracking keeps running underneath and counts what the announces missed). SOTC_VERIFY=1 implies mode 2
// wherever SOTC_NATIVE is on. Threads: every function is safe from any thread unless noted.
#pragma once

#include <cstdint>
#include <string>

namespace SotcNative {

/// SOTC_NATIVE: 0 off, 1 on, 2 on + verify.
int Mode();
inline bool On() {
    return Mode() != 0;
}
bool Verify();

/// What an announced range holds (decides the mechanism).
enum class Kind : uint8_t {
    Persistent, ///< written once (resource build, mip stream), then read-only for the GPU: upload once
    PerFrame,   ///< rewritten every frame (instances, constants, material params, DCB payloads): host-shared
    GpuWritten, ///< the GPU writes it (render targets, compute outputs): GPU-private, never uploaded
};

/// The engine finished writing [addr, addr + size). Persistent: queued for upload, the next submit that
/// reads it waits for the upload. PerFrame outside a window: uploaded at the next submit (fallback).
void Announce(uint64_t addr, uint64_t size, Kind kind);
/// The engine freed or decommitted [addr, addr + size): drop its device copy and any pending upload.
void Release(uint64_t addr, uint64_t size);

/// A guest range the engine uses for per-frame or GPU-shared data (a window), registered once. Only the range
/// is recorded here: its memory is not backed yet, so nothing is imported. Its blocks are imported one by one
/// with ImportWindowBlock when the pool commits them. False when the range overlaps a registered window.
bool RegisterWindow(uint64_t addr, uint64_t size, const char* name);
/// Imports one block [addr, addr + size) of a registered window as host memory (VK external host memory), once
/// its backing exists. From then on the block is served in place (InWindowBlock). False when the import was
/// refused: the block falls back to announced uploads, and its binds take the normal path (counted).
bool ImportWindowBlock(uint64_t addr, uint64_t size);
/// The block was given back or decommitted: drops its import. The buffer is destroyed once the GPU has
/// finished the last frame that could read it.
void DropWindowBlock(uint64_t addr, uint64_t size);
/// True when [addr, addr + size) lies inside one registered window, imported or not (lock-free). The verify
/// mode counts a CPU write there as covered; a bind there is served only when InWindowBlock holds for it.
bool InWindow(uint64_t addr, uint64_t size);
/// True when [addr, addr + size) lies inside one imported window block (bind fast path; lock-free). An
/// announce inside such a block needs no upload.
bool InWindowBlock(uint64_t addr, uint64_t size);

/// Frame ticket: FrameSubmitted is called by the port when the engine's frame N is fully submitted
/// (sceGnmSubmitDone); FrameWait(N) returns when the GPU finished frame N. Only the frame-ring reuse and
/// frame pacing may call FrameWait.
uint64_t FrameSubmitted();
void FrameWait(uint64_t frame);
uint64_t FrameCompleted();

/// Descriptor side records (framedata.md: 0x865410 / 0x8655d0 / 0x865600 / 0x865500): the engine encoded a
/// V#/T# for [base, base + size); the port keeps the record so binds never decode guest descriptor words.
void NoteDescriptor(uint64_t desc_addr, uint64_t base, uint64_t size, uint32_t format);

/// Verify (mode 2): a CPU write the announces did not cover (from the page tracker's fault path), a CPU
/// read of GPU-written memory (from the readback path), a CPU wait on the GPU outside FrameWait.
void VerifyUnannouncedWrite(uint64_t addr, uint64_t size, uint64_t rip);
void VerifyGpuRead(uint64_t addr, uint64_t size, uint64_t rip);
/// One CPU wait on a GPU result outside FrameWait: label_addr is the GPU label the wait polls (0 when the
/// port waits on a ticket instead), rip the guest RIP the wait returns to or a PortWaitSite() tag.
void VerifyGpuWait(uint64_t label_addr, uint64_t rip);
/// Port-side waits (the rip argument of VerifyGpuWait): the high bit marks a port site, the id names it.
constexpr uint64_t PortWaitSite(uint32_t id) {
    return (1ull << 63) | id;
}
enum PortWaitId : uint32_t {
    kPortWaitFlipGpuIdle = 1, ///< PcHostFlipWait mode 1: liverpool WaitGpuIdle before the flip
    kPortWaitFlipTick = 2,    ///< PcHostFlipWait: scheduler tick wait (mode 1 and mode 2)
};

/// 5 s report lines ("SOTC NATIVE ..."), window counters reset.
void Report(std::string& out, double seconds);

} // namespace SotcNative
