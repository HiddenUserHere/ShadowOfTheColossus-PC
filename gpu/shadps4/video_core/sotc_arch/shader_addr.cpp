// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A3 (SOTC_SHADER_ADDR, docs/re-stutters/native/ARCH-DRAW.md sections 6 and 8). The CPU replica of the address
// the generated code computes for a guest buffer, and the counters of the bind skip and of the verify run.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "shader_recompiler/resource.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_arch/addr_table.h"
#include "video_core/sotc_arch/shader_addr.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::ShaderAddr {

namespace {

// The profile of the pipeline being bound (set by BindResources on the draw thread, read by BindBuffers).
thread_local const Shader::Profile* t_bind_profile = nullptr;

// SOTC_SHADER_ADDR=2: the debug buffer the verify loads write into (layout: resource.h, ShaderAddrVerify*). Host
// visible and coherent, with a device address; the packet carries the address (VerifyAddress). Never freed by hand.
struct VerifyState {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    u8* mapped{};
    vk::DeviceAddress address{};
};
std::unique_ptr<VerifyState> g_verify;

// One record as the shader writes it (72 bytes, see resource.h).
struct VerifyRecord {
    u64 pgm_hash;
    u64 device_address;
    u64 vbase;
    u64 vsize;
    u64 address_value;
    u64 descriptor_value;
    u32 stage;
    u32 buffer;
    u32 element;
    u32 lane;
    u32 element_size;
    u32 pad;
};
static_assert(sizeof(VerifyRecord) == Shader::ShaderAddrVerifyRecordSize);

std::atomic<u64> g_skipped{0};
std::atomic<u64> g_skipped_written{0};
std::atomic<u64> g_skipped_bytes{0};
std::atomic<u64> g_match{0};
std::atomic<u64> g_mismatch{0};
std::atomic<u64> g_other_buffer{0};
std::atomic<u64> g_no_address{0};
std::atomic<u64> g_unanswered{0};

// The first unanswered binds of the report window, by guest address (mutex: only the report reads them).
constexpr size_t kListed = 8;
std::mutex g_list_mutex;
std::array<std::pair<VAddr, u64>, kListed> g_listed{};
size_t g_listed_count = 0;

void NoteUnanswered(VAddr base, u64 size) {
    std::scoped_lock lock{g_list_mutex};
    if (g_listed_count < kListed) {
        g_listed[g_listed_count++] = {base, size};
    }
}

} // namespace

bool On() {
    static const bool on = SwitchMode("SOTC_SHADER_ADDR") != 0;
    return on;
}

bool Verify() {
    static const bool verify = SwitchMode("SOTC_SHADER_ADDR") == 2;
    return verify;
}

u8 Mode() {
    // sotc: SOTC_ADDR_TABLE=1 (addr_table.h) turns the modes into 3 (addresses through the table) and 4 (table and
    // verify). The profile carries them, so they are a different pipeline cache key.
    static const u8 mode = [] {
        const u8 base = static_cast<u8>(SwitchMode("SOTC_SHADER_ADDR"));
        if (base != 0 && AddrTable::On()) {
            return static_cast<u8>(base == 2 ? 4 : 3);
        }
        return base;
    }();
    return mode;
}

// sotc: refusal of a verify run that cannot be checked. Prints the reason and exits: a run that went on would
// report nothing, or the packet would carry a null address for the shader's writes.
[[noreturn]] void Refuse(const char* reason) {
    std::fprintf(stderr, "SOTC SHADER_ADDR=2 refused: %s\n", reason);
    std::exit(EXIT_FAILURE);
}

