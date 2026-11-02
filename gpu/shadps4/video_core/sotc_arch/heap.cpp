// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_HEAP (ARCH-DRAW.md part 3). The shader-side view of the N5 bindless arrays: image views in the
// per-key arrays of set 1 (binding 1 + key, vk_bindless.h) and samplers in the bindless sampler array
// (texture_cache/sampler.h). The heap owns no descriptors: BindlessImages and BindlessSamplers do, and their
// slots are released only after the GPU passed the tick of the release (BindlessImages::Reclaim), so a slot a
// submitted frame can reference is never reused. VK_EXT_descriptor_heap is not used (runtime-array form only).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"

// sotc: the shader's guard capacities are the heap's (resource.h).
static_assert(Shader::HeapImageCapacity == VideoCore::BindlessImageCapacity);
static_assert(Shader::HeapSamplerCapacity == VideoCore::BindlessSamplers::Capacity);

namespace SotcArch::Heap {
namespace {
// Set by CheckRequirements when a requirement is missing on this device (process lifetime).
std::atomic<bool> g_refused{false};

// sotc: SOTC_HEAP debug buffer (the shader's guard and verify writes; layout: resource.h, HeapDebug*). Host visible
// and coherent, with a device address that the heap block carries (HeapBlock::debug_address). Never freed by hand.
struct DebugState {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    u8* mapped{};
    vk::DeviceAddress address{};
};
std::unique_ptr<DebugState> g_debug;

// One record as the shader writes it (40 bytes, resource.h).
struct DebugRecord {
    u64 pgm_hash;
    u64 table_address;
    u32 stage;
    u32 kind;
    u32 number;
    u32 classic;
    u32 table;
    u32 entry;
};
static_assert(sizeof(DebugRecord) == Shader::HeapRecordSize);

const char* KindName(u32 kind) {
    switch (kind) {
    case 0:
        return "image";
    case 1:
        return "sampler";
    case 2:
        return "guard";
    default:
        return "?";
    }
}

// The 16-bit slot of `slot` in a packed array of u32 words (two slots per word, low half first).
template <typename Words>
u32 HalfOf(const Words& words, u32 slot) {
    return (words[slot / 2] >> ((slot & 1) * 16)) & 0xFFFFu;
}

void CreateDebug(const Vulkan::Instance& instance) {
    if (g_debug) {
        return;
    }
    const auto refuse = [](const char* reason) {
        g_refused.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "SOTC HEAP refused: %s; the run uses the descriptor path\n", reason);
    };
    if (!instance.SotcBufferDeviceAddress()) {
        refuse("the device has no buffer device address (the debug buffer)");
        return;
    }
    const vk::Device device = instance.GetDevice();
    const u64 bytes = Shader::HeapRecordsOffset + u64{Shader::HeapRecordCapacity} * Shader::HeapRecordSize;
    auto [created, buffer] = device.createBufferUnique(vk::BufferCreateInfo{
        .size = bytes,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (created != vk::Result::eSuccess) {
        refuse("the debug buffer could not be created");
        return;
    }
    const vk::MemoryRequirements req = device.getBufferMemoryRequirements(*buffer);
    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();
    const vk::MemoryPropertyFlags wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = props.memoryTypeCount;
    for (u32 i = 0; i < props.memoryTypeCount && type == props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    if (type == props.memoryTypeCount) {
        refuse("no host-visible coherent memory type for the debug buffer");
        return;
    }
    // A buffer with a device address needs its memory allocated with the device address flag.
    const vk::StructureChain alloc_chain = {
        vk::MemoryAllocateInfo{.allocationSize = req.size, .memoryTypeIndex = type},
        vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress},
    };
    auto [alloc, memory] = device.allocateMemoryUnique(alloc_chain.get<vk::MemoryAllocateInfo>());
    if (alloc != vk::Result::eSuccess) {
        refuse("the debug buffer memory could not be allocated");
        return;
    }
    if (device.bindBufferMemory(*buffer, *memory, 0) != vk::Result::eSuccess) {
        refuse("the debug buffer memory could not be bound");
        return;
    }
    const auto [map_result, mapped] = device.mapMemory(*memory, 0, bytes);
    if (map_result != vk::Result::eSuccess) {
        refuse("the debug buffer memory could not be mapped");
        return;
    }
    auto state = std::make_unique<DebugState>();
    state->mapped = static_cast<u8*>(mapped);
    std::memset(state->mapped, 0, bytes);
    state->address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *buffer});
    state->buffer = std::move(buffer);
    state->memory = std::move(memory);
    std::fprintf(stderr, "SOTC HEAP: debug buffer at %#llx, %llu bytes\n",
                 static_cast<unsigned long long>(state->address), static_cast<unsigned long long>(bytes));
    g_debug = std::move(state);
}

// sotc: SOTC_HEAP_GPU_VERIFY=1: the shader compares the classic slot words with the index table.
bool GpuVerify() {
    static const bool on = SwitchMode("SOTC_HEAP_GPU_VERIFY") == 1;
    return on;
}
} // namespace

