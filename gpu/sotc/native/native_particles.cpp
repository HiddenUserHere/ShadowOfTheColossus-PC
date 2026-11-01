// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native particle stats, option 3 of docs/re-stutters/native/impl-particles.md. See
// native_particles.h for the contract.
//
// Timeline (particle_prev.cpp, checked against the disassembly). The call with pool counter c (after its
// increment) dispatches the count pass for call id c and reads the array dispatched by call id c - 2.
// Each array index is a system index of the pool, so a copy of any earlier dispatch answers the same
// question with older data.
//
// Copies. OnStatsRead registers the array of call id c (Entry). The draw thread later records the
// dispatch that writes it; NoteWrittenBind copies every written overlap into a 2 MiB host-readable ring
// (the newest entry of a base takes the copy, so a later frame's write never lands in an older entry).
// An entry is complete when its copies cover the whole array and the GPU finished the frame the dispatch
// belongs to (SotcNative::FrameCompleted): every write and copy of that frame is done, nothing waits.
//
// Serving. The call with counter c takes the entry of id c - 2 when complete; else the newest complete
// entry of ids c - 3 .. c - 4 (one or two calls older, inside the 1 s retirement tolerance); else the
// exact read. An entry is served only when it holds every system the loop reads (extent): a value the
// copy does not hold is never served, so no unknown count becomes a zero that would retire a live
// AutoDestroy system. The counts are copied into one of 16 guest blocks (low range) and 0x8c1b0 returns
// that block; ProcessStats only reads the array (decomp 0x1018c0), so nothing the game writes is lost.
//
// Verify (SotcNative::Verify): nothing is served. The candidate's counts are kept for the call, and the
// site after the load at 0x101a98 compares each exact count (r15d) with them.
#include "native_particles.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "native.h"
#include "video_core/buffer_cache/buffer.h"

// src/runtime_memory.c: a guest-visible block in the low range. Weak, as in native_results.cpp: the test
// binaries that link libbbgpu.a without the loader still link; without it nothing is served.
extern "C" void* runtime_low_map(size_t size, int prot) __attribute__((weak));

