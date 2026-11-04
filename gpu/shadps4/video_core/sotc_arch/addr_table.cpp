// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ADDR_TABLE (addr_table.h). The guest page table: a device-local buffer of u64 entries, a host shadow
// that the placements update, and dirty 4 KiB chunks uploaded on the upload command buffer (it runs before the
// commands of the submit that uses them).
#include <atomic>
#include <bit> // sotc: SOTC_GCN_SYNC (ForEachStalePlaced: countr_zero / countr_one)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <algorithm>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/gcn_native.h" // sotc: SOTC_GCN_MEM (Mode, Refuse)
#include "video_core/sotc_arch/shader_addr.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE (TracksGuestPages: the written fast path)

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
    // sotc: SOTC_ADDR_VERTEX. The buffer each entry lives in, host only: owner[page] is an index into owners (0: none,
    // the entry is 0). owners[0] is nullptr; a buffer gets its index the first time a placement names it.
    std::vector<u32> owner;              ///< guarded by g_mutex
    std::vector<const VideoCore::Buffer*> owners; ///< guarded by g_mutex
    std::unordered_map<const VideoCore::Buffer*, u32> owner_ids; ///< guarded by g_mutex
};

std::mutex g_mutex;
std::unique_ptr<State> g_state;

std::atomic<u64> g_updates{};        // entries written into the shadow
std::atomic<u64> g_served{};         // Touch calls that made their range resident and wrote its entries
std::atomic<u64> g_failed{};         // Touch calls refused, and chunks that could not be staged
std::atomic<u64> g_uploads{};        // dirty chunks copied to the table
std::atomic<u64> g_upload_bytes{};
std::atomic<u64> g_fast{};           // read Touch calls answered from the clean state (no ObtainBuffer)
std::atomic<u64> g_fast_written{};   // written Touch calls answered from the clean state (native, already GPU-modified)
std::atomic<u64> g_slow_unplaced{};  // slow read Touch calls: a page of the range was not placed
std::atomic<u64> g_slow_dirty{};     // slow read Touch calls: all placed, some page not clean
std::atomic<u64> g_slow_written{};   // slow written Touch calls, all reasons (the four below and verify)
std::atomic<u64> g_slow_w_untracked{}; // slow written: a tracked mode (readbacks), always the full path
std::atomic<u64> g_slow_w_unplaced{}; // slow written: a page of the range was not placed
std::atomic<u64> g_slow_w_dirty{};    // slow written: all placed, some page not clean
std::atomic<u64> g_slow_w_new{};      // slow written: native, placed and clean, but not yet GPU-modified (first write)
std::atomic<u64> g_slow_w_verify{};   // slow written: SOTC_SHADER_ADDR=2 runs the full path on a fast-ready bind
std::atomic<u64> g_reverts{};        // slow Touch calls whose clean bits a concurrent Dirty made stale
std::atomic<u64> g_dirty_calls{};    // ClearClean calls (Dirty, DirtyPages, OnPlace, OnRemove, written binds)
std::atomic<u64> g_would_fast{};     // SOTC_SHADER_ADDR=2: reads the fast path would have answered
std::atomic<u64> g_would_fast_bad{}; // SOTC_SHADER_ADDR=2: of those, the slow path wrote other entries

// sotc: SOTC_ADDR_VERTEX counters, per VertexPath (ObtainRead). table: answered by the table; fallback: ObtainBuffer ran
// (outside the window, a page not resident or of another owner, a refused Touch); verify_*: mode 2's comparison.
constexpr size_t kVertexPaths = static_cast<size_t>(VertexPath::Count);
std::atomic<u64> g_vx_table[kVertexPaths]{};
std::atomic<u64> g_vx_fallback[kVertexPaths]{};
std::atomic<u64> g_vx_verify_match[kVertexPaths]{};
std::atomic<u64> g_vx_verify_bad[kVertexPaths]{};

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

// The owner index of buffer (g_mutex held). 0 for nullptr; a new buffer is appended to the registry.
u32 OwnerIdLocked(State& s, const VideoCore::Buffer* buffer) {
    if (!buffer) {
        return 0;
    }
    const auto [it, inserted] = s.owner_ids.try_emplace(buffer, static_cast<u32>(s.owners.size()));
    if (inserted) {
        s.owners.push_back(buffer);
    }
    return it->second;
}

