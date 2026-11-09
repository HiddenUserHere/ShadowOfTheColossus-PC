// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: memory classes (plan step 6). See mem_classes.h. The allocator probe calls OnAlloc/OnFree
// (and OnAllocOwner); the memtype probe calls OnGranule/OnGranuleShared (probe_memtype.cpp). The draw
// thread calls Snapshot/ApplyRanges/SnapshotOwners/SnapshotGranules, RefreshLookup and the Note*
// feeders (shadow and apply modes). The guest fault path calls NoteWriteFault and NoteReadFault,
// which take no lock. Everything is gated on Mode() != 0, so with SOTC_CLASS_TRACK unset this does
// nothing.
//
// Three tables, three lookups:
//   g_cpu_only   allocator blocks classified CPU-only (OnAlloc). Lookup: g_cpu_lookup -> pool.
//   g_all        every allocator block, for the owner shadow (OnAllocOwner). Lookup: g_owner_lookup.
//   g_granules   memtype 64 KiB granules (OnGranule). Never freed. Lookup: g_granule_lookup -> index.
// A guest fault is attributed to a granule first, then to an allocator owner. The granule table is
// separate on purpose: a granule can share its base with an allocator block, and a block free must not
// erase a granule.

#include "mem_classes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SotcMemClasses {
namespace {

constexpr size_t kNameMax = 48;
constexpr uint32_t kOwnerReportLines = 24; // sotc: SOTC CLASS OWNER lines per report
constexpr uint32_t kNoPool = ~uint32_t{0}; // "none" for the trip candidate fields

// Range bounds, the same as SotcUntrack::Parse (SOTC_UNTRACK_RANGES): size 1 B to 4 GiB, base below
// 2^48. A range outside them is skipped and counted (g_skipped_ranges).
constexpr uint64_t kMaxRangeSize = 1ULL << 32;
constexpr uint64_t kMaxRangeBase = 1ULL << 48;

// RefreshLookup throttle: a rebuild needs at least kRebuildSyncs syncs AND kRebuildSpacing since the
// last one. Until then the fault lookup is older than the table (see mem_classes.h).
constexpr uint32_t kRebuildSyncs = 16;
constexpr auto kRebuildSpacing = std::chrono::milliseconds(100);

// sotc: a lock-free set of 64 KiB chunks over the guest address space (below kMaxRangeBase). Two sets use
// it: g_bind (chunks a GPU bind or a write fault touched, sticky, any thread) and g_pub (chunks of the
// published ranges in mode 1, or of the would-be ranges in mode 2; written by CollectLocked only). A 4 GiB
// region (65536 chunks, 8 KiB of bits) is claimed by a CAS on its key on first use. When no slot is left the
// overflow flag is set and nothing is published (fail closed).
constexpr uint32_t kChunkShift = 16;            // 64 KiB chunks: the granule size
constexpr uint32_t kChunkRegionShift = 16;      // 65536 chunks per region (4 GiB)
constexpr uint32_t kRegionSlots = 64;
constexpr uint32_t kRegionWords = 1024;         // 65536 bits per region

class ChunkMap {
public:
    // Calls fn(key, word, mask) for each 64-chunk word overlapping [addr, end). Callers pass addr < end.
    template <typename Fn>
    static void ForWords(uint64_t addr, uint64_t end, Fn&& fn) {
        if (end <= addr) {
            return;
        }
        const uint64_t last = (end - 1) >> kChunkShift;
        uint64_t chunk = addr >> kChunkShift;
        while (chunk <= last) {
            const uint64_t key = chunk >> kChunkRegionShift;
            const uint64_t idx = chunk & ((uint64_t{1} << kChunkRegionShift) - 1);
            const uint64_t bit = idx & 63;
            const uint64_t n = std::min<uint64_t>(
                {64 - bit, last - chunk + 1, (uint64_t{1} << kChunkRegionShift) - idx});
            const uint64_t bits = n == 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);
            fn(key, static_cast<uint32_t>(idx >> 6), bits << bit);
            chunk += n;
        }
    }

    // Sets the chunks of [addr, end). False when a region could not be claimed (overflow flag set).
    bool Set(uint64_t addr, uint64_t end) {
        bool ok = true;
        ForWords(addr, end, [&](uint64_t key, uint32_t word, uint64_t mask) {
            if (Region* r = Claim(key)) {
                r->bits[word].fetch_or(mask, std::memory_order_relaxed);
            } else {
                ok = false;
            }
        });
        return ok;
    }

    // True when any chunk of [addr, end) is set. Lock-free, no allocation.
    bool Any(uint64_t addr, uint64_t end) {
        bool hit = false;
        ForWords(addr, end, [&](uint64_t key, uint32_t word, uint64_t mask) {
            if (hit) {
                return;
            }
            const Region* r = Find(key);
            hit = r && (r->bits[word].load(std::memory_order_relaxed) & mask) != 0;
        });
        return hit;
    }

    bool Test(uint64_t addr) {
        return Any(addr, addr + 1);
    }

    // sotc: every chunk of [addr, end) is set (a missing region counts as not set).
    bool All(uint64_t addr, uint64_t end) {
        bool all = true;
        ForWords(addr, end, [&](uint64_t key, uint32_t word, uint64_t mask) {
            if (!all) {
                return;
            }
            const Region* r = Find(key);
            all = r && (r->bits[word].load(std::memory_order_relaxed) & mask) == mask;
        });
        return all;
    }

    // Clears every chunk (the regions stay claimed). Caller is the publisher (draw thread).
    void ClearAll() {
        for (Region& r : regions_) {
            if (r.key.load(std::memory_order_acquire) == 0) {
                continue;
            }
            for (auto& w : r.bits) {
                w.store(0, std::memory_order_relaxed);
            }
        }
    }

    bool Overflowed() const {
        return overflow_.load(std::memory_order_relaxed);
    }

private:
    struct Region {
        std::atomic<uint64_t> key{0}; // region number + 1; 0 = free slot
        std::array<std::atomic<uint64_t>, kRegionWords> bits{};
    };

    Region* Find(uint64_t key) {
        for (uint32_t i = 0; i < kRegionSlots; ++i) {
            Region& r = regions_[(key + i) % kRegionSlots];
            const uint64_t k = r.key.load(std::memory_order_acquire);
            if (k == key + 1) {
                return &r;
            }
            if (k == 0) {
                return nullptr; // slots are never freed, so an empty slot ends the probe
            }
        }
        return nullptr;
    }

    Region* Claim(uint64_t key) {
        for (uint32_t i = 0; i < kRegionSlots; ++i) {
            Region& r = regions_[(key + i) % kRegionSlots];
            uint64_t k = r.key.load(std::memory_order_acquire);
            if (k == 0 && r.key.compare_exchange_strong(k, key + 1, std::memory_order_acq_rel)) {
                return &r;
            }
            if (k == key + 1) {
                return &r; // k holds the key another thread stored, when the exchange failed
            }
        }
        overflow_.store(true, std::memory_order_relaxed);
        return nullptr;
    }

    std::array<Region, kRegionSlots> regions_{};
    std::atomic<bool> overflow_{false};
};

// sotc: one CPU-only allocator block. claim_ns: steady clock at OnAlloc (the warm-up starts there). owner:
// the allocator owner index from OnAllocOwner (kNoPool when it was not seen).
struct Entry {
    uint64_t size;
    uint32_t pool;
    uint32_t owner;
    uint64_t claim_ns;
};

// sotc: one live block of the all-blocks table (mode 2).
struct BlockEntry {
    uint64_t size;
    uint32_t owner; // index into g_owners
};

// sotc: one owner (allocator object). Filled on first sight; a slot past the cap keeps its first owner.
struct OwnerInfo {
    std::array<char, kNameMax> name{};
    uint64_t ptr = 0;
    int kind = 0;
    int slot = 0;
};

// sotc: one memtype granule record (granule table). Written under g_mutex when claimed, never changed.
struct GranuleRec {
    uint64_t size;
    uint32_t index; // dense index into g_granule_info and the per-granule atomics below
};

struct GranuleInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    std::atomic<uint32_t> pool{0}; // pool index; RelabelGranule may change it after the claim
};

std::mutex g_mutex;                    // guards the range tables, the pool and owner names and flags
std::map<uint64_t, Entry> g_cpu_only;  // base -> live CPU-only allocation
std::atomic<uint64_t> g_version{0};    // bumps on every change to the CPU-only and granule classes
std::atomic<bool> g_lookup_dirty{false};
std::array<std::array<char, kNameMax>, kMaxPools> g_pool_names{};
std::array<std::atomic<bool>, kMaxPools> g_pool_apply{}; // the pool is listed in SOTC_CLASS_POOLS
uint32_t g_pool_count = 0;                               // under g_mutex

// sotc: all-blocks table (every owner, CPU-only or not; mode 2 only). Allocator blocks only: granules
// are in their own table below.
std::map<uint64_t, BlockEntry> g_all;                 // under g_mutex: base -> live block
std::map<uint64_t, uint32_t> g_owner_index;           // under g_mutex: owner pointer -> index
std::array<OwnerInfo, kMaxOwners> g_owners{};         // under g_mutex
uint32_t g_owner_count = 0;                           // under g_mutex
std::atomic<uint64_t> g_owner_version{0};             // bumps on every change to g_all
std::atomic<bool> g_owner_dirty{false};
std::atomic<uint64_t> g_owner_capped{0};              // this window: blocks whose owner was past the cap
std::atomic<uint64_t> g_owner_skipped{0};             // this window: OnAllocOwner ranges outside the bounds

