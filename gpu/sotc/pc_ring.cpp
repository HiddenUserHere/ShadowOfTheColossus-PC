// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC memory model, per-frame upload ring. See pc_ring.h.
//
// Shadow state: per 4 KiB page of each arena block, the submit epoch of its first upload in that
// submit and a copy of the bytes uploaded then (a 64 MiB shadow of the arena). The ring uploads a
// page at most once per submit (it is declared written at the submit, uploaded by the first bind,
// and never protected). Today's path uploads it again within the same submit when a write fault
// re-dirtied it. If the bytes inside the second bind's range differ from the copy, the ring would
// have left the GPU with older bytes it reads: a divergence. Bytes outside the bound range belong
// to work not submitted yet and are declared at their own submit. (1 Nov: a shadow by 64 KiB
// granule counted ~100 harmless hazards per frame; a whole-page hash still counted 3-8.)
//
// Mode 1 (apply) declares the pages of each new arena allocation, once per page per submit
// (DeclareRange). The declare is only an optimisation: a page not declared still write-faults,
// because protection stays on, so a missed allocation costs a fault, never a stale byte.
//
// Blocks are few (3-4 of 32 MiB) and change only at area loads. A block slot is filled once and
// never reused for another base, so lock-free readers never see its arrays freed: a freed block
// is only marked inactive.

#include "pc_ring.h"
#include "arena_attrib.h" // sotc: SOTC_ARENA_ATTRIB (write faults on the arena by guest RIP)

#include "hooks.h"
#include "video_core/page_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (nullptr when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcPcRing {
namespace {

constexpr uint64_t kGranuleShift = 12; // 4 KiB, the tracking page
constexpr size_t kMaxBlocks = 32;
constexpr uint64_t kPage = uint64_t{1} << kGranuleShift;
constexpr uint64_t kPageMask = kPage - 1;

struct Block {
    std::atomic<uint64_t> base{0};
    std::atomic<uint64_t> size{0};
    std::atomic<bool> active{false};
    std::unique_ptr<std::atomic<uint32_t>[]> uploaded; // epoch of the last upload, 0 = never
    std::unique_ptr<unsigned char[]> shadow;           // the page's bytes at that upload (mode 2)
    std::unique_ptr<std::atomic<uint32_t>[]> declared; // per 4 KiB page: submit epoch of its declare (mode 1)
    std::atomic<bool> whole_pending{false};            // modes 3/4: pending list was full, emit the block
    std::unique_ptr<std::atomic<uint32_t>[]> gpu_epoch; // modes 4/5: per page, epoch of its last GPU write
    std::unique_ptr<std::atomic<uint64_t>[]> upl_seq;  // mode 7: per page, upload sequence of its last upload
};

Block g_blocks[kMaxBlocks];
std::atomic<size_t> g_block_count{0}; // slots filled (published after the slot's arrays)
std::mutex g_block_mutex;             // writers only

// Mode 8 heap serving (SetHostExtras): sorted, non-overlapping runs of heap granules served like arena
// blocks. Published as an immutable snapshot; old snapshots are kept (a lock-free reader may still hold
// one) and are few: a new one only when the run set changes.
std::atomic<const std::vector<RingExtent>*> g_extras{nullptr};
std::vector<std::unique_ptr<const std::vector<RingExtent>>> g_extras_kept; // under g_block_mutex

std::atomic<uint32_t> g_epoch{1}; // frame epoch; starts at 1 so 0 means "never"

// Window counters.
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<uint64_t> g_faults{0};       // write faults inside the arena
std::atomic<uint64_t> g_reuploads{0};    // pages uploaded again within the same submit
std::atomic<uint64_t> g_uploads{0};      // upload ranges touching the arena
std::atomic<uint64_t> g_upload_bytes{0}; // bytes of those ranges inside the arena
std::atomic<uint64_t> g_divergent{0};       // ... whose bytes changed, in a descriptor bind
std::atomic<uint64_t> g_divergent_sweep{0}; // ... in a DMA sweep (whole resident runs)
thread_local int t_sweep = 0;               // > 0 inside SynchronizeDmaBuffers
// Per site (Site enum): calls of the hook, and allocations whose pointer landed in an arena block.
enum Site : int { kSiteHeap = 0, kSiteBump, kSiteDispatch, kSiteCount };
const char* const kSiteNames[kSiteCount] = {"heap", "bump", "dispatch"};
std::atomic<uint64_t> g_site_calls[kSiteCount]{};
std::atomic<uint64_t> g_site_hits[kSiteCount]{};
std::atomic<uint64_t> g_declares{0}; // ... runs handed to DeclareCpuWrite
std::atomic<uint64_t> g_declared_bytes{0};

// Wrap sites (heap, dispatch): the size argument saved at the pre phase and read back at the post
// phase. A stack, because the dispatch site calls the heap site (nested pre/post pairs). A pair
// cut short by a longjmp leaves the depth off by one; the size of an unmatched pop is 0 (skipped).
constexpr unsigned kSizeStackDepth = 16;
struct SizeStack {
    uint64_t v[kSizeStackDepth];
    unsigned n;
};
thread_local SizeStack t_sizes{};
std::atomic<uint64_t> g_unreadable{0};   // pages not readable through the backing view
std::atomic<uint64_t> g_block_overflow{0};

// Full ring (modes 3 and 4). Arena extents recorded by the heap hook, guarded by g_ext_locked (a
// short spinlock: the heap hook runs on the game threads, the take on the draw thread). Epochs are
// non-decreasing along the list, so a take removes a prefix.
struct PendingExtent {
    uint64_t addr;
    uint64_t size;
    uint32_t epoch;
};
constexpr size_t kMaxPending = size_t{1} << 16;
constexpr size_t kSnapshots = 4;                              // takes kept for verify
constexpr uint64_t kSnapshotMaxBytes = uint64_t{32} << 20;   // per snapshot
std::atomic<bool> g_ext_locked{false};
std::vector<PendingExtent> g_pending;
std::atomic<uint64_t> g_block_version{0}; // bumped when the set of live blocks changes
std::atomic<uint64_t> g_ext_allocs{0};    // allocations that landed in an arena block (modes 3/4)
std::atomic<uint64_t> g_ext_overflow{0};  // of those, not recorded (pending list full)
std::atomic<uint64_t> g_ext_whole{0};     // whole-block extents emitted for them
std::atomic<uint64_t> g_takes{0};         // TakeSubmitExtents calls (one per submit)
std::atomic<uint64_t> g_taken{0};         // extents handed to the draw thread
std::atomic<uint64_t> g_taken_bytes{0};
std::atomic<uint64_t> g_copies{0};
std::atomic<uint64_t> g_copy_bytes{0};
std::atomic<uint64_t> g_copies_imported{0};
std::atomic<uint64_t> g_copies_fallback{0};
std::atomic<uint64_t> g_ring_faults{0};
std::atomic<uint64_t> g_snap_skipped{0};       // bytes of a take beyond the snapshot cap

// Modes 6 and 7 (protect then copy). Window counters, reset by Report.
//
// Entry queue. The game thread never touches the buffer cache or the page protection: RecordResult
// pushes the whole pages of an arena allocation here (lock-free, bounded), and the draw thread pops
// them in TakeRingEnters at the start of each take. The push happens before the allocation's extent
// is recorded (under g_ext_locked), so an extent the take returns always has its entry in the queue
// when the take pops it. A full queue drops the entry: its pages then keep normal write protection,
// which is correct (see pc_ring_copy.cpp).
struct EnterSlot {
    std::atomic<uint64_t> seq{0};
    uint64_t addr{0};
    uint64_t size{0};
};
constexpr size_t kEnterQueue = size_t{1} << 14; // power of two
struct EnterQueue {
    EnterSlot slots[kEnterQueue];
    std::atomic<uint64_t> enq{0}; // producers (game threads): next position to claim
    uint64_t deq = 0;             // consumer (draw thread only): next position to pop
    EnterQueue() {
        for (size_t i = 0; i < kEnterQueue; ++i) {
            slots[i].seq.store(i, std::memory_order_relaxed);
        }
    }
};
EnterQueue g_enter_q;
std::atomic<uint64_t> g_enter_dropped{0};    // entries not queued (queue full)
std::atomic<RingDirtyFn> g_dirty{nullptr};   // CPU-dirty probe (mode 7 verify)
std::atomic<uint64_t> g_upl_seq{0};          // upload sequence (mode 7): bumped per uploaded range
std::atomic<uint64_t> g_enter_calls{0};      // entries popped by the draw thread (takes)
std::atomic<uint64_t> g_enter_pages{0};      // pages those entries offered (whole pages of one allocation)
std::atomic<uint64_t> g_leave_pages{0};      // pages covered by the takes (protected, then copied)
std::atomic<uint64_t> g_v_skip_dirty{0};     // mode 7: bytes not compared, page CPU-dirty at the bind
std::atomic<uint64_t> g_v_skip_sup{0};       // mode 7: bytes not compared, page uploaded after the snapshot

// Verify (modes 4, 5). Window counters, reset by Report. Bytes, not pages.
std::atomic<uint64_t> g_v_binds{0};          // CheckBind calls (all binds)
std::atomic<uint64_t> g_v_checked{0};        // ... checked (one in SOTC_PC_RING_VERIFY_SAMPLE)
std::atomic<uint64_t> g_v_compared{0};       // bytes compared with a snapshot
std::atomic<uint64_t> g_v_divergent{0};      // ... whose guest bytes differ from the snapshot
std::atomic<uint64_t> g_v_div_binds{0};      // checked binds with at least one divergent byte
std::atomic<uint64_t> g_v_nosnap{0};         // checked bytes covered by no snapshot
// Coverage (modes 4, 5): the checked binds' arena bytes, by the age of the newest take whose
// extents cover them. Bucket 0: age 0 (this submit's take), 1: 1 to 3, 2: 4 to 15, 3: 16 to 63
// takes back, 4: no extent in the last 64 takes. Windows: K = 1 is bucket 0, K = 4 buckets 0-1,
// K = 16 buckets 0-2, K = 64 buckets 0-3; the bytes outside the window are the (b) class.
constexpr int kCovBuckets = 5;
std::atomic<uint64_t> g_cov_bytes[kCovBuckets]{};
std::atomic<uint64_t> g_cov_gpu[kCovBuckets]{}; // ... of those, in pages GPU-written within 64 submits
// Hot mask (mode 5). Window counters, plus the last mask's size.
std::atomic<uint64_t> g_hot_calls{0};
std::atomic<uint64_t> g_hot_changes{0};      // calls whose mask differs from the previous call's
std::atomic<uint64_t> g_hot_last_pages{0};
std::atomic<uint64_t> g_hot_last_extents{0};
std::atomic<uint64_t> g_gpu_write_bytes{0};  // NoteGpuWrite, arena bytes (window)
// Mode 5 incremental delta (HotRingDelta). Window counters, reset by Report.
std::atomic<uint64_t> g_delta_calls{0};       // takes that ran the delta
std::atomic<uint64_t> g_delta_enter{0};       // pages that joined the mask
std::atomic<uint64_t> g_delta_leave{0};       // pages that left it
std::atomic<uint64_t> g_delta_enter_max{0};   // most pages joining the mask in one take
std::atomic<uint64_t> g_delta_leave_max{0};   // most pages leaving the mask in one take
std::atomic<uint64_t> g_delta_applies{0};     // protection applies measured by the copy side
std::atomic<uint64_t> g_delta_apply_ns{0};    // their total time
std::atomic<uint64_t> g_hot_full_checks{0};   // full mask computations (HotRingPages) checked
std::atomic<uint64_t> g_hot_mismatch{0};      // pages where the delta state and the full mask differ (must be 0)

// sotc: SOTC_PC_RING=8 (host arena binds). g_h_blocks and g_h_block_bytes are the imported set (gauge);
// the others count one report window.
std::atomic<uint64_t> g_h_blocks{0};
std::atomic<uint64_t> g_h_block_bytes{0};
std::atomic<uint64_t> g_h_binds{0};
std::atomic<uint64_t> g_h_bytes{0};
std::atomic<uint64_t> g_h_written{0};
std::atomic<uint64_t> g_h_straddles{0};
std::atomic<uint64_t> g_h_straddles_served{0};
std::atomic<uint64_t> g_h_clips{0}; // windowed binds clipped to an imported run end
std::atomic<uint64_t> g_h_fallbacks{0};
std::atomic<uint64_t> g_h_write_seq{0};     // served written binds, ever (HostWriteSeq)
std::atomic<uint64_t> g_h_unique_read{0};   // unique bytes read per submit, summed over the window
std::atomic<uint64_t> g_h_unique_write{0};
std::atomic<uint64_t> g_h_mark{0};         // bytes marked CPU-modified for the normal path (mode 8)
std::atomic<uint64_t> g_h_mark_skip{0};    // bytes not marked: already marked in this submit epoch
std::atomic<uint64_t> g_h_labels{0};        // EOP labels seen by the label side (mode 8)
std::atomic<uint64_t> g_h_labels_deferred{0};
std::atomic<uint64_t> g_h_labels_excluded{0}; // ring8 labels not deferred: the submit waits on them
std::atomic<uint64_t> g_h_cp_wait_flush{0};   // ring8 CP waits on a pending label (open buffer flushed)
std::atomic<uint64_t> g_h_labels_forced{0};   // ring8 deferred labels written by the 500 ms watchdog
// sotc: SOTC_PC_RING=8 flip wait (SetFlipWait, OnFrameBoundary). Calls, waits on a tick, time, max.
std::atomic<FlipWaitFn> g_flip_fn{nullptr};
std::atomic<void*> g_flip_ctx{nullptr};
std::atomic<uint64_t> g_fw_calls{0};
std::atomic<uint64_t> g_fw_waits{0};
std::atomic<uint64_t> g_fw_ns{0};
std::atomic<uint64_t> g_fw_max_ns{0};
std::atomic<uint64_t> g_fw_halves{0};        // halves holding a tick, summed over the calls
// Verify (SOTC_PC_RING8_VERIFY): checks of sampled binds at the next submit start.
std::atomic<uint64_t> g_v8_reads{0}, g_v8_read_bytes{0}, g_v8_read_changed{0};
std::atomic<uint64_t> g_v8_writes{0}, g_v8_write_bytes{0}, g_v8_write_changed{0};

// A readable run of a snapshot: guest [addr, end) is held at bytes[off, off + end - addr).
struct Piece {
    uint64_t addr;
    uint64_t end;
    size_t off;
};
struct Snapshot {
    uint32_t epoch = 0;
    uint64_t upl_seq = 0; // mode 7: g_upl_seq when the snapshot was taken (after the protect step)
    std::vector<Piece> pieces; // sorted, disjoint
    std::vector<unsigned char> bytes;
};
std::mutex g_verify_mutex; // modes 4, 5: snapshots, their sequence and the verify state (draw thread, so uncontended)
Snapshot g_snaps[kSnapshots];
uint64_t g_snap_seq = 0; // snapshots taken; slot = seq % kSnapshots

// Take history (modes 3 to 5): the merged extents of each of the last kHistory takes, guarded by
// g_hist_mutex (draw thread only, so uncontended). Slot = take index % kHistory; the newest take
// is g_take_seq - 1. Feeds the hot mask and the coverage classes.
constexpr size_t kHistory = 64;
std::mutex g_hist_mutex;
std::vector<RingExtent> g_hist[kHistory];
uint64_t g_take_seq = 0;
std::vector<RingExtent> g_hot_last; // the last mask returned by HotRingPages
uint64_t g_hot_version = 0;

// Mode 5 delta state (HotRingDelta), draw thread only. The hot mask is the union, over the window,
// of the whole-page runs of each take (WholePageRuns). g_hot_pages holds the mask page by page: the
// newest take + 1 that whole-covers it. A page is in the mask iff it has an entry; the entry goes
// when its take leaves the window and no newer take covers the page. g_hot_queue keeps the runs of
// each take with whole pages, oldest first, so the expiry walks only what leaves.
struct HotTake {
    uint64_t take;
    std::vector<RingExtent> runs;
};
std::unordered_map<uint64_t, uint64_t> g_hot_pages;
std::deque<HotTake> g_hot_queue;

// Verify sampling (SOTC_PC_RING_VERIFY_SAMPLE, default 16, at least 1) and the hot window
// (SOTC_PC_RING_HOT_EPOCHS, default 32, 1 to kHistory). Read once.
uint64_t VerifySample() {
    static const uint64_t n = [] {
        const char* v = std::getenv("SOTC_PC_RING_VERIFY_SAMPLE");
        const long s = v ? std::atol(v) : 16;
        return static_cast<uint64_t>(s < 1 ? 1 : s);
    }();
    return n;
}

uint64_t HotEpochs() {
    static const uint64_t k = [] {
        const char* v = std::getenv("SOTC_PC_RING_HOT_EPOCHS");
        const long h = v ? std::atol(v) : 32;
        return static_cast<uint64_t>(std::clamp<long>(h, 1, static_cast<long>(kHistory)));
    }();
    return k;
}

// Modes 4, 5 and 7 check the binds against the snapshots.
bool VerifyOn() {
    const int m = Mode();
    return m == 4 || m == 5 || m == 7;
}

// Sorts extents by address and merges the ones that overlap or touch. Result: disjoint, sorted.
void MergeExtents(std::vector<RingExtent>& v) {
    std::sort(v.begin(), v.end(),
              [](const RingExtent& a, const RingExtent& b) { return a.addr < b.addr; });
    size_t n = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].size == 0) {
            continue;
        }
        if (n > 0 && v[i].addr <= v[n - 1].addr + v[n - 1].size) {
            const uint64_t end = std::max(v[n - 1].addr + v[n - 1].size, v[i].addr + v[i].size);
            v[n - 1].size = end - v[n - 1].addr;
        } else {
            v[n++] = v[i];
        }
    }
    v.resize(n);
}

