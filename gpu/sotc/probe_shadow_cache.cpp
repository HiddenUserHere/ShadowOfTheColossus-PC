// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: sun shadow cascade cache (SOTC_SHADOW_CACHE, docs/re-backend/12-shadow-cache.md).
//
// Hooks, all under the switch SOTC_SHADOW_CACHE (scripts/hooks.d/shadow_cache.py):
//   SHADOW_VIEW_BUILD    FUN_000c7850 (0xc7850), wrap. rsi = view_state. Builds one 0x1228-byte view
//                        object per cascade c, from the source block view_state[4] + 0x168 + c * 0xf0
//                        (0xf0 bytes), and stores it at view_state[2] + 0xdd8 + c * 8. Pre phase: the
//                        freeze (mode 1) or the comparison (mode 2). Post phase: the view object
//                        pointers of the cascades skipped in this build are recorded.
//   SHADOW_CASCADE_BODY  FUN_000cdbc0 (0xcdbc0), entry. rdi = the cascade job: job[0x11] = c, job[6] =
//                        the view object the body reads. Mode 1 returns before the body when job[6] is
//                        a recorded skip.
//
// Facts this design relies on (observed in the runs of impl-shadow3):
//   - view_state[2] is a new address on every build, so it cannot key snapshots.
//   - view_state[4] (the source block) alternates between two buffers by frame parity.
//   - Some frames have two builds (two view states in one flip); the body can run in a later flip than
//     its build. So the body is tied to its build by the view object pointer (job[6]), not by the flip.
//
// Snapshots: one per (build index within the flip, c). The build index is 0 for the first build of a
// flip and 1 for the second; builds past that are never frozen. A snapshot holds the bytes that cascade c
// was drawn with in that build position, copied at the build (view_state[4] + 0x168 + c * 0xf0), so it does
// not matter which of the two source buffers held them.
//
// Schedule (WouldSkip): cascade 2 is skipped on even flips (drawn on odd); cascade 3 is skipped on flips that
// are not multiples of 4. No flip draws both.
//
// Mode 1: a build whose cascade c is predicted to skip and whose snapshot exists writes the snapshot over
// the source block before the copy. The post phase records that build's view object pointer for c. At the
// body, a recorded pointer returns HookAction::Return (no pass, no draw). The ring is cleared when the flip
// changes, so a stale pointer never matches a later object.
// Mode 2: nothing is written and no body is skipped. A predicted build counts the bytes that the freeze
// would change (current block vs snapshot), and the snapshot is updated with the current block.
//
// Handlers return HookAction::Continue, except mode 1's skip at 0xcdbc0.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "guest_mem.h"
#include "probe_shadow_cache.h"
#include "video_core/sotc_stats.h"

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kBodyHook = "SHADOW_CASCADE_BODY";
constexpr const char* kViewHook = "SHADOW_VIEW_BUILD";
constexpr uint64_t kJobCtx = 0x18;          // job[3]: ctx
constexpr uint64_t kJobView = 0x10;         // job[2] = view_state[2]
constexpr uint64_t kJobViewObj = 0x30;      // job[6]: the cascade's view object
constexpr uint64_t kJobIndex = 0x88;        // job[0x11]: cascade index
constexpr uint64_t kBlockBase = 0x2400;     // ctx + 0x2400 + (c + 0x23) * 0x50: the 80-byte block
constexpr uint64_t kBlockStride = 0x50;
constexpr uint64_t kBlockIndexBias = 0x23;
constexpr uint64_t kViewState2 = 0x10;      // view_state[2]
constexpr uint64_t kViewSources = 0x20;     // view_state[4]: the matrix sources
constexpr uint64_t kSourceBase = 0x168;     // + c * 0xf0
constexpr uint64_t kSourceStride = 0xf0;
constexpr uint64_t kViewObjSlots = 0xdd8;   // view_state[2] + 0xdd8 + c * 8
constexpr size_t kBytes = kSourceStride;
constexpr size_t kBuildSlots = 2;           // builds per flip that can be frozen
constexpr size_t kRing = 32;                // recorded skips per flip
constexpr uint64_t kLogFrames = 20;         // flips logged per hook

// The schedule: cascade 2 skipped on even flips, cascade 3 skipped unless the flip is a multiple of 4.
bool WouldSkip(int c, uint64_t flip) {
    if (c == 2) {
        return (flip & 1) == 0;
    }
    if (c == 3) {
        return (flip & 3) != 0;
    }
    return false;
}

