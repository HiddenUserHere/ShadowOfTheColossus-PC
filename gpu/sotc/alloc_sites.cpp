// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ALLOC_SITES (alloc_sites.h).
#include "alloc_sites.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "video_core/sotc_trace.h" // ImageBase / ImageSize (safe reads of image memory)
#include "gpu_sites.h"             // sotc: learned GPU-written sites (SOTC_GPU_PLACE), fed from NoteGpuBuffer

extern "C" const unsigned char* runtime_memory_backing_pointer(uintptr_t address);

namespace SotcAllocSites {
namespace {

enum Flag : uint32_t {
    kGpuWriteBase = 1,    // a written buffer bind starts inside the allocation
    kGpuWriteOverlap = 2, // a written buffer bind's range reaches into it (the bound size may be a superset)
    kGpuRead = 4,         // a read buffer bind starts inside it
    kImgWrite = 8,        // a render target / depth / storage image starts inside it
    kImgRead = 16,        // a sampled image starts inside it
    kCpuFault = 32,       // a TRACK=3 write fault (the CPU wrote a GPU-written page) inside it
    kWriteOwn = 64,       // a written buffer bind of this buffer: base = the allocation start, size <= its size
                          // (+0x100 for V# rounding)
    kWriteForeign = 128,  // a written buffer bind that starts inside it at another offset or reaches past its end
                          // (another buffer's range, e.g. memory the allocation shares with a larger view)
};
constexpr int kFlagCount = 8;

struct Site {
    std::atomic<uint64_t> key{0}; // hash + 1 (0 = empty)
    uint64_t c1{}, c2{}, c3{};
    uint32_t slot{};
    std::atomic<uint64_t> allocs{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> finished{0};                    // allocations replaced by a later one
    std::array<std::atomic<uint64_t>, kFlagCount> with{}; // finished allocations that had each flag
    std::atomic<uint64_t> touched{0};                     // finished allocations with any GPU flag
    std::atomic<uint64_t> foreign_sample{0}; // first foreign written bind: offset << 32 | min(size, 4 GiB - 1), +1
    std::atomic<bool> learned{false};        // SotcGpuSites::Learn was called for this site (once)
};
constexpr size_t kSites = size_t{1} << 16;
std::array<Site, kSites> g_sites{};
std::atomic<uint64_t> g_site_overflow{0};

struct Live {
    uint64_t end;
    uint32_t site;
    uint32_t flags;
    uint32_t trace; // SOTC_ALLOC_TRACE record, ~0u when none
};

// SOTC_ALLOC_TRACE records (alloc_sites.h, Capture).
struct TraceRec {
    uint64_t c1, c2, c3, addr, size;
    uint32_t flags;
    uint32_t done;
    int64_t born_ms;   // steady clock at the allocation (the dump writes its age)
    uint64_t chain[8]; // return addresses up the frame-pointer chain from the caller's frame (image offsets)
    uint64_t regs[16];
    uint64_t stack[64];
    uint64_t deref[8][16];
};
constexpr size_t kMaxTrace = 5000;
std::vector<TraceRec> g_trace; // under g_mu
struct TraceSite {
    uint64_t c1, c2;
};
const std::vector<TraceSite>& TraceSites() {
    static const std::vector<TraceSite> sites = [] {
        std::vector<TraceSite> out;
        const char* v = std::getenv("SOTC_ALLOC_TRACE");
        if (!v || !v[0] || std::strcmp(v, "mixed") == 0) {
            // The mixed sites of the 5 Nov Lab run (docs/re-stutters, mixed_sites).
            out = {{0x8bf0b, 0x122bcd}, {0x8bf0b, 0x122bf9}, {0x8bf0b, 0x122cbd}, {0x8bf0b, 0x122c91},
                   {0x8bf0b, 0x193028}, {0x8bf0b, 0x192ff1}, {0x8bf0b, 0x1940f6}, {0x1b397a, 0x19aedf},
                   {0x1af9c3, 0},       {0x1afbdb, 0}};
            return out;
        }
        while (*v) {
            char* end = nullptr;
            const uint64_t a = std::strtoull(v, &end, 16);
            uint64_t b = 0;
            if (end && *end == ':') {
                b = std::strtoull(end + 1, &end, 16);
            }
            out.push_back({a, b});
            if (!end || *end != ',') {
                break;
            }
            v = end + 1;
        }
        return out;
    }();
    return sites;
}

// Reads n bytes of guest memory at p into out without faulting: image memory directly, guest RAM through the
// backing view (within one 16 KiB guest page). Unreadable bytes stay zero.
void SafeRead(uint64_t p, void* out, size_t n) {
    std::memset(out, 0, n);
    if (p == 0) {
        return;
    }
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t isize = SotcTrace::ImageSize();
    if (base != 0 && p >= base && p - base < isize) {
        const size_t m = static_cast<size_t>(std::min<uint64_t>(n, base + isize - p));
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), m);
        return;
    }
    const unsigned char* b = runtime_memory_backing_pointer(p);
    if (!b) {
        return;
    }
    const size_t m = static_cast<size_t>(std::min<uint64_t>(n, 0x4000 - (p & 0x3fff)));
    std::memcpy(out, b, m);
}
std::mutex g_mu;
std::map<uint64_t, Live> g_live; // start -> allocation
std::atomic<uint64_t> g_unowned_binds{0};
std::atomic<uint64_t> g_unowned_faults{0};
std::atomic<uint64_t> g_freed{0};    // allocations ended by a free
std::atomic<uint64_t> g_replaced{0}; // allocations ended by a later overlapping allocation (a free not seen)
std::array<std::atomic<uint64_t>, kFreeSources> g_free_by{};      // per FreeSource: frees that ended an allocation
std::array<std::atomic<uint64_t>, kFreeSources> g_free_unknown{}; // per FreeSource: frees of no live allocation
std::atomic<uint64_t> g_reallocs{0};       // realloc calls with a non-zero result
std::atomic<uint64_t> g_realloc_moved{0};  // ... that moved the block
std::atomic<uint64_t> g_realloc_nosite{0}; // ... whose old pointer started no live allocation (or was 0)
std::atomic<uint64_t> g_calls{0};
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();
std::mutex g_report_mu;

uint64_t Hash(uint64_t c1, uint64_t c2, uint64_t c3, uint32_t slot) {
    uint64_t h = c1 * 0x9E3779B97F4A7C15ull;
    h ^= (c2 + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
    h ^= (c3 + 0x165667B1ull) * 0x165667B19E3779F9ull;
    h ^= uint64_t{slot} * 0x27D4EB2F165667C5ull;
    return h ^ (h >> 29);
}

uint32_t SiteIndex(uint64_t c1, uint64_t c2, uint64_t c3, uint32_t slot) {
    const uint64_t h = Hash(c1, c2, c3, slot);
    const uint64_t key = h + 1;
    const size_t start = static_cast<size_t>(h) & (kSites - 1);
    for (size_t probe = 0; probe < kSites; ++probe) {
        Site& s = g_sites[(start + probe) & (kSites - 1)];
        uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == key) {
            return static_cast<uint32_t>((start + probe) & (kSites - 1));
        }
        if (k == 0) {
            // Fill the identity first, then publish the key (readers check the key before the fields).
            uint64_t expected = 0;
            if (s.key.compare_exchange_strong(expected, ~uint64_t{0}, std::memory_order_acq_rel)) {
                s.c1 = c1;
                s.c2 = c2;
                s.c3 = c3;
                s.slot = slot;
                s.key.store(key, std::memory_order_release);
                return static_cast<uint32_t>((start + probe) & (kSites - 1));
            }
            // Another thread is filling this bin: wait for its key, then compare.
            while ((k = s.key.load(std::memory_order_acquire)) == ~uint64_t{0}) {
            }
            if (k == key) {
                return static_cast<uint32_t>((start + probe) & (kSites - 1));
            }
        }
    }
    g_site_overflow.fetch_add(1, std::memory_order_relaxed);
    return ~0u;
}

void Finish(const Live& a, uint32_t how) { // under g_mu; how: 1 = replaced, 2 = freed
    if (a.trace != ~0u && a.trace < g_trace.size()) {
        g_trace[a.trace].flags = a.flags;
        g_trace[a.trace].done = how;
    }
    (how == 2 ? g_freed : g_replaced).fetch_add(1, std::memory_order_relaxed);
    if (a.site == ~0u) {
        return;
    }
    Site& s = g_sites[a.site];
    s.finished.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < kFlagCount; ++i) {
        if (a.flags & (1u << i)) {
            s.with[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (a.flags & (kGpuWriteBase | kGpuRead | kImgWrite | kImgRead)) {
        s.touched.fetch_add(1, std::memory_order_relaxed);
    }
}

// A new live allocation [addr, end) (under g_mu): allocations it overlaps are dead (their memory was handed out again
// without a free seen), so they are finished first.
void Insert(uint64_t addr, uint64_t end, uint32_t site, uint32_t trace) {
    auto it = g_live.upper_bound(addr);
    if (it != g_live.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > addr) {
            it = prev;
        }
    }
    while (it != g_live.end() && it->first < end) {
        Finish(it->second, 1);
        it = g_live.erase(it);
    }
    if (trace != ~0u && trace < g_trace.size()) {
        g_trace[trace].addr = addr;
    }
    g_live.emplace(addr, Live{end, site, 0, trace});
}

// The allocation containing addr (under g_mu), or end().
std::map<uint64_t, Live>::iterator Containing(uint64_t addr) {
    auto it = g_live.upper_bound(addr);
    if (it == g_live.begin()) {
        return g_live.end();
    }
    --it;
    return it->second.end > addr ? it : g_live.end();
}

const char* ClassOf(uint64_t allocs, uint64_t gpu_write, uint64_t gpu_read, uint64_t touched_any) {
    if (gpu_write != 0) {
        return gpu_write == allocs ? "GPU_WRITTEN" : "GPU_WRITTEN_MIXED";
    }
    if (gpu_read != 0) {
        return gpu_read == allocs ? "GPU_READ" : "GPU_READ_MIXED";
    }
    return touched_any != 0 ? "GPU_OVERLAP_ONLY" : "CPU_ONLY";
}

void Report() {
    const auto t = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{g_report_mu};
        if (t - g_last < std::chrono::seconds(5)) {
            return;
        }
        g_last = t;
    }
    // Per site: finished counts plus the live allocations' flags (long-lived buffers never finish).
    struct Row {
        uint32_t site;
        uint64_t allocs, live, write, overlap, read, img_w, img_r, fault, touched, own, foreign;
    };
    std::vector<Row> rows;
    std::vector<std::array<uint64_t, kFlagCount + 2>> live_counts(kSites); // flags..., live, live touched
    uint64_t live_total = 0;
    {
        std::scoped_lock lock{g_mu};
        live_total = g_live.size();
        for (const auto& [start, a] : g_live) {
            if (a.site == ~0u) {
                continue;
            }
            auto& c = live_counts[a.site];
            for (int i = 0; i < kFlagCount; ++i) {
                if (a.flags & (1u << i)) {
                    ++c[i];
                }
            }
            ++c[kFlagCount];
            if (a.flags & (kGpuWriteBase | kGpuRead | kImgWrite | kImgRead)) {
                ++c[kFlagCount + 1];
            }
        }
    }
    for (uint32_t i = 0; i < kSites; ++i) {
        const Site& s = g_sites[i];
        const uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == 0 || k == ~uint64_t{0}) {
            continue;
        }
        const auto& c = live_counts[i];
        Row r{i,
              s.finished.load() + c[kFlagCount],
              c[kFlagCount],
              s.with[0].load() + c[0],
              s.with[1].load() + c[1],
              s.with[2].load() + c[2],
              s.with[3].load() + c[3],
              s.with[4].load() + c[4],
              s.with[5].load() + c[5],
              s.touched.load() + c[kFlagCount + 1],
              s.with[6].load() + c[6],
              s.with[7].load() + c[7]};
        if (r.allocs != 0) {
            rows.push_back(r);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return (a.write + a.img_w) != (b.write + b.img_w) ? (a.write + a.img_w) > (b.write + b.img_w)
                                                          : a.allocs > b.allocs;
    });
    // Table file (whole table each time).
    const char* path = std::getenv("SOTC_ALLOC_SITES_FILE");
    if (!path || !path[0]) {
        path = "user/alloc_sites.tsv";
    }
    if (FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "class\tslot\tcaller1\tcaller2\tcaller3\tallocs\tlive\tbytes\tgpu_write\tgpu_write_overlap\t"
                        "gpu_read\timage_write\timage_read\tcpu_fault_on_gpu_page\tgpu_touched\twrite_own\t"
                        "write_foreign\tforeign_sample_off\tforeign_sample_size\n");
        for (const Row& r : rows) {
            const Site& s = g_sites[r.site];
            const char* cls = ClassOf(r.allocs, r.write + r.img_w, r.read + r.img_r, r.touched + r.overlap);
            const uint64_t fs = s.foreign_sample.load(std::memory_order_relaxed);
            std::fprintf(f,
                         "%s\t%u\t%#llx\t%#llx\t%#llx\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t"
                         "%llu\t%#llx\t%#llx\n",
                         cls, s.slot, (unsigned long long)s.c1, (unsigned long long)s.c2, (unsigned long long)s.c3,
                         (unsigned long long)r.allocs, (unsigned long long)r.live,
                         (unsigned long long)s.bytes.load(), (unsigned long long)r.write,
                         (unsigned long long)r.overlap, (unsigned long long)r.read, (unsigned long long)r.img_w,
                         (unsigned long long)r.img_r, (unsigned long long)r.fault, (unsigned long long)r.touched,
                         (unsigned long long)r.own, (unsigned long long)r.foreign,
                         fs != 0 ? (unsigned long long)((fs - 1) >> 32) : 0ull,
                         fs != 0 ? (unsigned long long)((fs - 1) & 0xffffffffull) : 0ull);
        }
        std::fclose(f);
    }
    // SOTC_ALLOC_TRACE: every record with its current flags.
    {
        std::vector<TraceRec> copy;
        {
            std::scoped_lock lock{g_mu};
            for (const auto& [start, a] : g_live) { // live records: current flags
                if (a.trace != ~0u && a.trace < g_trace.size()) {
                    g_trace[a.trace].flags = a.flags;
                }
            }
            copy = g_trace;
        }
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        if (!copy.empty()) {
            const char* tpath = std::getenv("SOTC_ALLOC_TRACE_FILE");
            if (!tpath || !tpath[0]) {
                tpath = "user/alloc_trace.tsv";
            }
            if (FILE* f = std::fopen(tpath, "w")) {
                std::fprintf(f, "c1\tc2\tc3\taddr\tsize\tflags\tdone\tage_ms");
                for (int i = 0; i < 8; ++i) {
                    std::fprintf(f, "\tchain%d", i);
                }
                static const char* kRegs[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
                                                "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rsp"};
                static const char* kDeref[8] = {"rbx", "rsi", "rdi", "rbp", "r12", "r13", "r14", "r15"};
                for (const char* r : kRegs) {
                    std::fprintf(f, "\t%s", r);
                }
                for (int i = 0; i < 64; ++i) {
                    std::fprintf(f, "\tstk%03x", i * 8);
                }
                for (const char* r : kDeref) {
                    for (int i = 0; i < 16; ++i) {
                        std::fprintf(f, "\t[%s+%02x]", r, i * 8);
                    }
                }
                std::fprintf(f, "\n");
                for (const TraceRec& t : copy) {
                    std::fprintf(f, "%#llx\t%#llx\t%#llx\t%#llx\t%#llx\t%u\t%u", (unsigned long long)t.c1,
                                 (unsigned long long)t.c2, (unsigned long long)t.c3, (unsigned long long)t.addr,
                                 (unsigned long long)t.size, t.flags, t.done);
                    std::fprintf(f, "\t%lld", static_cast<long long>(now_ms - t.born_ms));
                    for (const uint64_t v : t.chain) {
                        std::fprintf(f, "\t%#llx", (unsigned long long)v);
                    }
                    for (const uint64_t v : t.regs) {
                        std::fprintf(f, "\t%#llx", (unsigned long long)v);
                    }
                    for (const uint64_t v : t.stack) {
                        std::fprintf(f, "\t%#llx", (unsigned long long)v);
                    }
                    for (const auto& d : t.deref) {
                        for (const uint64_t v : d) {
                            std::fprintf(f, "\t%#llx", (unsigned long long)v);
                        }
                    }
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        }
    }
    uint64_t n_written = 0, n_mixed = 0, n_read = 0, n_cpu = 0, n_fault_sites = 0;
    for (const Row& r : rows) {
        const std::string cls = ClassOf(r.allocs, r.write + r.img_w, r.read + r.img_r, r.touched + r.overlap);
        if (cls == "GPU_WRITTEN") {
            ++n_written;
        } else if (cls == "GPU_WRITTEN_MIXED") {
            ++n_mixed;
        } else if (cls.rfind("GPU_READ", 0) == 0) {
            ++n_read;
        } else {
            ++n_cpu;
        }
        n_fault_sites += r.fault != 0 ? 1 : 0;
    }
    std::string core;
    if (On()) {
        CoreReport(core);
    }
    std::fprintf(stderr,
                 "SOTC ALLOC SITES (5 s): %zu sites (%llu GPU-written, %llu GPU-written in only some allocations, "
                 "%llu GPU-read, %llu CPU-only or overlap only), %llu sites with CPU faults on GPU pages; %llu live "
                 "allocations; ended by free %llu (0x12af0 %llu, small %llu, BPE %llu), by a later allocation (free not "
                 "seen) %llu; frees of no live allocation 0x12af0 %llu, small %llu, BPE %llu; reallocs %llu (moved %llu, "
                 "of no live allocation %llu); binds/images outside any allocation %llu, faults outside %llu; site table "
                 "full %llu; table in %s%s\n",
                 rows.size(), (unsigned long long)n_written, (unsigned long long)n_mixed,
                 (unsigned long long)n_read, (unsigned long long)n_cpu, (unsigned long long)n_fault_sites,
                 (unsigned long long)live_total, (unsigned long long)g_freed.load(),
                 (unsigned long long)g_free_by[0].load(), (unsigned long long)g_free_by[1].load(),
                 (unsigned long long)g_free_by[2].load(), (unsigned long long)g_replaced.load(),
                 (unsigned long long)g_free_unknown[0].load(), (unsigned long long)g_free_unknown[1].load(),
                 (unsigned long long)g_free_unknown[2].load(), (unsigned long long)g_reallocs.load(),
                 (unsigned long long)g_realloc_moved.load(), (unsigned long long)g_realloc_nosite.load(),
                 (unsigned long long)g_unowned_binds.exchange(0),
                 (unsigned long long)g_unowned_faults.exchange(0), (unsigned long long)g_site_overflow.load(), path, core.c_str());
}

void MaybeReport() {
    if ((g_calls.fetch_add(1, std::memory_order_relaxed) & 0xffff) == 0) {
        Report();
    }
}

} // namespace

bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_ALLOC_SITES");
        return v && v[0] == '1';
    }();
    return on;
}

// ---- Size core (alloc_sites.h, CoreOn) ----
// 256 shards selected by a hash of (addr >> 4). Each shard is an unordered_map start -> size under its own spinlock,
// so the threads that allocate and free do not share one lock. Entries are erased at their free. Ring-heap blocks are
// never freed (later allocations reuse them and overwrite the entry), so stale entries can pile up: each shard is
// bounded, and a full shard drops one entry (counted as dropped). A dropped entry only means no size at that free.
namespace {

constexpr size_t kCoreShards = 256;
constexpr size_t kCoreReserve = 8192;      // buckets reserved per shard up front (no rehash storms)
constexpr size_t kCoreShardMax = 16384;    // entries per shard before one is dropped (~4 M in total, worst case)

struct CoreShard {
    std::atomic<bool> busy{false};
    std::unordered_map<uint64_t, uint64_t> map;
};

struct CoreLock {
    CoreShard& s;
    explicit CoreLock(CoreShard& shard) : s(shard) {
        while (s.busy.exchange(true, std::memory_order_acquire)) {
            while (s.busy.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        }
    }
    ~CoreLock() { s.busy.store(false, std::memory_order_release); }
    CoreLock(const CoreLock&) = delete;
    CoreLock& operator=(const CoreLock&) = delete;
};

CoreShard* CoreShards() {
    static CoreShard* shards = [] {
        auto* a = new CoreShard[kCoreShards];
        for (size_t i = 0; i < kCoreShards; ++i) {
            a[i].map.reserve(kCoreReserve);
        }
        return a;
    }();
    return shards;
}

CoreShard& CoreShardFor(uint64_t addr) {
    const uint64_t h = (addr >> 4) * 0x9E3779B97F4A7C15ull;
    return CoreShards()[static_cast<size_t>(h >> 56)]; // top 8 bits: 0..255
}

std::atomic<int64_t> g_core_entries{0}; // approximate: inserts minus erases and drops
std::atomic<uint64_t> g_core_dropped{0};
std::atomic<uint64_t> g_core_hits{0};
std::atomic<uint64_t> g_core_misses{0};

} // namespace

bool CoreOn() {
    static const bool on = [] {
        const char* a = std::getenv("SOTC_ALLOC_SITES");
        const char* b = std::getenv("SOTC_NATIVE_OWNER_DISCARD");
        return (a && a[0] == '1') || (b && b[0] == '2'); // the free-time discard needs the sizes
    }();
    return on;
}

void CoreNoteAlloc(uint64_t addr, uint64_t size) {
    if (addr == 0 || size == 0) {
        return;
    }
    CoreShard& s = CoreShardFor(addr);
    CoreLock lock{s};
    auto it = s.map.find(addr);
    if (it != s.map.end()) {
        it->second = size; // reused address: the later allocation wins
        return;
    }
    if (s.map.size() >= kCoreShardMax) {
        s.map.erase(s.map.begin()); // arbitrary victim: a missing entry is safe (no discard at that free)
        g_core_dropped.fetch_add(1, std::memory_order_relaxed);
        g_core_entries.fetch_sub(1, std::memory_order_relaxed);
    }
    s.map.emplace(addr, size);
    g_core_entries.fetch_add(1, std::memory_order_relaxed);
}

uint64_t CoreTakeFree(uint64_t addr) {
    if (addr == 0) {
        return 0;
    }
    CoreShard& s = CoreShardFor(addr);
    CoreLock lock{s};
    auto it = s.map.find(addr);
    if (it == s.map.end()) {
        g_core_misses.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t size = it->second;
    s.map.erase(it);
    g_core_entries.fetch_sub(1, std::memory_order_relaxed);
    g_core_hits.fetch_add(1, std::memory_order_relaxed);
    return size;
}

void CoreReport(std::string& out) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), " core: entries %lld, hits %llu, misses %llu, dropped %llu",
                  static_cast<long long>(g_core_entries.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_core_hits.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_core_misses.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_core_dropped.load(std::memory_order_relaxed)));
    out += buf;
}

