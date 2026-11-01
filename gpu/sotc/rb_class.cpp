// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: explicit readbacks v1, the engine-fed registry of readback ranges. See rb_class.h.

#include "rb_class.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace SotcRbClass {
namespace {

constexpr size_t kMaxRanges = 64;
constexpr uint64_t kMaxRequestBytes = 64 * 1024; // a count array is u32 per system; cap the copy
constexpr size_t kMaxRequests = 256;             // pending requests (the draw thread takes them often)

struct Entry {
    uint64_t addr;
    uint64_t size;
    bool arena; // a TaggedArena block (freed with the block) or a stats array (kept)
};

std::mutex g_mutex;
std::vector<Entry> g_ranges;
std::atomic<uint64_t> g_version{1};
std::atomic<uint64_t> g_overflow{0};
std::mutex g_request_mutex;
std::vector<Range> g_requests;
std::atomic<uint64_t> g_asked{0};          // RequestCopy calls (any size)
std::atomic<uint64_t> g_queued{0};         // requests added to the pending list
std::atomic<uint64_t> g_deduped{0};        // requests merged into a pending one
std::atomic<uint64_t> g_empty{0};          // zero-size requests
std::atomic<uint64_t> g_request_dropped{0};
std::atomic<uint64_t> g_taken{0};          // requests handed to the buffer cache
std::atomic<uint64_t> g_held{0};
std::atomic<uint64_t> g_request_issued{0};
std::atomic<uint64_t> g_check_none{0};     // stats reads with no request for their slot
std::atomic<uint64_t> g_check_match{0};    // stats reads whose array is the requested one
std::atomic<uint64_t> g_check_mismatch{0}; // stats reads whose array moved since the request
std::atomic<uint64_t> g_check_beyond{0};   // matched reads past the requested extent

// sotc: SOTC_RB_CLASS=2 mask spans (RecordMaskSpan). kMaskSpanFrames: a span is kept while its
// frame is at most this many command processor frames old.
constexpr size_t kMaxMaskSpans = 1024;
constexpr uint64_t kMaskSpanFrames = 4;
struct MaskSpan {
    uint64_t addr;
    uint64_t size;
    uint64_t frame; // cp frame of its latest record (the consumer read)
};
std::mutex g_mask_mutex;
std::vector<MaskSpan> g_mask_spans;
std::atomic<uint64_t> g_mask_recorded{0}; // RecordMaskSpan calls accepted (new or refreshed)
std::atomic<uint64_t> g_mask_full{0};     // RecordMaskSpan calls dropped: table full

void AddLocked(uint64_t addr, uint64_t size, bool arena) {
    for (const Entry& e : g_ranges) {
        if (e.addr == addr && e.size == size) {
            return;
        }
    }
    if (g_ranges.size() == kMaxRanges) {
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ranges.push_back({addr, size, arena});
    g_version.fetch_add(1, std::memory_order_release);
    std::fprintf(stderr, "SOTC RB CLASS: %s %llx +%llx\n", arena ? "mask arena" : "stats array",
                 static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
}

} // namespace

bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_RB_CLASS");
        // sotc: =2 also serves the last applied mask copy (buffer_cache.cpp TryServePrevious); the
        // registry and requests are the same in both modes.
        const bool enabled = v && (v[0] == '1' || v[0] == '2');
        if (enabled) {
            std::fprintf(stderr, "SOTC RB CLASS: on (needs SOTC_RB_OWNER=1, SOTC_RB_MATERIALIZE=1, "
                                 "SOTC_HOOK_ALLOC_PROBE=1, SOTC_PARTICLE_PREV=1%s)\n",
                         v[0] == '2' ? ", SOTC_HOOK_XPR_PROBE=1 for the mask spans" : "");
        }
        return enabled;
    }();
    return on;
}

uint32_t Class2Parts() {
    // sotc: read once, at the first class-2 serve decision (buffer_cache.cpp TryServePrevious).
    static const uint32_t parts = [] {
        const char* v = std::getenv("SOTC_RB_CLASS2_PARTS");
        // Default 0 (no serve): the serve corrupted vertices in game (1 Nov, rb2 vs rb2p0) and
        // served ~22 pages per 5 s; the readback drop of class 2 comes from its other parts.
        const uint32_t p = v && v[0] ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
        std::fprintf(stderr, "SOTC RB CLASS 2 parts: 0x%x (1 serve, 2 window-wide drop)\n", p);
        return p;
    }();
    return parts;
}