// sotc: granule table (memtype granules, both modes). Granules are never freed.
std::map<uint64_t, GranuleRec> g_granules;                     // under g_mutex: granule base -> record
std::array<GranuleInfo, kMaxGranuleEntries> g_granule_info{};  // written under g_mutex before the publish
uint32_t g_granule_count = 0;                                  // under g_mutex
std::atomic<bool> g_granule_dirty{false};
std::atomic<uint64_t> g_granule_skipped{0};                    // this window: table full or out of bounds
// Per granule index. excluded: 1 = never CPU_ONLY again (GPU use seen, a write or read fault, an image
// overlap, or shared with another heap). Sticky: once set it is not cleared. sampled: 1 = the draw
// thread fed GPU pages for it at least once. A granule is CPU_ONLY only when sampled and not excluded,
// so a granule nobody samples is never published.
std::array<std::atomic<uint8_t>, kMaxGranuleEntries> g_gr_excluded{};
std::array<std::atomic<uint8_t>, kMaxGranuleEntries> g_gr_sampled{};
std::array<std::atomic<uint64_t>, kMaxGranuleEntries> g_gr_gpu_pages{}; // last fed GPU page count
// GPU bind (NoteGpuBind): bound 1 = a buffer or image bind overlapped the granule (sticky, never cleared).
// bind_frame: the cp frame of the last bind. ready: 1 = past the warm-up (SOTC_CLASS_WARMUP_S after claim).
// claim_ns: steady clock (ns) when OnGranule claimed the granule.
std::array<std::atomic<uint8_t>, kMaxGranuleEntries> g_gr_bound{};
std::array<std::atomic<uint64_t>, kMaxGranuleEntries> g_gr_bind_frame{};
std::array<std::atomic<uint8_t>, kMaxGranuleEntries> g_gr_ready{};
std::array<std::atomic<uint64_t>, kMaxGranuleEntries> g_gr_claim_ns{};
std::atomic<uint64_t> g_bind_fast{0};                    // sotc: binds whose chunks were all bound already
std::atomic<uint64_t> g_bind_ranges{0};                  // this window: NoteGpuBind calls
std::atomic<uint32_t> g_forced_sync{~uint32_t{0}};       // sync of the last forced granule rebuild (NoteGpuBind)

// Lock-free lookup for the fault path. Two slots: the writer (under g_rebuild_mutex) fills the inactive
// slot once its reader count is zero, then makes it the active one. A reader counts itself on the
// slot it reads and re-checks that the slot is still active; a retry is rare and cheap. All
// accesses are seq_cst, which orders the reader count against the writer's wait. Used for the
// CPU-only table (entry index = pool), the all-blocks table (entry index = owner) and the granule
// table (entry index = granule index).
struct LookupEntry {
    uint64_t addr;
    uint64_t end;
    uint32_t index;
};

class FaultLookup {
public:
    // Finds the entry holding addr and returns its index. Lock-free, no allocation.
    bool Find(uint64_t addr, uint32_t& index) {
        const uint32_t slot = Acquire();
        const auto& items = items_[slot];
        const auto it = std::upper_bound(items.begin(), items.end(), addr,
                                         [](uint64_t a, const LookupEntry& e) { return a < e.addr; });
        bool hit = false;
        if (it != items.begin()) {
            const LookupEntry& e = *std::prev(it);
            if (addr < e.end) {
                index = e.index;
                hit = true;
            }
        }
        Release(slot);
        return hit;
    }

    // Calls fn(entry) for every entry overlapping [addr, end), in address order. The reader count is held for
    // the whole walk, so fn must not wait for a lock that the rebuild takes (fn only touches atomics here).
    template <typename Fn>
    void ForEachOverlap(uint64_t addr, uint64_t end, Fn&& fn) {
        const uint32_t slot = Acquire();
        const auto& items = items_[slot];
        auto it = std::upper_bound(items.begin(), items.end(), addr,
                                   [](uint64_t a, const LookupEntry& e) { return a < e.addr; });
        if (it != items.begin() && std::prev(it)->end > addr) {
            fn(*std::prev(it)); // the entry that starts at or before addr and reaches into the range
        }
        for (; it != items.end() && it->addr < end; ++it) {
            fn(*it);
        }
        Release(slot);
    }

    // Copies `fresh` into the inactive slot and makes it active. Caller holds g_rebuild_mutex, not
    // g_mutex. Waits for the readers of that slot (they only binary-search it) to leave.
    void Publish(const std::vector<LookupEntry>& fresh) {
        const uint32_t next = active_.load(std::memory_order_seq_cst) ^ 1u;
        while (readers_[next].load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield(); // readers only binary-search here, so this ends at once
        }
        auto& items = items_[next];
        items.assign(fresh.begin(), fresh.end()); // no allocation once the capacity is there
        active_.store(next, std::memory_order_seq_cst);
    }

private:
    // Enters the active slot as a reader (re-checked, so a retry is rare). Pair with Release.
    uint32_t Acquire() {
        for (;;) {
            const uint32_t slot = active_.load(std::memory_order_seq_cst);
            readers_[slot].fetch_add(1, std::memory_order_seq_cst);
            if (active_.load(std::memory_order_seq_cst) == slot) {
                return slot;
            }
            readers_[slot].fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    void Release(uint32_t slot) {
        readers_[slot].fetch_sub(1, std::memory_order_seq_cst);
    }

    std::array<std::vector<LookupEntry>, 2> items_{};
    std::atomic<uint32_t> active_{0};
    std::array<std::atomic<uint32_t>, 2> readers_{};
};

FaultLookup g_cpu_lookup;     // CPU-only ranges -> pool
FaultLookup g_owner_lookup;   // all live allocator blocks -> owner (mode 2)
FaultLookup g_granule_lookup; // memtype granules -> granule index
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_faults{}; // write faults per pool, this window
// sotc: write faults per pool, this window, on chunks no GPU bind ever touched (granule: g_gr_bound 0; block:
// g_bind clear). Counted whether or not the chunk is a candidate. Upper bound of the faults the publish could
// remove: a GPU read without a bind is not visible here (see the report line "est. faults avoided").
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_unbound_faults{};

// sotc: publish rules (CollectLocked). g_bind: chunks bound by a GPU bind (ObtainBuffer, image binds) or
// excluded by a write fault on a candidate block, or NoteExcluded. Sticky. g_pub: chunks of the published
// (mode 1) or would-be (mode 2) ranges, rebuilt by every CollectLocked.
ChunkMap g_bind;
ChunkMap g_pub;
std::vector<Range> g_last_pub;   // under g_mutex: the list of the last publish (ApplyRanges)
std::vector<Range> g_retracked;  // under g_mutex: ranges the last publish left out (TakeRetracked)
std::atomic<bool> g_retrack_pending{false};   // a bind hit a published range: the next publish is due now
std::atomic<uint64_t> g_warm_deadline_ns{0};  // earliest ready time of a block still warming (0 = none)
std::atomic<uint64_t> g_bind_hits{0};         // this window: binds that hit a published (would-be) range
std::atomic<uint64_t> g_bind_hits_total{0};   // session
std::atomic<uint64_t> g_avoidable_faults{0};  // this window: write faults on candidates before publish
std::atomic<uint64_t> g_pub_faults{0};        // this window: write faults on published (would-be) ranges
std::atomic<uint64_t> g_cand_skipped{0};      // this window: candidates left out (chunk map full)
std::atomic<uint64_t> g_cand_ranges{0};       // last collect: candidate ranges (mode 2 reports them)
std::atomic<uint64_t> g_cand_bytes{0};        // last collect: candidate bytes
std::atomic<uint64_t> g_trip_logs{0};         // bind-hit log lines written (capped)
std::array<uint64_t, kMaxOwners> g_pub_owner_bytes{};  // under g_mutex: published bytes per owner (last collect)
std::array<uint64_t, kMaxOwners> g_pub_owner_ranges{}; // under g_mutex: published blocks per owner
// Mode 2 collect throttle (CollectShadowIfDue): the draw thread reads and writes g_collect_next; a bind
// hit from any thread (NoteExcluded) only sets g_shadow_due.
std::chrono::steady_clock::time_point g_collect_next{};
std::atomic<bool> g_shadow_due{false};

// Per owner, this window (mode 2). Writes and reads are counted on the fault path.
std::array<std::atomic<uint64_t>, kMaxOwners> g_owner_writes{};
std::array<std::atomic<uint64_t>, kMaxOwners> g_owner_reads{};
// Per owner, from the draw thread: the last GPU-page count and image-range count, and the window peak
// of the GPU-page count (reset to the last value when reported).
std::array<std::atomic<uint64_t>, kMaxOwners> g_owner_gpu_last{};
std::array<std::atomic<uint64_t>, kMaxOwners> g_owner_gpu_peak{};
std::array<std::atomic<uint64_t>, kMaxOwners> g_owner_images{};

// Lookup rebuild state. g_rebuild_mutex serializes RefreshLookup and is never held while g_mutex is
// taken inside it, except for the short copy; the slot write and the reader wait run without g_mutex.
std::mutex g_rebuild_mutex;
std::atomic<uint32_t> g_sync_calls{0};       // RefreshLookup calls (every sync)
uint32_t g_last_rebuild_sync = 0;            // under g_rebuild_mutex
std::chrono::steady_clock::time_point g_last_rebuild{}; // under g_rebuild_mutex
std::vector<LookupEntry> g_staging;          // under g_rebuild_mutex: the copied table, capacity kept
std::vector<LookupEntry> g_owner_staging;    // under g_rebuild_mutex: the copied all-blocks table
std::vector<LookupEntry> g_granule_staging;  // under g_rebuild_mutex: the copied granule table
std::atomic<uint64_t> g_lookup_rebuilds{0};  // this window
std::atomic<uint64_t> g_skipped_ranges{0};   // this window: OnAlloc ranges outside the bounds

// Mode 1 state, set by the draw thread and read by Report.
std::atomic<uint64_t> g_apply_ranges{0};
std::atomic<uint64_t> g_apply_bytes{0};
std::atomic<uint64_t> g_apply_publishes{0}; // this window
std::atomic<bool> g_apply_tripped{false};
std::atomic<uint64_t> g_apply_faults_untracked{0};

// Mode 1, per heap (granule pools). Published bytes and ranges are those handed to the publisher by the
// last ApplyRanges call (buffer_cache may still drop a range for an image overlap or the byte cap).
// Trips are counted per heap, and attributed to the heap of the last CPU_ONLY candidate fault.
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_pub_bytes{};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_pub_ranges{};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_trips{};
std::atomic<uint64_t> g_trips_unattributed{0};
// The last write fault on a CPU_ONLY candidate (a published range, or a granule that was CPU_ONLY).
// Diagnostic only: written by the fault path without a lock, read by the trip log.
std::atomic<uint64_t> g_cand_addr{0};
std::atomic<uint32_t> g_cand_pool{kNoPool};
std::atomic<uint32_t> g_cand_granule{kNoPool}; // granule index, or kNoPool for an allocator block

// Shadow window: the last totals the draw thread reported, plus window peaks.
std::atomic<uint64_t> g_shadow_ranges{0};
std::atomic<uint64_t> g_shadow_bytes{0};
std::atomic<uint64_t> g_shadow_gpu_last{0};
std::atomic<uint64_t> g_shadow_gpu_peak{0};
std::atomic<uint64_t> g_shadow_samples{0};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_peak{}; // window peak per pool
// Ranges of a pool that overlap a texture-cache image (last count). Images do not set region GPU
// bits, so these are counted apart and never untracked in mode 1.
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_image_ranges{};
std::array<uint64_t, kMaxPools> g_pool_session_peak{};      // session peak per pool (Report only)

void RaisePeak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t cur = peak.load(std::memory_order_relaxed);
    while (value > cur && !peak.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

// SOTC_CLASS_POOLS, read once. Unset, empty or "all": every pool is an apply pool (ApplyAllPools). An
// explicit comma-separated list names the apply pools. Granule heaps are named "memtype:<label>" (the label
// is the one the report prints), e.g. "memtype:GameSingleFrame". A label of two heap globals sharing a slot
// is comma-joined.
bool ApplyAllPools() {
    static const bool all = [] {
        const char* env = std::getenv("SOTC_CLASS_POOLS");
        return !env || !env[0] || std::strcmp(env, "all") == 0 || std::strcmp(env, "*") == 0;
    }();
    return all;
}

// The explicit list of SOTC_CLASS_POOLS (empty when the setting is "all" or unset).
const std::vector<std::string>& ApplyPoolNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        if (ApplyAllPools()) {
            return out;
        }
        const char* env = std::getenv("SOTC_CLASS_POOLS");
        const std::string text = env;
        size_t begin = 0;
        while (begin <= text.size()) {
            size_t end = text.find(',', begin);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string item = text.substr(begin, end - begin);
            const size_t first = item.find_first_not_of(' ');
            const size_t last = item.find_last_not_of(' ');
            if (first != std::string::npos) {
                out.push_back(item.substr(first, last - first + 1));
            }
            begin = end + 1;
        }
        return out;
    }();
    return names;
}

bool IsApplyPool(const char* key) {
    if (ApplyAllPools()) {
        // The per-frame arenas stay tracked: the GPU reads them every frame (SOTC_PC_RING, NoteExcluded).
        return std::strstr(key, "TaggedArena") == nullptr;
    }
    for (const std::string& name : ApplyPoolNames()) {
        if (name == key) {
            return true;
        }
    }
    return false;
}

// Index of a pool name, added on first use. Caller holds g_mutex.
uint32_t PoolIndex(const char* name) {
    const char* key = (name && name[0]) ? name : "?";
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        if (std::strncmp(g_pool_names[i].data(), key, kNameMax) == 0) {
            return i;
        }
    }
    if (g_pool_count == kMaxPools) {
        return kMaxPools - 1; // full: the last slot collects the rest
    }
    std::snprintf(g_pool_names[g_pool_count].data(), kNameMax, "%s", key);
    g_pool_apply[g_pool_count].store(IsApplyPool(key), std::memory_order_relaxed);
    return g_pool_count++;
}

