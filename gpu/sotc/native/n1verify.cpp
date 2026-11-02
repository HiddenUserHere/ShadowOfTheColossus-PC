// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: diagnostic, see n1verify.h. Everything here is lock-free except Report (one mutex-free pass over
// fixed tables, run by the report thread): the fault handler, the announce path and the bind path use
// atomics and fixed tables only, so the sub-mode cannot deadlock or allocate in a fault.
// ---------------------------------------------------------------------------------------------------------------
// HOW TO READ THE FRAME-SOURCE LINES (jitter brief). Needs SOTC_NATIVE=1 SOTC_VERIFY=n1 SOTC_CP_AHEAD=2 (frame tickets).
//
// Frame words. C = CPU frame = SubmitDone count (the CPU is building frame C + 1). rec = frames the draw thread
// finished recording. R = rec + 1 is the frame the draw thread records now. "ahead" = C - rec: 0 = the CPU is still
// building the frame the GPU records now (the safe case); 1 = it finished it and builds the next one; 2, 3+ = further.
//
// FRAMES line: rec_calls (frame ends seen), rec_jumps (a frame end that skipped numbers: the idle frame-end path, R is
// then only a lower bound; with tickets this must be ~0).
//
// BIND lines, one per bind kind (vertex index const storage texel image other); binds counts ObtainBuffer calls,
// memo= bind-memo hits, ahead = the CPU frames ahead of the draw thread at the bind (0 1 2 3+). Announce checks (the
// announce record of each page the bind covers; found = a record exists):
//   early      the newest announce of a covered page was made in a LATER CPU frame than R: this draw reads a page the
//              CPU already rewrote for a following frame (heap slot reused before the draw read it). THE SMOKING GUN.
//   undrained  the announce is not drained yet (queued after this submit's drain): the bind sees the range before the
//              port learned of its write. same = its frame is R (a late announce), old = an earlier frame (stuck).
//   changed    a sample hash of the guest bytes at bind time differs from the hash taken at announce time: the bytes
//              were written again after the announce (an announce made before the fill finished, or a later write).
//              changed_early = of those, the announce was also of a later frame.
//   lastwrite  the newest write FAULT on a covered page (record-only protection), its frame W = C_at_fault + 1:
//              rel = W - R, buckets -3- -2 -1 0 +1 +2 +3+ ; +n means the page was last written n frames AFTER the frame
//              the GPU is recording (a later frame's write is in the bytes the bind uploads), 0 = same frame (correct),
//              negative = older. none = no fault on those pages (unprotected, or never uploaded before).
// DRAIN line: pieces taken from the announce queue; exact = copied byte-exact at drain time; late = the page had been
// bound (first bind) in an earlier submit than this drain (late announce: that bind could not see the write);
// changed = the bytes at drain time differ from the announce-time hash; exact_changed = the same for exact pieces
// (the bytes copied are not the bytes announced); lag = CPU frames between the announce and the drain
// (0 1 2 3+, by the newest record of the piece).
// A lines (announcer table): per announcer (host return address of Announce, tag: for FillBuffer / CopyBuffer),
// announces, pages, and its binds: early / undrained / changed split by bind kind letters v i c s t m o
// (vertex index const storage texel image=m other=o), plus late (drain after first bind) and drain changes.
// WB lines (writer table): per writer RIP, binds whose newest write fault on a covered page was that writer's,
// as later / same / older than R, split by the same bind kind letters. later > 0 = that writer's later-frame data
// reached a bind of an earlier frame.
// MISS lines: bound_miss by allocator class (name/kind/slot of the block, or the memtype heap), and in the W rows
// miss_last=addr size class of the last missed bind. Class 'unclassified' = no block or granule was fed for it.
// Caveats: a 4 KiB page keeps ONE announce record (the newest; same-frame adjacent pieces are merged), the hash is a
// 64-word sample per page, binds over 1024 pages (image binds over 64) are looked at with a stride, and the bind-cache
// paths other than the memo hit never reach ObtainBuffer and are not seen.
// ---------------------------------------------------------------------------------------------------------------
#include "n1verify.h"

