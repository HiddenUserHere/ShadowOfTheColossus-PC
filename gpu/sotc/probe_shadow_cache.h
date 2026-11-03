// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: sun shadow cascade cache probe (SOTC_SHADOW_CACHE, docs/re-backend/12-shadow-cache.md).
// Hook SHADOW_CASCADE_BODY (scripts/hooks.d/shadow_cache.py), entry of RenderShadowCascade (0xcdbc0).
#pragma once

#include <string>

namespace SotcProbes {

/// Registers the SHADOW_CASCADE_BODY handler. Called from RegisterAllHandlers (hook_handlers.cpp).
void RegisterShadowCache();

/// Appends the "SOTC SHADOW CACHE" line for the last window and resets it. Prints nothing when
/// the hook is not installed. seconds = window length.
void ReportShadowCache(std::string& out, double seconds);

} // namespace SotcProbes
