// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the bridge between the pool's windows (pc_pool_windows.h) and the PC-native contract (native.h). It is the only
// file of the windows that calls SotcNative, so the pool logic and its unit tests do not link the native module.
#pragma once

#include <cstdint>

namespace SotcNativeWindows {

/// True when the PC-native memory model is on (SOTC_NATIVE 1 or 2). Windows are native-mode only.
bool ModeOn();

/// SotcNative::RegisterWindow for one reserved window (boot, once per window). False when the import is refused: the
/// window still places blocks, and its data falls back to announced uploads.
bool RegisterRange(uint64_t base, uint64_t size, const char* name);

} // namespace SotcNativeWindows
