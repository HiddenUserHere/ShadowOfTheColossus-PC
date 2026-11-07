// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GPU region allocator. See gpu_region.h for the design.
//
// Layout of the reserved range (addresses grow to the right):
//   [base .. committed_end)   granules committed through the batch host
//   [base .. bump)            handed out: slabs, large blocks, and gaps recorded as free runs
//   [bump .. limit)           not yet used
// Small blocks come from 64 KiB slabs taken from the bump pointer, one class per slab. Large blocks are first
// fit from the free runs (page-aligned, coalesced on release), else taken from the bump pointer. A run that
// ends at the bump pointer is returned to the bump pointer, so the range does not fragment at its top.
//
// Lock order: g_mutex only. The release hook and the host calls run with the lock held, except the release hook,
// which runs with it released so it may query the allocator (Contains, SizeOf).

#include "gpu_region.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
// runtime_memory.c: the reserve (sceKernelReserveVirtualRange) and batch map (sceKernelBatchMap) exports.
int32_t runtime_memory_reserve_range_export(void** inout, uint64_t size, int flags, uint64_t alignment);
int32_t runtime_memory_batch_map_export(void* entries, int count, int* processed);
// sotc: backing for the region outside the game's flexible budget (runtime_memory.c).
int32_t runtime_memory_port_flexible_export(uint64_t start, uint64_t size, int prot);
}

namespace SotcGpuRegion {
namespace {

// Same 32-byte layout as the port's BatchEntry (src/runtime_memory.c) and the game's queue entry.
struct BatchEntry {
    uint64_t start;
    uint64_t offset;
    uint64_t length;
    uint8_t prot;
    uint8_t type;
    uint16_t reserved;
    uint32_t operation;
};
static_assert(sizeof(BatchEntry) == 32, "BatchEntry must match the 32-byte batch queue entry");

constexpr uint32_t kBatchOpMapFlexible = 3;  // map_flexible at the fixed start address
constexpr uint64_t kPortChunk = uint64_t{16} << 20; // port commits: 16 MiB per map
constexpr uint8_t kProtCpuGpuReadWrite = 0x33;  // CPU read/write plus GPU read/write (SCE_KERNEL_PROT_GPU_ALL)
constexpr int kBatchChunk = 64;              // the game's batch queue depth

// Size classes: 16-byte steps up to 256, then powers of two up to 4096. A class serves a request when it is at
// least the size and a multiple of the alignment (both powers of two, so the offset inside a granule-aligned
// slab keeps the alignment).
constexpr uint64_t kClassSize[] = {16,  32,  48,  64,  80,  96,  112,  128,  144,  160,
                                   176, 192, 208, 224, 240, 256, 512, 1024, 2048, 4096};
constexpr int kClassCount = static_cast<int>(sizeof(kClassSize) / sizeof(kClassSize[0]));

constexpr uint64_t kDefaultRingFrames = 4;
constexpr uint64_t kDefaultQuarantineFrames = 3;

struct LiveBlock {
    uint64_t size;   // rounded usable size
    uint32_t slot;
    uint32_t kind;
    uint64_t born;   // frame of the allocation
};

struct QuarantinedBlock {
    uint64_t addr;
    uint64_t size;
    uint32_t slot;
    uint32_t kind;
    uint64_t born;
    uint64_t frame;  // frame of the Free() or ring expiry that put it here
};

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);  // alignment is a power of two
}

uint64_t NextPow2(uint64_t value) {
    uint64_t p = 1;
    while (p < value) {
        p <<= 1;
    }
    return p;
}

int ClassIndexOf(uint64_t size) {
    for (int i = 0; i < kClassCount; ++i) {
        if (kClassSize[i] == size) {
            return i;
        }
    }
    return -1;
}

uint64_t EnvFrames(const char* name, uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value > 1000000ull) {
        return fallback;
    }
    return static_cast<uint64_t>(value);
}

Host g_host = {&runtime_memory_reserve_range_export, &runtime_memory_batch_map_export,
               &runtime_memory_port_flexible_export};

std::mutex g_mutex;
// Read without the lock by Contains. Written by Init and ResetForTest under g_mutex.
std::atomic<uint64_t> g_base{0};
std::atomic<uint64_t> g_limit{0};

