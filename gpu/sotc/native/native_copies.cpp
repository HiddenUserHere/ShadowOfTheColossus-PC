// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU result copies. See native_copies.h for the contract.
//
// Ring: registrations take consecutive byte ranges of a 4 MiB host-cached buffer (never wrapping inside one
// range); a registration stays valid until the ring wraps over it. The draw thread records the GPU copy of each
// written overlap (barrier compute write -> transfer, copy, barrier transfer -> host and all commands); the
// newest registration of a base takes the write, so a later frame's write never lands in an older one.
// Complete: the copies cover the whole range and SotcNative::FrameCompleted() reached the frame of the
// registration (every write and copy of that frame is done).
//
// Indexes: by base (the newest registration there, for the draw thread's overlap query) and by key (newest
// first, for the clients' lookups). Both are rebuilt from the deque when it is pruned (rare: pruning drops
// the oldest entries only).
#include "native_copies.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "native.h"
#include "video_core/buffer_cache/buffer.h"

namespace SotcNativeCopies {
namespace {

constexpr uint64_t kAlign = 256;
constexpr size_t kMaxRecords = 8192;
constexpr size_t kKeyDepth = 8; // registrations kept per key in the key index

struct Entry {
    uint64_t key;
    uint64_t seq;
    uint64_t base;
    uint64_t bytes;
    uint64_t pos;    // monotonic ring byte position
    uint64_t frame;  // native frame of the registration
    uint64_t lo, hi; // guest bytes the recorded copies covered (bounds of their union)
    uint32_t copies;
};

std::mutex g_mutex; // everything below
std::deque<Entry> g_entries; // registration order; references stay valid across push_back / pop_front
std::map<uint64_t, Entry*> g_by_base;                    // newest registration per base
std::unordered_map<uint64_t, std::vector<Entry*>> g_by_key; // newest first, at most kKeyDepth
uint64_t g_head = 0;
VideoCore::Buffer* g_staging = nullptr;
std::atomic<bool> g_watching{false};

struct Counters {
    std::atomic<uint64_t> registered{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> copies{0};
    std::atomic<uint64_t> copied_bytes{0};
};
Counters g_c;

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

bool Valid(const Entry& e) {
    return g_head <= e.pos + kRingBytes;
}

bool Complete(const Entry& e, uint64_t completed) {
    return e.copies != 0 && e.lo <= e.base && e.hi >= e.base + e.bytes && completed >= e.frame && Valid(e);
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
}

// Drops the oldest entries the ring overwrote or the record cap pushes out. Caller holds g_mutex.
void PruneLocked() {
    while (!g_entries.empty() && (!Valid(g_entries.front()) || g_entries.size() > kMaxRecords)) {
        RemoveFromIndexes(&g_entries.front());
        g_entries.pop_front();
    }
    g_watching.store(!g_by_base.empty(), std::memory_order_release);
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

bool Armed() {
    static const bool armed = [] {
        const char* p = std::getenv("SOTC_NATIVE_PARTICLES");
        const char* m = std::getenv("SOTC_NATIVE_MASKS");
        return (p && std::strcmp(p, "prev") == 0) || (m && std::strcmp(m, "prev") == 0);
    }();
    return armed;
}

uint64_t CurrentFrame() {
    return SotcNative::FrameSubmitted() + 1;
}

bool Register(uint64_t key, uint64_t seq, uint64_t base, uint64_t bytes) {
    if (!Armed() || !SotcNative::On() || base == 0 || bytes == 0 || bytes > kMaxBytes) {
        g_c.refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t frame = CurrentFrame();
    std::lock_guard<std::mutex> lock(g_mutex);
    // A seq that did not move forward is a new object behind the same key: forget the old registrations.
    if (auto it = g_by_key.find(key); it != g_by_key.end() && !it->second.empty() && it->second.front()->seq >= seq) {
        const std::vector<Entry*> stale = it->second;
        for (Entry* e : stale) {
            RemoveFromIndexes(e);
            e->copies = 0; // dead: never complete, never served
        }
    }
    const uint64_t size = AlignUp(bytes, kAlign);
    uint64_t pos = g_head;
    if (pos % kRingBytes + size > kRingBytes) {
        pos = AlignUp(pos, kRingBytes); // a range never wraps
    }
    g_head = pos + size;
    g_entries.push_back(Entry{key, seq, base, bytes, pos, frame, 0, 0, 0});
    Entry* e = &g_entries.back();
    g_by_base[base] = e;
    auto& list = g_by_key[key];
    list.insert(list.begin(), e);
    if (list.size() > kKeyDepth) {
        list.pop_back();
    }
    PruneLocked();
    g_c.registered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool KeyOf(uint64_t base, uint64_t* key, uint64_t* seq) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_by_base.find(base);
    if (it == g_by_base.end() || !Valid(*it->second)) {
        return false;
    }
    *key = it->second->key;
    *seq = it->second->seq;
    return true;
}

uint64_t CopyOut(uint64_t key, uint64_t min_seq, uint64_t max_seq, uint64_t need, void* dst, bool* uncopied) {
    const uint64_t completed = SotcNative::FrameCompleted(); // before g_mutex: it takes the backend's lock
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_by_key.find(key);
    if (it == g_by_key.end()) {
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
        if (e->bytes < need || !Complete(*e, completed)) {
            continue;
        }
        const uint8_t* src = StagingLocked(*e, need);
        if (!src) {
            return 0;
        }
        std::memcpy(dst, src, need); // under g_mutex: no newer registration can take this ring range meanwhile
        return e->seq;
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
    if (!g_staging || g_by_base.empty()) {
        return;
    }
    // Registrations at most kMaxBytes long: the first one that can overlap starts at addr - kMaxBytes.
    auto it = g_by_base.lower_bound(addr > kMaxBytes ? addr - kMaxBytes : 0);
    for (; it != g_by_base.end() && it->first < addr + size; ++it) {
        Entry& e = *it->second;
        const uint64_t end = e.base + e.bytes;
        if (addr >= end || !Valid(e)) {
            continue;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
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
        g_c.copies.fetch_add(1, std::memory_order_relaxed);
        g_c.copied_bytes.fetch_add(hi - lo, std::memory_order_relaxed);
    }
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