void Init(const Vulkan::Instance& instance) {
    if (!Verify() || g_verify) {
        return;
    }
    // The packet word that carries the debug address (index_table_address) is the heap's index table.
    if (Heap::On()) {
        Refuse("SOTC_HEAP uses the same packet word (index_table_address); run with SOTC_HEAP unset");
    }
    // The verified reads go through the mirror window (or, in table mode, the table): without it they are never checked.
    if (!AddrTable::On() && Mirror::Windows().empty()) {
        Refuse("no GPU mirror window (needs SOTC_MIRROR=1 on a device with sparse buffers)");
    }
    if (!instance.SotcBufferDeviceAddress()) {
        Refuse("the device has no buffer device address");
    }
    const vk::Device device = instance.GetDevice();
    const u64 bytes = Shader::ShaderAddrVerifyRecordsOffset +
                      u64{Shader::ShaderAddrVerifyCapacity} * Shader::ShaderAddrVerifyRecordSize;
    auto [created, buffer] = device.createBufferUnique(vk::BufferCreateInfo{
        .size = bytes,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (created != vk::Result::eSuccess) {
        Refuse("the debug buffer could not be created");
    }
    const vk::MemoryRequirements req = device.getBufferMemoryRequirements(*buffer);
    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();
    const vk::MemoryPropertyFlags wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = props.memoryTypeCount;
    for (u32 i = 0; i < props.memoryTypeCount && type == props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    if (type == props.memoryTypeCount) {
        Refuse("no host-visible coherent memory type for the debug buffer");
    }
    // A buffer with a device address needs its memory allocated with the device address flag.
    const vk::StructureChain alloc_chain = {
        vk::MemoryAllocateInfo{.allocationSize = req.size, .memoryTypeIndex = type},
        vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress},
    };
    auto [alloc, memory] = device.allocateMemoryUnique(alloc_chain.get<vk::MemoryAllocateInfo>());
    if (alloc != vk::Result::eSuccess) {
        Refuse("the debug buffer memory could not be allocated");
    }
    if (device.bindBufferMemory(*buffer, *memory, 0) != vk::Result::eSuccess) {
        Refuse("the debug buffer memory could not be bound");
    }
    const auto [map_result, mapped] = device.mapMemory(*memory, 0, bytes);
    if (map_result != vk::Result::eSuccess) {
        Refuse("the debug buffer memory could not be mapped");
    }
    auto state = std::make_unique<VerifyState>();
    state->mapped = static_cast<u8*>(mapped);
    std::memset(state->mapped, 0, bytes);
    state->address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *buffer});
    state->buffer = std::move(buffer);
    state->memory = std::move(memory);
    std::fprintf(stderr, "SOTC SHADER_ADDR=2: debug buffer at %#llx, %llu bytes\n",
                 static_cast<unsigned long long>(state->address), static_cast<unsigned long long>(bytes));
    g_verify = std::move(state);
}

vk::DeviceAddress VerifyAddress() {
    return g_verify ? g_verify->address : 0;
}

void SetBindProfile(const Shader::Profile* profile) {
    t_bind_profile = profile;
}

const Shader::Profile* BindProfile() {
    return t_bind_profile;
}

vk::DeviceAddress Expected(VAddr base, u64 size) {
    // The window rule of the shader (ARCH-DRAW section 6, specialist 6's SPIR-V): the arena window when the whole
    // range lies inside it, else the mirror window when it lies inside that, else 0. Unsigned, no wrap.
    const u64 end = base + size;
    if (size == 0 || end < base) {
        return 0;
    }
    // sotc: SOTC_ADDR_TABLE: the shader's address of base is the table entry of its page (0 when not resident).
    if (AddrTable::On()) {
        return AddrTable::Translate(base);
    }
    // No arena window: the packet's arena_size is 0 (the ring-8 arena is many imported runs, not one window),
    // so the shader takes the arena step never and every range goes through the mirror.
    // Mirror window 0 (the packet carries one mirror window).
    const auto windows = Mirror::Windows();
    if (!windows.empty()) {
        const Mirror::Window& mirror = windows.front();
        if (base >= mirror.guest_base && end <= mirror.guest_base + mirror.size) {
            return mirror.device_base + (base - mirror.guest_base);
        }
    }
    return 0;
}