uint32_t Capture(uint64_t c1, uint64_t c2, uint64_t c3, uint64_t size, const uint64_t* regs) {
    bool wanted = false;
    for (const TraceSite& s : TraceSites()) {
        if (s.c1 == c1 && (s.c2 == 0 || s.c2 == c2)) {
            wanted = true;
            break;
        }
    }
    if (!wanted) {
        return ~0u;
    }
    TraceRec t{};
    t.c1 = c1;
    t.c2 = c2;
    t.c3 = c3;
    t.size = size;
    t.born_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
    {
        // Up to 8 return addresses through the frame-pointer chain, from the caller's frame (rbp at the call).
        const uint64_t base = SotcTrace::ImageBase();
        const uint64_t isize = SotcTrace::ImageSize();
        uint64_t fp = regs[6];
        uint64_t lo = regs[15];
        for (int i = 0; i < 8; ++i) {
            if (fp < lo || fp >= lo + 0x200000 || (fp & 7) != 0) {
                break;
            }
            uint64_t frame[2] = {};
            SafeRead(fp, frame, sizeof frame); // [fp] = the caller's fp, [fp+8] = return address
            const uint64_t ret = frame[1];
            t.chain[i] = (base != 0 && ret >= base && ret - base < isize) ? ret - base : ret;
            lo = fp + 16;
            fp = frame[0];
        }
    }
    std::memcpy(t.regs, regs, sizeof t.regs);
    SafeRead(regs[15], t.stack, sizeof t.stack); // rsp
    static constexpr int kDerefRegs[8] = {1, 4, 5, 6, 11, 12, 13, 14}; // rbx rsi rdi rbp r12 r13 r14 r15
    for (int i = 0; i < 8; ++i) {
        SafeRead(regs[kDerefRegs[i]], t.deref[i], sizeof t.deref[i]);
    }
    std::scoped_lock lock{g_mu};
    if (g_trace.size() >= kMaxTrace) {
        return ~0u;
    }
    g_trace.push_back(t);
    return static_cast<uint32_t>(g_trace.size() - 1);
}