#include "native.h"
#include "verify.h"
#include "../arena_attrib.h"
#include "../guest_mem.h"
#include "video_core/page_manager.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace SotcN1Verify {
namespace {

constexpr uint64_t kPageShift = 12;
constexpr size_t kRowBits = 11;
constexpr size_t kRowSlots = size_t{1} << kRowBits;
constexpr size_t kProbe = 32;
constexpr size_t kPendingBits = 12;
constexpr size_t kPendingSlots = size_t{1} << kPendingBits;
constexpr size_t kSuspectBits = 14;
constexpr size_t kSuspectSlots = size_t{1} << kSuspectBits;
constexpr size_t kRearmSlots = 8192;
constexpr uint64_t kFaultBytes = 8; // the bytes of a fault the coverage test looks at (the faulting address)
constexpr uint32_t kEarlyWindows = 2; // windows 0 and 1 (the first 10 s) are "early"
constexpr size_t kTopWrite = 20;
constexpr size_t kTopRead = 12;
constexpr size_t kTopNew = 12;
constexpr uint64_t kSmallBind = 64 * 1024;
constexpr size_t kKinds = static_cast<size_t>(SotcVerify::BindKind::Count);
constexpr size_t kAnnBits = 16;
constexpr size_t kAnnSlots = size_t{1} << kAnnBits;
constexpr size_t kLastBits = 16;
constexpr size_t kLastSlots = size_t{1} << kLastBits;
constexpr size_t kAnnRowBits = 9;
constexpr size_t kAnnRowSlots = size_t{1} << kAnnRowBits;
constexpr uint64_t kMaxAnnPages = 512;   // pages of one announce that get a record
constexpr uint64_t kMaxBindPages = 1024; // pages of one buffer bind that are looked at (stride beyond)
constexpr uint64_t kMaxImagePages = 64;  // the same for an image bind
constexpr uint64_t kMaxHashPages = 64;   // hash comparisons per bind
constexpr size_t kTopAnn = 16;
constexpr size_t kTopWb = 12;
constexpr size_t kClassSlots = 48;
// Order of SotcVerify::BindKind: Vertex Index Constant Storage Texel Image Other.
const char kKindLetter[kKinds + 1] = "vicstmo";
const char* const kKindName[kKinds] = {"vertex", "index", "const", "storage", "texel", "image", "other"};

size_t Slot(uint64_t key, size_t bits) {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

// One row per (RIP, who). Window counters are reset by Report; cumulative ones are not.
struct Row {
    std::atomic<uint64_t> key{0}; // (rip << 2 | who) | 1 << 63; 0 = free
    std::atomic<uint32_t> first_win{0};
    std::atomic<uint64_t> last_addr{0};
    std::atomic<uint64_t> faults{0};   // window: write faults recorded
    std::atomic<uint64_t> covered{0};  // window: ... covered by an announce before the submit start
    std::atomic<uint64_t> unann{0};    // window: ... not covered at the submit start
    std::atomic<uint64_t> unann_pc{0}; // window: ... of those, an announce touched the page (not the address)
    std::atomic<uint64_t> miss{0};     // window: ... of those, bound by the GPU before any announce
    std::atomic<uint64_t> miss_pc{0};  // window: ... of the misses, page touched by an announce meanwhile
    std::atomic<uint64_t> miss_small{0}; // window: ... of the misses, a bind of at most 64 KiB
    std::atomic<uint64_t> reads{0};    // window: read faults recorded
    std::atomic<uint64_t> c_faults{0};
    std::atomic<uint64_t> c_unann{0};
    std::atomic<uint64_t> c_miss{0};
    std::atomic<uint64_t> c_reads{0};
    std::atomic<uint64_t> miss_addr{0}; // last missed bind: address, size and allocator class (index + 1)
    std::atomic<uint64_t> miss_size{0};
    std::atomic<uint32_t> miss_class{0};
    // Binds whose newest write fault on a covered page was this writer's: [0] later than R, [1] = R, [2] older.
    std::atomic<uint64_t> bw[3][kKinds]{};
};
Row g_rows[kRowSlots];
std::atomic<uint64_t> g_rows_dropped{0};
std::atomic<uint32_t> g_window{0};

constexpr uint64_t RowKey(uint64_t rip, Who who) {
    return ((rip << 2) | static_cast<uint64_t>(who)) | (uint64_t{1} << 63);
}

Row* FindRow(uint64_t key, bool claim) {
    const size_t h = Slot(key, kRowBits);
    for (size_t i = 0; i < kProbe; ++i) {
        Row& r = g_rows[(h + i) & (kRowSlots - 1)];
        const uint64_t k = r.key.load(std::memory_order_acquire);
        if (k == key) {
            return &r;
        }
        if (k == 0 && claim) {
            uint64_t expect = 0;
            if (r.key.compare_exchange_strong(expect, key, std::memory_order_acq_rel)) {
                r.first_win.store(g_window.load(std::memory_order_relaxed), std::memory_order_relaxed);
                return &r;
            }
            if (expect == key) {
                return &r;
            }
        }
    }
    return nullptr;
}

// Window totals.
struct Totals {
    std::atomic<uint64_t> write_faults[3]{};
    std::atomic<uint64_t> read_faults[3]{};
    std::atomic<uint64_t> covered{0};
    std::atomic<uint64_t> unann{0};
    std::atomic<uint64_t> unann_pc{0};
    std::atomic<uint64_t> miss_buf{0};
    std::atomic<uint64_t> miss_img{0};
    std::atomic<uint64_t> miss_pc{0};
    std::atomic<uint64_t> miss_small{0};
    std::atomic<uint64_t> miss_standing{0};
    std::atomic<uint64_t> bind_page_only{0}; // a bind covered the page of a suspect but not its written address
    std::atomic<uint64_t> late_covered{0};   // a suspect cleared by an announce before any bind
    std::atomic<uint64_t> age[5]{};          // frames from the write to the first bind: 0, 1, 2-3, 4-15, 16+
    std::atomic<uint64_t> rearm_pages{0};
    std::atomic<uint64_t> rearm_runs{0};
    std::atomic<uint64_t> rearm_dropped{0};
    std::atomic<uint64_t> pending_dropped{0};
    std::atomic<uint64_t> suspect_replaced{0};
    std::atomic<uint64_t> loop_breaks{0};
    std::atomic<uint64_t> raced{0};
    std::atomic<uint64_t> submits{0};
};
Totals g_t;
std::atomic<uint64_t> g_cum_write_faults{0};
std::atomic<uint64_t> g_cum_read_faults{0};
std::atomic<uint64_t> g_cum_unann{0};
std::atomic<uint64_t> g_cum_miss{0};

// ---- Frame source state (see the guide at the top) ----
std::atomic<uint64_t> g_rec{0};  // frames the draw thread finished recording (PcHostNoteFrameRecorded)
std::atomic<uint64_t> g_sub{0};  // submit starts seen (OnSubmitStart)
std::atomic<uint64_t> g_aseq{0}; // announce sequence number

// The newest announce record of a page. Direct mapped; a colliding page takes the slot over. Writers: any thread
// (announce) and the draw thread (drained, bind_sub). key is zeroed first and written last (release).
struct AnnRec {
    std::atomic<uint64_t> key{0}; // page + 1
    std::atomic<uint64_t> aseq{0};
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> caller{0};
    std::atomic<uint32_t> cf{0};       // CPU frame (SubmitDone count) at the announce
    std::atomic<uint32_t> bind_sub{0}; // submit index of the first bind after the announce, 0 = not bound yet
    std::atomic<uint16_t> lo{0};       // announced bytes inside the page: [lo, hi)
    std::atomic<uint16_t> hi{0};
    std::atomic<uint8_t> kind{0};
    std::atomic<uint8_t> drained{0};
    std::atomic<uint8_t> hashed{0};
};
AnnRec g_ann[kAnnSlots];

// The newest write fault of a page.
struct LastWrite {
    std::atomic<uint64_t> key{0}; // page + 1
    std::atomic<uint64_t> row_key{0};
    std::atomic<uint32_t> cf{0};
};
LastWrite g_lastw[kLastSlots];

// Per announcer.
struct AnnRow {
    std::atomic<uint64_t> key{0}; // (caller << 2 | kind & 3) | 1 << 63
    std::atomic<uint64_t> announces{0};
    std::atomic<uint64_t> pages{0};
    std::atomic<uint64_t> bind_early[kKinds]{};
    std::atomic<uint64_t> bind_undrained[kKinds]{};
    std::atomic<uint64_t> bind_changed[kKinds]{};
    std::atomic<uint64_t> late{0}; // drained in a later submit than the first bind of a page
    std::atomic<uint64_t> drain_changed{0};
    std::atomic<uint64_t> c_early{0};
    std::atomic<uint64_t> c_late{0};
    std::atomic<uint64_t> c_changed{0};
};
AnnRow g_annrows[kAnnRowSlots];
std::atomic<uint64_t> g_annrows_dropped{0};

AnnRow* FindAnnRow(uint64_t key) {
    const size_t h = Slot(key, kAnnRowBits);
    for (size_t i = 0; i < kProbe; ++i) {
        AnnRow& r = g_annrows[(h + i) & (kAnnRowSlots - 1)];
        const uint64_t k = r.key.load(std::memory_order_acquire);
        if (k == key) {
            return &r;
        }
        if (k == 0) {
            uint64_t expect = 0;
            if (r.key.compare_exchange_strong(expect, key, std::memory_order_acq_rel) || expect == key) {
                return &r;
            }
        }
    }
    g_annrows_dropped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

constexpr uint64_t AnnKey(uint64_t caller, uint8_t kind) {
    return ((caller << 2) | (kind & 3u)) | (uint64_t{1} << 63);
}

// Window counters of the frame-source checks.
struct FrameTotals {
    std::atomic<uint64_t> rec_calls{0};
    std::atomic<uint64_t> rec_jumps{0};
    std::atomic<uint64_t> binds[kKinds]{};
    std::atomic<uint64_t> memo_binds[kKinds]{};
    std::atomic<uint64_t> ahead[kKinds][4]{};
    std::atomic<uint64_t> found[kKinds]{};
    std::atomic<uint64_t> early[kKinds]{};
    std::atomic<uint64_t> undrained[kKinds]{};
    std::atomic<uint64_t> undrained_same[kKinds]{};
    std::atomic<uint64_t> undrained_old[kKinds]{};
    std::atomic<uint64_t> changed[kKinds]{};
    std::atomic<uint64_t> changed_early[kKinds]{};
    std::atomic<uint64_t> lastw[kKinds][8]{}; // rel -3-.. +3+ at 0..6, none at 7
    std::atomic<uint64_t> sampled[kKinds]{};  // binds looked at with a stride
    std::atomic<uint64_t> drain_pieces{0};
    std::atomic<uint64_t> drain_exact{0};
    std::atomic<uint64_t> drain_found{0};
    std::atomic<uint64_t> drain_late{0};
    std::atomic<uint64_t> drain_late_bytes{0};
    std::atomic<uint64_t> drain_changed{0};
    std::atomic<uint64_t> drain_exact_changed{0};
    std::atomic<uint64_t> drain_lag[4]{};
    std::atomic<uint64_t> ann_total{0};
    std::atomic<uint64_t> ann_oversize{0};
    std::atomic<uint64_t> ann_evicted{0}; // a live undrained record replaced by another page
    std::atomic<uint64_t> ann_nohash{0};
};
FrameTotals g_f;
std::atomic<uint64_t> g_cum_early{0};
std::atomic<uint64_t> g_cum_late{0};
std::atomic<uint64_t> g_cum_changed{0};

// Allocator classes: the blocks the probes feed (any thread, mutex: never touched by the fault handler) and the
// classes of the missed binds (draw thread and report thread).
struct BlockInfo {
    uint64_t size;
    char name[28];
    int16_t kind;
    int16_t slot;
};
std::mutex g_blk_mutex;
std::map<uint64_t, BlockInfo> g_blocks;
std::map<uint64_t, BlockInfo> g_granules;

struct ClassSlot {
    char name[56];
    uint64_t miss_buf, miss_img;
    uint64_t kinds[kKinds];
};
std::mutex g_cls_mutex;
ClassSlot g_classes[kClassSlots];
size_t g_class_count = 0;

// The class name of addr: the eboot image, an allocator block (name, kind, slot) or a memtype granule (heap label).
void ClassName(uint64_t addr, char* out, size_t n) {
    uint64_t off = 0;
    if (SotcArenaAttrib::ImageOffset(addr, &off)) {
        std::snprintf(out, n, "eboot-image");
        return;
    }
    std::lock_guard<std::mutex> lock(g_blk_mutex);
    auto it = g_blocks.upper_bound(addr);
    if (it != g_blocks.begin()) {
        --it;
        if (addr < it->first + it->second.size) {
            std::snprintf(out, n, "alloc:%s/k%d/s%d", it->second.name, it->second.kind, it->second.slot);
            return;
        }
    }
    it = g_granules.upper_bound(addr);
    if (it != g_granules.begin()) {
        --it;
        if (addr < it->first + it->second.size) {
            std::snprintf(out, n, "heap:%s", it->second.name);
            return;
        }
    }
    std::snprintf(out, n, "unclassified");
}

// Index + 1 of the class slot of name (claims one; the last slot takes the overflow).
uint32_t InternClass(const char* name) {
    std::lock_guard<std::mutex> lock(g_cls_mutex);
    for (size_t i = 0; i < g_class_count; ++i) {
        if (std::strncmp(g_classes[i].name, name, sizeof g_classes[i].name - 1) == 0) {
            return static_cast<uint32_t>(i + 1);
        }
    }
    if (g_class_count < kClassSlots) {
        ClassSlot& c = g_classes[g_class_count];
        std::snprintf(c.name, sizeof c.name, "%s", name);
        return static_cast<uint32_t>(++g_class_count);
    }
    return static_cast<uint32_t>(kClassSlots);
}

// Sample hash of the bytes [lo, hi) of a page: up to 64 words spread over the range.
uint64_t PageHash(const uint8_t* page, uint32_t lo, uint32_t hi) {
    lo = (lo + 7u) & ~7u;
    hi &= ~7u;
    if (hi <= lo) {
        return 0;
    }
    const uint32_t words = (hi - lo) / 8;
    const uint32_t stride = std::max<uint32_t>(1, words / 64);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t w = 0; w < words; w += stride) {
        uint64_t v;
        std::memcpy(&v, page + lo + w * 8, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

const uint8_t* PagePtr(uint64_t page) {
    return SotcGuest::Backing(page << kPageShift, size_t{1} << kPageShift);
}

// The part of [addr, addr + size) inside a page, as byte offsets.
void PageSpan(uint64_t page, uint64_t addr, uint64_t size, uint32_t* lo, uint32_t* hi) {
    const uint64_t base = page << kPageShift;
    *lo = static_cast<uint32_t>(addr > base ? addr - base : 0);
    const uint64_t end = addr + size;
    *hi = static_cast<uint32_t>(end - base > 4096 ? 4096 : end - base);
}

uint32_t CpuFrame() {
    return static_cast<uint32_t>(SotcNative::FrameSubmitted());
}

size_t AheadBucket(uint64_t ahead) {
    return ahead > 3 ? 3 : static_cast<size_t>(ahead);
}


// Pending writes: the page, the faulting address, the row, the frame, whether an announce touched the page.
struct Pending {
    std::atomic<uint64_t> key{0}; // page + 1
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> row_key{0};
    std::atomic<uint64_t> frame{0};
    std::atomic<bool> pagecov{false};
};
Pending g_pending[kPendingSlots];
std::atomic<int64_t> g_pending_live{0};

// Suspects: unannounced writes the GPU may still read. Direct mapped by page.
struct Suspect {
    std::atomic<uint64_t> key{0}; // page + 1
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> row_key{0};
    std::atomic<uint64_t> frame{0};
    std::atomic<bool> pagecov{false};
    std::atomic<bool> bound{false};
};
Suspect g_suspects[kSuspectSlots];
std::atomic<int64_t> g_suspect_live{0};

// Pages the handler opened, to protect again at the next submit start. Producers: any thread; consumer: the
// draw thread (g_rearm_tail is its alone).
std::atomic<uint64_t> g_rearm[kRearmSlots]; // page + 1, 0 = empty
std::atomic<uint64_t> g_rearm_head{0};
uint64_t g_rearm_tail = 0;

Pending* FindPending(uint64_t page, bool claim, bool* created) {
    const size_t h = Slot(page, kPendingBits);
    for (size_t i = 0; i < kProbe; ++i) {
        Pending& p = g_pending[(h + i) & (kPendingSlots - 1)];
        const uint64_t k = p.key.load(std::memory_order_acquire);
        if (k == page + 1) {
            return &p;
        }
        if (k == 0 && claim) {
            uint64_t expect = 0;
            if (p.key.compare_exchange_strong(expect, page + 1, std::memory_order_acq_rel)) {
                if (created) {
                    *created = true;
                }
                return &p;
            }
            if (expect == page + 1) {
                return &p;
            }
        }
    }
    return nullptr;
}

bool Intersects(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}

void PushRearm(uint64_t page) {
    const uint64_t idx = g_rearm_head.fetch_add(1, std::memory_order_acq_rel);
    g_rearm[idx % kRearmSlots].store(page + 1, std::memory_order_release);
}

// "0x<eboot offset>" inside the image, "host:<module>+0x<offset>" for port code (report thread only).
void FormatRip(char* buf, size_t n, uint64_t addr) {
    uint64_t off = 0;
    if (SotcArenaAttrib::ImageOffset(addr, &off)) {
        std::snprintf(buf, n, "0x%llx", static_cast<unsigned long long>(off));
        return;
    }
#ifdef _WIN32
    HMODULE module = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &module) &&
        GetModuleFileNameA(module, path, sizeof path) != 0) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(buf, n, "host:%s+0x%llx", name ? name + 1 : path,
                      static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(module)));
        return;
    }
#endif
    std::snprintf(buf, n, "host:0x%llx", static_cast<unsigned long long>(addr));
}

const char* WhoName(Who who) {
    switch (who) {
    case Who::Guest:
        return "guest";
    case Who::HostGpu:
        return "host-gpu";
    default:
        return "host-other";
    }
}

} // namespace

