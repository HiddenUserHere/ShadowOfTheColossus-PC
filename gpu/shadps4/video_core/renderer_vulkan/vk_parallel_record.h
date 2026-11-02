// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: N5 parallel recording, counters (SOTC_N5_PARALLEL=1). The passes themselves are recorded by
// PassSecondary (vk_recorder.cpp, SOTC_PASS_SEC): the producer writes the pass blocks, a worker
// records them into a secondary command buffer, and the owner (the recorder's consumer thread)
// keeps every synchronization decision: barriers, copies and dispatches are never inside a pass, the
// BeginRendering carries VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, and the owner executes
// the secondary at EndRendering in sequence order. This file only counts what that path does, so the
// next step (more workers, pass look-ahead) can be sized from measurements.
//
// Threads: NoteFallback, NotePassParallel and NoteOwnerWait run on the owner (consumer) thread;
// NoteWorkerJob runs on the worker thread; Report may run on any thread. Counters are atomics.

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
    Count,
};

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

/// Appends one 5 s report line (the caller's window) and resets the window counters. Prints
/// nothing when Enabled() is false.
void Report(std::string& out, double seconds);

} // namespace Vulkan::ParallelRecord
