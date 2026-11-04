// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_GATE=1 (off by default; needs SOTC_PRELOAD_UPLOAD=2, whose idle uploader does the staged copies).
// Entering a new area, the first frame that draws it copies ~100 MiB of guest buffers (vertex, index and per-object
// data nobody uploaded yet) on the draw thread: each bind syncs its bytes before the draw. The data does not have to be
// on screen in that very frame: a draw whose buffers still hold more than SOTC_DRAW_GATE_MIN_KB (default 256) of bytes
// to upload, once the frame's draws have taken SOTC_DRAW_GATE_MB (default 10) of such uploads, is not recorded; its
// ranges go to the front of the idle uploader's queue (1 MiB pieces, at most SOTC_PRELOAD_UPLOAD_MB per frame), and
// the draw is made in a later frame, when they are clean. Rules:
//   - never deferred: a draw whose shaders write memory (its writes may feed later work), a draw of 6 indices or
//     fewer (full-screen passes), and, by the budget, a draw with an EQUAL depth test (it depends on a depth prepass
//     drawn before it);
//   - frame consistency: once a range is deferred in a frame, every later draw of that frame that reads it is
//     deferred too (EQUAL-depth draws included), even when the idle uploader cleaned it meanwhile. The prepass, colour
//     and shadow draws of an object are all skipped or all made, never a colour draw over a missing prepass;
//   - cap: a range deferred SOTC_DRAW_GATE_FRAMES (default 3) frames ago is drawn at once, whatever the budget (its
//     draw then syncs what is left itself, as before). A deferred object is missing for at most that many frames.
// Correctness of the uploads is the idle uploader's (BufferCache::PreuploadRange: a range the GPU wrote is never
// overwritten from guest memory, a later CPU write is seen again by page tracking or SOTC_NATIVE announces).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace SotcDrawGate {

/// SOTC_DRAW_GATE=1 and SOTC_PRELOAD_UPLOAD=2, read once.
bool On() noexcept;

/// A guest range a draw reads, with its bytes still to upload (BufferCache::PendingUploadBytes).
struct Range {
    uint64_t addr;
    uint64_t size;
    uint64_t pending;
};

/// Draw thread, for one draw: true when it is not to be recorded (its pending ranges were queued for the staged
/// upload). may_defer: the budget may defer it; may_follow: the frame rule applies (false for a draw that writes
/// memory or a full-screen pass, which are always made). frame: the presented frame count.
bool Decide(const Range* ranges, size_t count, bool may_defer, bool may_follow, uint64_t frame) noexcept;

/// The 5 s report line (driver.cpp); nothing when off.
void Report(std::string& out, double seconds);

} // namespace SotcDrawGate
