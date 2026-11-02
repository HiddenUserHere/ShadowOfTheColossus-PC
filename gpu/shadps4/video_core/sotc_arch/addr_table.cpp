// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ADDR_TABLE (addr_table.h). The guest page table: a device-local buffer of u64 entries, a host shadow
// that the placements update, and dirty 4 KiB chunks uploaded on the upload command buffer (it runs before the
// commands of the submit that uses them).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <algorithm>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/shader_addr.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::AddrTable {

std::atomic<bool> g_hooks_on{false};

namespace {

constexpr u64 kChunkEntries = 512; ///< entries per dirty chunk: 4 KiB of table
constexpr u64 kChunkBytes = kChunkEntries * sizeof(u64);
constexpr u64 kNumChunks = NumPages / kChunkEntries;
constexpr u64 kWords = NumPages / 64; ///< 64-page words of the placed and clean bitmaps
constexpr u64 kStreamSize = 8ULL << 20; ///< staging of the chunk uploads (a Map of one chunk each)

// sotc: the clean state (see addr_table.h). Bit p % 64 of word p / 64 is table page p. Written with seq_cst
// RMWs by the owners described in the proof at Touch; read lock-free by the fast path. Zero at start: nothing
// placed, nothing clean. g_dirty_seq is bumped by every ClearClean before its bits are cleared.
std::atomic<u64> g_placed[kWords]{};
std::atomic<u64> g_clean[kWords]{};
std::atomic<u64> g_dirty_seq{0};

struct State {
    vk::UniqueBuffer buffer;             ///< the table: NumPages u64 entries, device-local, with a device address
    vk::UniqueDeviceMemory memory;
    vk::DeviceAddress base{};
    std::unique_ptr<VideoCore::StreamBuffer> stream;
    std::vector<u64> shadow;             ///< the host copy of the entries (guarded by g_mutex)
    std::vector<u8> dirty;               ///< one flag per chunk (guarded by g_mutex)
    std::vector<u32> dirty_list;         ///< the chunks whose flag is set, so a flush visits only those
    u64 live{};                          ///< nonzero entries (guarded by g_mutex)
};

std::mutex g_mutex;
std::unique_ptr<State> g_state;

std::atomic<u64> g_updates{};        // entries written into the shadow
std::atomic<u64> g_served{};         // Touch calls that made their range resident and wrote its entries
std::atomic<u64> g_failed{};         // Touch calls refused, and chunks that could not be staged
std::atomic<u64> g_uploads{};        // dirty chunks copied to the table
std::atomic<u64> g_upload_bytes{};
std::atomic<u64> g_fast{};           // read Touch calls answered from the clean state (no ObtainBuffer)
std::atomic<u64> g_slow_unplaced{};  // slow read Touch calls: a page of the range was not placed
std::atomic<u64> g_slow_dirty{};     // slow read Touch calls: all placed, some page not clean
std::atomic<u64> g_slow_written{};   // slow written Touch calls (always slow, see Touch)
std::atomic<u64> g_reverts{};        // slow Touch calls whose clean bits a concurrent Dirty made stale
std::atomic<u64> g_dirty_calls{};    // ClearClean calls (Dirty, DirtyPages, OnPlace, OnRemove, written binds)
std::atomic<u64> g_would_fast{};     // SOTC_SHADER_ADDR=2: reads the fast path would have answered
std::atomic<u64> g_would_fast_bad{}; // SOTC_SHADER_ADDR=2: of those, the slow path wrote other entries

// sotc: refusal at Init, like ShaderAddr::Refuse: a run that goes on would read the table as zero.
[[noreturn]] void Refuse(const char* reason) {
    std::fprintf(stderr, "SOTC ADDR_TABLE refused: %s\n", reason);
    std::exit(EXIT_FAILURE);
}

// Table pages [first, last] of the window part of [addr, addr + size). False when none of it lies in the window.
bool WindowPages(VAddr addr, u64 size, u64& first, u64& last) {
    const VAddr window_end = WindowBase + WindowSize;
    if (size == 0 || addr >= window_end || addr + size <= WindowBase) {
        return false;
    }
    const VAddr lo = std::max<VAddr>(addr, WindowBase);
    const VAddr hi = std::min<VAddr>(addr + size, window_end); // exclusive
    first = (lo - WindowBase) >> PageShift;
    last = (hi - 1 - WindowBase) >> PageShift;
    return true;
}

// Calls fn(word, mask) for the words of table pages [first, last], mask = the pages of that word in the range.
template <typename Fn>
void ForWords(u64 first, u64 last, Fn&& fn) {
    for (u64 w = first / 64; w <= last / 64; ++w) {
        const u64 lo = w == first / 64 ? first % 64 : 0;
        const u64 hi = w == last / 64 ? last % 64 : 63;
        const u64 mask = hi - lo == 63 ? ~u64{0} : ((u64{1} << (hi - lo + 1)) - 1) << lo;
        fn(w, mask);
    }
}

// Clears the clean state of the pages of [addr, addr + size). Lock-free, any thread. The sequence bump comes BEFORE
// the bit clears: a Touch that set its bits after one of these clears has its sequence check fail (see Touch).
void ClearClean(VAddr addr, u64 size) {
    u64 first = 0;
    u64 last = 0;
    if (!WindowPages(addr, size, first, last)) {
        return;
    }
    g_dirty_calls.fetch_add(1, std::memory_order_relaxed);
    g_dirty_seq.fetch_add(1, std::memory_order_seq_cst);
    ForWords(first, last, [](u64 w, u64 mask) {
        g_clean[w].fetch_and(~mask, std::memory_order_seq_cst);
    });
}

// Writes one entry (g_mutex held). The placed bit follows the entry: set when it is not 0, cleared when it is.
void SetEntryLocked(State& s, u64 page, u64 value) {
    u64& entry = s.shadow[page];
    if (entry == value) {
        return;
    }
    if (entry == 0) {
        ++s.live;
    } else if (value == 0) {
        --s.live;
    }
    entry = value;
    const u64 bit = u64{1} << (page % 64);
    if (value == 0) {
        g_placed[page / 64].fetch_and(~bit, std::memory_order_seq_cst);
    } else {
        g_placed[page / 64].fetch_or(bit, std::memory_order_seq_cst);
    }
    const u64 chunk = page / kChunkEntries;
    if (!s.dirty[chunk]) {
        s.dirty[chunk] = 1;
        s.dirty_list.push_back(static_cast<u32>(chunk));
    }
    g_updates.fetch_add(1, std::memory_order_relaxed);
}

// True when a chunk of table pages [first, last] still has entries the device does not hold (g_mutex held).
bool AnyDirtyLocked(const State& s, u64 first, u64 last) {
    for (u64 chunk = first / kChunkEntries; chunk <= last / kChunkEntries; ++chunk) {
        if (s.dirty[chunk]) {
            return true;
        }
    }
    return false;
}

// Entries of the pages of [addr, addr + size) for a range whose byte addr lies at device. Each entry is the device
// address of its page's first byte: device minus the bytes of the page that lie before addr (u64 wrap, and the
// shader adds the offset in the page back, so the sum is exact).
void SetRangeLocked(State& s, VAddr addr, u64 size, vk::DeviceAddress device) {
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    for (u64 page = first; page <= last; ++page) {
        const VAddr page_start = WindowBase + (page << PageShift);
        SetEntryLocked(s, page, device + (page_start - addr));
    }
}

// Uploads the dirty chunks on the upload command buffer, then one barrier for all of them (g_mutex held). A chunk the
// stream cannot hold stays dirty and is retried by the next flush.
void FlushLocked(State& s, Vulkan::Scheduler& scheduler) {
    if (s.dirty_list.empty()) {
        return; // sotc: the common case: the Touch changed no entry
    }
    bool recorded = false;
    std::vector<u32> kept; // chunks the stream could not hold, retried by the next flush
    for (const u32 chunk : s.dirty_list) {
        auto [dst, src_offset] = s.stream->Map(kChunkBytes);
        if (dst == nullptr) {
            g_failed.fetch_add(1, std::memory_order_relaxed);
            kept.push_back(chunk);
            continue;
        }
        s.dirty[chunk] = 0;
        std::memcpy(dst, &s.shadow[chunk * kChunkEntries], kChunkBytes);
        s.stream->Commit();
        const vk::BufferCopy copy = {
            .srcOffset = src_offset,
            .dstOffset = chunk * kChunkBytes,
            .size = kChunkBytes,
        };
        scheduler.UploadCommandBuffer().copyBuffer(s.stream->Handle(), *s.buffer, copy);
        g_uploads.fetch_add(1, std::memory_order_relaxed);
        g_upload_bytes.fetch_add(kChunkBytes, std::memory_order_relaxed);
        recorded = true;
    }
    s.dirty_list = std::move(kept);
    if (recorded) {
        // Copies -> shader reads: the table is read through physical addresses by the next submit's shaders.
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
}

u32 FindDeviceLocalType(const vk::PhysicalDeviceMemoryProperties& props, u32 type_bits) {
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) ==
                vk::MemoryPropertyFlagBits::eDeviceLocal) {
            return i;
        }
    }
    // Not reached on a device with device-local memory (the table takes whatever type it is allowed).
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if (type_bits & (1u << i)) {
            return i;
        }
    }
    return props.memoryTypeCount;
}

} // namespace