u8 ProfileValue() {
    // A heap pipeline writes to the debug buffer, so its profile needs the buffer (made by Init): before that, or
    // when it could not be made, the profile is the one of heap off.
    if (!On() || !g_debug) {
        return 0;
    }
    return GpuVerify() ? 2 : 1;
}

vk::DeviceAddress DebugAddress() {
    return g_debug ? g_debug->address : 0;
}

void FillBlock(HeapBlock& block, const Shader::Info* stage, const Shader::BindlessSamplerSlots& slots) {
    block = {};
    block.debug_address = DebugAddress();
    if (!stage || !GpuVerify()) {
        return;
    }
    // The classic slots of this stage: the same absolute slots the classic bind writes (BindTexturesFull), compacted
    // to the stage's own range (relative slot = absolute - range base), as the shader reads them (resource.h).
    const u32 sampler_base = Shader::GetBindlessSamplerRange(stage->sw_stage).base;
    const u32 image_base = Shader::GetBindlessImageRange(stage->sw_stage).base;
    const u32 image_count =
        std::min<u32>(Shader::GetBindlessImageRange(stage->sw_stage).count, Shader::HeapVerifyImageWords * 2);
    for (u32 s = 0; s < stage->samplers.size() && s < Shader::HeapVerifySamplerWords * 2; ++s) {
        block.words[s / 2] |= HalfOf(slots.words, sampler_base + s) << ((s & 1) * 16);
    }
    for (u32 j = 0; j < image_count; ++j) {
        block.words[Shader::HeapVerifySamplerWords + j / 2] |= HalfOf(slots.image_words, image_base + j) << ((j & 1) * 16);
    }
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!g_debug) {
        return;
    }
    u8* const mapped = g_debug->mapped;
    const auto take = [mapped](u32 offset) {
        return std::atomic_ref<u32>(*reinterpret_cast<u32*>(mapped + offset)).exchange(0, std::memory_order_relaxed);
    };
    // The counters are taken before the records; a record written meanwhile is counted in the next window.
    const u32 guard = take(Shader::HeapDebugGuard);
    const u32 image_compared = take(Shader::HeapDebugCompareImage);
    const u32 image_mismatch = take(Shader::HeapDebugMismatchImage);
    const u32 sampler_compared = take(Shader::HeapDebugCompareSampler);
    const u32 sampler_mismatch = take(Shader::HeapDebugMismatchSampler);
    const u32 slots = take(Shader::HeapDebugSlots);
    const u32 kept = std::min(slots, Shader::HeapRecordCapacity);

    constexpr size_t kMaxListed = 8;
    std::array<DebugRecord, kMaxListed> listed{};
    size_t listed_count = 0;
    for (u32 i = 0; i < kept; ++i) {
        DebugRecord rec{};
        std::memcpy(&rec, mapped + Shader::HeapRecordsOffset + size_t{i} * Shader::HeapRecordSize, sizeof rec);
        const bool seen = std::any_of(listed.begin(), listed.begin() + listed_count, [&](const DebugRecord& other) {
            return other.pgm_hash == rec.pgm_hash && other.kind == rec.kind && other.number == rec.number &&
                   other.stage == rec.stage;
        });
        if (!seen && listed_count < kMaxListed) {
            listed[listed_count++] = rec;
        }
    }
    for (size_t i = 0; i < listed_count; ++i) {
        const DebugRecord& r = listed[i];
        std::fprintf(stderr,
                     "SOTC HEAP record: pgm %#llx stage %u %s %u, classic %u, table %u (entry %u), table address %#llx\n",
                     static_cast<unsigned long long>(r.pgm_hash), r.stage, KindName(r.kind), r.number, r.classic,
                     r.table, r.entry, static_cast<unsigned long long>(r.table_address));
    }
    std::memset(mapped + Shader::HeapRecordsOffset, 0, size_t{kept} * Shader::HeapRecordSize);

    const int n = std::snprintf(
        out, size,
        "SOTC HEAP guard: %u indices past the capacity (null used); verify: images %u compared, %u differ; "
        "samplers %u compared, %u differ; %u records kept, %u dropped (%.1f s)",
        guard, image_compared, image_mismatch, sampler_compared, sampler_mismatch, kept, slots - kept, seconds);
    if (n < 0) {
        out[0] = '\0';
    }
}

