// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: particle-system count readback, plan C7 (docs/re-stutters/reports2/07-readback-consumers.md,
// section 2.3). The hook changes no guest state; with SOTC_RB_CLASS=1 it also asks the buffer cache
// for copies of the count arrays (SotcRbClass::RequestCopy). With SOTC_RB_HOST (rb_host.h, R1) the
// same dispatched arrays are also registered as host runs (SotcRbHost::NoteStatsRange).
//
// Slot timeline of ParticleSystemPool::ProcessStats (eboot 0x1018c0), checked against the
// disassembly. Let k be the pool counter (pool+0x64) before call k. The call increments it.
//   - Dispatch: sub_928f0 at 0x10197d writes the count pass into slot k%3 (0x101916-0x10197d). The
//     array address is the descriptor's lo dword | (hi dword & 0xfff) << 32, the same arithmetic as
//     sub_8c1b0. The entries written are (mult * (bits or 1)) >> 2 (0x101933-0x10196a), with mult at
//     desc+8 and bits from desc+4 bits 16-29. ProcessStats writes no descriptor.
//   - Read: the counter is now k+1. For k+1 >= 3 the call reads slot (k+1)%3 through sub_8c1b0
//     (0x1019af), and array[system] at 0x101a98 uses that same array in the same call.
// So slot s is dispatched in calls k = s (mod 3) and its array is first read two calls later, in
// call k+2 (never in k+1). The read in call k is of the slot dispatched in call k-2. The slot the
// next call reads, (k+2)%3, was dispatched in call k-1.
//
// Copy request (SOTC_RB_CLASS=1): the hook of call k runs after the dispatch of call k. It asks for
// the array of the slot dispatched in this call, D = k%3 = (R+2)%3 where R is the read slot of the
// same call. That array is read in call k+2, which is two calls of lead time, not one. The request
// covers min(active-bitmap extent, pool+0x1a8 bound, entries the dispatch wrote) u32 values. The
// read in call k+2 is then checked against it (match, mismatch, no request, beyond the extent).
//
// Why no previous-frame serving: the read slot is already the oldest slot of the ring. The slot
// dispatched last is the newer one, and the GPU may still be running it, so pointing the read at it
// makes the CPU wait for newer GPU work. Older GPU data does not exist in a three-slot ring. A
// stall-free read needs a port-owned copy of a finished slot, which is not built here. The stall
// comes from the 64 KiB page being shared with GPU-written buffers (CHANGELOG, SOTC_RB_EXACT).
//
// SOTC_PARTICLE_PREV is observe-only in both modes:
//   1 = count the ProcessStats reads and whether the slot read and the dispatch-before slot are
//       readable; nothing is served ("served=0" in the report).
//   2 = as 1, and compare the counts of the read slot and the previous slot for the systems the
//       caller reads (active bitmap at pool+0x198, bound pool+0x1a8). The values come from the
//       backing view, so they show what a non-faulting reader would see; they do not prove what the
//       game reads.
// Only the return address 0x1019b4 (ProcessStats) is measured. WaterRendering::ReadQueries calls
// the same accessor from 0xe4930 and is ignored.
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "particle_prev.h"
#include "rb_class.h" // sotc: SOTC_RB_CLASS (stats ring arrays)
#include "rb_host.h"  // sotc: SOTC_RB_HOST (stats arrays as host runs)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kHook = "PARTICLE_COUNT_READ";
constexpr uint64_t kProcessStatsReturn = 0x1019b4; // eboot offset after "call sub_8c1b0" at 0x1019af
constexpr uint64_t kPoolRing = 0xf8;               // pool+0xf8: three descriptors
constexpr uint64_t kSlotStride = 0x28;             // descriptor stride
constexpr uint64_t kPoolBitmapPtr = 0x198;         // pool+0x198: u64* active-system bitmap
constexpr uint64_t kPoolBitmapWords = 0x1a0;       // pool+0x1a0: u32 number of bitmap words
constexpr uint64_t kPoolSystemLimit = 0x1a8;       // pool+0x1a8: u64 bound on the system index
constexpr uint64_t kMaxWords = 64;                 // mode 2 observation cap: 4096 systems
constexpr uint64_t kScanWords = 256;               // extent scan cap: 16384 systems (the RB 64 KiB cap)
constexpr size_t kMaxPools = 128;                  // pool records kept (LRU); each holds the 3 ring slots

