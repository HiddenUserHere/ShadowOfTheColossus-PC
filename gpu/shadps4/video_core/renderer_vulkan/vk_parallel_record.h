// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: N5 parallel recording, counters (SOTC_N5_PARALLEL=1). The passes themselves are recorded by
// PassSecondary (vk_recorder.cpp, SOTC_PASS_SEC): the producer writes the pass blocks, a worker
// records them into a secondary command buffer, and the owner (the recorder's consumer thread)
// keeps every synchronization decision: barriers, copies and dispatches are never inside a pass, the
// BeginRendering carries VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, and the owner executes
// the secondary at EndRendering in sequence order. This file only counts what that path does; the
// pass decisions, the worker pool and the look-ahead are in PassSecondary (vk_recorder.cpp).
//
// Threads: NoteFallback, NotePassParallel, NoteOwnerWait, NoteLookahead and NoteLookaheadPrepare run
// on the owner (consumer) thread; NoteWorkerJob runs on a worker thread; NoteVerify runs on the owner;
// Report may run on any thread. Counters are atomics.

#include <string>

#include "common/types.h"

namespace Vulkan::ParallelRecord {

/// Why a dynamic rendering pass (a BeginRendering the producer tagged for SOTC_PASS_SEC) stayed
/// on the owner.
enum class Fallback : u8 {
    FormatsInvalid, ///< no PassFormats were set for the BeginRendering
    Dropped,        ///< the command buffer's replay table was pruned (PassSecondary::Prune)
    ScanStopped,    ///< shutdown interrupted the scan of the pass blocks
    ScanTimeout,    ///< the producer did not publish the end of the pass in time
    ScanChunks,     ///< the pass spans more chunks than the scan takes
    ScanBlock,      ///< a block a secondary cannot hold (see the block category counts)
    FewDraws,       ///< the pass has no more draws than SOTC_PASS_SEC_MIN
    Workers,        ///< sotc: SOTC_PASS_WORKERS: every open-pass slot is taken
    Ring,           ///< sotc: the chunk ring is nearly full, a new pass would pin more chunks
    Count,
};

/// Why the look-ahead stopped (a stop event: the same stop can be counted again at the next call)
enum class LaStop : u8 {
    Other,      ///< a block of category Other (BeginRecorded, submits): the walk runs it
    Prune,      ///< an insert that needs a prune of the replay table: the walk does it
    Queue,      ///< every open-pass slot is taken
    Ring,       ///< the chunk ring is nearly full
    Incomplete, ///< the producer has not published the end of the next pass yet
    Count,
};

/// SOTC_PASS_WORKERS=N (1..8, or "auto" = min(4, logical cores / 4)): the pass worker threads, and
/// how many passes are open at once; the look-ahead runs from N = 2. Unset: 1 (one worker, no
/// look-ahead). Needs SOTC_PASS_SEC=1; otherwise 1. Read once.
int Workers();

/// SOTC_PASS_VERIFY=1: every pass job is checked (state snapshot against the consumer table, and the
/// blocks the worker executed against a reference hash). Read once.
bool Verify();

/// The counters and the clock reads of the pass path are on: Enabled(), or more than one worker, or
/// Verify().
bool Counted();

/// Owner: the look-ahead stopped for why (PassSecondary::LookAhead).
void NoteLookahead(LaStop why) noexcept;

/// Owner: the look-ahead prepared a pass; depth is the number of open passes after it.
void NoteLookaheadPrepare(u64 depth) noexcept;

/// Owner, at each EndRendering of a verified job: state_ok is the snapshot check at the start of the
/// pass, run_ok the worker's block hash against the reference.
void NoteVerify(bool state_ok, bool run_ok) noexcept;

/// SOTC_N5_PARALLEL=1 (parallel) or 2 (dry run), with SOTC_NATIVE on, SOTC_N5_GRAPH set,
/// SOTC_PARALLEL_RECORD=1 and SOTC_PASS_SEC=1 (the pass secondaries). Read once; logs once which
/// prerequisite is missing. Mode 0 when off or when a prerequisite is missing.
int Mode();

/// Mode() != 0: the counters and the clock reads of the counted paths are on.
bool Enabled();

/// Mode 2: every pass runs the parallel checks and is counted as would-parallel, then is recorded
/// on the owner as before (nothing goes to a worker).
bool Dry();

/// Owner: a pass that was not recorded on a secondary, and why. block: the BlockCategory index of
/// the blocking command when reason is ScanBlock, otherwise ignored.
void NoteFallback(Fallback reason, u8 block = 0xFF) noexcept;

/// Owner: a pass handed to a worker.
void NotePassParallel() noexcept;

/// Owner, mode 2: a pass that passed every check; draws and blocks are its draw and command
/// block counts (what a worker would have recorded).
void NoteWouldParallel(u64 draws, u64 blocks) noexcept;

/// Worker: one pass recorded; ops is the number of commands replayed (state entries included).
void NoteWorkerJob(u64 ops, u64 ns) noexcept;

/// Owner: time spent waiting for a worker at EndRendering.
void NoteOwnerWait(u64 ns) noexcept;

/// SOTC_N5_PARALLEL_TRANSLATE=1: the producer emission census is on (read once). Mode 2 (verify of
/// a worker translation) is not implemented: no op is translated off the draw thread yet.
bool TranslateCensus();

/// Monotonic clock in ns for the census timers (producer side only).
u64 NowNs() noexcept;

/// Producer (draw thread), CommandRecorder::Allocate: one block of the given category (a
/// BlockCategory index), its total bytes including header, and the ns spent writing it. Only the
/// header write is timed, not the wait for a free chunk (NextChunk).
void NoteEmit(u8 category, u32 bytes, u64 ns) noexcept;

/// sotc: SOTC_PASS_RECORD=1 with SOTC_PACKET=1 (SotcArch::Passes::ParallelRecord() and
/// SotcArch::Packet::On(), read once). True: the packet-pass counters and the stitch timer run, for
/// the worker recording of packet ops (ARCH-DRAW A6). False: nothing here runs.
bool PacketRecord();

/// Owner, at a BeginRendering that passed the scan (PacketRecord() only): whether the pass binds no
/// descriptor set (the packet shape) and its draw count.
void NotePacketPass(bool descriptor_free, u64 draws) noexcept;

/// Owner, at EndRendering (PacketRecord() only): ns from executing the secondary to the replay of
/// the state table on the primary (the stitch of the pass into the ordered stream).
void NoteStitch(u64 ns) noexcept;

/// Appends the 5 s report lines (the caller's window) and resets the window counters. Each group
/// prints nothing when its switches are off (Enabled() for the N5 lines, PacketRecord() for the packet
/// line).
void Report(std::string& out, double seconds);

} // namespace Vulkan::ParallelRecord
