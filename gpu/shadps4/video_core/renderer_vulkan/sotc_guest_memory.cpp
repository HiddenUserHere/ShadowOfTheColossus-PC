// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's memory for the GPU in place (sotc_guest_memory.h).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" {
const unsigned char* runtime_memory_backing_view(uint64_t* size);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace Vulkan::SotcGuestMemory {

namespace {

constexpr u64 kChunk = 64ULL << 20;
constexpr u64 kPage = 16ULL << 10; ///< the guest page (PAGE in src/runtime_memory.c)

/// Stage 1 (SOTC_PC_MEMORY): one 64 MiB chunk of the view.
struct Chunk {
    vk::DeviceMemory memory;
    vk::Buffer buffer;
    bool failed{}; ///< refused once: not asked again
};

/// Ring import (SOTC_PC_RING): one contiguous backing run of guest memory and its buffer.
struct Run {
    uint64_t guest_begin{};
    uint64_t guest_end{};
    const u8* host_begin{}; ///< backing pointer of guest_begin when imported
    u64 base_offset{};      ///< of guest_begin in buffer
    u64 buffer_size{};      ///< sotc: size of buffer (SOTC_PC_RING=8 binds need it)
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
};

/// The backing view: Missing until the runtime creates it, Refused for good (g_reason says why).
enum class View : u8 { Missing, Refused, Ready };

// Everything below the atomics is guarded by g_mutex.
const Instance* g_instance{};
const u8* g_base{};
u64 g_size{};
u32 g_memory_type{};
u64 g_alignment{}; ///< minImportedHostPointerAlignment
View g_view{View::Missing};
std::string g_reason;
std::mutex g_mutex;

bool g_pc_memory{}; ///< SOTC_PC_MEMORY=1
bool g_ring{};      ///< SOTC_PC_RING=3, 4 or 8
bool g_host{};      ///< sotc: SOTC_PC_RING=8: runs are imported as shader-readable buffers too

// Stage 1.
std::atomic<bool> g_stage1_done{};
std::atomic<bool> g_enabled{};
std::vector<Chunk> g_chunks;
u64 g_imported_bytes{};

// Ring import.
bool g_ring_decided{};
bool g_ring_ok{};
std::map<uint64_t, Run> g_runs;  ///< by guest_begin
std::vector<Run> g_retired;      ///< replaced runs: kept, the GPU may still read them
u64 g_ring_bytes{};
bool g_ring_refusal_logged{};

/// sotc: the usages of an imported run. The copies need transfer-source only; with
/// SOTC_PC_RING=8 the binds read (and write) the run from the shaders, so the shader usages too.
vk::BufferUsageFlags RunUsage() {
    if (!g_host) {
        return vk::BufferUsageFlagBits::eTransferSrc;
    }
    return vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
           vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
           vk::BufferUsageFlagBits::eUniformTexelBuffer |
           vk::BufferUsageFlagBits::eStorageTexelBuffer |
           vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
           vk::BufferUsageFlagBits::eIndirectBuffer;
}

const unsigned char* Backing(uint64_t guest) {
    return runtime_memory_backing_pointer(static_cast<uintptr_t>(guest));
}

bool Import(Chunk& chunk, u64 index) {
    const vk::Device device = g_instance->GetDevice();
    const u64 size = std::min(kChunk, g_size - index * kChunk);
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = const_cast<u8*>(g_base + index * kChunk),
    };
    const auto [memory_result, memory] = device.allocateMemory(vk::MemoryAllocateInfo{
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = g_memory_type,
    });
    if (memory_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC PC memory: chunk %llu not imported (%s)\n",
                     static_cast<unsigned long long>(index), vk::to_string(memory_result).c_str());
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, buffer] = device.createBuffer(vk::BufferCreateInfo{
        .pNext = &external,
        .size = size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
                 vk::BufferUsageFlagBits::eStorageBuffer,
    });
    if (buffer_result != vk::Result::eSuccess ||
        device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC PC memory: chunk %llu has no buffer\n",
                     static_cast<unsigned long long>(index));
        if (buffer_result == vk::Result::eSuccess) {
            device.destroyBuffer(buffer);
        }
        device.freeMemory(memory);
        return false;
    }
    chunk.memory = memory;
    chunk.buffer = buffer;
    g_imported_bytes += size;
    return true;
}

