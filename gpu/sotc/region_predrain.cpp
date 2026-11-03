// sotc: SOTC_REGION_PREDRAIN and SOTC_BIG_BIND_LOG, see region_predrain.h.
//
// Why (docs/re-backend/15-burst-stutters.md, sections 2.2 and 2.3, fix 1A). A 16 MiB tracking region
// starts with every CPU bit set (RegionManager's constructor), so the first bind that reaches a fresh
// region copies all of it on the draw thread, in one frame. Predrain queues each region when it is
// created and, in the draw thread's idle gaps, uploads its CPU-dirty runs in 1 MiB pieces through
// BufferCache::PredrainRange (the DrainRange path). A later bind then finds those pieces clean.
// At most SOTC_REGION_PREDRAIN_MB (default 8) are uploaded per presented frame.
//
// Correctness: the same as an early bind (DrainRange, SOTC_LOAD_STREAM). An upload copies the bytes the
// CPU wrote before it, then ForEachUploadRange clears the CPU bits and re-protects the pages. A CPU write
// after that faults and sets the bits again, so the next bind (or drain) copies the range again before
// the GPU reads it. The GPU only reads the arena through a bind, and every bind synchronizes first, so an
// early upload never hides a write that lands before that bind. Only the timing of the copy changes.
//
// Cost: arena residency for regions that no bind ever uses (VRAM), and copy bandwidth on the draw thread.
// Ranges of SOTC_CLASS_TRACK CPU-only classes are not uploaded.
//
// Counters are printed by Report() in the 5 s driver report and reset there. "Fresh" regions are those
// created and not yet synced by any upload (a pending bit, cleared by SynchronizeMemoryFull).
#include "region_predrain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "load_warm.h"
#include "mem_classes.h"