// Index of an owner (allocator object), added on first sight. Caller holds g_mutex. Owners past the
// cap share the last slot, which keeps the name of the owner that took it first.
uint32_t OwnerIndex(uint64_t owner, int kind, const char* pool, int slot, bool& capped) {
    capped = false;
    const auto found = g_owner_index.find(owner);
    if (found != g_owner_index.end()) {
        return found->second;
    }
    if (g_owner_count == kMaxOwners) {
        capped = true;
        return kMaxOwners - 1;
    }
    const uint32_t index = g_owner_count++;
    OwnerInfo& info = g_owners[index];
    std::snprintf(info.name.data(), kNameMax, "%s", (pool && pool[0]) ? pool : "?");
    info.ptr = owner;
    info.kind = kind;
    info.slot = slot;
    g_owner_index.emplace(owner, index);
    return index;
}

// OwnerClass: the proposed class of an owner from its window counts (see the report line).
// A write fault can only happen on a page the port protects, and it protects only pages the GPU
// reads (bound buffers and images), so write faults mean "GPU reads it" (first live run, 1 Nov:
// the kind-1 PageAllocator had 1,685 write faults/s and no GPU pages, and was wrongly CPU_ONLY).
const char* OwnerClass(uint64_t gpu_pages, uint64_t writes, uint64_t reads, uint64_t images) {
    if (gpu_pages != 0 || reads != 0) {
        return "GPU_WRITTEN";
    }
    if (writes != 0 || images != 0) {
        return "GPU_READ";
    }
    return "CPU_ONLY";
}

// A granule is CPU_ONLY when the draw thread sampled it, it is past its warm-up, and no GPU use was ever
// seen (see g_gr_excluded) and no bind overlapped it (see g_gr_bound).
bool GranuleCpuOnly(uint32_t index) {
    return g_gr_sampled[index].load(std::memory_order_relaxed) != 0 &&
           g_gr_ready[index].load(std::memory_order_relaxed) != 0 &&
           g_gr_excluded[index].load(std::memory_order_relaxed) == 0 &&
           g_gr_bound[index].load(std::memory_order_relaxed) == 0;
}

// Marks a granule as never CPU_ONLY again. The class changed, so the version bumps and the next
// ApplyRanges (the publish) drops the granule from the untracked list. Lock-free; any thread.
void MarkGranuleExcluded(uint32_t index) {
    if (index < kMaxGranuleEntries &&
        g_gr_excluded[index].exchange(1, std::memory_order_relaxed) == 0) {
        g_version.fetch_add(1, std::memory_order_release);
    }
}

// Steady clock in nanoseconds (the warm-up clock).
uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// SOTC_CLASS_WARMUP_S, read once: seconds after its claim before a granule may be CPU_ONLY (default 20).
uint64_t WarmupNs() {
    static const uint64_t ns = [] {
        const char* v = std::getenv("SOTC_CLASS_WARMUP_S");
        double s = (v && v[0]) ? std::strtod(v, nullptr) : 20.0;
        if (!(s >= 0.0)) {
            s = 0.0; // also catches NaN
        }
        s = std::min(s, 1.0e6);
        return static_cast<uint64_t>(s * 1.0e9);
    }();
    return ns;
}

// Sets the warm-up flag of a granule once it is WarmupNs past its claim. The flag change bumps the version
// so the next publish includes the granule. Called from the draw thread's sample feed (about every 250 ms).
void NoteWarmup(uint32_t index, uint64_t now_ns) {
    if (g_gr_ready[index].load(std::memory_order_relaxed) != 0) {
        return;
    }
    if (now_ns < g_gr_claim_ns[index].load(std::memory_order_relaxed) + WarmupNs()) {
        return;
    }
    if (g_gr_ready[index].exchange(1, std::memory_order_relaxed) == 0) {
        g_version.fetch_add(1, std::memory_order_release);
    }
}

// Marks a granule GPU-bound (sticky). The first bind changes the class, so the version bumps and the next
// publish drops it. The frame is stored on every bind (last bind wins). Lock-free; draw thread.
void MarkGranuleBound(uint32_t index, uint64_t cp_frame) {
    if (index >= kMaxGranuleEntries) {
        return;
    }
    g_gr_bind_frame[index].store(cp_frame, std::memory_order_relaxed);
    if (g_gr_bound[index].load(std::memory_order_relaxed) == 0 &&
        g_gr_bound[index].exchange(1, std::memory_order_relaxed) == 0) {
        g_version.fetch_add(1, std::memory_order_release);
    }
}

// Rebuilds only the granule lookup, if the granule table changed since its last rebuild. NoteGpuBind calls
// it (draw thread, at most once per sync) so that a bind of a granule claimed since the last RefreshLookup
// still finds it. Takes g_rebuild_mutex, then g_mutex for the copy; the caller must hold neither.
void RebuildGranuleLookupNow() {
    std::lock_guard<std::mutex> rebuild(g_rebuild_mutex);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_granule_dirty.load(std::memory_order_acquire)) {
            return;
        }
        g_granule_dirty.store(false, std::memory_order_release);
        g_granule_staging.clear();
        g_granule_staging.reserve(g_granules.size());
        for (const auto& [base, rec] : g_granules) {
            g_granule_staging.push_back(LookupEntry{base, base + rec.size, rec.index});
        }
    }
    g_granule_lookup.Publish(g_granule_staging);
}

