// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the pass rule of SOTC_PASSES (docs/re-stutters/native/ARCH-DRAW.md section 5). Helpers beyond the
// contract (sotc_arch.h): the draw-path call sites need the verify mode and the hooks below. Implemented in
// passes.cpp.
#pragma once

#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::Passes {

/// True when SOTC_PASSES=2: the pass rule runs next to today's per-binding checks and counts the differences.
bool Verifying();

/// Runtime hook (Runtime::AccessBuffer, every write): a later draw or dispatch may need a global barrier.
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

} // namespace SotcArch::Passes
