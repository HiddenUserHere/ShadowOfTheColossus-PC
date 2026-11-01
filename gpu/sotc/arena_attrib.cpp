// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ARENA_ATTRIB, the attribution of guest write faults on the per-frame arena. See
// arena_attrib.h for the classes and the output. Off unless SOTC_ARENA_ATTRIB is set; every entry
// point returns at once when it is off.
//
// Fault path (NoteWriteFault): no allocation, no lock. The block list is read lock-free (blocks
// are filled once and never reused for another base), the slot table is open addressing with
// compare-and-swap on the RIP, and the caller walk uses guarded reads (VirtualQuery) of the
// frame-pointer chain. Report and the exit line run on the report thread (or at exit), where
// allocation and locks are fine.
#include "arena_attrib.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// probe.c: the guest image base (the eboot's load address; 0 before the image is mapped). Weak, so
// test binaries without probe.c still link.
extern "C" uintptr_t sotc_arena_image_base(void) __attribute__((weak));

namespace SotcArenaAttrib {
namespace {

constexpr uint64_t kPageShift = 12; // 4 KiB, the page manager's tracking granule
constexpr uint64_t kPage = uint64_t{1} << kPageShift;
constexpr uint64_t kPageMask = kPage - 1;
constexpr size_t kMaxBlocks = 32;
constexpr size_t kSlotBits = 12;
constexpr size_t kSlots = size_t{1} << kSlotBits;
constexpr int kRewrite = 0; // allocated in an earlier epoch: a submit came after the allocation
constexpr int kFirst = 1;   // allocated in the current epoch: the fill of a new allocation
constexpr size_t kTopRewrite = 40;
constexpr size_t kTopFirst = 20;

struct Block {
    std::atomic<uint64_t> base{0};
    std::atomic<uint64_t> size{0};
    std::atomic<bool> active{false};
    std::unique_ptr<std::atomic<uint32_t>[]> alloc_ep; // per page: epoch of its last allocation, 0 = never
};
Block g_blocks[kMaxBlocks];
std::atomic<size_t> g_block_count{0}; // slots filled (published after the slot's array)
std::mutex g_block_mutex;             // writers only

// The submit epoch. Starts at 1 so that 0 means "never allocated".
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<uint64_t> g_rewrite{0};   // all-time totals
std::atomic<uint64_t> g_first{0};
std::atomic<uint64_t> g_untracked{0}; // arena write faults on pages with no allocation seen
std::atomic<uint64_t> g_dropped{0};   // faults not recorded (slot table full)

// The per-RIP table. rip == 0 marks a free slot. Counts are written by any faulting thread;
// win is the report thread's snapshot at the previous window.
struct Slot {
    std::atomic<uint64_t> rip{0};
    std::atomic<uint64_t> count[2]{};
    std::atomic<uint64_t> win[2]{};
    std::atomic<uint64_t> caller[2]{}; // return address of the function, and of its caller
    std::atomic<uint32_t> tid{0};      // faulting thread of the last fault
    std::atomic<uint64_t> first_frame{0}; // frame + 1 of the first fault (0 = none)
    std::atomic<uint64_t> last_frame{0};  // frame + 1 of the last fault
};
Slot g_slots[kSlots];

thread_local uint32_t t_fault_tid = 0;

std::mutex g_report_mutex; // the report thread and the exit report

// The guest image base, or 0 when probe.c is not linked.
uint64_t ImageBase() {
    return sotc_arena_image_base ? static_cast<uint64_t>(sotc_arena_image_base()) : 0;
}

// Block holding addr, and the page index inside it. Lock-free.
bool FindBlock(uint64_t addr, Block** out, uint64_t* index) {
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
            *index = (addr - base) >> kPageShift;
            return true;
        }
    }
    return false;
}

// Slot of rip, claimed when it is new. Lock-free (compare-and-swap); null when the table is full.
Slot* FindSlot(uint64_t rip) {
    const size_t h = static_cast<size_t>((rip * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[(h + i) & (kSlots - 1)];
        const uint64_t k = s.rip.load(std::memory_order_acquire);
        if (k == rip) {
            return &s;
        }
        if (k == 0) {
            uint64_t expect = 0;
            if (s.rip.compare_exchange_strong(expect, rip, std::memory_order_acq_rel)) {
                return &s;
            }
            if (expect == rip) {
                return &s;
            }
        }
    }
    return nullptr;
}

#ifdef _WIN32
// One guest word at addr, read only when the page is committed and readable (VirtualQuery does
// not allocate or take the heap). The guest stacks are committed, so the frame chain is readable.
bool ReadWord(uint64_t addr, uint64_t* out) {
    if (addr < 0x10000 || (addr & 7) != 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof mbi) !=
        sizeof mbi) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const uint64_t end = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
                         static_cast<uint64_t>(mbi.RegionSize);
    if (addr + 8 > end) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), 8);
    return true;
}

