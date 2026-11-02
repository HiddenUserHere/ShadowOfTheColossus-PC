// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU result copies. See native_copies.h for the contract.
//
// Ring: registrations take consecutive byte ranges of a 4 MiB host-cached buffer (never wrapping inside one
// range); a registration stays valid until the ring wraps over it. The draw thread records the GPU copy of each
// written overlap (barrier compute write -> transfer, copy, barrier transfer -> host and all commands).
//
// Writes: the CPU job registers a range before the frame's submit, but the draw thread records the dispatch
// later, and it can lag a frame or more behind the job. A single-write registration therefore waits in a FIFO
// per base (g_pending): the next recorded write at its base takes the oldest waiting registration of that
// base. A registration whose frame the GPU completed without its write is dropped (expired) so that a later
// write never pairs with it. Other registrations take the copies of every overlap with the newest registration
// of a base (the old rule).
//
// Complete: the copies cover the whole range and SotcNative::FrameCompleted() reached the frame of the
// registration (every write and copy of that frame is done).
//
// Indexes: by base (the newest registration there, for KeyOf and the overlap query) and by key (newest first,
// for the clients' lookups). Pruning drops the oldest entries only (the ring first; the record cap is sized so
// that it is never reached before the ring wraps).
#include "native_copies.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "native.h"
#include "video_core/buffer_cache/buffer.h"

