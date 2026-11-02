// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PASSES (ARCH-DRAW.md section 5, phase A6). Conservative pass rule: a global memory barrier is due
// before an op that may touch buffers when some write (any runtime write since the last barrier) is pending.
// Image barriers stay where they are (Runtime::Transit decides them per image state).
//
// Dependency: every buffer write the runtime records goes through Runtime::AccessBuffer. That is where
// NoteWrite() must be called (vk_runtime.cpp, see the report). Without that call the rule sees no writes, so =1
// must not be used and =2 reports them as omitted-needed.
#include <atomic>
#include <cstdio>

#include "video_core/sotc_arch/passes.h"

namespace SotcArch::Passes {

namespace {

int Mode() {
    static const int mode = SwitchMode("SOTC_PASSES");
    return mode;
}

std::atomic<bool> dirty{false};       // a write was recorded since the last flush of this rule
std::atomic<bool> buffer_need{false}; // today's per-binding check said overlap, for the current decision

std::atomic<u64> c_writes{};
std::atomic<u64> c_decisions{};
std::atomic<u64> c_flushes{};
std::atomic<u64> c_flush_buffer{}; // =1: due to the pass rule; =2: today's flush caused by a buffer overlap
std::atomic<u64> c_flush_image{};  // today's flush caused by an image transition only
std::atomic<u64> c_pass_only{};    // =2: the pass rule would flush, today does not (ops)
std::atomic<u64> c_omitted{};      // =2: today's buffer overlap with no pass due (must stay 0)

} // namespace

bool On() {
    return Mode() == 1;
}

bool Verifying() {
    return Mode() == 2;
}

bool ParallelRecord() {
    static const bool on = SwitchMode("SOTC_PASS_RECORD") == 1;
    return on;
}

void NoteWrite() {
    if (Mode() == 0) {
        return;
    }
    dirty.store(true, std::memory_order_relaxed);
    c_writes.fetch_add(1, std::memory_order_relaxed);
}

bool BufferNeed(bool need) {
    if (need && Mode() == 2) {
        buffer_need.store(true, std::memory_order_relaxed);
    }
    return need;
}

bool Decide(bool image_need, bool touches) {
    const int mode = Mode();
    if (mode == 0) {
        return image_need;
    }
    const bool pass_due = touches && dirty.load(std::memory_order_relaxed);
    const bool buffer_now = buffer_need.exchange(false, std::memory_order_relaxed);
    c_decisions.fetch_add(1, std::memory_order_relaxed);

    bool flush;
    if (mode == 1) {
        flush = image_need || pass_due;
        if (flush) {
            (image_need ? c_flush_image : c_flush_buffer).fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // Verify: today's decision is used. The pass rule is checked against it.
        flush = image_need;
        if (flush) {
            if (buffer_now) {
                c_flush_buffer.fetch_add(1, std::memory_order_relaxed);
                if (!pass_due) {
                    c_omitted.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                c_flush_image.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (pass_due) {
            c_pass_only.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (flush) {
        c_flushes.fetch_add(1, std::memory_order_relaxed);
        dirty.store(false, std::memory_order_relaxed);
    }
    return flush;
}

void Report(char* out, u32 size, double seconds) {
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 writes = take(c_writes);
    const u64 decisions = take(c_decisions);
    const u64 flushes = take(c_flushes);
    const u64 buffer = take(c_flush_buffer);
    const u64 image = take(c_flush_image);
    const u64 pass_only = take(c_pass_only);
    const u64 omitted = take(c_omitted);
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    std::snprintf(out, size,
                  "SOTC PASSES mode %d: decisions %llu (%.0f/s), flushes %llu (buffer %llu, image %llu), "
                  "pass due without flush %llu, omitted needed %llu (must be 0), writes noted %llu\n",
                  Mode(), static_cast<unsigned long long>(decisions), decisions / seconds,
                  static_cast<unsigned long long>(flushes), static_cast<unsigned long long>(buffer),
                  static_cast<unsigned long long>(image), static_cast<unsigned long long>(pass_only),
                  static_cast<unsigned long long>(omitted), static_cast<unsigned long long>(writes));
}

} // namespace SotcArch::Passes
