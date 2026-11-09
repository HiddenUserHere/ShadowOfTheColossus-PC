// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the PC-native memory model, the API of native.h. See docs/re-stutters/native/PLAN.md.
// Announced uploads are queued here and performed by the draw thread (BufferCache::NativeDrain); the frame
// ticket is a backend supplied by BufferCache.
#include "../preupload.h" // sotc: SOTC_PRELOAD_UPLOAD
#include "native.h"
#include "native_draw.h"
#include "../pc_ring.h" // sotc: 1a, HostServed (ring 8 imported runs)
#include "verify.h" // sotc: coverage oracle (SOTC_VERIFY)
#include "n1verify.h" // sotc: SOTC_VERIFY=n1
#include "video_core/buffer_cache/bpe_heap_guard.h" // sotc: B2, the last in-place writes over the BPE heap metadata

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SotcNative {
namespace {

constexpr uint64_t kMaxUploadChunk = uint64_t{64} << 20; // BufferCache::DrainRange limit
constexpr size_t kMaxPending = 65536;
// The guest address space the page tracker covers (MemoryTracker::MAX_CPU_PAGE_BITS). An announce past it
// is a hook that read a wrong register or field; it would crash the tracker (1 Nov: 0x0053005000520052).
constexpr uint64_t kGuestLimit = uint64_t{1} << 40;

// Counters. Relaxed atomics: they feed the report only.
struct Counters {
    std::atomic<uint64_t> announce_persistent{};
    std::atomic<uint64_t> announce_per_frame{};
    std::atomic<uint64_t> announce_gpu_written{};
    std::atomic<uint64_t> announce_dropped{};
    std::atomic<uint64_t> announce_released{};
    std::atomic<uint64_t> announce_rejected{}; // outside the guest address space (kGuestLimit)
    std::atomic<uint64_t> uploaded_bytes{};
    std::atomic<uint64_t> uploads{};
    std::atomic<uint64_t> uploads_clean{};
    std::atomic<uint64_t> marked_bytes{};
    std::atomic<uint64_t> exact_uploads{};
    std::atomic<uint64_t> exact_bytes{};
    std::atomic<uint64_t> images_reused{};
    std::atomic<uint64_t> cp_writes{}, cp_write_bytes{}; // command processor writes told to the caches
    std::atomic<uint64_t> frame_waits{};
    std::atomic<uint64_t> frame_wait_ns{};
    std::atomic<uint64_t> frame_wait_max_ns{};
    std::atomic<uint64_t> reuse_fences{};
    std::atomic<uint64_t> verify_unannounced{};
    std::atomic<uint64_t> verify_gpu_read{};
    std::atomic<uint64_t> verify_gpu_wait{};
    std::atomic<uint64_t> readbacks_skipped{};  // mode 1: a GPU-to-guest copy the port did not make
    std::atomic<uint64_t> stray_write_faults{}; // mode 1: a write fault no tracker protection caused
    std::atomic<uint64_t> stray_read_faults{};  // mode 1: a read fault no tracker protection caused
    // Timing audit (JITTER-BRIEF): what each drain did relative to the submit it ran for.
    std::atomic<uint64_t> drains{};             // NativeDrain calls
    std::atomic<uint64_t> drain_entries{};      // announces drained
    std::atomic<uint64_t> drain_bytes{};        // bytes of those announces
    std::atomic<uint64_t> drain_after_pickup{}; // ... announced after the CP picked the submit up
    std::atomic<uint64_t> drain_after_bytes{};
    std::atomic<uint64_t> drain_lag_max{};      // max (seq at drain - seq at pickup), in announces
    std::atomic<uint64_t> warm_deferred{};      // drains that postponed Persistent uploads (marked at once)
    std::atomic<uint64_t> warm_backlog_max{};   // bytes waiting for a later drain, max
    std::atomic<uint64_t> late_writes{}, late_bytes{};    // announced before the writer finished, same frame
    std::atomic<uint64_t> late_next{}, late_next_bytes{}; // ... changed after a SubmitDone (also a legal rewrite)
    // sotc: 1a. Announce fast path and the cost of the drain.
    std::atomic<uint64_t> announce_served{};       // announces inside a run ring 8 serves in place (queued, served)
    std::atomic<uint64_t> announce_merged{};       // announce pieces merged into a queued entry
    std::atomic<uint64_t> drain_ns{};              // NativeDrain wall time
    std::atomic<uint64_t> drain_pages{};           // 4 KiB pages given a CPU mark by the drain
    std::atomic<uint64_t> drain_invalidations{};   // TextureCache::InvalidateMemory calls
    std::atomic<uint64_t> drain_served{};          // queued entries served in place (no mark, no upload)
    std::atomic<uint64_t> drain_ranges{};          // merged ranges the drain processed
    // sotc: SOTC_NATIVE_OWNER_DISCARD (DiscardOwner, BufferCache::NativeDiscardOwner).
    std::atomic<uint64_t> owner_calls{}, owner_discarded{}, owner_bytes{};
    std::atomic<uint64_t> owner_pages_unprotected{}, owner_shared_pages{};
    std::atomic<uint64_t> owner_skipped_clean{}, owner_skipped_cmd{};
    // sotc: SOTC_NATIVE_DRAIN_AHEAD. The worker's batches and entries, the entries the draw thread found taken ahead of
    // its submit (and their bytes), and the draw thread's waits for the worker.
    std::atomic<uint64_t> ahead_batches{}, ahead_taken{}, ahead_entries{}, ahead_bytes{};
    std::atomic<uint64_t> ahead_waits{}, ahead_wait_ns{}, ahead_wait_max_ns{};
};
Counters g_c;
std::atomic<uint64_t> g_submitted_local{}; // frame counter when no backend is set
std::atomic<size_t> g_pending_count{};     // size of g_pending (lock-free look)
// sotc: SOTC_LEAN_SUBMIT=2 (verify). Bumped under g_mutex by every change that is not a draw-thread pop:
// Announce (push or merge) and Release. A verify snapshot is compared only when the version did not move.
uint64_t g_queue_version = 0;
// sotc: SOTC_LEAN_SUBMIT=2 verify counters (printed by Report when the verify is on). compared: drains where the
// snapshot was checked; raced: the queue changed (an announce or a release) between the snapshot and the end of
// the drain, so it cannot be compared; mismatches: the snapshot differs from the entries the per-entry loop popped.
std::atomic<uint64_t> g_take_compared{};
std::atomic<uint64_t> g_take_raced{};
std::atomic<uint64_t> g_take_mismatch{};
// sotc: SOTC_LEAN_SUBMIT=2 (read once). The verify runs in NativeDrain; the report line is printed only then.
const bool g_take_verify = [] {
    const char* v = std::getenv("SOTC_LEAN_SUBMIT");
    return v && v[0] == '2';
}();
std::atomic<uint64_t> g_announce_seq{};    // Announce calls (stamps PendingUpload::seq)

std::mutex g_mutex; // the pending queue and the descriptor map
std::deque<PendingUpload> g_pending;
// sotc: announces that arrive while g_pending is full (a load announces ~1M ranges before the draw thread
// drains once). They used to be dropped, which left every GPU copy and image over them stale for good in mode 1
// (no page fault catches the write later). Kept as merged exact intervals per kind instead: start -> end.
std::map<uint64_t, uint64_t> g_overflow[2];
size_t PendingTotal() {
    return g_pending.size() + g_overflow[0].size() + g_overflow[1].size();
}
void OverflowAdd(Kind kind, uint64_t addr, uint64_t end) {
    auto& m = g_overflow[kind == Kind::Persistent ? 0 : 1];
    auto it = m.upper_bound(addr);
    if (it != m.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= addr) {
            addr = prev->first;
            end = std::max(end, prev->second);
            it = m.erase(prev);
        }
    }
    while (it != m.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = m.erase(it);
    }
    m.emplace(addr, end);
}

struct DescRecord {
    uint64_t base;
    uint64_t size;
    uint32_t format;
};
std::unordered_map<uint64_t, DescRecord> g_desc; // guarded by g_mutex
uint64_t g_desc_replaced{};                      // guarded by g_mutex
uint64_t g_desc_noted{};                         // guarded by g_mutex

FrameBackend g_backend{};                        // written under g_mutex, read after g_backend_set
std::atomic<bool> g_backend_set{false};

bool Overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}

