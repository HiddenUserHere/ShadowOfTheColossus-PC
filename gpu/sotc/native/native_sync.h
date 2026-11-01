// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU-GPU synchronization of the PC-native engine (docs/re-stutters/native/sync.md section 6,
// IMPL-BRIEF N3). The only CPU wait on the GPU is SotcNative::FrameWait; these helpers decide which
// frame ticket each submit path and the engine's frame-slot spin wait on. Active only with SOTC_NATIVE.
#pragma once

#include <cstdint>
#include <string>

namespace SotcNativeSync {

/// sceGnmSubmitDone with SOTC_NATIVE on: the frame closed by the guest gets its ticket (FrameSubmitted).
void OnSubmitDone();

/// Frame reuse wait before the guest builds a new frame (command buffer submit, compute doorbell):
/// the frame two back used the same DCB slot, so FrameWait(closed - 1). Replaces GateWait with native on.
void WaitReuse();

/// sceGnmAreSubmitsAllowed with SOTC_NATIVE on: true when the frame slot of the next frame is free
/// (the frame two back is complete on the GPU). Never blocks.
bool SubmitsAllowed();

/// Counters of the window (SOTC NATIVE SYNC line for the 5 s report); resets them.
void Report(std::string& out, double seconds);

/// Registers the hook handlers of scripts/hooks.d/native_sync.py (called from RegisterAllHandlers).
void Register();

} // namespace SotcNativeSync