/// Under g_mutex. Finds the backing view and the memory type and alignment its pages can be
/// imported with. Looked at once the view exists; Refused is final.
View ResolveView() {
    if (g_view != View::Missing) {
        return g_view;
    }
    uint64_t size = 0;
    const u8* base = runtime_memory_backing_view(&size);
    if (!base || size == 0) {
        return View::Missing; // the game has not mapped memory yet
    }
    g_base = base;
    g_size = size;
    g_view = View::Refused;

    const vk::Device device = g_instance->GetDevice();
    const vk::PhysicalDevice physical = g_instance->GetPhysicalDevice();
    const auto [properties_result, host_properties] = device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, g_base);
    if (properties_result != vk::Result::eSuccess || host_properties.memoryTypeBits == 0) {
        g_reason = "the backing view cannot be imported (" +
                   vk::to_string(properties_result) + ")";
        return g_view;
    }
    // A host-visible, coherent type: what the GPU writes is the game's memory as soon as the
    // work is done, with no flush.
    const auto memory = physical.getMemoryProperties();
    constexpr auto wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    bool found = false;
    for (u32 i = 0; i < memory.memoryTypeCount && !found; ++i) {
        if ((host_properties.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            g_memory_type = i;
            found = true;
        }
    }
    if (!found) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "no coherent host memory type (bits 0x%x)",
                      host_properties.memoryTypeBits);
        g_reason = buf;
        return g_view;
    }

    // Ring import: the alignment every imported host pointer and size must have, and the
    // view's own start must have it too (ranges are rounded down/up to it inside the view).
    const auto props_chain = physical.getProperties2<vk::PhysicalDeviceProperties2,
                                                    vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
    g_alignment =
        props_chain.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>()
            .minImportedHostPointerAlignment;
    if (g_alignment == 0 || reinterpret_cast<uintptr_t>(g_base) % g_alignment != 0) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "the view is not aligned to %llu bytes",
                      static_cast<unsigned long long>(g_alignment));
        g_reason = buf;
        return g_view;
    }
    // The transfer-source use: the device must accept host allocations as an importable handle
    // for a buffer with TRANSFER_SRC usage.
    const auto external = physical.getExternalBufferProperties(vk::PhysicalDeviceExternalBufferInfo{
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    });
    if (!bool(external.externalMemoryProperties.externalMemoryFeatures &
              vk::ExternalMemoryFeatureFlagBits::eImportable)) {
        g_reason = "host allocations cannot be imported as transfer-source buffers";
        return g_view;
    }
    // sotc: SOTC_PC_RING=8: the shader usages must be importable too.
    if (g_host) {
        const auto shader = physical.getExternalBufferProperties(vk::PhysicalDeviceExternalBufferInfo{
            .usage = RunUsage(),
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        });
        if (!bool(shader.externalMemoryProperties.externalMemoryFeatures &
                  vk::ExternalMemoryFeatureFlagBits::eImportable)) {
            g_reason = "host allocations cannot be imported as shader-read buffers";
            return g_view;
        }
    }
    g_view = View::Ready;
    return g_view;
}

/// Under g_mutex. Stage 1: the first chunk is imported now, to prove the import works on this
/// driver before anything relies on it.
void Stage1() {
    const View view = ResolveView();
    if (view == View::Missing) {
        return; // not yet: the next submission looks again
    }
    g_stage1_done.store(true, std::memory_order_release);
    if (view == View::Refused) {
        std::fprintf(stderr, "SOTC PC memory: %s, off\n", g_reason.c_str());
        return;
    }
    g_chunks.resize((g_size + kChunk - 1) / kChunk);
    const auto start = std::chrono::steady_clock::now();
    if (!Import(g_chunks[0], 0)) {
        g_chunks[0].failed = true;
        std::fprintf(stderr, "SOTC PC memory: import refused by the driver, off\n");
        return;
    }
    g_enabled.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "SOTC PC memory: %llu MiB of game memory, %zu chunks of 64 MiB imported on use "
                 "(memory type %u); chunk 0 in %.1f ms\n",
                 static_cast<unsigned long long>(g_size >> 20), g_chunks.size(), g_memory_type,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           start)
                     .count());
}

