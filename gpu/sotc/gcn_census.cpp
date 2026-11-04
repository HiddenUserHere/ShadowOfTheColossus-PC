// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_CENSUS (gcn_census.h).

#include "gcn_census.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace SotcGcnCensus {
namespace {

constexpr uint64_t kWindowNs = 5'000'000'000ull;
constexpr size_t kSeenCap = size_t{1} << 20; // the session's T# set is reset past this (counted)

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Draw thread only: no locks.
struct Window {
    uint64_t start_ns = 0;
    uint64_t ops = 0, kinds[kKinds] = {};
    uint64_t writes_memory = 0, storage = 0, fmask = 0, dma = 0;
    uint64_t buf_plain = 0, buf_formatted = 0, buf_written = 0, buf_special = 0, buf_host = 0, buf_bytes = 0;
    uint64_t size_hist[kSizeBuckets] = {};
    uint64_t img_sampled = 0, img_storage = 0, img_array = 0, img_3d = 0, img_cube = 0, img_mip = 0,
             img_desc_array = 0, samplers = 0;
    uint64_t frames = 0, tsharp_distinct = 0, tsharp_new = 0, ssharp_distinct = 0;
    uint64_t max_tsharp_frame = 0;
};

struct State {
    Window w;
    uint64_t frame = ~0ull;
    std::unordered_set<uint64_t> frame_t, frame_s, seen_t;
    uint64_t seen_resets = 0;
};

State& S() {
    static State s;
    return s;
}

void CloseFrame(State& s) {
    if (s.frame == ~0ull) {
        return;
    }
    s.w.frames++;
    s.w.tsharp_distinct += s.frame_t.size();
    s.w.ssharp_distinct += s.frame_s.size();
    if (s.frame_t.size() > s.w.max_tsharp_frame) {
        s.w.max_tsharp_frame = s.frame_t.size();
    }
    s.frame_t.clear();
    s.frame_s.clear();
}

void Print(State& s, uint64_t now) {
    Window& w = s.w;
    const double secs = static_cast<double>(now - w.start_ns) / 1e9;
    const double ops = static_cast<double>(w.ops ? w.ops : 1);
    const double frames = static_cast<double>(w.frames ? w.frames : 1);
    const auto pct = [&](uint64_t v) { return 100.0 * static_cast<double>(v) / ops; };
    const auto per = [&](uint64_t v) { return static_cast<double>(v) / ops; };
    std::fprintf(stderr,
                 "SOTC GCN CENSUS (%.1f s, %llu frames, %.0f ops/frame): ops by kind [draw %.1f%%, draw indirect "
                 "%.1f%%, dispatch %.1f%%, dispatch indirect %.1f%%]; shaders writing memory %.1f%%, storage images "
                 "%.1f%%, fmasks %.1f%%, DMA reads %.1f%%\n",
                 secs, static_cast<unsigned long long>(w.frames), static_cast<double>(w.ops) / frames,
                 pct(w.kinds[kDraw]), pct(w.kinds[kDrawIndirect]), pct(w.kinds[kDispatch]),
                 pct(w.kinds[kDispatchIndirect]), pct(w.writes_memory), pct(w.storage), pct(w.fmask), pct(w.dma));
    const uint64_t guest = w.buf_plain + w.buf_formatted + w.buf_written;
    const double hist_total = static_cast<double>(guest ? guest : 1);
    std::fprintf(stderr,
                 "SOTC GCN CENSUS buffers per op: plain read %.2f, formatted %.2f, written %.2f, special %.2f; guest "
                 "ones in the imported arena %.1f%%; %.1f KiB bound per op; sizes [<4K %.1f%%, <64K %.1f%%, <1M "
                 "%.1f%%, <16M %.1f%%, larger %.1f%%]\n",
                 per(w.buf_plain), per(w.buf_formatted), per(w.buf_written), per(w.buf_special),
                 100.0 * static_cast<double>(w.buf_host) / hist_total,
                 static_cast<double>(w.buf_bytes) / ops / 1024.0, 100.0 * w.size_hist[0] / hist_total,
                 100.0 * w.size_hist[1] / hist_total, 100.0 * w.size_hist[2] / hist_total,
                 100.0 * w.size_hist[3] / hist_total, 100.0 * w.size_hist[4] / hist_total);
    std::fprintf(stderr,
                 "SOTC GCN CENSUS images per op: sampled %.2f, storage %.2f (arrays %.2f, 3D %.2f, cubes %.2f, mip "
                 "tails %.2f, descriptor arrays %.2f), samplers %.2f; per frame: distinct T# %.0f (max %llu), new T# "
                 "%.1f, distinct S# %.0f; session T# set resets %llu\n",
                 per(w.img_sampled), per(w.img_storage), per(w.img_array), per(w.img_3d), per(w.img_cube),
                 per(w.img_mip), per(w.img_desc_array), per(w.samplers),
                 static_cast<double>(w.tsharp_distinct) / frames, static_cast<unsigned long long>(w.max_tsharp_frame),
                 static_cast<double>(w.tsharp_new) / frames, static_cast<double>(w.ssharp_distinct) / frames,
                 static_cast<unsigned long long>(s.seen_resets));
    w = Window{};
    w.start_ns = now;
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_GCN_CENSUS");
        return v && v[0] == '1';
    }();
    return on;
}

void Note(const Op& op, const uint64_t* tsharps, uint32_t tsharp_count, const uint64_t* ssharps,
          uint32_t ssharp_count, uint64_t frame) noexcept {
    State& s = S();
    const uint64_t now = NowNs();
    if (s.w.start_ns == 0) {
        s.w.start_ns = now;
    }
    if (frame != s.frame) {
        CloseFrame(s);
        s.frame = frame;
    }
    Window& w = s.w;
    w.ops++;
    w.kinds[op.kind < kKinds ? op.kind : kDraw]++;
    w.writes_memory += op.writes_memory;
    w.storage += op.storage;
    w.fmask += op.fmask;
    w.dma += op.dma;
    w.buf_plain += op.buf_plain;
    w.buf_formatted += op.buf_formatted;
    w.buf_written += op.buf_written;
    w.buf_special += op.buf_special;
    w.buf_host += op.buf_host;
    w.buf_bytes += op.buf_bytes;
    for (uint32_t i = 0; i < kSizeBuckets; ++i) {
        w.size_hist[i] += op.size_hist[i];
    }
    w.img_sampled += op.img_sampled;
    w.img_storage += op.img_storage;
    w.img_array += op.img_array;
    w.img_3d += op.img_3d;
    w.img_cube += op.img_cube;
    w.img_mip += op.img_mip;
    w.img_desc_array += op.img_desc_array;
    w.samplers += op.samplers;
    for (uint32_t i = 0; i < tsharp_count; ++i) {
        s.frame_t.insert(tsharps[i]);
        if (s.seen_t.insert(tsharps[i]).second) {
            w.tsharp_new++;
            if (s.seen_t.size() > kSeenCap) {
                s.seen_t.clear();
                s.seen_resets++;
            }
        }
    }
    for (uint32_t i = 0; i < ssharp_count; ++i) {
        s.frame_s.insert(ssharps[i]);
    }
    if (now - w.start_ns >= kWindowNs) {
        Print(s, now);
    }
}

} // namespace SotcGcnCensus