bool g_inited = false;
uint64_t g_reserved = 0;
uint64_t g_committed_end = 0;  // granule-aligned, absolute
uint64_t g_bump = 0;           // page-aligned, absolute
uint64_t g_frame = 0;
uint64_t g_ring_frames = kDefaultRingFrames;
bool g_page_isolate = true;
uint64_t g_quarantine_frames = kDefaultQuarantineFrames;

std::vector<uint64_t> g_class_free[kClassCount];  // free block starts per class
uint64_t g_slab_next[kClassCount] = {};           // next free offset in the class's current slab (0: none)
uint64_t g_slab_end[kClassCount] = {};
std::map<uint64_t, uint64_t> g_free_runs;         // start -> length of free page-aligned runs below bump

std::unordered_map<uint64_t, LiveBlock> g_live;   // side table of live blocks, keyed by start
// Ring blocks in allocation order (start, born frame): OnFrame expires from the front instead of scanning g_live.
// An entry whose block was freed or reused since (born differs) is skipped.
std::deque<std::pair<uint64_t, uint64_t>> g_ring_fifo;
std::vector<QuarantinedBlock> g_quarantine;

void (*g_release_fn)(void* ctx, uint64_t addr, uint64_t size) = nullptr;
void* g_release_ctx = nullptr;
std::atomic<void (*)(void*, uint64_t, uint64_t)> g_quarantine_fn{nullptr};
std::atomic<void*> g_quarantine_ctx{nullptr};

uint64_t g_live_bytes = 0;
uint64_t g_released = 0;
uint64_t g_allocs_total = 0;
uint64_t g_frees_total = 0;
uint64_t g_allocs_window = 0;
uint64_t g_frees_window = 0;
uint64_t g_ring_expired = 0;
uint64_t g_alloc_failures = 0;
uint64_t g_unknown_frees = 0;

// Maps the granules from g_committed_end up to end (granule-aligned, absolute) through the batch host.
// Advances g_committed_end by what the host applied. Returns false when the host failed part of it.
bool CommitTo(uint64_t end) {
    if (g_host.port_map != nullptr) {
        // 16 MiB maps outside the game's flexible budget (a chunk past the reservation is trimmed).
        while (g_committed_end < end) {
            uint64_t length = kPortChunk;
            if (g_committed_end + length > g_limit.load(std::memory_order_relaxed)) {
                length = g_limit.load(std::memory_order_relaxed) - g_committed_end;
            }
            if (g_host.port_map(g_committed_end, length, kProtCpuGpuReadWrite) != 0) {
                return false;
            }
            g_committed_end += length;
        }
        return true;
    }
    while (g_committed_end < end) {
        BatchEntry entries[kBatchChunk];
        int count = 0;
        for (uint64_t cursor = g_committed_end; count < kBatchChunk && cursor < end; cursor += kGranule) {
            entries[count].start = cursor;
            entries[count].offset = 0;
            entries[count].length = kGranule;
            entries[count].prot = kProtCpuGpuReadWrite;
            entries[count].type = 0;
            entries[count].reserved = 0;
            entries[count].operation = kBatchOpMapFlexible;
            ++count;
        }
        int processed = 0;
        const int32_t rc = g_host.batch_map(entries, count, &processed);
        if (processed < 0) {
            processed = 0;
        }
        if (processed > count) {
            processed = count;
        }
        g_committed_end += static_cast<uint64_t>(processed) * kGranule;
        if (rc != 0 || processed != count) {
            return false;
        }
    }
    return true;
}

// Returns a free run to the allocator: merges it with its neighbours, and gives it back to the bump pointer
// when it ends there.
void AddFreeRun(uint64_t start, uint64_t length) {
    if (length == 0) {
        return;
    }
    auto after = g_free_runs.lower_bound(start);
    if (after != g_free_runs.end() && after->first == start + length) {
        length += after->second;
        after = g_free_runs.erase(after);
    }
    if (after != g_free_runs.begin()) {
        auto before = std::prev(after);
        if (before->first + before->second == start) {
            start = before->first;
            length += before->second;
            g_free_runs.erase(before);
        }
    }
    if (start + length == g_bump) {
        g_bump = start;
        return;
    }
    g_free_runs[start] = length;
}

// Takes size bytes at the given power-of-two alignment from the bump pointer, committing what it needs first.
// The skipped gap becomes a free run. Returns false (state unchanged) on overflow, range exhaustion or a failed
// commit.
bool TakeFromBump(uint64_t size, uint64_t alignment, uint64_t* out) {
    const uint64_t start = AlignUp(g_bump, alignment);
    const uint64_t end = start + size;
    if (end < start || end > g_limit.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!CommitTo(AlignUp(end, kGranule))) {
        return false;
    }
    if (start > g_bump) {
        AddFreeRun(g_bump, start - g_bump);
    }
    g_bump = end;
    *out = start;
    return true;
}