/// Under g_mutex. Decides the ring path once and logs which path is active. False while the
/// game has not mapped memory yet (undecided).
bool RingDecide() {
    if (g_ring_decided) {
        return g_ring_ok;
    }
    std::string reason;
    if (!g_instance) {
        reason = "VK_EXT_external_memory_host not enabled";
    } else {
        const View view = ResolveView();
        if (view == View::Missing) {
            return false;
        }
        if (view == View::Refused) {
            reason = g_reason;
        }
    }
    g_ring_decided = true;
    g_ring_ok = reason.empty();
    if (g_ring_ok) {
        std::fprintf(stderr,
                     "SOTC PC ring import: import (view %llu MiB, memory type %u, alignment %llu)\n",
                     static_cast<unsigned long long>(g_size >> 20), g_memory_type,
                     static_cast<unsigned long long>(g_alignment));
    } else {
        std::fprintf(stderr, "SOTC PC ring import: unavailable: %s\n", reason.c_str());
    }
    return g_ring_ok;
}

/// Under g_mutex. True when the run still maps its guest range to the same backing: two backing
/// lookups (the first and the last byte), since the view is linear and a run is contiguous.
bool RunMatches(const Run& run) {
    const uint64_t last = run.guest_end - 1;
    return Backing(run.guest_begin) == run.host_begin &&
           Backing(last) == run.host_begin + (last - run.guest_begin);
}

/// Under g_mutex. The run starting at guest_addr, if any; the one before it may still cover
/// guest_addr (LookupGuest checks the end).
const Run* FindRun(uint64_t guest_addr) {
    auto it = g_runs.upper_bound(guest_addr);
    if (it == g_runs.begin()) {
        return nullptr;
    }
    --it;
    return &it->second;
}

void RefuseRange(const std::string& why) {
    if (!g_ring_refusal_logged) {
        g_ring_refusal_logged = true;
        std::fprintf(stderr,
                     "SOTC PC ring import: a range was refused (%s); the caller copies on the CPU "
                     "(logged once)\n",
                     why.c_str());
    }
}

/// Under g_mutex. Imports one contiguous backing run [guest_begin, guest_end) whose first byte is
/// at host_begin. The host range is rounded out to minImportedHostPointerAlignment (start down,
/// end up), both of which must stay inside the view.
bool ImportRun(uint64_t guest_begin, uint64_t guest_end, const u8* host_begin) {
    const auto it = g_runs.find(guest_begin);
    if (it != g_runs.end() && it->second.guest_end == guest_end && RunMatches(it->second)) {
        return true;
    }
    const uintptr_t host = reinterpret_cast<uintptr_t>(host_begin);
    const uintptr_t host_end = host + (guest_end - guest_begin);
    const uintptr_t start = host - host % g_alignment;
    const uintptr_t stop = host_end + (g_alignment - host_end % g_alignment) % g_alignment;
    const uintptr_t view_lo = reinterpret_cast<uintptr_t>(g_base);
    if (start < view_lo || stop > view_lo + g_size) {
        RefuseRange("a run reaches outside the aligned view");
        return false;
    }
    // A stale run for this key is replaced, never destroyed: a submission may still read it.
    if (it != g_runs.end()) {
        g_retired.push_back(it->second);
        g_runs.erase(it);
    }

    const vk::Device device = g_instance->GetDevice();
    const vk::DeviceSize size = stop - start;
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = reinterpret_cast<void*>(start),
    };
    const auto [memory_result, memory] = device.allocateMemory(vk::MemoryAllocateInfo{
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = g_memory_type,
    });
    if (memory_result != vk::Result::eSuccess) {
        RefuseRange("memory import: " + vk::to_string(memory_result));
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, buffer] = device.createBuffer(vk::BufferCreateInfo{
        .pNext = &external,
        .size = size,
        .usage = RunUsage(),
    });
    if (buffer_result != vk::Result::eSuccess ||
        device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        if (buffer_result == vk::Result::eSuccess) {
            device.destroyBuffer(buffer);
        }
        device.freeMemory(memory);
        RefuseRange("buffer over the imported memory");
        return false;
    }
    Run run;
    run.guest_begin = guest_begin;
    run.guest_end = guest_end;
    run.host_begin = host_begin;
    run.base_offset = host - start;
    run.buffer_size = size;
    run.buffer = buffer;
    run.memory = memory;
    g_runs.emplace(guest_begin, run);
    g_ring_bytes += size;
    return true;
}

} // namespace

