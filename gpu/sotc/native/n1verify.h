// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: diagnostic (flicker brief 2, 1 Nov): the "N1VERIFY" sub-mode of mode 1, SOTC_NATIVE=1 SOTC_VERIFY=n1.
//
// Enabled by SOTC_NATIVE=1 together with SOTC_VERIFY=n1 (case-insensitive "n1"; SotcVerify::VerifyAll() ignores
// that value, so no other reader changes its mode and every mode-1 mechanic stays as it is).
//
// What it adds on top of mode 1, and nothing else:
//  - The page tracker's protection runs again for the pages the buffer cache and the texture cache watch,
//    exactly where mode 0 would protect them (after an upload, after a bind that marks bits), but the fault
//    handler (page_manager.cpp, HandleN1VerifyFault) only RECORDS and opens the page again. It marks nothing,
//    invalidates nothing, reads nothing back: mode 1 stays mode 1.
//  - Write fault (a CPU write to a page whose bytes the GPU holds): RIP, page, frame, thread class
//    (guest code, host code on the GPU-side threads, host code on any other thread). The write is "announced" when
//    a SotcNative::Announce that covers the faulting address comes before the next submit start
//    (the draw thread's NativeDrain point). One that does not is an unannounced write in mode 1. It then
//    becomes a suspect; if a buffer bind or an image bind of the GPU covers its page later and no announce
//    came meanwhile, it is a miss: the GPU reads the page although mode 1 never learned of the write.
//  - Read fault (a CPU read of a page the GPU wrote; the read protection of GPU-modified bits): RIP, page.
//    Mode 1 serves the stale guest bytes there (no readback).
//  - After a fault the OS protection stays open until the next submit start, where the pages are protected
//    again from the tracker's state (PageManager::N1VerifyRearm). So a page yields at most one record per
//    submit interval; a second write to the same page inside the interval is not seen.
//
// Frame-source detector (jitter brief, 1 Nov), the part that asks "does a draw of frame N get bytes of another
// frame, and from which writer?". It needs the draw thread's frame index: R = (last frame end the draw thread
// recorded) + 1, from BufferCache::PcHostNoteFrameRecorded. That is exact only with frame tickets
// (SOTC_CP_AHEAD=2 or SOTC_FRAME_AHEAD=1); the "FRAMES" line prints rec_jumps, which must be ~0 for R to be exact.
//  - Announce record per page (Announce, CPU side): the CPU frame, the announcer (host return address), a sample
//    hash of the announced bytes, drained / first-bind state. Checked at each bind and each drain.
//  - Last-write record per page (the write fault): CPU frame and writer RIP; checked at each bind.
//  - Allocator classes: probe_alloc / probe_memtype feed the blocks, so "bound_miss" rows name the allocator.
// Reading the output: see the "SOTC N1VERIFY" lines of Report(); the guide is at the top of n1verify.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace SotcN1Verify {

/// sotc: the environment read of SOTC_NATIVE=1 and SOTC_VERIFY=n1 (n1verify.cpp; prints the banner when on).
bool InitOn();
/// sotc: SOTC_NATIVE=1 and SOTC_VERIFY=n1, read once at static initialisation. It was a function-local static
/// behind a call on every per-bind guard; now On() is a plain load of this flag in every translation unit.
inline const bool kOnFlag = InitOn();
/// SOTC_NATIVE=1 and SOTC_VERIFY=n1.
inline bool On() { return kOnFlag; }

/// Who made the access: decided from the faulting RIP and thread.
enum class Who : uint8_t {
    Guest,     ///< RIP inside the guest image (eboot): the game's own code
    HostGpu,   ///< port code on the GPU-side threads (draw thread, command processor)
    HostOther, ///< port code on any other thread (runtime memcpy/memset, file reads, ... on guest threads)
};
Who Classify(uint64_t rip, bool gpu_side_thread);

/// Fault handler, any thread, lock-free and allocation-free. The page stays recorded for the re-protection.
void NoteWriteFault(uint64_t addr, uint64_t rip, Who who);
void NoteReadFault(uint64_t addr, uint64_t rip, Who who);
/// A fault the handler did not keep: same thread, RIP and page over and over (guard against a loop), or a
/// fault of a page the tracker does not want protected (racing unprotect).
void NoteLoopBreak();
void NoteRaced();

/// Announce kinds (the values of SotcNative::Kind, plus the port's own host writes).
constexpr uint8_t kAnnPersistent = 0;
constexpr uint8_t kAnnPerFrame = 1;
constexpr uint8_t kAnnGpuWritten = 2;
constexpr uint8_t kAnnDirect = 3; ///< a host write the rasterizer made itself (FillBuffer / CopyBuffer)

/// SotcNative::Announce: [addr, addr + size) was announced by the engine (any kind, any thread). caller is the
/// announcer (the host return address of Announce, or a small tag for the rasterizer's own writes).
void NoteAnnounce(uint64_t addr, uint64_t size, uint8_t kind = kAnnDirect, const void* caller = nullptr);
/// SotcNative::Release: the range was freed or decommitted; its announce records are dropped.
void NoteRelease(uint64_t addr, uint64_t size);
/// Draw thread, BufferCache::NativeDrain: one queued piece was taken (exact: copied byte-exact, else page-marked
/// or synced). Checks the drain against the announce records of its pages.
void NoteDrain(uint64_t addr, uint64_t size, bool exact);
/// Draw thread, PcHostNoteFrameRecorded: the draw thread finished recording frame seq (SubmitDone count).
void NoteFrameRecorded(uint64_t seq);

/// Allocator feed (probe_alloc.cpp: every tracked block; probe_memtype.cpp: every claimed granule). Any thread.
void OnBlock(uint64_t base, uint64_t size, const char* name, int kind, int slot);
void OnFreeBlock(uint64_t base);
void OnGranule(uint64_t base, uint64_t size, const char* pool);

/// Draw thread, at each submit start after NativeDrain: resolves the pending writes (announced or not),
/// then protects the faulted pages again.
void OnSubmitStart();

/// The GPU binds [addr, addr + size) of guest memory: a buffer bind (ObtainBuffer; the bind kind comes from
/// SotcVerify::CurrentBindKind) or an image (UpdateImage). via_memo: the bind was answered from the bind memo
/// (no ObtainBuffer call, no upload). Draw thread.
void NoteBind(uint64_t addr, uint64_t size, bool image, bool via_memo = false);

/// "SOTC N1VERIFY" lines for the 5 s window; resets the window counters. Prints nothing unless On().
void Report(std::string& out, double seconds);

} // namespace SotcN1Verify
