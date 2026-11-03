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

#include <cstdint>
#include <cstdlib>

// src/runtime_detclock.c (the virtual clock). Optional: absent means no clock is told.
extern "C" void det_clock_on_flip(uint64_t frame) __attribute__((weak));

namespace SotcDetFlip {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_DET_FLIP");
    return v && v[0] == '1';
}();

/// Tells the virtual clock that guest flip `frame` (1-based) has completed.
inline void NoteFlipped(uint64_t frame) {
    if (det_clock_on_flip != nullptr) {
        det_clock_on_flip(frame);
    }
}

} // namespace SotcDetFlip