namespace SotcNativeCopies {
namespace {

constexpr uint64_t kAlign = 256;
// Every record takes at least kAlign bytes of the ring, so the ring bounds the record count first.
constexpr size_t kMaxRecords = kRingBytes / kAlign;
constexpr size_t kKeyDepth = 8; // registrations kept per key in the key index
constexpr uint64_t kNeededFrames = 3; // a clients' lookup reaches this many frames back (masks: kOldestFrames)

struct Entry {
    uint64_t key;
    uint64_t seq;
    uint64_t base;
    uint64_t bytes;
    uint64_t pos;      // monotonic ring byte position
    uint64_t frame;    // native frame of the registration
    uint64_t lo, hi;   // guest bytes the recorded copies covered (bounds of their union)
    uint32_t copies;
    uint64_t async;    // sotc: newest async queue tick of a copy in this registration (0: graphics only)
    uint64_t identity; // the object behind the key (0: unknown)
    bool single;       // one GPU write per registration: paired through g_pending
};

std::mutex g_mutex; // everything below
std::deque<Entry> g_entries; // registration order; references stay valid across push_back / pop_front
std::map<uint64_t, Entry*> g_by_base;                       // newest registration per base
std::map<uint64_t, std::deque<Entry*>> g_pending;           // single-write registrations waiting for their write
std::unordered_map<uint64_t, std::vector<Entry*>> g_by_key; // newest first, at most kKeyDepth
uint64_t g_head = 0;
uint64_t g_expired_upto = 0; // completed frame the pending queues were last expired for
VideoCore::Buffer* g_staging = nullptr;
std::atomic<bool> g_watching{false};
std::atomic<uint64_t> g_recording_tick{0};   // sotc: N5 async: the async tick being recorded (0: graphics)
bool (*g_async_done)(uint64_t tick) = nullptr; // sotc: N5 async: completion of an async tick

struct Counters {
    std::atomic<uint64_t> registered{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> copies{0};
    std::atomic<uint64_t> copied_bytes{0};
    std::atomic<uint64_t> paired{0};
    std::atomic<uint64_t> unpaired{0};
    std::atomic<uint64_t> expired{0};
    std::atomic<uint64_t> pruned_ring{0};
    std::atomic<uint64_t> pruned_ring_live{0};
    std::atomic<uint64_t> pruned_count{0};
    std::atomic<uint64_t> kept_same_frame{0};
    std::atomic<uint64_t> identity_changed{0};
    std::atomic<uint64_t> forgot{0};
    std::atomic<uint64_t> key_missing{0};
    std::atomic<uint64_t> key_invalid{0};
    std::atomic<uint64_t> lag_max{0};
    std::atomic<uint64_t> write_lag_max{0};
    std::atomic<uint64_t> ring_span_max{0};
};
Counters g_c;

void MaxInto(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t prev = a.load(std::memory_order_relaxed);
    while (v > prev && !a.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

bool Valid(const Entry& e) {
    return g_head <= e.pos + kRingBytes;
}

// The reason a registration cannot be served now (kNone: it can). Caller holds g_mutex.
Miss Why(const Entry& e, uint64_t need, uint64_t completed) {
    if (!Valid(e)) {
        return Miss::kInvalid;
    }
    if (e.copies == 0) {
        return completed >= e.frame ? Miss::kNoWrite : Miss::kFrame;
    }
    if (e.bytes < need) {
        return Miss::kShort;
    }
    if (!(e.lo <= e.base && e.hi >= e.base + e.bytes)) {
        return Miss::kNotCovered;
    }
    if (completed < e.frame) {
        return Miss::kFrame;
    }
    if (e.async != 0 && !(g_async_done && g_async_done(e.async))) {
        return Miss::kAsync;
    }
    return Miss::kNone;
}

void RemoveFromIndexes(const Entry* e) {
    if (auto it = g_by_base.find(e->base); it != g_by_base.end() && it->second == e) {
        g_by_base.erase(it);
    }
    if (auto it = g_by_key.find(e->key); it != g_by_key.end()) {
        auto& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == e) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (list.empty()) {
            g_by_key.erase(it);
        }
    }
    if (e->single) {
        if (auto it = g_pending.find(e->base); it != g_pending.end()) {
            auto& q = it->second;
            for (size_t i = 0; i < q.size(); ++i) {
                if (q[i] == e) {
                    q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (q.empty()) {
                g_pending.erase(it);
            }
        }
    }
}

// Drops the oldest entries the ring overwrote or the record cap pushes out. Caller holds g_mutex.
void PruneLocked(uint64_t completed) {
    while (!g_entries.empty() && (!Valid(g_entries.front()) || g_entries.size() > kMaxRecords)) {
        const Entry& e = g_entries.front();
        if (!Valid(e)) {
            g_c.pruned_ring.fetch_add(1, std::memory_order_relaxed);
            if (e.frame + kNeededFrames >= completed) {
                g_c.pruned_ring_live.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_c.pruned_count.fetch_add(1, std::memory_order_relaxed);
        }
        RemoveFromIndexes(&e);
        g_entries.pop_front();
    }
    g_watching.store(!g_by_base.empty(), std::memory_order_release);
}

// Registrations whose frame the GPU completed have had their write recorded (a frame's writes are recorded before
// its submit). The ones still waiting are dropped so that a later write does not pair with them. Caller holds g_mutex.
void ExpirePendingLocked(uint64_t completed) {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        auto& q = it->second;
        while (!q.empty() && q.front()->frame <= completed) {
            q.pop_front();
            g_c.expired.fetch_add(1, std::memory_order_relaxed);
        }
        it = q.empty() ? g_pending.erase(it) : std::next(it);
    }
}

const uint8_t* StagingLocked(const Entry& e, uint64_t bytes) {
    if (!g_staging || g_staging->mapped_data.empty()) {
        return nullptr;
    }
    const uint64_t off = e.pos % kRingBytes;
    g_staging->Invalidate(off, bytes);
    return g_staging->mapped_data.data() + off;
}

} // namespace

// sotc: compute programs the draw thread dispatched, by code address (the LOD mirror tells its pre-shader variants
// apart with it). A handful of entries; the address is the one COMPUTE_PGM_LO/HI hold.
namespace {
constexpr size_t kProgramSlots = 16;
std::mutex g_program_mutex;
uint64_t g_program_addr[kProgramSlots];
uint64_t g_program_hash[kProgramSlots];
} // namespace

void NoteProgram(uint64_t code_address, uint64_t hash) {
    std::lock_guard<std::mutex> lock(g_program_mutex);
    for (size_t i = 0; i < kProgramSlots; ++i) {
        if (g_program_addr[i] == code_address || g_program_addr[i] == 0) {
            g_program_addr[i] = code_address;
            g_program_hash[i] = hash;
            return;
        }
    }
    g_program_addr[0] = code_address; // full: recycle the first slot
    g_program_hash[0] = hash;
}

uint64_t ProgramAt(uint64_t code_address) {
    std::lock_guard<std::mutex> lock(g_program_mutex);
    for (size_t i = 0; i < kProgramSlots; ++i) {
        if (g_program_addr[i] == code_address) {
            return g_program_hash[i];
        }
    }
    return 0;
}

bool Armed() {
    static const bool armed = [] {
        const char* p = std::getenv("SOTC_NATIVE_PARTICLES");
        const char* m = std::getenv("SOTC_NATIVE_MASKS");
        return (p && std::strcmp(p, "prev") == 0) || (m && (std::strcmp(m, "prev") == 0 || std::strcmp(m, "cpu") == 0));
    }();
    return armed;
}

uint64_t CurrentFrame() {
    return SotcNative::FrameSubmitted() + 1;
}

bool Register(uint64_t key, uint64_t seq, uint64_t base, uint64_t bytes, uint64_t identity, bool single_write) {
    if (!Armed() || !SotcNative::On() || base == 0 || bytes == 0 || bytes > kMaxBytes) {
        g_c.refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t frame = CurrentFrame();
    const uint64_t completed = SotcNative::FrameCompleted(); // before g_mutex: it takes the backend's lock
    std::lock_guard<std::mutex> lock(g_mutex);
    if (completed > g_expired_upto) {
        ExpirePendingLocked(completed);
        g_expired_upto = completed;
    }
    if (auto it = g_by_key.find(key); it != g_by_key.end() && !it->second.empty()) {
        const Entry* front = it->second.front();
        bool forget = front->seq > seq; // the frame went backwards: a new object behind the key
        if (front->seq == seq) {
            g_c.kept_same_frame.fetch_add(1, std::memory_order_relaxed); // two registrations of one frame
        }
        if (identity != 0 && front->identity != 0 && front->identity != identity) {
            g_c.identity_changed.fetch_add(1, std::memory_order_relaxed);
            forget = forget || front->bytes != bytes; // another object of another size: its copies are not its own
        }
        if (forget) {
            const std::vector<Entry*> stale = it->second;
            for (Entry* e : stale) {
                RemoveFromIndexes(e);
                e->copies = 0; // dead: never complete, never served
            }
            g_c.forgot.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const uint64_t size = AlignUp(bytes, kAlign);
    uint64_t pos = g_head;
    if (pos % kRingBytes + size > kRingBytes) {
        pos = AlignUp(pos, kRingBytes); // a range never wraps
    }
    g_head = pos + size;
    g_entries.push_back(Entry{});
    Entry* e = &g_entries.back();
    e->key = key;
    e->seq = seq;
    e->base = base;
    e->bytes = bytes;
    e->pos = pos;
    e->frame = frame;
    e->lo = 0;
    e->hi = 0;
    e->copies = 0;
    e->async = 0;
    e->identity = identity;
    e->single = single_write;
    g_by_base[base] = e;
    if (single_write) {
        g_pending[base].push_back(e);
    }
    auto& list = g_by_key[key];
    list.insert(list.begin(), e);
    if (list.size() > kKeyDepth) {
        list.pop_back();
    }
    PruneLocked(completed);
    if (!g_entries.empty()) {
        MaxInto(g_c.ring_span_max, g_head - g_entries.front().pos);
    }
    g_c.registered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool KeyOf(uint64_t base, uint64_t* key, uint64_t* seq, uint64_t* identity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_by_base.find(base);
    if (it == g_by_base.end()) {
        g_c.key_missing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!Valid(*it->second)) {
        g_c.key_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    *key = it->second->key;
    *seq = it->second->seq;
    if (identity) {
        *identity = it->second->identity;
    }
    return true;
}

bool KeyLive(uint64_t key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_by_key.find(key) != g_by_key.end();
}

uint64_t CopyOut(uint64_t key, uint64_t min_seq, uint64_t max_seq, uint64_t need, void* dst, bool* uncopied,
                 Miss* miss) {
    const uint64_t submitted = SotcNative::FrameSubmitted();
    const uint64_t completed = SotcNative::FrameCompleted(); // before g_mutex: it takes the backend's lock
    MaxInto(g_c.lag_max, submitted > completed ? submitted - completed : 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    Miss reason = Miss::kNoKey;
    const auto it = g_by_key.find(key);
    if (it == g_by_key.end()) {
        if (miss) {
            *miss = reason;
        }
        return 0;
    }
    for (const Entry* e : it->second) { // newest first
        if (e->seq > max_seq) {
            continue;
        }
        if (e->seq < min_seq) {
            break;
        }
        if (e->seq == max_seq && e->copies == 0 && uncopied) {
            *uncopied = true;
        }
        const Miss why = Why(*e, need, completed);
        if (why != Miss::kNone) {
            if (reason == Miss::kNoKey) {
                reason = why; // the newest candidate's reason
            }
            continue;
        }
        const uint8_t* src = StagingLocked(*e, need);
        if (!src) {
            if (miss) {
                *miss = Miss::kNoStaging;
            }
            return 0;
        }
        std::memcpy(dst, src, need); // under g_mutex: no newer registration can take this ring range meanwhile
        if (miss) {
            *miss = Miss::kNone;
        }
        return e->seq;
    }
    if (miss) {
        *miss = reason;
    }
    return 0;
}

void Attach(VideoCore::Buffer* staging) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_staging = staging;
}

bool Watching() {
    return g_watching.load(std::memory_order_acquire);
}

void NoteWrittenBind(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr, uint64_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_staging || (g_by_base.empty() && g_pending.empty())) {
        return;
    }
    const uint64_t submitted = SotcNative::FrameSubmitted(); // an atomic count (no lock)
    // Registrations are at most kMaxBytes long: the first one that can overlap starts at addr - kMaxBytes.
    const uint64_t scan_lo = addr > kMaxBytes ? addr - kMaxBytes : 0;
    // Copies the overlap of the write with e (recorded now, in stream order).
    const auto record = [&](Entry& e, uint64_t lo, uint64_t hi) {
        vk::MemoryBarrier before{};
        before.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {},
                               before, {}, {});
        cmdbuf.copyBuffer(src, g_staging->Handle(),
                          vk::BufferCopy{src_offset + (lo - addr), e.pos % kRingBytes + (lo - e.base), hi - lo});
        // The host reads the copy after the frame's fence; later commands may write the source again.
        vk::MemoryBarrier after{};
        after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        after.dstAccessMask = vk::AccessFlagBits::eHostRead;
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eAllCommands, {}, after,
                               {}, {});
        e.lo = e.copies == 0 || lo < e.lo ? lo : e.lo;
        e.hi = e.copies == 0 || hi > e.hi ? hi : e.hi;
        ++e.copies;
        if (const uint64_t tick = g_recording_tick.load(std::memory_order_relaxed); tick > e.async) {
            e.async = tick; // sotc: N5 async: the copy completes with its async submit
        }
        g_c.copies.fetch_add(1, std::memory_order_relaxed);
        g_c.copied_bytes.fetch_add(hi - lo, std::memory_order_relaxed);
    };
    // The other registrations: every overlap with the newest registration of a base takes the write.
    for (auto it = g_by_base.lower_bound(scan_lo); it != g_by_base.end() && it->first < addr + size; ++it) {
        Entry& e = *it->second;
        const uint64_t end = e.base + e.bytes;
        if (addr >= end || !Valid(e)) {
            continue;
        }
        if (e.single) {
            // Its write is paired through g_pending; a write with nothing waiting at the base is not its own.
            if (g_pending.find(e.base) == g_pending.end()) {
                g_c.unpaired.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
        record(e, lo, hi);
    }
    // Single-write registrations: the oldest waiting one at each base takes the write (FIFO, see the top comment).
    for (auto it = g_pending.lower_bound(scan_lo); it != g_pending.end() && it->first < addr + size;) {
        auto& q = it->second;
        while (!q.empty() && !Valid(*q.front())) {
            q.pop_front();
        }
        if (!q.empty()) {
            Entry& e = *q.front();
            const uint64_t end = e.base + e.bytes;
            if (addr < end) {
                q.pop_front();
                const uint64_t lo = addr > e.base ? addr : e.base;
                const uint64_t hi = addr + size < end ? addr + size : end;
                record(e, lo, hi);
                g_c.paired.fetch_add(1, std::memory_order_relaxed);
                MaxInto(g_c.write_lag_max, submitted > e.frame ? submitted - e.frame : 0);
            }
        }
        it = q.empty() ? g_pending.erase(it) : std::next(it);
    }
}

void SetRecordingTick(uint64_t tick) {
    g_recording_tick.store(tick, std::memory_order_relaxed);
}

void SetAsyncDone(bool (*done)(uint64_t tick)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_async_done = done;
}

void TakeStats(Stats& out) {
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    out.paired = take(g_c.paired);
    out.unpaired = take(g_c.unpaired);
    out.expired = take(g_c.expired);
    out.pruned_ring = take(g_c.pruned_ring);
    out.pruned_ring_live = take(g_c.pruned_ring_live);
    out.pruned_count = take(g_c.pruned_count);
    out.kept_same_frame = take(g_c.kept_same_frame);
    out.identity_changed = take(g_c.identity_changed);
    out.forgot = take(g_c.forgot);
    out.key_missing = take(g_c.key_missing);
    out.key_invalid = take(g_c.key_invalid);
    out.lag_max = take(g_c.lag_max);
    out.write_lag_max = take(g_c.write_lag_max);
    out.ring_span_max = take(g_c.ring_span_max);
}

void Report(std::string& out, double seconds) {
    if (!Armed()) {
        return;
    }
    size_t live = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        live = g_entries.size();
    }
    const double secs = seconds > 0.0 ? seconds : 1.0;
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC NATIVE COPIES: registered %.0f/s (refused %llu, live %zu), GPU copies %.0f/s, %.1f KiB/s\n",
                  static_cast<double>(take(g_c.registered)) / secs, static_cast<unsigned long long>(take(g_c.refused)),
                  live, static_cast<double>(take(g_c.copies)) / secs,
                  static_cast<double>(take(g_c.copied_bytes)) / 1024.0 / secs);
    out += line;
}

} // namespace SotcNativeCopies
