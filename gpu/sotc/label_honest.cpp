// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_LABELS_HONEST. Shared API and counters, see label_honest.h.

#include "label_honest.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace SotcLabels {
namespace {

std::atomic<uint64_t> g_shared_seq{0};
std::atomic<bool> g_live{false};

constexpr int kKinds = 3;
struct KindCounters {
    std::atomic<uint64_t> seen{0}, deferred{0}, honest_only{0}, excluded{0}, forced{0};
};
KindCounters g_counters[kKinds];

KindCounters& Counters(Kind kind) {
    return g_counters[static_cast<int>(kind) < kKinds ? static_cast<int>(kind) : 0];
}

} // namespace

bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_LABELS_HONEST");
        return v && v[0] == '1';
    }();
    return on;
}

bool Honest() {
    return On() && g_live.load(std::memory_order_acquire);
}

void MarkLive() {
    g_live.store(true, std::memory_order_release);
}

void NoteSharedTouch() {
    g_shared_seq.fetch_add(1, std::memory_order_release);
}

uint64_t SharedSeq() {
    return g_shared_seq.load(std::memory_order_acquire);
}

void NoteLabel(Kind kind, bool deferred, bool honest_only) {
    if (!On()) {
        return;
    }
    KindCounters& c = Counters(kind);
    c.seen.fetch_add(1, std::memory_order_relaxed);
    if (deferred) {
        c.deferred.fetch_add(1, std::memory_order_relaxed);
    }
    if (honest_only) {
        c.honest_only.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteExcluded(Kind kind) {
    if (On()) {
        Counters(kind).excluded.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteForced(Kind kind) {
    if (On()) {
        Counters(kind).forced.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReportLabels(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    uint64_t seen[kKinds], deferred[kKinds], honest[kKinds], excluded[kKinds], forced[kKinds];
    uint64_t all_seen = 0, all_deferred = 0, all_honest = 0;
    for (int k = 0; k < kKinds; ++k) {
        KindCounters& c = g_counters[k];
        seen[k] = c.seen.exchange(0, std::memory_order_relaxed);
        deferred[k] = c.deferred.exchange(0, std::memory_order_relaxed);
        honest[k] = c.honest_only.exchange(0, std::memory_order_relaxed);
        excluded[k] = c.excluded.exchange(0, std::memory_order_relaxed);
        forced[k] = c.forced.exchange(0, std::memory_order_relaxed);
        all_seen += seen[k];
        all_deferred += deferred[k];
        all_honest += honest[k];
    }
    // Cost gate: honest-only deferrals must stay under 2% of the labels seen.
    const double pct_honest = all_seen ? 100.0 * double(all_honest) / double(all_seen) : 0.0;
    const double pct_all = all_seen ? 100.0 * double(all_deferred) / double(all_seen) : 0.0;
    char line[512];
    std::snprintf(
        line, sizeof line,
        "SOTC labels honest: seen %llu, deferred %llu (%.2f%%), honest-only %llu (%.2f%%, gate <2%%, %.0f/s); "
        "EOP %llu/%llu/%llu/%llu/%llu, RM %llu/%llu/%llu/%llu/%llu, EOS %llu/%llu/%llu/%llu/%llu "
        "(seen/deferred/honest-only/excluded/forced)\n",
        static_cast<unsigned long long>(all_seen), static_cast<unsigned long long>(all_deferred),
        pct_all, static_cast<unsigned long long>(all_honest), pct_honest,
        seconds > 0.0 ? double(all_honest) / seconds : 0.0,
        static_cast<unsigned long long>(seen[0]), static_cast<unsigned long long>(deferred[0]),
        static_cast<unsigned long long>(honest[0]), static_cast<unsigned long long>(excluded[0]),
        static_cast<unsigned long long>(forced[0]), static_cast<unsigned long long>(seen[1]),
        static_cast<unsigned long long>(deferred[1]), static_cast<unsigned long long>(honest[1]),
        static_cast<unsigned long long>(excluded[1]), static_cast<unsigned long long>(forced[1]),
        static_cast<unsigned long long>(seen[2]), static_cast<unsigned long long>(deferred[2]),
        static_cast<unsigned long long>(honest[2]), static_cast<unsigned long long>(excluded[2]),
        static_cast<unsigned long long>(forced[2]));
    out += line;
}

} // namespace SotcLabels
