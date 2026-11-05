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

/// P3 (SOTC_NB_GPL=1, with SOTC_PIPE_GPL=1 and SOTC_STAGE_BINDINGS=1): never-wait pipelines. Graphics pipeline layouts
/// are created with independent sets; pre-rasterization and fragment libraries are built per shader (a layout holding
/// only that stage's set; the fragment part without attachment formats). The command processor only links parts that
/// exist (no library build on it); a missing part goes to a worker link job and the draw is skipped meanwhile.
inline bool Gpl() {
    static const bool on = SwitchOn("SOTC_NB_GPL");
    return on;
}

/// P4 (SOTC_NB_WARM=1, with SOTC_NB_GPL): libraries of new vertex and pixel shaders built ahead of their first draw on
/// background workers. Measured slower on the first session (the warm-up takes workers and CPU the frame needs); off.
inline bool Warm() {
    static const bool on = SwitchOn("SOTC_NB_WARM");
    return on;
}

/// SOTC_NB_WORKER_CAP=1 (independent of the phases above): the pipeline workers spend less CPU during play. Jobs no draw
/// or recorder waits for on this frame (the Shadow, Graphics and Background lanes) run on at most a quarter of the
/// hardware threads (at least 2) at below-normal priority; Compute, Translate and Tiny jobs, which the recorder or a
/// draw waits for, are not capped. New pipelines and objects may appear a few frames later. Off: no cap, as before.
inline bool WorkerCap() {
    static const bool on = SwitchOn("SOTC_NB_WORKER_CAP");
    return on;
}

} // namespace SotcNonBlocking
