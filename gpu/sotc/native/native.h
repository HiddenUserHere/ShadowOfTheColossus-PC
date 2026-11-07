// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the PC-native memory model (docs/re-stutters/native/PLAN.md). The game, patched at its own write,
// read and wait points (gpu/sotc/hook_handlers.cpp, scripts/hooks.d/native*.py), tells the port what it did
// instead of the port detecting it with page protection, fault readbacks and region compares.
//
// Contract (every caller relies on it):
// - Announce(addr, size, kind): the engine finished writing [addr, addr + size) and the GPU may read it from
//   the next submit on. Persistent data is uploaded once (upload workers, budgeted per frame). Per-frame data
//   is marked CPU-modified at the next submit, so the bind that reads it uploads its current bytes. A CPU
//   write after the announce of persistent data is a bug the verify mode counts (VerifyUnannouncedWrite).
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
#include <cstdlib>
#include <string>
#include <vector>

namespace SotcNative {

/// SOTC_NATIVE: 0 off, 1 on, 2 on + verify.
int Mode();
inline bool On() {
    return Mode() != 0;
}
bool Verify();

/// SOTC_NATIVE=1 turns the guest page tracking off: no page is write- or read-protected for the GPU,
/// no fault is handled as a tracker fault, and GPU-written bytes are never copied back to guest memory.
/// CPU writes then reach the port only through Announce (and the ring8 host imports). Mode 0 (today's UMA
/// emulation) and mode 2 (verify, the tracker still runs underneath) keep the tracking.
inline int TrackMode();

inline bool TracksGuestPages() {
    // sotc: SOTC_NATIVE_TRACK=1, diagnostic: mode 1 keeps the page protection of mode 2 (without its verify), so a
    // stale-memory cause (a write no announce covers) and a behaviour cause of a mode-1 bug can be told apart.
    // =2 keeps the protection but still skips the readbacks: a stale CPU write (fixed by =2) and a missing readback
    // (fixed by =1 only) are told apart too.
    // =3 keeps mode 1 as it is (announces, no readbacks) and only adds the protection of SOTC_NATIVE_TRACK_RANGE
    // (page_manager.cpp Protect): a write there that no announce covers is caught by its fault.
    return Mode() != 1 || TrackMode() == 1 || TrackMode() == 2;
}

/// sotc: SOTC_NATIVE_TRACK (0, 1, 2), see TracksGuestPages.
inline int TrackMode() {
    static const int track = [] {
        const char* v = std::getenv("SOTC_NATIVE_TRACK");
        return v == nullptr ? 0 : v[0] == '1' ? 1 : v[0] == '2' ? 2 : v[0] == '3' ? 3 : 0;
    }();
    return track;
}

/// sotc: the GPU-to-guest copies (readbacks) are skipped: mode 1, unless SOTC_NATIVE_TRACK=1.
inline bool SkipsReadbacks() {
    return Mode() == 1 && TrackMode() != 1;
}

/// What an announced range holds (decides the mechanism).
enum class Kind : uint8_t {
    Persistent, ///< written once (resource build, mip stream), then read-only for the GPU: upload once
    PerFrame,   ///< rewritten every frame (instances, constants, material params, DCB payloads): bind uploads
    GpuWritten, ///< the GPU writes it (render targets, compute outputs): GPU-private, never uploaded
};

/// The engine finished writing [addr, addr + size). Persistent: queued for upload, the next submit that
/// reads it waits for the upload. PerFrame: queued too, and marked CPU-modified by the draw thread (the bind
/// then uploads the current bytes). GpuWritten: nothing queued.
void Announce(uint64_t addr, uint64_t size, Kind kind);
/// Number of Announce calls so far (monotonic). Stamps queued announces; the command processor reads it when it
/// picks a submit up, and the drain counts the entries newer than that (timing audit, Report).
uint64_t AnnounceSeq();
/// A PerFrame announce made before its writer finished (at the allocation of a block, at the entry of the writer)
/// was found changed at a later check and announced again (native_hooks.cpp). same_frame: no SubmitDone came
/// between the first announce and the change (a real late write); else the engine may just have rewritten it.
void NoteLateWrite(uint64_t size, bool same_frame);

// sotc: SOTC_NATIVE_DRAIN_AHEAD=1 (read once; needs SOTC_NATIVE on, default off). A worker thread (native.cpp) takes the
// queued announces as they come, in announce order, and publishes them with the seq up to which they are all taken.
// The draw thread (BufferCache::NativeDrain, at each submit start) takes what was published, after it waited for the
// worker to reach the submit's pickup seq. PendingUpload is defined in native_draw.h.
struct PendingUpload;
/// sotc: SOTC_NATIVE_DRAIN_AHEAD=1 (and SOTC_NATIVE on). Any thread.
bool DrainAheadOn();
/// sotc: starts the worker (BufferCache constructor; no-op when the switch is off). Joins it in DrainAheadStop.
void DrainAheadStart();
/// sotc: stops the worker and joins it (BufferCache destructor; no-op when it did not start).
void DrainAheadStop();
/// sotc: one published announce: its range and its index into raw (DrainAheadTake's raw).
struct InvalRef {
    uint64_t addr;
    uint64_t size;
    uint32_t index;
};
/// sotc: one invalidation group of NativeDrain: the announces inval[begin, end) (inval sorted by address, see
/// InvalRef) lie in [lo, hi). A group stays under 1 MiB (HasImagesExact answers true past 256 pages).
struct InvalGroup {
    uint64_t lo;
    uint64_t hi;
    uint32_t begin;
    uint32_t end;
};
/// sotc: draw thread only. Waits until the worker has taken every announce with seq <= want_seq (0: no wait), then moves
/// what it took into raw (announce order, served entries included) and into ranges (the non-served ones, sorted and
/// merged per worker batch: MergeUploads). Returns how many entries of raw were published before the call.
/// inval and groups: every entry of raw (served ones too) sorted by address, and the invalidation groups over them
/// (the grouping NativeDrain used to do inline, built by the worker: native.cpp GroupInval).
size_t DrainAheadTake(uint64_t want_seq, std::vector<PendingUpload>& raw, std::vector<PendingUpload>& ranges,
                      std::vector<InvalRef>& inval, std::vector<InvalGroup>& groups);
/// sotc: draw thread. Counters of one drain: entries (and their bytes) the worker had taken ahead of the submit.
void NoteDrainAhead(uint64_t entries, uint64_t bytes);
/// sotc: any thread. Sorts taken (non-served announces) by kind, then address, and merges the overlapping or touching
/// ranges of one kind (at most 64 MiB each) into merged. taken is sorted in place. Pure: no shared state.
void MergeUploads(std::vector<PendingUpload>& taken, std::vector<PendingUpload>& merged);

/// The engine freed or decommitted [addr, addr + size): drop its device copy and any pending upload.
void Release(uint64_t addr, uint64_t size);

/// Frame ticket: FrameSubmitted is called by the port when the engine's frame N is fully submitted
/// (sceGnmSubmitDone); FrameWait(N) returns when the GPU finished frame N. Only the frame-ring reuse and
/// frame pacing may call FrameWait.
uint64_t FrameSubmitted();
void FrameWait(uint64_t frame);
uint64_t FrameCompleted();

/// Descriptor side records (framedata.md: 0x865410 / 0x8655d0 / 0x865600 / 0x865500): the engine encoded a
/// V#/T# for [base, base + size); the port keeps the record so binds never decode guest descriptor words.
void NoteDescriptor(uint64_t desc_addr, uint64_t base, uint64_t size, uint32_t format);

/// Mode 1 without tracking: a read back the port skipped (ReadMemory, DownloadMemory, texture download),
/// and a fault the tracker never caused (a write is made writable again, a read is not handled).
void NoteSkippedReadback(uint64_t addr, uint64_t size);
void NoteStrayFault(uint64_t addr, bool is_write, uint64_t rip);
/// Mode 1 without tracking: the command processor wrote guest memory (WRITE_DATA, DUMP_CONST_RAM) and told the
/// buffer and texture caches (Liverpool, the write fault of mode 0 did this); counted for the report.
void NoteCpGuestWrite(uint64_t size);
/// Mode 1: a GPU-written image whose memory the game reused (idle, fully rewritten by announces) is refreshed.
void NoteImageReused();

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
    // The flip's scheduler tick wait is the reuse fence of the ring halves (frame pacing): not reported.
};

/// 5 s report lines ("SOTC NATIVE ..."), window counters reset.
void Report(std::string& out, double seconds);

} // namespace SotcNative
