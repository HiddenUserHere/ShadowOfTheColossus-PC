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
// sotc: SOTC_FLAT_NOTES: the base index and the pending FIFOs exist as std::map (the default) and/or as vectors
// sorted by base with the same keys (FlatMode). The walk of a write is then a walk over contiguous memory.
// sotc: SOTC_NATIVE_COPIES_PAGES (PagesMode): the entries of both indexes are also kept under the 64 KiB granules
// they cover (g_gr_base, g_gr_pend), with a lock-free count per hashed granule (g_filter_counts). A written bind
// then looks at the granules it touches only: no entry there means no tree walk and no lock. The overlaps found are
// the ones the map walk finds (the same entries, the same order, the same pops).
#include "native_copies.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
#include "video_core/sotc_write_journal.h" // sotc: SOTC_WRITE_JOURNAL (page filter, journal counters)

namespace SotcNativeCopies {
namespace {

constexpr uint64_t kAlign = 256;
// Every record takes at least kAlign bytes of the ring, so the ring bounds the record count first.
constexpr size_t kMaxRecords = kRingBytes / kAlign;
constexpr size_t kKeyDepth = 8; // registrations kept per key in the key index
constexpr uint64_t kNeededFrames = 3; // a clients' lookup reaches this many frames back (masks: kOldestFrames)

using Clock = std::chrono::steady_clock;

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
    bool cf_base;      // sotc: SOTC_NATIVE_COPIES_FILTER: counted as the copying entry of its base
    bool cf_pend;      // sotc: SOTC_NATIVE_COPIES_FILTER: counted as a waiting single-write registration
};

// One copy a write takes: the overlap [lo, hi) of the write with the registration e.
struct Overlap {
    Entry* e;
    uint64_t lo;
    uint64_t hi;
};

std::mutex g_mutex; // everything below
std::deque<Entry> g_entries; // registration order; references stay valid across push_back / pop_front
std::map<uint64_t, Entry*> g_by_base;                       // newest registration per base
std::map<uint64_t, std::deque<Entry*>> g_pending;           // single-write registrations waiting for their write
std::unordered_map<uint64_t, std::vector<Entry*>> g_by_key; // newest first, at most kKeyDepth

// sotc: SOTC_FLAT_NOTES: the same two indexes as flat vectors sorted by base (see FlatMode). A slot exists for a
// base only while it has an entry, as a map key does; a pending slot never has an empty queue.
struct FlatBase {
    uint64_t base;
    Entry* entry;
};
struct FlatPending {
    uint64_t base;
    std::deque<Entry*> waiting;
};
std::vector<FlatBase> g_flat_base;
std::vector<FlatPending> g_flat_pending;

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
    // sotc: SOTC_FLAT_NOTES window counters (Report prints them when the switch is set)
    std::atomic<uint64_t> flat_writes{0};    // write walks (NoteWrittenBind past its early return)
    std::atomic<uint64_t> flat_map_ns{0};    // time of the std::map walks (modes 0 and 2)
    std::atomic<uint64_t> flat_vec_ns{0};    // time of the flat walks (modes 1 and 2)
    std::atomic<uint64_t> flat_mismatch{0};  // mode 2: answers of the flat indexes that differ from the map's
    // sotc: SOTC_NATIVE_COPIES_PAGES window counters (Report prints them when the switch is set)
    std::atomic<uint64_t> pages_writes{0};     // written binds that reached the page index
    std::atomic<uint64_t> pages_empty{0};      // of those, no entry under their granules (lock-free answer)
    std::atomic<uint64_t> pages_checked{0};    // candidate entries the page walk examined
    std::atomic<uint64_t> pages_mismatch{0};   // mode 2: writes whose page walk found other overlaps than the map walk
    // sotc: SOTC_NATIVE_COPIES_FILTER window counters (Report prints them when the switch is set)
    std::atomic<uint64_t> cf_writes{0};        // written binds that asked the copy filter
    std::atomic<uint64_t> cf_empty{0};         // of those, no copy-capable entry under their granules
    std::atomic<uint64_t> cf_mismatch{0};      // mode 2: an empty answer whose map walk found an overlap (must be 0)
};
Counters g_c;

// sotc: SOTC_WRITE_JOURNAL. Hashed page counts of the live entries (every page of [base, base + bytes)): Register adds
// an entry, PruneLocked removes it when it leaves g_entries. A write whose pages all have a zero count overlaps no entry.
// Owned by the registration lock (g_mutex); the draw thread reads it without the lock (MayTouch).
SotcWriteJournal::PageCounts g_filter;

// sotc: SOTC_FLAT_NOTES (read once). 0: the std::maps only, the flat vectors are not touched (today's code).
// 1: the flat vectors only. 2: both are kept up to date; every answer comes from the std::map, the flat answer is
// computed too and every difference is counted (flat_mismatch, which must read 0).
int FlatMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_FLAT_NOTES");
        return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
    }();
    return mode;
}
bool UseMap() {
    return FlatMode() != 1;
}
bool UseFlat() {
    return FlatMode() != 0;
}