bool On() {
    static const bool on = SwitchMode("SOTC_ADDR_TABLE") != 0 && SwitchMode("SOTC_SHADER_ADDR") != 0;
    return on;
}

void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    if (!On() || g_state) {
        return;
    }
    if (!instance.SotcBufferDeviceAddress()) {
        Refuse("the device has no buffer device address");
    }
    const vk::Device device = instance.GetDevice();
    const u64 bytes = NumPages * sizeof(u64);
    auto [created, buffer] = device.createBufferUnique(vk::BufferCreateInfo{
        .size = bytes,
        .usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (created != vk::Result::eSuccess) {
        Refuse("the table buffer could not be created");
    }
    const vk::MemoryRequirements req = device.getBufferMemoryRequirements(*buffer);
    const u32 type = FindDeviceLocalType(instance.GetMemoryProperties(), req.memoryTypeBits);
    if (type == instance.GetMemoryProperties().memoryTypeCount) {
        Refuse("no memory type for the table buffer");
    }
    // A buffer with a device address needs its memory allocated with the device address flag.
    const vk::StructureChain alloc_chain = {
        vk::MemoryAllocateInfo{.allocationSize = req.size, .memoryTypeIndex = type},
        vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress},
    };
    auto [alloc, memory] = device.allocateMemoryUnique(alloc_chain.get<vk::MemoryAllocateInfo>());
    if (alloc != vk::Result::eSuccess) {
        Refuse("the table memory could not be allocated");
    }
    if (device.bindBufferMemory(*buffer, *memory, 0) != vk::Result::eSuccess) {
        Refuse("the table memory could not be bound");
    }

    auto state = std::make_unique<State>();
    state->base = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *buffer});
    state->shadow.assign(NumPages, 0);
    state->dirty.assign(kNumChunks, 0);
    state->stream = std::make_unique<VideoCore::StreamBuffer>(
        instance, scheduler, VideoCore::MemoryType::HostUncached, kStreamSize);
    // Every entry starts at 0 (not resident). The fill runs on the upload command buffer, before any draw.
    scheduler.UploadCommandBuffer().fillBuffer(*buffer, 0, bytes, 0u);
    const vk::MemoryBarrier2 filled = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    scheduler.UploadCommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &filled,
    });
    state->buffer = std::move(buffer);
    state->memory = std::move(memory);
    std::fprintf(stderr, "SOTC ADDR_TABLE: %llu MiB table at %#llx, %llu pages of %llu KiB\n",
                 static_cast<unsigned long long>(bytes >> 20),
                 static_cast<unsigned long long>(state->base),
                 static_cast<unsigned long long>(NumPages),
                 static_cast<unsigned long long>(PageSize >> 10));
    g_state = std::move(state);
    g_hooks_on.store(true, std::memory_order_release); // the Dirty hooks run from here on (all bits start clear)
}

