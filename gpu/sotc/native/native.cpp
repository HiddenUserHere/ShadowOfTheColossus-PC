// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the PC-native memory model, the API of native.h. See docs/re-stutters/native/PLAN.md.
// Windows are imported once and served in place; announced uploads are queued here and performed by the
// draw thread (BufferCache::NativeDrain); the frame ticket is a backend supplied by BufferCache.
#include "native.h"
#include "native_draw.h"
#include "verify.h" // sotc: coverage oracle (SOTC_VERIFY)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "video_core/renderer_vulkan/sotc_guest_memory.h"

namespace SotcNative {
namespace {

constexpr uint64_t kMaxUploadChunk = uint64_t{64} << 20; // BufferCache::DrainRange limit
constexpr size_t kMaxPending = 65536;

// Counters. Relaxed atomics: they feed the report only.
struct Counters {
    std::atomic<uint64_t> announce_persistent{};
    std::atomic<uint64_t> announce_per_frame{};
    std::atomic<uint64_t> announce_gpu_written{};
    std::atomic<uint64_t> announce_in_window{};
    std::atomic<uint64_t> announce_dropped{};
    std::atomic<uint64_t> announce_released{};
    std::atomic<uint64_t> binds_served{};
    std::atomic<uint64_t> binds_refused{};
    std::atomic<uint64_t> uploaded_bytes{};
    std::atomic<uint64_t> uploads{};
    std::atomic<uint64_t> uploads_clean{};
    std::atomic<uint64_t> marked_bytes{};
    std::atomic<uint64_t> frame_waits{};
    std::atomic<uint64_t> frame_wait_ns{};
    std::atomic<uint64_t> frame_wait_max_ns{};
    std::atomic<uint64_t> reuse_fences{};
    std::atomic<uint64_t> verify_unannounced{};
    std::atomic<uint64_t> verify_gpu_read{};
    std::atomic<uint64_t> verify_gpu_wait{};
};
Counters g_c;
std::atomic<uint64_t> g_submitted_local{}; // frame counter when no backend is set
std::atomic<size_t> g_pending_count{};     // size of g_pending (lock-free look)

// Windows: an immutable sorted table, published atomically. Tables are kept for the life of the process
// (windows are only added), so a reader that loaded one can use it without a lock.
struct Table {
    std::vector<Range> windows; // sorted by addr, disjoint
};
std::atomic<const Table*> g_table{nullptr};

std::mutex g_mutex; // writers of the table, the pending queue, the new-window list, the descriptor map
std::vector<std::unique_ptr<const Table>> g_tables;
std::vector<Range> g_new_windows;  // registered, not yet ringed by the draw thread
std::deque<PendingUpload> g_pending;

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

bool OverlapsAnyWindow(const Table* t, uint64_t addr, uint64_t size) {
    if (!t) {
        return false;
    }
    for (const Range& w : t->windows) {
        if (Overlaps(addr, size, w.addr, w.size)) {
            return true;
        }
    }
    return false;
}

// Caller holds g_mutex.
void PublishTable(std::vector<Range> windows) {
    std::sort(windows.begin(), windows.end(),
              [](const Range& a, const Range& b) { return a.addr < b.addr; });
    auto table = std::make_unique<Table>();
    table->windows = std::move(windows);
    const Table* raw = table.get();
    g_tables.push_back(std::move(table));
    g_table.store(raw, std::memory_order_release);
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
    SotcVerify::NoteAnnounce(addr, size); // sotc: clears the coverage oracle's pending faults
    if (kind == Kind::GpuWritten) {
        g_c.announce_gpu_written.fetch_add(1, std::memory_order_relaxed);
        return; // the GPU writes it: nothing to upload
    }
    if (kind == Kind::Persistent) {
        g_c.announce_persistent.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_c.announce_per_frame.fetch_add(1, std::memory_order_relaxed);
        if (InWindow(addr, size)) {
            g_c.announce_in_window.fetch_add(1, std::memory_order_relaxed);
            return; // host-shared: the bind serves it in place
        }
    }
    // Outside a window, PerFrame is uploaded like Persistent (the draw thread marks it CPU-modified).
    std::scoped_lock lock{g_mutex};
    for (uint64_t at = addr; at < addr + size;) {
        const uint64_t piece = std::min<uint64_t>(kMaxUploadChunk, addr + size - at);
        if (g_pending.size() >= kMaxPending) {
            NoteDroppedUpload();
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "SOTC NATIVE upload queue full (%zu): announced ranges dropped\n",
                             kMaxPending);
            }
        } else {
            g_pending.push_back(PendingUpload{at, piece, kind});
        }
        at += piece;
    }
    g_pending_count.store(g_pending.size(), std::memory_order_release);
}

void Release(uint64_t addr, uint64_t size) {
    if (g_mode == 0 || size == 0) {
        return;
    }
    SotcVerify::NoteAnnounce(addr, size); // sotc: a released range is not an unannounced write
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
            kept.push_back(PendingUpload{p.addr, addr - p.addr, p.kind});
        }
        if (end > addr + size) {
            kept.push_back(PendingUpload{addr + size, end - (addr + size), p.kind});
        }
    }
    g_pending.swap(kept);
    g_pending_count.store(g_pending.size(), std::memory_order_release);
}