void Mismatch() {
    g_c.flat_mismatch.fetch_add(1, std::memory_order_relaxed);
}

// The answer of a query both indexes can give: the flat one in mode 1, the map's in mode 0 and 2 (mode 2 counts a
// disagreement). In mode 0 the flat argument is the empty vector's answer and is not used.
template <class T>
T Pick(T old_answer, T flat_answer) {
    if (FlatMode() == 1) {
        return flat_answer;
    }
    if (FlatMode() == 2 && old_answer != flat_answer) {
        Mismatch();
    }
    return old_answer;
}

void MaxInto(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t prev = a.load(std::memory_order_relaxed);
    while (v > prev && !a.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

uint64_t NsSince(Clock::time_point t0) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
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

// Index of the first slot whose base is not below base (slots are sorted by base).
template <class Slot>
size_t LowerIndex(const std::vector<Slot>& slots, uint64_t base) {
    return static_cast<size_t>(std::lower_bound(slots.begin(), slots.end(), base,
                                                [](const Slot& s, uint64_t b) { return s.base < b; }) -
                               slots.begin());
}

// The entry of the flat base index at base (nullptr when none). Caller holds g_mutex.
Entry* FlatFindBase(uint64_t base) {
    const size_t i = LowerIndex(g_flat_base, base);
    return i < g_flat_base.size() && g_flat_base[i].base == base ? g_flat_base[i].entry : nullptr;
}

void FlatSetBase(uint64_t base, Entry* e) {
    const size_t i = LowerIndex(g_flat_base, base);
    if (i < g_flat_base.size() && g_flat_base[i].base == base) {
        g_flat_base[i].entry = e;
    } else {
        g_flat_base.insert(g_flat_base.begin() + static_cast<std::ptrdiff_t>(i), FlatBase{base, e});
    }
}

void FlatEraseBase(uint64_t base) {
    const size_t i = LowerIndex(g_flat_base, base);
    if (i < g_flat_base.size() && g_flat_base[i].base == base) {
        g_flat_base.erase(g_flat_base.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// Index of the pending queue of base, or g_flat_pending.size() when it has none.
size_t FlatPendingAt(uint64_t base) {
    const size_t i = LowerIndex(g_flat_pending, base);
    return i < g_flat_pending.size() && g_flat_pending[i].base == base ? i : g_flat_pending.size();
}

void FlatPendingPush(uint64_t base, Entry* e) {
    size_t i = FlatPendingAt(base);
    if (i == g_flat_pending.size()) {
        i = LowerIndex(g_flat_pending, base);
        g_flat_pending.insert(g_flat_pending.begin() + static_cast<std::ptrdiff_t>(i), FlatPending{base, {}});
    }
    g_flat_pending[i].waiting.push_back(e);
}

void FlatPendingRemove(const Entry* e) {
    const size_t i = FlatPendingAt(e->base);
    if (i == g_flat_pending.size()) {
        return;
    }
    auto& q = g_flat_pending[i].waiting;
    for (size_t k = 0; k < q.size(); ++k) {
        if (q[k] == e) {
            q.erase(q.begin() + static_cast<std::ptrdiff_t>(k));
            break;
        }
    }
    if (q.empty()) {
        g_flat_pending.erase(g_flat_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// sotc: SOTC_NATIVE_COPIES_PAGES (read once). 0: the std::maps only (today's code). 1: the page index answers the
// writes: a write with no entry under its granules returns before the lock, the others take the page walk instead of
// the map walk. 2: the page walk is read next to the map walk (before the map pops) and the overlaps of the two are
// compared (verify mismatches must read 0). The page index needs the std::maps (FlatMode 0): with SOTC_FLAT_NOTES
// on, PagesOn is false and the writes take the flat walks as before.
int PagesMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_NATIVE_COPIES_PAGES");
        return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
    }();
    return mode;
}
bool PagesOn() {
    static const bool on = PagesMode() != 0 && FlatMode() == 0;
    return on;
}

// sotc: SOTC_NATIVE_COPIES_PAGES. A granule is 64 KiB of guest address space. g_gr_base maps a granule to the entries
// of g_by_base whose [base, base + bytes) it covers, g_gr_pend the same for the entries of the g_pending queues. An
// entry is in an index exactly while the std::map holds it there (the same add and remove points), and under all of
// its granules or none. Owned by the registration lock (g_mutex).
constexpr uint64_t kGranuleShift = 16;
// A write spanning more granules than this is not scanned granule by granule (the index is walked instead).
constexpr uint64_t kMaxScanGranules = 1024;
// The lock-free filter: a hashed slot counts the (entry, granule) pairs of both indexes that hash to it. Zero in a
// slot means no entry covers any granule of that slot. Written under g_mutex, read without it (PagesFilterEmpty).
constexpr unsigned kFilterBits = 16;
using GranuleIndex = std::unordered_map<uint64_t, std::vector<Entry*>>;
GranuleIndex g_gr_base;
GranuleIndex g_gr_pend;
std::atomic<uint32_t> g_filter_counts[size_t{1} << kFilterBits];
std::vector<Entry*> g_pg_base; // scratch of CollectPages (guarded by g_mutex)
std::vector<Entry*> g_pg_pend;

size_t FilterSlot(uint64_t granule) {
    return static_cast<size_t>((granule * 0x9E3779B97F4A7C15ull) >> (64 - kFilterBits));
}

// Adds e under each of its granules. Caller holds g_mutex.
void GranuleAdd(GranuleIndex& index, Entry* e) {
    const uint64_t g0 = e->base >> kGranuleShift;
    const uint64_t g1 = (e->base + e->bytes - 1) >> kGranuleShift;
    for (uint64_t g = g0; g <= g1; ++g) {
        index[g].push_back(e);
        g_filter_counts[FilterSlot(g)].fetch_add(1, std::memory_order_release);
    }
}

// Removes e from its granules when it is in the index (an entry not in it changes nothing). Caller holds g_mutex.
void GranuleRemove(GranuleIndex& index, Entry* e) {
    const uint64_t g0 = e->base >> kGranuleShift;
    const uint64_t g1 = (e->base + e->bytes - 1) >> kGranuleShift;
    const auto first = index.find(g0);
    if (first == index.end() || std::find(first->second.begin(), first->second.end(), e) == first->second.end()) {
        return;
    }
    for (uint64_t g = g0; g <= g1; ++g) {
        const auto it = index.find(g);
        auto& list = it->second;
        list.erase(std::find(list.begin(), list.end(), e));
        if (list.empty()) {
            index.erase(it);
        }
        g_filter_counts[FilterSlot(g)].fetch_sub(1, std::memory_order_release);
    }
}

// sotc: SOTC_NATIVE_COPIES_FILTER (read once; default off). A written bind copies only from the entries the map walk
// can take: the newest registration of a base that is not single-write, and the waiting single-write registrations
// (g_pending). The consumed single-write registrations stay in g_by_base for seconds and the page index of
// SOTC_NATIVE_COPIES_PAGES counts them too, so its filter is almost never empty; ~87% of the walks found nothing to
// copy (13% of the draw thread in the frames of an area load). 1: hashed granule counts of those copy-capable entries
// only, written at the same add and remove points as the maps; a write whose granules all read 0 overlaps no
// copy-capable entry and returns before the lock (the walk would copy nothing; it only skips the "unpaired" count and
// the pop of pending entries the ring overwrote, which the next walk or PruneLocked does). 2: the walk runs anyway and
// an empty answer that found an overlap is counted (cf mismatches must read 0). Needs the std::maps only (FlatMode 0)
// and no page index.
int CopyFilterMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_NATIVE_COPIES_FILTER");
        return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
    }();
    return mode;
}
bool CopyFilterOn() {
    static const bool on = CopyFilterMode() != 0 && FlatMode() == 0 && !PagesOn();
    return on;
}
std::atomic<uint32_t> g_copy_counts[size_t{1} << kFilterBits];

// Adds delta to the slots of e's granules. Caller holds g_mutex.
void CopyCount(const Entry* e, int delta) {
    if (e->bytes == 0) {
        return;
    }
    const uint64_t g0 = e->base >> kGranuleShift;
    const uint64_t g1 = (e->base + e->bytes - 1) >> kGranuleShift;
    for (uint64_t g = g0; g <= g1; ++g) {
        if (delta > 0) {
            g_copy_counts[FilterSlot(g)].fetch_add(1, std::memory_order_release);
        } else {
            g_copy_counts[FilterSlot(g)].fetch_sub(1, std::memory_order_release);
        }
    }
}

// Lock-free: true when no copy-capable entry covers a granule of [addr, addr + size). Any thread.
bool CopyFilterEmpty(uint64_t addr, uint64_t size) {
    const uint64_t g0 = addr >> kGranuleShift;
    const uint64_t g1 = (addr + size - 1) >> kGranuleShift;
    if (g1 - g0 >= kMaxScanGranules) {
        return false;
    }
    for (uint64_t g = g0; g <= g1; ++g) {
        if (g_copy_counts[FilterSlot(g)].load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    return true;
}

void BaseIndexAdd(Entry* e) {
    if (PagesOn()) {
        GranuleAdd(g_gr_base, e);
    }
    if (CopyFilterOn() && !e->single && !e->cf_base) {
        CopyCount(e, +1);
        e->cf_base = true;
    }
}
void BaseIndexRemove(Entry* e) {
    if (PagesOn()) {
        GranuleRemove(g_gr_base, e);
    }
    if (e->cf_base) {
        CopyCount(e, -1);
        e->cf_base = false;
    }
}
void PendIndexAdd(Entry* e) {
    if (PagesOn()) {
        GranuleAdd(g_gr_pend, e);
    }
    if (CopyFilterOn() && !e->cf_pend) {
        CopyCount(e, +1);
        e->cf_pend = true;
    }
}
void PendIndexRemove(Entry* e) {
    if (PagesOn()) {
        GranuleRemove(g_gr_pend, e);
    }
    if (e->cf_pend) {
        CopyCount(e, -1);
        e->cf_pend = false;
    }
}

// Lock-free: true when no entry of either index covers a granule of [addr, addr + size), so no entry overlaps the
// write. False when one may (also when the write spans too many granules to ask). Any thread.
bool PagesFilterEmpty(uint64_t addr, uint64_t size) {
    const uint64_t g0 = addr >> kGranuleShift;
    const uint64_t g1 = (addr + size - 1) >> kGranuleShift;
    if (g1 - g0 >= kMaxScanGranules) {
        return false;
    }
    for (uint64_t g = g0; g <= g1; ++g) {
        if (g_filter_counts[FilterSlot(g)].load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    return true;
}

void RemoveFromIndexes(Entry* e) {
    if (UseMap()) {
        if (auto it = g_by_base.find(e->base); it != g_by_base.end() && it->second == e) {
            g_by_base.erase(it);
            BaseIndexRemove(e);
        }
    }
    if (UseFlat() && FlatFindBase(e->base) == e) {
        FlatEraseBase(e->base);
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
        PendIndexRemove(e);
        if (UseMap()) {
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
        if (UseFlat()) {
            FlatPendingRemove(e);
        }
    }
}

// Drops the oldest entries the ring overwrote or the record cap pushes out. Caller holds g_mutex.
void PruneLocked(uint64_t completed) {
    while (!g_entries.empty() && (!Valid(g_entries.front()) || g_entries.size() > kMaxRecords)) {
        Entry& e = g_entries.front();
        if (!Valid(e)) {
            g_c.pruned_ring.fetch_add(1, std::memory_order_relaxed);
            if (e.frame + kNeededFrames >= completed) {
                g_c.pruned_ring_live.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_c.pruned_count.fetch_add(1, std::memory_order_relaxed);
        }
        RemoveFromIndexes(&e);
        if (SotcWriteJournal::On()) {
            g_filter.Add(e.base, e.bytes, -1); // sotc: SOTC_WRITE_JOURNAL: the entry leaves the page counts
        }
        g_entries.pop_front();
    }
    g_watching.store(!Pick(g_by_base.empty(), g_flat_base.empty()), std::memory_order_release);
}

// Registrations whose frame the GPU completed have had their write recorded (a frame's writes are recorded before
// its submit). The ones still waiting are dropped so that a later write does not pair with them. Caller holds g_mutex.
void ExpirePendingLocked(uint64_t completed) {
    if (UseMap()) {
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            auto& q = it->second;
            while (!q.empty() && q.front()->frame <= completed) {
                PendIndexRemove(q.front());
                q.pop_front();
                g_c.expired.fetch_add(1, std::memory_order_relaxed);
            }
            it = q.empty() ? g_pending.erase(it) : std::next(it);
        }
    }
    if (UseFlat()) {
        // In mode 2 the map counted the expired registrations already.
        const bool count = !UseMap();
        for (size_t p = 0; p < g_flat_pending.size();) {
            auto& q = g_flat_pending[p].waiting;
            while (!q.empty() && q.front()->frame <= completed) {
                q.pop_front();
                if (count) {
                    g_c.expired.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (q.empty()) {
                g_flat_pending.erase(g_flat_pending.begin() + static_cast<std::ptrdiff_t>(p));
            } else {
                ++p;
            }
        }
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

// The overlaps of a write [addr, addr + size) with the registrations, through the std::map indexes (today's
// walk), in the order the copies are recorded: the newest registration of each base first (ascending base), then
// the paired single-write registrations (ascending base). Pops the pending writes it takes. count: the window
// counters are updated (mode 2 runs the flat walk with count off). Caller holds g_mutex.
void CollectMap(uint64_t addr, uint64_t size, uint64_t scan_lo, uint64_t submitted, bool count,
                std::vector<Overlap>& out) {
    // The other registrations: every overlap with the newest registration of a base takes the write.
    for (auto it = g_by_base.lower_bound(scan_lo); it != g_by_base.end() && it->first < addr + size; ++it) {
        Entry& e = *it->second;
        const uint64_t end = e.base + e.bytes;
        if (addr >= end || !Valid(e)) {
            continue;
        }
        if (e.single) {
            // Its write is paired through g_pending; a write with nothing waiting at the base is not its own.
            if (g_pending.find(e.base) == g_pending.end() && count) {
                g_c.unpaired.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
        out.push_back(Overlap{&e, lo, hi});
    }
    // Single-write registrations: the oldest waiting one at each base takes the write (FIFO, see the top comment).
    for (auto it = g_pending.lower_bound(scan_lo); it != g_pending.end() && it->first < addr + size;) {
        auto& q = it->second;
        while (!q.empty() && !Valid(*q.front())) {
            PendIndexRemove(q.front());
            q.pop_front();
        }
        if (!q.empty()) {
            Entry& e = *q.front();
            const uint64_t end = e.base + e.bytes;
            if (addr < end) {
                PendIndexRemove(&e);
                q.pop_front();
                const uint64_t lo = addr > e.base ? addr : e.base;
                const uint64_t hi = addr + size < end ? addr + size : end;
                out.push_back(Overlap{&e, lo, hi});
                if (count) {
                    g_c.paired.fetch_add(1, std::memory_order_relaxed);
                    MaxInto(g_c.write_lag_max, submitted > e.frame ? submitted - e.frame : 0);
                }
            }
        }
        it = q.empty() ? g_pending.erase(it) : std::next(it);
    }
}

// The same walk over the flat indexes (SOTC_FLAT_NOTES). Same order, same pops, same counters. Caller holds g_mutex.
void CollectFlat(uint64_t addr, uint64_t size, uint64_t scan_lo, uint64_t submitted, bool count,
                 std::vector<Overlap>& out) {
    for (size_t i = LowerIndex(g_flat_base, scan_lo); i < g_flat_base.size() && g_flat_base[i].base < addr + size;
         ++i) {
        Entry& e = *g_flat_base[i].entry;
        const uint64_t end = e.base + e.bytes;
        if (addr >= end || !Valid(e)) {
            continue;
        }
        if (e.single) {
            if (FlatPendingAt(e.base) == g_flat_pending.size() && count) {
                g_c.unpaired.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
        out.push_back(Overlap{&e, lo, hi});
    }
    for (size_t p = LowerIndex(g_flat_pending, scan_lo); p < g_flat_pending.size() && g_flat_pending[p].base < addr + size;) {
        auto& q = g_flat_pending[p].waiting;
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
                out.push_back(Overlap{&e, lo, hi});
                if (count) {
                    g_c.paired.fetch_add(1, std::memory_order_relaxed);
                    MaxInto(g_c.write_lag_max, submitted > e.frame ? submitted - e.frame : 0);
                }
            }
        }
        if (q.empty()) {
            g_flat_pending.erase(g_flat_pending.begin() + static_cast<std::ptrdiff_t>(p));
        } else {
            ++p;
        }
    }
}

// sotc: SOTC_NATIVE_COPIES_PAGES. The entries of an index under the granules of [addr, addr + size), appended to out
// (an entry under several of them is appended several times; SortUniqueByBase drops the copies). Caller holds g_mutex.
void GatherCandidates(const GranuleIndex& index, uint64_t addr, uint64_t size, std::vector<Entry*>& out) {
    const uint64_t g0 = addr >> kGranuleShift;
    const uint64_t g1 = (addr + size - 1) >> kGranuleShift;
    if (g1 - g0 < kMaxScanGranules) {
        for (uint64_t g = g0; g <= g1; ++g) {
            if (const auto it = index.find(g); it != index.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
    } else {
        for (const auto& [g, list] : index) {
            if (g >= g0 && g <= g1) {
                out.insert(out.end(), list.begin(), list.end());
            }
        }
    }
}

// Sorts the candidates by base and keeps one per base (a base is one entry of g_by_base, and one pending queue).
void SortUniqueByBase(std::vector<Entry*>& v) {
    std::sort(v.begin(), v.end(), [](const Entry* a, const Entry* b) { return a->base < b->base; });
    v.erase(std::unique(v.begin(), v.end(), [](const Entry* a, const Entry* b) { return a->base == b->base; }), v.end());
}

// sotc: SOTC_NATIVE_COPIES_PAGES. The overlaps CollectMap finds, taken from the granule indexes instead of the maps:
// the same entries, in the same order (the base index by ascending base, then the pending writes by ascending base),
// and the same pops when pop is set. Only the candidates under the write's granules are looked at. pop: take the
// pending writes as CollectMap does (mode 1). !pop: read only (mode 2 reads before the map walk pops; the front of a
// queue is then its first valid entry). count: the window counters are updated. Caller holds g_mutex.
void CollectPages(uint64_t addr, uint64_t size, uint64_t submitted, bool count, bool pop, std::vector<Overlap>& out) {
    const uint64_t limit = addr + size; // a registration at or above it cannot overlap the write
    uint64_t checked = 0;
    std::vector<Entry*>& bases = g_pg_base;
    bases.clear();
    GatherCandidates(g_gr_base, addr, size, bases);
    SortUniqueByBase(bases);
    for (Entry* ep : bases) {
        Entry& e = *ep;
        if (e.base >= limit) {
            break;
        }
        ++checked;
        const uint64_t end = e.base + e.bytes;
        if (addr >= end || !Valid(e)) {
            continue;
        }
        if (e.single) {
            // Its write is paired through the pending queue; a write with nothing waiting at the base is not its own.
            if (g_pending.find(e.base) == g_pending.end() && count) {
                g_c.unpaired.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        const uint64_t lo = addr > e.base ? addr : e.base;
        const uint64_t hi = addr + size < end ? addr + size : end;
        out.push_back(Overlap{&e, lo, hi});
    }
    std::vector<Entry*>& pend = g_pg_pend;
    pend.clear();
    GatherCandidates(g_gr_pend, addr, size, pend);
    SortUniqueByBase(pend);
    for (const Entry* ep : pend) {
        if (ep->base >= limit) {
            break;
        }
        ++checked;
        const auto it = g_pending.find(ep->base);
        if (it == g_pending.end()) {
            continue;
        }
        auto& q = it->second;
        Entry* front = nullptr;
        if (pop) {
            while (!q.empty() && !Valid(*q.front())) {
                PendIndexRemove(q.front());
                q.pop_front();
            }
            front = q.empty() ? nullptr : q.front();
        } else {
            for (Entry* c : q) {
                if (Valid(*c)) {
                    front = c;
                    break;
                }
            }
        }
        if (front) {
            const uint64_t end = front->base + front->bytes;
            if (addr < end) {
                if (pop) {
                    PendIndexRemove(front);
                    q.pop_front();
                }
                const uint64_t lo = addr > front->base ? addr : front->base;
                const uint64_t hi = addr + size < end ? addr + size : end;
                out.push_back(Overlap{front, lo, hi});
                if (count) {
                    g_c.paired.fetch_add(1, std::memory_order_relaxed);
                    MaxInto(g_c.write_lag_max, submitted > front->frame ? submitted - front->frame : 0);
                }
            }
        }
        if (pop && q.empty()) {
            g_pending.erase(it);
        }
    }
    g_c.pages_checked.fetch_add(checked, std::memory_order_relaxed);
}

bool SameOverlaps(const std::vector<Overlap>& a, const std::vector<Overlap>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].e != b[i].e || a[i].lo != b[i].lo || a[i].hi != b[i].hi) {
            return false;
        }
    }
    return true;
}

// Scratch lists of NoteWrittenBind (guarded by g_mutex): the overlaps used, and the flat walk of mode 2.
std::vector<Overlap> g_overlaps;
std::vector<Overlap> g_overlaps_check;

// sotc: SOTC_WRITE_JOURNAL. The written ranges of the draw thread in this epoch: a flat per-thread vector (no lock, no
// map), appended by NoteWrittenBind and merged by EpochEnd (the start of the next submit). It is not read before the
// epoch end: the copies of an op are recorded at the op (a later op of the same submit may write the same bytes).
struct Range {
    uint64_t addr;
    uint64_t size;
};
constexpr size_t kJournalCap = size_t{1} << 17; // entries before a compaction (2 MiB per thread)
thread_local std::vector<Range> tl_journal;
thread_local std::vector<Range> tl_merged;
thread_local size_t tl_compact_at = kJournalCap;

// Sorts the ranges (in place) and merges the overlapping or adjacent ones into merged.
void MergeRanges(std::vector<Range>& ranges, std::vector<Range>& merged) {
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.addr < b.addr; });
    merged.clear();
    for (const Range& r : ranges) {
        if (!merged.empty() && r.addr <= merged.back().addr + merged.back().size) {
            const uint64_t end = std::max(merged.back().addr + merged.back().size, r.addr + r.size);
            merged.back().size = end - merged.back().addr;
        } else {
            merged.push_back(r);
        }
    }
}

void JournalAppend(uint64_t addr, uint64_t size) {
    if (tl_journal.size() >= tl_compact_at) {
        // Full before its epoch ended: merged in place (every byte stays covered). The threshold grows with the
        // result, so a journal of many disjoint ranges is not merged again and again.
        const auto t0 = Clock::now();
        MergeRanges(tl_journal, tl_merged);
        SotcWriteJournal::Count(SotcWriteJournal::g_counters.coalesced, tl_merged.size());
        SotcWriteJournal::Count(SotcWriteJournal::g_counters.coalesce_ns, NsSince(t0));
        tl_journal.swap(tl_merged);
        tl_compact_at = std::max(kJournalCap, 2 * tl_journal.size());
    }
    tl_journal.push_back(Range{addr, size});
    SotcWriteJournal::Count(SotcWriteJournal::g_counters.ranges);
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
    if (SotcWriteJournal::On()) {
        g_filter.Add(base, bytes, +1); // sotc: SOTC_WRITE_JOURNAL: the entry's pages join the counts
    }
    if (UseMap()) {
        if (PagesOn() || CopyFilterOn()) {
            if (auto old = g_by_base.find(base); old != g_by_base.end()) {
                // sotc: SOTC_NATIVE_COPIES_PAGES / _FILTER: the replaced entry leaves its granules
                BaseIndexRemove(old->second);
            }
        }
        g_by_base[base] = e;
        BaseIndexAdd(e);
    }
    if (UseFlat()) {
        FlatSetBase(base, e);
    }
    if (single_write) {
        if (UseMap()) {
            g_pending[base].push_back(e);
            PendIndexAdd(e);
        }
        if (UseFlat()) {
            FlatPendingPush(base, e);
        }
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
    Entry* found_map = nullptr;
    if (UseMap()) {
        if (const auto it = g_by_base.find(base); it != g_by_base.end()) {
            found_map = it->second;
        }
    }
    const Entry* found = Pick<Entry*>(found_map, UseFlat() ? FlatFindBase(base) : nullptr);
    if (!found) {
        g_c.key_missing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!Valid(*found)) {
        g_c.key_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    *key = found->key;
    *seq = found->seq;
    if (identity) {
        *identity = found->identity;
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
    // sotc: SOTC_WRITE_JOURNAL. Mode 1: a write that no live entry's page covers returns here, before the lock and the
    // walks (no entry can overlap it, so the walk would find nothing and pop nothing). Mode 2 runs the walk and checks.
    const bool journal = SotcWriteJournal::On();
    if (journal) {
        SotcWriteJournal::Count(SotcWriteJournal::g_counters.ops);
    }
    if (size == 0) {
        return;
    }
    if (journal) {
        JournalAppend(addr, size);
        if (!SotcWriteJournal::Verify() && !g_filter.MayTouch(addr, size)) {
            SotcWriteJournal::Count(SotcWriteJournal::g_counters.native_filtered);
            return;
        }
    }
    // sotc: SOTC_NATIVE_COPIES_PAGES: no entry under the write's granules means no overlap, so mode 1 returns here,
    // before the lock, with nothing walked and nothing popped (the walk would find nothing either). Mode 2 goes on.
    const bool pages = PagesOn();
    if (pages) {
        const bool empty = PagesFilterEmpty(addr, size);
        g_c.pages_writes.fetch_add(1, std::memory_order_relaxed);
        if (empty) {
            g_c.pages_empty.fetch_add(1, std::memory_order_relaxed);
            if (PagesMode() == 1 && !SotcWriteJournal::Verify()) {
                return;
            }
        }
    }
    // sotc: SOTC_NATIVE_COPIES_FILTER: no copy-capable entry under the write's granules means nothing to copy, so mode
    // 1 returns here, before the lock. Mode 2 goes on and checks the answer against the walk below.
    bool cf_empty = false;
    if (CopyFilterOn()) {
        cf_empty = CopyFilterEmpty(addr, size);
        g_c.cf_writes.fetch_add(1, std::memory_order_relaxed);
        if (cf_empty) {
            g_c.cf_empty.fetch_add(1, std::memory_order_relaxed);
            if (CopyFilterMode() == 1) {
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool base_empty = Pick(g_by_base.empty(), g_flat_base.empty());
    const bool pending_empty = Pick(g_pending.empty(), g_flat_pending.empty());
    if (!g_staging || (base_empty && pending_empty)) {
        return;
    }
    const uint64_t submitted = SotcNative::FrameSubmitted(); // an atomic count (no lock)
    // Registrations are at most kMaxBytes long: the first one that can overlap starts at addr - kMaxBytes.
    const uint64_t scan_lo = addr > kMaxBytes ? addr - kMaxBytes : 0;
    // Find every overlap first (the walk pops the pending writes it takes), then record them in that order.
    std::vector<Overlap>& out = g_overlaps;
    out.clear();
    if (pages) {
        if (PagesMode() == 1) {
            CollectPages(addr, size, submitted, true, true, out);
        } else {
            // Mode 2: the page walk reads the queues before the map walk pops them; the two must agree.
            std::vector<Overlap>& check = g_overlaps_check;
            check.clear();
            CollectPages(addr, size, submitted, false, false, check);
            CollectMap(addr, size, scan_lo, submitted, true, out);
            if (!SameOverlaps(out, check)) {
                g_c.pages_mismatch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else if (FlatMode() == 0) {
        CollectMap(addr, size, scan_lo, submitted, true, out);
    } else {
        g_c.flat_writes.fetch_add(1, std::memory_order_relaxed);
        if (UseMap()) {
            const auto t0 = Clock::now();
            CollectMap(addr, size, scan_lo, submitted, true, out);
            g_c.flat_map_ns.fetch_add(NsSince(t0), std::memory_order_relaxed);
        }
        if (UseFlat()) {
            // Mode 2: the flat walk checks the map's answer (and keeps its own pending queues in step).
            std::vector<Overlap>& flat = UseMap() ? g_overlaps_check : out;
            flat.clear();
            const auto t0 = Clock::now();
            CollectFlat(addr, size, scan_lo, submitted, !UseMap(), flat);
            g_c.flat_vec_ns.fetch_add(NsSince(t0), std::memory_order_relaxed);
            if (UseMap() && !SameOverlaps(out, flat)) {
                Mismatch();
            }
        }
    }
    if (cf_empty && !out.empty()) {
        g_c.cf_mismatch.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_NATIVE_COPIES_FILTER=2 verify
    }
    if (journal) {
        SotcWriteJournal::Count(SotcWriteJournal::g_counters.native_walks);
        if (SotcWriteJournal::Verify()) {
            // The filter's answer against the walk's: "no page" must mean "no overlap", and the reverse counts as
            // a false "may touch".
            const bool may = g_filter.MayTouch(addr, size);
            if (!may && !out.empty()) {
                SotcWriteJournal::Count(SotcWriteJournal::g_counters.verify_diff);
            } else if (may && out.empty()) {
                SotcWriteJournal::Count(SotcWriteJournal::g_counters.may_false);
            }
        }
    }
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
    for (const Overlap& o : out) {
        record(*o.e, o.lo, o.hi);
    }
}

void EpochEnd() {
    if (!SotcWriteJournal::On() || tl_journal.empty()) {
        return;
    }
    const auto t0 = Clock::now();
    std::vector<Range> originals;
    if (SotcWriteJournal::Verify()) {
        originals = tl_journal; // MergeRanges sorts its input in place
    }
    MergeRanges(tl_journal, tl_merged);
    SotcWriteJournal::Count(SotcWriteJournal::g_counters.coalesced, tl_merged.size());
    SotcWriteJournal::Count(SotcWriteJournal::g_counters.coalesce_ns, NsSince(t0));
    if (SotcWriteJournal::Verify()) {
        // Every journaled range must lie inside one merged interval; a merge that dropped bytes would fail here.
        uint64_t bad = 0;
        for (const Range& r : originals) {
            const auto it = std::upper_bound(tl_merged.begin(), tl_merged.end(), r.addr,
                                             [](uint64_t a, const Range& m) { return a < m.addr; });
            if (it == tl_merged.begin() || (it - 1)->addr + (it - 1)->size < r.addr + r.size) {
                ++bad;
            }
        }
        if (bad != 0) {
            SotcWriteJournal::Count(SotcWriteJournal::g_counters.verify_diff, bad);
        }
    }
    tl_journal.clear();
    tl_compact_at = kJournalCap;
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
    if (FlatMode() != 0) {
        const uint64_t walks = take(g_c.flat_writes);
        const uint64_t map_ns = take(g_c.flat_map_ns);
        const uint64_t vec_ns = take(g_c.flat_vec_ns);
        const uint64_t bad = take(g_c.flat_mismatch);
        std::snprintf(line, sizeof line,
                      "SOTC FLAT NOTES (native copies): mode %d, write walks %.0f/s, std::map %.0f ns/s, flat %.0f "
                      "ns/s, flat mismatches %llu\n",
                      FlatMode(), static_cast<double>(walks) / secs, static_cast<double>(map_ns) / secs,
                      static_cast<double>(vec_ns) / secs, static_cast<unsigned long long>(bad));
        out += line;
    }
    if (PagesMode() != 0) {
        // sotc: SOTC_NATIVE_COPIES_PAGES: window totals (not rates), the same window as the line above.
        const uint64_t writes = take(g_c.pages_writes);
        const uint64_t empty = take(g_c.pages_empty);
        const uint64_t checked = take(g_c.pages_checked);
        const uint64_t mismatch = take(g_c.pages_mismatch);
        std::snprintf(line, sizeof line,
                      "SOTC NATIVE COPIES pages: writes %llu, empty fast %llu, entries checked %llu, verify mismatches "
                      "%llu\n",
                      static_cast<unsigned long long>(writes), static_cast<unsigned long long>(empty),
                      static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatch));
        out += line;
    }
    if (CopyFilterOn()) {
        // sotc: SOTC_NATIVE_COPIES_FILTER: window totals, the same window as the lines above.
        const uint64_t writes = take(g_c.cf_writes);
        const uint64_t empty = take(g_c.cf_empty);
        const uint64_t mismatch = take(g_c.cf_mismatch);
        std::snprintf(line, sizeof line,
                      "SOTC NATIVE COPIES filter: mode %d, writes %llu, nothing to copy %llu (%.1f%%, %s), verify "
                      "mismatches %llu\n",
                      CopyFilterMode(), static_cast<unsigned long long>(writes),
                      static_cast<unsigned long long>(empty), writes ? 100.0 * double(empty) / double(writes) : 0.0,
                      CopyFilterMode() == 1 ? "skipped" : "walked", static_cast<unsigned long long>(mismatch));
        out += line;
    }
}

} // namespace SotcNativeCopies