// sotc: SOTC_NATIVE_DRAIN_AHEAD. Drops [addr, +size) from the announces the worker already took (Release calls it).
void DrainAheadDrop(uint64_t addr, uint64_t size);

// SOTC_NATIVE: 0 off (default), 1 on, 2 on plus verify. Anything else reads as 0.
int ReadMode() {
    const char* v = std::getenv("SOTC_NATIVE");
    if (!v || !v[0]) {
        return 0;
    }
    if (v[0] == '1') {
        return 1;
    }
    if (v[0] == '2') {
        return 2;
    }
    return 0;
}

const int g_mode = ReadMode();

const bool g_verify = [] {
    const char* v = std::getenv("SOTC_VERIFY");
    return g_mode != 0 && ((v && v[0] == '1') || g_mode == 2);
}();

// Verify counts every event; the first lines of each kind are logged.
void VerifyCount(const char* what, uint64_t addr, uint64_t size, uint64_t rip,
                 std::atomic<uint64_t>& count) {
    const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        std::fprintf(stderr, "SOTC NATIVE verify %s: addr %#llx size %llu rip %#llx\n", what,
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(rip));
    }
}

} // namespace

int Mode() {
    return g_mode;
}

bool Verify() {
    return g_verify;
}

void Announce(uint64_t addr, uint64_t size, Kind kind) {
    if (g_mode == 0 || size == 0) {
        return;
    }
    if (addr == 0 || addr >= kGuestLimit || size > kGuestLimit - addr) {
        // The host caller is the native hook that produced it; the first few are logged.
        if (g_c.announce_rejected.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::fprintf(stderr, "SOTC NATIVE announce rejected: addr 0x%llx size 0x%llx kind %d caller %p\n",
                         static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                         static_cast<int>(kind), __builtin_return_address(0));
        }
        return;
    }
    SotcVerify::NoteAnnounce(addr, size); // sotc: clears the coverage oracle's pending faults
    SotcN1Verify::NoteAnnounce(addr, size, static_cast<uint8_t>(kind), __builtin_return_address(0)); // sotc: SOTC_VERIFY=n1
    if (kind == Kind::GpuWritten) {
        g_announce_seq.fetch_add(1, std::memory_order_relaxed); // sotc: nothing queued; the count stays every Announce
        g_c.announce_gpu_written.fetch_add(1, std::memory_order_relaxed);
        return; // the GPU writes it: nothing to upload
    }
    if (kind == Kind::Persistent) {
        g_c.announce_persistent.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_c.announce_per_frame.fetch_add(1, std::memory_order_relaxed);
    }
    // PerFrame is queued like Persistent; the draw thread marks it CPU-modified instead of uploading it.
    // sotc: 1a. A range ring 8 serves in place is queued as served, not dropped: the drain then skips its page
    // mark and upload (the binds read the guest bytes), but still invalidates the images over it (ringed pages
    // never fault, so the texture cache would not hear of a write there).
    const bool served = SotcPcRing::HostServed(addr, size);
    std::scoped_lock lock{g_mutex};
    // sotc: SOTC_NATIVE_DRAIN_AHEAD: the seq is taken under g_mutex, with the push. Every queued announce with a seq up to
    // AnnounceSeq() is then in the queue or already popped, which the worker's watermark (TakeAllUploads) relies on.
    const uint64_t seq = g_announce_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t merged = 0;
    for (uint64_t at = addr; at < addr + size;) {
        const uint64_t piece = std::min<uint64_t>(kMaxUploadChunk, addr + size - at);
        // sotc: a piece that touches the newest queued range of its kind extends it, on either side. The union
        // holds announced bytes only (adjacent or overlapping), so the drain copies exactly what was written.
        // Sequential small writes (memcpy runs) fill one entry instead of overflowing the queue. Served and
        // not-served ranges are never merged (the drain treats them differently). One entry stays <= 64 MiB.
        if (!g_pending.empty()) {
            PendingUpload& back = g_pending.back();
            if (back.kind == kind && back.served == served) {
                const uint64_t lo = std::min<uint64_t>(back.addr, at);
                const uint64_t hi = std::max<uint64_t>(back.addr + back.size, at + piece);
                if (at <= back.addr + back.size && back.addr <= at + piece && hi - lo <= kMaxUploadChunk) {
                    back.addr = lo;
                    back.size = hi - lo;
                    back.seq = seq;
                    ++merged;
                    at += piece;
                    continue;
                }
            }
        }
        if (g_pending.size() >= kMaxPending) {
            // sotc: kept in the overflow (see g_overflow), drained after the queue. Dropping them left the GPU
            // copies over those bytes stale for good (broken particles in mode 1: 2.3M ranges dropped per route).
            OverflowAdd(kind, at, at + piece);
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "SOTC NATIVE upload queue full (%zu): announced ranges kept in the overflow\n",
                             kMaxPending);
            }
        } else {
            g_pending.push_back(PendingUpload{at, piece, kind, seq, served});
        }
        at += piece;
    }
    if (served) {
        g_c.announce_served.fetch_add(1, std::memory_order_relaxed);
    }
    if (merged) {
        g_c.announce_merged.fetch_add(merged, std::memory_order_relaxed);
    }
    ++g_queue_version;
    g_pending_count.store(PendingTotal(), std::memory_order_release);
}