void NoteSkip(u64 size, bool written) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    g_skipped_bytes.fetch_add(size, std::memory_order_relaxed);
    if (written) {
        g_skipped_written.fetch_add(1, std::memory_order_relaxed);
    }
}

void VerifyBind(VAddr base, u64 size, const VideoCore::Buffer* buffer, u64 offset) {
    if (!buffer || !buffer->HasBufferDeviceAddress()) {
        g_no_address.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const vk::DeviceAddress begin = buffer->BufferDeviceAddress();
    const vk::DeviceAddress obtained = begin + offset;
    const vk::DeviceAddress expected = Expected(base, size);
    if (expected == 0) {
        g_unanswered.fetch_add(1, std::memory_order_relaxed);
        NoteUnanswered(base, size);
        return;
    }
    if (expected == obtained) {
        g_match.fetch_add(1, std::memory_order_relaxed);
    } else if (expected >= begin && expected < begin + buffer->SizeBytes()) {
        // The same buffer at another offset: the address the shader computes points at other bytes.
        g_mismatch.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Another device memory (the mirror or an arena run, against a classic buffer): the same guest bytes
        // may live there, so equality of addresses says nothing. Byte comparison is the mirror's own verify.
        g_other_buffer.fetch_add(1, std::memory_order_relaxed);
    }
}

// SOTC_SHADER_ADDR=2: reads and resets the debug buffer. The GPU may still be writing it, so the counts lag by the
// frames in flight and a record can be torn; this is a diagnostic, not an exact count. Returns the summary line
// (counters). Up to 16 distinct records (by pgm hash and buffer handle) go to stderr, one line each, with the
// guest base and the two values in hex.
static size_t VerifyReport(char* out, u32 size, double seconds) {
    const auto take = [](u8* base, u32 offset) {
        return std::atomic_ref<u32>(*reinterpret_cast<u32*>(base + offset))
            .exchange(0, std::memory_order_relaxed);
    };
    u8* const mapped = g_verify->mapped;
    const u32 compared = take(mapped, Shader::ShaderAddrVerifyCompared);
    const u32 mismatch = take(mapped, Shader::ShaderAddrVerifyMismatch);
    const u32 zero = take(mapped, Shader::ShaderAddrVerifyMismatchZero);
    const u32 slots = take(mapped, Shader::ShaderAddrVerifySlots);
    const u32 uncompared = take(mapped, Shader::ShaderAddrVerifyUncompared);
    const u32 uncompared_nonzero = take(mapped, Shader::ShaderAddrVerifyUncomparedNonZero);
    const u32 kept = std::min(slots, Shader::ShaderAddrVerifyCapacity);

    constexpr size_t kMaxListed = 16;
    std::array<VerifyRecord, kMaxListed> listed{};
    size_t listed_count = 0;
    for (u32 i = 0; i < kept; ++i) {
        VerifyRecord rec{};
        std::memcpy(&rec, mapped + Shader::ShaderAddrVerifyRecordsOffset +
                              size_t{i} * Shader::ShaderAddrVerifyRecordSize, sizeof rec);
        const bool seen = std::any_of(listed.begin(), listed.begin() + listed_count,
                                      [&](const VerifyRecord& other) {
                                          return other.pgm_hash == rec.pgm_hash &&
                                                 other.buffer == rec.buffer;
                                      });
        if (!seen && listed_count < kMaxListed) {
            listed[listed_count++] = rec;
        }
    }
    for (size_t i = 0; i < listed_count; ++i) {
        const VerifyRecord& r = listed[i];
        std::fprintf(stderr,
                     "SOTC SHADER_ADDR verify record: pgm %#llx stage %u buffer %u element %u lane %u "
                     "elem %u B, V# base %#llx size %#llx, shader address %#llx, address value %#llx, "
                     "descriptor value %#llx\n",
                     static_cast<unsigned long long>(r.pgm_hash), r.stage, r.buffer, r.element, r.lane,
                     r.element_size, static_cast<unsigned long long>(r.vbase),
                     static_cast<unsigned long long>(r.vsize),
                     static_cast<unsigned long long>(r.device_address),
                     static_cast<unsigned long long>(r.address_value),
                     static_cast<unsigned long long>(r.descriptor_value));
    }
    // Reset: the counters were taken above; the kept records are cleared so the next window starts empty.
    std::memset(mapped + Shader::ShaderAddrVerifyRecordsOffset, 0,
                size_t{kept} * Shader::ShaderAddrVerifyRecordSize);

    const int n = std::snprintf(
        out, size,
        "SOTC SHADER_ADDR verify: %llu loads compared in %.1f s, %llu lanes differ (%llu with address "
        "value 0: non-resident or out-of-window page; %llu with a non-zero address value), %llu loads not "
        "compared (V# range; %llu lanes of them with a non-zero descriptor value), %u records kept, %u dropped",
        static_cast<unsigned long long>(compared), seconds, static_cast<unsigned long long>(mismatch),
        static_cast<unsigned long long>(zero), static_cast<unsigned long long>(mismatch - std::min(mismatch, zero)),
        static_cast<unsigned long long>(uncompared), static_cast<unsigned long long>(uncompared_nonzero), kept,
        slots - kept);
    return n < 0 ? 0 : std::min(static_cast<size_t>(n), static_cast<size_t>(size) - 1);
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    const u64 skipped = g_skipped.exchange(0, std::memory_order_relaxed);
    const u64 written = g_skipped_written.exchange(0, std::memory_order_relaxed);
    const u64 bytes = g_skipped_bytes.exchange(0, std::memory_order_relaxed);
    const u64 match = g_match.exchange(0, std::memory_order_relaxed);
    const u64 mismatch = g_mismatch.exchange(0, std::memory_order_relaxed);
    const u64 other = g_other_buffer.exchange(0, std::memory_order_relaxed);
    const u64 no_address = g_no_address.exchange(0, std::memory_order_relaxed);
    const u64 unanswered = g_unanswered.exchange(0, std::memory_order_relaxed);
    std::array<std::pair<VAddr, u64>, kListed> listed{};
    size_t listed_count = 0;
    {
        std::scoped_lock lock{g_list_mutex};
        listed_count = g_listed_count;
        listed = g_listed;
        g_listed_count = 0;
    }
    int n = std::snprintf(out, size,
                          "SOTC SHADER_ADDR: %llu binds skipped (%llu written, %.1f MiB) in %.1f s; "
                          "verify: %llu match, %llu mismatch, %llu other buffer, %llu no address, "
                          "%llu unanswered",
                          static_cast<unsigned long long>(skipped),
                          static_cast<unsigned long long>(written),
                          static_cast<double>(bytes) / (1024.0 * 1024.0), seconds,
                          static_cast<unsigned long long>(match),
                          static_cast<unsigned long long>(mismatch),
                          static_cast<unsigned long long>(other),
                          static_cast<unsigned long long>(no_address),
                          static_cast<unsigned long long>(unanswered));
    if (n < 0) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < listed_count && static_cast<u32>(n) < size; ++i) {
        const int m = std::snprintf(out + n, size - n, "%s%#llx+%#llx", i == 0 ? " [unanswered: " : " ",
                                    static_cast<unsigned long long>(listed[i].first),
                                    static_cast<unsigned long long>(listed[i].second));
        if (m < 0) {
            break;
        }
        n += m;
    }
    if (listed_count != 0 && static_cast<u32>(n) < size) {
        std::snprintf(out + n, size - n, "]");
    }
    // sotc: SOTC_SHADER_ADDR=2 (verify): the debug buffer's counters, on a line of their own.
    if (g_verify) {
        const size_t used = std::strlen(out);
        if (used + 2 < size) {
            out[used] = '\n';
            VerifyReport(out + used + 1, size - static_cast<u32>(used) - 1, seconds);
        }
    }
}

} // namespace SotcArch::ShaderAddr
