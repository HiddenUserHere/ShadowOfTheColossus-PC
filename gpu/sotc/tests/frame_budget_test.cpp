// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: synthetic flip sequences; no game or graphics device.
#include "frame_budget.h"

#include <cassert>
#include <limits>

int main() {
    using SotcLab::FrameBudget30;
    FrameBudget30 budget;
    for (int i = 0; i < 300; ++i) {
        budget.Add(1000.0 / 30.0);
    }
    assert(budget.late == 0); // the old >33 ms classification counted all 300
    budget.Add(FrameBudget30::period_ms + FrameBudget30::tolerance_ms);
    assert(budget.late == 0);
    budget.Add(34.0);
    budget.Add(66.6667);
    budget.Add(std::numeric_limits<double>::quiet_NaN());
    budget.Add(std::numeric_limits<double>::infinity());
    budget.Add(-1.0);
    assert(budget.late == 2 && budget.consecutive == 2 && budget.max_consecutive == 2);
    budget.Add(33.3333);
    assert(budget.consecutive == 0);
    budget.Add(50.0);
    assert(budget.late == 3 && budget.max_consecutive == 2);
    budget = {};
    assert(budget.late == 0 && budget.consecutive == 0 && budget.max_consecutive == 0);
}
