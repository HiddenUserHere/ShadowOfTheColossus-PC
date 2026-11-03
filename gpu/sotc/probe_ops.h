// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OPS_PROBE (docs/re-backend/14-draw-reduction.md, R4 and R6). Observe only.
// Hooks (scripts/hooks.d/ops_probe.py): OPS_DEPTH_WALK (0xc3a50), OPS_ALPHA_WALK (0xc3c70, entry),
// OPS_ALPHA_DONE (0xc3e48, the site after the walk), OPS_MAINCAM (0xcaed0, RenderView::MainCamera entry)
// and the six family A wrappers of the fog pass (0xa7310, 0xa7470, 0xa75b0, 0xa78a0, 0xa7eb0, 0xa83f0).
#pragma once

#include <string>

namespace SotcProbes {

void RegisterOpsProbe(); // probe_ops.cpp, switch SOTC_OPS_PROBE

/// The report line(s) of the window (driver.cpp 5 s report), appended to out. Resets the window.
void ReportOpsProbe(std::string& out, double seconds);

} // namespace SotcProbes
