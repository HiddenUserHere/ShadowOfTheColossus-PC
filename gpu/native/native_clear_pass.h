// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. The colour strip the fullscreen rect 0x93690 draws inside the
// engine's strip helpers: the same triangle the clear passes draw, but its effect is a screen-area
// fill of the bound targets. This module REPLAYS that strip as a rendering scope over the targets
// the guest bound (eLoad: what is under the strip stays), while the emulated path still serves the
// targets and the shader binds. Once those stages are native the scope carries the real draw.
//
// The strip helpers are patched at their head (kColorStripVa) by the native renderer when it is on
// for that entry (the SOTC_RENDER stage "DualPass" patches nothing yet; see the gap notes): the
// patched head lets the guest body run once and the replay is recorded after it.
#pragma once

#include "common/types.h"
#include "native/native_pass.h"

namespace Vulkan {
class Pipeline;
} // namespace Vulkan

namespace Native {

/// The strip helper head 0x1431f0 (prologue `push rbp; mov rbp,rsp; push r15; push r14; push r13;
/// push r12; push rbx; sub rsp,0x28`). The replay is recorded when the helper returns to it.
constexpr u64 kColorStripVa = 0x1431f0;

/// Decodes one 0x92140 target manager M (9 descriptor cells + the selector qword) into the
/// attachment set: the RT binder's decode (RtDerive) without the viewer-wide extent sanity test,
/// because a strip binds whatever the guest bound.
bool DecodeTargets(const u64* targets, TargetSet& set);

/// Records a rendering scope over the set's resolved views through the stream (frame order). With
/// a pipeline the fullscreen rect is drawn inside the scope; without one the scope only
/// establishes the attachments. Mutates texture-cache state.
bool ReplayTargets(const TargetSet& set, const Vulkan::Pipeline* pipeline);

} // namespace Native