bool RegisterWindow(uint64_t addr, uint64_t size, const char* name) {
    if (g_mode == 0 || size == 0) {
        return false;
    }
    const char* label = name ? name : "?";
    {
        std::scoped_lock lock{g_mutex};
        if (OverlapsAnyWindow(g_table.load(std::memory_order_acquire), addr, size)) {
            std::fprintf(stderr, "SOTC NATIVE window %s refused: overlaps a registered window\n", label);
            return false;
        }
    }
    // The import is refused when the range is not mapped or is not one stretch of backing.
    if (!Vulkan::SotcGuestMemory::ImportGuestRange(addr, size)) {
        std::fprintf(stderr, "SOTC NATIVE window %s refused: import failed (%#llx +%llu KiB)\n", label,
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size >> 10));
        return false;
    }
    std::scoped_lock lock{g_mutex};
    const Table* old = g_table.load(std::memory_order_acquire);
    if (OverlapsAnyWindow(old, addr, size)) { // registered by another thread meanwhile
        return false;
    }
    std::vector<Range> windows = old ? old->windows : std::vector<Range>{};
    windows.push_back(Range{addr, size});
    PublishTable(std::move(windows));
    g_new_windows.push_back(Range{addr, size});
    std::fprintf(stderr, "SOTC NATIVE window %s: %#llx +%llu KiB imported\n", label,
                 static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size >> 10));
    return true;
}

bool InWindow(uint64_t addr, uint64_t size) {
    const Table* t = g_table.load(std::memory_order_acquire);
    if (!t || size == 0) {
        return false;
    }
    const auto& w = t->windows;
    // The last window starting at or before addr.
    auto it = std::upper_bound(w.begin(), w.end(), addr,
                               [](uint64_t a, const Range& r) { return a < r.addr; });
    if (it == w.begin()) {
        return false;
    }
    --it;
    const uint64_t off = addr - it->addr;
    return off <= it->size && size <= it->size - off;
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
    uint64_t windows = 0;
    uint64_t win_bytes = 0;
    if (const Table* t = g_table.load(std::memory_order_acquire)) {
        windows = t->windows.size();
        for (const Range& w : t->windows) {
            win_bytes += w.size;
        }
    }
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const uint64_t served = take(g_c.binds_served);
    const uint64_t refused = take(g_c.binds_refused);
    const uint64_t ann_p = take(g_c.announce_persistent);
    const uint64_t ann_f = take(g_c.announce_per_frame);
    const uint64_t ann_g = take(g_c.announce_gpu_written);
    const uint64_t ann_w = take(g_c.announce_in_window);
    const uint64_t dropped = take(g_c.announce_dropped);
    const uint64_t released = take(g_c.announce_released);
    const uint64_t ups = take(g_c.uploads);
    const uint64_t clean = take(g_c.uploads_clean);
    const uint64_t up_bytes = take(g_c.uploaded_bytes);
    const uint64_t marked = take(g_c.marked_bytes);
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
    char line[512];
    std::snprintf(line, sizeof(line),
                  "SOTC NATIVE mode %d (%.1f s): windows %llu (%llu MiB) binds served %llu refused %llu; "
                  "announce persistent %llu per-frame %llu (in window %llu) gpu-written %llu dropped %llu "
                  "released %llu\n",
                  g_mode, seconds, static_cast<unsigned long long>(windows),
                  static_cast<unsigned long long>(win_bytes >> 20), static_cast<unsigned long long>(served),
                  static_cast<unsigned long long>(refused), static_cast<unsigned long long>(ann_p),
                  static_cast<unsigned long long>(ann_f), static_cast<unsigned long long>(ann_w),
                  static_cast<unsigned long long>(ann_g), static_cast<unsigned long long>(dropped),
                  static_cast<unsigned long long>(released));
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

bool TakeUpload(PendingUpload& out) {
    if (g_pending_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (g_pending.empty()) {
        return false;
    }
    out = g_pending.front();
    g_pending.pop_front();
    g_pending_count.store(g_pending.size(), std::memory_order_release);
    return true;
}

void TakeNewWindows(std::vector<Range>& out) {
    std::scoped_lock lock{g_mutex};
    out.insert(out.end(), g_new_windows.begin(), g_new_windows.end());
    g_new_windows.clear();
}

std::vector<Range> WindowRanges() {
    const Table* t = g_table.load(std::memory_order_acquire);
    return t ? t->windows : std::vector<Range>{};
}

void SetFrameBackend(const FrameBackend& backend) {
    std::scoped_lock lock{g_mutex};
    g_backend_set.store(false, std::memory_order_release);
    g_backend = backend;
    g_backend_set.store(backend.ctx != nullptr && backend.submitted && backend.wait && backend.completed,
                        std::memory_order_release);
}

void NoteBind(bool served) {
    (served ? g_c.binds_served : g_c.binds_refused).fetch_add(1, std::memory_order_relaxed);
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

void NoteDroppedUpload() {
    g_c.announce_dropped.fetch_add(1, std::memory_order_relaxed);
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
