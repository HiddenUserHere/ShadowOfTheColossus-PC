// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A2 GPU memory mirror (SOTC_MIRROR, ARCH-DRAW.md part 1; contract in sotc_arch.h). One sparse VkBuffer covers
// the GPU-visible guest range from kWindowBase: its offset o holds guest address base + o. Residency follows GPU demand:
// a 64 KiB page gets device memory and its guest bytes only when Touch (or a shader-side Find, through the touched
// flags) asks for it. Upload never binds: it copies announced bytes into the pages that are already resident and skips
// the rest. Map and unmap events arrive from any thread and are only queued; the draw thread processes them at submit
// start (ReadyForSubmit) and in Upload / Touch.
// The page tables are bitsets (one bit per page or 16 KiB sub-page) and lazily allocated slot chunks, so a window of
// the whole guest range costs a few MiB and no more.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_set>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::Mirror {
namespace {

// The guest range the GPU can reach (src/runtime_memory.c). Guest mappings are host pointers too (the ranges are
// reserved at their own host addresses), so a copy reads guest address a at host pointer a.
// GPU-visible mappings come from two places: find_free places them in [USER_MIN, USER_MAX), and MAP_FIXED may place
// them in the system-managed window [SYSTEM_LOW_MIN, SYSTEM_LOW_MAX) (Shadow of the Colossus maps 0x7f0000000 for the
// GPU: "mapped direct memory 00000007f0000000"). The window starts at SYSTEM_LOW_MIN and runs to USER_MAX, so it
// covers both. The hole [SYSTEM_LOW_MAX, USER_MIN) is host-owned memory (runtime_low_map) that no GPU mapping uses.
// If these constants change in src/runtime_memory.c, change them here.
constexpr VAddr kSystemLowMin = 0x0700000000ULL; // SYSTEM_LOW_MIN
constexpr VAddr kUserMax = 0xfc00000000ULL;      // USER_MAX
constexpr VAddr kWindowBase = kSystemLowMin;
constexpr u64 kWindowFull = kUserMax - kWindowBase; // the whole GPU-visible range: 982 GiB
// Guest mappings are made in PAGE (16 KiB) units (src/runtime_memory.c).
constexpr u64 kSubPage = 16ULL * 1024;
constexpr u64 kSubPerPage = PageSize / kSubPage;
constexpr u64 kChunkSize = 64ULL << 20; // device memory per VkDeviceMemory: 1024 pages
constexpr u32 kPagesPerChunk = u32(kChunkSize / PageSize);
constexpr u64 kVramMargin = 3ULL << 29; // 1.5 GiB kept free below the device-local heap size
constexpr u64 kStreamSize = 64ULL << 20;
constexpr u64 kUploadChunk = 8ULL << 20;
constexpr u64 kProbeSize = 64ULL << 10; // the first bytes of the first resident run: sample and =2 readback
constexpr u32 kVerifySlots = 8;
constexpr u64 kVerifyLag = 3;  // ticks to wait before the =2 readback compare
constexpr u32 kNoSlot = ~u32{0};
// Slot table chunks: 4096 pages (256 MiB of window) each, allocated when the first page of the chunk is bound.
constexpr u64 kBoundChunkPages = 4096;
// Window sizes tried, largest first: the whole GPU-visible range, then powers of two down to 4 GiB. The first one the
// device accepts is the window. A size above sparseAddressSpaceSize or maxBufferSize is not tried.
constexpr u64 kGiB = 1ULL << 30;
// sotc: 64 GiB from kWindowBase covers what the game maps (0x7f0000000 to about 0x1410000000 in the logs) with room,
// and leaves the device's sparse address space (about 1 TiB) to the buffer cache's own sparse buffers. A sparse
// buffer only reserves addresses: memory is bound per touched page. Maps outside count in the coverage line.
constexpr u64 kWindowSizes[] = {64 * kGiB, 32 * kGiB, 16 * kGiB, 8 * kGiB, 4 * kGiB};

int Mode() {
    static const int mode = SwitchMode("SOTC_MIRROR");
    return mode;
}

struct Op {
    VAddr addr;
    u64 size;
    bool map;
};

struct Verify {
    VAddr addr;
    u64 size;
    u32 slot;
    u64 tick;
    std::vector<u8> expect;
};

struct Counters {
    std::atomic<u64> bind_calls{};
    std::atomic<u64> pages_bound{};   // pages, not bind records
    std::atomic<u64> pages_unbound{}; // pages, not unbind records
    std::atomic<u64> pages_filled{};  // pages whose 64 KiB were copied from guest memory (Touch)
    std::atomic<u64> upload_bytes{};
    std::atomic<u64> upload_ranges{};
    std::atomic<u64> touches{};
    std::atomic<u64> touch_failed{};
    std::atomic<u64> refused_window{};
    std::atomic<u64> refused_vram{};
    std::atomic<u64> refused_alloc{};
    std::atomic<u64> nonresident_bytes{};
    std::atomic<u64> verify_checked{};
    std::atomic<u64> verify_mismatch{};
    std::atomic<u64> verify_skipped{};
    std::atomic<u64> gpu_sync_from_calls{};  // SyncFromBuffer: GPU writes copied from a buffer into the mirror
    std::atomic<u64> gpu_sync_from_bytes{};
    std::atomic<u64> gpu_sync_to_calls{};    // SyncToBuffer: addressed writes copied from the mirror into a buffer
    std::atomic<u64> gpu_sync_to_bytes{};
    std::atomic<u64> gpu_sync_refused{};     // sync bytes that could not be kept coherent (no slot, outside the window)
    std::atomic<u64> alias_binds{};          // alias bind records queued (AliasBind calls)
    std::atomic<u64> alias_pages_bound{};    // pages aliased (AliasBind)
    std::atomic<u64> alias_unbinds{};        // alias unbind records queued (AliasUnbind calls)
    std::atomic<u64> alias_pages_unbound{}; // pages whose alias was removed (AliasUnbind)
    std::atomic<u64> alias_calls{};          // sparse bind calls carrying alias ops (TakeAliasCalls)
    std::atomic<u64> alias_refused{};        // AliasBind calls refused (outside, owned page, memory type, shape)
    std::atomic<u64> touch_held{};           // Touch requests that asked the buffer cache to place the range
    std::atomic<u64> touch_aliased{};        // Touch requests served wholly by aliased pages
    std::atomic<u64> touch_own{};            // Touch requests served by own pages (the buffer cache could not hold them)
    std::atomic<u64> upload_aliased_bytes{}; // Upload bytes skipped: the buffer cache uploads aliased pages
    std::atomic<u64> sync_aliased_bytes{};   // sync bytes skipped: the write went to the same memory already
};

// One queued alias op, in the order of the buffer cache's bind calls. memory null: an unbind.
struct AliasOp {
    u64 first_page;
    u64 pages;
    vk::SparseMemoryBind bind; // resourceOffset and size: the window's bytes; memory and memoryOffset: the arena's
};

// Coverage tallies for the whole run: never reset by Report, so the last report of a run states the totals.
// Outside = the range had a byte outside the window [kWindowBase, base + window_size).
struct Coverage {
    std::atomic<u64> maps{};
    std::atomic<u64> maps_outside{};
    std::atomic<u64> map_low{~u64{0}}; // lowest start of any guest map event
    std::atomic<u64> map_high{};       // highest end of any guest map event
    std::atomic<u64> touches{};
    std::atomic<u64> touches_outside{};
    std::atomic<u64> finds{};
    std::atomic<u64> finds_outside{};
    std::atomic<u64> uploads{};
    std::atomic<u64> uploads_outside{};
};

// Atomic bit table: one bit per page or sub-page. Words are allocated by Reset (zero); bits are set and read from any
// thread. Every bit in use is one of the window's pages, so a window of the whole GPU range costs about 1 bit per 64 KiB.
class BitTable {
public:
    void Reset(u64 bits) {
        words_count_ = (bits + 63) / 64;
        words_ = std::make_unique<std::atomic<u64>[]>(words_count_); // value-initialized: zero
    }
    u64 Words() const { return words_count_; }
    std::atomic<u64>& Word(u64 w) { return words_[w]; }
    bool Test(u64 i) const { return ((words_[i >> 6].load(std::memory_order_acquire) >> (i & 63)) & 1) != 0; }
    void Set(u64 i) { words_[i >> 6].fetch_or(u64{1} << (i & 63), std::memory_order_acq_rel); }
    void Clear(u64 i) { words_[i >> 6].fetch_and(~(u64{1} << (i & 63)), std::memory_order_acq_rel); }
    // Sets (value true) or clears the bits [first, first + count), a word at a time.
    void SetRange(u64 first, u64 count, bool value) {
        const u64 end = first + count;
        for (u64 i = first; i < end;) {
            const u64 lo = i & 63;
            const u64 span = std::min<u64>(64 - lo, end - i);
            const u64 mask = (span == 64 ? ~u64{0} : (u64{1} << span) - 1) << lo;
            if (value) {
                words_[i >> 6].fetch_or(mask, std::memory_order_acq_rel);
            } else {
                words_[i >> 6].fetch_and(~mask, std::memory_order_acq_rel);
            }
            i += span;
        }
    }

private:
    std::unique_ptr<std::atomic<u64>[]> words_;
    u64 words_count_{};
};

void RaiseTo(std::atomic<u64>& a, u64 v) {
    u64 cur = a.load(std::memory_order_relaxed);
    while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void LowerTo(std::atomic<u64>& a, u64 v) {
    u64 cur = a.load(std::memory_order_relaxed);
    while (cur > v && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

struct State {
    vk::Device device;
    vk::Queue queue; // queue index 0 of the sparse family: the scheduler's graphics queue
    vk::UniqueBuffer buffer;
    vk::DeviceAddress device_base{};
    VAddr base = kWindowBase;
    u64 window_size{};
    u64 pages{};
    std::vector<Window> windows;
    // Per page: resident = bound, filled and usable; touched = a shader-side Find asked for it.
    BitTable resident;
    // Per page: resident through an alias (the buffer cache's arena memory, AliasBind), not an own slot. Such a page is
    // never bound or filled by the mirror: its bytes are the arena's.
    BitTable aliased;
    u32 alias_type_bits{};         // memory types the window can bind (memoryTypeBits of the window)
    BitTable touched;
    // Per 64 touched pages (one word of touched): set when that word holds a touch. ProcessPending reads only these.
    BitTable touched_words;
    // Per 16 KiB sub-page: guest-mapped. A page is mappable when all its sub-pages are mapped.
    BitTable mapped_sub;
    // Physical slot per page, in chunks of kBoundChunkPages allocated on first bind (kNoSlot when none).
    std::vector<std::unique_ptr<u32[]>> bound_chunks;
    BitTable retiring;              // page is on retire_list
    std::vector<u32> retire_list;   // unmapped pages whose binding waits for the frame fence
    u64 retire_max{};               // newest tick any unmap of retire_list happened at
    std::vector<vk::UniqueDeviceMemory> chunks;
    std::vector<u32> free_slots;    // slots that no page is bound to (unique: see EnsureBound)
    u64 budget{};
    u32 memory_type{};
    vk::UniqueSemaphore timeline;
    u64 bind_tick{};
    std::unique_ptr<VideoCore::StreamBuffer> stream;
    std::unique_ptr<VideoCore::Buffer> readback; // =2 only
    std::vector<Verify> verify;
    std::vector<u32> verify_free;
};

// The state lives until the process exits: the device is destroyed after the static destructors would run,
// so it is never freed (there is no Shutdown in the contract).
State* state{};
std::atomic<bool> enabled{false};
std::atomic<bool> failed{false};
std::atomic<bool> touched_pending{false};
std::atomic<u64> reserved_bytes{};
std::atomic<u64> bound_pages{}; // pages with a slot: incremented per page in EnsureBound, decremented per page in RetirePages
std::mutex pending_mutex;
std::vector<Op> pending;
// Draw thread work (ReadyForSubmit, Upload, Touch). Recursive: an Upload may flush, which calls ReadyForSubmit.
// Lock order: process_mutex, then alias_mutex. AliasBind and AliasUnbind take alias_mutex only: the buffer cache
// calls them from its own paths (placement, release, eviction), on any thread.
std::recursive_mutex process_mutex;
std::mutex alias_mutex;
std::vector<AliasOp> alias_queue;           // guarded by alias_mutex
std::atomic<u64> aliased_pages{};           // pages with the aliased bit (guarded writes under alias_mutex)
std::atomic<bool> alias_queued{false};
// The buffer cache hook that places a range in an arena (Touch). Set before any Touch; read on the draw thread.
ArenaHold hold_fn{};
void* hold_ctx{};
Counters counters;
Coverage coverage;

// Set while the draw thread is inside Upload or Touch. A flush there (stream wait) calls ReadyForSubmit, which must
// not run the pending work: it would re-enter the copy loops and change residency under them. The work runs at the
// next submit instead.
thread_local bool t_busy = false;
struct BusyScope {
    BusyScope() { t_busy = true; }
    ~BusyScope() { t_busy = false; }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
};

u32 DeviceLocalType(const vk::PhysicalDeviceMemoryProperties& props, u32 bits) {
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if (((bits >> i) & 1) != 0 &&
            (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    return kNoSlot;
}

u64 DeviceLocalBytes(const vk::PhysicalDeviceMemoryProperties& props) {
    u64 best = 0;
    for (u32 i = 0; i < props.memoryHeapCount; ++i) {
        if (props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            best = std::max<u64>(best, props.memoryHeaps[i].size);
        }
    }
    return best;
}

void Disable(const char* why) {
    LOG_WARNING(Render, "SOTC_MIRROR off: {}", why);
    failed.store(true, std::memory_order_release);
    std::scoped_lock lk{pending_mutex};
    pending.clear();
}

// Clips [addr, addr+size) to the window. False when nothing of the range is inside.
bool Clip(const State& s, VAddr addr, u64 size, u64& lo, u64& hi) {
    const VAddr end = s.base + s.window_size;
    if (size == 0 || addr >= end || addr + size <= s.base) {
        return false;
    }
    lo = std::max<VAddr>(addr, s.base);
    hi = std::min<VAddr>(addr + size, end);
    return true;
}

bool PageMappable(const State& s, u64 page) {
    const u64 first = page * kSubPerPage;
    for (u64 i = 0; i < kSubPerPage; ++i) {
        if (!s.mapped_sub.Test(first + i)) {
            return false;
        }
    }
    return true;
}

// Physical slot of a page (kNoSlot when none). Chunks that were never allocated hold no slot.
u32 BoundSlot(const State& s, u64 page) {
    const auto& chunk = s.bound_chunks[page / kBoundChunkPages];
    return chunk ? chunk[page % kBoundChunkPages] : kNoSlot;
}

void SetBoundSlot(State& s, u64 page, u32 slot) {
    auto& chunk = s.bound_chunks[page / kBoundChunkPages];
    if (!chunk) {
        chunk = std::make_unique<u32[]>(kBoundChunkPages);
        std::fill_n(chunk.get(), kBoundChunkPages, kNoSlot);
    }
    chunk[page % kBoundChunkPages] = slot;
}

// Pages covered by a list of bind or unbind records. Records merge contiguous pages, so records are not pages.
u64 PagesIn(const std::vector<vk::SparseMemoryBind>& binds) {
    u64 pages = 0;
    for (const vk::SparseMemoryBind& bind : binds) {
        pages += bind.size / PageSize;
    }
    return pages;
}

// Adds the sparse bind of one page. Contiguous pages of one chunk are merged into one record.
void AddBind(State& s, std::vector<vk::SparseMemoryBind>& binds, u64 page, u32 slot) {
    const vk::DeviceMemory memory = *s.chunks[slot / kPagesPerChunk];
    const u64 memory_offset = u64(slot % kPagesPerChunk) * PageSize;
    const u64 resource_offset = page * PageSize;
    if (!binds.empty()) {
        vk::SparseMemoryBind& last = binds.back();
        if (last.memory == memory && last.resourceOffset + last.size == resource_offset &&
            last.memoryOffset + last.size == memory_offset) {
            last.size += PageSize;
            return;
        }
    }
    binds.push_back(vk::SparseMemoryBind{
        .resourceOffset = resource_offset,
        .size = PageSize,
        .memory = memory,
        .memoryOffset = memory_offset,
    });
}

void AddUnbind(std::vector<vk::SparseMemoryBind>& binds, u64 page) {
    const u64 resource_offset = page * PageSize;
    if (!binds.empty()) {
        vk::SparseMemoryBind& last = binds.back();
        if (!last.memory && last.resourceOffset + last.size == resource_offset) {
            last.size += PageSize;
            return;
        }
    }
    binds.push_back(vk::SparseMemoryBind{
        .resourceOffset = resource_offset,
        .size = PageSize,
    });
}

// One vkQueueBindSparse call. signal != 0 signals the timeline (binds); 0 signals nothing (unbinds).
// The scheduler's queue lock (submit_mutex) is held, as buffer_cache.cpp does for its sparse binds.
void SubmitSparse(State& s, const std::vector<vk::SparseMemoryBind>& binds, u64 signal) {
    const vk::SparseBufferMemoryBindInfo info = {
        .buffer = *s.buffer,
        .bindCount = u32(binds.size()),
        .pBinds = binds.data(),
    };
    vk::BindSparseInfo bind_info = {
        .bufferBindCount = 1,
        .pBufferBinds = &info,
    };
    vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal,
    };
    vk::Semaphore sem = *s.timeline;
    if (signal != 0) {
        bind_info.pNext = &timeline_info;
        bind_info.signalSemaphoreCount = 1;
        bind_info.pSignalSemaphores = &sem;
    }
    std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
    const vk::Result result = s.queue.bindSparse(bind_info);
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during a mirror bind");
    ASSERT_MSG(result == vk::Result::eSuccess, "Mirror bind failed: {}", vk::to_string(result));
}

// Gives a page a physical slot and adds its bind to binds. A page that is still bound keeps its slot (its unbind
// waits for the frame fence), so it needs no bind. False when no slot can be had (budget or allocation refused).
// Slots are unique: a slot is in free_slots or bound to exactly one page, never both; bound_pages counts pages.
bool EnsureBound(State& s, u64 page, std::vector<vk::SparseMemoryBind>& binds) {
    if (BoundSlot(s, page) != kNoSlot) {
        return true;
    }
    if (s.free_slots.empty()) {
        const u64 reserved = u64(s.chunks.size() + 1) * kChunkSize;
        if (reserved > s.budget) {
            counters.refused_vram.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto [result, memory] = s.device.allocateMemoryUnique(vk::MemoryAllocateInfo{
            .allocationSize = kChunkSize,
            .memoryTypeIndex = s.memory_type,
        });
        if (result != vk::Result::eSuccess) {
            counters.refused_alloc.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        s.chunks.push_back(std::move(memory));
        reserved_bytes.store(reserved, std::memory_order_relaxed);
        const u32 first = u32(s.chunks.size() - 1) * kPagesPerChunk;
        for (u32 i = kPagesPerChunk; i-- > 0;) {
            s.free_slots.push_back(first + i);
        }
    }
    const u32 slot = s.free_slots.back();
    s.free_slots.pop_back();
    SetBoundSlot(s, page, slot);
    bound_pages.fetch_add(1, std::memory_order_relaxed);
    AddBind(s, binds, page, slot);
    return true;
}

// Submits the binds collected by EnsureBound and registers the wait for the next graphics submit. Returns the
// timeline value of the bind (0 when there was nothing to bind).
u64 SubmitBinds(State& s, Vulkan::Scheduler& scheduler, const std::vector<vk::SparseMemoryBind>& binds) {
    if (binds.empty()) {
        return 0;
    }
    const u64 value = ++s.bind_tick;
    SubmitSparse(s, binds, value);
    counters.bind_calls.fetch_add(1, std::memory_order_relaxed);
    counters.pages_bound.fetch_add(PagesIn(binds), std::memory_order_relaxed);
    scheduler.RequireExternalWait(*s.timeline, value, vk::PipelineStageFlagBits::eAllCommands);
    return value;
}

// Unbinds the pages whose frame fence is reached: the newest unmap tick must be done on the GPU.
void RetirePages(State& s, Vulkan::Scheduler& scheduler) {
    if (s.retire_list.empty() || !scheduler.IsFree(s.retire_max)) {
        return;
    }
    std::vector<vk::SparseMemoryBind> unbinds;
    for (const u32 page : s.retire_list) {
        s.retiring.Clear(page);
        const u32 slot = BoundSlot(s, page);
        if (s.resident.Test(page) || slot == kNoSlot) {
            continue;
        }
        AddUnbind(unbinds, page);
        s.free_slots.push_back(slot);
        SetBoundSlot(s, page, kNoSlot);
    }
    s.retire_list.clear();
    s.retire_max = 0;
    if (!unbinds.empty()) {
        const u64 pages = PagesIn(unbinds);
        SubmitSparse(s, unbinds, 0);
        counters.pages_unbound.fetch_add(pages, std::memory_order_relaxed);
        bound_pages.fetch_sub(pages, std::memory_order_relaxed);
    }
}

// Applies the queued guest map and unmap events, in order.
void ApplyOps(State& s, Vulkan::Scheduler& scheduler, const std::vector<Op>& ops) {
    const VAddr end = s.base + s.window_size;
    for (const Op& op : ops) {
        if (op.map) {
            // Coverage: every guest map event, inside the window or not (a map outside it is not GPU-readable here).
            coverage.maps.fetch_add(1, std::memory_order_relaxed);
            RaiseTo(coverage.map_high, op.addr + op.size);
            LowerTo(coverage.map_low, op.addr);
            if (op.addr < s.base || op.addr + op.size > end) {
                coverage.maps_outside.fetch_add(1, std::memory_order_relaxed);
            }
        }
        u64 lo{};
        u64 hi{};
        if (!Clip(s, op.addr, op.size, lo, hi)) {
            if (op.map) {
                counters.refused_window.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (op.map && (op.addr < s.base || op.addr + op.size > end)) {
            counters.refused_window.fetch_add(1, std::memory_order_relaxed);
        }
        const u64 sub_first = (lo - s.base) / kSubPage;
        const u64 sub_last = (hi - s.base + kSubPage - 1) / kSubPage;
        s.mapped_sub.SetRange(sub_first, sub_last - sub_first, op.map);
        if (op.map) {
            continue; // mapping alone binds nothing: pages are bound when they are touched
        }
        // Unmapped pages stop being resident now; their binding is released after the frame fence.
        const u64 page_first = (lo - s.base) / PageSize;
        const u64 page_last = (hi - s.base - 1) / PageSize;
        for (u64 page = page_first; page <= page_last; ++page) {
            // Only pages fully covered are released; a partly covered page stays bound (16 KiB guest
            // granularity: its other sub-pages may still be mapped).
            const VAddr page_begin = s.base + page * PageSize;
            if (page_begin < lo || page_begin + PageSize > hi) {
                continue;
            }
            if (!s.resident.Test(page)) {
                continue;
            }
            if (s.aliased.Test(page)) {
                continue; // sotc: an aliased page leaves with its arena bind (AliasUnbind), not here
            }
            s.resident.Clear(page);
            if (!s.retiring.Test(page)) {
                s.retiring.Set(page);
                s.retire_list.push_back(u32(page));
            }
            s.retire_max = std::max<u64>(s.retire_max, scheduler.CurrentTick());
        }
    }
}

// Makes the listed pages resident. A page that is not bound gets a slot; all the binds of the call go out as one
// sparse submit. Every page that is not resident is then filled with its 64 KiB of guest memory, and the copies are
// recorded in the upload command buffer, which runs in the submit that waits for the binds. Pages that are not
// mappable (some 16 KiB sub-page is unmapped) are left alone. Returns how many listed pages are not resident after.
u64 MakeResident(State& s, Vulkan::Scheduler& scheduler, const std::vector<u64>& pages) {
    std::vector<vk::SparseMemoryBind> binds;
    std::vector<u64> fill;
    u64 missing = 0;
    {
        // sotc: an alias bind of one of these pages (another thread, the buffer cache) must not interleave with the check.
        std::scoped_lock alias_lk{alias_mutex};
        for (const u64 page : pages) {
            if (s.resident.Test(page)) {
                continue; // own or aliased: already the bytes of the mirror
            }
            if (!PageMappable(s, page) || !EnsureBound(s, page, binds)) {
                ++missing;
                continue;
            }
            fill.push_back(page); // bound now, or bound earlier and not filled since its unmap (stale bytes)
        }
    }
    if (fill.empty()) {
        return missing;
    }
    const u64 value = SubmitBinds(s, scheduler, binds);
    // The copies below run after the bind wait of this submit. A stream wait can flush the open command buffer, and
    // then the wait went with that submit: it is registered again for the submit that carries the copies after it.
    u64 seen_tick = scheduler.CurrentTick();
    bool recorded = false;
    for (const u64 page : fill) {
        auto [dst, src_offset] = s.stream->Map(PageSize);
        if (dst == nullptr) {
            counters.refused_alloc.fetch_add(1, std::memory_order_relaxed);
            ++missing;
            continue;
        }
        if (value != 0 && scheduler.CurrentTick() != seen_tick) {
            scheduler.RequireExternalWait(*s.timeline, value, vk::PipelineStageFlagBits::eAllCommands);
            seen_tick = scheduler.CurrentTick();
        }
        // Through the backing view: a guest view may be mid-split (placeholder) on another thread (runtime_memory.c).
        Core::Memory::Instance()->CopySparseMemory(s.base + page * PageSize, static_cast<u8*>(dst), PageSize);
        s.stream->Commit();
        const vk::BufferCopy copy = {
            .srcOffset = src_offset,
            .dstOffset = page * PageSize,
            .size = PageSize,
        };
        scheduler.UploadCommandBuffer().copyBuffer(s.stream->Handle(), *s.buffer, copy);
        s.resident.Set(page);
        counters.pages_filled.fetch_add(1, std::memory_order_relaxed);
        recorded = true;
    }
    if (recorded) {
        // Copies -> shader reads (the same barrier Upload records after its copies).
        const vk::MemoryBarrier2 written = {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        scheduler.UploadCommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &written,
        });
    }
    return missing;
}

// The =2 compare: a readback of the probe bytes of an upload, taken kVerifyLag ticks later.
void VerifyPending(State& s, Vulkan::Scheduler& scheduler) {
    if (s.verify.empty()) {
        return;
    }
    const u64 now = scheduler.CurrentTick();
    for (auto it = s.verify.begin(); it != s.verify.end();) {
        if (now < it->tick + kVerifyLag || !scheduler.IsFree(it->tick)) {
            ++it;
            continue;
        }
        const u64 offset = u64(it->slot) * PageSize;
        s.readback->Invalidate(offset, it->size);
        const u8* got = s.readback->mapped_data.data() + offset;
        counters.verify_checked.fetch_add(1, std::memory_order_relaxed);
        if (std::memcmp(got, it->expect.data(), it->size) != 0) {
            counters.verify_mismatch.fetch_add(1, std::memory_order_relaxed);
            LOG_WARNING(Render, "SOTC_MIRROR=2: mismatch at {:#x} (+{:#x})", it->addr, it->size);
        }
        s.verify_free.push_back(it->slot);
        it = s.verify.erase(it);
    }
}

// Draw thread: retire, apply the queued map events, make resident the pages a shader asked for.
void ProcessPending(State& s, Vulkan::Scheduler& scheduler) {
    RetirePages(s, scheduler);
    std::vector<Op> ops;
    {
        std::scoped_lock lk{pending_mutex};
        ops.swap(pending);
    }
    ApplyOps(s, scheduler, ops);
    if (touched_pending.exchange(false, std::memory_order_acq_rel)) {
        // Two levels: touched_words names the words of touched that hold a touch, so the scan is per touch, not per page.
        // Find sets the touched bit before its touched_words bit, and this takes touched_words before the touched word:
        // a touch that lands between the two reads keeps its touched_words bit for the next submit.
        std::vector<u64> pages;
        for (u64 sw = 0; sw < s.touched_words.Words(); ++sw) {
            u64 summary = s.touched_words.Word(sw).exchange(0, std::memory_order_acq_rel);
            while (summary != 0) {
                const u64 w = sw * 64 + std::countr_zero(summary);
                summary &= summary - 1;
                u64 bits = s.touched.Word(w).exchange(0, std::memory_order_acq_rel);
                while (bits != 0) {
                    pages.push_back(w * 64 + std::countr_zero(bits));
                    bits &= bits - 1;
                }
            }
        }
        if (!pages.empty()) {
            MakeResident(s, scheduler, pages);
        }
    }
}

} // namespace

void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    if (Mode() == 0 || state != nullptr || failed.load()) {
        return;
    }
    if (!instance.SotcSparseBuffers() || !instance.SotcBufferDeviceAddress()) {
        Disable("the device has no sparse buffers or no buffer device address");
        return;
    }
    // sotc: A2b. The arenas and the window share memory: both need sparseResidencyAliased (enabled with the mirror).
    if (!instance.SotcSparseAliased()) {
        Disable("the device has no sparseResidencyAliased (the arena aliases need it)");
        return;
    }
    const u32 family = instance.SotcSparseQueueFamily();
    if (family == Vulkan::Instance::NoSparseQueueFamily) {
        Disable("no sparse-capable queue family");
        return;
    }

    auto s = std::make_unique<State>();
    s->device = instance.GetDevice();
    // Queue index 0 of the sparse family: the scheduler's graphics queue (same VkQueue, same lock).
    s->queue = instance.GetGraphicsQueue();

    // The limits that bound a sparse buffer. Maintenance4 (maxBufferSize) is core in Vulkan 1.3, the instance's target.
    const auto limit_props = instance.GetPhysicalDevice().getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMaintenance4Properties>();
    const u64 space_limit = limit_props.get<vk::PhysicalDeviceProperties2>().properties.limits.sparseAddressSpaceSize;
    const u64 buffer_limit = limit_props.get<vk::PhysicalDeviceMaintenance4Properties>().maxBufferSize;
    LOG_INFO(Render, "SOTC MIRROR limits: sparseAddressSpaceSize {} GiB, maxBufferSize {} GiB; GPU-visible guest range "
             "[{:#x}, {:#x}) = {} GiB",
             space_limit >> 30, buffer_limit >> 30, kWindowBase, kUserMax, kWindowFull >> 30);

    // Largest window first: the window must start at kWindowBase, so its size is the only choice that covers the range.
    for (const u64 size : kWindowSizes) {
        if ((space_limit != 0 && size > space_limit) || (buffer_limit != 0 && size > buffer_limit)) {
            LOG_INFO(Render, "SOTC MIRROR: window {} GiB not tried (above the device limit)", size >> 30);
            continue;
        }
        auto [created, buffer] = s->device.createBufferUnique(vk::BufferCreateInfo{
            .flags = vk::BufferCreateFlagBits::eSparseBinding |
                     vk::BufferCreateFlagBits::eSparseResidency |
                     vk::BufferCreateFlagBits::eSparseAliased, // sotc: A2b, the arenas bind the same memory
            .size = size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst |
                     vk::BufferUsageFlagBits::eTransferSrc |
                     vk::BufferUsageFlagBits::eShaderDeviceAddress,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        if (created == vk::Result::eSuccess) {
            s->buffer = std::move(buffer);
            s->window_size = size;
            break;
        }
        LOG_WARNING(Render, "SOTC MIRROR: window {} GiB refused by vkCreateBuffer: {}", size >> 30,
                    vk::to_string(created));
    }
    if (!s->window_size) {
        Disable("no window size could be created as a sparse buffer");
        return;
    }
    LOG_INFO(Render, "SOTC_MIRROR: window {:#x} at {:#x}: {} GiB, {} the GPU-visible range", s->window_size, s->base,
             s->window_size >> 30, s->window_size >= kWindowFull ? "covers all of" : "PARTIAL: does not cover");

    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();
    // (const reference: GetMemoryProperties returns one)
    const vk::MemoryRequirements req = s->device.getBufferMemoryRequirements(*s->buffer);
    if (req.alignment > PageSize) {
        Disable("sparse page alignment is larger than 64 KiB");
        return;
    }
    s->alias_type_bits = req.memoryTypeBits;
    s->memory_type = DeviceLocalType(props, req.memoryTypeBits);
    if (s->memory_type == kNoSlot) {
        Disable("no device-local memory type for the sparse pages");
        return;
    }
    const u64 local = DeviceLocalBytes(props);
    s->budget = local > kVramMargin ? local - kVramMargin : 0;
    s->device_base = s->device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *s->buffer});

    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [sem_result, sem] = s->device.createSemaphoreUnique(semaphore_chain.get());
    if (sem_result != vk::Result::eSuccess) {
        Disable("the mirror semaphore could not be created");
        return;
    }
    s->timeline = std::move(sem);

    // Page tables: bits per page and per sub-page (about 2.4 MiB per 64 GiB of window), and slot chunks that are
    // allocated only where a page is bound. The whole 982 GiB window needs about 14 MiB before any binding.
    s->pages = s->window_size / PageSize;
    const u64 subs = s->window_size / kSubPage;
    s->resident.Reset(s->pages);
    s->aliased.Reset(s->pages);
    s->touched.Reset(s->pages);
    s->touched_words.Reset((s->pages + 63) / 64);
    s->mapped_sub.Reset(subs);
    s->retiring.Reset(s->pages);
    s->bound_chunks.resize((s->pages + kBoundChunkPages - 1) / kBoundChunkPages);
    s->stream = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler,
                                                          VideoCore::MemoryType::HostUncached,
                                                          kStreamSize);
    if (Mode() == 2) {
        s->readback = std::make_unique<VideoCore::Buffer>(instance, VAddr{0},
                                                          u64{kVerifySlots} * PageSize,
                                                          VideoCore::MemoryType::HostCached,
                                                          "SotC mirror verify");
        for (u32 i = 0; i < kVerifySlots; ++i) {
            s->verify_free.push_back(i);
        }
    }
    s->windows.push_back(Window{
        .guest_base = s->base,
        .size = s->window_size,
        .buffer = *s->buffer,
        .device_base = s->device_base,
    });
    LOG_INFO(Render, "SOTC_MIRROR on: budget {} MiB, mode {}", s->budget >> 20, Mode());

    state = s.release();
    enabled.store(true, std::memory_order_release);
}

bool On() {
    return enabled.load(std::memory_order_acquire);
}

void OnMap(VAddr addr, u64 size) {
    if (size == 0 || Mode() == 0 || failed.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lk{pending_mutex};
    pending.push_back(Op{addr, size, true});
}

void OnUnmap(VAddr addr, u64 size) {
    if (size == 0 || Mode() == 0 || failed.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lk{pending_mutex};
    pending.push_back(Op{addr, size, false});
}

void ReadyForSubmit(Vulkan::Scheduler& scheduler) {
    if (!enabled.load(std::memory_order_acquire) || t_busy) {
        return;
    }
    // Called from every submit, possibly from the presenter thread: never wait for the draw thread here.
    std::unique_lock lk{process_mutex, std::try_to_lock};
    if (!lk.owns_lock()) {
        return;
    }
    ProcessPending(*state, scheduler);
    VerifyPending(*state, scheduler);
}

const Window* Find(VAddr addr, u64 size) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return nullptr;
    }
    State& s = *state;
    coverage.finds.fetch_add(1, std::memory_order_relaxed);
    if (addr < s.base || addr + size > s.base + s.window_size || addr + size < addr) {
        coverage.finds_outside.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const u64 page_first = (addr - s.base) / PageSize;
    const u64 page_last = (addr + size - 1 - s.base) / PageSize;
    bool all = true;
    for (u64 page = page_first; page <= page_last; ++page) {
        if (s.resident.Test(page)) {
            continue;
        }
        all = false;
        if (PageMappable(s, page)) {
            // Asked for by a shader: made resident at the next submit start (ReadyForSubmit). The word bit is set
            // after the page bit (ProcessPending relies on that order).
            s.touched.Set(page);
            s.touched_words.Set(page / 64);
            touched_pending.store(true, std::memory_order_release);
        }
    }
    return all ? &s.windows[0] : nullptr;
}

vk::DeviceAddress Translate(VAddr addr) {
    if (Find(addr, 1) == nullptr) {
        return 0;
    }
    return state->device_base + (addr - state->base);
}

std::span<const Window> Windows() {
    if (!enabled.load(std::memory_order_acquire)) {
        return {};
    }
    return state->windows;
}

bool Touch(Vulkan::Scheduler& scheduler, VAddr addr, u64 size) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return false;
    }
    std::scoped_lock lk{process_mutex};
    BusyScope busy;
    State& s = *state;
    counters.touches.fetch_add(1, std::memory_order_relaxed);
    coverage.touches.fetch_add(1, std::memory_order_relaxed);
    ProcessPending(s, scheduler); // map and unmap events first: mappability is decided by them
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        coverage.touches_outside.fetch_add(1, std::memory_order_relaxed);
        counters.touch_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (lo != addr || hi != addr + size) {
        coverage.touches_outside.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 first_page = (lo - s.base) / PageSize;
    const u64 last_page = (hi - s.base - 1) / PageSize;
    const bool whole_range = lo == addr && hi == addr + size;
    // sotc: A2b. The pages already resident are the bytes of the mirror: an arena's (aliased) or an own page.
    const auto all_resident = [&s, first_page, last_page]() {
        for (u64 page = first_page; page <= last_page; ++page) {
            if (!s.resident.Test(page)) {
                return false;
            }
        }
        return true;
    };
    // Counts a Touch that is served whole: aliased when every page is an alias, own when one is an own page.
    const auto count_served = [&s, first_page, last_page]() {
        bool any_own = false;
        for (u64 page = first_page; page <= last_page && !any_own; ++page) {
            any_own = !s.aliased.Test(page);
        }
        (any_own ? counters.touch_own : counters.touch_aliased).fetch_add(1, std::memory_order_relaxed);
    };
    if (whole_range && all_resident()) {
        count_served();
        return true;
    }
    // The buffer cache places the range in an arena when it holds it (ObtainBuffer: the placement binds the memory,
    // AliasBind aliases each new block, and the upload fills it). A range it cannot hold (ring-8 host runs, a window
    // of the arena that is refused) stays with the own pages below.
    if (hold_fn != nullptr && !all_resident()) {
        counters.touch_held.fetch_add(1, std::memory_order_relaxed);
        hold_fn(hold_ctx, lo, hi - lo);
    }
    std::vector<u64> pages;
    for (u64 page = first_page; page <= last_page; ++page) {
        if (!s.resident.Test(page)) {
            pages.push_back(page);
        }
    }
    const u64 missing = pages.empty() ? 0 : MakeResident(s, scheduler, pages);
    const bool whole = whole_range && missing == 0 && all_resident();
    if (whole) {
        count_served(); // the buffer cache's placement (aliased) or the own fill of the pages it could not hold
    } else {
        counters.touch_failed.fetch_add(1, std::memory_order_relaxed);
    }
    return whole;
}

bool Upload(Vulkan::Scheduler& scheduler, VAddr addr, u64 size, std::span<u8> sample) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return false;
    }
    std::scoped_lock lk{process_mutex};
    BusyScope busy;
    State& s = *state;
    coverage.uploads.fetch_add(1, std::memory_order_relaxed);
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        coverage.uploads_outside.fetch_add(1, std::memory_order_relaxed);
        counters.refused_window.fetch_add(1, std::memory_order_relaxed);
        return false; // counted no-op: the range is outside every window
    }
    if (lo != addr || hi != addr + size) {
        coverage.uploads_outside.fetch_add(1, std::memory_order_relaxed);
        counters.refused_window.fetch_add(1, std::memory_order_relaxed);
    }
    ProcessPending(s, scheduler);

    // Runs of resident bytes of [lo, hi). Upload never binds: a byte of a page that is not resident is skipped.
    const u64 page_first = (lo - s.base) / PageSize;
    const u64 page_last = (hi - s.base - 1) / PageSize;
    std::vector<std::pair<VAddr, u64>> runs;
    for (u64 page = page_first; page <= page_last; ++page) {
        const VAddr page_begin = s.base + page * PageSize;
        const VAddr a = std::max<VAddr>(lo, page_begin);
        const VAddr b = std::min<VAddr>(hi, page_begin + PageSize);
        if (!s.resident.Test(page)) {
            counters.nonresident_bytes.fetch_add(b - a, std::memory_order_relaxed);
            continue;
        }
        if (s.aliased.Test(page)) {
            // sotc: A2b. The arena of this page is uploaded by the buffer cache: the same memory, no copy here.
            counters.upload_aliased_bytes.fetch_add(b - a, std::memory_order_relaxed);
            continue;
        }
        if (!runs.empty() && runs.back().first + runs.back().second == a) {
            runs.back().second += b - a;
        } else {
            runs.emplace_back(a, b - a);
        }
    }
    if (runs.empty()) {
        return false;
    }

    // The probe is the first 64 KiB of the first resident run, as the copy reads it (the bytes staged for the GPU,
    // not a second guest read). It serves the caller's sample and the =2 readback compare. The sample is only
    // taken when it starts at addr and the run covers it, so a sampled range is resident.
    const VAddr probe_addr = runs.front().first;
    const u64 probe_size = std::min<u64>(runs.front().second, kProbeSize);
    const bool sample_ok = !sample.empty() && sample.size() <= kProbeSize && sample.size() <= size &&
                           probe_addr == addr && runs.front().second >= sample.size();
    const bool verify_wanted = Mode() == 2 && s.readback && !s.verify_free.empty();
    const bool need_probe = sample_ok || verify_wanted;
    std::vector<u8> probe;
    if (need_probe) {
        probe.resize(probe_size);
    }
    bool probed = false;

    // The guest bytes are read now, synchronously; the copies go into the scheduler's upload command buffer, which
    // is submitted before the commands of this submit. The command buffer is fetched per copy: a flush inside Map
    // ends the one recorded so far. No bind is submitted here, so no bind wait can be separated from these copies.
    u64 copied = 0;
    for (const auto& [run_addr, run_size] : runs) {
        for (u64 off = 0; off < run_size;) {
            const u64 n = std::min(kUploadChunk, run_size - off);
            auto [dst, src_offset] = s.stream->Map(n);
            if (dst == nullptr) {
                counters.refused_alloc.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            Core::Memory::Instance()->CopySparseMemory(run_addr + off, static_cast<u8*>(dst), n);
            if (need_probe && !probed && run_addr + off == probe_addr) {
                std::memcpy(probe.data(), dst, probe_size); // n >= probe_size: the chunk starts the probe run
                probed = true;
            }
            s.stream->Commit();
            const vk::BufferCopy copy = {
                .srcOffset = src_offset,
                .dstOffset = run_addr + off - s.base,
                .size = n,
            };
            scheduler.UploadCommandBuffer().copyBuffer(s.stream->Handle(), *s.buffer, copy);
            off += n;
            copied += n;
        }
    }
    counters.upload_bytes.fetch_add(copied, std::memory_order_relaxed);
    counters.upload_ranges.fetch_add(runs.size(), std::memory_order_relaxed);
    if (copied == 0) {
        return false;
    }

    // Copies -> shader reads and, for =2, the readback copy.
    const vk::MemoryBarrier2 written = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    scheduler.UploadCommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &written,
    });

    const bool sampled = sample_ok && probed;
    if (sampled) {
        std::memcpy(sample.data(), probe.data(), sample.size());
    }
    if (Mode() == 2) {
        if (verify_wanted && probed) {
            const u32 slot = s.verify_free.back();
            s.verify_free.pop_back();
            const vk::BufferCopy back = {
                .srcOffset = probe_addr - s.base,
                .dstOffset = u64{slot} * PageSize,
                .size = probe_size,
            };
            scheduler.UploadCommandBuffer().copyBuffer(*s.buffer, s.readback->Handle(), back);
            const vk::MemoryBarrier2 to_host = {
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            scheduler.UploadCommandBuffer().pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &to_host,
            });
            s.verify.push_back(Verify{probe_addr, probe_size, slot, scheduler.CurrentTick(), std::move(probe)});
        } else {
            counters.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return sampled;
}

namespace {

// Draw thread. Runs of consecutive resident pages inside [lo, hi), as (guest address, bytes). Residency is read
// without changing it.
// skip_aliased: the aliased pages are left out (their memory is the peer's arena already); their bytes go to skipped.
void ResidentRuns(const State& s, VAddr lo, VAddr hi, std::vector<std::pair<VAddr, u64>>& runs, bool skip_aliased,
                  u64& skipped) {
    for (u64 page = (lo - s.base) / PageSize; page <= (hi - s.base - 1) / PageSize; ++page) {
        if (!s.resident.Test(page)) {
            continue;
        }
        const VAddr page_begin = s.base + page * PageSize;
        const VAddr a = std::max<VAddr>(lo, page_begin);
        const VAddr b = std::min<VAddr>(hi, page_begin + PageSize);
        if (skip_aliased && s.aliased.Test(page)) {
            skipped += b - a;
            continue;
        }
        if (!runs.empty() && runs.back().first + runs.back().second == a) {
            runs.back().second += b - a;
        } else {
            runs.emplace_back(a, b - a);
        }
    }
}

// Coherence barriers around a copy into or out of the mirror. Before: every earlier command's memory access (a
// shader write, a previous read of the mirror) is done. After: the copy is visible to every later command.
void CopyBarrierBefore(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 before = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &before,
    });
}

void CopyBarrierAfter(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 after = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &after,
    });
}

} // namespace