void NoteAlloc(uint64_t addr, uint64_t size, uint32_t slot, uint64_t c1, uint64_t c2, uint64_t c3, uint32_t trace) {
    if (addr == 0 || size == 0) {
        return;
    }
    const uint32_t site = SiteIndex(c1, c2, c3, slot);
    if (site != ~0u) {
        g_sites[site].allocs.fetch_add(1, std::memory_order_relaxed);
        g_sites[site].bytes.fetch_add(size, std::memory_order_relaxed);
    }
    const uint64_t end = addr + size;
    {
        std::scoped_lock lock{g_mu};
        Insert(addr, end, site, trace);
    }
    MaybeReport();
}

void NoteFree(uint64_t addr, FreeSource source) {
    if (addr == 0 || source >= kFreeSources) {
        return;
    }
    {
        std::scoped_lock lock{g_mu};
        auto it = g_live.find(addr);
        if (it == g_live.end()) {
            g_free_unknown[source].fetch_add(1, std::memory_order_relaxed);
        } else {
            g_free_by[source].fetch_add(1, std::memory_order_relaxed);
            Finish(it->second, 2);
            g_live.erase(it);
        }
    }
    MaybeReport();
}

void ReallocPre(uint64_t old_addr, uint32_t* site, uint32_t* trace) {
    *site = ~0u;
    *trace = ~0u;
    if (old_addr == 0) {
        return;
    }
    std::scoped_lock lock{g_mu};
    auto it = g_live.find(old_addr);
    if (it != g_live.end()) {
        *site = it->second.site;
        *trace = it->second.trace;
    }
}

