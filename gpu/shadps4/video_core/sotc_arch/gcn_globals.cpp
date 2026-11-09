// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: ARCH-GCN-NATIVE S2/S3 host side of the globals buffer (gcn_native.h, namespace SotcGcn::Globals; layout in
// shader_recompiler/gcn_layout.h). One host-visible, persistently mapped buffer with a device address: the page table
// pointer, the miss ring the shaders append to, the seen bitmap that dedups it, and the T# table.
#include <atomic>
#include <cstdio>
#include <cstring>

#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/gcn_native.h"

namespace SotcGcn::Globals {
namespace {

using Shader::Gcn::GlobalsHeader;
using Shader::Gcn::MissRecord;
using Shader::Gcn::MissRingHeader;

struct State {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    u8* mapped{};
    vk::DeviceAddress address{};
    u32 read_index{}; // draw thread only (DrainMisses)
};

// Process lifetime, never destroyed (the device may be gone before static destructors run; see table_memo.cpp).
State* g_state{};

std::atomic<u64> g_page{};      // page misses drained (kMissPage)
std::atomic<u64> g_image{};     // image misses drained (kMissImage)
std::atomic<u64> g_sampler{};   // sampler misses drained (kMissSampler)
std::atomic<u64> g_overflows{}; // records the shaders wrote that the host never saw (ring lapped)
// sotc: cumulative totals since start (never reset by Report), and the last drain's view of the ring.
std::atomic<u64> g_page_total{};
std::atomic<u64> g_image_total{};
std::atomic<u64> g_sampler_total{};
std::atomic<u64> g_overflow_total{};
std::atomic<u64> g_pending_stops{}; // drains that stopped at a slot whose stamp was not there yet (record in flight)
std::atomic<u32> g_last_write{};    // write_index seen by the last drain
std::atomic<u32> g_last_read{};     // read_index after the last drain

// The shaders write these words and the host reads them (and the reverse for the header's page table pointer):
// volatile accesses keep the compiler from caching or merging them across the polling loop.
u32 LoadWord(const u32* p) {
    return *static_cast<const volatile u32*>(p);
}

// sotc: an acquire load of a word the shaders write atomically (write_index, a record's stamp): the reads after it
// (the record body) are not moved before it. Pairs with the shader's Release stamp store (gcn_layout.h).
u32 LoadAcquire(const u32* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

// The header, filled in full at creation; page_table_address alone is refreshed later (Address).
void FillHeader(State& s) {
    auto* header = reinterpret_cast<GlobalsHeader*>(s.mapped);
    header->page_table_address = SotcArch::AddrTable::Base();
    header->page_table_guest_base = SotcArch::AddrTable::WindowBase;
    header->page_table_size = SotcArch::AddrTable::WindowSize;
    header->miss_ring_offset = Shader::Gcn::kMissRingOffset;
    header->seen_offset = Shader::Gcn::kSeenOffset;
    header->tex_table_offset = Shader::Gcn::kTexTableOffset;
    header->tex_table_mask = Shader::Gcn::kTexTableCapacity - 1;
    header->null_image_slot = 0;
    header->flags = 1; // bit 0: misses are recorded
}

} // namespace

void Init(const Vulkan::Instance& instance) {
    const bool buf = Mode(Stage::Buf) != 0;
    const bool tex = Mode(Stage::Tex) != 0;
    if ((!buf && !tex) || g_state) {
        return;
    }
    const auto refuse = [buf, tex](const char* reason) {
        // sotc: diagnostic (stderr, once): LOG_INFO is filtered out of the session log.
        std::fprintf(stderr, "SOTC GCN GLOBALS: refused (buf %s, tex %s): %s\n", buf ? "on" : "off",
                     tex ? "on" : "off", reason);
        if (buf) {
            Refuse(Stage::Buf, reason);
        }
        if (tex) {
            Refuse(Stage::Tex, reason);
        }
    };
    if (!instance.SotcBufferDeviceAddress()) {
        refuse("the device has no buffer device address");
        return;
    }

    const vk::Device device = instance.GetDevice();
    constexpr u64 bytes = Shader::Gcn::kGlobalsSize;
    auto [created, buffer] = device.createBufferUnique(vk::BufferCreateInfo{
        .size = bytes,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                 vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (created != vk::Result::eSuccess) {
        refuse("the globals buffer could not be created");
        return;
    }

    // Host-visible and host-coherent (the host writes entries and reads the ring with no flush). Device-local too
    // when the device has it (Resizable BAR): the GPU reads the table at device-memory speed. Otherwise plain host.
    const vk::MemoryRequirements req = device.getBufferMemoryRequirements(*buffer);
    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();
    const vk::MemoryPropertyFlags host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = props.memoryTypeCount;
    for (u32 pass = 0; pass < 2 && type == props.memoryTypeCount; ++pass) {
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) == 0) {
                continue;
            }
            const vk::MemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
            if ((flags & host) != host) {
                continue;
            }
            if (pass == 0 && (flags & vk::MemoryPropertyFlagBits::eDeviceLocal) == vk::MemoryPropertyFlags{}) {
                continue;
            }
            type = i;
            break;
        }
    }
    if (type == props.memoryTypeCount) {
        refuse("no host-visible coherent memory type for the globals buffer");
        return;
    }

