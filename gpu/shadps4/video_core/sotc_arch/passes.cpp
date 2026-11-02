// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PASSES (ARCH-DRAW.md section 5, phase A6). Conservative pass rule: a global memory barrier is due
// before an op that may touch buffers when some write (any runtime write since the last barrier) is pending.
// Image barriers stay where they are (Runtime::Transit decides them per image state).
//
// sotc: SOTC_BARRIER_STATIC (same phase). The static rule replaces the per-binding overlap checks of the buffer side:
// two pending flags (a write, a read) since the last barrier, and one global barrier per decision that needs one.
// See passes.h for the contract and the modes.
//
// Dependency: every buffer write the runtime records goes through Runtime::AccessBuffer. That is where
// NoteWrite() must be called (vk_runtime.cpp, see the report). Without that call the rule sees no writes, so =1
// must not be used and =2 reports them as omitted-needed.
#include <atomic>
#include <cstdio>

#include "video_core/sotc_arch/guest_barrier.h"
#include "video_core/sotc_arch/passes.h"

namespace SotcArch::Passes {

namespace {

int Mode() {
    static const int mode = SwitchMode("SOTC_PASSES");
    return mode;
}

// sotc: SOTC_BARRIER_STATIC mode, read once. Named apart from Mode(): inside BarrierStatic the name Mode is its own.
int StaticMode() {
    static const int mode = SwitchMode("SOTC_BARRIER_STATIC");
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

// sotc: SOTC_BARRIER_STATIC state. A write or a read is pending since the last barrier recorded by the rule.
std::atomic<bool> s_pending_write{false};
std::atomic<bool> s_pending_read{false};
std::atomic<u64> s_ops{};        // decisions (ops) of the static rule
std::atomic<u64> s_old{};        // =2: barriers the old decision emits
std::atomic<u64> s_new{};        // =2: barriers the static rule would emit
std::atomic<u64> s_missed{};     // =2: old barrier, no static barrier (must stay 0)
std::atomic<u64> s_extra{};      // =2: static barrier, no old barrier
std::atomic<u64> s_recorded{};   // =1: barriers FlushBarriers recorded

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
    if (StaticMode() != 0) {
        s_pending_write.store(true, std::memory_order_relaxed);
    }
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

namespace BarrierStatic {

int Mode() {
    return StaticMode();
}

bool On() {
    return StaticMode() == 1;
}

bool Verifying() {
    return StaticMode() == 2;
}

bool BufferChecks() {
    // sotc: SOTC_BARRIER_GUEST=1 decides the buffer side with its guest sync points and safety net (guest_barrier.h).
    return !SotcArch::Passes::On() && !On() && !SotcArch::GuestBarrier::On();
}

bool Decide(bool image_need, bool buffer_need, bool touches, bool writes) {
    const int mode = StaticMode();
    if (mode == 0) {
        return SotcArch::Passes::Decide(image_need || buffer_need, touches);
    }
    const bool pw = s_pending_write.load(std::memory_order_relaxed);
    const bool pr = s_pending_read.load(std::memory_order_relaxed);
    // The static rule: one global barrier when this op reads after a pending write (RAW) or writes after a pending
    // read (WAR). The image side is kept as it is.
    const bool static_due = image_need || (touches && pw) || (writes && pr);
    s_ops.fetch_add(1, std::memory_order_relaxed);
    if (mode == 1) {
        // A buffer_need is zero in this mode (BufferChecks); it is still honoured, never dropped.
        return static_due || buffer_need;
    }
    // Verify: today's decision (image side and per-binding checks) is the one emitted. The static rule is counted
    // next to it; its flags are cleared when it decides a flush, as in mode 1.
    const bool old_due = SotcArch::Passes::Decide(image_need || buffer_need, touches);
    s_old.fetch_add(old_due ? 1 : 0, std::memory_order_relaxed);
    s_new.fetch_add(static_due ? 1 : 0, std::memory_order_relaxed);
    if (old_due && !static_due) {
        s_missed.fetch_add(1, std::memory_order_relaxed);
    }
    if (static_due && !old_due) {
        s_extra.fetch_add(1, std::memory_order_relaxed);
    }
    if (static_due) {
        s_pending_write.store(false, std::memory_order_relaxed);
        s_pending_read.store(false, std::memory_order_relaxed);
    }
    return old_due;
}

void NoteOp(bool touches, bool writes) {
    if (StaticMode() == 0) {
        return;
    }
    if (touches) {
        s_pending_read.store(true, std::memory_order_relaxed);
    }
    if (writes) {
        s_pending_write.store(true, std::memory_order_relaxed);
    }
}

bool DmaNeeds(bool check_reads) {
    if (s_pending_write.load(std::memory_order_relaxed)) {
        return true;
    }
    return check_reads && s_pending_read.load(std::memory_order_relaxed);
}

bool Pending() {
    return s_pending_write.load(std::memory_order_relaxed) ||
           s_pending_read.load(std::memory_order_relaxed);
}

void Flushed() {
    if (StaticMode() != 1) {
        return;
    }
    s_pending_write.store(false, std::memory_order_relaxed);
    s_pending_read.store(false, std::memory_order_relaxed);
    s_recorded.fetch_add(1, std::memory_order_relaxed);
}

void BarrierReport(char* out, u32 size, double seconds) {
    out[0] = '\0';
    const int mode = StaticMode();
    if (mode == 0) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 ops = take(s_ops);
    const u64 old_n = take(s_old);
    const u64 new_n = take(s_new);
    const u64 missed = take(s_missed);
    const u64 extra = take(s_extra);
    const u64 recorded = take(s_recorded);
    (void)seconds; // the counters cover the last report interval (the driver's 5 s)
    if (mode == 2) {
        std::snprintf(out, size,
                      "SOTC BARRIER_STATIC: ops %llu, barriers new %llu old %llu, missed %llu, extra %llu",
                      static_cast<unsigned long long>(ops), static_cast<unsigned long long>(new_n),
                      static_cast<unsigned long long>(old_n), static_cast<unsigned long long>(missed),
                      static_cast<unsigned long long>(extra));
    } else {
        std::snprintf(out, size, "SOTC BARRIER_STATIC: mode 1, ops %llu, barriers recorded %llu",
                      static_cast<unsigned long long>(ops), static_cast<unsigned long long>(recorded));
    }
}

} // namespace BarrierStatic

} // namespace SotcArch::Passes