void Release(uint64_t addr, uint64_t size) {
    if (g_mode == 0 || size == 0) {
        return;
    }
    SotcVerify::NoteAnnounce(addr, size); // sotc: a released range is not an unannounced write
    SotcN1Verify::NoteRelease(addr, size); // sotc: SOTC_VERIFY=n1: its announce records go
    SotcPreupload::Release(addr, size);    // sotc: SOTC_PRELOAD_UPLOAD: a freed range is not uploaded
    std::scoped_lock lock{g_mutex};
    // Drops the queued parts inside [addr, addr + size); the parts outside it stay queued.
    std::deque<PendingUpload> kept;
    for (const PendingUpload& p : g_pending) {
        if (!Overlaps(p.addr, p.size, addr, size)) {
            kept.push_back(p);
            continue;
        }
        g_c.announce_released.fetch_add(1, std::memory_order_relaxed);
        const uint64_t end = p.addr + p.size;
        if (p.addr < addr) {
            kept.push_back(PendingUpload{p.addr, addr - p.addr, p.kind, p.seq, p.served});
        }
        if (end > addr + size) {
            kept.push_back(PendingUpload{addr + size, end - (addr + size), p.kind, p.seq, p.served});
        }
    }
    g_pending.swap(kept);
    ++g_queue_version; // sotc: SOTC_LEAN_SUBMIT=2 verify (see g_queue_version)
    for (auto& m : g_overflow) {
        std::vector<std::pair<uint64_t, uint64_t>> parts;
        for (auto it = m.begin(); it != m.end();) {
            if (!Overlaps(it->first, it->second - it->first, addr, size)) {
                ++it;
                continue;
            }
            if (it->first < addr) {
                parts.emplace_back(it->first, addr);
            }
            if (it->second > addr + size) {
                parts.emplace_back(addr + size, it->second);
            }
            it = m.erase(it);
        }
        for (const auto& part : parts) {
            m.emplace(part.first, part.second);
        }
    }
    g_pending_count.store(PendingTotal(), std::memory_order_release);
    DrainAheadDrop(addr, size); // sotc: SOTC_NATIVE_DRAIN_AHEAD: the batches the worker took are released too
}

uint64_t FrameSubmitted() {
    if (g_backend_set.load(std::memory_order_acquire)) {
        return g_backend.submitted(g_backend.ctx);
    }
    return g_submitted_local.load(std::memory_order_relaxed);
}

