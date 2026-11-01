// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RB_HOST, the GPU_TO_CPU results in host memory. See rb_host.h.

#include "rb_host.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "label_honest.h" // sotc: SotcLabels::Honest, NoteSharedTouch (L1)
#include "video_core/renderer_vulkan/sotc_guest_memory.h" // sotc: LookupGuestView (ring8 imports)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcRbHost {
namespace {

constexpr uint64_t kGranule = 0x10000;   // 64 KiB: the granule of the host runs (ring8 heap runs use it)
constexpr size_t kMaxGranules = 512;     // live stats granules (32 MiB of imports at most)
constexpr size_t kMaxRanges = 4096;      // covered-range checks: at most this many merged runs are kept

struct Granule {
    uint64_t addr;
    uint64_t stamp; // last noted, for LRU
};

std::mutex g_mutex;                     // g_granules, g_version, g_stamp
std::vector<Granule> g_granules;
std::atomic<uint64_t> g_version{1};     // changes with g_granules
uint64_t g_stamp = 0;                   // guarded by g_mutex

// Window counters, reset by Report.
std::atomic<uint64_t> g_stats_noted{0};     // NoteStatsRange calls accepted
std::atomic<uint64_t> g_stats_new{0};       // granules added
std::atomic<uint64_t> g_stats_evicted{0};   // granules dropped (LRU)
std::atomic<uint64_t> g_refused{0};         // calls refused (mode 1 without honest labels)
std::atomic<uint64_t> g_mask_ptrs{0};       // NoteMaskPointer calls
std::atomic<uint64_t> g_mask_served{0};     // ... inside an imported run
std::atomic<uint64_t> g_touches{0};         // mode 1 served writes reported to the labels
std::atomic<uint64_t> g_covered_writes{0};  // served writes that lie in a covered range (all modes)
std::atomic<uint64_t> g_verify_ranges{0};   // mode 2 compares
std::atomic<uint64_t> g_verify_bytes{0};    // mode 2 bytes compared
std::atomic<uint64_t> g_verify_bad{0};      // mode 2 bytes that differ
std::atomic<uint64_t> g_verify_none{0};     // mode 2 readbacks with no snapshot (not covered)
std::atomic<uint64_t> g_mirror_copies{0};   // mode 2 GPU mirror copies recorded
std::atomic<uint64_t> g_mirror_unimported{0};
std::atomic<uint64_t> g_mismatch_logged{0};

// Draw thread caches (covered-range checks read these without the mutex).
struct Cache {
    uint64_t version = 0;
    std::vector<SotcRbClass::Range> runs; // merged stats runs
    uint64_t arena_version = 0;
    std::vector<SotcRbClass::Range> arena; // mask-arena blocks
};
thread_local Cache t_cache;

// Merges granules into sorted runs. Caller holds g_mutex.
std::vector<SotcRbClass::Range> MergedLocked() {
    std::vector<uint64_t> addrs;
    addrs.reserve(g_granules.size());
    for (const Granule& g : g_granules) {
        addrs.push_back(g.addr);
    }
    std::sort(addrs.begin(), addrs.end());
    std::vector<SotcRbClass::Range> out;
    for (const uint64_t a : addrs) {
        if (!out.empty() && a <= out.back().addr + out.back().size) {
            out.back().size = a + kGranule - out.back().addr;
        } else {
            out.push_back({a, kGranule});
        }
    }
    if (out.size() > kMaxRanges) {
        out.resize(kMaxRanges);
    }
    return out;
}

// True when [addr, addr + size) lies inside one merged run (the copy and the imports are per run).
bool Inside(const std::vector<SotcRbClass::Range>& ranges, uint64_t addr, uint64_t size) {
    for (const SotcRbClass::Range& r : ranges) {
        if (addr >= r.addr && addr + size <= r.addr + r.size) {
            return true;
        }
    }
    return false;
}

} // namespace

int Mode() {
    // Read once, as the other sotc switches. 1 and 2 are the modes; anything else is off.
    static const int mode = [] {
        const char* v = std::getenv("SOTC_RB_HOST");
        const int m = v && v[0] == '1' ? 1 : (v && v[0] == '2' ? 2 : 0);
        if (m != 0) {
            std::fprintf(stderr, "SOTC RB HOST: mode %d (%s; needs SOTC_LABELS_HONEST=1 for mode 1)\n", m,
                         m == 1 ? "GPU writes result ranges into host memory" : "verify: readbacks kept, bytes compared");
        }
        return m;
    }();
    return mode;
}

