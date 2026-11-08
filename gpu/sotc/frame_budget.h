// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: 30 FPS flip cadence diagnostics, independent of the legacy raw >33 ms counter.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SotcLab {
struct FrameBudget30 {
    static constexpr double period_ms = 1000.0 / 30.0;
    static constexpr double tolerance_ms = 0.5; // timestamp/pacing tolerance; not a new target FPS
    uint64_t late{}, consecutive{}, max_consecutive{};

    void Add(double ms) {
        if (!std::isfinite(ms) || ms < 0.0) {
            return; // invalid samples must not reset a streak or count as a frame
        }
        if (ms > period_ms + tolerance_ms) {
            ++late;
            max_consecutive = std::max(max_consecutive, ++consecutive);
        } else {
            consecutive = 0;
        }
    }
};
} // namespace SotcLab
