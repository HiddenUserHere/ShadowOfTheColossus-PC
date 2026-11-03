// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_SITES=1, draw and dispatch imports per guest call chain (see probe_drawsite.h).
//
// The import is called straight from the engine's wrapper (0x86xxxx family), so the return address of the
// host function is inside that wrapper. The chain above it is found by scanning the guest stack upwards
// for return addresses: qwords inside the eboot's code whose preceding bytes are a call instruction
// (E8 rel32, FF /2 with any of the common encodings). A scan can pick a stale address left on the stack,
// so the counts are a guide for the RE, not exact attribution. The key is the first three engine return
// addresses outside the wrapper range, as image offsets (symbolize with tools/re/eboot_re.py func).

#include "probe_drawsite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "hooks.h"
#include "video_core/sotc_stats.h"

namespace SotcProbes {
namespace {

constexpr uint64_t kImageSpan = 0x10000000;     // the eboot's code is below 256 MiB
constexpr uint64_t kWrapperLo = 0x858000;       // engine GNM wrapper family (docs/re-backend/01)
constexpr uint64_t kWrapperHi = 0x890000;
constexpr int kScanQwords = 512;                // guest stack scanned above the return slot
constexpr int kChain = 3;

struct Key {
    std::array<uint32_t, kChain> ret{};
    uint32_t wrapper = 0;
    bool dispatch = false;
    bool operator==(const Key& o) const {
        return ret == o.ret && wrapper == o.wrapper && dispatch == o.dispatch;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = k.wrapper * 0x9e3779b97f4a7c15ull ^ (k.dispatch ? 1 : 0);
        for (uint32_t r : k.ret) {
            h = (h ^ r) * 0x100000001b3ull;
        }
        return size_t(h);
    }
};

std::mutex g_mu;
std::unordered_map<Key, uint64_t, KeyHash> g_counts;
std::atomic<uint64_t> g_frames_at_start{0};

bool IsCallReturn(uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(v);
    if (p[-5] == 0xE8) {
        return true; // call rel32
    }
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) {
        return true; // call reg / [reg]
    }
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) {
        return true; // call [reg+disp8]
    }
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) {
        return true; // call [rip+disp32] / [reg+disp32]
    }
    if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10 && (p[-3] & 0x07) == 0x04) {
        return true; // call [base+index+disp8] (SIB)
    }
    return false;
}

// Number of qwords readable from p upward (at most max): the committed, readable region that holds p, cached
// per thread (the guest stack of a thread does not move).
int ReadableQwords(const uint64_t* p, int max) {
#ifdef _WIN32
    thread_local uintptr_t lo = 0, hi = 0;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < lo || a >= hi) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) == 0 ||
            (mbi.Protect & PAGE_GUARD) != 0) {
            return 0;
        }
        lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        hi = lo + mbi.RegionSize;
    }
    return int(std::min<uintptr_t>(uintptr_t(max), (hi - a) / 8));
#else
    return max;
#endif
}

} // namespace

bool DrawSitesOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DRAW_SITES");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

void NoteDrawSite(const void* ret_slot, bool dispatch) {
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (base == 0) {
        return;
    }
    const auto* slot = static_cast<const uint64_t*>(ret_slot);
    Key key;
    key.dispatch = dispatch;
    if (ReadableQwords(slot, 1) == 0) {
        return;
    }
    const uint64_t first = slot[0];
    if (first >= base && first - base < kImageSpan) {
        key.wrapper = uint32_t(first - base);
    }
    int n = 0;
    const int scan = ReadableQwords(slot, kScanQwords);
    for (int i = 1; i < scan && n < kChain; ++i) {
        const uint64_t v = slot[i];
        if (v < base + 16 || v - base >= kImageSpan) {
            continue;
        }
        const uint64_t off = v - base;
        if ((off >= kWrapperLo && off < kWrapperHi) || !IsCallReturn(v)) {
            continue;
        }
        key.ret[n++] = uint32_t(off);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_counts[key];
}

void ReportDrawSites(std::string& out, double) {
    if (!DrawSitesOn()) {
        return;
    }
    const uint64_t flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    const uint64_t frames = flips - g_frames_at_start.exchange(flips);
    std::vector<std::pair<uint64_t, Key>> rows;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& [k, n] : g_counts) {
            rows.emplace_back(n, k);
        }
        g_counts.clear();
    }
    if (frames == 0 || rows.empty()) {
        return;
    }
    const double f = double(frames);
    uint64_t draws = 0, dispatches = 0;
    for (const auto& [n, k] : rows) {
        (k.dispatch ? dispatches : draws) += n;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char line[256];
    std::snprintf(line, sizeof line, "SOTC DRAW SITES: %.0f draws/frame, %.0f dispatches/frame, %zu chains, %llu frames",
                  double(draws) / f, double(dispatches) / f, rows.size(), (unsigned long long)frames);
    out += line;
    out += '\n';
    for (size_t i = 0; i < rows.size() && i < 60; ++i) {
        const auto& [n, k] = rows[i];
        std::snprintf(line, sizeof line, "  %s %7.1f/frame wrapper %#x chain %#x %#x %#x",
                      k.dispatch ? "dispatch" : "draw    ", double(n) / f, k.wrapper, k.ret[0], k.ret[1], k.ret[2]);
        out += line;
        out += '\n';
    }
}

} // namespace SotcProbes