void Init(const Instance& instance) {
    const char* pc_memory = std::getenv("SOTC_PC_MEMORY");
    const char* pc_ring = std::getenv("SOTC_PC_RING");
    g_pc_memory = pc_memory && pc_memory[0] == '1';
    g_ring = pc_ring && (pc_ring[0] == '3' || pc_ring[0] == '4' || pc_ring[0] == '8');
    g_host = pc_ring && pc_ring[0] == '8'; // sotc: SOTC_PC_RING=8 (host arena binds)
    if (instance.IsExternalMemoryHostEnabled()) {
        g_instance = &instance;
    }
}

bool Ready() {
    if (!g_pc_memory || !g_instance) {
        return false;
    }
    if (!g_stage1_done.load(std::memory_order_acquire)) {
        std::scoped_lock lock{g_mutex};
        if (!g_stage1_done.load(std::memory_order_relaxed)) {
            Stage1();
        }
    }
    return g_enabled.load(std::memory_order_acquire);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

Span Get(const u8* host, u64 size) {
    if (!g_enabled.load(std::memory_order_acquire) || host < g_base || size == 0) {
        return {};
    }
    const u64 offset = static_cast<u64>(host - g_base);
    if (offset + size > g_size || offset / kChunk != (offset + size - 1) / kChunk) {
        return {};
    }
    const u64 index = offset / kChunk;
    std::scoped_lock lock{g_mutex};
    Chunk& chunk = g_chunks[index];
    if (!chunk.buffer) {
        if (chunk.failed || !Import(chunk, index)) {
            chunk.failed = true;
            return {};
        }
    }
    return {chunk.buffer, offset - index * kChunk};
}

bool Available() {
    if (!g_ring) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    return RingDecide();
}

bool ImportGuestRange(uint64_t guest_addr, uint64_t size) {
    if (!g_ring || size == 0) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (!RingDecide()) {
        return false;
    }
    // Already imported, and the backing is still the one imported: the common case, per submit.
    if (const Run* run = FindRun(guest_addr);
        run && guest_addr + size <= run->guest_end && RunMatches(*run)) {
        return true;
    }
    // Split the range where its backing stops being contiguous. The view is linear, so a run is
    // a stretch of guest pages whose backing pointers advance by the same amount: checked one
    // guest page at a time (only on the first import of a range).
    const uint64_t end = guest_addr + size;
    uint64_t at = guest_addr;
    while (at < end) {
        const u8* host = Backing(at);
        if (!host) {
            RefuseRange("guest memory is not mapped");
            return false;
        }
        uint64_t piece_end = std::min(end, (at / kPage + 1) * kPage);
        while (piece_end < end && Backing(piece_end) == host + (piece_end - at)) {
            piece_end = std::min(end, piece_end + kPage);
        }
        if (!ImportRun(at, piece_end, host)) {
            return false;
        }
        at = piece_end;
    }
    return true;
}

bool LookupGuestView(uint64_t guest_addr, uint64_t size, GuestView* out) {
    if (!g_ring || size == 0 || !out) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    const Run* run = FindRun(guest_addr);
    if (!run || guest_addr + size > run->guest_end) {
        return false;
    }
    out->buffer = run->buffer;
    out->offset = run->base_offset + (guest_addr - run->guest_begin);
    out->buffer_size = run->buffer_size;
    // The guest address of buffer byte 0: the same for every bind of this run.
    out->cpu_base = guest_addr - out->offset;
    return true;
}

bool LookupGuest(uint64_t guest_addr, uint64_t size, vk::Buffer* buffer, uint64_t* offset) {
    if (!g_ring || size == 0 || !buffer || !offset) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    const Run* run = FindRun(guest_addr);
    if (!run || guest_addr + size > run->guest_end) {
        return false;
    }
    *buffer = run->buffer;
    *offset = run->base_offset + (guest_addr - run->guest_begin);
    return true;
}

} // namespace Vulkan::SotcGuestMemory