bool On() {
    return Mode() != 0;
}

bool MayServe() {
    if (Mode() == 2) {
        return true;
    }
    if (Mode() != 1) {
        return false;
    }
    if (SotcLabels::Honest()) {
        return true;
    }
    // Mode 1 without honest labels would let the game read GPU results before the GPU wrote them.
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "SOTC RB HOST: mode 1 refused, SotcLabels::Honest() is false "
                             "(SOTC_LABELS_HONEST=1 needed); result ranges keep the readback path\n");
    }
    return false;
}

void NoteStatsRange(uint64_t addr, uint64_t size) {
    if (!On() || !addr || !size) {
        return;
    }
    if (!MayServe()) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t lo = addr & ~(kGranule - 1);
    const uint64_t hi = (addr + size + kGranule - 1) & ~(kGranule - 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    bool changed = false;
    for (uint64_t g = lo; g < hi; g += kGranule) {
        bool found = false;
        for (Granule& e : g_granules) {
            if (e.addr == g) {
                e.stamp = ++g_stamp;
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        if (g_granules.size() >= kMaxGranules) {
            // Least recently noted granule goes; its import leaves with the next publish.
            auto oldest = std::min_element(g_granules.begin(), g_granules.end(),
                                           [](const Granule& a, const Granule& b) { return a.stamp < b.stamp; });
            *oldest = g_granules.back();
            g_granules.pop_back();
            g_stats_evicted.fetch_add(1, std::memory_order_relaxed);
        }
        g_granules.push_back({g, ++g_stamp});
        g_stats_new.fetch_add(1, std::memory_order_relaxed);
        changed = true;
    }
    if (changed) {
        g_version.fetch_add(1, std::memory_order_release);
    }
    g_stats_noted.fetch_add(1, std::memory_order_relaxed);
}

void NoteMaskPointer(uint64_t addr, uint64_t size) {
    if (!On() || !addr || !size) {
        return;
    }
    g_mask_ptrs.fetch_add(1, std::memory_order_relaxed);
    Vulkan::SotcGuestMemory::GuestView view{};
    if (Vulkan::SotcGuestMemory::LookupGuestView(addr, size, &view)) {
        g_mask_served.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<SotcRbClass::Range> Runs() {
    if (!MayServe()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return MergedLocked();
}

bool Covers(uint64_t addr, uint64_t size) {
    if (!On() || !size) {
        return false;
    }
    const uint64_t version = g_version.load(std::memory_order_acquire);
    if (version != t_cache.version) {
        std::lock_guard<std::mutex> lock(g_mutex);
        t_cache.runs = MergedLocked();
        t_cache.version = g_version.load(std::memory_order_relaxed);
    }
    // Mask arena: the registry snapshot, re-read only when it changed.
    const uint64_t arena_version = SotcRbClass::Version();
    if (arena_version != t_cache.arena_version) {
        t_cache.arena = SotcRbClass::Snapshot(t_cache.arena_version);
    }
    return Inside(t_cache.runs, addr, size) || Inside(t_cache.arena, addr, size);
}

void NoteServedWrite(uint64_t addr, uint64_t size) {
    if (!On() || !Covers(addr, size)) {
        return;
    }
    g_covered_writes.fetch_add(1, std::memory_order_relaxed);
    if (Mode() == 1) {
        // The labels after this write wait for the GPU work that wrote it (label_honest.h).
        SotcLabels::NoteSharedTouch();
        g_touches.fetch_add(1, std::memory_order_relaxed);
    }
}

bool SnapshotBefore(uint64_t addr, uint64_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (Mode() != 2 || !size || !Covers(addr, size)) {
        g_verify_none.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(addr));
    if (!p) {
        g_verify_none.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    out.assign(p, p + size);
    return true;
}

void CompareApplied(uint64_t addr, const std::vector<uint8_t>& before, const uint8_t* applied) {
    if (Mode() != 2 || before.empty() || !applied) {
        return;
    }
    uint64_t bad = 0;
    size_t first = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] != applied[i]) {
            if (bad == 0) {
                first = i;
            }
            ++bad;
        }
    }
    g_verify_ranges.fetch_add(1, std::memory_order_relaxed);
    g_verify_bytes.fetch_add(before.size(), std::memory_order_relaxed);
    g_verify_bad.fetch_add(bad, std::memory_order_relaxed);
    if (bad != 0 && g_mismatch_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::fprintf(stderr, "SOTC RB HOST verify: %llu bytes differ in %zu at %llx+%llx (first at +%zx: "
                             "mirror %02x, readback %02x)\n",
                     static_cast<unsigned long long>(bad), before.size(),
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(before.size()),
                     first, before[first], applied[first]);
    }
}

bool RecordMirrorCopy(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr,
                      uint64_t size) {
    if (Mode() != 2 || !size || !Covers(addr, size)) {
        return false;
    }
    Vulkan::SotcGuestMemory::GuestView view{};
    if (!Vulkan::SotcGuestMemory::LookupGuestView(addr, size, &view)) {
        g_mirror_unimported.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The shader's writes are visible to the transfer read of the copy.
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                           {}, barrier, {}, {});
    cmdbuf.copyBuffer(src, view.buffer, vk::BufferCopy{src_offset, view.offset, size});
    g_mirror_copies.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    const double secs = seconds > 0.0 ? seconds : 1.0;
    size_t live = 0;
    uint64_t live_bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        live = g_granules.size();
        live_bytes = live * kGranule;
    }
    const uint64_t noted = g_stats_noted.exchange(0, std::memory_order_relaxed);
    const uint64_t fresh = g_stats_new.exchange(0, std::memory_order_relaxed);
    const uint64_t evicted = g_stats_evicted.exchange(0, std::memory_order_relaxed);
    const uint64_t refused = g_refused.exchange(0, std::memory_order_relaxed);
    const uint64_t mask_ptrs = g_mask_ptrs.exchange(0, std::memory_order_relaxed);
    const uint64_t mask_served = g_mask_served.exchange(0, std::memory_order_relaxed);
    const uint64_t touches = g_touches.exchange(0, std::memory_order_relaxed);
    const uint64_t covered = g_covered_writes.exchange(0, std::memory_order_relaxed);
    const uint64_t vr = g_verify_ranges.exchange(0, std::memory_order_relaxed);
    const uint64_t vb = g_verify_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t vbad = g_verify_bad.exchange(0, std::memory_order_relaxed);
    const uint64_t vnone = g_verify_none.exchange(0, std::memory_order_relaxed);
    const uint64_t mirrors = g_mirror_copies.exchange(0, std::memory_order_relaxed);
    const uint64_t unimported = g_mirror_unimported.exchange(0, std::memory_order_relaxed);
    char line[700];
    std::snprintf(line, sizeof line,
                  "SOTC RB HOST mode %d: stats runs noted %.0f/s (new granules %.0f/s, evicted %.0f/s, live %zu "
                  "= %llu KiB), refused %llu; mask pointers %.0f/s, host-served %.0f%%; served writes covered "
                  "%.0f/s, label touches %.0f/s\n",
                  Mode(), noted / secs, fresh / secs, evicted / secs, live,
                  static_cast<unsigned long long>(live_bytes >> 10), static_cast<unsigned long long>(refused),
                  mask_ptrs / secs, mask_ptrs ? 100.0 * static_cast<double>(mask_served) / static_cast<double>(mask_ptrs) : 0.0,
                  covered / secs, touches / secs);
    out += line;
    char verify[300];
    std::snprintf(verify, sizeof verify,
                  "SOTC RB HOST verify: ranges %llu, bytes %llu, mismatched bytes %llu (must be 0), no snapshot "
                  "%llu; mirror copies %llu, unimported %llu\n",
                  static_cast<unsigned long long>(vr), static_cast<unsigned long long>(vb),
                  static_cast<unsigned long long>(vbad), static_cast<unsigned long long>(vnone),
                  static_cast<unsigned long long>(mirrors), static_cast<unsigned long long>(unimported));
    out += verify;
}

} // namespace SotcRbHost