vk::DeviceAddress Base() {
    return g_state ? g_state->base : 0;
}

// Why a page that Touch marks clean cannot be stale (the readers are Touch's fast path and the slow path's set; the
// writers are Dirty / DirtyPages, OnPlace, OnRemove, and the written binds). All bitmap and sequence accesses are
// seq_cst, so they have one total order S.
//  1. A change of guest bytes or of the device entries clears the bits, and the clear comes after the change in the
//     writer's program order: a CPU bit rises in UpdateStateAndProtection under the region mutex, and the guest's own
//     write goes through only after the page is unprotected (after that call); the announce drain runs on the draw
//     thread before the binds of its submit; NoteCpuBytesChanged runs after its copy; OnPlace/OnRemove change the
//     entries; a written bind clears before its GPU write (which the draw orders after it).
//  2. ClearClean bumps g_dirty_seq (b) BEFORE it clears the bits (c).
//  3. A slow read Touch takes s1 = g_dirty_seq, then the sync (its copy of the bytes), then sets its bits (t), then
//     takes s2 = g_dirty_seq; when s2 != s1 it clears its bits again.
//  4. Race: a CPU write W that the sync's copy did not see. Its change is after the copy, so b is after s1 in S.
//     If c precedes t, then b precedes t and s2 comes after t, so s2 sees b and the bits are cleared again. If c
//     follows t, the bits are clear after c. Either way the bits are clear after Touch returns, unless no W happened.
//  5. The sync leaves a page with a CPU bit only when it did not take it; Touch then does not set the bits (the
//     IsRegionCpuModified check). A device entry that a failed chunk upload has not reached (AnyDirtyLocked) also
//     blocks the set, so a clean page always has its shadow entries on the device.
// So a page is clean only while its guest bytes and its device entries are the ones its last sync left, and a read
// that finds all its pages placed and clean has nothing to upload.
bool Touch(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size,
           bool is_written) {
    State* s = g_state.get();
    if (!s || !On()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (addr < WindowBase || addr - WindowBase > WindowSize - size || size > 0xffffffffULL) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    // Fast path: bit reads only. A written range always takes the slow path (what the cheap call would need is in
    // the report of this change; the GPU-modified and current-write bookkeeping of ObtainBuffer is not cheap).
    bool all_placed = true;
    bool all_clean = true;
    ForWords(first, last, [&](u64 w, u64 mask) {
        if ((g_placed[w].load(std::memory_order_seq_cst) & mask) != mask) {
            all_placed = false;
        }
        if ((g_clean[w].load(std::memory_order_seq_cst) & mask) != mask) {
            all_clean = false;
        }
    });
    const bool eligible = !is_written && all_placed && all_clean;
    // SOTC_SHADER_ADDR=2 runs the slow path every time (so the verify measures the real path), and counts the fast
    // path it would have taken and whether the slow path wrote the entries the fast path would have kept.
    const bool verify = ShaderAddr::Verify();
    if (eligible && !verify) {
        g_fast.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (is_written) {
        g_slow_written.fetch_add(1, std::memory_order_relaxed);
        // A GPU write changes the bytes behind the pages: they are not clean any more (before the bind, so the
        // draw that follows cannot read them clean).
        ClearClean(addr, size);
    } else if (!eligible) {
        (all_placed ? g_slow_dirty : g_slow_unplaced).fetch_add(1, std::memory_order_relaxed);
    }
    const u64 seq_before = g_dirty_seq.load(std::memory_order_seq_cst); // s1
    // The buffer cache makes the range resident and synchronized, and serves ring-8 binds from their run. Called
    // without g_mutex: ObtainBuffer places blocks, and the placements call OnPlace.
    const auto [buffer, offset] = cache.ObtainBuffer(addr, static_cast<u32>(size), is_written);
    if (!buffer || !buffer->HasBufferDeviceAddress()) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const vk::DeviceAddress device = buffer->BufferDeviceAddress() + offset;
    bool clean_ok = false;
    {
        std::scoped_lock lock{g_mutex};
        const bool compare = verify && eligible;
        static thread_local std::vector<u64> before;
        if (compare) {
            before.assign(s->shadow.begin() + first, s->shadow.begin() + last + 1);
        }
        SetRangeLocked(*s, addr, size, device);
        FlushLocked(*s, scheduler);
        if (compare) {
            g_would_fast.fetch_add(1, std::memory_order_relaxed);
            if (!std::equal(before.begin(), before.end(), s->shadow.begin() + first)) {
                g_would_fast_bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
        clean_ok = !is_written && !AnyDirtyLocked(*s, first, last);
    }
    // A CPU bit left in the range after the sync (not taken by it) keeps the pages dirty.
    if (clean_ok && cache.IsRegionCpuModified(addr, size)) {
        clean_ok = false;
    }
    if (clean_ok) {
        ForWords(first, last, [](u64 w, u64 mask) {
            g_clean[w].fetch_or(mask, std::memory_order_seq_cst); // t
        });
        if (g_dirty_seq.load(std::memory_order_seq_cst) != seq_before) { // s2
            ForWords(first, last, [](u64 w, u64 mask) {
                g_clean[w].fetch_and(~mask, std::memory_order_seq_cst);
            });
            g_reverts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_served.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void DirtyRange(VAddr addr, u64 size) noexcept {
    ClearClean(addr, size);
}

void DirtyPages(VAddr base, u64 mask) noexcept {
    // Runs of set bits: one ClearClean per run of 4 KiB tracking pages.
    for (u64 i = 0; i < 64;) {
        if (((mask >> i) & 1) == 0) {
            ++i;
            continue;
        }
        u64 j = i;
        while (j < 64 && ((mask >> j) & 1) != 0) {
            ++j;
        }
        ClearClean(base + (i << 12), (j - i) << 12);
        i = j;
    }
}

void OnPlace(VAddr addr, u64 size, vk::DeviceAddress device_addr) {
    State* s = g_state.get();
    if (!s || size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return;
    }
    ClearClean(addr, size); // before the entries change: a fast read must not see a clean page with the old entry
    std::scoped_lock lock{g_mutex};
    SetRangeLocked(*s, addr, size, device_addr);
}

void OnRemove(VAddr addr, u64 size) {
    State* s = g_state.get();
    if (!s || size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return;
    }
    // Released blocks are whole blocks (multiples of the page), so every page of the range is cleared.
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    ClearClean(addr, size);
    std::scoped_lock lock{g_mutex};
    for (u64 page = first; page <= last; ++page) {
        SetEntryLocked(*s, page, 0);
    }
}

vk::DeviceAddress Translate(VAddr addr) {
    State* s = g_state.get();
    if (!s || addr < WindowBase || addr - WindowBase >= WindowSize) {
        return 0;
    }
    std::scoped_lock lock{g_mutex};
    const u64 entry = s->shadow[(addr - WindowBase) >> PageShift];
    return entry == 0 ? 0 : entry + (addr & (PageSize - 1));
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    State* s = g_state.get();
    if (!s) {
        out[0] = '\0';
        return;
    }
    u64 live{};
    {
        std::scoped_lock lock{g_mutex};
        live = s->live;
    }
    const u64 updates = g_updates.exchange(0, std::memory_order_relaxed);
    const u64 served = g_served.exchange(0, std::memory_order_relaxed);
    const u64 failed = g_failed.exchange(0, std::memory_order_relaxed);
    const u64 uploads = g_uploads.exchange(0, std::memory_order_relaxed);
    const u64 upload_bytes = g_upload_bytes.exchange(0, std::memory_order_relaxed);
    const u64 fast = g_fast.exchange(0, std::memory_order_relaxed);
    const u64 slow_unplaced = g_slow_unplaced.exchange(0, std::memory_order_relaxed);
    const u64 slow_dirty = g_slow_dirty.exchange(0, std::memory_order_relaxed);
    const u64 slow_written = g_slow_written.exchange(0, std::memory_order_relaxed);
    const u64 reverts = g_reverts.exchange(0, std::memory_order_relaxed);
    const u64 dirty_calls = g_dirty_calls.exchange(0, std::memory_order_relaxed);
    const u64 would_fast = g_would_fast.exchange(0, std::memory_order_relaxed);
    const u64 would_fast_bad = g_would_fast_bad.exchange(0, std::memory_order_relaxed);
    const int n = std::snprintf(
        out, size,
        "SOTC ADDR_TABLE: %llu entries live (%llu MiB of guest pages), table %llu MiB device + %llu MiB host "
        "shadow, %llu entry updates (%.0f/s), touches served %llu, failed %llu, %llu chunks uploaded (%llu KiB)",
        static_cast<unsigned long long>(live), static_cast<unsigned long long>((live * PageSize) >> 20),
        static_cast<unsigned long long>((NumPages * sizeof(u64)) >> 20),
        static_cast<unsigned long long>((NumPages * sizeof(u64)) >> 20),
        static_cast<unsigned long long>(updates), seconds > 0.0 ? updates / seconds : 0.0,
        static_cast<unsigned long long>(served), static_cast<unsigned long long>(failed),
        static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(upload_bytes >> 10));
    if (n < 0 || static_cast<u32>(n) >= size) {
        return;
    }
    std::snprintf(out + n, size - static_cast<u32>(n),
                  ". CLEAN: fast %llu, slow not placed %llu, slow dirty %llu, slow written %llu, reverts %llu, "
                  "dirty marks %llu (%.0f/s), SHADER_ADDR=2 would-be fast %llu (entries differ %llu)",
                  static_cast<unsigned long long>(fast), static_cast<unsigned long long>(slow_unplaced),
                  static_cast<unsigned long long>(slow_dirty), static_cast<unsigned long long>(slow_written),
                  static_cast<unsigned long long>(reverts), static_cast<unsigned long long>(dirty_calls),
                  seconds > 0.0 ? dirty_calls / seconds : 0.0, static_cast<unsigned long long>(would_fast),
                  static_cast<unsigned long long>(would_fast_bad));
}

} // namespace SotcArch::AddrTable