// First fit over the free runs. Returns false when no run holds an aligned block of size bytes.
bool TakeFromRuns(uint64_t size, uint64_t alignment, uint64_t* out) {
    for (auto it = g_free_runs.begin(); it != g_free_runs.end(); ++it) {
        const uint64_t run_start = it->first;
        const uint64_t run_length = it->second;
        const uint64_t start = AlignUp(run_start, alignment);
        if (start < run_start || start + size > run_start + run_length) {
            continue;
        }
        g_free_runs.erase(it);
        if (start > run_start) {
            g_free_runs[run_start] = start - run_start;
        }
        const uint64_t tail = run_start + run_length - (start + size);
        if (tail > 0) {
            g_free_runs[start + size] = tail;
        }
        *out = start;
        return true;
    }
    return false;
}

}  // namespace

bool Init(uint64_t reserve_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inited) {
        return true;
    }
    if (reserve_bytes == 0) {
        reserve_bytes = kDefaultReserve;
    }
    const uint64_t size = AlignUp(reserve_bytes, kGranule);
    if (size < reserve_bytes || size > kRangeHi - kRangeLo) {
        return false;
    }
    void* va = nullptr;
    const int32_t rc = g_host.reserve(&va, size, 0, kGranule);
    const uint64_t base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(va));
    if (rc != 0 || base == 0) {
        std::fprintf(stderr, "GPU region: reserve of %" PRIu64 " bytes failed (0x%x)\n", size,
                     static_cast<unsigned>(rc));
        return false;
    }
    if (base % kGranule != 0 || base < kRangeLo || base > kRangeHi - size) {
        std::fprintf(stderr, "GPU region: reserved range 0x%" PRIx64 " (+%" PRIu64 ") is outside the region bounds\n",
                     base, size);
        return false;
    }
    g_reserved = size;
    g_committed_end = base;
    g_bump = base;
    g_frame = 0;
    g_ring_frames = EnvFrames("SOTC_GPU_REGION_RING_FRAMES", kDefaultRingFrames);
    {
        const char* v = std::getenv("SOTC_GPU_REGION_PAGE_ISOLATE");
        g_page_isolate = !(v && v[0] == '0');
    }
    g_quarantine_frames = EnvFrames("SOTC_GPU_REGION_QUARANTINE_FRAMES", kDefaultQuarantineFrames);
    g_inited = true;
    g_limit.store(base + size, std::memory_order_relaxed);
    g_base.store(base, std::memory_order_release);
    return true;
}

uint64_t Alloc(uint64_t size, uint64_t align, uint32_t slot, uint32_t kind) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_inited || kind > kKindRing || size > kRangeHi || align > kRangeHi) {
        ++g_alloc_failures;
        return 0;
    }
    if (size == 0) {
        size = 16;
    }
    // sotc: SOTC_GPU_REGION_PAGE_ISOLATE (default 1): a small block gets a 4 KiB page of its own, so the CPU's writes
    // into one block (e.g. an init after the allocation) never hit a page another block's GPU data protects.
    if (g_page_isolate && size < 4096) {
        size = 4096;
    }
    if (align < 16) {
        align = 16;
    }
    align = NextPow2(align);

    uint64_t addr = 0;
    uint64_t usable = 0;
    if (size <= kSmallMax && align <= kSmallMax) {
        int index = -1;
        for (int i = 0; i < kClassCount; ++i) {
            if (kClassSize[i] >= size && kClassSize[i] % align == 0) {
                index = i;
                break;
            }
        }
        const uint64_t class_size = kClassSize[index];
        usable = class_size;
        if (!g_class_free[index].empty()) {
            addr = g_class_free[index].back();
            g_class_free[index].pop_back();
        } else {
            if (g_slab_next[index] == 0 || g_slab_next[index] + class_size > g_slab_end[index]) {
                uint64_t slab = 0;
                if (!TakeFromBump(kGranule, kGranule, &slab)) {
                    ++g_alloc_failures;
                    return 0;
                }
                g_slab_next[index] = slab;
                g_slab_end[index] = slab + kGranule;
            }
            addr = g_slab_next[index];
            g_slab_next[index] += class_size;
        }
    } else {
        usable = AlignUp(size, kPage);
        const uint64_t run_alignment = std::max<uint64_t>(align, kPage);
        if (!TakeFromRuns(usable, run_alignment, &addr) && !TakeFromBump(usable, run_alignment, &addr)) {
            ++g_alloc_failures;
            return 0;
        }
    }

    g_live[addr] = LiveBlock{usable, slot, kind, g_frame};
    if (kind == kKindRing) {
        g_ring_fifo.emplace_back(addr, g_frame);
    }
    g_live_bytes += usable;
    ++g_allocs_total;
    ++g_allocs_window;
    return addr;
}