// sotc: a bind hit a published range (mode 1) or a would-be range (mode 2). Nothing is cleared here: the
// chunks stay set until the next CollectLocked (which also leaves the bound range out), so a chunk shared
// with another published range keeps its mark. Mode 1 makes the publish due now (RetrackPending). Draw
// thread only (it reads g_pool_names unlocked, as the trip log does).
void HitPublished(uint64_t addr, uint64_t end, uint32_t pool, uint64_t cp_frame) {
    g_bind_hits.fetch_add(1, std::memory_order_relaxed);
    g_bind_hits_total.fetch_add(1, std::memory_order_relaxed);
    if (pool < kMaxPools) {
        g_pool_trips[pool].fetch_add(1, std::memory_order_relaxed);
    }
    g_version.fetch_add(1, std::memory_order_release);
    const bool apply = Mode() == 1;
    if (apply) {
        g_retrack_pending.store(true, std::memory_order_release);
    } else {
        g_shadow_due.store(true, std::memory_order_relaxed); // mode 2: recollect at the next sync
    }
    if (g_trip_logs.fetch_add(1, std::memory_order_relaxed) < 20) {
        std::fprintf(stderr, "SOTC CLASS TRIP (bind): cp frame %llu bound 0x%llx..0x%llx in %s, %s\n",
                     static_cast<unsigned long long>(cp_frame), static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(end), g_pool_names[pool < kMaxPools ? pool : 0].data(),
                     apply ? "published range retracked, publish due now"
                           : "would-be range (shadow, nothing changed)");
    }
}

// sotc: every published entry (granule or allocator block) that the bind [addr, end) overlaps is a hit.
// Draw thread. Both lookups are walked lock-free; the callbacks only touch atomics.
// lo/hi: the union [lo, hi) of the full spans of the published entries hit (not just the bound part).
bool CheckBindHits(uint64_t addr, uint64_t end, uint64_t cp_frame, uint64_t& lo, uint64_t& hi) {
    bool hit = false;
    auto note = [&hit, &lo, &hi](uint64_t a, uint64_t b) {
        if (!hit) {
            lo = a;
            hi = b;
            hit = true;
        } else {
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
    };
    g_granule_lookup.ForEachOverlap(addr, end, [cp_frame, &note](const LookupEntry& e) {
        if (g_pub.Any(e.addr, e.end)) {
            HitPublished(e.addr, e.end, g_granule_info[e.index].pool.load(std::memory_order_relaxed), cp_frame);
            note(e.addr, e.end);
        }
    });
    g_cpu_lookup.ForEachOverlap(addr, end, [cp_frame, &note](const LookupEntry& e) {
        if (g_pub.Any(e.addr, e.end)) {
            HitPublished(e.addr, e.end, e.index, cp_frame);
            note(e.addr, e.end);
        }
    });
    return hit;
}

// sotc: marks [addr, end) bound (sticky), after checking it for hits. Lock-free. Returns true when the range
// hit a published (mode 1) or would-be (mode 2) range; then hit_lo/hit_hi (when given) get the full span
// union of the hit entries. A failed claim sets the chunk map's overflow flag.
bool MarkChunksBound(uint64_t addr, uint64_t end, uint64_t cp_frame, uint64_t* hit_lo,
                     uint64_t* hit_hi) {
    const uint64_t cend = std::min(end, kMaxRangeBase);
    if (addr >= cend) {
        return false;
    }
    uint64_t lo = 0, hi = 0;
    const bool hit = g_pub.Any(addr, cend) && CheckBindHits(addr, cend, cp_frame, lo, hi);
    g_bind.Set(addr, cend);
    if (hit) {
        if (hit_lo) {
            *hit_lo = lo;
        }
        if (hit_hi) {
            *hit_hi = hi;
        }
    }
    return hit;
}

// sotc: the publish rule, the one place a range becomes a candidate. Caller holds g_mutex. Re-checked on
// every call (ApplyRanges on each publish, CollectShadowIfDue in mode 2). Out gets the candidates:
//  - an allocator block of an apply pool, past its warm-up (WarmupNs after its claim), with no chunk of
//    its span bound or excluded (g_bind);
//  - a memtype granule of an apply heap that is CPU_ONLY by class (GranuleCpuOnly) and whose chunk is free.
// Every candidate sets its chunks in g_pub. A chunk map overflow publishes nothing. Also sets the per-heap
// and per-owner totals, the candidate totals and the warm-up deadline (the next time a block turns ready).
void CollectLocked(uint64_t now, std::vector<Range>& out, bool publish) {
    out.clear();
    g_pub.ClearAll();
    std::array<uint64_t, kMaxPools> bytes{}, ranges{};
    std::array<uint64_t, kMaxOwners> owner_bytes{}, owner_ranges{};
    uint64_t warm_deadline = 0;
    uint64_t skipped = 0;
    const uint64_t warm = WarmupNs();
    const bool unsafe = g_bind.Overflowed() || g_pub.Overflowed();
    for (const auto& [base, e] : g_cpu_only) {
        if (unsafe || !g_pool_apply[e.pool].load(std::memory_order_relaxed)) {
            continue;
        }
        const uint64_t ready = e.claim_ns + warm;
        if (now < ready) {
            if (warm_deadline == 0 || ready < warm_deadline) {
                warm_deadline = ready;
            }
            continue;
        }
        if (g_bind.Any(base, base + e.size)) {
            continue;
        }
        if (!g_pub.Set(base, base + e.size)) {
            ++skipped;
            continue;
        }
        out.push_back(Range{base, e.size, e.pool});
        bytes[e.pool] += e.size;
        ranges[e.pool] += 1;
        if (e.owner < kMaxOwners) {
            owner_bytes[e.owner] += e.size;
            owner_ranges[e.owner] += 1;
        }
    }
    for (const auto& [base, rec] : g_granules) {
        const uint32_t pool = g_granule_info[rec.index].pool.load(std::memory_order_relaxed);
        if (unsafe || !g_pool_apply[pool].load(std::memory_order_relaxed) ||
            !GranuleCpuOnly(rec.index)) {
            continue;
        }
        if (g_bind.Any(base, base + rec.size)) {
            continue;
        }
        if (!g_pub.Set(base, base + rec.size)) {
            ++skipped;
            continue;
        }
        out.push_back(Range{base, rec.size, pool});
        bytes[pool] += rec.size;
        ranges[pool] += 1;
    }
    uint64_t total_ranges = 0;
    uint64_t total_bytes = 0;
    for (uint32_t pool = 0; pool < kMaxPools; ++pool) {
        g_pool_pub_bytes[pool].store(bytes[pool], std::memory_order_relaxed);
        g_pool_pub_ranges[pool].store(ranges[pool], std::memory_order_relaxed);
        total_ranges += ranges[pool];
        total_bytes += bytes[pool];
    }
    for (uint32_t i = 0; i < kMaxOwners; ++i) {
        g_pub_owner_bytes[i] = owner_bytes[i];
        g_pub_owner_ranges[i] = owner_ranges[i];
    }
    g_cand_ranges.store(total_ranges, std::memory_order_relaxed);
    g_cand_bytes.store(total_bytes, std::memory_order_relaxed);
    g_cand_skipped.fetch_add(skipped, std::memory_order_relaxed);
    g_warm_deadline_ns.store(warm_deadline, std::memory_order_relaxed);
    if (publish) {
        // Ranges of the previous publish that this one leaves out were untracked until now: their CPU writes
        // were not seen, so the caller marks them CPU-modified (TakeRetracked). Identity is (addr, size).
        std::vector<std::pair<uint64_t, uint64_t>> fresh;
        fresh.reserve(out.size());
        for (const Range& r : out) {
            fresh.emplace_back(r.addr, r.size);
        }
        std::sort(fresh.begin(), fresh.end());
        g_retracked.clear();
        for (const Range& r : g_last_pub) {
            if (!std::binary_search(fresh.begin(), fresh.end(), std::make_pair(r.addr, r.size))) {
                g_retracked.push_back(r);
            }
        }
        g_last_pub = out;
    }
}

// sotc: mode 2. The would-be list is recollected at most every 250 ms (sooner after a bind hit, which
// resets g_collect_next). Draw thread; takes g_mutex only here.
void CollectShadowIfDue() {
    const auto now_tp = std::chrono::steady_clock::now();
    if (!g_shadow_due.exchange(false, std::memory_order_relaxed) && now_tp < g_collect_next) {
        return;
    }
    g_collect_next = now_tp + std::chrono::milliseconds(250);
    std::vector<Range> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    CollectLocked(NowNs(), out, false);
}

// sotc: the top 10 allocator owners by published bytes (last collect), one report line. Caller holds g_mutex.
void AppendPublishedOwners(std::string& out) {
    std::array<uint32_t, kMaxOwners> order{};
    for (uint32_t i = 0; i < kMaxOwners; ++i) {
        order[i] = i;
    }
    std::partial_sort(order.begin(), order.begin() + 10, order.end(), [](uint32_t a, uint32_t b) {
        return g_pub_owner_bytes[a] > g_pub_owner_bytes[b];
    });
    std::string text = "SOTC CLASS owners published (top 10 by MiB):";
    bool any = false;
    for (uint32_t k = 0; k < 10; ++k) {
        const uint32_t i = order[k];
        if (g_pub_owner_bytes[i] == 0) {
            break;
        }
        char item[200];
        std::snprintf(item, sizeof item, "%s %s 0x%llx kind %d: %.1f MiB in %llu blocks",
                      any ? ";" : "", g_owners[i].name.data(),
                      static_cast<unsigned long long>(g_owners[i].ptr), g_owners[i].kind,
                      g_pub_owner_bytes[i] / 1048576.0,
                      static_cast<unsigned long long>(g_pub_owner_ranges[i]));
        text += item;
        any = true;
    }
    if (any) {
        out += text;
        out += "\n";
    }
}

// The last CPU_ONLY candidate fault (diagnostic, for the trip log). granule = kNoPool for a block.
void RecordCandidate(uint64_t addr, uint32_t pool, uint32_t granule) {
    g_cand_addr.store(addr, std::memory_order_relaxed);
    g_cand_pool.store(pool, std::memory_order_relaxed);
    g_cand_granule.store(granule, std::memory_order_relaxed);
}

// Per-pool lines of the report: live CPU-only ranges and MiB, the write faults of the window, and
// in shadow mode the GPU-page peaks (window and session). Appends one line when any pool shows.
void AppendPools(std::string& out, double secs, const std::array<uint64_t, kMaxPools>& faults,
                 const std::array<uint64_t, kMaxPools>& unbound, bool shadow) {
    std::array<uint64_t, kMaxPools> count{}, pool_bytes{};
    std::string text = "SOTC CLASS pools:"; // fault counts come from the lookup, which may lag a sync
    bool any = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [base, e] : g_cpu_only) {
        count[e.pool] += 1;
        pool_bytes[e.pool] += e.size;
    }
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        uint64_t w = 0;
        if (shadow) {
            w = g_pool_peak[i].exchange(0, std::memory_order_relaxed);
            g_pool_session_peak[i] = std::max(g_pool_session_peak[i], w);
        }
        if (count[i] == 0 && g_pool_session_peak[i] == 0 && faults[i] == 0) {
            continue;
        }
        char item[256];
        int len = std::snprintf(item, sizeof item, "%s %s %llu (%.1f MiB)", any ? ";" : "",
                                g_pool_names[i].data(), static_cast<unsigned long long>(count[i]),
                                pool_bytes[i] / 1048576.0);
        if (shadow) {
            len += std::snprintf(item + len, sizeof item - len, " gpu %llu/%llu",
                                 static_cast<unsigned long long>(w),
                                 static_cast<unsigned long long>(g_pool_session_peak[i]));
        }
        len += std::snprintf(item + len, sizeof item - len, " faults %llu/s (never-bound %llu/s)",
                             static_cast<unsigned long long>(faults[i] / secs + 0.5),
                             static_cast<unsigned long long>(unbound[i] / secs + 0.5));
        if (const uint64_t img = g_pool_image_ranges[i].load(std::memory_order_relaxed)) {
            std::snprintf(item + len, sizeof item - len, " image-ranges %llu",
                          static_cast<unsigned long long>(img));
        }
        text += item;
        any = true;
    }
    if (any) {
        out += text;
        out += shadow ? " (gpu = window peak / session peak; want 0)\n" : "\n";
    }
}

