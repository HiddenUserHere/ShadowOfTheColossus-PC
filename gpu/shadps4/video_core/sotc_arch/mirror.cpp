// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A2 GPU memory mirror (SOTC_MIRROR, ARCH-DRAW.md part 1; contract in sotc_arch.h). One sparse VkBuffer covers
// the PS4 user range from USER_MIN: its offset o holds guest address base + o. Residency follows GPU demand: a 64 KiB
// page gets device memory and its guest bytes only when Touch (or a shader-side Find, through the touched flags) asks
// for it. Upload never binds: it copies announced bytes into the pages that are already resident and skips the rest.
// Map and unmap events arrive from any thread and are only queued; the draw thread processes them at submit start
// (ReadyForSubmit) and in Upload / Touch.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
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

// USER_MIN of src/runtime_memory.c. The user range is reserved at the same host address, so guest addresses
// are host pointers: a copy reads the guest bytes at their own address. If USER_MIN changes, change this.
constexpr VAddr kUserMin = 0x1000000000ULL;
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
constexpr u64 kWindowSizes[] = {16ULL << 30, 8ULL << 30, 4ULL << 30};

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
};

struct State {
    vk::Device device;
    vk::Queue queue; // queue index 0 of the sparse family: the scheduler's graphics queue
    vk::UniqueBuffer buffer;
    vk::DeviceAddress device_base{};
    VAddr base = kUserMin;
    u64 window_size{};
    u64 pages{};
    std::vector<Window> windows;
    // Per page: resident = bound, filled and usable; touched = a shader-side Find asked for it.
    std::unique_ptr<std::atomic<u8>[]> resident;
    std::unique_ptr<std::atomic<u8>[]> touched;
    // Per 16 KiB sub-page: guest-mapped. A page is mappable when all its sub-pages are mapped.
    std::unique_ptr<std::atomic<u8>[]> mapped_sub;
    std::vector<u32> bound;         // physical slot per page, kNoSlot when none
    std::vector<u8> retiring;       // page is on retire_list
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
std::recursive_mutex process_mutex;
Counters counters;

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
        if (s.mapped_sub[first + i].load(std::memory_order_acquire) == 0) {
            return false;
        }
    }
    return true;
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
    if (s.bound[page] != kNoSlot) {
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
    s.bound[page] = slot;
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
        s.retiring[page] = 0;
        if (s.resident[page].load(std::memory_order_acquire) != 0 || s.bound[page] == kNoSlot) {
            continue;
        }
        AddUnbind(unbinds, page);
        s.free_slots.push_back(s.bound[page]);
        s.bound[page] = kNoSlot;
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
        for (u64 i = sub_first; i < sub_last; ++i) {
            s.mapped_sub[i].store(op.map ? 1 : 0, std::memory_order_release);
        }
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
            if (s.resident[page].load(std::memory_order_acquire) == 0) {
                continue;
            }
            s.resident[page].store(0, std::memory_order_release);
            if (!s.retiring[page]) {
                s.retiring[page] = 1;
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
    for (const u64 page : pages) {
        if (s.resident[page].load(std::memory_order_acquire) != 0) {
            continue;
        }
        if (!PageMappable(s, page) || !EnsureBound(s, page, binds)) {
            ++missing;
            continue;
        }
        fill.push_back(page); // bound now, or bound earlier and not filled since its unmap (stale bytes)
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
        s.resident[page].store(1, std::memory_order_release);
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
        std::vector<u64> pages;
        for (u64 page = 0; page < s.pages; ++page) {
            if (s.touched[page].exchange(0, std::memory_order_acq_rel) != 0) {
                pages.push_back(page);
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
    const u32 family = instance.SotcSparseQueueFamily();
    if (family == Vulkan::Instance::NoSparseQueueFamily) {
        Disable("no sparse-capable queue family");
        return;
    }

    auto s = std::make_unique<State>();
    s->device = instance.GetDevice();
    // Queue index 0 of the sparse family: the scheduler's graphics queue (same VkQueue, same lock).
    s->queue = instance.GetGraphicsQueue();

    for (const u64 size : kWindowSizes) {
        auto [created, buffer] = s->device.createBufferUnique(vk::BufferCreateInfo{
            .flags = vk::BufferCreateFlagBits::eSparseBinding |
                     vk::BufferCreateFlagBits::eSparseResidency,
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
    }
    if (!s->window_size) {
        Disable("no window size could be created as a sparse buffer");
        return;
    }
    LOG_INFO(Render, "SOTC_MIRROR: window {:#x} at {:#x}", s->window_size, s->base);

    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();
    // (const reference: GetMemoryProperties returns one)
    const vk::MemoryRequirements req = s->device.getBufferMemoryRequirements(*s->buffer);
    if (req.alignment > PageSize) {
        Disable("sparse page alignment is larger than 64 KiB");
        return;
    }
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

    s->pages = s->window_size / PageSize;
    const u64 subs = s->window_size / kSubPage;
    s->resident = std::make_unique<std::atomic<u8>[]>(s->pages);
    s->touched = std::make_unique<std::atomic<u8>[]>(s->pages);
    s->mapped_sub = std::make_unique<std::atomic<u8>[]>(subs);
    s->bound.assign(s->pages, kNoSlot);
    s->retiring.assign(s->pages, 0);
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
    if (addr < s.base || addr + size > s.base + s.window_size || addr + size < addr) {
        return nullptr;
    }
    const u64 page_first = (addr - s.base) / PageSize;
    const u64 page_last = (addr + size - 1 - s.base) / PageSize;
    bool all = true;
    for (u64 page = page_first; page <= page_last; ++page) {
        if (s.resident[page].load(std::memory_order_acquire) != 0) {
            continue;
        }
        all = false;
        if (PageMappable(s, page)) {
            // Asked for by a shader: made resident at the next submit start (ReadyForSubmit).
            s.touched[page].store(1, std::memory_order_release);
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
    ProcessPending(s, scheduler); // map and unmap events first: mappability is decided by them
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        counters.touch_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::vector<u64> pages;
    for (u64 page = (lo - s.base) / PageSize; page <= (hi - s.base - 1) / PageSize; ++page) {
        pages.push_back(page);
    }
    const u64 missing = MakeResident(s, scheduler, pages);
    bool whole = lo == addr && hi == addr + size && missing == 0;
    for (size_t i = 0; whole && i < pages.size(); ++i) {
        whole = s.resident[pages[i]].load(std::memory_order_acquire) != 0;
    }
    if (!whole) {
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
    u64 lo{};
    u64 hi{};
    if (!Clip(s, addr, size, lo, hi)) {
        counters.refused_window.fetch_add(1, std::memory_order_relaxed);
        return false; // counted no-op: the range is outside every window
    }
    if (lo != addr || hi != addr + size) {
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
        if (s.resident[page].load(std::memory_order_acquire) == 0) {
            counters.nonresident_bytes.fetch_add(b - a, std::memory_order_relaxed);
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
        "verify %llu checked, %llu mismatches, %llu skipped, budget left %.1f MiB",
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
        budget_left / mib);
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
