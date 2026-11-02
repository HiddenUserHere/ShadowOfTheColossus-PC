// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BARRIER_GUEST. Barriers at the game's own sync points, with a hashed page bitmap as the safety net.
//
// Mode 0 (default): nothing here runs; the barrier rules are today's.
// Mode 1: a guest sync point (EVENT_WRITE_EOP/EOS, EVENT_WRITE partial and cache flushes, ACQUIRE_MEM, RELEASE_MEM,
//         DMA_DATA with CP sync) orders the pending buffer writes with ONE global memory barrier, recorded at the
//         packet's place in the op stream (Liverpool::QueueGuestSync -> Rasterizer::GuestSyncPoint). Per-op buffer
//         hazard checks (IsBufferAccessed, GuestWriteOverlap, Passes::BufferNeed) are off. An op still gets a barrier
//         from the safety net when it reads a range written since the last barrier (RAW), or writes a range written
//         or read since then (WAW, WAR). Host-originated operations keep their own barriers.
// Mode 2: verify. Today's barriers are emitted; the new rule runs next to them and counts the differences.
//
// The bitmaps are hashed: a granule (64 KiB of guest address space) maps to one bit of 2^20. A collision only adds a
// barrier, never drops one. Both maps are cleared together when a memory barrier is recorded.
#pragma once

#include <span>
#include <utility>

#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::GuestBarrier {

/// The kinds of guest sync point the command processor queues (counted per kind in the report).
enum class Sync : u32 { Eop, Eos, Event, Acquire, Release, Dma, Count };

/// The mode, read once from SOTC_BARRIER_GUEST: 0 off, 1 on, 2 verify.
int Mode();
bool On();
bool Verifying();
/// Mode 1 or 2: the guest sync markers are queued and the safety net runs.
bool Active();

// Draw thread. Every buffer access (Runtime::AccessBuffer) and every addressed write feeds the safety net.
void NoteAccess(VAddr addr, u64 size, bool write);
/// A write without a known range (the flag only): the next guest sync point orders it.
void NoteWritePending();

/// True when a guest sync point would find a pending write (mode 1: the barrier is due; verify: simulated).
bool WritePending();

/// Draw thread, at a guest sync point (marker). Counts the kind. Returns true when the caller must record the global
/// barrier now (mode 1, a pending write). Verify: a pending write is simulated as ordered and counted, returns false.
bool SyncPoint(Sync kind);

/// The safety net check of one op: true when a read hits a granule written since the last barrier, or a write hits one
/// written or read since then. unknown_reads: the op reads memory with no known range, so any pending write counts.
bool NetHit(std::span<const std::pair<VAddr, u64>> reads, std::span<const std::pair<VAddr, u64>> writes,
            bool unknown_reads);

/// Draw thread, one call per barrier decision of an op. image: the image side (needs_barrier). net: NetHit.
/// buffer_old: today's per-binding side (verify only). old_due: today's decision (verify only). Returns whether the
/// op flushes (mode 1: image or net; verify: the simulated new rule, which is not emitted).
bool Decision(bool image, bool net, bool buffer_old, bool old_due);

/// Runtime::FlushBarriers recorded a memory barrier (mode 1 only): every pending write and read is ordered now.
void Recorded();

/// Counters of the last 5 s (resets them): one line for the driver's report. Empty when off.
void Report(char* out, u32 size, double seconds);

} // namespace SotcArch::GuestBarrier
