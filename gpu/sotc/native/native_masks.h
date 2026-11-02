// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native XPR visibility masks, option 3 of docs/re-stutters/native/PLAN.md (approved by the user 1 Nov):
// the main-scene mask consumers draw from the last completed GPU copy of their mask (the previous frame's cull)
// instead of waiting for this frame's cull and reading GPU memory. Switch SOTC_NATIVE_MASKS=prev (needs SOTC_NATIVE).
// Copies: native_copies.h. Producer hooks NATIVE_MASK_PRODUCE(_SHADOW/_DECAL), the cull label hooks NATIVE_CULL_RESET /
// NATIVE_DECAL_RESET / NATIVE_CULL_RECORDED (scripts/hooks.d/native_results.py), consumers in native_results.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace SotcNativeMasks {

/// SOTC_NATIVE_MASKS=prev or =cpu, read once: the consumers are served without waiting for the GPU.
bool Prev();

/// SOTC_NATIVE_MASKS=cpu, read once: the masks of the current frame are computed on the CPU (cpu_cull.h) and served
/// first; the last completed GPU copy is the fallback. Off by default until its cost and the LOD mirror's agreement
/// are good enough (docs/re-stutters/native/CPUCULL-BRIEF.md).
bool Cpu();

/// Registers the producer handler (hook_handlers.cpp through SotcNativeResults::Register).
void Register();

/// A consumer of the main scene (BPXPR_ConsumeVisibilityMask, ...Indirect) or of the decals (sub_1a8a40) is about to read `count` bits at
/// `mask`. Returns the guest pointer the consumer should read instead. First choice: the CPU mask of the same frame
/// (computed at the producer site, cpu_cull.h), when it exists for the same object and count (CPUCULL-BRIEF). Else a
/// block holding the newest completed GPU copy of the same mask (same set entry and slot, this or an earlier frame),
/// or a zero mask (nothing of that set is drawn this frame) when no copy is complete. Verify mode (SotcNative::Verify)
/// returns `mask`; a served CPU mask is compared bit by bit with the real GPU mask of its frame once that copy is
/// complete (SOTC NATIVE_MASKS verify line).
uint64_t Serve(uint64_t mask, uint64_t count);

/// "SOTC NATIVE_MASKS" 5 s lines (counters reset): the copy ring, the CPU mask cost and the verify. Nothing when off.
void Report(std::string& out, double seconds);

} // namespace SotcNativeMasks
