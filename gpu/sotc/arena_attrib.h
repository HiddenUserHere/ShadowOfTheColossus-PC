// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ARENA_ATTRIB=1 (off by default). Attributes the guest write faults on the per-frame
// arena (the kind-2 "Default TaggedArena" blocks, see pc_ring.h) to the guest code that caused
// them, so the writers after a submit can be named by eboot offset.
//
// Epochs. Every submit (SotcPcRing::OnSubmit, the gnmdriver submit paths) advances the epoch. An
// arena page remembers the epoch of its last allocation (fed by the RenderSingleFrameGPU heap
// wrap, the same hook as SOTC_PC_RING; see pc_ring.cpp HeapPost). A write fault on an arena page
// is then classified as:
//   rewrite  the page was allocated in an earlier epoch than the current one, so a submit has
//            been issued since the allocation (the submit may have read the bytes, the write is
//            a rewrite after it);
//   first    the page was allocated in the current epoch (no submit since): the fill of a new
//            allocation;
//   untracked  the page has no allocation seen by the hook (a heap the hook does not see).
// The granule is the 4 KiB page, so two allocations sharing a page are attributed to the newer
// one. Writes to a page that is already unprotected (CPU-dirty, between a fault and the next
// upload) do not fault and are not counted: the counts are fault counts, and each fault is one
// re-protect cycle of the page.
//
// Recording. A fixed table of 4096 slots keyed by the guest RIP of the faulting write. Per slot:
// count per class, the pages (4 KiB each), first and last frame, the faulting thread, and two
// caller addresses (frame-pointer walk from the fault context, guarded reads). Lock-free, no
// allocation on the fault path. A full table drops the fault (counted).
//
// Output (stderr, from SotcPcRing::Report every 5 s, and once at exit):
//   SOTC ARENA ATTRIB window ...                  totals of the window
//   SOTC ARENA ATTRIB REWRITE <rank> off=0x.. n=.. pages=.. thread=.. caller=0x.. caller2=0x.. frames=a-b
//   SOTC ARENA ATTRIB FIRST <rank> off=0x.. ...
//   SOTC ARENA ATTRIB FINAL REWRITE|FIRST ...     cumulative, at exit
// Offsets are eboot virtual addresses (the guest offset of the port's logs). tools/arena_attrib.py
// turns them into a ranked table with function names.
#pragma once

#include <cstdint>
#include <string>

namespace SotcArenaAttrib {

/// SOTC_ARENA_ATTRIB, read once. Off unless set to a non-zero value.
bool On();

/// Registers the exit report (atexit) when on. Called from SotcPcRing::Register.
void Init();

/// Allocator probe feed (from SotcPcRing::OnBlock / OnFreeBlock, any thread). Only the kind-2
/// "Default TaggedArena" blocks are kept.
void OnBlock(uint64_t base, uint64_t size, const char* name, int kind);
void OnFreeBlock(uint64_t base);

/// A submit (SotcPcRing::OnSubmit). Advances the epoch.
void OnSubmit();

/// A frame boundary (SotcPcRing::OnFrameBoundary).
void OnFrameBoundary();

/// An allocation returned by the RenderSingleFrameGPU heap (game thread): the pages it covers
/// inside an arena block take the current epoch as their allocation epoch.
void NoteAlloc(uint64_t addr, uint64_t size);

/// Thread id of the calling thread (0 when unavailable).
uint32_t CurrentThread();

/// Eboot offset of addr (a guest RIP or address) when it lies inside the image (the same offsets the
/// SOTC ARENA ATTRIB lines print). False when the image is not mapped or addr is outside it. Used by
/// the verify reports (verify.cpp), which print offsets the same way.
bool ImageOffset(uint64_t addr, uint64_t* offset);

/// Scope set by the page manager around a fault that it forwards to the draw thread: the write
/// fault is recorded for the faulting guest thread (thread id taken before the forward), not for
/// the thread that runs the handler. Zero means "the calling thread".
class FaultThreadScope {
public:
    explicit FaultThreadScope(uint32_t tid);
    ~FaultThreadScope();
    FaultThreadScope(const FaultThreadScope&) = delete;
    FaultThreadScope& operator=(const FaultThreadScope&) = delete;

private:
    uint32_t prev_;
};

/// A guest write fault at addr (page manager write branch, any thread). rip is the faulting
/// instruction (Common::GetRip of the context); ctx is the fault context (EXCEPTION_POINTERS on
/// Windows), read for rsp and rbp. Lock-free, no allocation.
void NoteWriteFault(uint64_t rip, void* ctx, uint64_t addr);

/// The 5 s window (appended to out, prints only when there was activity) and the cumulative
/// report at exit (SotcPcRing::Report calls the window one first).
void Report(std::string& out, double seconds);

} // namespace SotcArenaAttrib