bool On() {
    // The environment part is read once; the device part is final before any pipeline is built (the instance).
    // sotc: the heap writes its descriptors in GENERAL: SOTC_LAYOUT_UNIFIED=1 only (2 is verify, which uses the old
    // layouts), and the feature is checked by CheckRequirements.
    static const bool env_on = SwitchMode("SOTC_HEAP") != 0 && SwitchMode("SOTC_N5_BINDLESS") != 0 &&
                               SwitchMode("SOTC_BINDLESS_SAMP") != 0 &&
                               SwitchMode("SOTC_LAYOUT_UNIFIED") == 1;
    return env_on && !g_refused.load(std::memory_order_relaxed);
}

bool VertexOn() {
    // sotc: SOTC_HEAP_VS, read once. GPU verify refuses it (sotc_arch.h).
    static const bool on = SwitchMode("SOTC_HEAP_VS") == 1 && SwitchMode("SOTC_HEAP") != 0 && !GpuVerify();
    return on && On();
}

bool CompositeOpOn() {
    // sotc: SOTC_HEAP_COMPOSITE_OP=1, read once (see sotc_arch.h).
    static const bool on = SwitchMode("SOTC_HEAP_COMPOSITE_OP") == 1;
    return on;
}

bool UsesDescriptorHeap() {
    // VK_EXT_descriptor_heap is left for later: the emitted SPIR-V is the runtime-array form only.
    return false;
}

void CheckRequirements(bool bindless_images, bool bindless_samplers) {
    if (SwitchMode("SOTC_HEAP") == 0) {
        return;
    }
    const bool n5 = SwitchMode("SOTC_N5_BINDLESS") != 0;
    const bool samp = SwitchMode("SOTC_BINDLESS_SAMP") != 0;
    // sotc: the heap's GENERAL descriptors need the layouts mode acting (=1 or the auto default) with the feature.
    const bool layout = Layouts::On() && Layouts::Supported();
    if (n5 && samp && layout && bindless_images && bindless_samplers) {
        return;
    }
    g_refused.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "SOTC HEAP refused: needs SOTC_N5_BINDLESS=1 (%s), SOTC_BINDLESS_SAMP=1 (%s), "
                 "SOTC_LAYOUT_UNIFIED=1 with VK_KHR_unified_image_layouts (%s) and a device with the bindless "
                 "arrays (%s); the run uses the descriptor path\n",
                 n5 && bindless_images ? "ok" : "missing", samp && bindless_samplers ? "ok" : "missing",
                 layout ? "ok" : "missing", bindless_images && bindless_samplers ? "ok" : "missing");
}

void Init(const Vulkan::Instance& instance) {
    if (!On()) {
        return;
    }
    // sotc: the debug buffer is needed by every heap pipeline (the guard writes to it), so a failure refuses the heap.
    CreateDebug(instance);
    if (!On()) {
        return;
    }
    // Once per process: the heap is process-global, and every TextureCache calls Init.
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    LOG_INFO(Render_Vulkan, "SOTC HEAP: VK_EXT_descriptor_heap not used (runtime-array form)");
    (void)instance;
}

u32 NullImageIndex(u32 key) {
    return VideoCore::BindlessImages::NullSlot(key);
}

u32 NullSamplerIndex() {
    return VideoCore::BindlessSamplers::DefaultSlot(false);
}

u32 IndexOf(VideoCore::ImageView& view, u32 key, vk::ImageLayout layout) {
    VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
    if (!heap || key >= Shader::NUM_BINDLESS_IMAGE_KEYS || !view.image_view) {
        return NullImageIndex(key);
    }
    // The same slot the rasterizer binds (its BindlessImageIndex): the view keeps it per key while the heap
    // still holds the view in this layout, and takes a new one otherwise.
    const vk::ImageView handle = *view.image_view;
    u32& slot = view.bindless_slots[key];
    if (slot == Shader::NO_BINDLESS_IMAGE_SLOT || !heap->Holds(key, slot, handle, layout)) {
        heap->Release(key, slot, handle);
        slot = heap->Acquire(key, handle, layout);
    }
    heap->Use(key, slot);
    if (heap->Mode() == 2 && slot != NullImageIndex(key)) {
        heap->Verify(key, slot, handle);
    }
    return slot;
}

u32 IndexOfSampler(VideoCore::Sampler& sampler) {
    // The sampler's slot in the bindless sampler array, given by the texture cache when the sampler was created
    // (TextureCache::GetSampler under SOTC_HEAP). The GC keeps samplers that hold a slot.
    if (sampler.bindless_slot == VideoCore::BindlessSamplers::NoSlot) {
        return NullSamplerIndex();
    }
    return sampler.bindless_slot;
}

} // namespace SotcArch::Heap