void FrameWait(uint64_t frame) {
    if (g_mode == 0 || !g_backend_set.load(std::memory_order_acquire)) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    g_backend.wait(g_backend.ctx, frame); // the backend waits for the frame's GPU ticket only
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    g_c.frame_waits.fetch_add(1, std::memory_order_relaxed);
    g_c.frame_wait_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = g_c.frame_wait_max_ns.load(std::memory_order_relaxed);
    while (ns > prev && !g_c.frame_wait_max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

uint64_t FrameCompleted() {
    if (!g_backend_set.load(std::memory_order_acquire)) {
        return 0;
    }
    return g_backend.completed(g_backend.ctx);
}

void NoteDescriptor(uint64_t desc_addr, uint64_t base, uint64_t size, uint32_t format) {
    if (g_mode == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ++g_desc_noted;
    auto [it, inserted] = g_desc.try_emplace(desc_addr, DescRecord{base, size, format});
    if (!inserted) {
        if (it->second.base != base || it->second.size != size || it->second.format != format) {
            ++g_desc_replaced;
        }
        it->second = DescRecord{base, size, format};
    }
}

uint64_t AnnounceSeq() {
    return g_announce_seq.load(std::memory_order_relaxed);
}

void NoteLateWrite(uint64_t size, bool same_frame) {
    if (same_frame) {
        g_c.late_writes.fetch_add(1, std::memory_order_relaxed);
        g_c.late_bytes.fetch_add(size, std::memory_order_relaxed);
    } else {
        g_c.late_next.fetch_add(1, std::memory_order_relaxed);
        g_c.late_next_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

void NoteImageReused() {
    g_c.images_reused.fetch_add(1, std::memory_order_relaxed);
}

void NoteCpGuestWrite(uint64_t size) {
    g_c.cp_writes.fetch_add(1, std::memory_order_relaxed);
    g_c.cp_write_bytes.fetch_add(size, std::memory_order_relaxed);
}

void NoteSkippedReadback(uint64_t addr, uint64_t size) {
    if (g_c.readbacks_skipped.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::fprintf(stderr, "SOTC NATIVE readback skipped: addr 0x%llx size 0x%llx\n",
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
    }
}

void NoteStrayFault(uint64_t addr, bool is_write, uint64_t rip) {
    VerifyCount(is_write ? "stray write fault (no tracker protection)"
                         : "stray read fault (no tracker protection)",
                addr, 0, rip, is_write ? g_c.stray_write_faults : g_c.stray_read_faults);
    // sotc (B2): the in-place GPU writes that reached the BPE heap metadata just before this fault
    VideoCore::BpeNative::ReportNearFault(addr);
}

void VerifyUnannouncedWrite(uint64_t addr, uint64_t size, uint64_t rip) {
    if (g_verify) {
        VerifyCount("unannounced write", addr, size, rip, g_c.verify_unannounced);
        SotcVerify::AggregateUnannounced(addr, size, rip); // sotc: per-RIP table (verify.cpp)
    }
}

void VerifyGpuRead(uint64_t addr, uint64_t size, uint64_t rip) {
    if (g_verify) {
        VerifyCount("gpu-written read", addr, size, rip, g_c.verify_gpu_read);
    }
}

void VerifyGpuWait(uint64_t label_addr, uint64_t rip) {
    if (g_verify) {
        VerifyCount("label wait", label_addr, 0, rip, g_c.verify_gpu_wait);
    }
}

void Report(std::string& out, double seconds) {
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const uint64_t ann_p = take(g_c.announce_persistent);
    const uint64_t ann_f = take(g_c.announce_per_frame);
    const uint64_t ann_g = take(g_c.announce_gpu_written);
    const uint64_t dropped = take(g_c.announce_dropped);
    const uint64_t released = take(g_c.announce_released);
    const uint64_t rejected = g_c.announce_rejected.load(std::memory_order_relaxed); // session total
    const uint64_t ups = take(g_c.uploads);
    const uint64_t clean = take(g_c.uploads_clean);
    const uint64_t up_bytes = take(g_c.uploaded_bytes);
    const uint64_t marked = take(g_c.marked_bytes);
    const uint64_t exact = take(g_c.exact_uploads);
    const uint64_t exact_bytes = take(g_c.exact_bytes);
    const uint64_t waits = take(g_c.frame_waits);
    const uint64_t wait_ns = take(g_c.frame_wait_ns);
    const uint64_t wait_max = take(g_c.frame_wait_max_ns);
    const uint64_t fences = take(g_c.reuse_fences);
    size_t pending = 0;
    size_t desc = 0;
    uint64_t desc_noted = 0;
    uint64_t desc_replaced = 0;
    {
        std::scoped_lock lock{g_mutex};
        pending = g_pending.size();
        desc = g_desc.size();
        desc_noted = g_desc_noted;
        desc_replaced = g_desc_replaced;
    }
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "SOTC NATIVE mode %d (%.1f s): announce persistent %llu per-frame %llu gpu-written %llu "
                  "dropped %llu released %llu rejected %llu (session)\n",
                  g_mode, seconds, static_cast<unsigned long long>(ann_p), static_cast<unsigned long long>(ann_f),
                  static_cast<unsigned long long>(ann_g), static_cast<unsigned long long>(dropped),
                  static_cast<unsigned long long>(released), static_cast<unsigned long long>(rejected));
    out += line;
    std::snprintf(line, sizeof(line),
                  "SOTC NATIVE uploads %llu (clean %llu) %llu KiB, marked %llu KiB, queued %zu; descriptors "
                  "%zu (noted %llu, replaced %llu); frame waits %llu avg %.2f ms max %.2f ms; reuse fences %llu\n",
                  static_cast<unsigned long long>(ups), static_cast<unsigned long long>(clean),
                  static_cast<unsigned long long>(up_bytes >> 10), static_cast<unsigned long long>(marked >> 10),
                  pending, desc, static_cast<unsigned long long>(desc_noted),
                  static_cast<unsigned long long>(desc_replaced), static_cast<unsigned long long>(waits),
                  waits ? wait_ns / 1e6 / static_cast<double>(waits) : 0.0, wait_max / 1e6,
                  static_cast<unsigned long long>(fences));
    out += line;
    {
        // Timing audit: drained entries newer than the submit they were drained for (CPU work after the pickup is
        // visible to that submit's binds), Persistent uploads the budget postponed, writes found after their
        // announce (re-announced by native_hooks.cpp).
        const auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        const uint64_t drains_n = take(g_c.drains);
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE TIMING drains %llu entries %llu %llu KiB, after-pickup %llu %llu KiB (lag max "
                      "%llu), warm deferred %llu (backlog max %llu KiB), late writes same-frame %llu %llu B "
                      "next-frame %llu %llu B\n",
                      ull(drains_n), ull(take(g_c.drain_entries)), ull(take(g_c.drain_bytes) >> 10),
                      ull(take(g_c.drain_after_pickup)), ull(take(g_c.drain_after_bytes) >> 10),
                      ull(take(g_c.drain_lag_max)), ull(take(g_c.warm_deferred)),
                      ull(take(g_c.warm_backlog_max) >> 10), ull(take(g_c.late_writes)),
                      ull(take(g_c.late_bytes)), ull(take(g_c.late_next)), ull(take(g_c.late_next_bytes)));
        out += line;
        // sotc: 1a. What NativeDrain costs on the draw thread, per frame (SubmitDone count in the window) and
        // what the announce fast path did (served in place, merged into a queued entry).
        const uint64_t drain_ns = take(g_c.drain_ns);
        const uint64_t frames_now = FrameSubmitted();
        static uint64_t frames_prev = 0;
        const uint64_t frames = frames_now >= frames_prev ? frames_now - frames_prev : 0;
        frames_prev = frames_now;
        const double ms = drain_ns / 1e6;
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE TIMING drain %.2f ms (%.3f ms per frame over %llu frames, %.1f drains per frame), "
                      "ranges %llu, pages marked %llu, texture invalidations %llu, served entries %llu; announces "
                      "served %llu, merged %llu\n",
                      ms, frames ? ms / static_cast<double>(frames) : 0.0, ull(frames),
                      frames ? static_cast<double>(drains_n) / static_cast<double>(frames) : 0.0,
                      ull(take(g_c.drain_ranges)), ull(take(g_c.drain_pages)), ull(take(g_c.drain_invalidations)),
                      ull(take(g_c.drain_served)), ull(take(g_c.announce_served)), ull(take(g_c.announce_merged)));
        out += line;
        if (g_take_verify) { // sotc: SOTC_LEAN_SUBMIT=2
            std::snprintf(line, sizeof(line), "SOTC NATIVE TAKE VERIFY: compared %llu, raced %llu, mismatches %llu\n",
                          ull(g_take_compared.exchange(0, std::memory_order_relaxed)),
                          ull(g_take_raced.exchange(0, std::memory_order_relaxed)),
                          ull(g_take_mismatch.exchange(0, std::memory_order_relaxed)));
            out += line;
        }
    }
    if (DrainAheadOn()) { // sotc: SOTC_NATIVE_DRAIN_AHEAD counters, reset per window
        const auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        const uint64_t waits = take(g_c.ahead_waits);
        const uint64_t wait_ns = take(g_c.ahead_wait_ns);
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE DRAIN AHEAD (%.1f s): worker batches %llu entries %llu; drained ahead %llu entries "
                      "%llu KiB; submit-start waits %llu avg %.3f ms max %.3f ms\n",
                      seconds, ull(take(g_c.ahead_batches)), ull(take(g_c.ahead_taken)),
                      ull(take(g_c.ahead_entries)), ull(take(g_c.ahead_bytes) >> 10), ull(waits),
                      waits ? wait_ns / 1e6 / static_cast<double>(waits) : 0.0, take(g_c.ahead_wait_max_ns) / 1e6);
        out += line;
    }
    if (OwnerDiscardOn()) { // sotc: SOTC_NATIVE_OWNER_DISCARD counters, reset per window
        const auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE OWNER DISCARD (%.1f s): calls %llu discarded %llu (%llu KiB), pages unprotected "
                      "%llu, shared boundary pages %llu, skipped clean %llu, skipped in command buffers %llu\n",
                      seconds, ull(take(g_c.owner_calls)), ull(take(g_c.owner_discarded)),
                      ull(take(g_c.owner_bytes) >> 10), ull(take(g_c.owner_pages_unprotected)),
                      ull(take(g_c.owner_shared_pages)), ull(take(g_c.owner_skipped_clean)),
                      ull(take(g_c.owner_skipped_cmd)));
        out += line;
    }
    if (g_mode == 1) {
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE tracking off (%.1f s): readbacks skipped %llu, stray faults write %llu "
                      "read %llu, exact uploads %llu %llu KiB, images reused %llu, command processor writes %llu "
                      "%llu KiB\n",
                      seconds, static_cast<unsigned long long>(take(g_c.readbacks_skipped)),
                      static_cast<unsigned long long>(take(g_c.stray_write_faults)),
                      static_cast<unsigned long long>(take(g_c.stray_read_faults)),
                      static_cast<unsigned long long>(exact), static_cast<unsigned long long>(exact_bytes >> 10),
                      static_cast<unsigned long long>(take(g_c.images_reused)),
                      static_cast<unsigned long long>(take(g_c.cp_writes)),
                      static_cast<unsigned long long>(take(g_c.cp_write_bytes) >> 10));
        out += line;
    }
    if (g_verify) {
        std::snprintf(line, sizeof(line),
                      "SOTC NATIVE verify: unannounced writes %llu, gpu-written reads %llu, label waits %llu\n",
                      static_cast<unsigned long long>(g_c.verify_unannounced.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_c.verify_gpu_read.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_c.verify_gpu_wait.load(std::memory_order_relaxed)));
        out += line;
    }
}

// ---- Draw-thread side (native_draw.h) ----

// sotc: the pop rule of TakeUpload, shared with TakeUploads and PeekUploads. The queue front first; the
// overflow (after the queue, one piece per pop) only when the queue is empty. Caller holds g_mutex. The
// containers are templates so PeekUploads can run the same rule on copies.
template <class Queue, class Overflow>
bool PopUpload(Queue& pending, Overflow& overflow, PendingUpload& out) {
    if (!pending.empty()) {
        out = pending.front();
        pending.pop_front();
        return true;
    }
    for (int k = 0; k < 2; ++k) { // sotc: the overflow after the queue (see g_overflow)
        auto& m = overflow[k];
        if (m.empty()) {
            continue;
        }
        auto it = m.begin();
        const uint64_t piece = std::min<uint64_t>(kMaxUploadChunk, it->second - it->first);
        out = PendingUpload{it->first, piece, k == 0 ? Kind::Persistent : Kind::PerFrame};
        if (piece == it->second - it->first) {
            m.erase(it);
        } else {
            const uint64_t rest_end = it->second;
            const uint64_t rest = it->first + piece;
            m.erase(it);
            m.emplace(rest, rest_end);
        }
        return true;
    }
    return false;
}

bool TakeUpload(PendingUpload& out) {
    if (g_pending_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (!PopUpload(g_pending, g_overflow, out)) {
        return false;
    }
    g_pending_count.store(PendingTotal(), std::memory_order_release);
    return true;
}

// sotc: SOTC_LEAN_SUBMIT=1. The same pops as repeated TakeUpload calls, under one lock.
size_t TakeUploads(std::vector<PendingUpload>& out, size_t max) {
    if (g_pending_count.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    std::scoped_lock lock{g_mutex};
    size_t n = 0;
    PendingUpload up{};
    while (n < max && PopUpload(g_pending, g_overflow, up)) {
        out.push_back(up);
        ++n;
    }
    if (n != 0) {
        g_pending_count.store(PendingTotal(), std::memory_order_release);
    }
    return n;
}

// sotc: SOTC_LEAN_SUBMIT=2 (verify). What TakeUploads would pop from the queue now, on copies: the queue is
// not changed. Returns the queue version of that state (see g_queue_version).
uint64_t PeekUploads(std::vector<PendingUpload>& out) {
    std::scoped_lock lock{g_mutex};
    std::deque<PendingUpload> pending(g_pending);
    std::map<uint64_t, uint64_t> overflow[2] = {g_overflow[0], g_overflow[1]};
    PendingUpload up{};
    while (PopUpload(pending, overflow, up)) {
        out.push_back(up);
    }
    return g_queue_version;
}

void VerifyTakeUploads(const std::vector<PendingUpload>& snapshot, const std::vector<PendingUpload>& popped,
                       uint64_t version) {
    bool same_version = false;
    {
        std::scoped_lock lock{g_mutex};
        same_version = g_queue_version == version;
    }
    if (!same_version) {
        g_take_raced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_take_compared.fetch_add(1, std::memory_order_relaxed);
    bool differs = snapshot.size() != popped.size();
    for (size_t i = 0; !differs && i < snapshot.size(); ++i) {
        const PendingUpload& a = snapshot[i];
        const PendingUpload& b = popped[i];
        differs = a.addr != b.addr || a.size != b.size || a.kind != b.kind || a.seq != b.seq || a.served != b.served;
    }
    if (differs) {
        g_take_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

// sotc: SOTC_NATIVE_DRAIN_AHEAD. Sorts the non-served announces of a batch by kind, then address, and merges the
// overlapping or touching ranges of one kind (at most kMaxUploadChunk each). The draw thread's NativeDrain used to do
// this inline; the drain-ahead worker does it per batch, the draw thread once more over the batches' ranges.
void MergeUploads(std::vector<PendingUpload>& taken, std::vector<PendingUpload>& merged) {
    std::sort(taken.begin(), taken.end(), [](const PendingUpload& a, const PendingUpload& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.addr < b.addr;
    });
    merged.clear();
    for (const auto& e : taken) {
        if (!merged.empty()) {
            PendingUpload& r = merged.back();
            const uint64_t lo = std::min<uint64_t>(r.addr, e.addr);
            const uint64_t hi = std::max<uint64_t>(r.addr + r.size, e.addr + e.size);
            if (r.kind == e.kind && e.addr <= r.addr + r.size && r.addr <= e.addr + e.size &&
                hi - lo <= kMaxUploadChunk) {
                r.addr = lo;
                r.size = hi - lo;
                continue;
            }
        }
        merged.push_back(e);
    }
}

namespace {

// sotc: SOTC_NATIVE_DRAIN_AHEAD (read once; needs SOTC_NATIVE on). The announces are taken by a worker thread as they
// are queued, so the draw thread's submit start finds them taken. Threads and locks:
// - Worker: TakeAll (g_mutex, the same pops as TakeUploads), then Publish (ready_mutex). It holds no lock of the
//   buffer cache, the texture cache, the memory tracker or the scheduler, and calls no Vulkan or tracker function.
// - Draw thread (Take, at NativeDrain): ready_mutex (the wait and the swap). Never g_mutex while holding it.
// - Release (any thread, under g_mutex) takes ready_mutex to drop the published ranges it covers. Order: g_mutex
//   before ready_mutex; the worker never holds both.
// The draw thread gets every announce with a seq up to its pickup seq: the worker publishes a seq only after it took the
// queue under g_mutex, and Announce takes its seq under g_mutex with the push (so no queued announce is in flight then).
struct DrainAheadState {
    std::mutex ready_mutex;                  // ready_raw, ready_ranges, the wait of Take
    std::condition_variable ready_cv;        // the draw thread waits here for ready_seq
    std::vector<PendingUpload> ready_raw;    // published, announce order (served entries included), not yet taken
    std::vector<PendingUpload> ready_ranges; // published: the non-served ranges of each batch, sorted and merged
    std::vector<InvalRef> ready_inval;       // sotc: published: every ready_raw entry, sorted by (addr, index)
    std::vector<InvalGroup> ready_groups;    // sotc: published: the invalidation groups over ready_inval
    std::atomic<uint64_t> ready_seq{0};      // every announce with a seq up to this is published or taken
    std::mutex wake_mutex;                   // wake_requested, stop_requested
    std::condition_variable wake_cv;
    bool wake_requested = false;
    bool stop_requested = false;
    std::atomic<bool> running{false};
    std::thread* worker = nullptr; // set by Start, joined and cleared by Stop (the BufferCache constructor/destructor)
};
// Heap-allocated and never freed: a worker still running at process exit must not touch destroyed statics.
DrainAheadState& DaState() {
    static DrainAheadState* state = new DrainAheadState;
    return *state;
}

// The worker waits this long between takes when no drain requests one.
constexpr auto kDrainAheadPoll = std::chrono::microseconds(100);

const bool g_drain_ahead_on = [] {
    const char* v = std::getenv("SOTC_NATIVE_DRAIN_AHEAD");
    return v && v[0] == '1' && Mode() != 0;
}();

// Pops every queued upload (the pops of TakeUploads, overflow included) and returns the announce seq current under the
// same lock. An announce with a seq up to it is queued (and popped here) or was popped before.
uint64_t TakeAll(std::vector<PendingUpload>& out) {
    std::scoped_lock lock{g_mutex};
    const uint64_t seq = g_announce_seq.load(std::memory_order_relaxed);
    PendingUpload up{};
    while (PopUpload(g_pending, g_overflow, up)) {
        out.push_back(up);
    }
    g_pending_count.store(PendingTotal(), std::memory_order_release);
    return seq;
}

// One batch: raw (announce order) and taken (the non-served ones, for MergeUploads). Returns the seq it covers.
uint64_t TakeBatch(std::vector<PendingUpload>& raw, std::vector<PendingUpload>& taken,
                   std::vector<PendingUpload>& ranges) {
    raw.clear();
    ranges.clear();
    const uint64_t seq = TakeAll(raw);
    if (!raw.empty()) {
        taken.clear();
        for (const PendingUpload& u : raw) {
            if (!u.served) {
                taken.push_back(u);
            }
        }
        MergeUploads(taken, ranges);
    }
    return seq;
}

// sotc: SOTC_NATIVE_INVAL_MERGE groups, built here so the draw thread does no sort. ready_inval holds every published
// announce (served ones too) sorted by address; GroupInval makes the groups NativeDrain used to make inline: greedy in
// address order, a group extends while the next announce starts at or before its end and stays under 1 MiB.
bool InvalLess(const InvalRef& a, const InvalRef& b) {
    return a.addr != b.addr ? a.addr < b.addr : a.index < b.index;
}

void GroupInval(DrainAheadState& s) {
    constexpr uint64_t kMaxGroup = uint64_t{1} << 20; // HasImagesExact answers true past 256 pages
    const std::vector<InvalRef>& sorted = s.ready_inval;
    s.ready_groups.clear();
    for (size_t g = 0; g < sorted.size();) {
        const uint64_t lo = sorted[g].addr;
        uint64_t hi = lo + sorted[g].size;
        size_t end = g + 1;
        while (end < sorted.size() && sorted[end].addr <= hi) {
            const uint64_t next_hi = std::max<uint64_t>(hi, sorted[end].addr + sorted[end].size);
            if (next_hi - lo > kMaxGroup) {
                break;
            }
            hi = next_hi;
            ++end;
        }
        s.ready_groups.push_back(InvalGroup{lo, hi, static_cast<uint32_t>(g), static_cast<uint32_t>(end)});
        g = end;
    }
}

// Sorts every ready_raw entry again and groups them: after a drop split or removed entries, the indices moved.
void RebuildInval(DrainAheadState& s) {
    s.ready_inval.clear();
    for (size_t i = 0; i < s.ready_raw.size(); ++i) {
        s.ready_inval.push_back(InvalRef{s.ready_raw[i].addr, s.ready_raw[i].size, static_cast<uint32_t>(i)});
    }
    std::sort(s.ready_inval.begin(), s.ready_inval.end(), InvalLess);
    GroupInval(s);
}

// Adds the entries appended to ready_raw from index first on: sorts them, merges them into ready_inval, groups.
void AddInval(DrainAheadState& s, size_t first) {
    static thread_local std::vector<InvalRef> batch;
    static thread_local std::vector<InvalRef> merged;
    batch.clear();
    for (size_t i = first; i < s.ready_raw.size(); ++i) {
        batch.push_back(InvalRef{s.ready_raw[i].addr, s.ready_raw[i].size, static_cast<uint32_t>(i)});
    }
    std::sort(batch.begin(), batch.end(), InvalLess);
    merged.resize(s.ready_inval.size() + batch.size());
    std::merge(s.ready_inval.begin(), s.ready_inval.end(), batch.begin(), batch.end(), merged.begin(), InvalLess);
    s.ready_inval.swap(merged);
    GroupInval(s);
}

void Publish(DrainAheadState& s, const std::vector<PendingUpload>& raw, const std::vector<PendingUpload>& ranges,
             uint64_t seq) {
    {
        std::scoped_lock lock{s.ready_mutex};
        const size_t first = s.ready_raw.size();
        s.ready_raw.insert(s.ready_raw.end(), raw.begin(), raw.end());
        if (!raw.empty()) {
            AddInval(s, first); // sotc: the groups of this batch, merged with the ones already published
        }
        // sotc: kept sorted and merged across batches, so the draw thread uses them as they are (no sort there).
        if (s.ready_ranges.empty()) {
            s.ready_ranges.assign(ranges.begin(), ranges.end());
        } else if (!ranges.empty()) {
            static thread_local std::vector<PendingUpload> all;
            all.assign(s.ready_ranges.begin(), s.ready_ranges.end());
            all.insert(all.end(), ranges.begin(), ranges.end());
            MergeUploads(all, s.ready_ranges);
        }
        s.ready_seq.store(seq, std::memory_order_release);
    }
    s.ready_cv.notify_all();
    if (!raw.empty()) {
        g_c.ahead_batches.fetch_add(1, std::memory_order_relaxed);
        g_c.ahead_taken.fetch_add(raw.size(), std::memory_order_relaxed);
    }
}

void DrainAheadWorker(DrainAheadState& s) {
    std::vector<PendingUpload> raw;
    std::vector<PendingUpload> taken;
    std::vector<PendingUpload> ranges;
    for (;;) {
        const uint64_t seq = TakeBatch(raw, taken, ranges);
        Publish(s, raw, ranges, seq);
        std::unique_lock lock{s.wake_mutex};
        s.wake_cv.wait_for(lock, kDrainAheadPoll, [&s] { return s.wake_requested || s.stop_requested; });
        s.wake_requested = false;
        if (s.stop_requested) {
            return;
        }
    }
}

// Removes [addr, +size) from a published list, splitting the entries it cuts (the rule of Release for the queue).
bool DropFrom(std::vector<PendingUpload>& v, uint64_t addr, uint64_t size) {
    std::vector<PendingUpload> kept;
    kept.reserve(v.size());
    bool changed = false;
    for (const PendingUpload& p : v) {
        if (!Overlaps(p.addr, p.size, addr, size)) {
            kept.push_back(p);
            continue;
        }
        changed = true;
        const uint64_t end = p.addr + p.size;
        if (p.addr < addr) {
            kept.push_back(PendingUpload{p.addr, addr - p.addr, p.kind, p.seq, p.served});
        }
        if (end > addr + size) {
            kept.push_back(PendingUpload{addr + size, end - (addr + size), p.kind, p.seq, p.served});
        }
    }
    v.swap(kept);
    return changed;
}

void DrainAheadDrop(uint64_t addr, uint64_t size) {
    if (!g_drain_ahead_on) {
        return;
    }
    DrainAheadState& s = DaState();
    std::scoped_lock lock{s.ready_mutex};
    if (DropFrom(s.ready_raw, addr, size)) {
        RebuildInval(s); // sotc: the raw indices moved (a split or a removed entry)
    }
    DropFrom(s.ready_ranges, addr, size);
}

void RequestDrainAheadWake(DrainAheadState& s) {
    {
        std::scoped_lock lock{s.wake_mutex};
        s.wake_requested = true;
    }
    s.wake_cv.notify_one();
}

} // namespace

bool DrainAheadOn() {
    return g_drain_ahead_on;
}

void DrainAheadStart() {
    DrainAheadState& s = DaState();
    if (!g_drain_ahead_on || s.worker != nullptr) {
        return;
    }
    {
        std::scoped_lock lock{s.wake_mutex};
        s.wake_requested = false;
        s.stop_requested = false;
    }
    s.running.store(true, std::memory_order_release);
    s.worker = new std::thread([&s] { DrainAheadWorker(s); });
}

void DrainAheadStop() {
    DrainAheadState& s = DaState();
    if (s.worker == nullptr) {
        return;
    }
    {
        std::scoped_lock lock{s.wake_mutex};
        s.stop_requested = true;
    }
    s.wake_cv.notify_all();
    s.worker->join();
    delete s.worker;
    s.worker = nullptr;
    s.running.store(false, std::memory_order_release);
}

size_t DrainAheadTake(uint64_t want_seq, std::vector<PendingUpload>& raw, std::vector<PendingUpload>& ranges,
                      std::vector<InvalRef>& inval, std::vector<InvalGroup>& groups) {
    DrainAheadState& s = DaState();
    if (!s.running.load(std::memory_order_acquire)) {
        // Not started (or stopped): nothing else would take the queue, so this takes it as the worker would.
        std::vector<PendingUpload> batch_raw;
        std::vector<PendingUpload> batch_taken;
        std::vector<PendingUpload> batch_ranges;
        const uint64_t seq = TakeBatch(batch_raw, batch_taken, batch_ranges);
        Publish(s, batch_raw, batch_ranges, seq);
    }
    std::unique_lock lock{s.ready_mutex};
    const size_t ahead = s.ready_raw.size(); // published before this drain asked
    if (s.ready_seq.load(std::memory_order_acquire) < want_seq) {
        RequestDrainAheadWake(s);
        const auto t0 = std::chrono::steady_clock::now();
        s.ready_cv.wait(lock, [&s, want_seq] { return s.ready_seq.load(std::memory_order_acquire) >= want_seq; });
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        g_c.ahead_waits.fetch_add(1, std::memory_order_relaxed);
        g_c.ahead_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        uint64_t prev = g_c.ahead_wait_max_ns.load(std::memory_order_relaxed);
        while (ns > prev && !g_c.ahead_wait_max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
        }
    }
    raw.clear();
    ranges.clear();
    raw.swap(s.ready_raw);
    ranges.swap(s.ready_ranges);
    inval.clear(); // sotc: the worker starts the next batch of groups empty
    groups.clear();
    inval.swap(s.ready_inval);
    groups.swap(s.ready_groups);
    return ahead;
}

void NoteDrainAhead(uint64_t entries, uint64_t bytes) {
    g_c.ahead_entries.fetch_add(entries, std::memory_order_relaxed);
    g_c.ahead_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

namespace {
std::atomic<void*> g_owner_ctx{nullptr};
std::atomic<void (*)(void*, uint64_t, uint64_t)> g_owner_fn{nullptr};
} // namespace

void SetOwnerDiscard(void* ctx, void (*fn)(void* ctx, uint64_t addr, uint64_t size)) {
    g_owner_ctx.store(ctx, std::memory_order_relaxed);
    g_owner_fn.store(fn, std::memory_order_release);
}

// SOTC_NATIVE_OWNER_DISCARD: 1 = at allocations (0x12820) only, 2 = also at frees (0x12af0).
int OwnerDiscardMode() {
    static const int env_mode = [] {
        const char* v = std::getenv("SOTC_NATIVE_OWNER_DISCARD");
        return (v && (v[0] == '1' || v[0] == '2')) ? v[0] - '0' : 0;
    }();
    return g_mode != 0 ? env_mode : 0;
}

bool OwnerDiscardOn() {
    return OwnerDiscardMode() != 0;
}

bool OwnerDiscardAtFree() {
    return OwnerDiscardMode() == 2;
}

namespace {
std::atomic<void*> g_region_ctx{nullptr};
std::atomic<void (*)(void*, uint64_t, uint64_t)> g_region_fn{nullptr};
} // namespace

void SetRegionDiscard(void* ctx, void (*fn)(void* ctx, uint64_t addr, uint64_t size)) {
    g_region_ctx.store(ctx, std::memory_order_relaxed);
    g_region_fn.store(fn, std::memory_order_release);
}

void DiscardRegion(uint64_t addr, uint64_t size) {
    if (g_mode == 0 || addr == 0 || size == 0) {
        return;
    }
    if (auto* fn = g_region_fn.load(std::memory_order_acquire)) {
        fn(g_region_ctx.load(std::memory_order_relaxed), addr, size);
    }
}

void DiscardOwnerForced(uint64_t addr, uint64_t size) {
    if (g_mode == 0 || addr == 0 || size == 0) {
        return;
    }
    if (auto* fn = g_owner_fn.load(std::memory_order_acquire)) {
        fn(g_owner_ctx.load(std::memory_order_relaxed), addr, size);
    }
}

namespace {
thread_local uint64_t t_owner_tag = 0;
}
void SetOwnerDiscardTag(uint64_t tag) {
    t_owner_tag = tag;
}
uint64_t OwnerDiscardTag() {
    return t_owner_tag;
}

void NoteOwnerDiscard(OwnerDiscard what, uint64_t n) {
    switch (what) {
    case OwnerDiscard::Call: g_c.owner_calls.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::Discarded: g_c.owner_discarded.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::Bytes: g_c.owner_bytes.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::PagesUnprotected: g_c.owner_pages_unprotected.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::SharedPages: g_c.owner_shared_pages.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::SkippedClean: g_c.owner_skipped_clean.fetch_add(n, std::memory_order_relaxed); break;
    case OwnerDiscard::SkippedCmd: g_c.owner_skipped_cmd.fetch_add(n, std::memory_order_relaxed); break;
    }
}

void DiscardOwner(uint64_t addr, uint64_t size) {
    if (!OwnerDiscardOn() || addr == 0 || size == 0) {
        return;
    }
    NoteOwnerDiscard(OwnerDiscard::Call);
    if (auto* fn = g_owner_fn.load(std::memory_order_acquire)) {
        fn(g_owner_ctx.load(std::memory_order_relaxed), addr, size);
    }
}

void SetFrameBackend(const FrameBackend& backend) {
    std::scoped_lock lock{g_mutex};
    g_backend_set.store(false, std::memory_order_release);
    g_backend = backend;
    g_backend_set.store(backend.ctx != nullptr && backend.submitted && backend.wait && backend.completed,
                        std::memory_order_release);
}

void NoteUploaded(uint64_t bytes) {
    g_c.uploads.fetch_add(1, std::memory_order_relaxed);
    if (bytes == 0) {
        g_c.uploads_clean.fetch_add(1, std::memory_order_relaxed);
    }
    g_c.uploaded_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteMarked(uint64_t bytes) {
    g_c.marked_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteExactUpload(uint64_t bytes) {
    g_c.exact_uploads.fetch_add(1, std::memory_order_relaxed);
    g_c.exact_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteDrainEntry(uint64_t seq, uint64_t pickup_seq, uint64_t size) {
    g_c.drain_entries.fetch_add(1, std::memory_order_relaxed);
    g_c.drain_bytes.fetch_add(size, std::memory_order_relaxed);
    if (pickup_seq != 0 && seq > pickup_seq) {
        g_c.drain_after_pickup.fetch_add(1, std::memory_order_relaxed);
        g_c.drain_after_bytes.fetch_add(size, std::memory_order_relaxed);
        const uint64_t lag = seq - pickup_seq;
        uint64_t prev = g_c.drain_lag_max.load(std::memory_order_relaxed);
        while (lag > prev && !g_c.drain_lag_max.compare_exchange_weak(prev, lag, std::memory_order_relaxed)) {
        }
    }
}

void NoteDrainDone(uint64_t warm_deferred_bytes, uint64_t warm_backlog_bytes) {
    g_c.drains.fetch_add(1, std::memory_order_relaxed);
    if (warm_deferred_bytes != 0) {
        g_c.warm_deferred.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t prev = g_c.warm_backlog_max.load(std::memory_order_relaxed);
    while (warm_backlog_bytes > prev &&
           !g_c.warm_backlog_max.compare_exchange_weak(prev, warm_backlog_bytes, std::memory_order_relaxed)) {
    }
}

void NoteDroppedUpload() {
    g_c.announce_dropped.fetch_add(1, std::memory_order_relaxed);
}

void NoteDrainWork(uint64_t ns, uint64_t pages_marked, uint64_t invalidations, uint64_t served_entries,
                   uint64_t ranges) {
    g_c.drain_ns.fetch_add(ns, std::memory_order_relaxed);
    g_c.drain_pages.fetch_add(pages_marked, std::memory_order_relaxed);
    g_c.drain_invalidations.fetch_add(invalidations, std::memory_order_relaxed);
    g_c.drain_served.fetch_add(served_entries, std::memory_order_relaxed);
    g_c.drain_ranges.fetch_add(ranges, std::memory_order_relaxed);
}

void FlipReuseFence(uint64_t frame_now, RingDepth depth) {
    if (g_mode == 0) {
        return;
    }
    const uint64_t d = static_cast<uint64_t>(depth);
    if (frame_now < d) {
        return;
    }
    g_c.reuse_fences.fetch_add(1, std::memory_order_relaxed);
    FrameWait(frame_now - d);
}

} // namespace SotcNative