void SyncFromBuffer(Vulkan::Scheduler& scheduler, vk::Buffer src, u64 src_offset, VAddr addr, u64 size,
                    bool peer_arena) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return;
    }
    std::scoped_lock lk{process_mutex};
    BusyScope busy;
    State& s = *state;
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        counters.gpu_sync_refused.fetch_add(size, std::memory_order_relaxed);
        return; // outside every window: the mirror does not hold these bytes
    }
    counters.gpu_sync_refused.fetch_add((lo - addr) + (addr + size - hi), std::memory_order_relaxed);
    ProcessPending(s, scheduler); // map and unmap events first: mappability is decided by them

    // The GPU bytes must land in every page of the range, so a page that is not resident is made resident first.
    // Its fill from guest memory is recorded in the upload command buffer (before this submit); the copy below is
    // recorded after the writing op, so it wins over the fill. A page that cannot be made resident keeps its old
    // bytes (counted as refused).
    std::vector<u64> pages;
    for (u64 page = (lo - s.base) / PageSize; page <= (hi - s.base - 1) / PageSize; ++page) {
        if (!s.resident.Test(page)) {
            pages.push_back(page);
        }
    }
    if (!pages.empty()) {
        const u64 missing = MakeResident(s, scheduler, pages);
        if (missing != 0) {
            counters.gpu_sync_refused.fetch_add(missing * PageSize, std::memory_order_relaxed);
        }
    }

    // sotc: A2b. An aliased page of an arena source is the same memory: the write is already in the mirror.
    std::vector<std::pair<VAddr, u64>> runs;
    u64 skipped = 0;
    ResidentRuns(s, lo, hi, runs, peer_arena, skipped);
    counters.sync_aliased_bytes.fetch_add(skipped, std::memory_order_relaxed);
    if (runs.empty()) {
        return;
    }
    std::vector<vk::BufferCopy> copies;
    copies.reserve(runs.size());
    u64 bytes = 0;
    for (const auto& [run_addr, run_size] : runs) {
        copies.push_back(vk::BufferCopy{
            .srcOffset = src_offset + (run_addr - addr),
            .dstOffset = run_addr - s.base,
            .size = run_size,
        });
        bytes += run_size;
    }
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    CopyBarrierBefore(cmdbuf);
    cmdbuf.copyBuffer(src, *s.buffer, copies);
    CopyBarrierAfter(cmdbuf);
    counters.gpu_sync_from_calls.fetch_add(1, std::memory_order_relaxed);
    counters.gpu_sync_from_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void SyncToBuffer(Vulkan::Scheduler& scheduler, vk::Buffer dst, u64 dst_offset, VAddr addr, u64 size,
                  bool peer_arena) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return;
    }
    std::scoped_lock lk{process_mutex};
    BusyScope busy;
    State& s = *state;
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        counters.gpu_sync_refused.fetch_add(size, std::memory_order_relaxed);
        return;
    }
    counters.gpu_sync_refused.fetch_add((lo - addr) + (addr + size - hi), std::memory_order_relaxed);
    ProcessPending(s, scheduler);

    // Only resident pages hold the shader's bytes: the addressed shader made its range resident (Touch) at bind.
    // A page that is not resident is not copied, so the buffer keeps its bytes there (counted as refused).
    // sotc: A2b. An aliased page of an arena destination is the same memory: the shader's write is already there.
    std::vector<std::pair<VAddr, u64>> runs;
    u64 skipped = 0;
    ResidentRuns(s, lo, hi, runs, peer_arena, skipped);
    counters.sync_aliased_bytes.fetch_add(skipped, std::memory_order_relaxed);
    u64 bytes = 0;
    std::vector<vk::BufferCopy> copies;
    copies.reserve(runs.size());
    for (const auto& [run_addr, run_size] : runs) {
        copies.push_back(vk::BufferCopy{
            .srcOffset = run_addr - s.base,
            .dstOffset = dst_offset + (run_addr - addr),
            .size = run_size,
        });
        bytes += run_size;
    }
    counters.gpu_sync_refused.fetch_add((hi - lo) - bytes - skipped, std::memory_order_relaxed);
    if (copies.empty()) {
        return;
    }
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    CopyBarrierBefore(cmdbuf);
    cmdbuf.copyBuffer(*s.buffer, dst, copies);
    CopyBarrierAfter(cmdbuf);
    counters.gpu_sync_to_calls.fetch_add(1, std::memory_order_relaxed);
    counters.gpu_sync_to_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void SetArenaHold(ArenaHold fn, void* ctx) {
    hold_fn = fn;
    hold_ctx = ctx;
}