void ReallocPost(uint64_t old_addr, uint64_t new_addr, uint64_t size, uint32_t site, uint32_t trace) {
    if (new_addr == 0) {
        return; // failed, or a realloc to size 0 (the old block was freed through its heap)
    }
    g_reallocs.fetch_add(1, std::memory_order_relaxed);
    if (site == ~0u) {
        g_realloc_nosite.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{g_mu};
        if (new_addr == old_addr) {
            auto it = g_live.find(old_addr);
            if (it != g_live.end()) {
                it->second.end = old_addr + (size != 0 ? size : 1); // resized in place: same allocation, same flags
                return;
            }
        } else {
            g_realloc_moved.fetch_add(1, std::memory_order_relaxed);
            if (auto it = g_live.find(old_addr); it != g_live.end()) {
                Finish(it->second, 2);
                g_live.erase(it);
            }
        }
        // The moved block continues the old allocation's site (no trace record: its capture was the old block's).
        if (site != ~0u) {
            g_sites[site].allocs.fetch_add(1, std::memory_order_relaxed);
            g_sites[site].bytes.fetch_add(size, std::memory_order_relaxed);
        }
        Insert(new_addr, new_addr + (size != 0 ? size : 1), site, new_addr == old_addr ? trace : ~0u);
    }
    MaybeReport();
}

