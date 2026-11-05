// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BLOCK_LEDGER (block_ledger.h).

#include "block_ledger.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "video_core/sotc_stats.h"

namespace SotcBlockLedger {
namespace {

// The roles the ledger reports apart (SotcStats::CpuRole); every other role is counted as "other".
enum Slot : uint8_t { SlotCp, SlotDraw, SlotRec, SlotOther, SlotCount };

constexpr std::array<const char*, SiteCount> kSiteNames{
    "pipeline", "compute pipeline", "translate", "pack load", "register binary", "storage save",
    "storage load", "shader index", "driver cache save", "driver cache get", "gpl library", "gpl lock",
    "compute wait",
};
constexpr std::array<const char*, SlotCount> kSlotNames{"cp", "draw", "rec", "other"};

struct Cell {
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> count{0};
};
std::array<std::array<Cell, SiteCount>, SlotCount> g_cells;

Slot SlotOfThread() noexcept {
    switch (SotcStats::tl_role) {
    case SotcStats::RoleCp:
        return SlotCp;
    case SotcStats::RoleDraw:
        return SlotDraw;
    case SotcStats::RoleRecorder:
        return SlotRec;
    default:
        return SlotOther;
    }
}

struct Snapshot {
    std::array<std::array<uint64_t, SiteCount>, SlotCount> ns{};
    std::array<std::array<uint64_t, SiteCount>, SlotCount> count{};
};

Snapshot Take() noexcept {
    Snapshot s;
    for (size_t r = 0; r < SlotCount; ++r) {
        for (size_t i = 0; i < SiteCount; ++i) {
            s.ns[r][i] = g_cells[r][i].ns.load(std::memory_order_relaxed);
            s.count[r][i] = g_cells[r][i].count.load(std::memory_order_relaxed);
        }
    }
    return s;
}

// The difference of two snapshots, as text: per slot its total and the non-zero sites. skip_other drops the
// "other" slot (the per-frame text names the frame threads only).
std::string Format(const Snapshot& now, const Snapshot& last, bool with_counts, bool skip_other, double per) {
    std::string out;
    char buf[160];
    for (size_t r = 0; r < SlotCount; ++r) {
        if (skip_other && r == SlotOther) {
            continue;
        }
        uint64_t total = 0;
        for (size_t i = 0; i < SiteCount; ++i) {
            total += now.ns[r][i] - last.ns[r][i];
        }
        if (total == 0) {
            continue;
        }
        std::snprintf(buf, sizeof buf, "%s%s %.1f ms [", out.empty() ? "" : "; ", kSlotNames[r], total / 1e6 / per);
        out += buf;
        bool first = true;
        for (size_t i = 0; i < SiteCount; ++i) {
            const uint64_t ns = now.ns[r][i] - last.ns[r][i];
            if (ns == 0) {
                continue;
            }
            const uint64_t n = now.count[r][i] - last.count[r][i];
            if (with_counts) {
                std::snprintf(buf, sizeof buf, "%s%s %.1f (%llu)", first ? "" : ", ", kSiteNames[i], ns / 1e6 / per,
                              static_cast<unsigned long long>(n));
            } else {
                std::snprintf(buf, sizeof buf, "%s%s %.1f", first ? "" : ", ", kSiteNames[i], ns / 1e6 / per);
            }
            out += buf;
            first = false;
        }
        out += "]";
    }
    return out;
}

} // namespace

uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void Add(Site site, uint64_t ns) noexcept {
    if (site >= SiteCount) {
        return;
    }
    auto& cell = g_cells[SlotOfThread()][site];
    cell.ns.fetch_add(ns, std::memory_order_relaxed);
    cell.count.fetch_add(1, std::memory_order_relaxed);
}

std::string TakeReport(uint64_t flips) {
    static Snapshot last{};
    const Snapshot now = Take();
    const std::string totals = Format(now, last, true, false, 1.0);
    const std::string per_frame = flips ? Format(now, last, false, true, static_cast<double>(flips)) : std::string{};
    last = now;
    std::string line = "SOTC BLOCK LEDGER (5 s, ms (calls)): ";
    line += totals.empty() ? "nothing" : totals;
    if (!per_frame.empty()) {
        line += " | per frame: " + per_frame;
    }
    return line;
}

std::string TakeFrameText() {
    static Snapshot last{};
    const Snapshot now = Take();
    std::string text = Format(now, last, false, true, 1.0);
    last = now;
    return text.empty() ? text : ", ledger " + text;
}

} // namespace SotcBlockLedger
