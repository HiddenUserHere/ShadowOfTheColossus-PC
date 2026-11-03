// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DET_FLIP=1, flips and vblanks paced by the work instead of the display (offline
// deterministic test mode, see the SOTC_DETERMINISTIC runner in sotc.py).
//  - unset or 0: nothing changes; the flips follow the vblank thread's wall clock (driver.cpp).
//  - 1: the present thread takes each guest flip as soon as it is submitted, with no 30 FPS cap
//    and no vsync wait (Immediate present, Mailbox if missing). Each flip waits for its frame's GPU
//    work (Presenter::Present), then the clock is told "frame N flipped" (det_clock_on_flip, weak:
//    the virtual clock piece may be absent), then the flip events fire and the game's vblank
//    events are generated: game flip rate + 1 vblanks per flip (2 at 60 Hz for the 30 FPS cap),
//    at once, not on a timer.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

// src/runtime_detclock.c (the virtual clock). Optional: absent means no clock is told.
extern "C" void det_clock_on_flip(uint64_t frame) __attribute__((weak));

namespace SotcDetFlip {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_DET_FLIP");
    return v && v[0] == '1';
}();

/// SOTC_DET_REALTIME=1: the run never goes faster than real time. Flip N is held until N frames of
/// SOTC_DET_CLOCK_HZ (default 30) have passed on the wall clock since the first flip, so light scenes
/// (menus, loads, a still camera) play at their normal speed instead of fast-forward. A run slower than real
/// time is not held. The guest still sees the frame-locked virtual time either way.
inline const bool kRealtime = [] {
    const char* v = std::getenv("SOTC_DET_REALTIME");
    return v && v[0] == '1';
}();

/// Tells the virtual clock that guest flip `frame` (1-based) has completed, after the real-time hold.
inline void NoteFlipped(uint64_t frame) {
    if (kRealtime) {
        using Clock = std::chrono::steady_clock;
        static const double hz = [] {
            const char* v = std::getenv("SOTC_DET_CLOCK_HZ");
            const double d = v ? std::atof(v) : 0.0;
            return d >= 1.0 && d <= 1000.0 ? d : 30.0;
        }();
        // Each flip at least one frame period after the previous one; a late frame is not made up by a
        // burst of fast ones (present thread only).
        static Clock::time_point last{};
        const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
        const auto now = Clock::now();
        const auto due = last == Clock::time_point{} ? now : std::max(last + period, now);
        std::this_thread::sleep_until(due);
        last = due;
    }
    if (det_clock_on_flip != nullptr) {
        det_clock_on_flip(frame);
    }
}

} // namespace SotcDetFlip
