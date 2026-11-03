// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREPASS_PROBE (docs/re-backend/04-depth-prepass.md, section 9). Observe only.
// Hooks (scripts/hooks.d/prepass_probe.py): PREPASS_RESOLVE_A (0xafcc0), PREPASS_RESOLVE_B (0xb0100),
// PREPASS_JOB (0xc1860, wrap), PREPASS_LIST_PASS (0x11d3d0).
#pragma once

#include <string>

namespace SotcProbes {

void RegisterPrepassProbe(); // probe_prepass.cpp, switch SOTC_PREPASS_PROBE

/// The report line(s) of the window (driver.cpp 5 s report), appended to out. Resets the window.
void ReportPrepassProbe(std::string& out, double seconds);

} // namespace SotcProbes
