// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native XPR visibility masks, option 3 of docs/re-stutters/native/PLAN.md (approved by the user 1 Nov):
// the main-scene mask consumers draw from the last completed GPU copy of their mask (the previous frame's cull)
// instead of waiting for this frame's cull and reading GPU memory. Switch SOTC_NATIVE_MASKS=prev (needs SOTC_NATIVE).
// Copies: native_copies.h. Producer hooks NATIVE_MASK_PRODUCE(_SHADOW), the cull label hooks NATIVE_CULL_RESET /
// NATIVE_CULL_RECORDED (scripts/hooks.d/native_results.py), consumers in native_results.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace SotcNativeMasks {

/// SOTC_NATIVE_MASKS=prev, read once.
bool Prev();

/// Registers the producer handler (hook_handlers.cpp through SotcNativeResults::Register).
void Register();

/// A consumer of the main scene (BPXPR_ConsumeVisibilityMask, ...Indirect) is about to read `count` bits at
/// `mask`. Returns the guest pointer the consumer should read instead: a block holding the newest completed copy
/// of the same mask (same set entry and slot, this or an earlier frame), or a zero mask (nothing of that set is
/// drawn this frame) when no copy is complete. Verify mode (SotcNative::Verify) returns `mask` and counts the bits
/// in which the candidate differs from the real mask (read through the backing view).
uint64_t Serve(uint64_t mask, uint64_t count);

/// "SOTC NATIVE_MASKS" 5 s line (counters reset). Nothing when off.
void Report(std::string& out, double seconds);

} // namespace SotcNativeMasks