// Window counters, read and reset by ReportParticlePrev.
std::atomic<uint64_t> g_calls{0};           // ProcessStats reads seen
std::atomic<uint64_t> g_read_unreadable{0}; // read slot (the one the game reads) not readable
std::atomic<uint64_t> g_prev_unreadable{0}; // previous slot not readable
std::atomic<uint64_t> g_systems{0};         // mode 2: active systems compared
std::atomic<uint64_t> g_differ{0};          // mode 2: systems whose counts differ
std::atomic<uint64_t> g_calls_differ{0};    // mode 2: calls with at least one difference
std::atomic<uint64_t> g_pool_evicted{0};    // pool records dropped to make room (LRU)
std::atomic<uint64_t> g_pool_evicted_pending{0}; // ... of which still held an unread request

// A copy requested for a slot in the call that dispatched it, checked at its read (two calls later).
struct Expect {
    bool valid;
    uint64_t base;    // the array the request named
    uint64_t entries; // u32 values the request covered
};

// The pending requests of one pool object. Each pool has three ring slots and at most two are
// pending at once (the dispatch of this call and the one of the previous call), so a record holds
// one Expect per ring slot. Records are keyed by the pool pointer and recycled least recently used.
// (The earlier table was 8 entries shared by all pools: it held 4 pools' worth of pending requests,
// and with more pools every new request evicted a live one before its read, so reads found none.)
struct PoolRecord {
    bool used;
    uint64_t pool;
    uint64_t stamp; // last touch, for LRU
    Expect slot[3];
    bool Pending() const {
        return slot[0].valid || slot[1].valid || slot[2].valid;
    }
};
std::mutex g_expect_mutex;
PoolRecord g_pools[kMaxPools] = {};
uint64_t g_stamp = 0; // guarded by g_expect_mutex

// Reads size bytes at a guest address through the backing view (never a direct read: the GPU page
// tracker may protect these pages). False when unmapped or when the value would cross a page.
bool ReadGuest(uint64_t address, void* out, size_t size) {
    if ((address & 0xfff) + size > 0x1000) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
    if (!p) {
        return false;
    }
    std::memcpy(out, p, size);
    return true;
}

// The guest address of a count array, from its descriptor: the same arithmetic as sub_8c1b0.
bool ArrayAddress(uint64_t descriptor, uint64_t* array) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!ReadGuest(descriptor, &lo, 4) || !ReadGuest(descriptor + 4, &hi, 4)) {
        return false;
    }
    *array = uint64_t{lo} | ((uint64_t{hi} & 0xfff) << 32);
    return true;
}

// The u32 entries a dispatch of this descriptor writes: ProcessStats 0x101933-0x10196a, a 32-bit
// imul of mult (desc+8) by (bits or 1), then shr 2. bits = desc+4 bits 16-29.
bool DispatchEntries(uint64_t descriptor, uint64_t* entries) {
    uint32_t flags = 0;
    uint32_t mult = 0;
    if (!ReadGuest(descriptor + 4, &flags, 4) || !ReadGuest(descriptor + 8, &mult, 4)) {
        return false;
    }
    uint32_t bits = (flags >> 16) & 0x3fff;
    if (bits == 0) {
        bits = 1;
    }
    *entries = static_cast<uint32_t>(mult * bits) >> 2;
    return true;
}

