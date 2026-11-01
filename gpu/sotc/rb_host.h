// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RB_HOST, component R1 of docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md: GPU_TO_CPU
// results without read faults. The game reads the XPR visibility masks and the particle count arrays
// with plain loads. Today the port protects the pages the GPU writes, so such a load faults and the
// draw thread does a readback first. Here the GPU writes those bytes into memory the CPU reads in
// place (imported host pages, the ring8 path), so the load finds them there and no readback runs.
//
// Mode 1 (SOTC_RB_HOST=1, needs SotcLabels::Honest(), label_honest.h; refused otherwise, one log line):
//   - Particle stats arrays. The requests of particle_prev.cpp (RequestDispatched) name the array the
//     count pass of this call writes. NoteStatsRange registers its 64 KiB granules as host runs (LRU,
//     kMaxRuns). The draw thread publishes them with Runs() as extra ring8 runs (pc_ring SetHostExtras,
//     see the call-site snippet in the report). A dispatch that binds such a run is then served from the
//     imported buffer (BufferCache::PcHostBind), so its writes land in the guest pages with no GPU
//     tracking, and the game's read two calls later finds them there.
//   - XPR masks. They live in the kind-1 "Default TaggedArena" blocks (SotcRbClass registry), which
//     ring8 imports as arena blocks already (assumed from the code, not measured). NoteMaskPointer counts
//     how many mask pointers BuildSetDraws writes fall in a host-served run, so the report says whether
//     the existing path already covers them.
//   - Ordering: the CPU read is not preceded by a GPU wait. NoteServedWrite reports every served write of
//     a result range to SotcLabels::NoteSharedTouch, so the labels after it are deferred until the GPU has
//     done that work (label_honest.h). That is the only ordering argument; it is not measured yet.
//
// Mode 2 (SOTC_RB_HOST=2, verify): the normal path stays, so the GPU buffer write is tracked and the
//   readback still runs. In addition a GPU copy after the producing dispatch mirrors the bytes into the
//   imported run (RecordMirrorCopy). At each readback apply, the bytes already in the guest pages (the
//   mirror) are compared with the bytes the readback brings (CompareApplied). Mismatched bytes must be 0.
//
// Counters are printed by Report (one "SOTC RB HOST" line per 5 s window, from the FPS report).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rb_class.h"                                 // sotc: SotcRbClass::Range
#include "video_core/renderer_vulkan/vk_common.h" // sotc: vk::CommandBuffer, vk::Buffer

namespace SotcRbHost {

/// SOTC_RB_HOST, read once: 1 = mode 1, 2 = mode 2 (verify), otherwise 0 (off).
int Mode();
/// Mode() != 0.
bool On();
/// Mode 1 may serve result ranges: SotcLabels::Honest(). Logs once when mode 1 is refused for it.
bool MayServe();

/// Particle stats (particle_prev.cpp RequestDispatched): the array of a count pass this call dispatched,
/// base address and bytes (the requested extent). Draw-thread-safe and game-thread-safe (mutex).
void NoteStatsRange(uint64_t addr, uint64_t size);

/// XPR masks (probe_xprmem.cpp ReadMasks): one mask pointer and its bytes (4 * ceil(n / 32)). Counts
/// whether a host-served run already covers it. Any thread.
void NoteMaskPointer(uint64_t addr, uint64_t size);

/// The runs to publish as extra host runs (pc_ring SetHostExtras), sorted and merged, 64 KiB aligned.
/// Empty unless mode 1 may serve. Draw thread.
std::vector<SotcRbClass::Range> Runs();

/// True when [addr, addr + size) lies in a stats run (Runs) or in a mask-arena block (SotcRbClass). Draw
/// thread (the arena snapshot is cached per thread).
bool Covers(uint64_t addr, uint64_t size);

/// Draw thread, at a served write (BufferCache::PcHostBind, is_written): in mode 1, a write into a covered
/// range is reported to the label deferral (SotcLabels::NoteSharedTouch).
void NoteServedWrite(uint64_t addr, uint64_t size);

/// Mode 2 (readback apply): copies the bytes the guest pages hold for [addr, addr + size) before the
/// readback writes them (the mirror). False when not covered or unreadable (then nothing is compared).
bool SnapshotBefore(uint64_t addr, uint64_t size, std::vector<uint8_t>& out);
/// Mode 2: compares the snapshot with the bytes the readback applied (applied = the same guest bytes,
/// after the copy). Counts the mismatches and logs the first few.
void CompareApplied(uint64_t addr, const std::vector<uint8_t>& before, const uint8_t* applied);

/// Mode 2: records the GPU copy of [src_offset, src_offset + size) in src into the imported run holding
/// guest [addr, addr + size), after a shader-write to transfer-read barrier. Call from the command
/// buffer recording of the dispatch that wrote the range (Scheduler::Record). False when mode 2 is off,
/// not covered, or not imported (then nothing is recorded and no compare happens).
bool RecordMirrorCopy(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr,
                      uint64_t size);

/// Appends the SOTC RB HOST report line(s) (window counters, reset) to out. Writes nothing when off.
void Report(std::string& out, double seconds);

} // namespace SotcRbHost