bool AliasAccepts(u32 memory_type) {
    if (!enabled.load(std::memory_order_acquire) || memory_type >= 32) {
        return false;
    }
    return ((state->alias_type_bits >> memory_type) & 1) != 0;
}

bool AliasBind(VAddr addr, u64 size, vk::DeviceMemory memory, u64 memory_offset) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return false;
    }
    State& s = *state;
    const bool shape = addr >= s.base && addr + size > addr && addr + size <= s.base + s.window_size &&
                       (addr - s.base) % PageSize == 0 && size % PageSize == 0 && memory_offset % PageSize == 0 &&
                       static_cast<bool>(memory);
    if (!shape) {
        counters.alias_refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u64 first = (addr - s.base) / PageSize;
    const u64 pages = size / PageSize;
    std::scoped_lock lk{alias_mutex};
    for (u64 page = first; page < first + pages; ++page) {
        // An own page keeps its own bytes (the old sync path covers it); a page already aliased is never bound twice.
        if (BoundSlot(s, page) != kNoSlot || s.resident.Test(page)) {
            counters.alias_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    alias_queue.push_back(AliasOp{
        .first_page = first,
        .pages = pages,
        .bind = vk::SparseMemoryBind{
            .resourceOffset = first * PageSize,
            .size = size,
            .memory = memory,
            .memoryOffset = memory_offset,
        },
    });
    for (u64 page = first; page < first + pages; ++page) {
        s.resident.Set(page);
        s.aliased.Set(page);
    }
    aliased_pages.fetch_add(pages, std::memory_order_relaxed);
    alias_queued.store(true, std::memory_order_release);
    counters.alias_binds.fetch_add(1, std::memory_order_relaxed);
    counters.alias_pages_bound.fetch_add(pages, std::memory_order_relaxed);
    return true;
}

void AliasUnbind(VAddr addr, u64 size) {
    if (!enabled.load(std::memory_order_acquire) || size == 0) {
        return;
    }
    State& s = *state;
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        return;
    }
    // Whole pages only: the buffer cache unbinds whole blocks, and a block is a page.
    const u64 first = (lo - s.base + PageSize - 1) / PageSize;
    const u64 end = (hi - s.base) / PageSize;
    std::scoped_lock lk{alias_mutex};
    u64 page = first;
    while (page < end) {
        if (!s.aliased.Test(page)) {
            ++page;
            continue;
        }
        u64 run_end = page;
        while (run_end < end && s.aliased.Test(run_end)) {
            ++run_end;
        }
        const u64 run = run_end - page;
        s.aliased.SetRange(page, run, false);
        s.resident.SetRange(page, run, false);
        alias_queue.push_back(AliasOp{
            .first_page = page,
            .pages = run,
            .bind = vk::SparseMemoryBind{
                .resourceOffset = page * PageSize,
                .size = run * PageSize,
            },
        });
        aliased_pages.fetch_sub(run, std::memory_order_relaxed);
        alias_queued.store(true, std::memory_order_release);
        counters.alias_unbinds.fetch_add(1, std::memory_order_relaxed);
        counters.alias_pages_unbound.fetch_add(run, std::memory_order_relaxed);
        page = run_end;
    }
}

bool AliasActive() {
    return alias_queued.load(std::memory_order_acquire) || aliased_pages.load(std::memory_order_relaxed) != 0;
}

void AliasBarrier(vk::CommandBuffer cmdbuf) {
    if (!AliasActive()) {
        return;
    }
    // Writes through one alias (an arena or the window) to reads through the other: no resource tracks that pair.
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void TakeAliasCalls(std::vector<AliasCall>& out) {
    std::vector<AliasOp> ops;
    {
        std::scoped_lock lk{alias_mutex};
        ops.swap(alias_queue);
        alias_queued.store(false, std::memory_order_release);
    }
    if (ops.empty() || !enabled.load(std::memory_order_acquire)) {
        return;
    }
    State& s = *state;
    // One call per run of ops with no page twice: two binds of one page in one call are not ordered (the unbind of
    // a page and its rebind must be two calls, in order).
    std::unordered_set<u64> seen;
    AliasCall call{*s.buffer, {}};
    for (const AliasOp& op : ops) {
        bool clash = false;
        for (u64 page = op.first_page; page < op.first_page + op.pages && !clash; ++page) {
            clash = seen.count(page) != 0;
        }
        if (clash) {
            out.push_back(std::move(call));
            call = AliasCall{*s.buffer, {}};
            seen.clear();
        }
        for (u64 page = op.first_page; page < op.first_page + op.pages; ++page) {
            seen.insert(page);
        }
        if (!call.binds.empty()) {
            vk::SparseMemoryBind& last = call.binds.back();
            const bool contiguous = last.memory == op.bind.memory &&
                                    last.resourceOffset + last.size == op.bind.resourceOffset &&
                                    (!op.bind.memory || last.memoryOffset + last.size == op.bind.memoryOffset);
            if (contiguous) {
                last.size += op.bind.size;
                continue;
            }
        }
        call.binds.push_back(op.bind);
    }
    if (!call.binds.empty()) {
        out.push_back(std::move(call));
    }
    counters.alias_calls.fetch_add(out.size(), std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    if (!enabled.load(std::memory_order_acquire)) {
        std::snprintf(out, size, "SOTC MIRROR: off");
        return;
    }
    const double sec = seconds > 0 ? seconds : 1.0;
    const double mib = 1024.0 * 1024.0;
    const u64 bound = counters.pages_bound.exchange(0, std::memory_order_relaxed);
    const u64 unbound = counters.pages_unbound.exchange(0, std::memory_order_relaxed);
    const u64 binds = counters.bind_calls.exchange(0, std::memory_order_relaxed);
    const u64 filled = counters.pages_filled.exchange(0, std::memory_order_relaxed);
    const u64 up_bytes = counters.upload_bytes.exchange(0, std::memory_order_relaxed);
    const u64 up_ranges = counters.upload_ranges.exchange(0, std::memory_order_relaxed);
    const u64 touches = counters.touches.exchange(0, std::memory_order_relaxed);
    const u64 touch_failed = counters.touch_failed.exchange(0, std::memory_order_relaxed);
    const u64 refused_win = counters.refused_window.exchange(0, std::memory_order_relaxed);
    const u64 refused_vram = counters.refused_vram.exchange(0, std::memory_order_relaxed);
    const u64 refused_alloc = counters.refused_alloc.exchange(0, std::memory_order_relaxed);
    const u64 nonres = counters.nonresident_bytes.exchange(0, std::memory_order_relaxed);
    const u64 checked = counters.verify_checked.exchange(0, std::memory_order_relaxed);
    const u64 mismatch = counters.verify_mismatch.exchange(0, std::memory_order_relaxed);
    const u64 skipped = counters.verify_skipped.exchange(0, std::memory_order_relaxed);
    const u64 from_calls = counters.gpu_sync_from_calls.exchange(0, std::memory_order_relaxed);
    const u64 from_bytes = counters.gpu_sync_from_bytes.exchange(0, std::memory_order_relaxed);
    const u64 to_calls = counters.gpu_sync_to_calls.exchange(0, std::memory_order_relaxed);
    const u64 to_bytes = counters.gpu_sync_to_bytes.exchange(0, std::memory_order_relaxed);
    const u64 sync_refused = counters.gpu_sync_refused.exchange(0, std::memory_order_relaxed);
    const u64 bound_now = bound_pages.load(std::memory_order_relaxed);
    const u64 reserved = reserved_bytes.load(std::memory_order_relaxed);
    const u64 budget_left = state->budget > reserved ? state->budget - reserved : 0;
    // Slots are unique, so bound pages can never exceed the reserved slots. If they do, the counter is wrong.
    const bool over = bound_now * PageSize > reserved;
    std::snprintf(
        out, size,
        "SOTC MIRROR: window %llu MiB at %#llx, reserved %.1f MiB, bound %.1f MiB (%llu pages)%s, "
        "binds/s %.1f (%llu pages bound, %llu unbound), uploads %.2f MiB/s (%llu ranges), "
        "touches %llu (%llu failed), pages filled %llu, "
        "refusals: window %llu, vram %llu, alloc %llu, skipped non-resident %.2f MiB, "
        "verify %llu checked, %llu mismatches, %llu skipped, budget left %.1f MiB, "
        "gpu writes to mirror %llu (%.2f MiB), mirror writes to buffers %llu (%.2f MiB), "
        "gpu sync refused %.2f MiB",
        static_cast<unsigned long long>(state->window_size >> 20),
        static_cast<unsigned long long>(state->base),
        reserved / mib,
        static_cast<double>(bound_now) * PageSize / mib,
        static_cast<unsigned long long>(bound_now), over ? " (BUG: exceeds reserved)" : "",
        static_cast<double>(binds) / sec, static_cast<unsigned long long>(bound),
        static_cast<unsigned long long>(unbound), up_bytes / mib / sec,
        static_cast<unsigned long long>(up_ranges),
        static_cast<unsigned long long>(touches), static_cast<unsigned long long>(touch_failed),
        static_cast<unsigned long long>(filled),
        static_cast<unsigned long long>(refused_win), static_cast<unsigned long long>(refused_vram),
        static_cast<unsigned long long>(refused_alloc), nonres / mib, static_cast<unsigned long long>(checked),
        static_cast<unsigned long long>(mismatch), static_cast<unsigned long long>(skipped),
        budget_left / mib,
        static_cast<unsigned long long>(from_calls), static_cast<double>(from_bytes) / mib,
        static_cast<unsigned long long>(to_calls), static_cast<double>(to_bytes) / mib,
        static_cast<double>(sync_refused) / mib);

    // Coverage (cumulative for the run): how many guest maps, touches, shader finds and uploads asked for a range the
    // window does not hold. Zero "outside" counts and a highest map end below the window end show full coverage.
    const size_t used = std::strlen(out);
    if (used < size) {
        const u64 low = coverage.map_low.load(std::memory_order_relaxed);
        std::snprintf(
            out + used, size - used,
            ", coverage: window [%#llx, %#llx), maps %llu (outside %llu, lowest %#llx, highest end %#llx), "
            "touches %llu (outside %llu), finds %llu (outside %llu), uploads %llu (outside %llu)",
            static_cast<unsigned long long>(state->base),
            static_cast<unsigned long long>(state->base + state->window_size),
            static_cast<unsigned long long>(coverage.maps.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.maps_outside.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(low == ~u64{0} ? 0 : low),
            static_cast<unsigned long long>(coverage.map_high.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.touches.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.touches_outside.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.finds.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.finds_outside.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.uploads.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(coverage.uploads_outside.load(std::memory_order_relaxed)));
    }

    // sotc: A2b aliases (one copy: the arena memory is also the window's). Per interval unless marked total.
    const u64 al_binds = counters.alias_binds.exchange(0, std::memory_order_relaxed);
    const u64 al_bound = counters.alias_pages_bound.exchange(0, std::memory_order_relaxed);
    const u64 al_unbinds = counters.alias_unbinds.exchange(0, std::memory_order_relaxed);
    const u64 al_unbound = counters.alias_pages_unbound.exchange(0, std::memory_order_relaxed);
    const u64 al_calls = counters.alias_calls.exchange(0, std::memory_order_relaxed);
    const u64 al_refused = counters.alias_refused.exchange(0, std::memory_order_relaxed);
    const u64 t_held = counters.touch_held.exchange(0, std::memory_order_relaxed);
    const u64 t_aliased = counters.touch_aliased.exchange(0, std::memory_order_relaxed);
    const u64 t_own = counters.touch_own.exchange(0, std::memory_order_relaxed);
    const u64 up_skip = counters.upload_aliased_bytes.exchange(0, std::memory_order_relaxed);
    const u64 sync_skip = counters.sync_aliased_bytes.exchange(0, std::memory_order_relaxed);
    const u64 aliased_now = aliased_pages.load(std::memory_order_relaxed);
    const size_t used_alias = std::strlen(out);
    if (used_alias < size) {
        std::snprintf(
            out + used_alias, size - used_alias,
            ", aliases: %llu pages (%.1f MiB) shared with arenas, own %llu pages; binds/s %.1f (%llu pages, "
            "%llu refused), unbinds/s %.1f (%llu pages), calls %llu; touches: %llu aliased, %llu held by the "
            "buffer cache, %llu own; upload skipped %.2f MiB, sync skipped %.2f MiB",
            static_cast<unsigned long long>(aliased_now), static_cast<double>(aliased_now) * PageSize / mib,
            static_cast<unsigned long long>(bound_now), static_cast<double>(al_binds) / sec,
            static_cast<unsigned long long>(al_bound), static_cast<unsigned long long>(al_refused),
            static_cast<double>(al_unbinds) / sec, static_cast<unsigned long long>(al_unbound),
            static_cast<unsigned long long>(al_calls), static_cast<unsigned long long>(t_aliased),
            static_cast<unsigned long long>(t_held), static_cast<unsigned long long>(t_own),
            static_cast<double>(up_skip) / mib, static_cast<double>(sync_skip) / mib);
    }
}

} // namespace SotcArch::Mirror

// C bridge for src/runtime_memory.c. The runtime references these weakly; this object is pulled into
// libbbgpu.a by the scheduler's ReadyForSubmit call (a strong reference), so the weak references bind.
extern "C" void bbgpu_mirror_on_map(uint64_t addr, uint64_t size) {
    SotcArch::Mirror::OnMap(addr, size);
}

extern "C" void bbgpu_mirror_on_unmap(uint64_t addr, uint64_t size) {
    SotcArch::Mirror::OnUnmap(addr, size);
}
