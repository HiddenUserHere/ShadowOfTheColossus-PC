// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the bridge between the pool's windows (pc_pool_windows.h) and the PC-native contract (native.h). It is the only
// file of the windows that calls SotcNative, so the pool logic and its unit tests do not link the native module.
#pragma once

#include <cstdint>

namespace SotcNativeWindows {

/// True when the PC-native memory model is on (SOTC_NATIVE 1 or 2). Windows are native-mode only.
bool ModeOn();

/// SotcNative::RegisterWindow for one reserved window (boot, once per window). False when the range overlaps a
/// registered window; the window still places blocks.
bool RegisterRange(uint64_t base, uint64_t size, const char* name);

/// SotcNative::ImportWindowBlock for one committed block of a window. False when the import is refused: the block
/// falls back to announced uploads and its binds take the normal path.
bool ImportBlock(uint64_t base, uint64_t size);
/// SotcNative::DropWindowBlock for one block that is freed or decommitted.
void DropBlock(uint64_t base, uint64_t size);

} // namespace SotcNativeWindows
