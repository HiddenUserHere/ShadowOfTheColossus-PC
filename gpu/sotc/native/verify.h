// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_VERIFY, the one switch of every verify mode of the PC-native model (native.h, PLAN.md).
// verify.cpp implements it; the readers only call the inline helpers below.
//
// Parts:
//  1. VerifyAll() and FeatureMode(): the single switch and the per-reader rule "=2 if the feature is on".
//  2. Coverage oracle (native mode 2, page tracker fault path): every guest write fault is pending
//     until the next submit; an Announce of the range clears it. A pending
//     fault left at the submit is an unannounced write and is reported through
//     SotcNative::VerifyUnannouncedWrite (aggregated per guest RIP, eboot offsets). Faults are not
//     reported at fault time: the engine writes first and announces after, so a fault-time report would
//     count every ordinary write.
//     Each unannounced write then stays a suspect for kSuspectFrames frames. NoteGpuConsume (the
//     uploads of the old tracking path, which still runs in mode 2) marks the suspects whose bytes the GPU
//     takes: those are real misses ("SOTC VERIFY CONSUMED", per RIP and path). A suspect that expires was
//     never read by the GPU and needs no announce once page tracking is gone.
//  3. Per-range log (loader.md section 8): one entry per sync-copy range at its upload, capped, reported
//     per 5 s window as "SOTC VERIFY RANGE".
//  4. Frame data counters (framedata.md section 3, a-j): "SOTC FRAMEDATA" line per 5 s window.
#pragma once

#include <cstdint>
#include <string>