void OnBlock(uint64_t base, uint64_t size, const char* name, int kind) {
    if (!On() || kind != 1 || !name || std::strcmp(name, "Default TaggedArena") != 0 || !base ||
        !size || size > (uint64_t{1} << 32)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    AddLocked(base, size, true);
}

void OnFreeBlock(uint64_t base) {
    if (!On() || !base) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_ranges.size(); ++i) {
        if (g_ranges[i].arena && g_ranges[i].addr == base) {
            g_ranges.erase(g_ranges.begin() + static_cast<std::ptrdiff_t>(i));
            g_version.fetch_add(1, std::memory_order_release);
            return;
        }
    }
}

void RequestCopy(uint64_t addr, uint64_t size) {
    if (!On()) {
        return;
    }
    g_asked.fetch_add(1, std::memory_order_relaxed);
    if (!addr || !size) {
        g_empty.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t bytes = size < kMaxRequestBytes ? size : kMaxRequestBytes;
    std::lock_guard<std::mutex> lock(g_request_mutex);
    // The same array asked again before the draw thread takes the list: one copy covers both, and
    // it runs no earlier than the latest request, so merging is safe.
    for (Range& r : g_requests) {
        if (r.addr == addr) {
            if (bytes > r.size) {
                r.size = bytes;
            }
            g_deduped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (g_requests.size() >= kMaxRequests) {
        g_request_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_requests.push_back({addr, bytes});
    g_queued.fetch_add(1, std::memory_order_relaxed);
}

void TakeRequests(std::vector<Range>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(g_request_mutex);
    out.swap(g_requests);
    g_taken.fetch_add(out.size(), std::memory_order_relaxed);
}

void NoteStatsReadCheck(StatsCheck check, bool beyond) {
    if (!On()) {
        return;
    }
    switch (check) {
    case StatsCheck::kNoRequest:
        g_check_none.fetch_add(1, std::memory_order_relaxed);
        break;
    case StatsCheck::kMismatch:
        g_check_mismatch.fetch_add(1, std::memory_order_relaxed);
        break;
    case StatsCheck::kMatch:
        g_check_match.fetch_add(1, std::memory_order_relaxed);
        if (beyond) {
            g_check_beyond.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
}

bool InRanges(const std::vector<Range>& ranges, uint64_t addr, uint64_t size) {
    for (const Range& r : ranges) {
        if (addr < r.addr + r.size && r.addr < addr + size) {
            return true;
        }
    }
    return false;
}

void RecordMaskSpan(uint64_t addr, uint64_t size, uint64_t frame) {
    if (!On() || !addr || !size) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mask_mutex);
    // Spans older than the bound are dropped before anything else: an old span must never be
    // extended, since its block may hold other data by now (rb_class.h).
    std::erase_if(g_mask_spans,
                  [&](const MaskSpan& s) { return s.frame + kMaskSpanFrames < frame; });
    g_mask_recorded.fetch_add(1, std::memory_order_relaxed);
    for (MaskSpan& s : g_mask_spans) {
        if (s.addr == addr && s.size == size) {
            if (frame > s.frame) {
                s.frame = frame;
            }
            return;
        }
    }
    if (g_mask_spans.size() >= kMaxMaskSpans) {
        g_mask_full.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_mask_spans.push_back({addr, size, frame});
}

std::vector<Range> MaskSpansSince(uint64_t frame) {
    std::vector<Range> spans;
    {
        std::lock_guard<std::mutex> lock(g_mask_mutex);
        for (const MaskSpan& s : g_mask_spans) {
            if (s.frame + kMaskSpanFrames >= frame) {
                spans.push_back({s.addr, s.size});
            }
        }
    }
    std::sort(spans.begin(), spans.end(),
              [](const Range& a, const Range& b) { return a.addr < b.addr; });
    std::vector<Range> merged;
    for (const Range& r : spans) {
        if (!merged.empty() && r.addr <= merged.back().addr + merged.back().size) {
            const uint64_t end = std::max(merged.back().addr + merged.back().size, r.addr + r.size);
            merged.back().size = end - merged.back().addr;
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

bool CoveredBy(const std::vector<Range>& spans, uint64_t lo, uint64_t hi) {
    if (hi <= lo) {
        return true;
    }
    // spans is sorted and merged (adjacent ones joined): [lo, hi) is covered only by the last
    // span that starts at or before lo.
    const auto it = std::upper_bound(spans.begin(), spans.end(), lo,
                                     [](uint64_t v, const Range& r) { return v < r.addr; });
    if (it == spans.begin()) {
        return false;
    }
    const Range& r = *std::prev(it);
    return lo >= r.addr && hi <= r.addr + r.size;
}

void NoteHeld(uint64_t windows) {
    g_held.fetch_add(windows, std::memory_order_relaxed);
}

void NoteRequestIssued(uint64_t windows) {
    g_request_issued.fetch_add(windows, std::memory_order_relaxed);
}

std::vector<Range> Snapshot(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_relaxed);
    std::vector<Range> out;
    out.reserve(g_ranges.size());
    for (const Entry& e : g_ranges) {
        out.push_back({e.addr, e.size});
    }
    return out;
}

uint64_t Version() {
    return g_version.load(std::memory_order_acquire);
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    size_t arenas = 0;
    size_t arrays = 0;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Entry& e : g_ranges) {
            (e.arena ? arenas : arrays) += 1;
            bytes += e.size;
        }
    }
    (void)arrays;
    const uint64_t asked = g_asked.exchange(0, std::memory_order_relaxed);
    const uint64_t queued = g_queued.exchange(0, std::memory_order_relaxed);
    const uint64_t deduped = g_deduped.exchange(0, std::memory_order_relaxed);
    const uint64_t empty = g_empty.exchange(0, std::memory_order_relaxed);
    const uint64_t dropped = g_request_dropped.exchange(0, std::memory_order_relaxed);
    const uint64_t taken = g_taken.exchange(0, std::memory_order_relaxed);
    const uint64_t held = g_held.exchange(0, std::memory_order_relaxed);
    const uint64_t issued = g_request_issued.exchange(0, std::memory_order_relaxed);
    const uint64_t none = g_check_none.exchange(0, std::memory_order_relaxed);
    const uint64_t match = g_check_match.exchange(0, std::memory_order_relaxed);
    const uint64_t mismatch = g_check_mismatch.exchange(0, std::memory_order_relaxed);
    const uint64_t beyond = g_check_beyond.exchange(0, std::memory_order_relaxed);
    const double secs = seconds > 0.0 ? seconds : 1.0;
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC RB CLASS: %zu mask arena blocks (%.1f MiB); mask windows held for the next "
                  "frame %.0f/s; stats copies asked %.0f/s, queued %.0f/s, deduped %.0f/s, empty %.0f/s, "
                  "taken %.0f/s, windows armed %.0f/s, dropped %llu%s\n",
                  arenas, bytes / 1048576.0, held / secs, asked / secs, queued / secs, deduped / secs,
                  empty / secs, taken / secs, issued / secs, static_cast<unsigned long long>(dropped),
                  g_overflow.load(std::memory_order_relaxed) ? " (table full)" : "");
    out += line;
    char check[200];
    std::snprintf(check, sizeof check,
                  "SOTC RB CLASS: stats reads checked: match %llu, mismatch %llu, no request %llu, "
                  "beyond the requested extent %llu (of the matches)\n",
                  static_cast<unsigned long long>(match), static_cast<unsigned long long>(mismatch),
                  static_cast<unsigned long long>(none), static_cast<unsigned long long>(beyond));
    out += check;
    size_t live = 0;
    {
        std::lock_guard<std::mutex> lock(g_mask_mutex);
        live = g_mask_spans.size();
    }
    const uint64_t recorded = g_mask_recorded.exchange(0, std::memory_order_relaxed);
    const uint64_t full = g_mask_full.exchange(0, std::memory_order_relaxed);
    char mask[200];
    std::snprintf(mask, sizeof mask,
                  "SOTC RB CLASS: mask spans recorded %llu (live %zu, table full %llu)\n",
                  static_cast<unsigned long long>(recorded), live,
                  static_cast<unsigned long long>(full));
    out += mask;
}

} // namespace SotcRbClass