namespace SotcNativeParticles {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kVerifyHook = "NATIVE_PARTICLE_VERIFY";
constexpr int kProtReadWrite = 1 | 2;       // PROT_READ | PROT_WRITE (src/compat_win.h)
constexpr uint64_t kRingBytes = kCopyRingBytes; // host-readable copies of the dispatched arrays
constexpr uint64_t kCopyAlign = 256;
constexpr uint64_t kMaxEntries = 4096;      // u32 counts per copy (16 KiB); larger arrays keep the exact read
constexpr uint64_t kPadEntries = 256;       // systems created after the dispatch, before its read
constexpr size_t kMaxRecords = 512;
constexpr uint64_t kServeBlocks = 16;
constexpr uint64_t kServeBlockBytes = kMaxEntries * 4;
constexpr uint64_t kOldestAge = 4; // serve ids c - 2 .. c - 4

struct Entry {
    uint64_t pool;
    uint64_t id;
    uint64_t base;
    uint64_t entries; // u32 counts the copy holds
    uint64_t pos;     // monotonic ring byte position
    uint64_t frame;   // native frame of the dispatch
    uint64_t lo, hi;  // guest bytes the recorded copies covered (bounds of their union)
    uint32_t copies;
};

std::mutex g_mutex; // g_entries, g_head, g_staging, the copies and the serve memcpy
std::deque<Entry> g_entries;
uint64_t g_head = 0; // ring end of the newest allocation
VideoCore::Buffer* g_staging = nullptr;
std::atomic<bool> g_watching{false};
std::atomic<uint64_t> g_span_lo{~0ull}; // bounds of the registered arrays (prefilter of NoteWrittenBind)
std::atomic<uint64_t> g_span_hi{0};

uint64_t g_next_block = 0; // the next serve block, guarded by g_mutex

struct Counters {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> registered{0};
    std::atomic<uint64_t> too_large{0};    // arrays past kMaxEntries (exact read)
    std::atomic<uint64_t> copies{0};       // GPU copies recorded
    std::atomic<uint64_t> served_exact{0}; // id c - 2: the same data as the exact read
    std::atomic<uint64_t> served_older{0}; // id c - 3 or c - 4
    std::atomic<uint64_t> fallback{0};     // no complete entry: the exact read
    std::atomic<uint64_t> not_copied{0};   // ... of which the c - 2 entry had no copy at all
    std::atomic<uint64_t> empty{0};        // calls that read no system
    std::atomic<uint64_t> no_native{0};    // armed without SOTC_NATIVE
    std::atomic<uint64_t> v_counts{0};     // verify: counts compared
    std::atomic<uint64_t> v_mismatch{0};   // verify: counts that differ
    std::atomic<uint64_t> v_mismatch_exact{0}; // ... of which from the c - 2 entry (must be 0)
    std::atomic<uint64_t> v_zero{0};       // verify: copy 0 where the exact count is > 0
};
Counters g_c;

// Verify: the candidate of the current call (the site handler runs on the same thread, later).
struct VerifyCall {
    bool active = false;
    bool exact = false;
    uint64_t pool = 0;
    std::vector<uint32_t> counts;
};
thread_local VerifyCall t_verify;

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

bool Valid(const Entry& e) {
    return g_head <= e.pos + kRingBytes;
}

bool Complete(const Entry& e, uint64_t completed) {
    return e.copies != 0 && e.lo <= e.base && e.hi >= e.base + e.entries * 4 && completed >= e.frame &&
           Valid(e);
}

// Caller holds g_mutex. Drops the entries the ring overwrote and recomputes the prefilter span.
void PruneLocked() {
    while (!g_entries.empty() && (!Valid(g_entries.front()) || g_entries.size() > kMaxRecords)) {
        g_entries.pop_front();
    }
    uint64_t lo = ~0ull;
    uint64_t hi = 0;
    for (const Entry& e : g_entries) {
        lo = e.base < lo ? e.base : lo;
        hi = e.base + e.entries * 4 > hi ? e.base + e.entries * 4 : hi;
    }
    g_span_lo.store(lo, std::memory_order_relaxed);
    g_span_hi.store(hi, std::memory_order_relaxed);
    g_watching.store(!g_entries.empty(), std::memory_order_release);
}

void RegisterLocked(uint64_t pool, uint64_t id, uint64_t base, uint64_t entries) {
    // A counter that did not move forward is a new pool object at the same address: forget the old one.
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        it = it->pool == pool && it->id >= id ? g_entries.erase(it) : it + 1;
    }
    const uint64_t bytes = AlignUp(entries * 4, kCopyAlign);
    uint64_t pos = g_head;
    if (pos % kRingBytes + bytes > kRingBytes) {
        pos = AlignUp(pos, kRingBytes); // the copy never wraps
    }
    g_head = pos + bytes;
    g_entries.push_back(Entry{pool, id, base, entries, pos, SotcNative::FrameSubmitted() + 1, 0, 0, 0});
    PruneLocked();
}

// The entry to serve for the call with counter c, or null. Caller holds g_mutex.
const Entry* PickLocked(uint64_t pool, uint64_t counter, uint64_t read_base, uint64_t extent,
                        uint64_t completed, bool* exact, bool* uncopied) {
    const Entry* best = nullptr;
    for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
        const Entry& e = *it;
        if (e.pool != pool || e.id + 2 > counter || e.id + kOldestAge < counter) {
            continue;
        }
        const bool same = e.id + 2 == counter;
        if (same && e.base != read_base) {
            continue; // not the array this call reads (the descriptor moved)
        }
        if (same && e.copies == 0) {
            *uncopied = true;
        }
        if (e.entries < extent || !Complete(e, completed)) {
            continue;
        }
        if (!best || e.id > best->id) {
            best = &e;
        }
    }
    *exact = best && best->id + 2 == counter;
    return best;
}