// Reads n bytes in 8-byte chunks (aligned, so a chunk never crosses a page). Failed chunks are
// zeroed and counted.
bool ReadChunks(uint64_t addr, unsigned char* out, size_t n, uint64_t* failed) {
    bool all = true;
    for (size_t i = 0; i < n; i += 8) {
        const size_t k = std::min<size_t>(8, n - i);
        if (!SotcGuest::Read(addr + i, out + i, k)) {
            std::memset(out + i, 0, k);
            ++*failed;
            all = false;
        }
    }
    return all;
}

// Writes n bytes in 8-byte chunks. False when any chunk was refused.
bool WriteChunks(uint64_t addr, const unsigned char* in, size_t n, uint64_t* failed) {
    bool all = true;
    for (size_t i = 0; i < n; i += 8) {
        const size_t k = std::min<size_t>(8, n - i);
        if (!SotcGuest::Write(addr + i, in + i, k)) {
            ++*failed;
            all = false;
        }
    }
    return all;
}

// The bytes each cascade was last drawn with, for one build position of a flip.
struct Snapshot {
    std::array<std::array<unsigned char, kBytes>, 5> bytes{};
    std::array<bool, 5> have{};
};

// The state of the build being run (pre phase), used by its post phase.
struct Pending {
    bool valid = false;
    uint64_t state = 0;   // view_state
    uint64_t view2 = 0;   // view_state[2] at the build
    std::array<bool, 5> skip{};
};

// A recorded skip: the view object of cascade c whose body must return.
struct Entry {
    bool used = false;
    uint64_t obj = 0;
};

std::mutex g_mutex;
std::array<Snapshot, kBuildSlots> g_snap{};
Pending g_pending{};
std::array<Entry, kRing> g_ring{};
unsigned g_ring_next = 0;
uint64_t g_flip = ~uint64_t{0};             // the flip of the current builds
int g_builds_in_flip = 0;
bool g_multi_counted = false;

// Window counters (report line).
uint64_t g_frames = 0;                      // flips seen at c7850
uint64_t g_multi_flips = 0;                 // flips with two or more builds
uint64_t g_builds = 0;                      // c7850 calls
std::array<uint64_t, 5> g_drawn{};          // bodies that ran
std::array<uint64_t, 5> g_skipped{};        // mode 1: bodies returned before the body
std::array<uint64_t, 5> g_predicted{};      // builds (first of the flip) predicted to skip
uint64_t g_restores = 0;                    // mode 1: freezes written; mode 2: freezes that would be written
uint64_t g_changed = 0;                     // of those, bytes that differ from the current block
uint64_t g_no_snapshot = 0;                 // predicted builds with no snapshot yet
uint64_t g_recorded = 0;                    // skips recorded at c7850 post
uint64_t g_matched = 0;                     // bodies that found a recorded skip
uint64_t g_freeze_failed = 0;               // mode 1: writes refused
uint64_t g_chunks_failed = 0;               // source chunk reads or writes refused
uint64_t g_state_unreadable = 0;            // view_state[2] or view_state[4] not readable
uint64_t g_slot_unreadable = 0;             // a view object slot not readable at post
uint64_t g_block_misses = 0;                // reads of the constant block that failed
uint64_t g_view_mismatch = 0;               // job[6] != view_state[2] + 0xdd8 + c * 8

