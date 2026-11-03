// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: frame ring probe (SOTC_FRAME_RINGS, docs/re-backend/10-ring-reuse.md). Observe only.
#pragma once

#include <cstdint>
#include <string>

namespace SotcFrameRings {

/// Registers the RING_INIT handler (scripts/hooks.d/frame_rings.py).
void Register();

/// SOTC_FRAME_RINGS: 0 off, 1 and 2 both measure. No mode skips any write tracking (see the doc:
/// the fast path is not implemented because reuse of the CPU ring is not guarded by a GPU wait).
int Mode();

/// Called at the top of BufferCache::RecordCurrentWrite. Counts the record when it lies inside
/// the CPU frame ring (fast ops F). Counts only; nothing is skipped.
void NoteWrite(uint64_t addr, uint64_t size);

/// Called from BufferCache::NativeExactUpload (the CPU copy path that also calls
/// RecordCurrentWrite). Counts the in-ring records that came from that path, so X excludes them.
void NoteCpuPath(uint64_t addr, uint64_t size);

/// Appends the "SOTC FRAME RINGS" line (5 s report) and resets the window counters.
void Report(std::string& out, double seconds);

} // namespace SotcFrameRings
