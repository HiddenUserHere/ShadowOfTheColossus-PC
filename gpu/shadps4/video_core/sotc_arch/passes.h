// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the pass rule of SOTC_PASSES (docs/re-stutters/native/ARCH-DRAW.md section 5) and the static barrier rule of
// SOTC_BARRIER_STATIC (phase A6). Helpers beyond the contract (sotc_arch.h): the draw-path call sites need the verify
// mode and the hooks below. Implemented in passes.cpp.
#pragma once

#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::Passes {

/// True when SOTC_PASSES=2: the pass rule runs next to today's per-binding checks and counts the differences.
bool Verifying();

/// Runtime hook (Runtime::AccessBuffer, every write): a later draw or dispatch may need a global barrier. Also marks
/// a pending write for the static rule (BarrierStatic).
void NoteWrite();

/// Per-binding buffer check result, recorded for the =2 comparison. Returns `need` unchanged.
bool BufferNeed(bool need);

/// Draw thread, at the place where today's code calls FlushBarriers on needs_barrier.
/// image_need: the image side and the buffer side as today's code computed them (needs_barrier).
/// touches: the op binds at least one buffer or fixed-function read.
/// Returns whether FlushBarriers runs here. Off: returns image_need. =1: image_need or (a write since the last
/// flush and touches). =2: returns image_need and counts the pass rule's decision next to it.
bool Decide(bool image_need, bool touches);

/// Counters of the last 5 s (resets them): one line for the driver's report.
void Report(char* out, u32 size, double seconds);

/// sotc: SOTC_BARRIER_STATIC. One static rule for the buffer side of the barriers. The per-binding overlap checks and
/// the per-resource records (Runtime::AccessBuffer ranges) are replaced by two pending flags: a write (a bound stage
/// that writes memory, or any DMA, copy or mirror write) and a read (an op that binds memory). An op that reads after
/// a pending write, or writes after a pending read, is ordered by ONE global barrier (all stages, all writes) recorded
/// before it. Image barriers (Runtime::Transit) stay as they are.
/// Mode 0: off, the functions below answer as today. Mode 1: the static rule decides. Mode 2: verify, the old decision
/// is the one emitted and the static rule is counted next to it (missed, extra).
namespace BarrierStatic {

/// The mode, read once from SOTC_BARRIER_STATIC: 0 off, 1 on, 2 verify.
int Mode();
bool On();
bool Verifying();

/// True when the per-binding overlap checks run (Runtime::IsBufferAccessed in the bind paths, the addressed-write
/// hazard). They are off in mode 1 and with SOTC_PASSES=1 (whose pass rule covers them).
bool BufferChecks();

/// Draw thread, at each barrier decision of an op. image_need: the image side (Transit, render state, as today).
/// buffer_need: the per-binding side (overlap checks, addressed-write hazard); zero in mode 1.
/// touches: the op binds or reads memory (as Passes::Decide's touches). writes: a bound stage writes memory
/// (Shader::Info::writes_memory). Returns whether FlushBarriers runs before the op. In mode 1 the flush clears the
/// pending flags (Flushed, called by FlushBarriers). In mode 2 the static flags are cleared when the static rule
/// decides a flush.
bool Decide(bool image_need, bool buffer_need, bool touches, bool writes);

/// Draw thread, at the end of an op (ResetBindings, before the mirror copies that follow the op). The op's reads and
/// writes become pending for the next decisions.
void NoteOp(bool touches, bool writes);

/// Modes 1 and 2: the answer of Runtime::IsBufferAccessed for the DMA, copy and mirror paths (no range is known): a
/// pending write, or with check_reads a pending read too.
bool DmaNeeds(bool check_reads);

/// FlushBarriers, mode 1: some read or write is pending, so the barrier it records orders all of them.
bool Pending();

/// FlushBarriers, mode 1: the barrier is recorded; the pending flags are cleared. Other modes: no-op.
void Flushed();

/// Counters of the last 5 s (resets them): one line for the driver's report, empty when off.
void BarrierReport(char* out, u32 size, double seconds);

} // namespace BarrierStatic

} // namespace SotcArch::Passes