// rbp of the faulting context (EXCEPTION_POINTERS on Windows).
uint64_t ContextRbp(void* ctx) {
    const auto* ep = static_cast<const EXCEPTION_POINTERS*>(ctx);
    return (ep && ep->ContextRecord) ? static_cast<uint64_t>(ep->ContextRecord->Rbp) : 0;
}
#endif

// The return address of the faulting function (frame pointer [rbp+8]) and of its caller (the
// frame above it). The guest code keeps frame pointers. Zero when not readable.
void WalkCallers(uint64_t rbp, uint64_t* c1, uint64_t* c2) {
    *c1 = 0;
    *c2 = 0;
#ifdef _WIN32
    if (rbp == 0 || (rbp & 7) != 0) {
        return;
    }
    uint64_t ret = 0;
    uint64_t next = 0;
    if (!ReadWord(rbp + 8, &ret)) {
        return;
    }
    *c1 = ret;
    if (!ReadWord(rbp, &next) || next <= rbp || next - rbp > (uint64_t{1} << 20)) {
        return;
    }
    uint64_t ret2 = 0;
    if (ReadWord(next + 8, &ret2)) {
        *c2 = ret2;
    }
#else
    (void)rbp;
#endif
}

// "0x<eboot offset>" inside the image, "host:0x<address>" elsewhere, "-" for zero.
void FormatAddr(char* buf, size_t size, uint64_t addr) {
    const uint64_t img = ImageBase();
    if (addr == 0) {
        std::snprintf(buf, size, "-");
    } else if (img != 0 && addr >= img && addr - img < 0x20000000) {
        std::snprintf(buf, size, "0x%llx", static_cast<unsigned long long>(addr - img));
    } else {
        std::snprintf(buf, size, "host:0x%llx", static_cast<unsigned long long>(addr));
    }
}

// Thread name for the report ("-" when none). Report thread only: it opens the thread.
std::string ThreadLabel(uint32_t tid) {
    char out[128];
    if (tid == 0) {
        std::snprintf(out, sizeof out, "-/0");
        return out;
    }
    std::string name = "-";
#ifdef _WIN32
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static GetDescription get_description = reinterpret_cast<GetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription")));
    if (get_description) {
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(get_description(h, &wide)) && wide) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    std::string utf8(static_cast<size_t>(n - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), n, nullptr, nullptr);
                    name = utf8;
                }
                LocalFree(wide);
            }
            CloseHandle(h);
        }
    }
#endif
    for (char& c : name) {
        if (c == ' ' || c == '\t') {
            c = '_'; // one token per field in the log
        }
    }
    std::snprintf(out, sizeof out, "%s/%u", name.c_str(), static_cast<unsigned>(tid));
    return out;
}

struct Totals {
    uint64_t submits = 0;
    uint64_t frames = 0;
    uint64_t rewrite = 0;
    uint64_t first = 0;
    uint64_t untracked = 0;
    uint64_t dropped = 0;
};

Totals Snapshot() {
    Totals t;
    t.submits = g_submits.load(std::memory_order_relaxed);
    t.frames = g_frame.load(std::memory_order_relaxed);
    t.rewrite = g_rewrite.load(std::memory_order_relaxed);
    t.first = g_first.load(std::memory_order_relaxed);
    t.untracked = g_untracked.load(std::memory_order_relaxed);
    t.dropped = g_dropped.load(std::memory_order_relaxed);
    return t;
}

Totals g_last_window; // under g_report_mutex
bool g_exit_done = false;

// One ranked list. kind 0 = rewrite, 1 = first. A window list ranks by the count since the last
// window (and updates the snapshot); the final list ranks by the cumulative count.
struct Row {
    size_t slot;
    uint64_t n;
};