// The whole-page runs of one take's merged extents (disjoint, page aligned). A page that an
// extent covers only in part is not in any run: it keeps normal write protection.
void WholePageRuns(const std::vector<RingExtent>& merged, std::vector<RingExtent>& out) {
    for (const RingExtent& e : merged) {
        const uint64_t lo = (e.addr + kPageMask) & ~kPageMask;
        const uint64_t hi = (e.addr + e.size) & ~kPageMask;
        if (hi > lo) {
            out.push_back(RingExtent{lo, hi - lo});
        }
    }
}

// Page addresses (unique) to sorted, disjoint, merged extents.
void PagesToExtents(std::vector<uint64_t>& pages, std::vector<RingExtent>& out) {
    std::sort(pages.begin(), pages.end());
    for (size_t i = 0; i < pages.size();) {
        size_t j = i + 1;
        while (j < pages.size() && pages[j] == pages[j - 1] + kPage) {
            ++j;
        }
        out.push_back(RingExtent{pages[i], pages[j - 1] - pages[i] + kPage});
        i = j;
    }
}

// A byte range [a, z) of a checked bind, and (for the coverage sweep) the age bucket it fell in.
struct Span {
    uint64_t a;
    uint64_t z;
    int bucket;
};

struct ExtentLock {
    ExtentLock() {
        while (g_ext_locked.exchange(true, std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    ~ExtentLock() {
        g_ext_locked.store(false, std::memory_order_release);
    }
    ExtentLock(const ExtentLock&) = delete;
    ExtentLock& operator=(const ExtentLock&) = delete;
};

// The block holding addr, and the granule index; false outside the arena.
bool Find(uint64_t addr, Block** out, uint64_t* granule) {
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        Block& b = g_blocks[i];
        if (!b.active.load(std::memory_order_relaxed)) {
            continue;
        }
        const uint64_t base = b.base.load(std::memory_order_relaxed);
        const uint64_t size = b.size.load(std::memory_order_relaxed);
        if (addr >= base && addr - base < size) {
            *out = &b;
            *granule = (addr - base) >> kGranuleShift;
            return true;
        }
    }
    return false;
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PC_RING");
        int m = v ? std::atoi(v) : 0;
        if (m < 0 || m > 8) {
            m = 0;
        }
        if (m) {
            const char* what = m == 1   ? "apply: unprotect at allocation"
                               : m == 2 ? "shadow"
                               : m == 3 ? "full ring"
                               : m == 4 ? "full ring, verify"
                               : m == 5 ? "full ring, hot mask, verify"
                               : m == 6 ? "full ring, protect then copy"
                               : m == 7 ? "full ring, protect then copy, verify"
                                        : "host arena binds (imported guest memory, no copy)";
            std::fprintf(stderr,
                         "SOTC PC RING: mode %d (%s; needs SOTC_HOOK_ALLOC_PROBE=1, and "
                         "SOTC_HOOK_FRAME_FLIP=1 for the per-frame figures)\n",
                         m, what);
        }
        return m;
    }();
    return mode;
}

int FullRingMode() {
    const int m = Mode();
    return (m >= 3 && m <= 7) ? m : 0;
}

// sotc: C-linkage gate for the runtime (src/runtime_memory.c, weak reference): 1 in ring modes 6 and 7.
extern "C" int sotc_pc_ring_full6(void) {
    return FullRingMode() >= 6 ? 1 : 0;
}

void OnBlock(uint64_t base, uint64_t size, const char* name, int kind) {
    SotcArenaAttrib::OnBlock(base, size, name, kind); // sotc: SOTC_ARENA_ATTRIB (own block list)
    if (!On() || kind != 2 || !name || std::strcmp(name, "Default TaggedArena") != 0 || !base ||
        !size || size > (uint64_t{1} << 32)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_block_mutex);
    const size_t n = g_block_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) { // the same base again (freed, then reused): reactivate
        Block& b = g_blocks[i];
        if (b.base.load(std::memory_order_relaxed) == base &&
            b.size.load(std::memory_order_relaxed) == size) {
            b.active.store(true, std::memory_order_release);
            g_block_version.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (n == kMaxBlocks) {
        g_block_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Block& b = g_blocks[n];
    const size_t pages = static_cast<size_t>((size + kPageMask) >> kGranuleShift);
    if (VerifyOn()) {
        b.gpu_epoch.reset(new std::atomic<uint32_t>[pages]());
    }
    if (Mode() == 7) {
        b.upl_seq.reset(new std::atomic<uint64_t>[pages]());
    }
    if (Mode() == 2) {
        b.uploaded.reset(new std::atomic<uint32_t>[pages]());
        b.shadow.reset(new unsigned char[pages * kPage]());
    } else if (Mode() == 1) {
        b.declared.reset(new std::atomic<uint32_t>[pages]());
    }
    b.base.store(base, std::memory_order_relaxed);
    b.size.store(size, std::memory_order_relaxed);
    b.active.store(true, std::memory_order_relaxed);
    g_block_count.store(n + 1, std::memory_order_release);
    g_block_version.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "SOTC PC RING: arena block %llx +%llx MiB (%zu blocks)\n",
                 static_cast<unsigned long long>(base), static_cast<unsigned long long>(size >> 20),
                 n + 1);
}

void OnFreeBlock(uint64_t base) {
    SotcArenaAttrib::OnFreeBlock(base);
    if (!On() || !base) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_block_mutex);
    const size_t n = g_block_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        if (g_blocks[i].base.load(std::memory_order_relaxed) == base &&
            g_blocks[i].active.exchange(false, std::memory_order_release)) {
            g_block_version.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnFrameBoundary() {
    SotcArenaAttrib::OnFrameBoundary();
    if (!On()) {
        return;
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    // sotc: mode 8 flip wait. Game render thread, before the frame's writers reuse a half.
    if (Mode() == 8 && Ring8FlipWaitOn()) {
        if (FlipWaitFn fn = g_flip_fn.load(std::memory_order_acquire)) {
            fn(g_flip_ctx.load(std::memory_order_acquire));
        }
    }
}

void SetFlipWait(FlipWaitFn fn, void* ctx) {
    g_flip_ctx.store(ctx, std::memory_order_release);
    g_flip_fn.store(fn, std::memory_order_release);
}

int Ring8FlipWaitMode() {
    // sotc: 1 = the drain wait (WaitGpuIdle at every flip; ~6 FPS, 1 Nov). 2 = the precise wait: the
    // GPU tick of the frame that used the half is recorded when its frame end reaches the draw thread,
    // and only that tick is waited for (no drain). Default 2 since 1 Nov (the half reuse race showed on
    // every flip: avg 8.3 ms of GPU lag); 0 disables.
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PC_RING8_FLIPWAIT");
        if (!v || !v[0]) {
            return 2;
        }
        const int m = std::atoi(v);
        return (m == 1 || m == 2) ? m : 0;
    }();
    return Mode() == 8 ? mode : 0;
}

bool Ring8FlipWaitOn() {
    return Ring8FlipWaitMode() != 0;
}

void NoteFlipWait(uint64_t ns, bool waited, unsigned halves) {
    g_fw_calls.fetch_add(1, std::memory_order_relaxed);
    g_fw_halves.fetch_add(halves, std::memory_order_relaxed);
    if (!waited) {
        return;
    }
    g_fw_waits.fetch_add(1, std::memory_order_relaxed);
    g_fw_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = g_fw_max_ns.load(std::memory_order_relaxed);
    while (prev < ns && !g_fw_max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

void OnSubmit() {
    SotcArenaAttrib::OnSubmit(); // sotc: SOTC_ARENA_ATTRIB epoch (runs with the ring off too)
    if (!On()) {
        return;
    }
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_submits.fetch_add(1, std::memory_order_relaxed);
}

void NoteWriteFault(uint64_t addr) {
    Block* b = nullptr;
    uint64_t g = 0;
    if (Find(addr, &b, &g)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteGpuWrite(uint64_t addr, uint64_t size) {
    if (!VerifyOn() || !size) {
        return;
    }
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    const uint64_t end = addr + size;
    uint64_t counted = 0;
    for (uint64_t a = addr & ~kPageMask; a < end; a += kPage) {
        Block* b = nullptr;
        uint64_t g = 0;
        if (!Find(a, &b, &g)) {
            continue;
        }
        b->gpu_epoch[g].store(epoch, std::memory_order_relaxed);
        counted += std::min(a + kPage, end) - std::max(a, addr);
    }
    g_gpu_write_bytes.fetch_add(counted, std::memory_order_relaxed);
}

void NoteUpload(uint64_t addr, uint64_t size, uint64_t bind_addr, uint64_t bind_size) {
    if (Mode() == 7 && size) {
        // sotc: mode 7. Every page of an uploaded range gets the sequence of this upload, so a
        // snapshot taken before it is known to be superseded for that page (CompareRun).
        const uint64_t seq = g_upl_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
        const uint64_t end = addr + size;
        for (uint64_t a = addr & ~kPageMask; a < end; a += kPage) {
            Block* b = nullptr;
            uint64_t g = 0;
            if (Find(a, &b, &g) && b->upl_seq) {
                b->upl_seq[g].store(seq, std::memory_order_release);
            }
        }
        return;
    }
    if (Mode() != 2 || !size) {
        return;
    }
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    const uint64_t bind_end = bind_addr + bind_size;
    bool counted = false;
    const uint64_t end = addr + size;
    for (uint64_t a = addr & ~kPageMask; a < end; a += kPage) {
        Block* b = nullptr;
        uint64_t g = 0;
        if (!Find(a, &b, &g)) {
            continue;
        }
        if (!counted) {
            g_uploads.fetch_add(1, std::memory_order_relaxed);
            counted = true;
        }
        g_upload_bytes.fetch_add(kPage, std::memory_order_relaxed);
        const unsigned char* bytes = runtime_memory_backing_pointer(static_cast<uintptr_t>(a));
        if (!bytes) {
            g_unreadable.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        unsigned char* copy = &b->shadow[g * kPage];
        if (b->uploaded[g].load(std::memory_order_relaxed) == epoch) {
            g_reuploads.fetch_add(1, std::memory_order_relaxed);
            // Only the bytes this bind reads matter: the rest of the page may hold data of work
            // not submitted yet, which the ring declares at its own submit.
            const uint64_t lo = std::max(a, bind_addr);
            const uint64_t hi = std::min(a + kPage, bind_end);
            if (lo < hi && std::memcmp(copy + (lo - a), bytes + (lo - a), hi - lo) != 0) {
                (t_sweep > 0 ? g_divergent_sweep : g_divergent)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            continue; // the copy keeps the first upload's bytes: what the ring would have sent
        }
        std::memcpy(copy, bytes, kPage);
        b->uploaded[g].store(epoch, std::memory_order_relaxed);
    }
}

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

// Mode 1. Declares the pages [addr, addr + size) that lie in block b, once per page per submit: a
// page already declared in this epoch (by this thread or another) is skipped. Runs of pages that
// are write-protected go to DeclareCpuWrite, which invalidates them as the write fault's write
// branch would, so the game's writes to them do not fault. Pages not protected are left alone.
// Pages declared here and written again later in the submit still fault (protection stays on),
// so nothing is missed.
void DeclareRange(Block* b, uint64_t addr, uint64_t size, uint32_t epoch) {
    const uint64_t base = b->base.load(std::memory_order_relaxed);
    const uint64_t block_end = base + b->size.load(std::memory_order_relaxed);
    if (addr < base || addr >= block_end) {
        return;
    }
    const uint64_t end = std::min(addr + size, block_end);
    uint64_t run_lo = 0;
    uint64_t run_hi = 0;
    // sotc: SOTC_PC_RING_SLOW=1 keeps the old declare (posted to the draw thread, which runs the
    // invalidation there). Default: DeclareRingWrite, which settles the runs on the calling thread.
    static const bool slow_declare = [] {
        const char* v = std::getenv("SOTC_PC_RING_SLOW");
        return v && v[0] == '1';
    }();
    auto flush = [&] {
        const uint64_t hi = std::min(run_hi, block_end);
        if (hi > run_lo) {
            if (slow_declare) {
                if (VideoCore::PageManager::MaybeWriteProtected(run_lo, hi - run_lo)) {
                    VideoCore::PageManager::DeclareCpuWrite(run_lo, hi - run_lo);
                    g_declares.fetch_add(1, std::memory_order_relaxed);
                    g_declared_bytes.fetch_add(hi - run_lo, std::memory_order_relaxed);
                }
            } else {
                // The lock-free "nothing protected" look is inside DeclareRingWrite (counted there).
                VideoCore::PageManager::DeclareRingWrite(run_lo, hi - run_lo);
                g_declares.fetch_add(1, std::memory_order_relaxed);
                g_declared_bytes.fetch_add(hi - run_lo, std::memory_order_relaxed);
            }
        }
        run_lo = run_hi = 0;
    };
    for (uint64_t p = std::max(addr & ~kPageMask, base); p < end; p += kPage) {
        std::atomic<uint32_t>& slot = b->declared[(p - base) >> kGranuleShift];
        if (slot.exchange(epoch, std::memory_order_relaxed) == epoch) {
            flush(); // declared already in this submit
            continue;
        }
        if (run_hi != p) {
            flush();
            run_lo = p;
        }
        run_hi = p + kPage;
    }
    flush();
}

// Reads a 64-bit field of a guest object. Guest pointers are host pointers (the game runs
// natively), so the game's own object is read directly. The caller checks the pointer is not null.
uint64_t ReadU64(uint64_t addr) {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), sizeof v);
    return v;
}

// Site "bump" (comparison only, hook PC_RING_BUMP, SOTC_PC_RING_BUMP): entry of the frame bump allocator sub_48440
// (rdi = allocator object, rsi = size, edx = align). The allocator returns the cursor (object
// +0x98) rounded up to align, and advances the cursor by size inside the chunk that ends at
// +0xa8. The pre phase declares exactly that range before the allocator returns and the game
// writes it. An allocation that spills into the next chunk is not declared: its writes still
// fault, as they do today. Offsets and the align rule are from the decompilation (reports3/05).
HookAction BumpPre(GuestRegs& regs, void*) {
    g_site_calls[kSiteBump].fetch_add(1, std::memory_order_relaxed);
    const uint64_t obj = regs.rdi;
    const uint64_t size = regs.rsi;
    if (Mode() != 1 || !obj || !size || size > (uint64_t{1} << 30)) {
        return HookAction::Continue;
    }
    const uint64_t cursor = ReadU64(obj + 0x98);
    const uint64_t chunk = ReadU64(obj + 0x88);
    const uint64_t chunk_end = ReadU64(obj + 0xa8);
    if (!cursor || !chunk) {
        return HookAction::Continue;
    }
    const int32_t align = static_cast<int32_t>(regs.rdx);
    const uint64_t mask = static_cast<uint64_t>(static_cast<int64_t>(align - 1));
    const uint64_t at = (cursor + mask) & ~mask;
    if (!at || at + size < at || at + size > chunk_end) {
        return HookAction::Continue;
    }
    Block* b = nullptr;
    uint64_t granule = 0;
    if (!Find(at, &b, &granule)) {
        return HookAction::Continue;
    }
    g_site_hits[kSiteBump].fetch_add(1, std::memory_order_relaxed);
    DeclareRange(b, at, size, g_epoch.load(std::memory_order_acquire));
    return HookAction::Continue;
}

void SizePush(uint64_t size) {
    if (t_sizes.n < kSizeStackDepth) {
        t_sizes.v[t_sizes.n] = size;
    }
    ++t_sizes.n;
}

uint64_t SizePop() {
    if (t_sizes.n == 0) {
        return 0;
    }
    --t_sizes.n;
    return t_sizes.n < kSizeStackDepth ? t_sizes.v[t_sizes.n] : 0;
}

// Post phase of a wrap site: the pointer returned in rax, with the size saved by the pre phase.
// Declares the range when the pointer lies in an arena block (the same check for every site).
void DeclareResult(int site, uint64_t addr, uint64_t size) {
    if (!addr || !size || size > (uint64_t{1} << 30)) {
        return;
    }
    Block* b = nullptr;
    uint64_t granule = 0;
    if (!Find(addr, &b, &granule)) {
        return;
    }
    g_site_hits[site].fetch_add(1, std::memory_order_relaxed);
    DeclareRange(b, addr, size, g_epoch.load(std::memory_order_acquire));
}

// Modes 6 and 7. Lock-free push of an entry (any thread, Vyukov bounded queue: a producer claims a
// position with a CAS on enq, then publishes its slot with a release store of seq). False when the
// queue is full. The entry is only an optimisation: a dropped one leaves its pages normally
// protected.
bool EnterPush(uint64_t addr, uint64_t size) {
    EnterQueue& q = g_enter_q;
    uint64_t pos = q.enq.load(std::memory_order_relaxed);
    for (;;) {
        EnterSlot& slot = q.slots[pos & (kEnterQueue - 1)];
        const uint64_t seq = slot.seq.load(std::memory_order_acquire);
        const int64_t diff = static_cast<int64_t>(seq - pos);
        if (diff == 0) {
            if (q.enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                slot.addr = addr;
                slot.size = size;
                slot.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false; // the slot is still unread from a lap ago: full
        } else {
            pos = q.enq.load(std::memory_order_relaxed);
        }
    }
}

// Modes 6 and 7. Draw thread only (the single consumer). False when the queue is empty.
bool EnterPop(uint64_t& addr, uint64_t& size) {
    EnterQueue& q = g_enter_q;
    EnterSlot& slot = q.slots[q.deq & (kEnterQueue - 1)];
    const uint64_t seq = slot.seq.load(std::memory_order_acquire);
    if (static_cast<int64_t>(seq - (q.deq + 1)) < 0) {
        return false; // not published yet
    }
    addr = slot.addr;
    size = slot.size;
    slot.seq.store(q.deq + kEnterQueue, std::memory_order_release); // free for the next lap
    ++q.deq;
    return true;
}

// Modes 3 and 4. Records the allocation [addr, addr + size) as an extent of the current epoch when
// it lies in an arena block (clipped to the block end). Merges it into the last pending extent when
// that one has the same epoch and the two overlap or touch. When the pending list is full, the
// block is flagged: TakeSubmitExtents emits the whole block instead, so nothing is lost.
// Modes 6 and 7: before the extent is recorded, the whole pages of the allocation are pushed to the
// entry queue (EnterPush). Every path below records the extent or flags its block, so a queued entry
// always has an extent (taken later) that consumes its pages. Runs on the game thread: it takes only
// the lock-free queue and the short extent spinlock, never the buffer cache or the page protection.
void RecordResult(uint64_t addr, uint64_t size) {
    if (!addr || !size || size > (uint64_t{1} << 30)) {
        return;
    }
    Block* b = nullptr;
    uint64_t granule = 0;
    if (!Find(addr, &b, &granule)) {
        return;
    }
    const uint64_t block_end = b->base.load(std::memory_order_relaxed) +
                               b->size.load(std::memory_order_relaxed);
    if (addr + size > block_end) {
        size = block_end - addr;
    }
    if (Mode() == 6 || Mode() == 7) {
        // Whole pages only: a page the allocation covers in part keeps its normal protection.
        const uint64_t first = (addr + kPageMask) & ~kPageMask;
        const uint64_t last = (addr + size) & ~kPageMask;
        if (last > first) {
            if (!EnterPush(first, last - first)) {
                g_enter_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    g_site_hits[kSiteHeap].fetch_add(1, std::memory_order_relaxed);
    g_ext_allocs.fetch_add(1, std::memory_order_relaxed);
    ExtentLock lock;
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    if (!g_pending.empty()) {
        PendingExtent& back = g_pending.back();
        if (back.epoch == epoch && addr >= back.addr && addr <= back.addr + back.size) {
            back.size = std::max(back.addr + back.size, addr + size) - back.addr;
            return;
        }
    }
    if (g_pending.size() < kMaxPending) {
        if (g_pending.capacity() == 0) {
            g_pending.reserve(kMaxPending);
        }
        g_pending.push_back(PendingExtent{addr, size, epoch});
        return;
    }
    b->whole_pending.store(true, std::memory_order_relaxed);
    g_ext_overflow.fetch_add(1, std::memory_order_relaxed);
}

// Site "heap" (default, hook PC_RING_HEAP, SOTC_PC_RING): wrap of the allocator slot +0xa0 of the
// RenderSingleFrameGPU heap object, sub_1d750 (rdi = heap, rsi = size, edx = align). The memtype
// dispatcher 0x12820 reaches it only through that slot (table[DAT_02ed1f54] = the heap), so every
// allocation of the frame heap, from any caller, passes here once. The heap's class vtable is
// shared with the other heaps of render_context_register_heaps: their calls are counted too, and
// only the ones that land in an arena block are declared. The returned pointer is computed inside
// the function, so it is read at the post phase.
HookAction HeapPre(GuestRegs& regs, void*) {
    g_site_calls[kSiteHeap].fetch_add(1, std::memory_order_relaxed);
    if (Mode() == 1 || FullRingMode() || SotcArenaAttrib::On()) {
        SizePush(regs.rsi);
    }
    return HookAction::Continue;
}

void HeapPost(GuestRegs& regs, void*) {
    // sotc: SOTC_ARENA_ATTRIB shares this wrap (hooks.cpp enables it): the allocation's pages take
    // the current submit epoch. It never declares.
    const bool attrib = SotcArenaAttrib::On();
    if (FullRingMode()) {
        // Modes 3/4: record only, never declare (no DeclareCpuWrite).
        const uint64_t size = SizePop();
        if (attrib) {
            SotcArenaAttrib::NoteAlloc(regs.rax, size);
        }
        RecordResult(regs.rax, size);
        return;
    }
    if (Mode() != 1 && !attrib) {
        return;
    }
    const uint64_t size = SizePop();
    if (attrib) {
        SotcArenaAttrib::NoteAlloc(regs.rax, size);
    }
    if (Mode() == 1) {
        DeclareResult(kSiteHeap, regs.rax, size);
    }
}

// Site "dispatch" (hook PC_RING_ALLOC, SOTC_PC_RING_DISPATCH): wrap of the generic allocator
// 0x12820 (size, memtype, align, flags), once per allocation of every family. Kept for comparison.
HookAction AllocPre(GuestRegs& regs, void*) {
    g_site_calls[kSiteDispatch].fetch_add(1, std::memory_order_relaxed);
    if (Mode() == 1) {
        SizePush(regs.rdi);
    }
    return HookAction::Continue;
}

void AllocPost(GuestRegs& regs, void*) {
    if (Mode() != 1) {
        return;
    }
    DeclareResult(kSiteDispatch, regs.rax, SizePop());
}

} // namespace

void Register() {
    SotcArenaAttrib::Init(); // sotc: SOTC_ARENA_ATTRIB exit report
    SotcHooks::Handler heap;
    heap.pre = &HeapPre;
    heap.post = &HeapPost;
    SotcHooks::RegisterHandler("PC_RING_HEAP", heap);

    SotcHooks::Handler bump;
    bump.pre = &BumpPre;
    SotcHooks::RegisterHandler("PC_RING_BUMP", bump);

    SotcHooks::Handler alloc;
    alloc.pre = &AllocPre;
    alloc.post = &AllocPost;
    SotcHooks::RegisterHandler("PC_RING_ALLOC", alloc);
}

SweepScope::SweepScope() {
    ++t_sweep;
}
SweepScope::~SweepScope() {
    --t_sweep;
}

void TakeSubmitExtents(std::vector<RingExtent>& out) {
    if (!FullRingMode()) {
        return;
    }
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t whole = 0;
    const size_t first = out.size();
    // Draw thread only, so these are not shared. The lock below copies into them and does no
    // allocation: the game threads spin on g_ext_locked, and an allocation here could wait on a
    // lock they hold.
    static std::vector<PendingExtent> taken;
    static std::vector<RingExtent> whole_blocks;
    if (taken.capacity() == 0) {
        taken.reserve(kMaxPending);
        whole_blocks.reserve(kMaxBlocks);
    }
    taken.clear();
    whole_blocks.clear();
    {
        ExtentLock lock;
        // Every pending extent has an epoch <= the current one; the prefix test keeps the rule
        // exact if that ever changes (the list is ordered by epoch).
        size_t n = 0;
        while (n < g_pending.size() && g_pending[n].epoch <= epoch) {
            ++n;
        }
        taken.assign(g_pending.begin(), g_pending.begin() + static_cast<std::ptrdiff_t>(n));
        g_pending.erase(g_pending.begin(), g_pending.begin() + static_cast<std::ptrdiff_t>(n));
        // Whole-block fallbacks: a block flagged since the last take is handed over whole.
        const size_t blocks = g_block_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < blocks; ++i) {
            if (g_blocks[i].whole_pending.exchange(false, std::memory_order_relaxed)) {
                whole_blocks.push_back(RingExtent{g_blocks[i].base.load(std::memory_order_relaxed),
                                                  g_blocks[i].size.load(std::memory_order_relaxed)});
            }
        }
    }
    // Outside the lock: the allocations of the output.
    out.reserve(out.size() + taken.size() + whole_blocks.size());
    for (const PendingExtent& extent : taken) {
        out.push_back(RingExtent{extent.addr, extent.size});
        bytes += extent.size;
    }
    count = taken.size();
    for (const RingExtent& block : whole_blocks) {
        out.push_back(block);
        bytes += block.size;
        ++count;
        ++whole;
    }
    {
        // The take enters the history (hot mask, coverage). Merged, so the lists stay short.
        std::vector<RingExtent> merged(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
        MergeExtents(merged);
        std::lock_guard<std::mutex> hist(g_hist_mutex);
        g_hist[g_take_seq % kHistory] = std::move(merged);
        ++g_take_seq;
    }
    g_takes.fetch_add(1, std::memory_order_relaxed);
    g_taken.fetch_add(count, std::memory_order_relaxed);
    g_taken_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_ext_whole.fetch_add(whole, std::memory_order_relaxed);
}

void TakeRingEnters(std::vector<RingExtent>& out) {
    if (Mode() != 6 && Mode() != 7) {
        return;
    }
    // Pops what is queued now, at most one queue's worth (producers may keep pushing meanwhile;
    // their entries wait for the next take).
    uint64_t count = 0;
    uint64_t pages = 0;
    for (size_t i = 0; i < kEnterQueue; ++i) {
        uint64_t addr = 0;
        uint64_t size = 0;
        if (!EnterPop(addr, size)) {
            break;
        }
        out.push_back(RingExtent{addr, size});
        ++count;
        pages += size >> kGranuleShift;
    }
    g_enter_calls.fetch_add(count, std::memory_order_relaxed);
    g_enter_pages.fetch_add(pages, std::memory_order_relaxed);
}

std::vector<RingExtent> ArenaBlocks(uint64_t& version) {
    std::vector<RingExtent> live;
    std::lock_guard<std::mutex> lock(g_block_mutex);
    version = g_block_version.load(std::memory_order_relaxed);
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        const Block& b = g_blocks[i];
        if (b.active.load(std::memory_order_relaxed)) {
            live.push_back(RingExtent{b.base.load(std::memory_order_relaxed),
                                      b.size.load(std::memory_order_relaxed)});
        }
    }
    // Mode 8 heap runs. A run that overlaps an arena block is left out (the block covers it).
    if (const auto* extras = g_extras.load(std::memory_order_acquire)) {
        const size_t blocks = live.size();
        for (const RingExtent& e : *extras) {
            bool overlaps = false;
            for (size_t i = 0; i < blocks && !overlaps; ++i) {
                overlaps = e.addr < live[i].addr + live[i].size && live[i].addr < e.addr + e.size;
            }
            if (!overlaps) {
                live.push_back(e);
            }
        }
    }
    return live;
}

void SetHostExtras(std::vector<RingExtent> runs) {
    std::sort(runs.begin(), runs.end(),
              [](const RingExtent& a, const RingExtent& b) { return a.addr < b.addr; });
    std::lock_guard<std::mutex> lock(g_block_mutex);
    const auto* now = g_extras.load(std::memory_order_relaxed);
    if (now && now->size() == runs.size() &&
        std::equal(runs.begin(), runs.end(), now->begin(), [](const RingExtent& a, const RingExtent& b) {
            return a.addr == b.addr && a.size == b.size;
        })) {
        return;
    }
    if (!now && runs.empty()) {
        return;
    }
    auto next = std::make_unique<const std::vector<RingExtent>>(std::move(runs));
    g_extras.store(next.get(), std::memory_order_release);
    g_extras_kept.push_back(std::move(next));
    g_block_version.fetch_add(1, std::memory_order_relaxed);
}

bool InArena(uint64_t addr, uint64_t size) {
    const uint64_t end = addr + size;
    if (!size || end < addr || !On()) {
        return false;
    }
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        const Block& b = g_blocks[i];
        if (!b.active.load(std::memory_order_relaxed)) {
            continue;
        }
        const uint64_t base = b.base.load(std::memory_order_relaxed);
        if (addr < base + b.size.load(std::memory_order_relaxed) && base < end) {
            return true;
        }
    }
    return false;
}

void SnapshotExtents(const std::vector<RingExtent>& extents) {
    if (!VerifyOn() || extents.empty()) {
        return;
    }
    std::vector<RingExtent> sorted(extents);
    std::sort(sorted.begin(), sorted.end(),
              [](const RingExtent& a, const RingExtent& b) { return a.addr < b.addr; });
    std::lock_guard<std::mutex> lock(g_verify_mutex);
    Snapshot& s = g_snaps[g_snap_seq % kSnapshots];
    s.epoch = g_epoch.load(std::memory_order_acquire);
    s.upl_seq = g_upl_seq.load(std::memory_order_acquire);
    s.pieces.clear();
    s.bytes.clear();
    bool open = false;
    Piece cur{0, 0, 0};
    auto close = [&] {
        if (open) {
            s.pieces.push_back(cur);
            open = false;
        }
    };
    // Copies the guest bytes [a, z), inside one page, into the snapshot.
    auto copy = [&](uint64_t a, uint64_t z) {
        const unsigned char* src = runtime_memory_backing_pointer(static_cast<uintptr_t>(a));
        if (!src) {
            g_unreadable.fetch_add(1, std::memory_order_relaxed);
            close();
            return;
        }
        const uint64_t len = z - a;
        if (s.bytes.size() + len > kSnapshotMaxBytes) {
            g_snap_skipped.fetch_add(len, std::memory_order_relaxed);
            close();
            return;
        }
        if (!open || cur.end != a) {
            close();
            cur = Piece{a, a, s.bytes.size()};
            open = true;
        }
        s.bytes.insert(s.bytes.end(), src, src + len);
        cur.end = z;
    };
    size_t i = 0;
    while (i < sorted.size()) {
        const uint64_t lo = sorted[i].addr;
        uint64_t hi = lo + sorted[i].size;
        ++i;
        while (i < sorted.size() && sorted[i].addr <= hi) {
            hi = std::max(hi, sorted[i].addr + sorted[i].size);
            ++i;
        }
        if (hi <= lo) {
            continue;
        }
        for (uint64_t p = lo & ~kPageMask; p < hi; p += kPage) {
            copy(std::max(lo, p), std::min(hi, p + kPage));
        }
    }
    close();
    ++g_snap_seq;
}

namespace {

// Totals of one checked bind.
struct VerifyTotals {
    uint64_t compared = 0;
    uint64_t divergent = 0;
    uint64_t nosnap = 0;
    uint64_t skip_dirty = 0; // mode 7: bytes of CPU-dirty pages (uploaded by this bind)
    uint64_t skip_sup = 0;   // mode 7: bytes of pages uploaded after the snapshot
    uint64_t first_off = 0; // bind-relative offset of the first divergent byte
    uint32_t first_age = 0; // submits since the take of the snapshot holding that byte
    bool first_set = false;
};

// Compares the guest bytes [x0, x1) with the snapshot run pc (held in s.bytes), byte by byte. The
// backing view is read per page, as the snapshot was taken.
// Mode 7 only: true when the bytes [c, z) of one page are not compared. Such a page is CPU-dirty
// at the bind (the bind uploads it, so the guest bytes are the ones the GPU gets), or it was
// uploaded after the snapshot (the upload is the newer copy of the bytes, and NoteUpload stored
// its sequence on the page). Caller: CompareRun, with the bind's totals.
bool SkipForMode7(const Snapshot& s, uint64_t c, uint64_t z, VerifyTotals& t) {
    if (Mode() != 7) {
        return false;
    }
    Block* b = nullptr;
    uint64_t g = 0;
    if (Find(c, &b, &g) && b->upl_seq &&
        b->upl_seq[g].load(std::memory_order_acquire) > s.upl_seq) {
        t.skip_sup += z - c;
        return true;
    }
    const RingDirtyFn dirty = g_dirty.load(std::memory_order_acquire);
    if (dirty && dirty(c & ~kPageMask, kPage)) {
        t.skip_dirty += z - c;
        return true;
    }
    return false;
}

void CompareRun(const Snapshot& s, const Piece& pc, uint64_t x0, uint64_t x1, uint64_t bind_lo,
                VerifyTotals& t) {
    for (uint64_t c = x0; c < x1;) {
        const uint64_t z = std::min(x1, (c & ~kPageMask) + kPage);
        if (SkipForMode7(s, c, z, t)) {
            c = z;
            continue;
        }
        const unsigned char* guest = runtime_memory_backing_pointer(static_cast<uintptr_t>(c));
        if (guest) { // unreadable now: not compared
            const unsigned char* held = s.bytes.data() + pc.off + (c - pc.addr);
            const uint64_t len = z - c;
            t.compared += len;
            if (std::memcmp(held, guest, len) != 0) {
                for (uint64_t i = 0; i < len; ++i) {
                    if (held[i] == guest[i]) {
                        continue;
                    }
                    ++t.divergent;
                    if (!t.first_set) {
                        t.first_set = true;
                        t.first_off = c + i - bind_lo;
                        t.first_age = g_epoch.load(std::memory_order_acquire) - s.epoch;
                    }
                }
            }
        }
        c = z;
    }
}

// Byte-precise compare of the bind [lo, hi): each byte is compared with the newest snapshot that
// covers it. The bytes left after the snapshots are counted as no snapshot. Caller holds
// g_verify_mutex.
void VerifyBind(uint64_t lo, uint64_t hi, VerifyTotals& t) {
    const uint64_t seq = g_snap_seq;
    const uint64_t valid = std::min<uint64_t>(seq, kSnapshots);
    std::vector<Span> todo{Span{lo, hi, 0}};
    for (uint64_t k = 0; k < valid && !todo.empty(); ++k) {
        const Snapshot& s = g_snaps[(seq - 1 - k) % kSnapshots];
        std::vector<Span> next;
        for (const Span& sp : todo) {
            uint64_t cur = sp.a;
            auto it = std::lower_bound(s.pieces.begin(), s.pieces.end(), sp.a,
                                       [](const Piece& pc, uint64_t v) { return pc.end <= v; });
            for (; it != s.pieces.end() && it->addr < sp.z; ++it) {
                const uint64_t x0 = std::max(sp.a, it->addr);
                const uint64_t x1 = std::min(sp.z, it->end);
                if (x0 >= x1) {
                    continue;
                }
                if (cur < x0) {
                    next.push_back(Span{cur, x0, 0});
                }
                CompareRun(s, *it, x0, x1, lo, t);
                cur = x1;
            }
            if (cur < sp.z) {
                next.push_back(Span{cur, sp.z, 0});
            }
        }
        todo.swap(next);
    }
    for (const Span& sp : todo) {
        t.nosnap += sp.z - sp.a;
    }
}

// Coverage of a checked bind: its arena bytes, each classed by the newest take (age 0 = this
// submit's take) whose extents cover it; bytes in no take of the last kHistory are bucket 4. The
// GPU-written count uses the per-page epoch of the last GPU write (NoteGpuWrite). Caller holds
// g_verify_mutex.
void ClassifyBind(uint64_t lo, uint64_t hi) {
    std::vector<Span> todo;
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        const Block& b = g_blocks[i];
        if (!b.active.load(std::memory_order_relaxed)) {
            continue;
        }
        const uint64_t base = b.base.load(std::memory_order_relaxed);
        const uint64_t end = base + b.size.load(std::memory_order_relaxed);
        const uint64_t x0 = std::max(lo, base);
        const uint64_t x1 = std::min(hi, end);
        if (x0 < x1) {
            todo.push_back(Span{x0, x1, kCovBuckets - 1});
        }
    }
    if (todo.empty()) {
        return;
    }
    std::vector<Span> done;
    {
        std::lock_guard<std::mutex> hist(g_hist_mutex);
        const uint64_t takes = std::min<uint64_t>(g_take_seq, kHistory);
        for (uint64_t age = 0; age < takes && !todo.empty(); ++age) {
            const std::vector<RingExtent>& ext = g_hist[(g_take_seq - 1 - age) % kHistory];
            const int bucket = age == 0 ? 0 : age < 4 ? 1 : age < 16 ? 2 : 3;
            std::vector<Span> next;
            for (const Span& sp : todo) {
                uint64_t cur = sp.a;
                auto it = std::lower_bound(
                    ext.begin(), ext.end(), sp.a,
                    [](const RingExtent& e, uint64_t v) { return e.addr + e.size <= v; });
                for (; it != ext.end() && it->addr < sp.z; ++it) {
                    const uint64_t x0 = std::max(sp.a, it->addr);
                    const uint64_t x1 = std::min(sp.z, it->addr + it->size);
                    if (x0 >= x1) {
                        continue;
                    }
                    if (cur < x0) {
                        next.push_back(Span{cur, x0, 0});
                    }
                    done.push_back(Span{x0, x1, bucket});
                    cur = x1;
                }
                if (cur < sp.z) {
                    next.push_back(Span{cur, sp.z, 0});
                }
            }
            todo.swap(next);
        }
    }
    for (Span sp : todo) {
        sp.bucket = kCovBuckets - 1;
        done.push_back(sp);
    }
    const uint32_t now = g_epoch.load(std::memory_order_acquire);
    for (const Span& sp : done) {
        g_cov_bytes[sp.bucket].fetch_add(sp.z - sp.a, std::memory_order_relaxed);
        for (uint64_t p = sp.a & ~kPageMask; p < sp.z; p += kPage) {
            Block* b = nullptr;
            uint64_t g = 0;
            if (!Find(std::max(sp.a, p), &b, &g)) {
                continue;
            }
            const uint32_t w = b->gpu_epoch[g].load(std::memory_order_relaxed);
            if (w == 0 || now - w > 64) {
                continue;
            }
            g_cov_gpu[sp.bucket].fetch_add(std::min(sp.z, p + kPage) - std::max(sp.a, p),
                                           std::memory_order_relaxed);
        }
    }
}

// The first divergent binds (cumulative, not reset by Report), guarded by g_verify_mutex.
struct DivergentBind {
    uint64_t addr;
    uint64_t size;
    uint32_t age;
    uint64_t first_off;
    uint64_t bytes;
};
constexpr size_t kDivergentLog = 16;
std::vector<DivergentBind> g_div_log;

} // namespace

void CheckBind(uint64_t addr, uint64_t size) {
    if (!VerifyOn() || !size || addr + size <= addr || !InArena(addr, size)) {
        return;
    }
    const uint64_t seen = g_v_binds.fetch_add(1, std::memory_order_relaxed);
    if (seen % VerifySample() != 0) {
        return;
    }
    const uint64_t lo = addr;
    const uint64_t hi = addr + size;
    std::lock_guard<std::mutex> lock(g_verify_mutex);
    g_v_checked.fetch_add(1, std::memory_order_relaxed);
    VerifyTotals t;
    VerifyBind(lo, hi, t);
    g_v_compared.fetch_add(t.compared, std::memory_order_relaxed);
    g_v_divergent.fetch_add(t.divergent, std::memory_order_relaxed);
    g_v_nosnap.fetch_add(t.nosnap, std::memory_order_relaxed);
    g_v_skip_dirty.fetch_add(t.skip_dirty, std::memory_order_relaxed);
    g_v_skip_sup.fetch_add(t.skip_sup, std::memory_order_relaxed);
    if (t.divergent) {
        g_v_div_binds.fetch_add(1, std::memory_order_relaxed);
        if (g_div_log.size() < kDivergentLog) {
            const DivergentBind d{lo, size, t.first_age, t.first_off, t.divergent};
            g_div_log.push_back(d);
            std::fprintf(stderr,
                         "SOTC PC RING verify divergent bind %zu/%zu: addr %llx size %llx, "
                         "snapshot age %u submits, first differing byte +%llx, %llu bytes differ\n",
                         g_div_log.size(), kDivergentLog, static_cast<unsigned long long>(d.addr),
                         static_cast<unsigned long long>(d.size), d.age,
                         static_cast<unsigned long long>(d.first_off),
                         static_cast<unsigned long long>(d.bytes));
        }
    }
    ClassifyBind(lo, hi);
}

void HotRingPages(std::vector<RingExtent>& out, uint64_t& version) {
    out.clear();
    if (Mode() != 5) {
        version = 0;
        return;
    }
    std::lock_guard<std::mutex> hist(g_hist_mutex);
    // The mask is the union of the whole-page runs of each take in the window (see WholePageRuns),
    // the same definition HotRingDelta keeps page by page. A page with any byte outside one take's
    // extents stays under normal protection, so its writes fault and are uploaded.
    std::vector<RingExtent> all;
    const uint64_t k = std::min<uint64_t>(g_take_seq, HotEpochs());
    for (uint64_t i = 0; i < k; ++i) {
        WholePageRuns(g_hist[(g_take_seq - 1 - i) % kHistory], all);
    }
    MergeExtents(all);
    out = all;
    uint64_t pages = 0;
    for (const RingExtent& e : out) {
        pages += e.size >> kGranuleShift;
    }
    // Check against the incremental state (HotRingDelta, run earlier in this take): same page set.
    uint64_t diff = pages > g_hot_pages.size() ? pages - g_hot_pages.size()
                                               : g_hot_pages.size() - pages;
    for (const RingExtent& e : out) {
        for (uint64_t p = e.addr; p < e.addr + e.size; p += kPage) {
            if (g_hot_pages.find(p) == g_hot_pages.end()) {
                ++diff;
            }
        }
    }
    g_hot_full_checks.fetch_add(1, std::memory_order_relaxed);
    if (diff) {
        g_hot_mismatch.fetch_add(diff, std::memory_order_relaxed);
    }
    const bool same = out.size() == g_hot_last.size() &&
                      std::equal(out.begin(), out.end(), g_hot_last.begin(),
                                 [](const RingExtent& a, const RingExtent& b) {
                                     return a.addr == b.addr && a.size == b.size;
                                 });
    if (!same) {
        g_hot_last = out;
        ++g_hot_version;
        g_hot_changes.fetch_add(1, std::memory_order_relaxed);
    }
    version = g_hot_version;
    g_hot_calls.fetch_add(1, std::memory_order_relaxed);
    g_hot_last_pages.store(pages, std::memory_order_relaxed);
    g_hot_last_extents.store(out.size(), std::memory_order_relaxed);
}

void HotRingDelta(std::vector<RingExtent>& enter, std::vector<RingExtent>& leave) {
    enter.clear();
    leave.clear();
    if (Mode() != 5) {
        return;
    }
    const uint64_t k = HotEpochs();
    std::vector<uint64_t> entered;
    std::vector<uint64_t> left;
    {
        std::lock_guard<std::mutex> hist(g_hist_mutex);
        if (g_take_seq == 0) {
            return;
        }
        const uint64_t take = g_take_seq - 1; // the take just recorded by TakeSubmitExtents
        // Enter first: a page this take covers gets its newest take, so the expiry below does not
        // see its older entry as leaving (no page joins and leaves in one call).
        HotTake now{take, {}};
        WholePageRuns(g_hist[take % kHistory], now.runs);
        for (const RingExtent& run : now.runs) {
            for (uint64_t p = run.addr; p < run.addr + run.size; p += kPage) {
                const auto [it, inserted] = g_hot_pages.try_emplace(p, 0);
                if (inserted) {
                    entered.push_back(p);
                }
                it->second = take + 1;
            }
        }
        if (!now.runs.empty()) {
            g_hot_queue.push_back(std::move(now));
        }
        // Leave: the takes that left the window (take + K <= current). A page leaves when its
        // newest entry is one of them: no later take covered it.
        while (!g_hot_queue.empty() && g_hot_queue.front().take + k <= take) {
            const HotTake& old = g_hot_queue.front();
            for (const RingExtent& run : old.runs) {
                for (uint64_t p = run.addr; p < run.addr + run.size; p += kPage) {
                    const auto it = g_hot_pages.find(p);
                    if (it != g_hot_pages.end() && it->second == old.take + 1) {
                        g_hot_pages.erase(it);
                        left.push_back(p);
                    }
                }
            }
            g_hot_queue.pop_front();
        }
    }
    PagesToExtents(entered, enter);
    PagesToExtents(left, leave);
    g_delta_calls.fetch_add(1, std::memory_order_relaxed);
    g_delta_enter.fetch_add(entered.size(), std::memory_order_relaxed);
    g_delta_leave.fetch_add(left.size(), std::memory_order_relaxed);
    uint64_t prev = g_delta_enter_max.load(std::memory_order_relaxed);
    while (entered.size() > prev &&
           !g_delta_enter_max.compare_exchange_weak(prev, entered.size(), std::memory_order_relaxed)) {
    }
    prev = g_delta_leave_max.load(std::memory_order_relaxed);
    while (left.size() > prev &&
           !g_delta_leave_max.compare_exchange_weak(prev, left.size(), std::memory_order_relaxed)) {
    }
}

void NoteHotDeltaApply(uint64_t ns) {
    g_delta_applies.fetch_add(1, std::memory_order_relaxed);
    g_delta_apply_ns.fetch_add(ns, std::memory_order_relaxed);
}

void NoteRingCopy(uint64_t bytes, bool imported) {
    if (!FullRingMode()) {
        return;
    }
    g_copies.fetch_add(1, std::memory_order_relaxed);
    g_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    (imported ? g_copies_imported : g_copies_fallback).fetch_add(1, std::memory_order_relaxed);
}

void NoteRingFault() {
    if (FullRingMode()) {
        g_ring_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void SetRingDirtyProbe(RingDirtyFn fn) {
    g_dirty.store(fn, std::memory_order_release);
}

void NoteRingLeave(uint64_t pages) {
    if (FullRingMode() >= 6) {
        g_leave_pages.fetch_add(pages, std::memory_order_relaxed);
    }
}

int ArenaRelation(uint64_t addr, uint64_t size) {
    const uint64_t end = addr + size;
    if (Mode() != 8 || !size || end < addr) {
        return 0;
    }
    int relation = 0;
    // Mode 8 heap runs (SetHostExtras): sorted and disjoint, so the run holding addr is the last one
    // starting at or below it, and a run after it overlaps the bind only when it starts before end.
    if (const auto* extras = g_extras.load(std::memory_order_acquire); extras && !extras->empty()) {
        auto it = std::upper_bound(extras->begin(), extras->end(), addr,
                                   [](uint64_t a, const RingExtent& e) { return a < e.addr; });
        if (it != extras->begin()) {
            const RingExtent& e = *std::prev(it);
            if (addr < e.addr + e.size) {
                if (end <= e.addr + e.size) {
                    return 1;
                }
                relation = 2;
            }
        }
        if (it != extras->end() && it->addr < end) {
            relation = 2;
        }
    }
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        const Block& b = g_blocks[i];
        if (!b.active.load(std::memory_order_relaxed)) {
            continue;
        }
        const uint64_t base = b.base.load(std::memory_order_relaxed);
        const uint64_t block_size = b.size.load(std::memory_order_relaxed);
        if (!(addr < base + block_size && base < end)) {
            continue;
        }
        if (addr >= base && end <= base + block_size) {
            return 1;
        }
        relation = 2;
    }
    return relation;
}

uint64_t BlockVersion() {
    return g_block_version.load(std::memory_order_relaxed);
}

uint32_t SubmitEpoch() {
    return g_epoch.load(std::memory_order_acquire);
}

void NoteHostBlocks(uint64_t count, uint64_t bytes) {
    g_h_blocks.store(count, std::memory_order_relaxed);
    g_h_block_bytes.store(bytes, std::memory_order_relaxed);
}

void NoteHostBind(uint64_t bytes, bool written) {
    g_h_binds.fetch_add(1, std::memory_order_relaxed);
    g_h_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (written) {
        g_h_written.fetch_add(1, std::memory_order_relaxed);
        g_h_write_seq.fetch_add(1, std::memory_order_release);
    }
}

void NoteHostStraddle(bool served) {
    g_h_straddles.fetch_add(1, std::memory_order_relaxed);
    if (served) {
        g_h_straddles_served.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteHostClip() {
    g_h_clips.fetch_add(1, std::memory_order_relaxed);
}

void NoteHostFallback() {
    g_h_fallbacks.fetch_add(1, std::memory_order_relaxed);
}

uint64_t HostWriteSeq() {
    return g_h_write_seq.load(std::memory_order_acquire);
}

void NoteHostUnique(uint64_t read_bytes, uint64_t write_bytes) {
    g_h_unique_read.fetch_add(read_bytes, std::memory_order_relaxed);
    g_h_unique_write.fetch_add(write_bytes, std::memory_order_relaxed);
}

void NoteHostMark(uint64_t marked, uint64_t skipped) {
    g_h_mark.fetch_add(marked, std::memory_order_relaxed);
    g_h_mark_skip.fetch_add(skipped, std::memory_order_relaxed);
}

void NoteHostLabel(bool deferred) {
    g_h_labels.fetch_add(1, std::memory_order_relaxed);
    if (deferred) {
        g_h_labels_deferred.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteHostLabelExcluded() {
    g_h_labels_excluded.fetch_add(1, std::memory_order_relaxed);
}

void NoteHostCpWaitFlush() {
    g_h_cp_wait_flush.fetch_add(1, std::memory_order_relaxed);
}

void NoteHostLabelForced() {
    g_h_labels_forced.fetch_add(1, std::memory_order_relaxed);
}

bool Ring8Verify() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PC_RING8_VERIFY");
        return Mode() == 8 && v && v[0] == '1';
    }();
    return on;
}

bool Ring8Group() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PC_RING8_GROUP");
        return v && v[0] == '1';
    }();
    return on;
}

uint32_t Ring8VerifySample() {
    static const uint32_t sample = [] {
        const char* v = std::getenv("SOTC_PC_RING8_VERIFY_SAMPLE");
        const long n = v ? std::atol(v) : 32;
        return n < 1 ? 1u : static_cast<uint32_t>(n);
    }();
    return sample;
}

void NoteHostVerify(bool written, uint64_t bytes, bool changed) {
    if (written) {
        g_v8_writes.fetch_add(1, std::memory_order_relaxed);
        g_v8_write_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (changed) {
            g_v8_write_changed.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_v8_reads.fetch_add(1, std::memory_order_relaxed);
        g_v8_read_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (changed) {
            g_v8_read_changed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Mode 8 report lines (Report prints nothing else in this mode). The read traffic is the unique
// bytes per submit, summed over the submits of the window, per frame (the GPU reads host memory
// per submit; overlapping binds of one submit are read once).
void ReportHost(std::string& out, double seconds) {
    const uint64_t frames = g_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t binds = g_h_binds.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = g_h_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t written = g_h_written.exchange(0, std::memory_order_relaxed);
    const uint64_t straddles = g_h_straddles.exchange(0, std::memory_order_relaxed);
    const uint64_t served = g_h_straddles_served.exchange(0, std::memory_order_relaxed);
    const uint64_t fallbacks = g_h_fallbacks.exchange(0, std::memory_order_relaxed);
    const uint64_t clips = g_h_clips.exchange(0, std::memory_order_relaxed);
    const uint64_t uniq_r = g_h_unique_read.exchange(0, std::memory_order_relaxed);
    const uint64_t uniq_w = g_h_unique_write.exchange(0, std::memory_order_relaxed);
    const uint64_t labels = g_h_labels.exchange(0, std::memory_order_relaxed);
    const uint64_t deferred = g_h_labels_deferred.exchange(0, std::memory_order_relaxed);
    const uint64_t excluded = g_h_labels_excluded.exchange(0, std::memory_order_relaxed);
    const uint64_t cp_flushes = g_h_cp_wait_flush.exchange(0, std::memory_order_relaxed);
    const uint64_t forced = g_h_labels_forced.exchange(0, std::memory_order_relaxed);
    const uint64_t marked = g_h_mark.exchange(0, std::memory_order_relaxed);
    const uint64_t skipped = g_h_mark_skip.exchange(0, std::memory_order_relaxed);
    const double mib = 1024.0 * 1024.0;
    char line[480];
    std::snprintf(line, sizeof line,
                  "SOTC PC RING host: blocks imported %llu (%llu MiB), binds served from host "
                  "%.0f/s (summed %.2f MiB/s), unique read %s MiB/frame, unique written %s MiB/frame, "
                  "straddles %llu (served %llu), clipped %llu, fallbacks %llu, written binds %llu\n",
                  static_cast<unsigned long long>(g_h_blocks.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_h_block_bytes.load(std::memory_order_relaxed) >>
                                                  20),
                  static_cast<double>(binds) / seconds,
                  static_cast<double>(bytes) / seconds / mib,
                  frames ? std::to_string(static_cast<double>(uniq_r) / mib / frames).c_str()
                         : "n/a (no frame count)",
                  frames ? std::to_string(static_cast<double>(uniq_w) / mib / frames).c_str()
                         : "n/a (no frame count)",
                  static_cast<unsigned long long>(straddles),
                  static_cast<unsigned long long>(served),
                  static_cast<unsigned long long>(clips),
                  static_cast<unsigned long long>(fallbacks),
                  static_cast<unsigned long long>(written));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC PC RING host labels: EOP labels %llu, deferred to the GPU %llu, excluded by "
                  "lookahead %llu, CP waits that flushed %llu, forced %llu; normal-path "
                  "CPU marks %.2f MiB, skipped (marked in the same submit) %.2f MiB\n",
                  static_cast<unsigned long long>(labels), static_cast<unsigned long long>(deferred),
                  static_cast<unsigned long long>(excluded),
                  static_cast<unsigned long long>(cp_flushes),
                  static_cast<unsigned long long>(forced),
                  static_cast<double>(marked) / mib, static_cast<double>(skipped) / mib);
    out += line;
    if (Ring8FlipWaitOn()) { // sotc: the flip wait (BufferCache::PcHostFlipWait)
        const uint64_t fw_calls = g_fw_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t fw_waits = g_fw_waits.exchange(0, std::memory_order_relaxed);
        const uint64_t fw_ns = g_fw_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t fw_max = g_fw_max_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t fw_halves = g_fw_halves.exchange(0, std::memory_order_relaxed);
        std::snprintf(line, sizeof line,
                      "SOTC PC RING8 flip waits %llu (avg %.1f us, max %.3f ms), halves tracked "
                      "%.2f per flip, flips %llu%s\n",
                      static_cast<unsigned long long>(fw_waits),
                      fw_waits ? static_cast<double>(fw_ns) / 1000.0 / static_cast<double>(fw_waits)
                               : 0.0,
                      static_cast<double>(fw_max) / 1e6,
                      fw_calls ? static_cast<double>(fw_halves) / static_cast<double>(fw_calls) : 0.0,
                      static_cast<unsigned long long>(fw_calls),
                      frames == 0 && fw_calls == 0 ? " (frame flip hook off: set SOTC_HOOK_FRAME_FLIP=1)"
                                                   : "");
        out += line;
    }
    if (Ring8Verify()) {
        const uint64_t rd = g_v8_reads.exchange(0, std::memory_order_relaxed);
        const uint64_t rdb = g_v8_read_bytes.exchange(0, std::memory_order_relaxed);
        const uint64_t rdc = g_v8_read_changed.exchange(0, std::memory_order_relaxed);
        const uint64_t wr = g_v8_writes.exchange(0, std::memory_order_relaxed);
        const uint64_t wrb = g_v8_write_bytes.exchange(0, std::memory_order_relaxed);
        const uint64_t wrc = g_v8_write_changed.exchange(0, std::memory_order_relaxed);
        std::snprintf(line, sizeof line,
                      "SOTC PC RING host verify (1 in %u): read binds checked %llu (%.2f MiB), "
                      "changed before the next submit %llu; written binds checked %llu (%.2f MiB), "
                      "changed before the next submit %llu\n",
                      Ring8VerifySample(), static_cast<unsigned long long>(rd),
                      static_cast<double>(rdb) / mib, static_cast<unsigned long long>(rdc),
                      static_cast<unsigned long long>(wr), static_cast<double>(wrb) / mib,
                      static_cast<unsigned long long>(wrc));
        out += line;
    }
}

void Report(std::string& out, double seconds) {
    SotcArenaAttrib::Report(out, seconds); // sotc: SOTC_ARENA_ATTRIB window (own switch)
    if (!On() || seconds <= 0.0) {
        return;
    }
    if (Mode() == 8) { // sotc: host arena binds, counted apart from the modes below
        ReportHost(out, seconds);
        return;
    }
    const uint64_t frames = g_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t submits = g_submits.exchange(0, std::memory_order_relaxed);
    const uint64_t faults = g_faults.exchange(0, std::memory_order_relaxed);
    const uint64_t reup = g_reuploads.exchange(0, std::memory_order_relaxed);
    const uint64_t uploads = g_uploads.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = g_upload_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t diverg = g_divergent.exchange(0, std::memory_order_relaxed);
    const uint64_t diverg_sweep = g_divergent_sweep.exchange(0, std::memory_order_relaxed);
    uint64_t calls[kSiteCount];
    uint64_t hits[kSiteCount];
    for (int s = 0; s < kSiteCount; ++s) {
        calls[s] = g_site_calls[s].exchange(0, std::memory_order_relaxed);
        hits[s] = g_site_hits[s].exchange(0, std::memory_order_relaxed);
    }
    const uint64_t declares = g_declares.exchange(0, std::memory_order_relaxed);
    const uint64_t declared = g_declared_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t unread = g_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t overflow = g_block_overflow.load(std::memory_order_relaxed);
    uint64_t arena = 0;
    size_t active = 0;
    const size_t n = g_block_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (g_blocks[i].active.load(std::memory_order_relaxed)) {
            arena += g_blocks[i].size.load(std::memory_order_relaxed);
            ++active;
        }
    }
    if (!frames && !submits && !faults && !uploads) {
        return;
    }
    if (FullRingMode()) {
        const uint64_t takes = g_takes.exchange(0, std::memory_order_relaxed);
        const uint64_t taken = g_taken.exchange(0, std::memory_order_relaxed);
        const uint64_t taken_bytes = g_taken_bytes.exchange(0, std::memory_order_relaxed);
        const uint64_t allocs = g_ext_allocs.exchange(0, std::memory_order_relaxed);
        const uint64_t ext_over = g_ext_overflow.exchange(0, std::memory_order_relaxed);
        const uint64_t whole = g_ext_whole.exchange(0, std::memory_order_relaxed);
        const uint64_t copy_bytes = g_copy_bytes.exchange(0, std::memory_order_relaxed);
        const uint64_t imported = g_copies_imported.exchange(0, std::memory_order_relaxed);
        const uint64_t fallback = g_copies_fallback.exchange(0, std::memory_order_relaxed);
        const uint64_t ring_faults = g_ring_faults.exchange(0, std::memory_order_relaxed);
        const uint64_t v_binds = g_v_binds.exchange(0, std::memory_order_relaxed);
        const uint64_t v_checked = g_v_checked.exchange(0, std::memory_order_relaxed);
        const uint64_t v_compared = g_v_compared.exchange(0, std::memory_order_relaxed);
        const uint64_t v_div = g_v_divergent.exchange(0, std::memory_order_relaxed);
        const uint64_t v_div_binds = g_v_div_binds.exchange(0, std::memory_order_relaxed);
        const uint64_t v_nosnap = g_v_nosnap.exchange(0, std::memory_order_relaxed);
        const uint64_t snap_skip = g_snap_skipped.exchange(0, std::memory_order_relaxed);
        const double per_frame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
        char verify[400];
        if (VerifyOn()) {
            std::snprintf(verify, sizeof verify,
                          "verify (1 in %llu arena binds): binds %llu, checked %llu, bytes compared "
                          "%.1f MiB, divergent %llu bytes in %llu binds, no snapshot %.1f MiB "
                          "(snapshot cap skipped %.1f MiB)",
                          static_cast<unsigned long long>(VerifySample()),
                          static_cast<unsigned long long>(v_binds),
                          static_cast<unsigned long long>(v_checked), v_compared / 1048576.0,
                          static_cast<unsigned long long>(v_div),
                          static_cast<unsigned long long>(v_div_binds), v_nosnap / 1048576.0,
                          snap_skip / 1048576.0);
        } else {
            std::snprintf(verify, sizeof verify, "verify off (mode %d)", Mode());
        }
        char line[1100];
        std::snprintf(
            line, sizeof line,
            "SOTC PC RING full (mode %d): %llu frames, %llu submits, arena %zu blocks %.0f MiB, "
            "arena allocs %.0f/s; extents %.1f/submit, %.2f MiB/frame (whole-block %llu); copies "
            "imported %llu, fallback %llu (%.2f MiB/frame); %s; overflow %llu allocs, block table "
            "%llu; ring faults %llu\n",
            Mode(), static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(submits), active, arena / 1048576.0,
            allocs / seconds,
            takes ? static_cast<double>(taken) / static_cast<double>(takes) : 0.0,
            taken_bytes / 1048576.0 * per_frame, static_cast<unsigned long long>(whole),
            static_cast<unsigned long long>(imported), static_cast<unsigned long long>(fallback),
            copy_bytes / 1048576.0 * per_frame, verify,
            static_cast<unsigned long long>(ext_over), static_cast<unsigned long long>(overflow),
            static_cast<unsigned long long>(ring_faults));
        out += line;
        if (VerifyOn()) {
            // Coverage of the checked binds' arena bytes. Window K = buckets for takes 0 .. K-1
            // back (K = 1, 4, 16, 64); the rest of the bytes are (b) for that window.
            uint64_t cov[kCovBuckets];
            uint64_t cov_gpu[kCovBuckets];
            uint64_t cov_total = 0;
            uint64_t gpu_total = 0;
            for (int b = 0; b < kCovBuckets; ++b) {
                cov[b] = g_cov_bytes[b].exchange(0, std::memory_order_relaxed);
                cov_gpu[b] = g_cov_gpu[b].exchange(0, std::memory_order_relaxed);
                cov_total += cov[b];
                gpu_total += cov_gpu[b];
            }
            const uint64_t gpu_writes = g_gpu_write_bytes.exchange(0, std::memory_order_relaxed);
            const uint64_t a1 = cov[0];
            const uint64_t a4 = a1 + cov[1];
            const uint64_t a16 = a4 + cov[2];
            const uint64_t a64 = a16 + cov[3];
            const double total = cov_total ? static_cast<double>(cov_total) : 1.0;
            char cov_line[600];
            std::snprintf(
                cov_line, sizeof cov_line,
                "SOTC PC RING coverage (checked arena bytes %.2f MiB): covered by an extent of "
                "the last K submits, K=1/4/16/64: %.1f/%.1f/%.1f/%.1f%% (b, not covered: "
                "%.1f/%.1f/%.1f/%.1f%%, of which %.1f%% in no extent of 64 submits); GPU-written "
                "within 64 submits: %.1f%% of the bytes (%.1f%% of the (b) bytes at K=64); "
                "NoteGpuWrite %.2f MiB\n",
                cov_total / 1048576.0, 100.0 * a1 / total, 100.0 * a4 / total,
                100.0 * a16 / total, 100.0 * a64 / total, 100.0 * (cov_total - a1) / total,
                100.0 * (cov_total - a4) / total, 100.0 * (cov_total - a16) / total,
                100.0 * (cov_total - a64) / total, 100.0 * cov[kCovBuckets - 1] / total,
                100.0 * gpu_total / total,
                100.0 * (cov_gpu[kCovBuckets - 1]) /
                    static_cast<double>(std::max<uint64_t>(cov_total - a64, 1)),
                gpu_writes / 1048576.0);
            out += cov_line;
        }
        if (Mode() >= 6) {
            // sotc: modes 6 and 7. Pages entering at allocation and leaving at the takes, per submit.
            const uint64_t enter_calls = g_enter_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t enter_pages = g_enter_pages.exchange(0, std::memory_order_relaxed);
            const uint64_t enter_dropped = g_enter_dropped.exchange(0, std::memory_order_relaxed);
            const uint64_t leave_pages = g_leave_pages.exchange(0, std::memory_order_relaxed);
            const uint64_t skip_dirty = g_v_skip_dirty.exchange(0, std::memory_order_relaxed);
            const uint64_t skip_sup = g_v_skip_sup.exchange(0, std::memory_order_relaxed);
            const double per_submit = submits ? 1.0 / static_cast<double>(submits) : 0.0;
            char safe_line[480];
            std::snprintf(
                safe_line, sizeof safe_line,
                "SOTC PC RING protect-then-copy (mode %d): entered %.1f pages/submit (%.0f "
                "allocs/s, %.2f MiB/frame), left %.1f pages/submit; verify skipped (mode 7) "
                "dirty %.2f MiB, superseded %.2f MiB\n",
                Mode(), enter_pages * per_submit, enter_calls / seconds,
                enter_pages * kPage / 1048576.0 * (frames ? 1.0 / static_cast<double>(frames) : 0.0),
                leave_pages * per_submit, skip_dirty / 1048576.0, skip_sup / 1048576.0);
            out += safe_line;
            if (enter_dropped) {
                char drop_line[160];
                std::snprintf(drop_line, sizeof drop_line,
                              "SOTC PC RING entry queue full: %llu allocations not entered "
                              "(their pages keep normal protection)\n",
                              static_cast<unsigned long long>(enter_dropped));
                out += drop_line;
            }
        }
        if (Mode() == 5) {
            const uint64_t calls = g_hot_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t changes = g_hot_changes.exchange(0, std::memory_order_relaxed);
            char hot_line[300];
            std::snprintf(
                hot_line, sizeof hot_line,
                "SOTC PC RING hot mask (last %llu of %zu takes): %llu pages (%.1f MiB) in %llu "
                "extents; changed on %llu of %llu calls\n",
                static_cast<unsigned long long>(HotEpochs()), kHistory,
                static_cast<unsigned long long>(g_hot_last_pages.load(std::memory_order_relaxed)),
                g_hot_last_pages.load(std::memory_order_relaxed) * kPage / 1048576.0,
                static_cast<unsigned long long>(g_hot_last_extents.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(changes), static_cast<unsigned long long>(calls));
            out += hot_line;
            const uint64_t dcalls = g_delta_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t denter = g_delta_enter.exchange(0, std::memory_order_relaxed);
            const uint64_t dleave = g_delta_leave.exchange(0, std::memory_order_relaxed);
            const uint64_t dmax_in = g_delta_enter_max.exchange(0, std::memory_order_relaxed);
            const uint64_t dmax_out = g_delta_leave_max.exchange(0, std::memory_order_relaxed);
            const uint64_t dapplies = g_delta_applies.exchange(0, std::memory_order_relaxed);
            const uint64_t dns = g_delta_apply_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t fchecks = g_hot_full_checks.exchange(0, std::memory_order_relaxed);
            const uint64_t fmis = g_hot_mismatch.exchange(0, std::memory_order_relaxed);
            char delta_line[400];
            std::snprintf(
                delta_line, sizeof delta_line,
                "SOTC PC RING hot delta: %llu takes, pages in %.1f per take (max %llu), out %.1f "
                "(max %llu); apply %.3f ms per delta apply (%llu applies); full checks %llu, "
                "mismatches %llu\n",
                static_cast<unsigned long long>(dcalls),
                dcalls ? static_cast<double>(denter) / static_cast<double>(dcalls) : 0.0,
                static_cast<unsigned long long>(dmax_in),
                dcalls ? static_cast<double>(dleave) / static_cast<double>(dcalls) : 0.0,
                static_cast<unsigned long long>(dmax_out),
                dapplies ? static_cast<double>(dns) / 1e6 / static_cast<double>(dapplies) : 0.0,
                static_cast<unsigned long long>(dapplies), static_cast<unsigned long long>(fchecks),
                static_cast<unsigned long long>(fmis));
            out += delta_line;
        }
        return;
    }
    if (Mode() == 1) {
        // Hooks are installed by switch, so the sites are known only after InstallHooks.
        const char* const hook_names[kSiteCount] = {"PC_RING_HEAP", "PC_RING_BUMP",
                                                    "PC_RING_ALLOC"};
        std::string sites;
        for (int s = 0; s < kSiteCount; ++s) {
            if (!SotcHooks::IsInstalled(hook_names[s])) {
                continue;
            }
            char part[160];
            std::snprintf(part, sizeof part, "%s%s: hook calls %.0f/s, arena allocs %.0f/s",
                          sites.empty() ? "" : "; ", kSiteNames[s], calls[s] / seconds,
                          hits[s] / seconds);
            sites += part;
        }
        if (sites.empty()) {
            sites = "none installed";
        }
        char line[640];
        std::snprintf(line, sizeof line,
                      "SOTC PC RING apply (%s): %llu frames, %llu submits, arena %zu blocks "
                      "%.0f MiB; declares %.0f/s %.2f MiB/frame; write faults in the arena "
                      "%.0f/s%s\n",
                      sites.c_str(), static_cast<unsigned long long>(frames),
                      static_cast<unsigned long long>(submits), active, arena / 1048576.0,
                      declares / seconds,
                      frames ? declared / 1048576.0 / static_cast<double>(frames) : 0.0,
                      faults / seconds, overflow ? " (block table full)" : "");
        out += line;
        // sotc: DeclareRingWrite counters (SOTC_PC_RING_SLOW=1: no calls here).
        const auto rd = VideoCore::PageManager::GetRingDeclareStats(true);
        if (rd.calls) {
            char rl[400];
            std::snprintf(rl, sizeof rl,
                          "SOTC PC RING declare (fast): calls %llu, skipped %llu (nothing "
                          "protected), fast runs %llu (%llu pages), fallback runs %llu (image or "
                          "owed bytes), draw drains %llu, %.1f us per call\n",
                          static_cast<unsigned long long>(rd.calls),
                          static_cast<unsigned long long>(rd.skipped),
                          static_cast<unsigned long long>(rd.fast_runs),
                          static_cast<unsigned long long>(rd.pages),
                          static_cast<unsigned long long>(rd.fallback_runs),
                          static_cast<unsigned long long>(rd.drains),
                          rd.ns / 1000.0 / static_cast<double>(rd.calls));
            out += rl;
        }
        return;
    }
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC PC RING shadow: %llu frames, %llu submits, arena %zu blocks %.0f MiB; "
                  "write faults %.0f/s; uploads %.0f/s %.2f MiB/frame; re-uploads in one submit "
                  "%llu pages, divergent %llu in binds + %llu in DMA sweeps, unreadable %llu%s\n",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(submits),
                  active, arena / 1048576.0, faults / seconds, uploads / seconds,
                  frames ? bytes / 1048576.0 / static_cast<double>(frames) : 0.0,
                  static_cast<unsigned long long>(reup), static_cast<unsigned long long>(diverg),
                  static_cast<unsigned long long>(diverg_sweep),
                  static_cast<unsigned long long>(unread), overflow ? " (block table full)" : "");
    out += line;
}

} // namespace SotcPcRing
