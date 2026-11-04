// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_GATE (draw_gate.h).

#include "draw_gate.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "preupload.h"

namespace SotcDrawGate {
namespace {

uint64_t EnvU64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    return v && v[0] ? std::strtoull(v, nullptr, 10) : fallback;
}

constexpr uint64_t kGranuleBits = 20; // 1 MiB: ranges are marked per granule (the frame rule and the cap)

struct Granule {
    uint64_t first_defer = ~0ull; // frame of the first deferral of the current wait
    uint64_t last_defer = ~0ull;  // frame of the last deferral
};

struct State {
    uint64_t budget = EnvU64("SOTC_DRAW_GATE_MB", 10) << 20;
    uint64_t min_bytes = EnvU64("SOTC_DRAW_GATE_MIN_KB", 256) << 10;
    uint64_t cap_frames = std::max<uint64_t>(1, EnvU64("SOTC_DRAW_GATE_FRAMES", 3));
    uint64_t frame = ~0ull;
    uint64_t admitted = 0; // pending bytes of the draws made this frame
    std::unordered_map<uint64_t, Granule> granules;
    uint64_t last_counted_frame = ~0ull;
    // Report figures: the draw thread adds, the report thread takes them (exchange).
    std::atomic<uint64_t> draws_deferred{0}, draws_followed{0}, draws_capped{0}, bytes_deferred{0}, bytes_admitted{0};
    std::atomic<uint64_t> frames_with_deferral{0}, waiting{0};
};

State& S() {
    static State s;
    return s;
}

template <typename F>
void ForGranules(const Range& r, F&& f) {
    if (r.size == 0) {
        return;
    }
    const uint64_t first = r.addr >> kGranuleBits;
    const uint64_t last = (r.addr + r.size - 1) >> kGranuleBits;
    for (uint64_t g = first; g <= last; ++g) {
        f(g);
    }
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DRAW_GATE");
        const bool wanted = v && v[0] == '1';
        if (wanted && SotcPreupload::Mode() != 2) {
            std::fprintf(stderr, "SOTC DRAW GATE: needs SOTC_PRELOAD_UPLOAD=2 (its idle uploader), off\n");
            return false;
        }
        if (wanted) {
            std::fprintf(stderr, "SOTC DRAW GATE: on\n");
        }
        return wanted;
    }();
    return on;
}

bool Decide(const Range* ranges, size_t count, bool may_defer, bool may_follow, uint64_t frame) noexcept {
    State& s = S();
    if (frame != s.frame) {
        s.frame = frame;
        s.admitted = 0;
        // Entries of granules neither deferred lately nor waiting go (the map stays small).
        if ((frame & 63) == 0) {
            std::erase_if(s.granules, [&](const auto& kv) {
                return kv.second.last_defer == ~0ull || frame - kv.second.last_defer > 2 * s.cap_frames + 8;
            });
        }
    }
    uint64_t pending = 0;
    for (size_t i = 0; i < count; ++i) {
        pending += ranges[i].pending;
    }
    // The frame rule: a range deferred earlier in this frame defers every draw that reads it.
    bool skip = false;
    bool followed = false;
    if (may_follow) {
        for (size_t i = 0; i < count && !skip; ++i) {
            ForGranules(ranges[i], [&](uint64_t g) {
                const auto it = s.granules.find(g);
                if (it != s.granules.end() && it->second.last_defer == frame) {
                    skip = true;
                }
            });
        }
        followed = skip;
    }
    // The budget: the frame's uploads by draws are spent.
    if (!skip && may_defer && pending >= s.min_bytes && s.admitted + pending > s.budget) {
        bool capped = false;
        for (size_t i = 0; i < count && !capped; ++i) {
            if (ranges[i].pending == 0) {
                continue;
            }
            ForGranules(ranges[i], [&](uint64_t g) {
                const auto it = s.granules.find(g);
                if (it != s.granules.end() && it->second.first_defer != ~0ull &&
                    frame - it->second.first_defer >= s.cap_frames) {
                    capped = true;
                }
            });
        }
        if (capped) {
            s.draws_capped.fetch_add(1, std::memory_order_relaxed);
        } else {
            skip = true;
        }
    }
    if (skip) {
        for (size_t i = 0; i < count; ++i) {
            const Range& r = ranges[i];
            if (r.pending == 0) {
                continue;
            }
            ForGranules(r, [&](uint64_t g) {
                Granule& gr = s.granules[g];
                if (gr.first_defer == ~0ull) {
                    gr.first_defer = frame;
                }
                gr.last_defer = frame;
            });
            SotcPreupload::QueueUrgent(r.addr, r.size);
        }
        (followed ? s.draws_followed : s.draws_deferred).fetch_add(1, std::memory_order_relaxed);
        s.bytes_deferred.fetch_add(pending, std::memory_order_relaxed);
        if (s.last_counted_frame != frame) {
            s.last_counted_frame = frame;
            s.frames_with_deferral.fetch_add(1, std::memory_order_relaxed);
        }
        s.waiting.store(s.granules.size(), std::memory_order_relaxed);
        return true;
    }
    s.admitted += pending;
    s.bytes_admitted.fetch_add(pending, std::memory_order_relaxed);
    // Drawn: its granules' wait is over (those deferred this very frame keep their mark for the frame rule).
    for (size_t i = 0; i < count; ++i) {
        ForGranules(ranges[i], [&](uint64_t g) {
            const auto it = s.granules.find(g);
            if (it != s.granules.end() && it->second.last_defer != frame) {
                s.granules.erase(it);
            }
        });
    }
    s.waiting.store(s.granules.size(), std::memory_order_relaxed);
    return false;
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    State& s = S();
    const double mib = 1024.0 * 1024.0;
    char line[320];
    std::snprintf(line, sizeof line,
                  "SOTC DRAW GATE (%.1f s): deferred %llu draws (+%llu by the frame rule) in %llu frames, %.1f MiB "
                  "left to the staged upload; drawn with uploads %.1f MiB; capped %llu; ranges waiting %zu\n",
                  seconds, (unsigned long long)s.draws_deferred.exchange(0, std::memory_order_relaxed),
                  (unsigned long long)s.draws_followed.exchange(0, std::memory_order_relaxed),
                  (unsigned long long)s.frames_with_deferral.exchange(0, std::memory_order_relaxed),
                  s.bytes_deferred.exchange(0, std::memory_order_relaxed) / mib,
                  s.bytes_admitted.exchange(0, std::memory_order_relaxed) / mib,
                  (unsigned long long)s.draws_capped.exchange(0, std::memory_order_relaxed),
                  static_cast<size_t>(s.waiting.load(std::memory_order_relaxed)));
    out += line;
}

} // namespace SotcDrawGate
