// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the PC-native memory model, the API of native.h. See docs/re-stutters/native/PLAN.md.
// Announced uploads are queued here and performed by the draw thread (BufferCache::NativeDrain); the frame
// ticket is a backend supplied by BufferCache.
#include "native.h"
#include "native_draw.h"
#include "../pc_ring.h" // sotc: 1a, HostServed (ring 8 imported runs)
#include "verify.h" // sotc: coverage oracle (SOTC_VERIFY)
#include "n1verify.h" // sotc: SOTC_VERIFY=n1

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
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
    const uint64_t seq = g_announce_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    SotcVerify::NoteAnnounce(addr, size); // sotc: clears the coverage oracle's pending faults
    SotcN1Verify::NoteAnnounce(addr, size, static_cast<uint8_t>(kind), __builtin_return_address(0)); // sotc: SOTC_VERIFY=n1
    if (kind == Kind::GpuWritten) {
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
            NoteDroppedUpload();
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "SOTC NATIVE upload queue full (%zu): announced ranges dropped\n",
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
