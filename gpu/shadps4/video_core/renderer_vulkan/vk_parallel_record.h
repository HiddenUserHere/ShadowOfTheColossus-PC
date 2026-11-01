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

/// SOTC_N5_PARALLEL=1, with SOTC_NATIVE on, SOTC_N5_GRAPH set, SOTC_PARALLEL_RECORD=1 and
/// SOTC_PASS_SEC=1 (the pass secondaries). Read once; logs once which prerequisite is missing.
bool Enabled();

/// Owner: a pass that was not recorded on a secondary, and why. block: the BlockCategory index of
/// the blocking command when reason is ScanBlock, otherwise ignored.
void NoteFallback(Fallback reason, u8 block = 0xFF) noexcept;

/// Owner: a pass handed to a worker.
void NotePassParallel() noexcept;

/// Worker: one pass recorded; ops is the number of commands replayed (state entries included).
void NoteWorkerJob(u64 ops, u64 ns) noexcept;

/// Owner: time spent waiting for a worker at EndRendering.
void NoteOwnerWait(u64 ns) noexcept;

/// Appends one 5 s report line (the caller's window) and resets the window counters. Prints
/// nothing when Enabled() is false.
void Report(std::string& out, double seconds);

} // namespace Vulkan::ParallelRecord