// c7850 pre: once per build. Mode 1 freezes the skipped cascades; mode 2 compares.
HookAction OnViewBuildPre(GuestRegs& regs, void*) {
    static const int mode = SotcHooks::HookMode(kViewHook);
    if (mode == 0) {
        return HookAction::Continue;
    }
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    const uint64_t state = regs.rsi;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (flip != g_flip) {
        g_flip = flip;
        g_builds_in_flip = 0;
        g_multi_counted = false;
        for (auto& e : g_ring) {
            e.used = false;
        }
        ++g_frames;
    }
    const int bidx = g_builds_in_flip++;
    ++g_builds;
    if (bidx == 1 && !g_multi_counted) {
        g_multi_counted = true;
        ++g_multi_flips;
    }
    g_pending = Pending{};
    g_pending.state = state;
    g_pending.valid = true;

    uint64_t view2 = 0, src = 0;
    const bool v2_ok = state != 0 && SotcGuest::Read(state + kViewState2, &view2) && view2 != 0;
    const bool src_ok = v2_ok && SotcGuest::Read(state + kViewSources, &src) && src != 0;
    if (!src_ok) {
        ++g_state_unreadable;
        if (flip < kLogFrames) {
            std::fprintf(stderr, "SOTC SHADOW CACHE build flip %llu bidx %d state 0x%llx unreadable (view2 %d src %d)\n",
                         static_cast<unsigned long long>(flip), bidx, static_cast<unsigned long long>(state),
                         v2_ok ? 1 : 0, src_ok ? 1 : 0);
        }
        g_pending.valid = false;
        return HookAction::Continue;
    }
    g_pending.view2 = view2;

    const bool frozen_slot = bidx < static_cast<int>(kBuildSlots);
    Snapshot* snap = frozen_slot ? &g_snap[static_cast<size_t>(bidx)] : nullptr;
    char seen[200] = {};
    int len = 0;
    for (int c = 0; c < 5; ++c) {
        const uint64_t region = src + kSourceBase + static_cast<uint64_t>(c) * kSourceStride;
        std::array<unsigned char, kBytes> cur{};
        const bool cur_ok = ReadChunks(region, cur.data(), kBytes, &g_chunks_failed);
        const bool predicted = WouldSkip(c, flip);
        const bool have = snap && snap->have[c];
        if (predicted && bidx == 0) {
            ++g_predicted[c];
        }
        if (predicted && snap && !have) {
            ++g_no_snapshot;
        }
        if (predicted && have) {
            const bool changed = !cur_ok || std::memcmp(cur.data(), snap->bytes[c].data(), kBytes) != 0;
            ++g_restores;
            g_changed += changed ? 1 : 0;
            if (mode == 1) {
                if (WriteChunks(region, snap->bytes[c].data(), kBytes, &g_chunks_failed)) {
                    g_pending.skip[c] = true;
                } else {
                    ++g_freeze_failed;
                }
            }
        }
        if (snap && cur_ok && !g_pending.skip[c]) {
            snap->bytes[c] = cur;
            snap->have[c] = true;
        }
        if (flip < kLogFrames && len < static_cast<int>(sizeof seen) - 24) {
            len += std::snprintf(seen + len, sizeof seen - len, " c%d p%d h%d r%d s%d", c, predicted ? 1 : 0,
                                 have ? 1 : 0, cur_ok ? 1 : 0, g_pending.skip[c] ? 1 : 0);
        }
    }
    if (flip < kLogFrames) {
        std::fprintf(stderr,
                     "SOTC SHADOW CACHE build flip %llu bidx %d view2 0x%llx src 0x%llx:%s (mode %d)\n",
                     static_cast<unsigned long long>(flip), bidx, static_cast<unsigned long long>(view2),
                     static_cast<unsigned long long>(src), seen, mode);
    }
    return HookAction::Continue;
}

// c7850 post: the view objects built in this call are now in view_state[2] + 0xdd8 + c * 8. Record the
// pointers of the skipped cascades, so their bodies can match them.
void OnViewBuildPost(GuestRegs&, void*) {
    static const int mode = SotcHooks::HookMode(kViewHook);
    if (mode != 1) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pending.valid) {
        return;
    }
    g_pending.valid = false;
    for (int c = 0; c < 5; ++c) {
        if (!g_pending.skip[c]) {
            continue;
        }
        uint64_t obj = 0;
        if (!SotcGuest::Read(g_pending.view2 + kViewObjSlots + static_cast<uint64_t>(c) * 8, &obj) || obj == 0) {
            ++g_slot_unreadable;
            continue;
        }
        Entry& e = g_ring[g_ring_next];
        g_ring_next = (g_ring_next + 1) % g_ring.size();
        e.used = true;
        e.obj = obj;
        ++g_recorded;
    }
}

// 0xcdbc0 entry: mode 1 returns before the body when its view object was recorded as a skip.
HookAction OnCascadeBody(GuestRegs& regs, void*) {
    static const int mode = SotcHooks::HookMode(kBodyHook);
    if (mode == 0) {
        return HookAction::Continue;
    }
    const uint64_t job = regs.rdi;
    uint32_t raw_index = 0;
    uint64_t ctx = 0, view2 = 0, obj = 0;
    if (!SotcGuest::Read(job + kJobIndex, &raw_index) || !SotcGuest::Read(job + kJobCtx, &ctx) ||
        !SotcGuest::Read(job + kJobView, &view2)) {
        return HookAction::Continue;
    }
    const int c = static_cast<int>(raw_index);
    if (c < 0 || c > 4 || ctx == 0 || view2 == 0) {
        return HookAction::Continue;
    }
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    const uint64_t block = ctx + kBlockBase + (static_cast<uint64_t>(c) + kBlockIndexBias) * kBlockStride;
    uint64_t q[10] = {};
    const bool block_ok = SotcGuest::Read(block, q, sizeof q);
    const bool obj_ok = SotcGuest::Read(job + kJobViewObj, &obj);
    uint64_t slot = 0;
    const bool slot_ok = SotcGuest::Read(view2 + kViewObjSlots + static_cast<uint64_t>(c) * 8, &slot);

    bool skip = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!block_ok) {
            ++g_block_misses;
        }
        if (!obj_ok || !slot_ok || obj != slot) {
            ++g_view_mismatch;
        }
        if (mode == 1 && obj_ok) {
            for (auto& e : g_ring) {
                if (e.used && e.obj == obj) {
                    e.used = false;
                    skip = true;
                    ++g_matched;
                    break;
                }
            }
        }
        if (skip) {
            ++g_skipped[c];
        } else {
            ++g_drawn[c];
        }
    }
    if (flip < kLogFrames) {
        std::fprintf(stderr,
                     "SOTC SHADOW CACHE body flip %llu c %d obj 0x%llx match %d would_skip %d (mode %d)\n",
                     static_cast<unsigned long long>(flip), c, static_cast<unsigned long long>(obj),
                     skip ? 1 : 0, WouldSkip(c, flip) ? 1 : 0, mode);
    }
    if (skip) {
        return HookAction::Return;
    }
    return HookAction::Continue;
}

} // namespace