namespace SotcRegionPredrain {
namespace {

constexpr std::uint64_t kRegionShift = 24;                       // 16 MiB regions (HIGHER_PAGE_BITS)
constexpr std::uint64_t kRegionSize = std::uint64_t{1} << kRegionShift;
constexpr std::uint64_t kPieceSize = std::uint64_t{1} << 20;     // 1 MiB drain pieces
constexpr std::uint64_t kMaxRegions = std::uint64_t{1} << (40 - kRegionShift); // guest addresses < 2^40
constexpr std::uint64_t kPendingWords = kMaxRegions / 64;
constexpr std::uint64_t kBigSyncBytes = std::uint64_t{4} << 20; // a bind sync counted as big
constexpr unsigned kCallsPerGap = 64;                           // pieces per idle gap, at most
constexpr std::size_t kMaxLoggedKeys = 256;

const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_REGION_PREDRAIN");
    return v && v[0] == '1';
}();
const bool kBigBindLog = [] {
    const char* v = std::getenv("SOTC_BIG_BIND_LOG");
    return v && v[0] == '1';
}();
// SOTC_REGION_PREDRAIN_MB: uploads per presented frame, 1-256, default 8.
const std::uint64_t kFrameCapBytes = [] {
    const char* v = std::getenv("SOTC_REGION_PREDRAIN_MB");
    const long long mb = v ? std::atoll(v) : 8;
    return static_cast<std::uint64_t>(std::clamp<long long>(mb, 1, 256)) << 20;
}();

// Pending bits: one per region, set when created, cleared by the first sync that covers it.
std::atomic<std::uint64_t> g_pending[kPendingWords]{};
std::atomic<std::uint64_t> g_pending_count{0};

// The queue of predrain: region base addresses, in creation order. Any thread pushes, the draw thread pops.
std::mutex g_mutex;
std::deque<std::uint64_t> g_queue;
std::atomic<bool> g_has_work{false};

// Draw thread only (Idle).
std::uint64_t s_cur_base = 0;
std::uint64_t s_cur_off = 0;
bool s_cur_valid = false;
std::uint64_t s_frame = ~0ULL;
std::uint64_t s_frame_bytes = 0;

// Counters of the current 5 s window (exchanged to zero by Report).
std::atomic<std::uint64_t> s_pd_bytes{0}, s_pd_regions{0}, s_pd_calls{0}, s_pd_queued{0};
std::atomic<std::uint64_t> s_bb_syncs{0}, s_bb_bytes{0}, s_bb_max{0}, s_bb_fresh{0};

// SOTC_BIG_BIND_LOG: the bind context and the keys already logged.
struct BindContext {
    bool valid = false;
    std::uint64_t pgm_hash = 0;
    std::uint32_t hw_stage = 0;
    std::uint32_t sw_stage = 0;
    std::uint32_t binding = 0;
    std::uint64_t base = 0;
    std::uint32_t vsize = 0;
    std::uint32_t num_records = 0;
    std::uint32_t stride = 0;
};
thread_local BindContext t_ctx{};
std::mutex g_log_mutex;
std::unordered_set<std::uint64_t> g_logged;

// SOTC_CLASS_TRACK CPU-only ranges, refreshed when the class table version moves (draw thread).
std::vector<SotcMemClasses::Range> s_cpu_only;
std::uint64_t s_cpu_only_version = ~0ULL;

std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

bool Enabled() noexcept {
    return kEnabled;
}

bool BigBindLogEnabled() noexcept {
    return kBigBindLog;
}

bool TrackOn() noexcept {
    return kEnabled || kBigBindLog;
}

void NoteRegionCreated(std::uint64_t region_index) noexcept {
    if (region_index >= kMaxRegions) {
        return;
    }
    const std::uint64_t bit = std::uint64_t{1} << (region_index & 63);
    const std::uint64_t old = g_pending[region_index >> 6].fetch_or(bit, std::memory_order_acq_rel);
    if (!(old & bit)) {
        g_pending_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (!kEnabled) {
        return;
    }
    {
        std::lock_guard lk{g_mutex};
        g_queue.push_back(region_index << kRegionShift);
        g_has_work.store(true, std::memory_order_relaxed);
    }
    s_pd_queued.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t NoteWindowSynced(std::uint64_t addr, std::uint64_t size) noexcept {
    if (size == 0 || g_pending_count.load(std::memory_order_relaxed) == 0) {
        return 0;
    }
    const std::uint64_t first = addr >> kRegionShift;
    std::uint64_t last = (addr + size - 1) >> kRegionShift;
    last = std::min(last, kMaxRegions - 1);
    std::uint32_t cleared = 0;
    for (std::uint64_t idx = first; idx <= last && idx < kMaxRegions; ++idx) {
        const std::uint64_t bit = std::uint64_t{1} << (idx & 63);
        const std::uint64_t old = g_pending[idx >> 6].fetch_and(~bit, std::memory_order_acq_rel);
        if (old & bit) {
            ++cleared;
            g_pending_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return cleared;
}

bool HasWork() noexcept {
    return g_has_work.load(std::memory_order_relaxed);
}

unsigned Idle(std::uint64_t budget_ns, std::uint64_t frame, DrainFn drain, StopFn stop, void* ctx) {
    if (!kEnabled || !g_has_work.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (frame != s_frame) {
        s_frame = frame;
        s_frame_bytes = 0;
    }
    if (s_frame_bytes >= kFrameCapBytes) {
        return 0; // this frame's share is used
    }
    const std::uint64_t deadline = Now() + budget_ns;
    unsigned calls = 0;
    while (calls < kCallsPerGap) {
        if (!s_cur_valid) {
            std::lock_guard lk{g_mutex};
            if (g_queue.empty()) {
                g_has_work.store(false, std::memory_order_relaxed);
                break;
            }
            s_cur_base = g_queue.front();
            g_queue.pop_front();
            s_cur_off = 0;
            s_cur_valid = true;
        }
        const std::uint64_t addr = s_cur_base + s_cur_off;
        const std::uint64_t size = std::min(kPieceSize, kRegionSize - s_cur_off);
        s_cur_off += size;
        ++calls;
        s_pd_calls.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t uploaded;
        {
            // Not a guest bind: the learned set of SOTC_LOAD_WARM ignores these binds.
            SotcLoadWarm::WarmScope scope;
            uploaded = drain(ctx, addr, size);
        }
        if (uploaded != 0) {
            s_frame_bytes += uploaded;
            s_pd_bytes.fetch_add(uploaded, std::memory_order_relaxed);
        }
        if (s_cur_off >= kRegionSize) {
            s_cur_valid = false;
            s_pd_regions.fetch_add(1, std::memory_order_relaxed);
        }
        if (stop(ctx) || Now() >= deadline || s_frame_bytes >= kFrameCapBytes) {
            break;
        }
    }
    return calls;
}

bool InCpuOnlyClass(std::uint64_t addr, std::uint64_t size) noexcept {
    if (!SotcMemClasses::On() || size == 0) {
        return false;
    }
    const std::uint64_t version_now = SotcMemClasses::Version();
    if (version_now != s_cpu_only_version) {
        std::uint64_t version = 0;
        s_cpu_only = SotcMemClasses::Snapshot(version);
        s_cpu_only_version = version;
    }
    const std::uint64_t end = addr + size;
    for (const auto& r : s_cpu_only) {
        if (r.addr < end && addr < r.addr + r.size) {
            return true;
        }
    }
    return false;
}

void SetBindContext(std::uint64_t pgm_hash, std::uint32_t hw_stage, std::uint32_t sw_stage,
                    std::uint32_t binding, std::uint64_t base, std::uint32_t vsize,
                    std::uint32_t num_records, std::uint32_t stride) noexcept {
    t_ctx = BindContext{true, pgm_hash, hw_stage, sw_stage, binding, base, vsize, num_records, stride};
}

void ClearBindContext() noexcept {
    t_ctx.valid = false;
}

void NoteBindSync(std::uint64_t addr, std::uint64_t size, std::uint64_t bytes,
                  std::uint32_t fresh) noexcept {
    if (bytes < kBigSyncBytes) {
        return;
    }
    s_bb_syncs.fetch_add(1, std::memory_order_relaxed);
    s_bb_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s_bb_fresh.fetch_add(fresh, std::memory_order_relaxed);
    std::uint64_t cur = s_bb_max.load(std::memory_order_relaxed);
    while (bytes > cur && !s_bb_max.compare_exchange_weak(cur, bytes, std::memory_order_relaxed)) {
    }
    if (!kBigBindLog) {
        return;
    }
    // Once per (pgm_hash, stage, binding). Without a context (a bind outside BindBuffers), once per region.
    const std::uint64_t key =
        t_ctx.valid ? (t_ctx.pgm_hash * 0x9E3779B97F4A7C15ULL) ^ (std::uint64_t{t_ctx.hw_stage} << 40) ^
                          (std::uint64_t{t_ctx.sw_stage} << 48) ^ t_ctx.binding
                    : ~(addr >> kRegionShift);
    std::lock_guard lk{g_log_mutex};
    if (g_logged.size() >= kMaxLoggedKeys || !g_logged.insert(key).second) {
        return;
    }
    if (t_ctx.valid) {
        std::fprintf(stderr,
                     "SOTC BIG BIND: pgm %#llx hw stage %u sw stage %u binding %u, V# base %#llx "
                     "size %#x num_records %u stride %u, request %#llx+%#llx, dirty %.1f MiB, "
                     "fresh regions %u\n",
                     static_cast<unsigned long long>(t_ctx.pgm_hash), t_ctx.hw_stage, t_ctx.sw_stage,
                     t_ctx.binding, static_cast<unsigned long long>(t_ctx.base), t_ctx.vsize,
                     t_ctx.num_records, t_ctx.stride, static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(size), bytes / 1048576.0, fresh);
    } else {
        std::fprintf(stderr,
                     "SOTC BIG BIND: no shader context, request %#llx+%#llx, dirty %.1f MiB, "
                     "fresh regions %u\n",
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                     bytes / 1048576.0, fresh);
    }
}

void Report(std::string& out, double seconds) {
    if (!TrackOn() || seconds <= 0.0) {
        return;
    }
    const std::uint64_t pd_bytes = s_pd_bytes.exchange(0, std::memory_order_relaxed);
    const std::uint64_t pd_regions = s_pd_regions.exchange(0, std::memory_order_relaxed);
    const std::uint64_t pd_calls = s_pd_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t pd_queued = s_pd_queued.exchange(0, std::memory_order_relaxed);
    const std::uint64_t bb_syncs = s_bb_syncs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t bb_bytes = s_bb_bytes.exchange(0, std::memory_order_relaxed);
    const std::uint64_t bb_max = s_bb_max.exchange(0, std::memory_order_relaxed);
    const std::uint64_t bb_fresh = s_bb_fresh.exchange(0, std::memory_order_relaxed);
    char line[512];
    if (kEnabled) {
        std::snprintf(line, sizeof line,
                      "SOTC REGION PREDRAIN: %.1f MiB/s uploaded, %.2f regions/s finished, %.1f "
                      "calls/s, %.2f regions/s queued, %llu pending, queue %zu\n",
                      pd_bytes / 1048576.0 / seconds, pd_regions / seconds, pd_calls / seconds,
                      pd_queued / seconds,
                      static_cast<unsigned long long>(g_pending_count.load(std::memory_order_relaxed)),
                      [] {
                          std::lock_guard lk{g_mutex};
                          return g_queue.size();
                      }());
        out += line;
    }
    if (kBigBindLog || kEnabled) {
        std::snprintf(line, sizeof line,
                      "SOTC BIG BIND SYNCS: %.2f/s of 4 MiB or more, %.1f MiB/s copied by them "
                      "(max %.1f MiB), %.2f fresh regions/s touched\n",
                      bb_syncs / seconds, bb_bytes / 1048576.0 / seconds, bb_max / 1048576.0,
                      bb_fresh / seconds);
        out += line;
    }
}

} // namespace SotcRegionPredrain