bool Contains(uint64_t addr) {
    const uint64_t base = g_base.load(std::memory_order_acquire);
    const uint64_t limit = g_limit.load(std::memory_order_relaxed);
    return base != 0 && addr >= base && addr < limit;
}

uint64_t SizeOf(uint64_t addr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_live.find(addr);
    return it == g_live.end() ? 0 : it->second.size;
}

bool SlotOf(uint64_t addr, uint32_t* slot) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_live.find(addr);
    if (it == g_live.end() || slot == nullptr) {
        return false;
    }
    *slot = it->second.slot;
    return true;
}

void Free(uint64_t addr) {
    uint64_t size = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_live.find(addr);
        if (it == g_live.end()) {
            ++g_unknown_frees;
            return;
        }
        const LiveBlock block = it->second;
        g_live.erase(it);
        g_live_bytes -= block.size;
        g_quarantine.push_back(QuarantinedBlock{addr, block.size, block.slot, block.kind, block.born, g_frame});
        ++g_frees_total;
        ++g_frees_window;
        size = block.size;
    }
    if (auto* fn = g_quarantine_fn.load(std::memory_order_acquire)) {
        fn(g_quarantine_ctx.load(std::memory_order_relaxed), addr, size);
    }
}

void OnFrame(uint64_t frame) {
    std::vector<QuarantinedBlock> expired;
    std::vector<QuarantinedBlock> entered; // ring blocks that entered the quarantine now (the quarantine hook)
    void (*release_fn)(void*, uint64_t, uint64_t) = nullptr;
    void* release_ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_inited) {
            return;
        }
        g_frame = frame;

        // Ring blocks older than the ring frame count move to quarantine.
        while (!g_ring_fifo.empty()) {
            const auto [addr, born] = g_ring_fifo.front();
            if (frame < born || frame - born <= g_ring_frames) {
                break; // the rest is younger
            }
            g_ring_fifo.pop_front();
            const auto it = g_live.find(addr);
            if (it == g_live.end() || it->second.kind != kKindRing || it->second.born != born) {
                continue; // freed, or the address was handed out again since
            }
            const LiveBlock block = it->second;
            g_quarantine.push_back(QuarantinedBlock{addr, block.size, block.slot, block.kind, block.born, frame});
            entered.push_back(g_quarantine.back());
            g_live_bytes -= block.size;
            ++g_ring_expired;
            g_live.erase(it);
        }

        // Quarantined blocks older than the quarantine frame count are released.
        std::vector<QuarantinedBlock> keep;
        keep.reserve(g_quarantine.size());
        for (const QuarantinedBlock& q : g_quarantine) {
            if (frame >= q.frame && frame - q.frame > g_quarantine_frames) {
                expired.push_back(q);
            } else {
                keep.push_back(q);
            }
        }
        g_quarantine.swap(keep);
        release_fn = g_release_fn;
        release_ctx = g_release_ctx;
    }
    if (auto* fn = g_quarantine_fn.load(std::memory_order_acquire); fn != nullptr && !entered.empty()) {
        void* ctx = g_quarantine_ctx.load(std::memory_order_relaxed);
        for (const QuarantinedBlock& q : entered) {
            fn(ctx, q.addr, q.size);
        }
    }
    if (expired.empty()) {
        return;
    }

    // Hook first, with the lock released: the blocks are still unusable until it returns. Adjacent ranges are
    // merged into one call.
    std::sort(expired.begin(), expired.end(),
              [](const QuarantinedBlock& a, const QuarantinedBlock& b) { return a.addr < b.addr; });
    if (release_fn != nullptr) {
        size_t i = 0;
        while (i < expired.size()) {
            uint64_t start = expired[i].addr;
            uint64_t end = start + expired[i].size;
            size_t j = i + 1;
            while (j < expired.size() && expired[j].addr == end) {
                end += expired[j].size;
                ++j;
            }
            release_fn(release_ctx, start, end - start);
            i = j;
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const QuarantinedBlock& q : expired) {
        const int index = q.size <= kSmallMax ? ClassIndexOf(q.size) : -1;
        if (index >= 0) {
            g_class_free[index].push_back(q.addr);
        } else {
            AddFreeRun(q.addr, q.size);
        }
        ++g_released;
    }
}