bool InitOn() {
    const char* native = std::getenv("SOTC_NATIVE");
    const char* verify = std::getenv("SOTC_VERIFY");
    const bool v = native && native[0] == '1' && verify && (verify[0] == 'n' || verify[0] == 'N') &&
                   verify[1] == '1' && verify[2] == '\0';
    if (v) {
        std::fprintf(stderr, "SOTC N1VERIFY: on (mode 1 plus record-only page protection; diagnostic)\n");
    }
    return v;
}

Who Classify(uint64_t rip, bool gpu_side_thread) {
    uint64_t off = 0;
    if (SotcArenaAttrib::ImageOffset(rip, &off)) {
        return Who::Guest;
    }
    return gpu_side_thread ? Who::HostGpu : Who::HostOther;
}

void NoteWriteFault(uint64_t addr, uint64_t rip, Who who) {
    const uint64_t key = RowKey(rip, who);
    const uint64_t page = addr >> kPageShift;
    g_t.write_faults[static_cast<size_t>(who)].fetch_add(1, std::memory_order_relaxed);
    g_cum_write_faults.fetch_add(1, std::memory_order_relaxed);
    if (Row* r = FindRow(key, true)) {
        r->faults.fetch_add(1, std::memory_order_relaxed);
        r->c_faults.fetch_add(1, std::memory_order_relaxed);
        r->last_addr.store(addr, std::memory_order_relaxed);
    } else {
        g_rows_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    {   // the newest write of the page: CPU frame and writer, for the frame-source check at the bind
        LastWrite& lw = g_lastw[Slot(page, kLastBits)];
        lw.key.store(0, std::memory_order_relaxed);
        lw.row_key.store(key, std::memory_order_relaxed);
        lw.cf.store(CpuFrame(), std::memory_order_relaxed);
        lw.key.store(page + 1, std::memory_order_release);
    }
    bool created = false;
    Pending* p = FindPending(page, true, &created);
    if (!p) {
        g_t.pending_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        p->addr.store(addr, std::memory_order_relaxed);
        p->row_key.store(key, std::memory_order_relaxed);
        p->frame.store(SotcVerify::Frame(), std::memory_order_relaxed);
        if (created) {
            p->pagecov.store(false, std::memory_order_relaxed);
            g_pending_live.fetch_add(1, std::memory_order_relaxed);
        }
    }
    PushRearm(page);
}

void NoteReadFault(uint64_t addr, uint64_t rip, Who who) {
    const uint64_t key = RowKey(rip, who);
    g_t.read_faults[static_cast<size_t>(who)].fetch_add(1, std::memory_order_relaxed);
    g_cum_read_faults.fetch_add(1, std::memory_order_relaxed);
    if (Row* r = FindRow(key, true)) {
        r->reads.fetch_add(1, std::memory_order_relaxed);
        r->c_reads.fetch_add(1, std::memory_order_relaxed);
        r->last_addr.store(addr, std::memory_order_relaxed);
    } else {
        g_rows_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    PushRearm(addr >> kPageShift);
}

void NoteLoopBreak() {
    g_t.loop_breaks.fetch_add(1, std::memory_order_relaxed);
}

void NoteRaced() {
    g_t.raced.fetch_add(1, std::memory_order_relaxed);
}

namespace {
void NoteAnnounceSuspects(uint64_t addr, uint64_t size);

// The announce record of every page of [addr, addr + size) (the first kMaxAnnPages): CPU frame, announcer, sample
// hash. Adjacent or overlapping pieces of the same frame and kind merge into one record per page.
void RecordAnnounce(uint64_t addr, uint64_t size, uint8_t kind, const void* caller) {
    const uint64_t aseq = g_aseq.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t cf = CpuFrame();
    const uint64_t first = addr >> kPageShift;
    uint64_t last = (addr + size - 1) >> kPageShift;
    g_f.ann_total.fetch_add(1, std::memory_order_relaxed);
    if (last - first >= kMaxAnnPages) {
        g_f.ann_oversize.fetch_add(1, std::memory_order_relaxed);
        last = first + kMaxAnnPages - 1;
    }
    const uint64_t ckey = AnnKey(reinterpret_cast<uint64_t>(caller), kind);
    if (AnnRow* row = FindAnnRow(ckey)) {
        row->announces.fetch_add(1, std::memory_order_relaxed);
        row->pages.fetch_add(last - first + 1, std::memory_order_relaxed);
    }
    for (uint64_t page = first; page <= last; ++page) {
        uint32_t lo = 0, hi = 0;
        PageSpan(page, addr, size, &lo, &hi);
        AnnRec& r = g_ann[Slot(page, kAnnBits)];
        const uint64_t old = r.key.load(std::memory_order_acquire);
        if (old == page + 1 && r.cf.load(std::memory_order_relaxed) == cf &&
            r.kind.load(std::memory_order_relaxed) == kind && r.drained.load(std::memory_order_relaxed) == 0 &&
            lo <= r.hi.load(std::memory_order_relaxed) && hi >= r.lo.load(std::memory_order_relaxed)) {
            lo = std::min<uint32_t>(lo, r.lo.load(std::memory_order_relaxed)); // same frame: union the bytes
            hi = std::max<uint32_t>(hi, r.hi.load(std::memory_order_relaxed));
        } else if (old != 0 && old != page + 1 && r.drained.load(std::memory_order_relaxed) == 0) {
            g_f.ann_evicted.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t hash = 0;
        uint8_t hashed = 0;
        if (const uint8_t* p = PagePtr(page)) {
            hash = PageHash(p, lo, hi);
            hashed = 1;
        } else {
            g_f.ann_nohash.fetch_add(1, std::memory_order_relaxed);
        }
        r.key.store(0, std::memory_order_relaxed);
        r.aseq.store(aseq, std::memory_order_relaxed);
        r.hash.store(hash, std::memory_order_relaxed);
        r.caller.store(reinterpret_cast<uint64_t>(caller), std::memory_order_relaxed);
        r.cf.store(cf, std::memory_order_relaxed);
        r.bind_sub.store(0, std::memory_order_relaxed);
        r.lo.store(static_cast<uint16_t>(lo), std::memory_order_relaxed);
        r.hi.store(static_cast<uint16_t>(hi), std::memory_order_relaxed);
        r.kind.store(kind, std::memory_order_relaxed);
        r.drained.store(0, std::memory_order_relaxed);
        r.hashed.store(hashed, std::memory_order_relaxed);
        r.key.store(page + 1, std::memory_order_release);
    }
}
} // namespace

void NoteAnnounce(uint64_t addr, uint64_t size, uint8_t kind, const void* caller) {
    if (size == 0 || !On()) {
        return;
    }
    if (kind != kAnnGpuWritten) {
        RecordAnnounce(addr, size, kind, caller);
    }
    NoteAnnounceSuspects(addr, size);
}

void NoteRelease(uint64_t addr, uint64_t size) {
    if (size == 0 || !On()) {
        return;
    }
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    if (last - first < 4096) {
        for (uint64_t page = first; page <= last; ++page) {
            AnnRec& r = g_ann[Slot(page, kAnnBits)];
            uint64_t expect = page + 1;
            r.key.compare_exchange_strong(expect, 0, std::memory_order_acq_rel);
        }
        return;
    }
    for (AnnRec& r : g_ann) {
        const uint64_t k = r.key.load(std::memory_order_acquire);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            uint64_t expect = k;
            r.key.compare_exchange_strong(expect, 0, std::memory_order_acq_rel);
        }
    }
}

void NoteFrameRecorded(uint64_t seq) {
    const uint64_t prev = g_rec.exchange(seq, std::memory_order_relaxed);
    g_f.rec_calls.fetch_add(1, std::memory_order_relaxed);
    if (prev != 0 && seq > prev + 1) {
        g_f.rec_jumps.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteDrain(uint64_t addr, uint64_t size, bool exact) {
    if (size == 0 || !On()) {
        return;
    }
    const uint64_t now_aseq = g_aseq.load(std::memory_order_relaxed);
    const uint32_t cf_now = CpuFrame();
    g_f.drain_pieces.fetch_add(1, std::memory_order_relaxed);
    if (exact) {
        g_f.drain_exact.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t first = addr >> kPageShift;
    uint64_t last = (addr + size - 1) >> kPageShift;
    if (last - first >= kMaxAnnPages) {
        last = first + kMaxAnnPages - 1;
    }
    bool found = false, late = false, changed = false;
    uint32_t lag = 0;
    uint64_t caller = 0;
    uint8_t kind = 0;
    for (uint64_t page = first; page <= last; ++page) {
        AnnRec& r = g_ann[Slot(page, kAnnBits)];
        if (r.key.load(std::memory_order_acquire) != page + 1) {
            continue;
        }
        uint32_t plo = 0, phi = 0;
        PageSpan(page, addr, size, &plo, &phi);
        const uint32_t rlo = r.lo.load(std::memory_order_relaxed);
        const uint32_t rhi = r.hi.load(std::memory_order_relaxed);
        if (phi <= rlo || plo >= rhi) {
            continue;
        }
        if (!found) {
            found = true;
            lag = cf_now - r.cf.load(std::memory_order_relaxed);
            caller = r.caller.load(std::memory_order_relaxed);
            kind = r.kind.load(std::memory_order_relaxed);
        }
        if (r.drained.load(std::memory_order_relaxed) == 0 && r.aseq.load(std::memory_order_relaxed) <= now_aseq) {
            r.drained.store(1, std::memory_order_relaxed);
            if (r.bind_sub.load(std::memory_order_relaxed) != 0) {
                late = true; // a bind of this page came before this drain
            }
        }
        if (r.hashed.load(std::memory_order_relaxed)) {
            if (const uint8_t* p = PagePtr(page)) {
                changed |= PageHash(p, rlo, rhi) != r.hash.load(std::memory_order_relaxed);
            }
        }
    }
    if (!found) {
        return;
    }
    g_f.drain_found.fetch_add(1, std::memory_order_relaxed);
    g_f.drain_lag[AheadBucket(lag)].fetch_add(1, std::memory_order_relaxed);
    AnnRow* row = FindAnnRow(AnnKey(caller, kind));
    if (late) {
        g_f.drain_late.fetch_add(1, std::memory_order_relaxed);
        g_f.drain_late_bytes.fetch_add(size, std::memory_order_relaxed);
        g_cum_late.fetch_add(1, std::memory_order_relaxed);
        if (row) {
            row->late.fetch_add(1, std::memory_order_relaxed);
            row->c_late.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (changed) {
        g_f.drain_changed.fetch_add(1, std::memory_order_relaxed);
        if (exact) {
            g_f.drain_exact_changed.fetch_add(1, std::memory_order_relaxed);
        }
        if (row) {
            row->drain_changed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnBlock(uint64_t base, uint64_t size, const char* name, int kind, int slot) {
    if (!On() || base == 0 || size == 0) {
        return;
    }
    BlockInfo b{};
    b.size = size;
    std::snprintf(b.name, sizeof b.name, "%s", name ? name : "?");
    b.kind = static_cast<int16_t>(kind);
    b.slot = static_cast<int16_t>(slot);
    std::lock_guard<std::mutex> lock(g_blk_mutex);
    g_blocks[base] = b;
}

void OnFreeBlock(uint64_t base) {
    if (!On()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_blk_mutex);
    g_blocks.erase(base);
}

void OnGranule(uint64_t base, uint64_t size, const char* pool) {
    if (!On() || base == 0 || size == 0) {
        return;
    }
    BlockInfo b{};
    b.size = size;
    std::snprintf(b.name, sizeof b.name, "%s", pool ? pool : "?");
    std::lock_guard<std::mutex> lock(g_blk_mutex);
    g_granules[base] = b;
}

namespace {
void NoteAnnounceSuspects(uint64_t addr, uint64_t size) {
    if (g_pending_live.load(std::memory_order_relaxed) <= 0 &&
        g_suspect_live.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    const auto on_pending = [&](Pending& p) {
        const uint64_t fa = p.addr.load(std::memory_order_relaxed);
        if (Intersects(fa, kFaultBytes, addr, size)) {
            const uint64_t row_key = p.row_key.load(std::memory_order_relaxed);
            if (p.key.exchange(0, std::memory_order_acq_rel) != 0) {
                g_pending_live.fetch_sub(1, std::memory_order_relaxed);
                g_t.covered.fetch_add(1, std::memory_order_relaxed);
                if (Row* r = FindRow(row_key, false)) {
                    r->covered.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            p.pagecov.store(true, std::memory_order_relaxed);
        }
    };
    const auto on_suspect = [&](Suspect& s, uint64_t page) {
        uint64_t expect = page + 1;
        if (s.key.load(std::memory_order_acquire) != expect) {
            return;
        }
        const uint64_t fa = s.addr.load(std::memory_order_relaxed);
        if (Intersects(fa, kFaultBytes, addr, size)) {
            if (s.key.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) {
                g_suspect_live.fetch_sub(1, std::memory_order_relaxed);
                g_t.late_covered.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            s.pagecov.store(true, std::memory_order_relaxed);
        }
    };
    if (last - first < 256) {
        for (uint64_t page = first; page <= last; ++page) {
            if (Pending* p = FindPending(page, false, nullptr)) {
                on_pending(*p);
            }
            on_suspect(g_suspects[Slot(page, kSuspectBits)], page);
        }
        return;
    }
    // A large announce: one pass over each table is cheaper than probing every page.
    for (Pending& p : g_pending) {
        const uint64_t k = p.key.load(std::memory_order_acquire);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            on_pending(p);
        }
    }
    for (Suspect& s : g_suspects) {
        const uint64_t k = s.key.load(std::memory_order_acquire);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            on_suspect(s, k - 1);
        }
    }
}
} // namespace

void OnSubmitStart() {
    g_sub.fetch_add(1, std::memory_order_relaxed);
    g_t.submits.fetch_add(1, std::memory_order_relaxed);
    const uint64_t frame = SotcVerify::Frame();
    // 1. Resolve the pending writes: still pending at the submit start = nobody announced them.
    for (Pending& p : g_pending) {
        if (p.key.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        const uint64_t k = p.key.exchange(0, std::memory_order_acq_rel);
        if (k == 0) {
            continue;
        }
        g_pending_live.fetch_sub(1, std::memory_order_relaxed);
        const uint64_t page = k - 1;
        const uint64_t addr = p.addr.load(std::memory_order_relaxed);
        const uint64_t row_key = p.row_key.load(std::memory_order_relaxed);
        const bool pagecov = p.pagecov.load(std::memory_order_relaxed);
        g_t.unann.fetch_add(1, std::memory_order_relaxed);
        g_cum_unann.fetch_add(1, std::memory_order_relaxed);
        if (pagecov) {
            g_t.unann_pc.fetch_add(1, std::memory_order_relaxed);
        }
        if (Row* r = FindRow(row_key, false)) {
            r->unann.fetch_add(1, std::memory_order_relaxed);
            r->c_unann.fetch_add(1, std::memory_order_relaxed);
            if (pagecov) {
                r->unann_pc.fetch_add(1, std::memory_order_relaxed);
            }
        }
        Suspect& s = g_suspects[Slot(page, kSuspectBits)];
        const uint64_t old = s.key.load(std::memory_order_acquire);
        if (old == 0) {
            g_suspect_live.fetch_add(1, std::memory_order_relaxed);
        } else if (old != page + 1) {
            g_t.suspect_replaced.fetch_add(1, std::memory_order_relaxed);
        }
        s.addr.store(addr, std::memory_order_relaxed);
        s.row_key.store(row_key, std::memory_order_relaxed);
        s.frame.store(frame, std::memory_order_relaxed);
        s.pagecov.store(pagecov, std::memory_order_relaxed);
        s.bound.store(false, std::memory_order_relaxed);
        s.key.store(page + 1, std::memory_order_release);
    }
    // 2. Protect the pages the handler opened again, from the tracker's state (runs of adjacent pages at once).
    const uint64_t head = g_rearm_head.load(std::memory_order_acquire);
    if (head - g_rearm_tail > kRearmSlots) {
        g_t.rearm_dropped.fetch_add(head - g_rearm_tail - kRearmSlots, std::memory_order_relaxed);
        g_rearm_tail = head - kRearmSlots;
    }
    static thread_local std::vector<uint64_t> pages;
    pages.clear();
    for (; g_rearm_tail < head; ++g_rearm_tail) {
        const uint64_t v = g_rearm[g_rearm_tail % kRearmSlots].exchange(0, std::memory_order_acq_rel);
        if (v != 0) {
            pages.push_back(v - 1);
        }
    }
    if (pages.empty()) {
        return;
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    size_t i = 0;
    while (i < pages.size()) {
        size_t j = i + 1;
        while (j < pages.size() && pages[j] == pages[j - 1] + 1) {
            ++j;
        }
        VideoCore::PageManager::N1VerifyRearm(pages[i] << kPageShift, (j - i) << kPageShift);
        g_t.rearm_runs.fetch_add(1, std::memory_order_relaxed);
        i = j;
    }
    g_t.rearm_pages.fetch_add(pages.size(), std::memory_order_relaxed);
}

namespace {
void NoteBindSuspects(uint64_t addr, uint64_t size, bool image, size_t bkind) {
    if (size == 0 || g_suspect_live.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    const uint64_t frame = SotcVerify::Frame();
    const auto check = [&](Suspect& s, uint64_t page) {
        if (s.key.load(std::memory_order_acquire) != page + 1) {
            return;
        }
        const uint64_t fa = s.addr.load(std::memory_order_relaxed);
        if (!Intersects(fa, kFaultBytes, addr, size)) {
            g_t.bind_page_only.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (s.bound.exchange(true, std::memory_order_acq_rel)) {
            g_t.miss_standing.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool pagecov = s.pagecov.load(std::memory_order_relaxed);
        const uint64_t age = frame - s.frame.load(std::memory_order_relaxed);
        g_t.age[age == 0 ? 0 : age == 1 ? 1 : age < 4 ? 2 : age < 16 ? 3 : 4].fetch_add(1, std::memory_order_relaxed);
        (image ? g_t.miss_img : g_t.miss_buf).fetch_add(1, std::memory_order_relaxed);
        g_cum_miss.fetch_add(1, std::memory_order_relaxed);
        if (pagecov) {
            g_t.miss_pc.fetch_add(1, std::memory_order_relaxed);
        }
        if (size <= kSmallBind) {
            g_t.miss_small.fetch_add(1, std::memory_order_relaxed);
        }
        // The allocator class of the missed page (probe_alloc / probe_memtype feed), by bind kind.
        char cname[56];
        ClassName(fa, cname, sizeof cname);
        const uint32_t cidx = InternClass(cname);
        {
            std::lock_guard<std::mutex> lock(g_cls_mutex);
            ClassSlot& c = g_classes[cidx - 1];
            (image ? c.miss_img : c.miss_buf) += 1;
            c.kinds[bkind] += 1;
        }
        if (Row* r = FindRow(s.row_key.load(std::memory_order_relaxed), false)) {
            r->miss_addr.store(fa, std::memory_order_relaxed);
            r->miss_size.store(size, std::memory_order_relaxed);
            r->miss_class.store(cidx, std::memory_order_relaxed);
            r->miss.fetch_add(1, std::memory_order_relaxed);
            r->c_miss.fetch_add(1, std::memory_order_relaxed);
            if (pagecov) {
                r->miss_pc.fetch_add(1, std::memory_order_relaxed);
            }
            if (size <= kSmallBind) {
                r->miss_small.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    if (last - first < 256) {
        for (uint64_t page = first; page <= last; ++page) {
            check(g_suspects[Slot(page, kSuspectBits)], page);
        }
        return;
    }
    for (Suspect& s : g_suspects) { // a large bind: one pass over the table
        const uint64_t k = s.key.load(std::memory_order_acquire);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            check(s, k - 1);
        }
    }
}
} // namespace

void NoteBind(uint64_t addr, uint64_t size, bool image, bool via_memo) {
    if (size == 0) {
        return;
    }
    size_t kind = image ? static_cast<size_t>(SotcVerify::BindKind::Image)
                        : static_cast<size_t>(SotcVerify::CurrentBindKind());
    if (kind >= kKinds) {
        kind = kKinds - 1;
    }
    NoteBindSuspects(addr, size, image, kind);
    g_f.binds[kind].fetch_add(1, std::memory_order_relaxed);
    if (via_memo) {
        g_f.memo_binds[kind].fetch_add(1, std::memory_order_relaxed);
    }
    // Frame source: R is the frame the draw thread records now, the CPU has finished frame `cpu`.
    const uint64_t rec = g_rec.load(std::memory_order_relaxed);
    const uint64_t R = rec + 1;
    const uint64_t cpu = SotcNative::FrameSubmitted();
    g_f.ahead[kind][AheadBucket(cpu > rec ? cpu - rec : 0)].fetch_add(1, std::memory_order_relaxed);
    const uint32_t sub = static_cast<uint32_t>(g_sub.load(std::memory_order_relaxed));
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    const uint64_t npages = last - first + 1;
    const uint64_t cap = image ? kMaxImagePages : kMaxBindPages;
    const uint64_t stride = npages > cap ? (npages + cap - 1) / cap : 1;
    if (stride > 1) {
        g_f.sampled[kind].fetch_add(1, std::memory_order_relaxed);
    }
    bool found = false, is_early = false, undrained = false, undrained_same = false, undrained_old = false;
    bool changed = false, changed_early = false;
    uint64_t attr_caller = 0;
    uint8_t attr_kind = 0;
    bool attr_set = false;
    bool have_w = false;
    uint64_t best_w = 0, best_row = 0;
    uint64_t hashed_pages = 0;
    for (uint64_t page = first; page <= last; page += stride) {
        AnnRec& r = g_ann[Slot(page, kAnnBits)];
        if (r.key.load(std::memory_order_acquire) == page + 1) {
            uint32_t plo = 0, phi = 0;
            PageSpan(page, addr, size, &plo, &phi);
            const uint32_t rlo = r.lo.load(std::memory_order_relaxed);
            const uint32_t rhi = r.hi.load(std::memory_order_relaxed);
            if (phi > rlo && plo < rhi) {
                found = true;
                const uint64_t A = uint64_t{r.cf.load(std::memory_order_relaxed)} + 1;
                bool page_flag = false;
                if (A > R) {
                    is_early = true;
                    page_flag = true;
                }
                if (r.drained.load(std::memory_order_relaxed) == 0) {
                    undrained = true;
                    page_flag = true;
                    if (A == R) {
                        undrained_same = true;
                    } else if (A < R) {
                        undrained_old = true;
                    }
                }
                if (!image && !via_memo && r.hashed.load(std::memory_order_relaxed) && hashed_pages < kMaxHashPages) {
                    ++hashed_pages;
                    if (const uint8_t* p = PagePtr(page)) {
                        if (PageHash(p, rlo, rhi) != r.hash.load(std::memory_order_relaxed)) {
                            changed = true;
                            changed_early |= A > R;
                            page_flag = true;
                        }
                    }
                }
                uint32_t expect = 0;
                r.bind_sub.compare_exchange_strong(expect, sub, std::memory_order_relaxed);
                if (page_flag && !attr_set) {
                    attr_set = true;
                    attr_caller = r.caller.load(std::memory_order_relaxed);
                    attr_kind = r.kind.load(std::memory_order_relaxed);
                }
            }
        }
        LastWrite& lw = g_lastw[Slot(page, kLastBits)];
        if (lw.key.load(std::memory_order_acquire) == page + 1) {
            const uint64_t W = uint64_t{lw.cf.load(std::memory_order_relaxed)} + 1;
            if (!have_w || W > best_w) {
                have_w = true;
                best_w = W;
                best_row = lw.row_key.load(std::memory_order_relaxed);
            }
        }
    }
    if (found) {
        g_f.found[kind].fetch_add(1, std::memory_order_relaxed);
    }
    if (is_early) {
        g_f.early[kind].fetch_add(1, std::memory_order_relaxed);
    }
    if (undrained) {
        g_f.undrained[kind].fetch_add(1, std::memory_order_relaxed);
        if (undrained_same) {
            g_f.undrained_same[kind].fetch_add(1, std::memory_order_relaxed);
        }
        if (undrained_old) {
            g_f.undrained_old[kind].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (changed) {
        g_f.changed[kind].fetch_add(1, std::memory_order_relaxed);
        if (changed_early) {
            g_f.changed_early[kind].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (attr_set) {
        if (AnnRow* row = FindAnnRow(AnnKey(attr_caller, attr_kind))) {
            if (is_early) {
                row->bind_early[kind].fetch_add(1, std::memory_order_relaxed);
                row->c_early.fetch_add(1, std::memory_order_relaxed);
                g_cum_early.fetch_add(1, std::memory_order_relaxed);
            }
            if (undrained) {
                row->bind_undrained[kind].fetch_add(1, std::memory_order_relaxed);
            }
            if (changed) {
                row->bind_changed[kind].fetch_add(1, std::memory_order_relaxed);
                row->c_changed.fetch_add(1, std::memory_order_relaxed);
                g_cum_changed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!have_w) {
        g_f.lastw[kind][7].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t rel = static_cast<int64_t>(best_w) - static_cast<int64_t>(R);
    const size_t bucket = static_cast<size_t>(std::max<int64_t>(-3, std::min<int64_t>(3, rel)) + 3);
    g_f.lastw[kind][bucket].fetch_add(1, std::memory_order_relaxed);
    if (Row* row = FindRow(best_row, false)) {
        row->bw[rel > 0 ? 0 : rel == 0 ? 1 : 2][kind].fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {
// The FRAMES, BIND and DRAIN lines of the frame-source checks (window counters, reset here).
void ReportFrameSource(std::string& out, uint32_t window) {
    char buf[1024];
    const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::snprintf(buf, sizeof buf,
                  "SOTC N1VERIFY w=%u FRAMES rec=%llu cpu=%llu rec_calls=%llu rec_jumps=%llu | announces=%llu "
                  "oversize=%llu evicted_undrained=%llu nohash=%llu | cumulative early_binds=%llu late_drains=%llu "
                  "changed_binds=%llu\n",
                  window, ull(g_rec.load(std::memory_order_relaxed)), ull(SotcNative::FrameSubmitted()),
                  ull(take(g_f.rec_calls)), ull(take(g_f.rec_jumps)), ull(take(g_f.ann_total)),
                  ull(take(g_f.ann_oversize)), ull(take(g_f.ann_evicted)), ull(take(g_f.ann_nohash)),
                  ull(g_cum_early.load(std::memory_order_relaxed)), ull(g_cum_late.load(std::memory_order_relaxed)),
                  ull(g_cum_changed.load(std::memory_order_relaxed)));
    out += buf;
    for (size_t k = 0; k < kKinds; ++k) {
        const uint64_t binds = take(g_f.binds[k]);
        const uint64_t memo = take(g_f.memo_binds[k]);
        uint64_t ahead[4], lw[8];
        for (size_t i = 0; i < 4; ++i) {
            ahead[i] = take(g_f.ahead[k][i]);
        }
        uint64_t lw_sum = 0;
        for (size_t i = 0; i < 8; ++i) {
            lw[i] = take(g_f.lastw[k][i]);
            lw_sum += lw[i];
        }
        const uint64_t found = take(g_f.found[k]);
        const uint64_t early = take(g_f.early[k]);
        const uint64_t undrained = take(g_f.undrained[k]);
        const uint64_t undrained_same = take(g_f.undrained_same[k]);
        const uint64_t undrained_old = take(g_f.undrained_old[k]);
        const uint64_t changed = take(g_f.changed[k]);
        const uint64_t changed_early = take(g_f.changed_early[k]);
        const uint64_t sampled = take(g_f.sampled[k]);
        if (binds == 0 && lw_sum == 0) {
            continue;
        }
        std::snprintf(buf, sizeof buf,
                      "SOTC N1VERIFY w=%u BIND %s binds=%llu memo=%llu sampled=%llu | ahead 0=%llu 1=%llu 2=%llu 3+=%llu "
                      "| announce found=%llu early=%llu undrained=%llu (same_frame=%llu old=%llu) changed=%llu "
                      "(changed_early=%llu) | lastwrite rel -3-=%llu -2=%llu -1=%llu 0=%llu +1=%llu +2=%llu +3+=%llu "
                      "none=%llu\n",
                      window, kKindName[k], ull(binds), ull(memo), ull(sampled), ull(ahead[0]), ull(ahead[1]),
                      ull(ahead[2]), ull(ahead[3]), ull(found), ull(early), ull(undrained), ull(undrained_same),
                      ull(undrained_old), ull(changed), ull(changed_early), ull(lw[0]), ull(lw[1]), ull(lw[2]),
                      ull(lw[3]), ull(lw[4]), ull(lw[5]), ull(lw[6]), ull(lw[7]));
        out += buf;
    }
    std::snprintf(buf, sizeof buf,
                  "SOTC N1VERIFY w=%u DRAIN pieces=%llu exact=%llu with_record=%llu late=%llu (bytes=%llu) changed=%llu "
                  "exact_changed=%llu | lag_cpu_frames 0=%llu 1=%llu 2=%llu 3+=%llu\n",
                  window, ull(take(g_f.drain_pieces)), ull(take(g_f.drain_exact)), ull(take(g_f.drain_found)),
                  ull(take(g_f.drain_late)), ull(take(g_f.drain_late_bytes)), ull(take(g_f.drain_changed)),
                  ull(take(g_f.drain_exact_changed)), ull(take(g_f.drain_lag[0])), ull(take(g_f.drain_lag[1])),
                  ull(take(g_f.drain_lag[2])), ull(take(g_f.drain_lag[3])));
    out += buf;
}

// "host:<module>+0x<offset>" for an announcer; a small value is a tag of the rasterizer's own writes.
void FormatAnnouncer(char* buf, size_t n, uint64_t caller) {
    if (caller < 4096) { // a tag of the rasterizer's own host writes (NoteAnnounce callers in vk_rasterizer.cpp)
        std::snprintf(buf, n, "tag:%s", caller == 1 ? "fill-buffer" : caller == 2 ? "copy-buffer" : "unknown");
        return;
    }
    FormatRip(buf, n, caller);
}

void ReportAnnouncers(std::string& out) {
    struct Snap {
        uint64_t key, announces, pages, late, drain_changed, c_early, c_late, c_changed;
        uint64_t early[kKinds], undrained[kKinds], changed[kKinds];
        uint64_t score;
    };
    const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    std::vector<Snap> rows;
    for (AnnRow& r : g_annrows) {
        const uint64_t key = r.key.load(std::memory_order_acquire);
        if (key == 0) {
            continue;
        }
        Snap s{};
        s.key = key;
        s.announces = take(r.announces);
        s.pages = take(r.pages);
        s.late = take(r.late);
        s.drain_changed = take(r.drain_changed);
        s.c_early = r.c_early.load(std::memory_order_relaxed);
        s.c_late = r.c_late.load(std::memory_order_relaxed);
        s.c_changed = r.c_changed.load(std::memory_order_relaxed);
        for (size_t k = 0; k < kKinds; ++k) {
            s.early[k] = take(r.bind_early[k]);
            s.undrained[k] = take(r.bind_undrained[k]);
            s.changed[k] = take(r.bind_changed[k]);
            s.score += s.early[k] * 4 + s.changed[k] * 2 + s.undrained[k];
        }
        s.score += s.late * 4 + s.drain_changed * 2;
        if (s.score || s.announces) {
            rows.push_back(s);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Snap& a, const Snap& b) {
        return a.score != b.score ? a.score > b.score : a.announces > b.announces;
    });
    const auto split = [](char* buf, size_t n, const uint64_t* v) {
        int len = 0;
        bool any = false;
        buf[0] = '\0';
        for (size_t k = 0; k < kKinds; ++k) {
            if (v[k]) {
                len += std::snprintf(buf + len, n - len, "%s%c=%llu", any ? " " : "", kKindLetter[k],
                                     static_cast<unsigned long long>(v[k]));
                any = true;
            }
        }
        if (!any) {
            std::snprintf(buf, n, "-");
        }
    };
    char buf[768];
    for (size_t i = 0; i < rows.size() && i < kTopAnn; ++i) {
        const Snap& s = rows[i];
        static const char* const kind_name[4] = {"persistent", "per_frame", "gpu_written", "direct"};
        char who[96], e[64], u[64], c[64];
        FormatAnnouncer(who, sizeof who, (s.key & ~(uint64_t{1} << 63)) >> 2);
        split(e, sizeof e, s.early);
        split(u, sizeof u, s.undrained);
        split(c, sizeof c, s.changed);
        std::snprintf(buf, sizeof buf,
                      "SOTC N1VERIFY A %zu announcer=%s kind=%s announces=%llu pages=%llu | bind early[%s] undrained[%s] "
                      "changed[%s] | drain late=%llu changed=%llu | cum early=%llu late=%llu changed=%llu\n",
                      i + 1, who, kind_name[(s.key & 3u)], static_cast<unsigned long long>(s.announces),
                      static_cast<unsigned long long>(s.pages), e, u, c, static_cast<unsigned long long>(s.late),
                      static_cast<unsigned long long>(s.drain_changed), static_cast<unsigned long long>(s.c_early),
                      static_cast<unsigned long long>(s.c_late), static_cast<unsigned long long>(s.c_changed));
        out += buf;
    }
}

// bound_miss by allocator class (window counts, reset here).
void ReportMissClasses(std::string& out, uint32_t window) {
    std::lock_guard<std::mutex> lock(g_cls_mutex);
    char buf[512];
    for (size_t i = 0; i < g_class_count; ++i) {
        ClassSlot& c = g_classes[i];
        if (c.miss_buf == 0 && c.miss_img == 0) {
            continue;
        }
        int n = std::snprintf(buf, sizeof buf, "SOTC N1VERIFY w=%u MISS class=%s buf=%llu img=%llu by_bind_kind[", window,
                              c.name, static_cast<unsigned long long>(c.miss_buf),
                              static_cast<unsigned long long>(c.miss_img));
        bool any = false;
        for (size_t k = 0; k < kKinds; ++k) {
            if (c.kinds[k]) {
                n += std::snprintf(buf + n, sizeof buf - n, "%s%c=%llu", any ? " " : "", kKindLetter[k],
                                   static_cast<unsigned long long>(c.kinds[k]));
                any = true;
            }
        }
        std::snprintf(buf + n, sizeof buf - n, "]\n");
        out += buf;
        c.miss_buf = c.miss_img = 0;
        std::memset(c.kinds, 0, sizeof c.kinds);
    }
}
} // namespace

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    const uint32_t window = g_window.load(std::memory_order_relaxed);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const bool early = window < kEarlyWindows;
    char buf[1536];
    const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const uint64_t wf[3] = {take(g_t.write_faults[0]), take(g_t.write_faults[1]), take(g_t.write_faults[2])};
    const uint64_t rf[3] = {take(g_t.read_faults[0]), take(g_t.read_faults[1]), take(g_t.read_faults[2])};
    std::snprintf(buf, sizeof buf,
                  "SOTC N1VERIFY w=%u t=%.0fs %s window=%.1fs submits=%llu | write_faults guest=%llu host_gpu=%llu "
                  "host_other=%llu | covered=%llu unannounced=%llu (page_announced=%llu) | bound_miss buf=%llu "
                  "img=%llu (small<=64K=%llu page_announced=%llu) standing=%llu bind_page_only=%llu "
                  "late_announced=%llu | miss_age_frames 0=%llu 1=%llu 2-3=%llu 4-15=%llu 16+=%llu\n",
                  window, t, early ? "early" : "late", seconds,
                  static_cast<unsigned long long>(take(g_t.submits)), static_cast<unsigned long long>(wf[0]),
                  static_cast<unsigned long long>(wf[1]), static_cast<unsigned long long>(wf[2]),
                  static_cast<unsigned long long>(take(g_t.covered)),
                  static_cast<unsigned long long>(take(g_t.unann)),
                  static_cast<unsigned long long>(take(g_t.unann_pc)),
                  static_cast<unsigned long long>(take(g_t.miss_buf)),
                  static_cast<unsigned long long>(take(g_t.miss_img)),
                  static_cast<unsigned long long>(take(g_t.miss_small)),
                  static_cast<unsigned long long>(take(g_t.miss_pc)),
                  static_cast<unsigned long long>(take(g_t.miss_standing)),
                  static_cast<unsigned long long>(take(g_t.bind_page_only)),
                  static_cast<unsigned long long>(take(g_t.late_covered)),
                  static_cast<unsigned long long>(take(g_t.age[0])), static_cast<unsigned long long>(take(g_t.age[1])),
                  static_cast<unsigned long long>(take(g_t.age[2])), static_cast<unsigned long long>(take(g_t.age[3])),
                  static_cast<unsigned long long>(take(g_t.age[4])));
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC N1VERIFY w=%u read_faults guest=%llu host_gpu=%llu host_other=%llu | rearmed_pages=%llu "
                  "runs=%llu rearm_dropped=%llu | pending_dropped=%llu suspect_replaced=%llu rows_dropped=%llu "
                  "loop_breaks=%llu raced=%llu | live pending=%lld suspects=%lld | cumulative write_faults=%llu "
                  "unannounced=%llu bound_miss=%llu read_faults=%llu\n",
                  window, static_cast<unsigned long long>(rf[0]), static_cast<unsigned long long>(rf[1]),
                  static_cast<unsigned long long>(rf[2]), static_cast<unsigned long long>(take(g_t.rearm_pages)),
                  static_cast<unsigned long long>(take(g_t.rearm_runs)),
                  static_cast<unsigned long long>(take(g_t.rearm_dropped)),
                  static_cast<unsigned long long>(take(g_t.pending_dropped)),
                  static_cast<unsigned long long>(take(g_t.suspect_replaced)),
                  static_cast<unsigned long long>(g_rows_dropped.exchange(0, std::memory_order_relaxed)),
                  static_cast<unsigned long long>(take(g_t.loop_breaks)),
                  static_cast<unsigned long long>(take(g_t.raced)),
                  static_cast<long long>(g_pending_live.load(std::memory_order_relaxed)),
                  static_cast<long long>(g_suspect_live.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_cum_write_faults.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_cum_unann.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_cum_miss.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_cum_read_faults.load(std::memory_order_relaxed)));
    out += buf;

    ReportFrameSource(out, window);

    // Snapshot the rows and reset their window counters.
    struct Snap {
        uint64_t key, last_addr;
        uint32_t first_win;
        uint64_t faults, covered, unann, unann_pc, miss, miss_pc, miss_small, reads;
        uint64_t c_faults, c_unann, c_miss, c_reads;
        uint64_t miss_addr, miss_size;
        uint32_t miss_class;
        uint64_t bw[3][kKinds];
        uint64_t bw_total;
    };
    std::vector<Snap> rows;
    for (Row& r : g_rows) {
        const uint64_t key = r.key.load(std::memory_order_acquire);
        if (key == 0) {
            continue;
        }
        Snap s{};
        s.key = key;
        s.last_addr = r.last_addr.load(std::memory_order_relaxed);
        s.first_win = r.first_win.load(std::memory_order_relaxed);
        s.faults = take(r.faults);
        s.covered = take(r.covered);
        s.unann = take(r.unann);
        s.unann_pc = take(r.unann_pc);
        s.miss = take(r.miss);
        s.miss_pc = take(r.miss_pc);
        s.miss_small = take(r.miss_small);
        s.reads = take(r.reads);
        s.c_faults = r.c_faults.load(std::memory_order_relaxed);
        s.c_unann = r.c_unann.load(std::memory_order_relaxed);
        s.c_miss = r.c_miss.load(std::memory_order_relaxed);
        s.c_reads = r.c_reads.load(std::memory_order_relaxed);
        s.miss_addr = r.miss_addr.load(std::memory_order_relaxed);
        s.miss_size = r.miss_size.load(std::memory_order_relaxed);
        s.miss_class = r.miss_class.load(std::memory_order_relaxed);
        for (size_t w = 0; w < 3; ++w) {
            for (size_t k = 0; k < kKinds; ++k) {
                s.bw[w][k] = take(r.bw[w][k]);
                s.bw_total += s.bw[w][k];
            }
        }
        if (s.faults || s.unann || s.miss || s.reads || s.bw_total) {
            rows.push_back(s);
        }
    }
    const auto describe = [&](const Snap& s, char* where, size_t n, const char** who_name, bool* is_new) {
        const uint64_t rip = (s.key & ~(uint64_t{1} << 63)) >> 2;
        const Who who = static_cast<Who>((s.key >> 0) & 3);
        FormatRip(where, n, rip);
        *who_name = WhoName(who);
        // NEW: first seen after the early windows
        *is_new = s.first_win == window && window >= kEarlyWindows;
    };
    // Write rows: misses first, then unannounced, then fault count.
    std::vector<Snap> wr, wb;
    for (const Snap& s : rows) {
        if (s.faults || s.unann || s.miss) {
            wr.push_back(s);
        }
        if (s.bw_total) {
            wb.push_back(s);
        }
    }
    std::sort(wr.begin(), wr.end(), [](const Snap& a, const Snap& b) {
        if (a.miss != b.miss) {
            return a.miss > b.miss;
        }
        if (a.unann != b.unann) {
            return a.unann > b.unann;
        }
        return a.faults > b.faults;
    });
    size_t listed_new = 0;
    for (size_t i = 0; i < wr.size(); ++i) {
        const Snap& s = wr[i];
        char where[96];
        const char* who = "";
        bool is_new = false;
        describe(s, where, sizeof where, &who, &is_new);
        if (i >= kTopWrite && !(is_new && listed_new < kTopNew)) {
            continue;
        }
        char miss_last[160] = "";
        if (s.miss_addr != 0) {
            char cname[56] = "?";
            if (s.miss_class != 0) {
                std::lock_guard<std::mutex> lock(g_cls_mutex);
                std::snprintf(cname, sizeof cname, "%s", g_classes[s.miss_class - 1].name);
            }
            std::snprintf(miss_last, sizeof miss_last, " miss_last=0x%llx size=%llu class=%s",
                          static_cast<unsigned long long>(s.miss_addr), static_cast<unsigned long long>(s.miss_size),
                          cname);
        }
        listed_new += is_new && i >= kTopWrite ? 1 : 0;
        std::snprintf(buf, sizeof buf,
                      "SOTC N1VERIFY W %zu rip=%s who=%s faults=%llu covered=%llu unannounced=%llu (page_announced=%llu) "
                      "bound_miss=%llu (small=%llu page_announced=%llu) first_w=%u%s last_addr=0x%llx | cum faults=%llu "
                      "unannounced=%llu bound_miss=%llu%s\n",
                      i + 1, where, who, static_cast<unsigned long long>(s.faults),
                      static_cast<unsigned long long>(s.covered), static_cast<unsigned long long>(s.unann),
                      static_cast<unsigned long long>(s.unann_pc), static_cast<unsigned long long>(s.miss),
                      static_cast<unsigned long long>(s.miss_small), static_cast<unsigned long long>(s.miss_pc),
                      s.first_win, is_new ? " NEW" : "", static_cast<unsigned long long>(s.last_addr),
                      static_cast<unsigned long long>(s.c_faults), static_cast<unsigned long long>(s.c_unann),
                      static_cast<unsigned long long>(s.c_miss), miss_last);
        out += buf;
    }
    // WB rows: per writer, the binds whose newest write fault on a covered page was this writer's.
    std::sort(wb.begin(), wb.end(), [](const Snap& a, const Snap& b) {
        const auto later = [](const Snap& x) {
            uint64_t v = 0;
            for (size_t k = 0; k < kKinds; ++k) {
                v += x.bw[0][k];
            }
            return v;
        };
        return later(a) != later(b) ? later(a) > later(b) : a.bw_total > b.bw_total;
    });
    for (size_t i = 0; i < wb.size() && i < kTopWb; ++i) {
        const Snap& s = wb[i];
        char where[96];
        const char* who = "";
        bool is_new = false;
        describe(s, where, sizeof where, &who, &is_new);
        int n = std::snprintf(buf, sizeof buf, "SOTC N1VERIFY WB %zu rip=%s who=%s binds_after_write", i + 1, where, who);
        static const char* const rel_name[3] = {"later", "same", "older"};
        for (size_t w = 0; w < 3; ++w) {
            n += std::snprintf(buf + n, sizeof buf - n, " %s[", rel_name[w]);
            bool any = false;
            for (size_t k = 0; k < kKinds; ++k) {
                if (s.bw[w][k]) {
                    n += std::snprintf(buf + n, sizeof buf - n, "%s%c=%llu", any ? " " : "", kKindLetter[k],
                                       static_cast<unsigned long long>(s.bw[w][k]));
                    any = true;
                }
            }
            n += std::snprintf(buf + n, sizeof buf - n, "%s]", any ? "" : "-");
        }
        std::snprintf(buf + n, sizeof buf - n, "\n");
        out += buf;
    }
    ReportAnnouncers(out);
    ReportMissClasses(out, window);
    // Read rows.
    std::vector<Snap> rr;
    for (const Snap& s : rows) {
        if (s.reads) {
            rr.push_back(s);
        }
    }
    std::sort(rr.begin(), rr.end(), [](const Snap& a, const Snap& b) { return a.reads > b.reads; });
    listed_new = 0;
    for (size_t i = 0; i < rr.size(); ++i) {
        const Snap& s = rr[i];
        char where[96];
        const char* who = "";
        bool is_new = false;
        describe(s, where, sizeof where, &who, &is_new);
        if (i >= kTopRead && !(is_new && listed_new < kTopNew)) {
            continue;
        }
        listed_new += is_new && i >= kTopRead ? 1 : 0;
        std::snprintf(buf, sizeof buf,
                      "SOTC N1VERIFY R %zu rip=%s who=%s reads=%llu first_w=%u%s last_addr=0x%llx | cum reads=%llu\n",
                      i + 1, where, who, static_cast<unsigned long long>(s.reads), s.first_win, is_new ? " NEW" : "",
                      static_cast<unsigned long long>(s.last_addr), static_cast<unsigned long long>(s.c_reads));
        out += buf;
    }
    g_window.fetch_add(1, std::memory_order_relaxed);
}

} // namespace SotcN1Verify