void RegisterShadowCache() {
    SotcHooks::Handler body;
    body.pre = &OnCascadeBody;
    body.post = nullptr;
    body.user = nullptr;
    SotcHooks::RegisterHandler(kBodyHook, body);

    SotcHooks::Handler view;
    view.pre = &OnViewBuildPre;
    view.post = &OnViewBuildPost;
    view.user = nullptr;
    SotcHooks::RegisterHandler(kViewHook, view);
}

void ReportShadowCache(std::string& out, double seconds) {
    (void)seconds;
    if (!SotcHooks::IsInstalled(kBodyHook)) {
        return;
    }
    const int mode = SotcHooks::HookMode(kBodyHook);
    std::array<uint64_t, 5> drawn{}, skipped{}, predicted{};
    uint64_t frames = 0, multi = 0, builds = 0, restores = 0, changed = 0, nosnap = 0, recorded = 0,
             matched = 0, failed = 0, chunks = 0, unreadable = 0, slot_bad = 0, misses = 0, mismatch = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        drawn = g_drawn;
        skipped = g_skipped;
        predicted = g_predicted;
        frames = g_frames;
        multi = g_multi_flips;
        builds = g_builds;
        restores = g_restores;
        changed = g_changed;
        nosnap = g_no_snapshot;
        recorded = g_recorded;
        matched = g_matched;
        failed = g_freeze_failed;
        chunks = g_chunks_failed;
        unreadable = g_state_unreadable;
        slot_bad = g_slot_unreadable;
        misses = g_block_misses;
        mismatch = g_view_mismatch;
        g_drawn = {};
        g_skipped = {};
        g_predicted = {};
        g_frames = 0;
        g_multi_flips = 0;
        g_builds = 0;
        g_restores = 0;
        g_changed = 0;
        g_no_snapshot = 0;
        g_recorded = 0;
        g_matched = 0;
        g_freeze_failed = 0;
        g_chunks_failed = 0;
        g_state_unreadable = 0;
        g_slot_unreadable = 0;
        g_block_misses = 0;
        g_view_mismatch = 0;
    }
    if (frames == 0) {
        return;
    }
    const double f = static_cast<double>(frames);
    char line[800];
    std::snprintf(
        line, sizeof line,
        "SOTC SHADOW CACHE: mode %d%s, cascades drawn per flip d0..d4 = %.2f %.2f %.2f %.2f %.2f, "
        "skipped s2/s3 = %llu/%llu per window (%s; predicted builds %llu/%llu), restores R = %llu (bytes "
        "changed %llu, no snapshot %llu, freeze refused %llu, recorded %llu, matched %llu), flips %llu "
        "(builds %llu, flips with two builds %llu), block reads failed %llu, view mismatches %llu, "
        "source chunks failed %llu, state unreadable %llu, slot unreadable %llu\n",
        mode, mode == 1 ? " (freeze and skip)" : (mode == 2 ? " (log only)" : " (not implemented)"),
        drawn[0] / f, drawn[1] / f, drawn[2] / f, drawn[3] / f, drawn[4] / f,
        static_cast<unsigned long long>(skipped[2]), static_cast<unsigned long long>(skipped[3]),
        mode == 1 ? "bodies skipped" : "would skip, not done",
        static_cast<unsigned long long>(predicted[2]), static_cast<unsigned long long>(predicted[3]),
        static_cast<unsigned long long>(restores), static_cast<unsigned long long>(changed),
        static_cast<unsigned long long>(nosnap), static_cast<unsigned long long>(failed),
        static_cast<unsigned long long>(recorded), static_cast<unsigned long long>(matched),
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(builds),
        static_cast<unsigned long long>(multi), static_cast<unsigned long long>(misses),
        static_cast<unsigned long long>(mismatch), static_cast<unsigned long long>(chunks),
        static_cast<unsigned long long>(unreadable), static_cast<unsigned long long>(slot_bad));
    out += line;
}

} // namespace SotcProbes