void EmitList(std::string& out, int kind, bool final_list, std::vector<Row>& rows, size_t top) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.n > b.n; });
    const char* kind_name = kind == kRewrite ? "REWRITE" : "FIRST";
    char line[512];
    for (size_t r = 0; r < rows.size() && r < top; ++r) {
        const Slot& s = g_slots[rows[r].slot];
        char off[32], caller[32], caller2[32];
        FormatAddr(off, sizeof off, s.rip.load(std::memory_order_relaxed));
        FormatAddr(caller, sizeof caller, s.caller[0].load(std::memory_order_relaxed));
        FormatAddr(caller2, sizeof caller2, s.caller[1].load(std::memory_order_relaxed));
        const uint64_t first = s.first_frame.load(std::memory_order_relaxed);
        const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
        const std::string thread = ThreadLabel(s.tid.load(std::memory_order_relaxed));
        std::snprintf(line, sizeof line,
                      "SOTC ARENA ATTRIB %s%s %2zu off=%s n=%llu thread=%s caller=%s caller2=%s "
                      "frames=%llu-%llu\n",
                      final_list ? "FINAL " : "", kind_name, r + 1, off,
                      static_cast<unsigned long long>(rows[r].n), thread.c_str(), caller,
                      caller2, static_cast<unsigned long long>(first ? first - 1 : 0),
                      static_cast<unsigned long long>(last ? last - 1 : 0));
        out += line;
    }
}

// Builds the report. final_report: cumulative counts, all rows, FINAL lines. Otherwise the window
// since the last call. Caller holds g_report_mutex.
void Build(std::string& out, bool final_report, double seconds) {
    const Totals now = Snapshot();
    Totals win;
    win.submits = now.submits - g_last_window.submits;
    win.frames = now.frames - g_last_window.frames;
    win.rewrite = now.rewrite - g_last_window.rewrite;
    win.first = now.first - g_last_window.first;
    win.untracked = now.untracked - g_last_window.untracked;
    win.dropped = now.dropped - g_last_window.dropped;
    g_last_window = now;

    std::vector<Row> rewrite_rows;
    std::vector<Row> first_rows;
    size_t used = 0;
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.rip.load(std::memory_order_acquire) == 0) {
            continue;
        }
        ++used;
        const uint64_t c0 = s.count[kRewrite].load(std::memory_order_relaxed);
        const uint64_t c1 = s.count[kFirst].load(std::memory_order_relaxed);
        uint64_t d0 = c0;
        uint64_t d1 = c1;
        if (!final_report) {
            d0 = c0 - s.win[kRewrite].load(std::memory_order_relaxed);
            d1 = c1 - s.win[kFirst].load(std::memory_order_relaxed);
            s.win[kRewrite].store(c0, std::memory_order_relaxed);
            s.win[kFirst].store(c1, std::memory_order_relaxed);
        }
        if (d0 > 0) {
            rewrite_rows.push_back(Row{i, d0});
        }
        if (d1 > 0) {
            first_rows.push_back(Row{i, d1});
        }
    }

    char head[320];
    if (final_report) {
        std::snprintf(head, sizeof head,
                      "SOTC ARENA ATTRIB FINAL: submits %llu, frames %llu, faults rewrite %llu, "
                      "first %llu, untracked %llu, dropped %llu, rips recorded %zu\n",
                      static_cast<unsigned long long>(now.submits),
                      static_cast<unsigned long long>(now.frames),
                      static_cast<unsigned long long>(now.rewrite),
                      static_cast<unsigned long long>(now.first),
                      static_cast<unsigned long long>(now.untracked),
                      static_cast<unsigned long long>(now.dropped), used);
        out += head;
        EmitList(out, kRewrite, true, rewrite_rows, kTopRewrite);
        EmitList(out, kFirst, true, first_rows, kTopFirst);
        return;
    }
    if (win.rewrite + win.first + win.untracked + win.dropped == 0) {
        return; // no arena write faults in the window: nothing to say
    }
    std::snprintf(head, sizeof head,
                  "SOTC ARENA ATTRIB window %.1fs: frames %llu, submits %llu, faults rewrite %llu, "
                  "first %llu, untracked %llu, dropped %llu, rips recorded %zu\n",
                  seconds, static_cast<unsigned long long>(win.frames),
                  static_cast<unsigned long long>(win.submits),
                  static_cast<unsigned long long>(win.rewrite),
                  static_cast<unsigned long long>(win.first),
                  static_cast<unsigned long long>(win.untracked),
                  static_cast<unsigned long long>(win.dropped), used);
    out += head;
    EmitList(out, kRewrite, false, rewrite_rows, kTopRewrite);
    EmitList(out, kFirst, false, first_rows, kTopFirst);
}

void ExitReport() {
    std::lock_guard<std::mutex> lock(g_report_mutex);
    if (g_exit_done) {
        return;
    }
    g_exit_done = true;
    std::string out;
    Build(out, true, 0.0);
    std::fwrite(out.data(), 1, out.size(), stderr);
    std::fflush(stderr);
}

} // namespace

bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_ARENA_ATTRIB");
        const bool value = v && *v && std::atoi(v) != 0;
        if (value) {
            std::fprintf(stderr,
                         "SOTC ARENA ATTRIB: on (guest write faults on the arena by RIP; needs "
                         "SOTC_HOOK_ALLOC_PROBE=1)\n");
        }
        return value;
    }();
    return on;
}

void Init() {
    if (!On()) {
        return;
    }
    static const bool registered = [] {
        std::atexit(ExitReport);
        return true;
    }();
    (void)registered;
}

void OnBlock(uint64_t base, uint64_t size, const char* name, int kind) {
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
            return;
        }
    }
    if (n == kMaxBlocks) {
        return;
    }
    Block& b = g_blocks[n];
    const size_t pages = static_cast<size_t>((size + kPageMask) >> kPageShift);
    b.alloc_ep.reset(new std::atomic<uint32_t>[pages]()); // zero: never allocated
    b.base.store(base, std::memory_order_relaxed);
    b.size.store(size, std::memory_order_relaxed);
    b.active.store(true, std::memory_order_relaxed);
    g_block_count.store(n + 1, std::memory_order_release);
}

void OnFreeBlock(uint64_t base) {
    if (!On() || !base) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_block_mutex);
    const size_t n = g_block_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        if (g_blocks[i].base.load(std::memory_order_relaxed) == base) {
            g_blocks[i].active.store(false, std::memory_order_release);
        }
    }
}

void OnSubmit() {
    if (!On()) {
        return;
    }
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_submits.fetch_add(1, std::memory_order_relaxed);
}

void OnFrameBoundary() {
    if (!On()) {
        return;
    }
    g_frame.fetch_add(1, std::memory_order_relaxed);
}

void NoteAlloc(uint64_t addr, uint64_t size) {
    if (!On() || !addr || !size) {
        return;
    }
    Block* b = nullptr;
    uint64_t first = 0;
    if (!FindBlock(addr, &b, &first)) {
        return;
    }
    const uint64_t base = b->base.load(std::memory_order_relaxed);
    const uint64_t block_end = base + b->size.load(std::memory_order_relaxed);
    const uint64_t end = std::min(addr + size, block_end);
    const uint64_t last = (end - base - 1) >> kPageShift; // inclusive: the pages the allocation touches
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    for (uint64_t p = first; p <= last; ++p) {
        b->alloc_ep[p].store(epoch, std::memory_order_relaxed);
    }
}

uint32_t CurrentThread() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

FaultThreadScope::FaultThreadScope(uint32_t tid) : prev_(t_fault_tid) {
    if (tid != 0) {
        t_fault_tid = tid;
    }
}

FaultThreadScope::~FaultThreadScope() {
    t_fault_tid = prev_;
}

void NoteWriteFault(uint64_t rip, void* ctx, uint64_t addr) {
    if (!On()) {
        return;
    }
    Block* b = nullptr;
    uint64_t index = 0;
    if (!FindBlock(addr, &b, &index)) {
        return; // not an arena page
    }
    const uint32_t epoch = b->alloc_ep[index].load(std::memory_order_relaxed);
    if (epoch == 0) {
        g_untracked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t now = g_epoch.load(std::memory_order_relaxed);
    const int cls = epoch < now ? kRewrite : kFirst;
    (cls == kRewrite ? g_rewrite : g_first).fetch_add(1, std::memory_order_relaxed);
    if (rip == 0) {
        g_dropped.fetch_add(1, std::memory_order_relaxed); // no RIP to attribute it to
        return;
    }
    Slot* s = FindSlot(rip);
    if (!s) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t frame = g_frame.load(std::memory_order_relaxed) + 1;
    uint64_t expect = 0;
    s->first_frame.compare_exchange_strong(expect, frame, std::memory_order_relaxed);
    s->last_frame.store(frame, std::memory_order_relaxed);
    s->count[cls].fetch_add(1, std::memory_order_relaxed);
    s->tid.store(t_fault_tid ? t_fault_tid : CurrentThread(), std::memory_order_relaxed);
    uint64_t c1 = 0;
    uint64_t c2 = 0;
#ifdef _WIN32
    WalkCallers(ContextRbp(ctx), &c1, &c2);
#else
    (void)ctx;
    WalkCallers(0, &c1, &c2);
#endif
    s->caller[0].store(c1, std::memory_order_relaxed);
    s->caller[1].store(c2, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_report_mutex);
    if (g_exit_done) {
        return;
    }
    Build(out, false, seconds);
}

} // namespace SotcArenaAttrib