namespace SotcVerify {

/// SOTC_VERIFY, read once: any non-empty value other than "0" turns every verify mode on.
bool VerifyAll();

/// The per-reader rule. A reader's own switch is read as usual (0 off, 1 on, 2 verify); with SOTC_VERIFY
/// set, a feature that is on (mode 1) runs its verify mode (2). Mode 0 stays off, mode 2 stays 2.
///   int mode = <read the feature's own switch>;   // 0, 1 or 2
///   mode = SotcVerify::FeatureMode(mode);
inline int FeatureMode(int mode) {
    return (mode == 1 && VerifyAll()) ? 2 : mode;
}

// ---- 2. Coverage oracle ---------------------------------------------------------------------------

/// Page tracker, guest write fault (HandleGuestFault write branch), called when
/// SotcNative::Verify() or VerifyAll(). Writes the last-write record of the page (VerifyAll) and, under
/// SotcNative::Verify(), a pending record. Lock-free, no allocation.
void NoteWriteFault(uint64_t addr, uint64_t size, uint64_t rip);

/// The engine announced [addr, addr + size) (SotcNative::Announce) or released it (Release): pending
/// faults on those pages are covered. Called from the native API, any thread.
void NoteAnnounce(uint64_t addr, uint64_t size);

/// The engine's frame N is fully submitted (SotcNative::FrameSubmitted, the port's submit path). Resolves
/// the pending faults: each one still pending calls SotcNative::VerifyUnannouncedWrite and is cleared.
/// Advances the frame counter. Called once per submit, from the submit thread.
void OnSubmit();

/// Frames submitted so far (OnSubmit count).
uint64_t Frame();

/// Where the GPU took CPU-written guest bytes (NoteGpuConsume).
enum class ConsumePath : uint8_t { BufferUpload, ImageUpload, Count };
/// The old tracking path uploads [addr, addr + size) for the GPU (draw thread), for a bind (buffer) or an
/// image of bind_size bytes. Marks the unannounced writes in it as consumed. A small bind (<= 64 KiB) is
/// a read the shader almost surely makes; a large one may only cover the bytes (a V# over a whole heap).
/// bind_written: the bind is a shader write (the old path uploads the dirty bytes before a GPU write too, so
/// such a take is no proof of a read). Native verify only; one relaxed load otherwise.
void NoteGpuConsume(uint64_t addr, uint64_t size, ConsumePath path, uint64_t bind_size, bool bind_written);

// ---- 3. Per-range log -----------------------------------------------------------------------------

/// The publish hook (loader.md section 6a, 0x14d670): the object at addr was published in this frame.
/// Keyed by the exact base address. Capped table; off unless VerifyAll().
void NotePublish(uint64_t addr, uint64_t size);

/// One sync-copy range uploaded at bind (buffer cache sync copy, bind and dma callers). `caller` is the
/// port's upload site (a return address), `resource` the object's name ("" when unknown), `streamed` true
/// for a W4 copy. The publish frame and last write come from the tables above. No-op unless VerifyAll().
void NoteBind(uint64_t addr, uint64_t size, uint64_t bind_frame, uint64_t caller, const char* resource,
              bool streamed);

// ---- 4. Frame data counters -----------------------------------------------------------------------

/// The counters of framedata.md section 3. Each is a sum (bytes or calls) per 5 s window.
enum class Counter : uint8_t {
    DcbBytes,       // a: DCB writer cursor delta (0x86d210), split by caller
    CcbBytes,       // b: CCB writer cursor delta (0x16f100), split by caller
    HeapBytes,      // c: per-frame heap (0x8c0d0) size sum, split by caller
    HeapCalls,      // c: per-frame heap calls
    HeapFail,       // d: 0x12820 failures (the 0x124f0 fallback taken)
    HeapTierHits,   // d: 0x47e30 sub-allocator hits
    DeleterItems,   // e: items appended to the deleter (0x8ef20)
    DeleterBytes,   // e: their bytes (0x12af0)
    DeleterPeak,    // e: longest pending list seen at 0x90ea0 (a maximum, not a sum)
    ContextCalls,   // f: 0x90470 calls
    ContextBytes,   // f: DCB + CCB sizes requested
    ResourceCalls,  // g: 0x8be60 calls
    ResourceBytes,  // g: their sizes (CPU and GPU category)
    ConstWrites,    // h: constant V# writes (0x861ce0, 0x861e60)
    SlotWrites,     // h: slot writes per stage
    DispatchCalls,  // i: 0x928f0 calls
    DispatchBytes,  // i: the two buffer sizes of each call
    Count
};

/// Adds n to counter c (verify on only; the call costs one relaxed load when off).
void Count(Counter c, uint64_t n = 1);
/// As Count, and also records n under the caller address for the caller-split counters (a, b, c).
void CountCaller(Counter c, uint64_t n, uint64_t caller);

// ---- 5. Byte oracle (SOTC VERIFY BYTES) -----------------------------------------------------------
// What mode 1 would hand the GPU differently from mode 0, byte for byte. The oracle keeps a "native view"
// per 4 KiB page: the bytes the GPU holds as of the last upload of each byte (the old path's uploads, which
// run in mode 2 and read the guest bytes). At each later upload it compares the guest bytes with the view:
//   - a byte with an announce since its last refresh (announced) is refreshed: mode 1 uploads it;
//   - a changed byte with no announce on it, but an announce on its page: mode 1's page-granular upload
//     refreshes the page (covered_page), so it is not a mode 1 miss under that rule;
//   - a changed byte with no announce on its page: a miss (miss_new the first time, miss_standing while the
//     GPU keeps the stale bytes). The view keeps the old bytes, as mode 1 would.
//   - a byte a shader wrote (gpu_origin, from a write bind): mode 1 keeps the GPU's bytes until an exact
//     announce copies the byte, so a changed byte without an exact announce is a miss (miss_gpu_*).
// Only binds of at most kBytesBindMax bytes and image uploads are compared. Views are capped (kViewPageCap).
// Each miss is attributed to the RIP of the last write fault on its page (the page tracker, mode 2 only;
// "unknown" when no fault was recorded).
constexpr uint64_t kBytesBindMax = 64 * 1024;

/// The bind kind of the uploads on this thread, for the SOTC VERIFY BYTES split.
enum class BindKind : uint8_t { Vertex, Index, Constant, Storage, Texel, Image, Other, Count };
BindKind CurrentBindKind();

/// Sets the bind kind of the buffer uploads made in its scope (the rasterizer's bind sites).
class BindKindScope {
public:
    explicit BindKindScope(BindKind kind);
    ~BindKindScope();
    BindKindScope(const BindKindScope&) = delete;
    BindKindScope& operator=(const BindKindScope&) = delete;

private:
    BindKind prev_;
};

/// True when the byte oracle runs: SOTC_VERIFY and a native verify mode (SotcNative::Verify()).
bool BytesOn();

/// The old path uploads guest bytes [addr, addr + size) for the GPU; bytes = those guest bytes, read now.
/// Compares them with the native view and refreshes the view as described above. Native verify only.
void NoteUploadBytes(uint64_t addr, uint64_t size, const uint8_t* bytes, BindKind kind);

/// A shader write bind covers [addr, addr + size): the GPU now holds bytes that differ from guest memory
/// until the next upload of them (the old path reads them back there). Native verify only.
void NoteGpuWrite(uint64_t addr, uint64_t size);

/// A buffer bind above kBytesBindMax was not compared.
void NoteBytesSkipped();

// ---- Report ---------------------------------------------------------------------------------------

/// The 5 s verify lines, appended to out: "SOTC VERIFY UNANNOUNCED", "SOTC VERIFY CONSUMED", "SOTC VERIFY
/// BYTES" (part 5, byte oracle), "SOTC VERIFY RANGE", "SOTC FRAMEDATA". Resets the window counters. Prints nothing when VerifyAll() is off and no native verify ran.
void Report(std::string& out, double seconds);

/// The per-RIP aggregation of an unannounced write (called by SotcNative::VerifyUnannouncedWrite).
void AggregateUnannounced(uint64_t addr, uint64_t size, uint64_t rip);

} // namespace SotcVerify