// sotc: one line per memtype heap (granule pool): granules, CPU_ONLY granules, the last GPU-page count,
// the published bytes of the last publish, and the trips attributed to the heap. Also the granule
// table's skip count of the window and the trips that had no candidate fault.
void AppendHeaps(std::string& out) {
    struct HeapRow {
        uint64_t granules = 0;
        uint64_t bytes = 0;
        uint64_t cpu_only = 0;
        uint64_t gpu_pages = 0;
        uint64_t bound = 0;   // granules with a GPU bind (sticky)
        uint64_t warming = 0; // granules not yet past the warm-up
    };
    std::array<HeapRow, kMaxPools> rows{};
    uint32_t total = 0;
    uint64_t skipped = 0;
    uint64_t binds = 0;
    uint64_t bind_fast = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        total = g_granule_count;
        for (uint32_t i = 0; i < g_granule_count; ++i) {
            const GranuleInfo& info = g_granule_info[i];
            HeapRow& row = rows[info.pool.load(std::memory_order_relaxed)];
            row.granules += 1;
            row.bytes += info.size;
            row.cpu_only += GranuleCpuOnly(i) ? 1 : 0; // after the bind and warm-up filters
            row.gpu_pages += g_gr_gpu_pages[i].load(std::memory_order_relaxed);
            row.bound += g_gr_bound[i].load(std::memory_order_relaxed) != 0 ? 1 : 0;
            row.warming += g_gr_ready[i].load(std::memory_order_relaxed) == 0 ? 1 : 0;
        }
        skipped = g_granule_skipped.exchange(0, std::memory_order_relaxed);
        binds = g_bind_ranges.exchange(0, std::memory_order_relaxed);
        bind_fast = g_bind_fast.exchange(0, std::memory_order_relaxed);
        for (uint32_t pool = 0; pool < g_pool_count; ++pool) {
            if (rows[pool].granules == 0) {
                continue;
            }
            char line[480];
            std::snprintf(
                line, sizeof line,
                "SOTC CLASS heap %s: granules %llu (%.1f MiB), bound %llu, warming %llu, CPU_ONLY %llu, "
                "gpu pages %llu (last), published %.1f MiB in %llu ranges, trips %llu\n",
                g_pool_names[pool].data(), static_cast<unsigned long long>(rows[pool].granules),
                rows[pool].bytes / 1048576.0, static_cast<unsigned long long>(rows[pool].bound),
                static_cast<unsigned long long>(rows[pool].warming),
                static_cast<unsigned long long>(rows[pool].cpu_only),
                static_cast<unsigned long long>(rows[pool].gpu_pages),
                g_pool_pub_bytes[pool].load(std::memory_order_relaxed) / 1048576.0,
                static_cast<unsigned long long>(g_pool_pub_ranges[pool].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_pool_trips[pool].load(std::memory_order_relaxed)));
            out += line;
        }
    }
    if (total != 0 || skipped != 0) {
        char line[260];
        std::snprintf(line, sizeof line,
                      "SOTC CLASS granules: %u claimed, %llu bind ranges this window (%llu already bound, "
                      "fast path), %llu skipped this window (table full or outside the bounds), trips without "
                      "a candidate fault %llu\n",
                      total, static_cast<unsigned long long>(binds), static_cast<unsigned long long>(bind_fast),
                      static_cast<unsigned long long>(skipped),
                      static_cast<unsigned long long>(g_trips_unattributed.load(std::memory_order_relaxed)));
        out += line;
    }
}

// sotc: the SOTC CLASS OWNER lines (mode 2): every owner with live blocks, ranked by activity (GPU
// pages, write and read faults per second, image ranges), the top kOwnerReportLines printed.
void AppendOwners(std::string& out, double secs,
                  const std::array<uint64_t, kMaxOwners>& writes,
                  const std::array<uint64_t, kMaxOwners>& reads) {
    struct Row {
        uint32_t index;
        uint64_t blocks;
        uint64_t bytes;
        uint64_t gpu;
        uint64_t images;
        uint64_t activity;
        std::array<char, kNameMax> name;
        uint64_t ptr;
        int kind;
        int slot;
    };
    std::vector<Row> rows;
    uint64_t capped = 0;
    uint64_t skipped = 0;
    uint64_t total_blocks = 0;
    uint64_t total_bytes = 0;
    uint32_t owners_used = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        owners_used = g_owner_count;
        std::array<uint64_t, kMaxOwners> blocks{}, bytes{};
        for (const auto& [base, e] : g_all) {
            blocks[e.owner] += 1;
            bytes[e.owner] += e.size;
            total_blocks += 1;
            total_bytes += e.size;
        }
        for (uint32_t i = 0; i < g_owner_count; ++i) {
            if (blocks[i] == 0) {
                continue;
            }
            const uint64_t last = g_owner_gpu_last[i].load(std::memory_order_relaxed);
            const uint64_t peak = g_owner_gpu_peak[i].exchange(last, std::memory_order_relaxed);
            Row r{};
            r.index = i;
            r.blocks = blocks[i];
            r.bytes = bytes[i];
            r.gpu = std::max(peak, last);
            r.images = g_owner_images[i].load(std::memory_order_relaxed);
            r.activity = writes[i] + reads[i] + r.gpu + r.images;
            r.name = g_owners[i].name;
            r.ptr = g_owners[i].ptr;
            r.kind = g_owners[i].kind;
            r.slot = g_owners[i].slot;
            rows.push_back(r);
        }
        capped = g_owner_capped.exchange(0, std::memory_order_relaxed);
        skipped = g_owner_skipped.exchange(0, std::memory_order_relaxed);
    }
    if (rows.empty()) {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.activity != b.activity) {
            return a.activity > b.activity;
        }
        return a.bytes > b.bytes;
    });
    char head[256];
    std::snprintf(head, sizeof head,
                  "SOTC CLASS OWNERS: %zu owners with blocks (%u slots), %llu blocks, %.1f MiB; "
                  "blocks past the owner cap %llu, skipped %llu; top %u by activity (gpu pages + "
                  "writes/s + reads/s + image ranges)\n",
                  rows.size(), owners_used, static_cast<unsigned long long>(total_blocks),
                  total_bytes / 1048576.0, static_cast<unsigned long long>(capped),
                  static_cast<unsigned long long>(skipped), kOwnerReportLines);
    out += head;
    const size_t shown = std::min<size_t>(rows.size(), kOwnerReportLines);
    for (size_t k = 0; k < shown; ++k) {
        const Row& r = rows[k];
        const uint64_t w = writes[r.index];
        const uint64_t rd = reads[r.index];
        char line[400];
        std::snprintf(line, sizeof line,
                      "SOTC CLASS OWNER 0x%llx %s kind %d slot %d: live %.1f MiB (%llu blocks), "
                      "gpu pages %llu, writes %.1f/s, reads %.1f/s, image ranges %llu, class %s\n",
                      static_cast<unsigned long long>(r.ptr), r.name.data(), r.kind, r.slot,
                      r.bytes / 1048576.0, static_cast<unsigned long long>(r.blocks),
                      static_cast<unsigned long long>(r.gpu), w / secs, rd / secs,
                      static_cast<unsigned long long>(r.images),
                      OwnerClass(r.gpu, w, rd, r.images));
        out += line;
    }
}

