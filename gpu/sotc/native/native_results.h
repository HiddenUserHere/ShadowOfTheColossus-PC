// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU-result consumers (docs/re-stutters/native/impl-results.md): the masks fallback
// (SOTC_NATIVE_MASKS=all), the verify counters of the XPR label spins and of the water ring read.
// Declarations of scripts/hooks.d/native_results.py; handlers registered in hook_handlers.cpp.
#pragma once

#include <string>

namespace SotcNativeResults {

/// Registers the handlers of the native result hooks (gpu/sotc/hook_handlers.cpp). A hook whose
/// switch (SOTC_NATIVE) is off installs nothing, so the registration is harmless then.
void Register();

/// Appends one "SOTC NATIVE_RESULTS ..." line per consumer with its counters of the last window and
/// resets them (driver.cpp, 5 s report). Nothing is appended when no native result hook is installed.
void Report(std::string& out, double seconds);

} // namespace SotcNativeResults