// Writes one entry (g_mutex held). The placed bit follows the entry: set when it is not 0, cleared when it is.
// owner: the buffer the entry lives in (0 when value is 0).
void SetEntryLocked(State& s, u64 page, u64 value, u32 owner) {
    s.owner[page] = value == 0 ? 0 : owner;
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
// shader adds the offset in the page back, so the sum is exact). owner: the buffer that holds the range.
void SetRangeLocked(State& s, VAddr addr, u64 size, vk::DeviceAddress device, u32 owner) {
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    for (u64 page = first; page <= last; ++page) {
        const VAddr page_start = WindowBase + (page << PageShift);
        SetEntryLocked(s, page, device + (page_start - addr), owner);
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

namespace {

// sotc: SOTC_GCN_MEM. The table was asked for by its own two switches (the run that always refused by exit). A run that
// asked for it through SOTC_GCN_MEM alone refuses that stage instead, and goes on without the table.
bool TableAsked() {
    static const bool asked = SwitchMode("SOTC_ADDR_TABLE") != 0 && SwitchMode("SOTC_SHADER_ADDR") != 0;
    return asked;
}

// sotc: an Init failure. Exits when the table was asked for by its own switches; otherwise refuses SOTC_GCN_MEM only.
void InitFailed(const char* reason) {
    // SOTC_SHADER_ADDR with GCN_MEM is mode 3 (the table): without the table the shader would address the mirror window,
    // which this run does not create. Exit, like the table's own switches.
    if (TableAsked() || SwitchMode("SOTC_SHADER_ADDR") != 0) {
        Refuse(reason);
    }
    SotcGcn::Refuse(SotcGcn::Stage::Mem, reason);
}

} // namespace

bool On() {
    static const bool table = TableAsked();
    return table || SotcGcn::Mode(SotcGcn::Stage::Mem) != 0; // sotc: SOTC_GCN_MEM (read on every call, one load)
}

void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    if (!On() || g_state) {
        return;
    }
    if (!instance.SotcBufferDeviceAddress()) {
        InitFailed("the device has no buffer device address");
        return;
    }
    const vk::Device device = instance.GetDevice();
    const u64 bytes = NumPages * sizeof(u64);
    auto [created, buffer] = device.createBufferUnique(vk::BufferCreateInfo{
        .size = bytes,
        .usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (created != vk::Result::eSuccess) {
        InitFailed("the table buffer could not be created");
        return;
    }
    const vk::MemoryRequirements req = device.getBufferMemoryRequirements(*buffer);
    const u32 type = FindDeviceLocalType(instance.GetMemoryProperties(), req.memoryTypeBits);
    if (type == instance.GetMemoryProperties().memoryTypeCount) {
        InitFailed("no memory type for the table buffer");
        return;
    }
    // A buffer with a device address needs its memory allocated with the device address flag.
    const vk::StructureChain alloc_chain = {
        vk::MemoryAllocateInfo{.allocationSize = req.size, .memoryTypeIndex = type},
        vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress},
    };
    auto [alloc, memory] = device.allocateMemoryUnique(alloc_chain.get<vk::MemoryAllocateInfo>());
    if (alloc != vk::Result::eSuccess) {
        InitFailed("the table memory could not be allocated");
        return;
    }
    if (device.bindBufferMemory(*buffer, *memory, 0) != vk::Result::eSuccess) {
        InitFailed("the table memory could not be bound");
        return;
    }

    auto state = std::make_unique<State>();
    state->base = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *buffer});
    state->shadow.assign(NumPages, 0);
    state->dirty.assign(kNumChunks, 0);
    state->owner.assign(NumPages, 0);
    state->owners.push_back(nullptr); // index 0: no owner
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

// What "clean" means. A clean page has no guest byte changed since its last sync without that sync taking the
// change, and its device entries are the shadow's. It is not "the bytes equal guest memory": in native mode
// (SOTC_NATIVE=1) a GPU write never copies its bytes to guest memory (DownloadMemory returns at once) and never
// touches an entry, so a GPU write does not make a page dirty there. In the tracked modes a GPU write does make
// the guest bytes stale (readbacks copy GPU bytes into guest memory later), so a written bind clears the bits
// there, before its GPU write.
//
// Why a page that Touch marks clean cannot be stale (the readers are Touch's fast paths and the slow path's set; the
// writers are Dirty / DirtyPages, OnPlace, OnRemove, NoteCpuBytesChanged, and the tracked-mode written binds). All
// bitmap and sequence accesses are seq_cst, so they have one total order S.
//  1. A change of guest bytes or of the device entries clears the bits, and the clear comes after the change in the
//     writer's program order: a CPU bit rises in UpdateStateAndProtection under the region mutex, and the guest's own
//     write goes through only after the page is unprotected (after that call); the announce drain runs on the draw
//     thread before the binds of its submit; NoteCpuBytesChanged runs after its copy; OnPlace/OnRemove change the
//     entries; a tracked-mode written bind clears before its GPU write (which the draw orders after it).
//     A native written bind changes neither guest bytes nor entries, so it needs no clear.
//  2. ClearClean bumps g_dirty_seq (b) BEFORE it clears the bits (c).
//  3. A slow Touch (read, or native written) takes s1 = g_dirty_seq, then the sync (its copy of the bytes), then
//     sets its bits (t), then takes s2 = g_dirty_seq; when s2 != s1 it clears its bits again.
//  4. Race: a CPU write W that the sync's copy did not see. Its change is after the copy, so b is after s1 in S.
//     If c precedes t, then b precedes t and s2 comes after t, so s2 sees b and the bits are cleared again. If c
//     follows t, the bits are clear after c. Either way the bits are clear after Touch returns, unless no W happened.
//  5. The sync leaves a page with a CPU bit only when it did not take it; Touch then does not set the bits (the
//     IsRegionCpuModified check). A device entry that a failed chunk upload has not reached (AnyDirtyLocked) also
//     blocks the set, so a clean page always has its shadow entries on the device.
//  6. A fast written Touch (native, placed, clean, range already GPU-modified: WrittenAlreadyGpuModified) runs no
//     sync and sets no bit: it needs none, because the range has no CPU bit (clean) and its GPU-modified state is
//     already recorded (tracker bits and gpu_modified_ranges, both checked). Its later effects are NoteWrittenFast's
//     and the BindBuffers skip branch's mem_epoch bump, which a fast read does not need.
// So a page is clean only while no unsynced guest change and no entry change has happened to it since its last
// sync, and a bind that finds all its pages placed and clean has nothing to upload.
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
    // Fast paths: bit reads only. A read with all pages placed and clean has nothing to upload. A written range
    // with all pages placed and clean is fast in native mode when the range is already GPU-modified: it changes
    // no guest byte and no entry, and adds no GPU-modified state (WrittenAlreadyGpuModified), so only the bookkeeping
    // NoteWrittenFast keeps runs. Every other written bind takes the full ObtainBuffer path.
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
    const bool placed_clean = all_placed && all_clean;
    const bool native = !SotcNative::TracksGuestPages(); // SOTC_NATIVE=1
    const bool read_fast = !is_written && placed_clean;
    const bool written_fast = is_written && native && placed_clean && cache.WrittenAlreadyGpuModified(addr, size);
    // SOTC_SHADER_ADDR=2 runs the slow path every time (so the verify measures the real path), and counts the fast
    // path it would have taken and whether the slow path wrote the entries the fast path would have kept.
    const bool verify = ShaderAddr::Verify();
    if ((read_fast || written_fast) && !verify) {
        // A fast Touch makes no residency check, so it stamps the blocks as one does (the eviction of an
        // idle block must not take a block this bind reads; ObtainBuffer's memo hit does the same).
        cache.NoteBindUse(addr, size);
        if (is_written) {
            cache.NoteWrittenFast(addr, size);
            g_fast_written.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_fast.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    if (is_written) {
        g_slow_written.fetch_add(1, std::memory_order_relaxed);
        if (verify && written_fast) {
            g_slow_w_verify.fetch_add(1, std::memory_order_relaxed);
        } else if (!native) {
            g_slow_w_untracked.fetch_add(1, std::memory_order_relaxed);
        } else if (!all_placed) {
            g_slow_w_unplaced.fetch_add(1, std::memory_order_relaxed);
        } else if (!all_clean) {
            g_slow_w_dirty.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_slow_w_new.fetch_add(1, std::memory_order_relaxed);
        }
        if (!native) {
            // Tracked modes: a GPU write makes the guest bytes stale (readbacks), so the bits go before the draw.
            ClearClean(addr, size);
        }
    } else if (!read_fast) {
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
        const bool compare = verify && (read_fast || written_fast);
        static thread_local std::vector<u64> before;
        if (compare) {
            before.assign(s->shadow.begin() + first, s->shadow.begin() + last + 1);
        }
        SetRangeLocked(*s, addr, size, device, OwnerIdLocked(*s, buffer));
        FlushLocked(*s, scheduler);
        if (compare) {
            g_would_fast.fetch_add(1, std::memory_order_relaxed);
            if (!std::equal(before.begin(), before.end(), s->shadow.begin() + first)) {
                g_would_fast_bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // A native written sync is a read sync for the bits: it takes the CPU bits, sets the GPU bits, and changes
        // no guest byte. A tracked-mode written bind leaves the bits clear (its GPU bytes are not the guest's).
        clean_ok = (native || !is_written) && !AnyDirtyLocked(*s, first, last);
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

void OnPlace(VAddr addr, u64 size, vk::DeviceAddress device_addr, const VideoCore::Buffer* owner) {
    State* s = g_state.get();
    if (!s || size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return;
    }
    if (device_addr == 0) {
        // sotc: SOTC_GCN_MEM: no device address (no BDA on the run): an entry of 0 + offset would point at garbage.
        OnRemove(addr, size);
        return;
    }
    ClearClean(addr, size); // before the entries change: a fast read must not see a clean page with the old entry
    std::scoped_lock lock{g_mutex};
    SetRangeLocked(*s, addr, size, device_addr, OwnerIdLocked(*s, owner));
    // sotc: SOTC_GCN_SYNC. A second clear after the change: a MarkClean whose sequence read came before the first clear
    // must still see the sequence move, or see the dirty entries under g_mutex (see the proof at Touch, step 1).
    if (SotcGcn::Sync::On()) {
        ClearClean(addr, size);
    }
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
        SetEntryLocked(*s, page, 0, 0);
    }
    if (SotcGcn::Sync::On()) { // sotc: SOTC_GCN_SYNC, the second clear after the change (see OnPlace)
        ClearClean(addr, size);
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

// sotc: SOTC_GCN_MEM / SOTC_GCN_SYNC (addr_table.h). Lock-free: the bit words only.

bool AllPlaced(VAddr addr, u64 size) noexcept {
    if (size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return false;
    }
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    bool all = true;
    ForWords(first, last, [&](u64 w, u64 mask) {
        if (all && (g_placed[w].load(std::memory_order_seq_cst) & mask) != mask) {
            all = false;
        }
    });
    return all;
}

void ForEachStalePlaced(void (*fn)(void* user, VAddr addr, u64 size), void* user) {
    if (!fn) {
        return;
    }
    const auto emit = [fn, user](u64 begin, u64 end) {
        fn(user, WindowBase + (begin << PageShift), (end - begin) << PageShift);
    };
    // A run that reaches bit 63 of a word may continue at bit 0 of the next one: it stays open until a word breaks it.
    bool open = false;
    u64 begin = 0; // first page of the open or current run
    for (u64 w = 0; w < kWords; ++w) {
        const u64 placed = g_placed[w].load(std::memory_order_seq_cst);
        const u64 stale = placed == 0 ? 0 : placed & ~g_clean[w].load(std::memory_order_seq_cst);
        if (open && (stale & 1) == 0) {
            emit(begin, w * 64); // the run ended at the boundary
            open = false;
        }
        if (stale == 0) {
            continue; // the common case: nothing stale in these 64 pages
        }
        u64 bit = 0;
        while (bit < 64) {
            const u64 rest = stale >> bit;
            if (rest == 0) {
                break;
            }
            const u64 start = bit + static_cast<u64>(std::countr_zero(rest));
            const u64 end = start + static_cast<u64>(std::countr_one(stale >> start)); // <= 64
            if (!(start == 0 && open)) {
                begin = w * 64 + start; // a new run (an open one continues at bit 0)
            }
            if (end == 64) {
                open = true;
            } else {
                emit(begin, w * 64 + end);
                open = false;
            }
            bit = end;
        }
    }
    if (open) {
        emit(begin, NumPages);
    }
}

u64 CleanSequence() noexcept {
    return g_dirty_seq.load(std::memory_order_seq_cst);
}

bool MarkClean(VAddr addr, u64 size, u64 sequence) noexcept {
    State* s = g_state.get();
    if (!s || !AllPlaced(addr, size)) {
        return false;
    }
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    {
        // An entry the device does not hold yet (a pending chunk upload): the pages stay stale.
        std::scoped_lock lock{g_mutex};
        if (AnyDirtyLocked(*s, first, last)) {
            return false;
        }
    }
    // The Touch protocol (proof at Touch): set the bits (t), then a sequence change since `sequence` clears them again.
    ForWords(first, last, [](u64 w, u64 mask) {
        g_clean[w].fetch_or(mask, std::memory_order_seq_cst);
    });
    if (g_dirty_seq.load(std::memory_order_seq_cst) != sequence) {
        ForWords(first, last, [](u64 w, u64 mask) {
            g_clean[w].fetch_and(~mask, std::memory_order_seq_cst);
        });
        g_reverts.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// sotc: SOTC_ADDR_VERTEX (see addr_table.h). The mode is read once, like On().
bool VertexOn() {
    static const bool on = On() && SwitchMode("SOTC_ADDR_VERTEX") != 0;
    return on;
}

namespace {
bool VertexVerify() {
    static const bool verify = SwitchMode("SOTC_ADDR_VERTEX") == 2;
    return verify;
}
} // namespace

bool Backing(VAddr addr, u64 size, const VideoCore::Buffer*& buffer, u64& offset) {
    State* s = g_state.get();
    if (!s || size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return false;
    }
    const u64 first = (addr - WindowBase) >> PageShift;
    const u64 last = (addr - WindowBase + size - 1) >> PageShift;
    std::scoped_lock lock{g_mutex};
    const u64 first_entry = s->shadow[first];
    const u32 id = s->owner[first];
    if (first_entry == 0 || id == 0) {
        return false;
    }
    // One backing: every page has the same owner, and the entries are linear (page p at first_entry + (p - first) * 16K).
    for (u64 page = first; page <= last; ++page) {
        if (s->owner[page] != id || s->shadow[page] != first_entry + ((page - first) << PageShift)) {
            return false;
        }
    }
    const VideoCore::Buffer* owner = s->owners[id];
    if (!owner || !owner->HasBufferDeviceAddress()) {
        return false;
    }
    // The device address of byte addr: the first page's entry plus the byte's offset in its page (see Translate).
    const vk::DeviceAddress device = first_entry + (addr & (PageSize - 1));
    const vk::DeviceAddress base = owner->BufferDeviceAddress();
    if (device < base || device - base > owner->SizeBytes() || size > owner->SizeBytes() - (device - base)) {
        return false;
    }
    buffer = owner;
    offset = device - base;
    return true;
}

std::pair<const VideoCore::Buffer*, u64> ObtainRead(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler,
                                                    VAddr addr, u32 size, VertexPath path) {
    if (!VertexOn() || size == 0) {
        return cache.ObtainBuffer(addr, size, false);
    }
    const size_t i = static_cast<size_t>(path);
    // Outside the window the table holds nothing: the old path, counted here (Touch would count it as refused).
    if (addr < WindowBase || addr - WindowBase > WindowSize - size) {
        g_vx_fallback[i].fetch_add(1, std::memory_order_relaxed);
        return cache.ObtainBuffer(addr, size, false);
    }
    const VideoCore::Buffer* buffer = nullptr;
    u64 offset = 0;
    const bool answered = Touch(cache, scheduler, addr, size, false) && Backing(addr, size, buffer, offset);
    if (answered) {
        g_vx_table[i].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_vx_fallback[i].fetch_add(1, std::memory_order_relaxed);
    }
    if (!VertexVerify()) {
        return answered ? std::pair{buffer, offset} : cache.ObtainBuffer(addr, size, false);
    }
    // Mode 2: the old path runs too; its result is the one the draw uses, and the table's answer is compared with it.
    const auto old = cache.ObtainBuffer(addr, size, false);
    if (answered) {
        if (buffer == old.first && offset == old.second) {
            g_vx_verify_match[i].fetch_add(1, std::memory_order_relaxed);
        } else {
            g_vx_verify_bad[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    return old;
}

void ReportVertex(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    static const char* const kNames[kVertexPaths] = {"vertex", "index", "indirect", "indirect count",
                                                     "dispatch indirect"};
    int used = std::snprintf(out, size, "SOTC ADDR_VERTEX (table / fallback):");
    for (size_t i = 0; i < kVertexPaths; ++i) {
        if (used < 0 || static_cast<u32>(used) + 1 >= size) {
            break; // the line is full: the rest is cut
        }
        const u64 table = g_vx_table[i].exchange(0, std::memory_order_relaxed);
        const u64 fallback = g_vx_fallback[i].exchange(0, std::memory_order_relaxed);
        const u64 match = g_vx_verify_match[i].exchange(0, std::memory_order_relaxed);
        const u64 bad = g_vx_verify_bad[i].exchange(0, std::memory_order_relaxed);
        const u32 left = size - static_cast<u32>(used);
        used += VertexVerify()
                    ? std::snprintf(out + used, left, " %s %llu/%llu [match %llu, differ %llu]", kNames[i],
                                    static_cast<unsigned long long>(table),
                                    static_cast<unsigned long long>(fallback),
                                    static_cast<unsigned long long>(match),
                                    static_cast<unsigned long long>(bad))
                    : std::snprintf(out + used, left, " %s %llu/%llu", kNames[i],
                                    static_cast<unsigned long long>(table),
                                    static_cast<unsigned long long>(fallback));
    }
    if (used >= 0 && static_cast<u32>(used) + 1 < size) {
        std::snprintf(out + used, size - static_cast<u32>(used), " over %.1f s", seconds);
    }
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
    const u64 fast_written = g_fast_written.exchange(0, std::memory_order_relaxed);
    const u64 w_untracked = g_slow_w_untracked.exchange(0, std::memory_order_relaxed);
    const u64 w_unplaced = g_slow_w_unplaced.exchange(0, std::memory_order_relaxed);
    const u64 w_dirty = g_slow_w_dirty.exchange(0, std::memory_order_relaxed);
    const u64 w_new = g_slow_w_new.exchange(0, std::memory_order_relaxed);
    const u64 w_verify = g_slow_w_verify.exchange(0, std::memory_order_relaxed);
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
                  ". CLEAN: fast %llu, slow not placed %llu, slow dirty %llu, reverts %llu, "
                  "dirty marks %llu (%.0f/s), written fast %llu, written slow %llu (untracked %llu, not placed %llu, "
                  "dirty %llu, first %llu, SHADER_ADDR=2 %llu), SHADER_ADDR=2 would-be fast %llu (entries differ %llu)",
                  static_cast<unsigned long long>(fast), static_cast<unsigned long long>(slow_unplaced),
                  static_cast<unsigned long long>(slow_dirty), static_cast<unsigned long long>(reverts),
                  static_cast<unsigned long long>(dirty_calls), seconds > 0.0 ? dirty_calls / seconds : 0.0,
                  static_cast<unsigned long long>(fast_written), static_cast<unsigned long long>(slow_written),
                  static_cast<unsigned long long>(w_untracked), static_cast<unsigned long long>(w_unplaced),
                  static_cast<unsigned long long>(w_dirty), static_cast<unsigned long long>(w_new),
                  static_cast<unsigned long long>(w_verify), static_cast<unsigned long long>(would_fast),
                  static_cast<unsigned long long>(would_fast_bad));
}

} // namespace SotcArch::AddrTable