// sotc: feeds one site to SotcGpuSites (once per site; slot 20, the render-target pool, is never learned).
static void LearnSite(uint32_t site) {
    Site& s = g_sites[site];
    if (s.slot == 20 || s.learned.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    SotcGpuSites::Learn(s.c1, s.c2, s.c3, s.slot, SotcGpuSites::SlotFlags(s.slot));
    SotcGpuSites::MaybeSave();
}

void NoteGpuBuffer(uint64_t addr, uint64_t size, bool written) {
    if (addr == 0) {
        return;
    }
    uint32_t learn_site = ~0u; // sotc: a written bind's site, learned after g_mu is released
    {
        std::scoped_lock lock{g_mu};
        auto it = Containing(addr);
        if (it == g_live.end()) {
            g_unowned_binds.fetch_add(1, std::memory_order_relaxed);
        } else {
            it->second.flags |= written ? kGpuWriteBase : kGpuRead;
            if (written) {
                const uint64_t off = addr - it->first;
                const uint64_t asize = it->second.end - it->first;
                if (off == 0 && size <= asize + 0x100) {
                    it->second.flags |= kWriteOwn;
                } else {
                    it->second.flags |= kWriteForeign;
                    if (it->second.site != ~0u) {
                        uint64_t expect = 0;
                        const uint64_t sample =
                            ((std::min<uint64_t>(off, 0xffffffffull) << 32) | std::min<uint64_t>(size, 0xfffffffeull)) +
                            1;
                        g_sites[it->second.site].foreign_sample.compare_exchange_strong(expect, sample,
                                                                                        std::memory_order_relaxed);
                    }
                }
                learn_site = it->second.site;
                // The rest of the bound range (a superset when the V# size is larger than the buffer): overlap only.
                const uint64_t end = addr + std::min<uint64_t>(size, uint64_t{1} << 20);
                for (auto n = std::next(it); n != g_live.end() && n->first < end; ++n) {
                    n->second.flags |= kGpuWriteOverlap;
                }
            }
        }
    }
    if (learn_site != ~0u) {
        LearnSite(learn_site);
    }
    MaybeReport();
}

void NoteGpuImage(uint64_t addr, uint64_t size, bool written) {
    (void)size;
    if (addr == 0) {
        return;
    }
    std::scoped_lock lock{g_mu};
    auto it = Containing(addr);
    if (it == g_live.end()) {
        g_unowned_binds.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    it->second.flags |= written ? kImgWrite : kImgRead;
}

void NoteCpuFault(uint64_t addr) {
    std::scoped_lock lock{g_mu};
    auto it = Containing(addr);
    if (it == g_live.end()) {
        g_unowned_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    it->second.flags |= kCpuFault;
}

} // namespace SotcAllocSites
