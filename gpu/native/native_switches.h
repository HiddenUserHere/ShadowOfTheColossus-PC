// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Switches of the native command processor / no-spin plan
// (docs/engine/native-render-gaps.md N2-94+). N2-109: the validated switches are ON by
// default ("0" turns one off), including SOTC_NATIVE_VLABELS (redesigned in N2-112); SOTC_SUBMIT_IDLE=cp stays off (N2-106: the command processor spun
// on the full draw ring). Each reads its environment variable once.
#pragma once

#include <cstdlib>

namespace Native::Switch {

inline bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1';
}
/// Default-on switch: unset or anything but "0" is on.
inline bool EnvNotOff(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr || v[0] != '0';
}

/// W1: the GpuRecorder consumer blocks on its condition variable without a timeout.
inline bool RecBlock() {
    static const bool on = EnvNotOff("SOTC_REC_BLOCK");
    return on;
}
/// W1: the draw thread (GpuDraw, BackLoop) blocks on back_cv without a timeout.
inline bool BackBlock() {
    static const bool on = EnvNotOff("SOTC_BACK_BLOCK");
    return on;
}
/// W1: the command processor sleeps on the progress epoch (Liverpool::NotifyProgress) instead of
/// the 100 us poll backoff when every queue is parked on a wait.
inline bool CpEvents() {
    static const bool on = EnvNotOff("SOTC_CP_EVENTS");
    return on;
}
/// W2: native waits are resolved against the virtual label table (native_labels.h) first.
/// N2-115: ON by default again ("0" turns it off). Since N2-112 a pending label write never passes
/// a wait (that made objects jump, N2-110); it only parks the wait without re-polling until the
/// producer's real write.
inline bool VirtualLabels() {
    static const bool on = EnvNotOff("SOTC_NATIVE_VLABELS");
    return on;
}
/// W3: all-native submissions run on the op-list executor (liverpool_native_cp.cpp) instead of
/// the PM4 parser coroutines.
inline bool NativeCp() {
    static const bool on = EnvNotOff("SOTC_NATIVE_CP");
    return on;
}
/// W4: consecutive draw-thread ops are batched into one back-ring entry.
inline bool BackBatch() {
    static const bool on = EnvNotOff("SOTC_BACK_BATCH");
    return on;
}
/// W5: GpuDraw records each pass segment directly into a real command buffer and a submit
/// thread receives whole command buffers (vk_scheduler_segments.cpp).
inline bool PassRecorder() {
    static const bool on = EnvNotOff("SOTC_PASS_RECORDER");
    return on;
}
/// W4 experiment: SubmitDone waits only for the command processor to drain (SOTC_SUBMIT_IDLE=cp).
inline bool SubmitIdleCp() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_SUBMIT_IDLE");
        return v != nullptr && v[0] == 'c' && v[1] == 'p';
    }();
    return on;
}

} // namespace Native::Switch