    // A buffer with a device address needs its memory allocated with the device address flag.
    const vk::StructureChain alloc_chain = {
        vk::MemoryAllocateInfo{.allocationSize = req.size, .memoryTypeIndex = type},
        vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress},
    };
    auto [alloc, memory] = device.allocateMemoryUnique(alloc_chain.get<vk::MemoryAllocateInfo>());
    if (alloc != vk::Result::eSuccess) {
        refuse("the globals memory could not be allocated");
        return;
    }
    if (device.bindBufferMemory(*buffer, *memory, 0) != vk::Result::eSuccess) {
        refuse("the globals memory could not be bound");
        return;
    }
    const auto [map_result, mapped] = device.mapMemory(*memory, 0, bytes);
    if (map_result != vk::Result::eSuccess) {
        refuse("the globals memory could not be mapped");
        return;
    }

    auto* state = new State; // process lifetime (see g_state)
    state->buffer = std::move(buffer);
    state->memory = std::move(memory);
    state->mapped = static_cast<u8*>(mapped);
    state->address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *state->buffer});
    std::memset(state->mapped, 0, bytes);
    FillHeader(*state);
    g_state = state;

    LOG_INFO(Render, "SOTC GCN GLOBALS: {} KiB at {:#x} (buffer {}, table {})", bytes >> 10,
             static_cast<u64>(state->address), buf ? "on" : "off", tex ? "on" : "off");
    // sotc: diagnostic (stderr, once): the address the packet carries as word 88.
    std::fprintf(stderr, "SOTC GCN GLOBALS: created, address %#llx, %llu bytes (buf %s, tex %s)\n",
                 static_cast<unsigned long long>(state->address), static_cast<unsigned long long>(bytes),
                 buf ? "on" : "off", tex ? "on" : "off");
}

vk::DeviceAddress Address() {
    State* s = g_state;
    if (!s) {
        return 0;
    }
    // The table may come up after this buffer: the header takes its base once it has one.
    auto* header = reinterpret_cast<GlobalsHeader*>(s->mapped);
    if (header->page_table_address == 0) {
        header->page_table_address = SotcArch::AddrTable::Base();
    }
    return s->address;
}

u8* Mapped() {
    return g_state ? g_state->mapped : nullptr;
}