// Systems the caller reads: one past the highest active index below pool+0x1a8 (the loop at
// 0x101a10-0x101ddc reads only those). Scans at most kScanWords bitmap words. False when unreadable.
bool ActiveExtent(uint64_t pool, uint64_t* extent) {
    uint64_t bitmap = 0;
    uint64_t limit = 0;
    uint32_t words = 0;
    if (!ReadGuest(pool + kPoolBitmapPtr, &bitmap, 8) || !ReadGuest(pool + kPoolBitmapWords, &words, 4) ||
        !ReadGuest(pool + kPoolSystemLimit, &limit, 8)) {
        return false;
    }
    const uint64_t scan = words < kScanWords ? words : kScanWords;
    uint64_t out = 0;
    for (uint64_t w = 0; w < scan; ++w) {
        uint64_t bits = 0;
        if (!ReadGuest(bitmap + 8 * w, &bits, 8)) {
            break;
        }
        while (bits != 0) {
            const uint64_t i = w * 64 + static_cast<uint64_t>(std::countr_zero(bits));
            bits &= bits - 1;
            if (i >= limit) {
                break;
            }
            if (i + 1 > out) {
                out = i + 1;
            }
        }
    }
    *extent = out;
    return true;
}

// The record of a pool, or null. Touches its LRU stamp. Caller holds g_expect_mutex.
PoolRecord* FindLocked(uint64_t pool) {
    for (PoolRecord& r : g_pools) {
        if (r.used && r.pool == pool) {
            r.stamp = ++g_stamp;
            return &r;
        }
    }
    return nullptr;
}

// The record of a pool, created when missing: the first free record, else the least recently used
// one (counted as an eviction, with a pending request if it had one). Caller holds g_expect_mutex.
PoolRecord* RecordLocked(uint64_t pool) {
    if (PoolRecord* found = FindLocked(pool)) {
        return found;
    }
    PoolRecord* target = nullptr;
    for (PoolRecord& r : g_pools) {
        if (!r.used) {
            target = &r;
            break;
        }
    }
    if (!target) {
        for (PoolRecord& r : g_pools) {
            if (!target || r.stamp < target->stamp) {
                target = &r;
            }
        }
        g_pool_evicted.fetch_add(1, std::memory_order_relaxed);
        if (target->Pending()) {
            g_pool_evicted_pending.fetch_add(1, std::memory_order_relaxed);
        }
    }
    *target = PoolRecord{};
    target->used = true;
    target->pool = pool;
    target->stamp = ++g_stamp;
    return target;
}

// Stores the request made for (pool, slot), replacing an earlier one for the same slot.
void Remember(uint64_t pool, uint64_t slot, uint64_t base, uint64_t entries) {
    if (slot >= 3) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_expect_mutex);
    RecordLocked(pool)->slot[slot] = Expect{true, base, entries};
}

// Takes the request made for (pool, slot), if one is stored.
bool Take(uint64_t pool, uint64_t slot, Expect* out) {
    if (slot >= 3) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_expect_mutex);
    PoolRecord* r = FindLocked(pool);
    if (!r || !r->slot[slot].valid) {
        return false;
    }
    *out = r->slot[slot];
    r->slot[slot].valid = false;
    return true;
}

// The read of slot (read_slot) in this call: compare its array with the one requested two calls ago.
void CheckRead(uint64_t pool, uint64_t slot, uint64_t read_base) {
    Expect e{};
    if (!Take(pool, slot, &e)) {
        SotcRbClass::NoteStatsReadCheck(SotcRbClass::StatsCheck::kNoRequest, false);
        return;
    }
    if (e.base != read_base) {
        SotcRbClass::NoteStatsReadCheck(SotcRbClass::StatsCheck::kMismatch, false);
        return;
    }
    uint64_t extent = 0;
    const bool beyond = ActiveExtent(pool, &extent) && extent > e.entries;
    SotcRbClass::NoteStatsReadCheck(SotcRbClass::StatsCheck::kMatch, beyond);
}

