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
// - Host-shared windows (RegisterWindow): guest ranges imported once as host memory; the GPU reads and writes
//   them in place. The engine's own double/triple buffering plus the frame ticket order the reuse.
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

/// A guest range the engine uses for per-frame or GPU-shared data, imported once as host memory (VK
/// external host memory) and served in place. Returns false when the import was refused (the range then
/// falls back to announced uploads). Call before the GPU first binds it.
bool RegisterWindow(uint64_t addr, uint64_t size, const char* name);
/// True when [addr, addr + size) lies inside one registered window (bind fast path; lock-free).
bool InWindow(uint64_t addr, uint64_t size);

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
void VerifyGpuWait(uint64_t label_addr, uint64_t rip);

/// 5 s report lines ("SOTC NATIVE ..."), window counters reset.
void Report(std::string& out, double seconds);

} // namespace SotcNative