void DrainMisses(const MissSink& sink) {
    State* s = g_state;
    if (!s) {
        return;
    }
    u8* base = s->mapped;
    auto* ring_header = reinterpret_cast<MissRingHeader*>(base + Shader::Gcn::kMissRingOffset);
    auto* ring = reinterpret_cast<MissRecord*>(base + Shader::Gcn::kMissRingOffset + sizeof(MissRingHeader));
    constexpr u32 kMask = Shader::Gcn::kMissRingCapacity - 1;

    // A snapshot of the producer's index. Records below it may still be in flight: their stamp is not there yet and
    // the loop stops there, so a later drain takes them.
    const u32 write = LoadAcquire(&ring_header->write_index); // sotc: acquire (was a volatile load + fence)
    if (write - s->read_index > Shader::Gcn::kMissRingCapacity) {
        // The ring lapped the host: the records in between are gone. Count them and skip to the oldest one kept.
        const u32 lost = write - s->read_index - Shader::Gcn::kMissRingCapacity;
        g_overflows.fetch_add(lost, std::memory_order_relaxed);
        g_overflow_total.fetch_add(lost, std::memory_order_relaxed);
        s->read_index = write - Shader::Gcn::kMissRingCapacity;
    }

    for (u32 i = s->read_index; i != write; ++i) {
        const MissRecord* rec = &ring[i & kMask];
        if (LoadAcquire(&rec->stamp) != i + 1) { // sotc: acquire, pairs with the shader's Release stamp store
            g_pending_stops.fetch_add(1, std::memory_order_relaxed);
            break; // not written yet (incomplete): the next drain retries from here
        }
        // Seqlock-style read: the body is valid only when the stamp is the same before and after it.
        std::atomic_thread_fence(std::memory_order_acquire);
        const u32 kind = LoadWord(&rec->kind);
        const u32 view_class = LoadWord(&rec->view_class);
        u32 words[8];
        for (u32 k = 0; k < 8; ++k) {
            words[k] = LoadWord(&rec->words[k]);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (LoadWord(&rec->stamp) != i + 1) {
            break; // the slot was reused while we read it: the ring lapped us, the next drain skips ahead
        }

        switch (kind) {
        case Shader::Gcn::kMissPage:
            g_page.fetch_add(1, std::memory_order_relaxed);
            g_page_total.fetch_add(1, std::memory_order_relaxed);
            if (sink.page) {
                const VAddr page = static_cast<VAddr>(words[0]) | (static_cast<VAddr>(words[1]) << 32);
                // sotc: view_class holds the page flags, words[2..4] the access's V# (gcn_layout.h).
                const VAddr base = static_cast<VAddr>(words[2]) | (static_cast<VAddr>(words[3]) << 32);
                sink.page(sink.user, page, view_class, base, words[4]);
            }
            break;
        case Shader::Gcn::kMissImage:
            g_image.fetch_add(1, std::memory_order_relaxed);
            g_image_total.fetch_add(1, std::memory_order_relaxed);
            if (sink.image) {
                sink.image(sink.user, words, view_class);
            }
            break;
        case Shader::Gcn::kMissSampler: {
            g_sampler.fetch_add(1, std::memory_order_relaxed);
            g_sampler_total.fetch_add(1, std::memory_order_relaxed);
            if (sink.sampler) {
                const u32 ssharp[4] = {words[0], words[1], words[2], words[3]};
                sink.sampler(sink.user, ssharp, view_class);
            }
            break;
        }
        default:
            break; // kMissNone or an unknown kind: nothing to resolve
        }
        s->read_index = i + 1;
    }
    g_last_write.store(write, std::memory_order_relaxed); // sotc: for the report (the ring's state, not a rate)
    g_last_read.store(s->read_index, std::memory_order_relaxed);

    // Dedup starts over for the next submit. Records of work still in flight are re-appended if they miss again.
    std::memset(base + Shader::Gcn::kSeenOffset, 0, Shader::Gcn::kSeenBits / 8);
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    if (!g_state) {
        out[0] = '\0';
        return;
    }
    const u64 page = g_page.exchange(0, std::memory_order_relaxed);
    const u64 image = g_image.exchange(0, std::memory_order_relaxed);
    const u64 sampler = g_sampler.exchange(0, std::memory_order_relaxed);
    const u64 overflows = g_overflows.exchange(0, std::memory_order_relaxed);
    // sotc: the rates are per window; the totals are since start, and write/read index are the ring as the last drain
    // saw it (write_index read live from the mapping too), so a window with no drained record is not "no misses".
    const u32 live_write = LoadAcquire(&reinterpret_cast<const MissRingHeader*>(
                                            g_state->mapped + Shader::Gcn::kMissRingOffset)->write_index);
    std::snprintf(out, size,
                  "SOTC GCN GLOBALS: misses page %.1f/s image %.1f/s sampler %.1f/s, overflows %llu; totals page %llu "
                  "image %llu sampler %llu overflows %llu, in-flight stops %llu, ring write_index %u (live %u) "
                  "read_index %u",
                  seconds > 0.0 ? page / seconds : 0.0, seconds > 0.0 ? image / seconds : 0.0,
                  seconds > 0.0 ? sampler / seconds : 0.0, static_cast<unsigned long long>(overflows),
                  static_cast<unsigned long long>(g_page_total.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_image_total.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_sampler_total.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_overflow_total.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_pending_stops.load(std::memory_order_relaxed)),
                  g_last_write.load(std::memory_order_relaxed), live_write,
                  g_last_read.load(std::memory_order_relaxed));
}

} // namespace SotcGcn::Globals
