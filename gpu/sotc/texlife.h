// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine texture lifecycle events (SOTC_TEX_LIFECYCLE). Hooks: scripts/hooks.d/texlife.py,
// handlers and registry: texlife.cpp. Engine facts: docs/re-backend/08-texture-lifecycle.md.
//
// Mode 0 (default): nothing runs. Mode 1: an engine texture (CTexture, static, not a render target)
// whose registry generation is the one its texture-cache image was last refreshed at skips the
// clean-image RefreshImage call (texture_cache.h UpdateImage). Mode 2: today's path always runs, and
// the event registry is compared with it (missed events are counted).
#pragma once

#include <cstdint>
#include <string>

namespace SotcTexLife {

/// SOTC_TEX_LIFECYCLE: 0 off, 1 fast, 2 verify. Read once.
int Mode();

/// The registry generation of the static engine texture at guest base address `base` (T# dword0
/// << 8), or 0 when no live static engine texture has that base (unknown, render target, or
/// not an engine texture). The generation changes on every create, level, mip and destroy event.
uint32_t StaticGen(uint64_t base);

/// Counters: a fast validation (mode 1) was taken, or a verify check (mode 2) was made. For a
/// verify check, missed is true when the image was CPU- or GPU-dirty while the generation had not
/// changed since its last refresh (an event that the hooks did not see).
void NoteFast();
void NoteVerify(bool missed);

/// Appends the report lines (driver.cpp 5 s report). Nothing when the mode is 0. The counters are
/// cumulative since launch.
void Report(std::string& out);

/// Registers the TEXLIFE_* handlers. Called from RegisterAllHandlers (hook_handlers.cpp).
void RegisterTexLife();

} // namespace SotcTexLife