void SetQuarantineHook(void (*fn)(void* ctx, uint64_t addr, uint64_t size), void* ctx) {
    g_quarantine_ctx.store(ctx, std::memory_order_relaxed);
    g_quarantine_fn.store(fn, std::memory_order_release);
}

void SetReleaseHook(void (*fn)(void* ctx, uint64_t addr, uint64_t size), void* ctx) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_release_fn = fn;
    g_release_ctx = ctx;
}

Counters GetCounters() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Counters c;
    c.reserved_bytes = g_reserved;
    c.committed_bytes = g_committed_end - g_base.load(std::memory_order_relaxed);
    c.bump_bytes = g_bump - g_base.load(std::memory_order_relaxed);
    c.live_blocks = g_live.size();
    c.live_bytes = g_live_bytes;
    c.quarantined_blocks = g_quarantine.size();
    for (const QuarantinedBlock& q : g_quarantine) {
        c.quarantined_bytes += q.size;
    }
    c.released = g_released;
    c.allocs_total = g_allocs_total;
    c.frees_total = g_frees_total;
    c.ring_expired = g_ring_expired;
    c.alloc_failures = g_alloc_failures;
    c.unknown_frees = g_unknown_frees;
    c.frame = g_frame;
    return c;
}

void Report(std::string& out) {
    const Counters c = GetCounters();
    uint64_t allocs_window = 0;
    uint64_t frees_window = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        allocs_window = g_allocs_window;
        frees_window = g_frees_window;
        g_allocs_window = 0;
        g_frees_window = 0;
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "GPU region: reserved=%" PRIu64 " committed=%" PRIu64 " bump=%" PRIu64 " frame=%" PRIu64 "\n",
                  c.reserved_bytes, c.committed_bytes, c.bump_bytes, c.frame);
    out += line;
    std::snprintf(line, sizeof(line),
                  "  live blocks=%" PRIu64 " bytes=%" PRIu64 " quarantined=%" PRIu64 " bytes=%" PRIu64
                  " released=%" PRIu64 "\n",
                  c.live_blocks, c.live_bytes, c.quarantined_blocks, c.quarantined_bytes, c.released);
    out += line;
    std::snprintf(line, sizeof(line),
                  "  allocs window=%" PRIu64 " frees window=%" PRIu64 " allocs total=%" PRIu64
                  " frees total=%" PRIu64 "\n",
                  allocs_window, frees_window, c.allocs_total, c.frees_total);
    out += line;
    std::snprintf(line, sizeof(line),
                  "  ring expiries=%" PRIu64 " alloc failures=%" PRIu64 " unknown frees=%" PRIu64 "\n",
                  c.ring_expired, c.alloc_failures, c.unknown_frees);
    out += line;
}

void SetHostForTest(const Host& host) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_host = host;
}

void ResetForTest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_host = Host{&runtime_memory_reserve_range_export, &runtime_memory_batch_map_export,
                  &runtime_memory_port_flexible_export};
    g_base.store(0, std::memory_order_release);
    g_limit.store(0, std::memory_order_relaxed);
    g_inited = false;
    g_reserved = 0;
    g_committed_end = 0;
    g_bump = 0;
    g_frame = 0;
    g_ring_frames = kDefaultRingFrames;
    g_quarantine_frames = kDefaultQuarantineFrames;
    for (int i = 0; i < kClassCount; ++i) {
        g_class_free[i].clear();
        g_slab_next[i] = 0;
        g_slab_end[i] = 0;
    }
    g_free_runs.clear();
    g_live.clear();
    g_ring_fifo.clear();
    g_quarantine.clear();
    g_release_fn = nullptr;
    g_release_ctx = nullptr;
    g_live_bytes = 0;
    g_released = 0;
    g_allocs_total = 0;
    g_frees_total = 0;
    g_allocs_window = 0;
    g_frees_window = 0;
    g_ring_expired = 0;
    g_alloc_failures = 0;
    g_unknown_frees = 0;
}

}  // namespace SotcGpuRegion
