// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DET_ROUTE=1, the deterministic Lab route. With the switch set, every wait of the Lab
// route (lab_commands.cpp), of the engine-command give-up (engine_cmd.cpp) and the autopilot and
// free-camera step (freecam.cpp) is counted in guest frames, not wall-clock time, and the game's pad
// reads neutral input (runtime_pad.c). One second of route time is SOTC_DET_ROUTE_FPS frames
// (default 30, the game's rate). With the switch off nothing here is used.
//   SOTC_DET_EXIT_FRAMES=<n>  the run ends n frames after the route starts (0 or unset: route end plus
//                             SOTC_LAB_ROUTE_EXIT seconds, in frames)
// Header only, so the switch is read in the same way in every file that includes it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace SotcDetRoute {

/// SOTC_DET_ROUTE is set (not empty, not "0"). Read once.
inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DET_ROUTE");
        return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return on;
}

/// Frames per second of the route clock: SOTC_DET_ROUTE_FPS, default 30.
inline double Fps() {
    static const double fps = [] {
        const char* v = std::getenv("SOTC_DET_ROUTE_FPS");
        if (!v || !*v) {
            return 30.0;
        }
        char* end = nullptr;
        const double d = std::strtod(v, &end);
        return end != v && std::isfinite(d) && d > 0.0 ? d : 30.0;
    }();
    return fps;
}

/// Seconds to frames, rounded to the nearest frame.
inline int64_t Frames(double seconds) {
    return static_cast<int64_t>(std::llround(seconds * Fps()));
}

/// SOTC_DET_EXIT_FRAMES: frames from the route start to the end of the run. 0: not set.
inline int64_t ExitFrames() {
    static const int64_t n = [] {
        const char* v = std::getenv("SOTC_DET_EXIT_FRAMES");
        if (!v || !*v) {
            return int64_t{0};
        }
        char* end = nullptr;
        const long long k = std::strtoll(v, &end, 10);
        return end != v && k > 0 ? static_cast<int64_t>(k) : int64_t{0};
    }();
    return n;
}

} // namespace SotcDetRoute
