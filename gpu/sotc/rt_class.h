// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RT_CLASS (off by default), render targets live only in VRAM.
// See docs/re-stutters/PC-MEMORY-REWRITE.md (class table): the RTAllocator blocks hold GPU-written
// render targets allocated once per area load (render_targets_allocate 0x9aef0). Measured owner
// "RTAllocator": 0 CPU write faults and 0 CPU read faults on those pages, so the CPU never needs
// their bytes.
//
//   SOTC_RT_CLASS=0 (default): no effect.
//   SOTC_RT_CLASS=2: count only. Per 5 s, how often the texture cache downloads an image inside an
//                    RTAllocator block to guest memory, tracks it for CPU writes, or sees a CPU
//                    write invalidate it.
//   SOTC_RT_CLASS=1: apply. Render target, depth target and storage images inside an RTAllocator
//                    block are not written back to guest memory and are not CPU-tracked (their
//                    pages are not protected). A CPU write that still reaches one takes the normal
//                    path and is counted.
//
// Blocks come from the allocator probe (OnBlock / OnFreeBlock, any thread); only blocks whose
// name is "RTAllocator" are kept.
#pragma once

#include <cstdint>
#include <string>

namespace SotcRtClass {

/// SOTC_RT_CLASS: 0 (off), 1 (apply) or 2 (count only), read once.
int Mode();

/// Allocator probe feed (any thread). Keeps blocks named "RTAllocator" only.
void OnBlock(uint64_t base, uint64_t size, const char* name, int kind);
void OnFreeBlock(uint64_t base);

/// True when [addr, addr + size) lies inside one registered block (any thread).
bool Contains(uint64_t addr, uint64_t size);

/// Counters the texture cache reports through here (any thread).
void NoteDownload(uint64_t bytes);        ///< mode 2 / fallback: a download inside a block ran
void NoteDownloadSkipped(uint64_t bytes); ///< mode 1: a download inside a block was not done
void NoteTrack();                         ///< a CPU track of an image inside a block was set
void NoteTrackSkipped();                  ///< mode 1: a track of a render target was skipped
void NoteCpuInvalidate();                 ///< a CPU write invalidated an image inside a block

/// SOTC FPS 5 s report: appends one "SOTC RT CLASS: ..." line (with a newline) to `out` and resets
/// the counters. Appends nothing when the mode is 0. `seconds`: the length of the period.
void Report(std::string& out, double seconds);

} // namespace SotcRtClass
