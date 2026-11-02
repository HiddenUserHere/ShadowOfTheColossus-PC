// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OPCOST (sotc_arch.h, section A0): what one op costs on the draw thread and on the command processor.
// Counts and sums are exact; p50/p99 come from a log2 histogram of the op times (each estimate is the upper bound
// of its bucket, so within 2x). Every Note is a few relaxed atomic adds; nothing is taken when the switch is off.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstdio>

#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/sotc_stats.h"

namespace SotcArch::OpCost {

namespace {

// Bucket b holds op times in [2^(b-1), 2^b) ns; 40 buckets reach 2^39 ns (about 9 minutes).
constexpr u32 kBuckets = 40;
constexpr u32 kThreads = 2;
constexpr u32 kOps = static_cast<u32>(Op::Count);

struct Cell {
    std::atomic<u64> count{0};
    std::atomic<u64> sum_ns{0};
    std::atomic<u64> hist[kBuckets]{};
};

Cell g_cells[kThreads][kOps];
// Command processor frames at the previous report (SotcStats::cp_frame).
u64 g_last_frames = 0;
bool g_primed = false;

const char* const kThreadNames[kThreads] = {"draw", "cp"};
const char* const kOpNames[kOps] = {"draw", "draw indirect", "dispatch", "dispatch indirect", "other"};

u32 Bucket(u64 ns) {
    const u32 b = ns == 0 ? 0 : 64 - static_cast<u32>(std::countl_zero(ns));
    return b < kBuckets ? b : kBuckets - 1;
}

// Upper bound, in us, of the first bucket where the cumulative count reaches q of total.
double PercentileUs(const u64* hist, u64 total, double q) {
    const u64 target = static_cast<u64>(static_cast<double>(total) * q + 0.999999);
    u64 cum = 0;
    for (u32 b = 0; b < kBuckets; ++b) {
        cum += hist[b];
        if (cum >= target && hist[b] != 0) {
            return static_cast<double>(u64{1} << b) / 1e3;
        }
    }
    return 0.0;
}

// Appends formatted text at out[pos]; pos stops at size - 1 when the line is full.
void Append(char* out, u32 size, u32& pos, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void Append(char* out, u32 size, u32& pos, const char* fmt, ...) {
    if (pos + 1 >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(out + pos, size - pos, fmt, args);
    va_end(args);
    if (n > 0) {
        pos = std::min<u32>(pos + static_cast<u32>(n), size - 1);
    }
}

} // namespace

bool On() {
    // SwitchMode: 1 and 2 both measure (section A0 has no verify mode).
    static const bool on = SwitchMode("SOTC_OPCOST") != 0;
    return on;
}

void Note(Thread thread, Op op, u64 ns) {
    Cell& cell = g_cells[static_cast<u32>(thread)][static_cast<u32>(op)];
    cell.count.fetch_add(1, std::memory_order_relaxed);
    cell.sum_ns.fetch_add(ns, std::memory_order_relaxed);
    cell.hist[Bucket(ns)].fetch_add(1, std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (seconds <= 0.0) {
        seconds = 1.0;
    }

    // Frames of this window, from the last report (the first report only sets the baseline).
    const u64 frames_now = SotcStats::cp_frame.load(std::memory_order_relaxed);
    const u64 frames = g_primed ? frames_now - g_last_frames : 0;
    g_last_frames = frames_now;
    g_primed = true;

    // Take and reset every cell, so each thread's numbers come from one window.
    u64 counts[kThreads][kOps]{};
    u64 sums[kThreads][kOps]{};
    u64 hists[kThreads][kOps][kBuckets]{};
    u64 total_ops = 0;
    for (u32 t = 0; t < kThreads; ++t) {
        for (u32 o = 0; o < kOps; ++o) {
            Cell& cell = g_cells[t][o];
            counts[t][o] = cell.count.exchange(0, std::memory_order_relaxed);
            sums[t][o] = cell.sum_ns.exchange(0, std::memory_order_relaxed);
            for (u32 b = 0; b < kBuckets; ++b) {
                hists[t][o][b] = cell.hist[b].exchange(0, std::memory_order_relaxed);
            }
            total_ops += counts[t][o];
        }
    }
    if (total_ops == 0) {
        return;
    }

    const double per_frame = frames ? static_cast<double>(frames) : 0.0;
    const char* const unit = frames ? "/frame" : "/s";
    u32 pos = 0;
    Append(out, size, pos, "SOTC OPCOST");
    bool any_thread = false;
    for (u32 t = 0; t < kThreads; ++t) {
        u64 n = 0;
        u64 ns = 0;
        u64 merged[kBuckets]{};
        for (u32 o = 0; o < kOps; ++o) {
            n += counts[t][o];
            ns += sums[t][o];
            for (u32 b = 0; b < kBuckets; ++b) {
                merged[b] += hists[t][o][b];
            }
        }
        if (n == 0) {
            continue;
        }
        const double us_per_op = static_cast<double>(ns) / 1e3 / static_cast<double>(n);
        const double p50 = PercentileUs(merged, n, 0.50);
        const double p99 = PercentileUs(merged, n, 0.99);
        const char* const sep = any_thread ? ";" : "";
        any_thread = true;
        if (frames) {
            Append(out, size, pos,
                   "%s %s: %.0f ops/frame (%llu frames), %.2f us/op (p50 <=%.2f, p99 <=%.2f us) by kind:",
                   sep, kThreadNames[t], static_cast<double>(n) / per_frame,
                   static_cast<unsigned long long>(frames), us_per_op, p50, p99);
        } else {
            Append(out, size, pos, "%s %s: %.0f ops/s, %.2f us/op (p50 <=%.2f, p99 <=%.2f us) by kind:",
                   sep, kThreadNames[t], static_cast<double>(n) / seconds, us_per_op, p50, p99);
        }
        bool first_kind = true;
        for (u32 o = 0; o < kOps; ++o) {
            if (counts[t][o] == 0) {
                continue;
            }
            const double kind_n = frames ? static_cast<double>(counts[t][o]) / per_frame
                                         : static_cast<double>(counts[t][o]) / seconds;
            Append(out, size, pos, "%s %s %.0f%s x %.2f us", first_kind ? "" : ",", kOpNames[o], kind_n,
                   unit, static_cast<double>(sums[t][o]) / 1e3 / static_cast<double>(counts[t][o]));
            first_kind = false;
        }
    }
}

} // namespace SotcArch::OpCost