// Logs the untrack self-check trip once (the apply state went from not tripped to tripped), with the
// last CPU_ONLY candidate fault as the owner that tripped it. Draw thread.
void LogTrip() {
    const uint32_t pool = g_cand_pool.load(std::memory_order_relaxed);
    const uint32_t granule = g_cand_granule.load(std::memory_order_relaxed);
    const uint64_t addr = g_cand_addr.load(std::memory_order_relaxed);
    if (pool == kNoPool) {
        g_trips_unattributed.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "SOTC CLASS TRIP: untrack self-check tripped; no CPU_ONLY candidate "
                             "fault was recorded\n");
        return;
    }
    g_pool_trips[pool].fetch_add(1, std::memory_order_relaxed);
    std::string name;
    uint64_t granule_base = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        name = g_pool_names[pool].data();
        if (granule != kNoPool && granule < g_granule_count) {
            granule_base = g_granule_info[granule].base;
        }
    }
    if (granule != kNoPool) {
        std::fprintf(stderr, "SOTC CLASS TRIP: untrack self-check tripped by a write fault at 0x%llx "
                             "in granule 0x%llx of %s (released)\n",
                     static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(granule_base), name.c_str());
    } else {
        std::fprintf(stderr, "SOTC CLASS TRIP: untrack self-check tripped by a write fault at 0x%llx "
                             "in allocator pool %s (released)\n",
                     static_cast<unsigned long long>(addr), name.c_str());
    }
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CLASS_TRACK");
        if (!v || !v[0]) {
            return 0;
        }
        const int n = std::atoi(v);
        if (n == 1) {
            // Mode 1 publishes through SotcUntrack's auto list, and the CP ahead gate turns the
            // untracking off (as Parse()).
            const char* ahead = std::getenv("SOTC_CP_AHEAD"); // sotc: default 2 (Liverpool::CpAheadMode)
            if (!(ahead && ahead[0]) || std::atoi(ahead) != 0) {
                std::fprintf(stderr, "SOTC CLASS: SOTC_CLASS_TRACK=1 refused, SOTC_CP_AHEAD is "
                                     "set; class mode stays off\n");
                return 0;
            }
            // Mode 1 untracks the CPU-only ranges only: the static SOTC_UNTRACK_RANGES list is not read.
            const char* static_ranges = std::getenv("SOTC_UNTRACK_RANGES");
            if (static_ranges && static_ranges[0]) {
                std::fprintf(stderr, "SOTC CLASS: SOTC_UNTRACK_RANGES is ignored in class mode "
                                     "(SOTC_CLASS_TRACK=1); the CPU-only ranges are the untrack list\n");
            }
        }
        return (n == 1 || n == 2) ? n : 0;
    }();
    return mode;
}

void OnAlloc(uint64_t base, uint64_t size, bool cpu_only, const char* pool) {
    if (Mode() == 0 || !cpu_only || base == 0) {
        return;
    }
    // Same bounds as SotcUntrack::Parse, and no wrap of base + size. Counted in the report.
    if (size == 0 || size > kMaxRangeSize || base >= kMaxRangeBase || base > UINT64_MAX - size) {
        g_skipped_ranges.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cpu_only.insert_or_assign(base, Entry{size, PoolIndex(pool), kNoPool, NowNs()});
    g_version.fetch_add(1, std::memory_order_release);
    g_lookup_dirty.store(true, std::memory_order_release);
}

// sotc: modes 1 and 2. Same bounds as OnAlloc; skipped ranges are counted apart (OnAlloc counts the
// CPU-only ones, which the same allocation also feeds, so one counter would count them twice). Mode 2
// fills the all-blocks table. Mode 1 only names the owner of the CPU-only block (OnAlloc runs first, from
// probe_alloc.cpp), for the "owners published" report line.
void OnAllocOwner(uint64_t base, uint64_t size, uint64_t owner, int kind, const char* pool,
                  int slot) {
    const int mode = Mode();
    if (mode == 0 || base == 0) {
        return;
    }
    if (size == 0 || size > kMaxRangeSize || base >= kMaxRangeBase || base > UINT64_MAX - size) {
        g_owner_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    bool capped = false;
    const uint32_t index = OwnerIndex(owner, kind, pool, slot, capped);
    if (capped) {
        g_owner_capped.fetch_add(1, std::memory_order_relaxed);
    }
    if (mode == 1) {
        const auto it = g_cpu_only.find(base);
        if (it != g_cpu_only.end() && it->second.size == size) {
            it->second.owner = index;
        }
        return;
    }
    g_all.insert_or_assign(base, BlockEntry{size, index});
    g_owner_version.fetch_add(1, std::memory_order_release);
    g_owner_dirty.store(true, std::memory_order_release);
}

std::vector<std::pair<uint64_t, uint64_t>> CollectHostServe(const char* pool_name, uint64_t& version) {
    std::vector<std::pair<uint64_t, uint64_t>> runs;
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_acquire);
    uint32_t pool = kMaxPools;
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        if (std::strncmp(g_pool_names[i].data(), pool_name, kNameMax) == 0) {
            pool = i;
            break;
        }
    }
    if (pool == kMaxPools) {
        return runs;
    }
    for (uint32_t i = 0; i < g_granule_count; ++i) {
        const GranuleInfo& info = g_granule_info[i];
        if (info.pool.load(std::memory_order_relaxed) != pool ||
            g_gr_bound[i].load(std::memory_order_relaxed) == 0 ||
            g_gr_excluded[i].load(std::memory_order_relaxed) != 0) {
            continue;
        }
        runs.emplace_back(info.base, info.size);
    }
    std::sort(runs.begin(), runs.end());
    size_t out = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (out != 0 && runs[out - 1].first + runs[out - 1].second == runs[i].first) {
            runs[out - 1].second += runs[i].second;
        } else {
            runs[out++] = runs[i];
        }
    }
    runs.resize(out);
    return runs;
}

// sotc: SOTC_PC_RING8_HEAPS_MODE=2 (see CollectHostGranules in mem_classes.h). Same pool lookup as CollectHostServe.
std::vector<HostGranule> CollectHostGranules(const char* pool_name, uint64_t& version) {
    std::vector<HostGranule> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_acquire);
    uint32_t pool = kMaxPools;
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        if (std::strncmp(g_pool_names[i].data(), pool_name, kNameMax) == 0) {
            pool = i;
            break;
        }
    }
    if (pool == kMaxPools) {
        return out;
    }
    for (uint32_t i = 0; i < g_granule_count; ++i) {
        const GranuleInfo& info = g_granule_info[i];
        if (info.pool.load(std::memory_order_relaxed) != pool) {
            continue;
        }
        out.push_back(HostGranule{info.base, info.size,
                                  g_gr_bound[i].load(std::memory_order_relaxed) != 0,
                                  g_gr_excluded[i].load(std::memory_order_relaxed) == 0});
    }
    std::sort(out.begin(), out.end(),
              [](const HostGranule& a, const HostGranule& b) { return a.addr < b.addr; });
    return out;
}