// Called after the dispatch of this call wrote slot (desc): asks for its array, the one the read two
// calls later uses. Requested bytes = min(bitmap extent, pool+0x1a8, dispatch entries) * 4.
void RequestDispatched(uint64_t pool, uint64_t slot, uint64_t desc) {
    uint64_t base = 0;
    uint64_t dispatched = 0;
    uint64_t extent = 0;
    if (!ArrayAddress(desc, &base) || !base || !DispatchEntries(desc, &dispatched) ||
        !ActiveExtent(pool, &extent)) {
        return;
    }
    const uint64_t entries = extent < dispatched ? extent : dispatched;
    SotcRbClass::RequestCopy(base, entries * 4); // zero entries: counted as empty, nothing queued
    if (entries != 0) {
        Remember(pool, slot, base, entries);
        // sotc: SOTC_RB_HOST (R1): the same array becomes a host run, so this dispatch writes it in place.
        SotcRbHost::NoteStatsRange(base, entries * 4);
    }
}

HookAction OnCountRead(GuestRegs& regs, void*) {
    // Read once, after the hooks are installed (the first call comes from the game).
    static const int mode = SotcHooks::HookMode(kHook);
    if (mode == 0) {
        return HookAction::Continue;
    }

    // Only the ProcessStats call site. At entry, [rsp] is the return address.
    const uint64_t image = static_cast<uint64_t>(SotcHooks::ImageBase());
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), 8);
    if (image == 0 || ret != image + kProcessStatsReturn) {
        return HookAction::Continue;
    }

    // rdi is the descriptor of the slot being read; r13 is the pool object (ProcessStats keeps
    // param_1 there). Anything else is not the pattern we checked, so leave the call alone.
    const uint64_t pool = regs.r13;
    const uint64_t read_desc = regs.rdi;
    if (read_desc < pool + kPoolRing || (read_desc - pool - kPoolRing) % kSlotStride != 0) {
        return HookAction::Continue;
    }
    const uint64_t read_slot = (read_desc - pool - kPoolRing) / kSlotStride;
    if (read_slot >= 3) {
        return HookAction::Continue;
    }
    // The slot the next call reads, (read + 1) % 3, was dispatched in the previous call (mode 1/2
    // counters only). The slot dispatched in this call is (read + 2) % 3 (see the file comment).
    const uint64_t prev_desc = pool + kPoolRing + ((read_slot + 1) % 3) * kSlotStride;

    g_calls.fetch_add(1, std::memory_order_relaxed);

    // sotc: SOTC_RB_CLASS. The slot this call dispatched, D = (read + 2) % 3, is read in call k+2;
    // ask for its array now, after the dispatch. The read here is checked against the request
    // made two calls ago.
    // sotc: SOTC_RB_HOST (rb_host.h) needs the dispatched array too; RequestCopy itself is inert
    // unless SOTC_RB_CLASS is on, and the check of the read is class-only.
    const bool class_on = SotcRbClass::On();
    if (class_on) {
        uint64_t read_base = 0;
        if (ArrayAddress(read_desc, &read_base)) {
            CheckRead(pool, read_slot, read_base);
        }
    }
    if (class_on || SotcRbHost::On()) {
        const uint64_t dispatch_slot = (read_slot + 2) % 3;
        RequestDispatched(pool, dispatch_slot, pool + kPoolRing + dispatch_slot * kSlotStride);
    }

    uint64_t read_array = 0;
    uint64_t prev_array = 0;
    uint32_t sample = 0;
    const bool read_ok = ArrayAddress(read_desc, &read_array) && ReadGuest(read_array, &sample, 4);
    const bool prev_ok = ArrayAddress(prev_desc, &prev_array) && ReadGuest(prev_array, &sample, 4);
    if (!read_ok) {
        g_read_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    if (!prev_ok) {
        g_prev_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    if (mode != 2 || !read_ok || !prev_ok) {
        return HookAction::Continue;
    }

    // Systems the caller reads: bit i of the active bitmap, with i < pool+0x1a8 (as at 0x101a98).
    uint64_t bitmap = 0;
    uint64_t limit = 0;
    uint32_t words = 0;
    if (!ReadGuest(pool + kPoolBitmapPtr, &bitmap, 8) || !ReadGuest(pool + kPoolBitmapWords, &words, 4) ||
        !ReadGuest(pool + kPoolSystemLimit, &limit, 8)) {
        return HookAction::Continue;
    }
    if (words > kMaxWords) {
        words = static_cast<uint32_t>(kMaxWords);
    }

    uint64_t systems = 0;
    uint64_t differ = 0;
    for (uint64_t w = 0; w < words; ++w) {
        uint64_t bits = 0;
        if (!ReadGuest(bitmap + 8 * w, &bits, 8)) {
            break;
        }
        while (bits != 0) {
            const uint64_t i = w * 64 + static_cast<uint64_t>(std::countr_zero(bits));
            bits &= bits - 1;
            if (i >= limit) {
                break;
            }
            uint32_t cur = 0;
            uint32_t prev = 0;
            if (!ReadGuest(read_array + 4 * i, &cur, 4) || !ReadGuest(prev_array + 4 * i, &prev, 4)) {
                continue;
            }
            ++systems;
            if (cur != prev) {
                ++differ;
            }
        }
    }
    g_systems.fetch_add(systems, std::memory_order_relaxed);
    g_differ.fetch_add(differ, std::memory_order_relaxed);
    if (differ != 0) {
        g_calls_differ.fetch_add(1, std::memory_order_relaxed);
    }
    return HookAction::Continue;
}

} // namespace