const uint8_t* StagingLocked(const Entry& e, uint64_t bytes) {
    if (!g_staging || g_staging->mapped_data.empty()) {
        return nullptr;
    }
    const uint64_t off = e.pos % kRingBytes;
    g_staging->Invalidate(off, bytes);
    return g_staging->mapped_data.data() + off;
}

uint8_t* Blocks() {
    static uint8_t* const blocks = []() -> uint8_t* {
        void* p = runtime_low_map ? runtime_low_map(kServeBlocks * kServeBlockBytes, kProtReadWrite) : nullptr;
        return static_cast<uint8_t*>(p);
    }();
    return blocks;
}

HookAction OnVerifySite(GuestRegs& regs, void*) {
    VerifyCall& v = t_verify;
    if (!v.active || regs.r13 != v.pool || regs.rbx >= v.counts.size()) {
        return HookAction::Continue;
    }
    const uint32_t exact = static_cast<uint32_t>(regs.r15);
    const uint32_t copy = v.counts[regs.rbx];
    g_c.v_counts.fetch_add(1, std::memory_order_relaxed);
    if (copy != exact) {
        g_c.v_mismatch.fetch_add(1, std::memory_order_relaxed);
        if (v.exact) {
            g_c.v_mismatch_exact.fetch_add(1, std::memory_order_relaxed);
        }
        if (copy == 0 && static_cast<int32_t>(exact) > 0) {
            g_c.v_zero.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return HookAction::Continue;
}

uint64_t Take(std::atomic<uint64_t>& c) {
    return c.exchange(0, std::memory_order_relaxed);
}

} // namespace

bool Armed() {
    static const bool armed = [] {
        const char* v = std::getenv("SOTC_NATIVE_PARTICLES");
        return v != nullptr && std::strcmp(v, "prev") == 0;
    }();
    return armed;
}

HookAction OnStatsRead(GuestRegs& regs, uint64_t pool, uint64_t counter, uint64_t read_base,
                       uint64_t dispatch_base, uint64_t dispatched, uint64_t extent) {
    t_verify.active = false;
    if (!Armed()) {
        return HookAction::Continue;
    }
    g_c.calls.fetch_add(1, std::memory_order_relaxed);
    if (!SotcNative::On()) {
        g_c.no_native.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    const bool verify = SotcNative::Verify();
    const uint64_t completed = SotcNative::FrameCompleted(); // before g_mutex: it takes the backend's lock

    std::lock_guard<std::mutex> lock(g_mutex);
    // The array this call's pass writes, read two calls later. Only what the loop can read is copied.
    if (dispatch_base != 0 && dispatched != 0) {
        uint64_t entries = AlignUp(extent + kPadEntries, 64);
        entries = entries < dispatched ? entries : dispatched;
        if (entries > kMaxEntries) {
            g_c.too_large.fetch_add(1, std::memory_order_relaxed);
        } else {
            RegisterLocked(pool, counter, dispatch_base, entries);
            g_c.registered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (counter < 3) {
        return HookAction::Continue; // ProcessStats does not call 0x8c1b0 then; kept for safety
    }
    if (extent == 0) {
        g_c.empty.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue; // the loop reads nothing
    }
    bool exact = false;
    bool uncopied = false;
    const Entry* e = PickLocked(pool, counter, read_base, extent, completed, &exact, &uncopied);
    const uint8_t* src = e ? StagingLocked(*e, extent * 4) : nullptr;
    uint8_t* blocks = Blocks();
    if (!src || !blocks) {
        g_c.fallback.fetch_add(1, std::memory_order_relaxed);
        if (uncopied) {
            g_c.not_copied.fetch_add(1, std::memory_order_relaxed);
        }
        return HookAction::Continue;
    }
    if (verify) {
        t_verify.active = true;
        t_verify.exact = exact;
        t_verify.pool = pool;
        t_verify.counts.resize(extent);
        std::memcpy(t_verify.counts.data(), src, extent * 4);
        return HookAction::Continue;
    }
    uint8_t* block = blocks + (g_next_block++ % kServeBlocks) * kServeBlockBytes;
    std::memcpy(block, src, extent * 4);
    (exact ? g_c.served_exact : g_c.served_older).fetch_add(1, std::memory_order_relaxed);
    regs.rax = reinterpret_cast<uint64_t>(block);
    return HookAction::Return;
}

void Register() {
    SotcHooks::Handler handler;
    handler.pre = &OnVerifySite;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kVerifyHook, handler);
}

void Attach(VideoCore::Buffer* staging) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_staging = staging;
}

bool Watching() {
    return g_watching.load(std::memory_order_acquire);
}

void NoteWrittenBind(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr,
                     uint64_t size) {
    if (size == 0 || addr >= g_span_hi.load(std::memory_order_relaxed) ||
        addr + size <= g_span_lo.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_staging) {
        return;
    }
    uint64_t seen[8];
    size_t nseen = 0;
    for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
        Entry& e = *it;
        const uint64_t end = e.base + e.entries * 4;
        if (addr >= end || addr + size <= e.base) {
            continue;
        }
        bool newer = false; // a newer entry of the same array took this write
        for (size_t i = 0; i < nseen; ++i) {
            newer = newer || seen[i] == e.base;
        }
        if (newer) {
            continue;
        }
        if (nseen < 8) {
            seen[nseen++] = e.base;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
        vk::MemoryBarrier before{};
        before.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                               {}, before, {}, {});
        cmdbuf.copyBuffer(src, g_staging->Handle(),
                          vk::BufferCopy{src_offset + (lo - addr), e.pos % kRingBytes + (lo - e.base), hi - lo});
        // The host reads the copy after the frame's fence; later commands may write the source again.
        vk::MemoryBarrier after{};
        after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        after.dstAccessMask = vk::AccessFlagBits::eHostRead;
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eAllCommands, {},
                               after, {}, {});
        e.lo = e.copies == 0 || lo < e.lo ? lo : e.lo;
        e.hi = e.copies == 0 || hi > e.hi ? hi : e.hi;
        ++e.copies;
        g_c.copies.fetch_add(1, std::memory_order_relaxed);
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
    const uint64_t calls = Take(g_c.calls);
    char line[700];
    std::snprintf(line, sizeof line,
                  "SOTC NATIVE_PARTICLES %s: calls %.1f/s, served same %llu older %llu, exact reads %llu (no copy "
                  "%llu), empty %llu, registered %llu (too large %llu, live %zu), GPU copies %llu, no SOTC_NATIVE "
                  "%llu; verify counts %llu mismatched %llu (same-call %llu, must be 0; copy 0 for a live count "
                  "%llu)\n",
                  SotcNative::Verify() ? "verify" : "prev", static_cast<double>(calls) / secs,
                  static_cast<unsigned long long>(Take(g_c.served_exact)),
                  static_cast<unsigned long long>(Take(g_c.served_older)),
                  static_cast<unsigned long long>(Take(g_c.fallback)),
                  static_cast<unsigned long long>(Take(g_c.not_copied)),
                  static_cast<unsigned long long>(Take(g_c.empty)),
                  static_cast<unsigned long long>(Take(g_c.registered)),
                  static_cast<unsigned long long>(Take(g_c.too_large)), live,
                  static_cast<unsigned long long>(Take(g_c.copies)),
                  static_cast<unsigned long long>(Take(g_c.no_native)),
                  static_cast<unsigned long long>(Take(g_c.v_counts)),
                  static_cast<unsigned long long>(Take(g_c.v_mismatch)),
                  static_cast<unsigned long long>(Take(g_c.v_mismatch_exact)),
                  static_cast<unsigned long long>(Take(g_c.v_zero)));
    out += line;
}

} // namespace SotcNativeParticles