// sotc: a memtype granule (probe_memtype.cpp), in both modes. Granules are never freed and never enter
// the allocator tables, so an allocator block with the same base is unaffected. Same bounds as OnAlloc;
// a granule past kMaxGranuleEntries is skipped and counted.
void OnGranule(uint64_t base, uint64_t size, const char* pool) {
    if (Mode() == 0 || base == 0) {
        return;
    }
    if (size == 0 || size > kMaxRangeSize || base >= kMaxRangeBase || base > UINT64_MAX - size) {
        g_granule_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_granules.count(base) != 0) {
        return;
    }
    if (g_granule_count == kMaxGranuleEntries) {
        g_granule_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t index = g_granule_count++;
    GranuleInfo& info = g_granule_info[index];
    info.base = base;
    info.size = size;
    info.pool.store(PoolIndex(pool), std::memory_order_relaxed);
    g_gr_claim_ns[index].store(NowNs(), std::memory_order_relaxed); // the warm-up starts at the claim
    g_granules.emplace(base, GranuleRec{size, index});
    // sotc: a granule claimed over chunks the GPU already bound is bound now: NoteGpuBind's fast path
    // skips binds whose chunks are all bound, so a later bind would not mark it.
    if (g_bind.Any(base, base + size)) {
        MarkGranuleBound(index, 0);
    }
    g_version.fetch_add(1, std::memory_order_release);
    g_granule_dirty.store(true, std::memory_order_release);
}

// sotc: a granule was touched by a second heap (probe_memtype.cpp, first foreign touch). Its GPU use is
// not known per heap, so it is never CPU_ONLY. base is the granule base given to OnGranule.
bool RelabelGranule(uint64_t base, const char* to_pool) {
    if (Mode() == 0 || !to_pool || !to_pool[0]) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_granules.find(base);
    if (it == g_granules.end()) {
        return false;
    }
    GranuleInfo& info = g_granule_info[it->second.index];
    const uint32_t target = PoolIndex(to_pool);
    if (info.pool.load(std::memory_order_relaxed) == target) {
        return false; // already in that pool: nothing changed
    }
    info.pool.store(target, std::memory_order_relaxed);
    g_version.fetch_add(1, std::memory_order_release); // the next publish re-checks the granule under its new pool
    return true;
}

void OnGranuleShared(uint64_t base) {
    if (Mode() == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_granules.find(base);
    if (it != g_granules.end()) {
        MarkGranuleExcluded(it->second.index);
    }
}

void OnFree(uint64_t base) {
    if (Mode() == 0 || base == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cpu_only.erase(base) != 0) {
        g_version.fetch_add(1, std::memory_order_release);
        g_lookup_dirty.store(true, std::memory_order_release);
    }
    if (g_all.erase(base) != 0) {
        g_owner_version.fetch_add(1, std::memory_order_release);
        g_owner_dirty.store(true, std::memory_order_release);
    }
}

std::vector<Range> Snapshot(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_relaxed);
    std::vector<Range> out;
    out.reserve(g_cpu_only.size());
    for (const auto& [base, e] : g_cpu_only) {
        out.push_back(Range{base, e.size, e.pool});
    }
    return out;
}

std::vector<OwnerRange> SnapshotOwners(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_owner_version.load(std::memory_order_relaxed);
    std::vector<OwnerRange> out;
    out.reserve(g_all.size());
    for (const auto& [base, e] : g_all) {
        out.push_back(OwnerRange{base, e.size, e.owner});
    }
    return out;
}

std::vector<GranuleRange> SnapshotGranules(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_granule_count;
    std::vector<GranuleRange> out;
    out.reserve(g_granules.size());
    for (const auto& [base, rec] : g_granules) {
        out.push_back(GranuleRange{base, rec.size, rec.index, g_granule_info[rec.index].pool.load(std::memory_order_relaxed)});
    }
    return out;
}

std::vector<Range> ApplyRanges(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The version is read before the rules run, so a change during the collect publishes again next time.
    version = g_version.load(std::memory_order_relaxed);
    std::vector<Range> out;
    CollectLocked(NowNs(), out, true); // every candidate is re-checked here (warm-up, binds, excluded, overflow)
    g_retrack_pending.store(false, std::memory_order_release); // this publish carries the retrack
    return out;
}

uint64_t Version() {
    return g_version.load(std::memory_order_acquire);
}

void RefreshLookup() {
    g_sync_calls.fetch_add(1, std::memory_order_relaxed);
    if (Mode() == 2) {
        CollectShadowIfDue(); // the would-be list (draw thread; throttled)
    }
    // A block that finished its warm-up changes the list: the version bumps so the next publish takes it.
    const uint64_t deadline = g_warm_deadline_ns.load(std::memory_order_relaxed);
    if (deadline != 0 && NowNs() >= deadline) {
        g_warm_deadline_ns.store(0, std::memory_order_relaxed);
        g_version.fetch_add(1, std::memory_order_release);
    }
    if (!g_lookup_dirty.load(std::memory_order_acquire) &&
        !g_owner_dirty.load(std::memory_order_acquire) &&
        !g_granule_dirty.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> rebuild(g_rebuild_mutex);
    const uint32_t sync = g_sync_calls.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (sync - g_last_rebuild_sync < kRebuildSyncs || now - g_last_rebuild < kRebuildSpacing) {
        return; // throttled: the dirty flags stay set and the next sync tries again
    }
    bool cpu_rebuilt = false;
    bool owner_rebuilt = false;
    bool granule_rebuilt = false;
    {
        // Only the copies run under g_mutex. Each flag is cleared under it too, so an OnAlloc or
        // OnFree after this copy sets it again.
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_lookup_dirty.load(std::memory_order_acquire)) {
            g_lookup_dirty.store(false, std::memory_order_release);
            g_staging.clear();
            g_staging.reserve(g_cpu_only.size());
            for (const auto& [base, e] : g_cpu_only) {
                g_staging.push_back(LookupEntry{base, base + e.size, e.pool});
            }
            cpu_rebuilt = true;
        }
        if (g_owner_dirty.load(std::memory_order_acquire)) {
            g_owner_dirty.store(false, std::memory_order_release);
            g_owner_staging.clear();
            g_owner_staging.reserve(g_all.size());
            for (const auto& [base, e] : g_all) {
                g_owner_staging.push_back(LookupEntry{base, base + e.size, e.owner});
            }
            owner_rebuilt = true;
        }
        if (g_granule_dirty.load(std::memory_order_acquire)) {
            g_granule_dirty.store(false, std::memory_order_release);
            g_granule_staging.clear();
            g_granule_staging.reserve(g_granules.size());
            for (const auto& [base, rec] : g_granules) {
                g_granule_staging.push_back(LookupEntry{base, base + rec.size, rec.index});
            }
            granule_rebuilt = true;
        }
    }
    if (!cpu_rebuilt && !owner_rebuilt && !granule_rebuilt) {
        return;
    }
    if (cpu_rebuilt) {
        g_cpu_lookup.Publish(g_staging);
    }
    if (owner_rebuilt) {
        g_owner_lookup.Publish(g_owner_staging);
    }
    if (granule_rebuilt) {
        g_granule_lookup.Publish(g_granule_staging);
    }
    g_last_rebuild_sync = sync;
    g_last_rebuild = now;
    g_lookup_rebuilds.fetch_add(1, std::memory_order_relaxed);
}

// Attribution order: a granule first (the finer owner), then the CPU-only allocator range (pool
// faults), then the allocator block owner (mode 2). A granule hit does not also count the block owner.
void NoteWriteFault(uint64_t addr) {
    uint32_t granule = 0;
    if (g_granule_lookup.Find(addr, granule)) {
        const uint32_t pool = g_granule_info[granule].pool.load(std::memory_order_relaxed);
        g_pool_faults[pool].fetch_add(1, std::memory_order_relaxed);
        if (g_gr_bound[granule].load(std::memory_order_relaxed) == 0) {
            g_pool_unbound_faults[pool].fetch_add(1, std::memory_order_relaxed);
        }
        if (GranuleCpuOnly(granule)) {
            RecordCandidate(addr, pool, granule);
            g_avoidable_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_pub.Test(addr)) {
            g_pub_faults.fetch_add(1, std::memory_order_relaxed);
        }
        // A write fault on a protected page means the GPU reads it: the granule is never CPU_ONLY again.
        MarkGranuleExcluded(granule);
        return;
    }
    uint32_t pool = 0;
    if (g_cpu_lookup.Find(addr, pool)) {
        g_pool_faults[pool].fetch_add(1, std::memory_order_relaxed);
        if (!g_bind.Test(addr)) {
            g_pool_unbound_faults[pool].fetch_add(1, std::memory_order_relaxed);
        }
        if (g_pool_apply[pool].load(std::memory_order_relaxed)) {
            RecordCandidate(addr, pool, kNoPool);
            if (!g_bind.Test(addr)) {
                g_avoidable_faults.fetch_add(1, std::memory_order_relaxed); // a candidate: publish takes it off
            }
            if (g_pub.Test(addr)) {
                g_pub_faults.fetch_add(1, std::memory_order_relaxed);
            }
            // Same rule as a granule: a write fault on a candidate block excludes its chunk for good, so the
            // next publish leaves the whole block out (CollectLocked tests the block's full span).
            g_bind.Set(addr, addr + 1);
        }
    }
    if (Mode() == 2) {
        uint32_t owner = 0;
        if (g_owner_lookup.Find(addr, owner)) {
            g_owner_writes[owner].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NoteReadFault(uint64_t addr) {
    if (Mode() != 2) {
        return;
    }
    uint32_t granule = 0;
    if (g_granule_lookup.Find(addr, granule)) {
        MarkGranuleExcluded(granule); // a read fault on a granule: the GPU writes it
        return;
    }
    uint32_t owner = 0;
    if (g_owner_lookup.Find(addr, owner)) {
        g_owner_reads[owner].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteGranuleGpuPages(uint32_t index, uint64_t gpu_pages) {
    if (index >= kMaxGranuleEntries) {
        return;
    }
    if (g_gr_sampled[index].exchange(1, std::memory_order_relaxed) == 0) {
        g_version.fetch_add(1, std::memory_order_release); // a new sample can make the granule CPU_ONLY
    }
    if (g_gr_ready[index].load(std::memory_order_relaxed) == 0) {
        NoteWarmup(index, NowNs()); // the clock is read only until the granule is ready
    }
    g_gr_gpu_pages[index].store(gpu_pages, std::memory_order_relaxed);
    if (gpu_pages != 0) {
        MarkGranuleExcluded(index);
    }
}

// sotc: see mem_classes.h. Draw thread. Walks the granules overlapping the range (binary search, then the
// entries in address order), so the cost is O(log n + granules touched) with no allocation.
bool NoteGpuBind(uint64_t addr, uint64_t size, uint64_t cp_frame, uint64_t* hit_lo,
                 uint64_t* hit_hi) {
    if (Mode() == 0 || size == 0) {
        return false;
    }
    const uint64_t end = (UINT64_MAX - addr < size) ? UINT64_MAX : addr + size; // no wrap
    g_bind_ranges.fetch_add(1, std::memory_order_relaxed);
    // sotc: fast path (~1M binds/s): every chunk already bound means every granule over it is marked bound
    // (marked by that earlier bind, or at its claim by OnGranule), and no published range can overlap it
    // (publish needs a bind-free span; bound is sticky). Nothing to do.
    if (g_bind.All(addr, end)) {
        g_bind_fast.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A granule claimed since the last lookup rebuild is not in the lookup yet. Rebuild it here, at most once
    // per sync, so the bind is not lost. (Draw thread: RefreshLookup runs on it too, so no lock is held here.)
    const uint32_t sync = g_sync_calls.load(std::memory_order_relaxed);
    if (g_granule_dirty.load(std::memory_order_acquire) &&
        g_forced_sync.load(std::memory_order_relaxed) != sync) {
        g_forced_sync.store(sync, std::memory_order_relaxed);
        RebuildGranuleLookupNow();
    }
    g_granule_lookup.ForEachOverlap(addr, end, [cp_frame](const LookupEntry& e) {
        MarkGranuleBound(e.index, cp_frame);
    });
    // sotc: the chunks of the bind (allocator blocks too). A bind into a published range is a hit first.
    return MarkChunksBound(addr, end, cp_frame, hit_lo, hit_hi);
}

void TakeRetracked(std::vector<Range>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.swap(g_retracked);
    g_retracked.clear();
}

void NoteExcluded(uint64_t addr, uint64_t size) {
    if (Mode() == 0 || size == 0) {
        return;
    }
    const uint64_t end = (UINT64_MAX - addr < size) ? UINT64_MAX : addr + size; // no wrap
    MarkChunksBound(addr, end, 0, nullptr, nullptr);
}

bool RetrackPending() {
    return g_retrack_pending.load(std::memory_order_acquire);
}

void NoteGranuleImage(uint32_t index, bool overlaps) {
    if (overlaps) {
        MarkGranuleExcluded(index);
    }
}

void NotePublish(uint64_t ranges, uint64_t bytes) {
    g_apply_ranges.store(ranges, std::memory_order_relaxed);
    g_apply_bytes.store(bytes, std::memory_order_relaxed);
    g_apply_publishes.fetch_add(1, std::memory_order_relaxed);
}

void NoteApplyState(bool tripped, uint64_t faults_untracked) {
    const bool was = g_apply_tripped.exchange(tripped, std::memory_order_relaxed);
    g_apply_faults_untracked.store(faults_untracked, std::memory_order_relaxed);
    if (tripped && !was) {
        LogTrip();
    }
}

void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages) {
    g_shadow_ranges.store(ranges, std::memory_order_relaxed);
    g_shadow_bytes.store(cpu_only_bytes, std::memory_order_relaxed);
    g_shadow_gpu_last.store(gpu_pages, std::memory_order_relaxed);
    g_shadow_samples.fetch_add(1, std::memory_order_relaxed);
    RaisePeak(g_shadow_gpu_peak, gpu_pages);
}

void NoteImageRanges(uint32_t pool, uint64_t ranges) {
    if (pool < kMaxPools) {
        g_pool_image_ranges[pool].store(ranges, std::memory_order_relaxed);
    }
}

void NoteShadowPool(uint32_t pool, uint64_t gpu_pages) {
    if (pool < kMaxPools) {
        RaisePeak(g_pool_peak[pool], gpu_pages);
    }
}

void NoteShadowOwner(uint32_t owner, uint64_t gpu_pages) {
    if (owner < kMaxOwners) {
        g_owner_gpu_last[owner].store(gpu_pages, std::memory_order_relaxed);
        RaisePeak(g_owner_gpu_peak[owner], gpu_pages);
    }
}

void NoteOwnerImageRanges(uint32_t owner, uint64_t ranges) {
    if (owner < kMaxOwners) {
        g_owner_images[owner].store(ranges, std::memory_order_relaxed);
    }
}

void Report(std::string& out, double seconds) {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    const double secs = seconds > 0 ? seconds : 1.0;
    // The window's write faults, read and reset in both modes before any early return.
    std::array<uint64_t, kMaxPools> faults{};
    std::array<uint64_t, kMaxPools> unbound{};
    uint64_t unbound_total = 0;
    for (uint32_t i = 0; i < kMaxPools; ++i) {
        faults[i] = g_pool_faults[i].exchange(0, std::memory_order_relaxed);
        unbound[i] = g_pool_unbound_faults[i].exchange(0, std::memory_order_relaxed);
        if (g_pool_apply[i].load(std::memory_order_relaxed)) {
            unbound_total += unbound[i]; // only pools the publish may take (not the per-frame arenas)
        }
    }
    std::array<uint64_t, kMaxOwners> owner_writes{};
    std::array<uint64_t, kMaxOwners> owner_reads{};
    for (uint32_t i = 0; i < kMaxOwners; ++i) {
        owner_writes[i] = g_owner_writes[i].exchange(0, std::memory_order_relaxed);
        owner_reads[i] = g_owner_reads[i].exchange(0, std::memory_order_relaxed);
    }
    // sotc: publish-side counters of the window, both modes (read and reset before any early return).
    const uint64_t avoidable = g_avoidable_faults.exchange(0, std::memory_order_relaxed);
    const uint64_t pub_faults = g_pub_faults.exchange(0, std::memory_order_relaxed);
    const uint64_t hits = g_bind_hits.exchange(0, std::memory_order_relaxed);
    const uint64_t cand_skipped = g_cand_skipped.exchange(0, std::memory_order_relaxed);
    const bool unsafe = g_bind.Overflowed() || g_pub.Overflowed();
    // Table counters of the window, read and reset here. Shown only when something happened.
    const uint64_t rebuilds = g_lookup_rebuilds.exchange(0, std::memory_order_relaxed);
    const uint64_t skipped = g_skipped_ranges.exchange(0, std::memory_order_relaxed);
    if (rebuilds != 0 || skipped != 0) {
        char table[200];
        std::snprintf(table, sizeof table,
                      "SOTC CLASS table: %llu lookup rebuilds, %llu ranges skipped (size 0, size "
                      "> 4 GiB, base >= 2^48 or overflow)\n",
                      static_cast<unsigned long long>(rebuilds),
                      static_cast<unsigned long long>(skipped));
        out += table;
    }
    AppendHeaps(out); // per memtype heap, both modes (also zeroes the window's granule skip count)
    if (mode == 1) {
        const uint64_t publishes = g_apply_publishes.exchange(0, std::memory_order_relaxed);
        const bool tripped = g_apply_tripped.load(std::memory_order_relaxed);
        char line[320];
        std::snprintf(
            line, sizeof line,
            "SOTC CLASS apply: %llu ranges (%.1f MiB) untracked, %llu publishes this window, %s, "
            "faults untracked %llu\n",
            static_cast<unsigned long long>(g_apply_ranges.load(std::memory_order_relaxed)),
            g_apply_bytes.load(std::memory_order_relaxed) / 1048576.0,
            static_cast<unsigned long long>(publishes),
            tripped ? "TRIPPED (released, no more publishes)" : "not tripped",
            static_cast<unsigned long long>(g_apply_faults_untracked.load(std::memory_order_relaxed)));
        out += line;
        char pub[400];
        std::snprintf(
            pub, sizeof pub,
            "SOTC CLASS publish: avoidable write faults %llu/s (candidates before publish), faults on "
            "published ranges %llu, bind hits (trips) %llu this window, %llu total, candidates left out "
            "%llu, retrack %s%s\n",
            static_cast<unsigned long long>(avoidable / secs + 0.5),
            static_cast<unsigned long long>(pub_faults), static_cast<unsigned long long>(hits),
            static_cast<unsigned long long>(g_bind_hits_total.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(cand_skipped), RetrackPending() ? "due" : "idle",
            unsafe ? ", CHUNK MAP FULL (nothing published)" : "");
        out += pub;
        char est[320];
        std::snprintf(
            est, sizeof est,
            "SOTC CLASS avoid: est. faults avoided <= %llu/s (write faults on never-GPU-bound chunks, all "
            "pools; GPU reads without a bind are not seen), published now %.1f MiB, faults on published "
            "ranges %llu\n",
            static_cast<unsigned long long>(unbound_total / secs + 0.5),
            g_apply_bytes.load(std::memory_order_relaxed) / 1048576.0,
            static_cast<unsigned long long>(pub_faults));
        out += est;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            AppendPublishedOwners(out);
        }
        AppendPools(out, secs, faults, unbound, false);
        return;
    }
    // Shadow mode.
    const uint64_t samples = g_shadow_samples.exchange(0, std::memory_order_relaxed);
    if (samples == 0) {
        return; // the draw thread took no sample this window
    }
    const uint64_t ranges = g_shadow_ranges.load(std::memory_order_relaxed);
    const uint64_t bytes = g_shadow_bytes.load(std::memory_order_relaxed);
    const uint64_t last = g_shadow_gpu_last.load(std::memory_order_relaxed);
    const uint64_t peak = g_shadow_gpu_peak.exchange(0, std::memory_order_relaxed);
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC CLASS shadow: %llu CPU-only ranges (%.1f MiB), GPU pages inside them: "
                  "%llu now, %llu peak (want 0), %llu samples\n",
                  static_cast<unsigned long long>(ranges), bytes / 1048576.0,
                  static_cast<unsigned long long>(last), static_cast<unsigned long long>(peak),
                  static_cast<unsigned long long>(samples));
    out += line;
    AppendPools(out, secs, faults, unbound, true);
    {
        char would[420];
        std::snprintf(
            would, sizeof would,
            "SOTC CLASS would-publish: %llu ranges (%.1f MiB), avoidable write faults %llu/s "
            "(candidates before publish), faults on would-be ranges %llu, bind hits %llu this window, "
            "%llu total; nothing changed%s\n",
            static_cast<unsigned long long>(g_cand_ranges.load(std::memory_order_relaxed)),
            g_cand_bytes.load(std::memory_order_relaxed) / 1048576.0,
            static_cast<unsigned long long>(avoidable / secs + 0.5),
            static_cast<unsigned long long>(pub_faults), static_cast<unsigned long long>(hits),
            static_cast<unsigned long long>(g_bind_hits_total.load(std::memory_order_relaxed)),
            unsafe ? " (chunk map full: list empty)" : "");
        out += would;
        std::lock_guard<std::mutex> lock(g_mutex);
        AppendPublishedOwners(out);
    }
    AppendOwners(out, secs, owner_writes, owner_reads);
}

} // namespace SotcMemClasses
