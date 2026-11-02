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

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::AddrTable {

namespace {

constexpr u64 kChunkEntries = 512; ///< entries per dirty chunk: 4 KiB of table
constexpr u64 kChunkBytes = kChunkEntries * sizeof(u64);
constexpr u64 kNumChunks = NumPages / kChunkEntries;
constexpr u64 kStreamSize = 8ULL << 20; ///< staging of the chunk uploads (a Map of one chunk each)

struct State {
    vk::UniqueBuffer buffer;             ///< the table: NumPages u64 entries, device-local, with a device address
    vk::UniqueDeviceMemory memory;
    vk::DeviceAddress base{};
    std::unique_ptr<VideoCore::StreamBuffer> stream;
    std::vector<u64> shadow;             ///< the host copy of the entries (guarded by g_mutex)
    std::vector<u8> dirty;               ///< one flag per chunk (guarded by g_mutex)
    u64 live{};                          ///< nonzero entries (guarded by g_mutex)
};

std::mutex g_mutex;
std::unique_ptr<State> g_state;

std::atomic<u64> g_updates{};        // entries written into the shadow
std::atomic<u64> g_served{};         // Touch calls that made their range resident and wrote its entries
std::atomic<u64> g_failed{};         // Touch calls refused, and chunks that could not be staged
std::atomic<u64> g_uploads{};        // dirty chunks copied to the table
std::atomic<u64> g_upload_bytes{};

// sotc: refusal at Init, like ShaderAddr::Refuse: a run that goes on would read the table as zero.
[[noreturn]] void Refuse(const char* reason) {
    std::fprintf(stderr, "SOTC ADDR_TABLE refused: %s\n", reason);
    std::exit(EXIT_FAILURE);
}

// Writes one entry (g_mutex held).
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
    s.dirty[page / kChunkEntries] = 1;
    g_updates.fetch_add(1, std::memory_order_relaxed);
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
    bool recorded = false;
    for (u64 chunk = 0; chunk < kNumChunks; ++chunk) {
        if (!s.dirty[chunk]) {
            continue;
        }
        auto [dst, src_offset] = s.stream->Map(kChunkBytes);
        if (dst == nullptr) {
            g_failed.fetch_add(1, std::memory_order_relaxed);
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
}

vk::DeviceAddress Base() {
    return g_state ? g_state->base : 0;
}

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
    // The buffer cache makes the range resident and synchronized, and serves ring-8 binds from their run. Called
    // without g_mutex: ObtainBuffer places blocks, and the placements call OnPlace.
    const auto [buffer, offset] = cache.ObtainBuffer(addr, static_cast<u32>(size), is_written);
    if (!buffer || !buffer->HasBufferDeviceAddress()) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const vk::DeviceAddress device = buffer->BufferDeviceAddress() + offset;
    {
        std::scoped_lock lock{g_mutex};
        SetRangeLocked(*s, addr, size, device);
        FlushLocked(*s, scheduler);
    }
    g_served.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void OnPlace(VAddr addr, u64 size, vk::DeviceAddress device_addr) {
    State* s = g_state.get();
    if (!s || size == 0 || addr < WindowBase || addr - WindowBase > WindowSize - size) {
        return;
    }
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
    std::snprintf(out, size,
                  "SOTC ADDR_TABLE: %llu entries live (%llu MiB of guest pages), table %llu MiB device + %llu MiB host "
                  "shadow, %llu entry updates (%.0f/s), touches served %llu, failed %llu, %llu chunks uploaded (%llu KiB)",
                  static_cast<unsigned long long>(live),
                  static_cast<unsigned long long>((live * PageSize) >> 20),
                  static_cast<unsigned long long>((NumPages * sizeof(u64)) >> 20),
                  static_cast<unsigned long long>((NumPages * sizeof(u64)) >> 20),
                  static_cast<unsigned long long>(updates), seconds > 0.0 ? updates / seconds : 0.0,
                  static_cast<unsigned long long>(served), static_cast<unsigned long long>(failed),
                  static_cast<unsigned long long>(uploads),
                  static_cast<unsigned long long>(upload_bytes >> 10));
}

} // namespace SotcArch::AddrTable
