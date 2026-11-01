// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GEN_BIND and SOTC_GEN_DMA, the 64 KiB generation table of the draw-thread design
// (docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md, V1 and M2). Every invalidation of
// buffer-cache tracked memory bumps the counters of the granules it touches; a cached answer
// that depends on guest memory stores the Sum over its range and is valid while the Sum is
// unchanged. Storage and bump rules: sotc_generation.cpp.
#pragma once

#include <cstdlib>
#include <string>

#include "common/types.h"

namespace SotcGen {

/// Granule size: 64 KiB (2^16 bytes). Guest addresses are 40 bits (BufferCache::ADDRESS_SPACE_BITS).
constexpr u64 kShift = 16;
constexpr u64 kAddrBits = 40;

/// SOTC_GEN_BIND: 0 off (default), 1 on, 2 verify (the full path runs too and counts mismatches).
/// Read from the environment at namespace scope; the initializer reads nothing but the variable,
/// so the order of initialization does not matter.
inline const int kBindMode = [] {
    const char* v = std::getenv("SOTC_GEN_BIND");
    return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
}();

/// SOTC_GEN_DMA: 0 off (default), 1 on, 2 verify (the DMA sweep syncs skipped runs too and
/// counts runs that would have uploaded bytes).
inline const int kDmaMode = [] {
    const char* v = std::getenv("SOTC_GEN_DMA");
    return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
}();

/// True for SOTC_GEN_BIND=1 or 2: BufferCache::ObtainBuffer answers clean read bindings from the
/// bind memo (buffer_cache.cpp).
inline bool On() {
    return kBindMode != 0;
}

/// True for SOTC_GEN_BIND=2: a memo hit also runs the full path and counts a mismatch when that
/// path would have uploaded bytes (a missed bump).
inline bool Verify() {
    return kBindMode == 2;
}

/// True for SOTC_GEN_DMA=1 or 2: SynchronizeDmaBuffers skips runs whose Sum did not change.
inline bool DmaOn() {
    return kDmaMode != 0;
}

/// True for SOTC_GEN_DMA=2: skipped runs are synced anyway and counted when they would upload.
inline bool DmaVerify() {
    return kDmaMode == 2;
}

/// True when any bump must happen (either switch). The bump sites test this first.
inline bool Bumps() {
    return kBindMode != 0 || kDmaMode != 0;
}

/// True when [addr, addr + size) lies inside the tracked 40-bit space. Outside it Bump is a no-op
/// and Sum does not see the range, so a cached answer must not be stored for it.
inline bool Covered(u64 addr, u64 size) {
    return size != 0 && addr < (u64{1} << kAddrBits) && size <= (u64{1} << kAddrBits) - addr;
}

/// Adds one to every granule that [addr, addr + size) touches. Any thread, lock-free. Call it
/// AFTER the store that made the invalidation visible (release): a reader that loads the Sum
/// before its sync and sees the old value is then ordered before the change.
void Bump(u64 addr, u64 size);

/// Bump for the bits of one RegionManager word: word_base is the CPU address of its first page
/// (256 KiB, aligned), page_mask has bit p set for page p of the word (4 KiB each). Bumps the
/// granules that hold a set page. Any thread, lock-free.
void BumpWordPages(u64 word_base, u64 page_mask);

/// Sum of the counters of the granules [addr, addr + size). It changes whenever any granule of
/// the range was bumped since the last call that read it (counters only grow). Any thread.
u64 Sum(u64 addr, u64 size);

/// Counters reported by Report (window, reset by Report).
enum class Stat : u32 {
    BindHit,            // memo hits that returned without a sync
    BindMiss,           // memo lookups (eligible binds) that took the full path
    BindVerifyMismatch, // SOTC_GEN_BIND=2: hits whose full path uploaded bytes (missed bump)
    DmaRunSkip,         // SOTC_GEN_DMA: runs skipped by the DMA sweep
    DmaRunSync,         // SOTC_GEN_DMA: runs synced by the DMA sweep
    DmaVerifyMismatch,  // SOTC_GEN_DMA=2: skipped runs that would have uploaded bytes
    Count
};

/// Adds n to a window counter. Lock-free.
void Count(Stat s, u64 n = 1);

/// Appends the "SOTC GEN BIND: ..." line of the last window to out and resets the counters.
/// Nothing is appended when both switches are 0. seconds = window length.
void Report(std::string& out, double seconds);

} // namespace SotcGen