namespace SotcProbes {

void RegisterParticlePrev() {
    SotcHooks::Handler handler;
    handler.pre = &OnCountRead;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kHook, handler);
}

void ReportParticlePrev(std::string& out, double seconds) {
    if (!SotcHooks::IsInstalled(kHook)) {
        return;
    }
    const int mode = SotcHooks::HookMode(kHook);
    const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t read_bad = g_read_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t prev_bad = g_prev_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t systems = g_systems.exchange(0, std::memory_order_relaxed);
    const uint64_t differ = g_differ.exchange(0, std::memory_order_relaxed);
    const uint64_t calls_differ = g_calls_differ.exchange(0, std::memory_order_relaxed);
    const uint64_t evicted = g_pool_evicted.exchange(0, std::memory_order_relaxed);
    const uint64_t evicted_pending = g_pool_evicted_pending.exchange(0, std::memory_order_relaxed);
    const double rate = seconds > 0 ? static_cast<double>(calls) / seconds : 0.0;

    char line[512];
    int n = std::snprintf(line, sizeof(line),
                          "SOTC PARTICLE_PREV mode=%d served=0 (observe only): ProcessStats reads %llu (%.2f/s), "
                          "read slot unreadable %llu, prev slot unreadable %llu, pool records evicted %llu "
                          "(with a pending request %llu)",
                          mode, static_cast<unsigned long long>(calls), rate,
                          static_cast<unsigned long long>(read_bad), static_cast<unsigned long long>(prev_bad),
                          static_cast<unsigned long long>(evicted),
                          static_cast<unsigned long long>(evicted_pending));
    if (mode == 2 && n > 0 && n < static_cast<int>(sizeof(line))) {
        const double pct = systems > 0 ? 100.0 * static_cast<double>(differ) / static_cast<double>(systems) : 0.0;
        std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                      ", systems compared %llu, differing %llu (%.2f%%), calls with a difference %llu",
                      static_cast<unsigned long long>(systems), static_cast<unsigned long long>(differ), pct,
                      static_cast<unsigned long long>(calls_differ));
    }
    out += line;
    out += '\n';
}

} // namespace SotcProbes
