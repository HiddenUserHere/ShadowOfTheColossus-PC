// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the switches of the non-blocking frame work (docs/re-stutters/native/arch-next/nonblocking-frame.md). Each
// phase is off by default until its measurement gate passes; each is read once.
#pragma once

#include <cstdlib>
#include <cstring>

namespace SotcNonBlocking {

inline bool SwitchOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

/// P1 (SOTC_NB_IO=1): secondary work leaves the frame threads. The driver cache save at the end of the boot hold goes to
/// the saver thread, and the programs of the pretranslate pack are loaded by a worker when the game announces them.
inline bool Io() {
    static const bool on = SwitchOn("SOTC_NB_IO");
    return on;
}

/// P2 (SOTC_NB_SCHED=1): the pipeline workers' job lanes run with the modern rules: graphics jobs ordered by demand
/// (draws skipped for them) plus age, cancelled when the command processor built the pipeline itself, background
/// jobs on at most half the workers at below-normal thread priority. Off: the same lanes, FIFO, no cap.
inline bool Sched() {
    static const bool on = SwitchOn("SOTC_NB_SCHED");
    return on;
}

} // namespace SotcNonBlocking
