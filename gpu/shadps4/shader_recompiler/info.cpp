// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: dynamic_tsharp_array_size from Pink-shadPS4 (luizgustavs): T# tables a shader indexes at
// run time (SotC's local IBL reads each reflection probe's cubemap from such a table).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "common/elf_info.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"

namespace Shader {

void Info::RefreshSharpTables() {
    constexpr VAddr AddressMask = 0xFFFFFFFFFFFFULL;
    auto* memory = Core::Memory::Instance();
    auto& flattened_ud_buf = FlatBuf();
    for (const SharpTable& table : sharp_tables) {
        u32* dst = flattened_ud_buf.data() + table.flatbuf_off;
        const u64 size = u64{table.NumDwords()} * sizeof(u32);
        const VAddr ptr = (VAddr{flattened_ud_buf[table.ptr_lo]} |
                           VAddr{flattened_ud_buf[table.ptr_hi]} << 32) &
                          AddressMask;
        const VAddr src = ptr + u64{table.offset_dw} * sizeof(u32);
        // Through the backing: a garbage table pointer may point at free address space, where a
        // direct read is a host access violation. The tables are written by the CPU, so the
        // backing is current.
        if (ptr == 0 || !memory->TryReadBacking(src, dst, size)) {
            // Null T#s: the array binds null images, like the unflattened sharp did
            std::memset(dst, 0, size);
        }
    }
}

bool StageBindingsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_STAGE_BINDINGS");
        return v && v[0] == '1';
    }();
    return enabled;
}

static std::atomic<bool> g_bindless_samplers{false};

void SetBindlessSamplersEnabled(bool enabled) {
    g_bindless_samplers.store(enabled, std::memory_order_relaxed);
}

bool BindlessSamplersEnabled() {
    return g_bindless_samplers.load(std::memory_order_relaxed);
}

bool UsesBindlessSamplers(const Info& info) {
    // sotc: SOTC_HEAP: a plain stage of a heap pipeline reads its samplers through descriptors.
    if (!BindlessSamplersEnabled() || info.samplers.empty() || info.variant == StageVariantPlain) {
        return false;
    }
    return info.samplers.size() <= GetBindlessSamplerRange(info.sw_stage).count;
}

// sotc: SOTC_N5_BINDLESS (see runtime_info.h). Like the sampler flag, a process-wide setting.
static std::atomic<bool> g_bindless_images{false};

void SetBindlessImagesEnabled(bool enabled) {
    g_bindless_images.store(enabled, std::memory_order_relaxed);
}

bool BindlessImagesEnabled() {
    return g_bindless_images.load(std::memory_order_relaxed);
}

u32 BindlessImageKeyOf(const Info& info, const ImageResource& image) noexcept {
    // Sampled and non-atomic storage images. Atomics keep their descriptors (typed image ops with
    // a format in the SPIR-V type), and so do descriptor arrays and mip-fallback images, which
    // index descriptors in ways the heap does not cover.
    if (!BindlessImagesEnabled() || image.is_atomic || image.array_size != 0 ||
        image.mip_fallback_mode != MipStorageFallbackMode::None) {
        return NO_BINDLESS_IMAGE_KEY;
    }
    // The same SPIR-V type ImageType() gives the descriptor: the view type of the T# decides the
    // dimension, the number class the sampled type (ImageType in spirv_emit_context.cpp).
    const auto sharp = image.GetSharp(info);
    const auto number_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
    u32 base{};
    switch (number_class) {
    case AmdGpu::NumberClass::Float:
        base = 0;
        break;
    case AmdGpu::NumberClass::Uint:
        base = 3;
        break;
    default:
        return NO_BINDLESS_IMAGE_KEY; // sint: no heap array
    }
    u32 shape{};
    switch (sharp.GetViewType(image.is_array)) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color2D:
        shape = 0;
        break;
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube: // a 2D array image in SPIR-V
        shape = 1;
        break;
    case AmdGpu::ImageType::Color3D:
        shape = 2;
        break;
    default:
        return NO_BINDLESS_IMAGE_KEY; // msaa and anything unknown
    }
    // Storage images: the same shapes in the second half of the keys. The format is not part of
    // their type (Unknown in ImageType, non-atomic), so no key depends on it.
    return base + shape + (image.is_written ? NUM_BINDLESS_SAMPLED_IMAGE_KEYS : 0u);
}

u32 BindlessImageSlotOf(const Info& info, u32 index) noexcept {
    // sotc: SOTC_HEAP: a plain stage of a heap pipeline keeps the descriptors of its images.
    if (!BindlessImagesEnabled() || info.variant == StageVariantPlain) {
        return NO_BINDLESS_IMAGE_SLOT;
    }
    // Slots go to the eligible images in order, up to the count of the stage; the rest keep
    // their descriptors. Both the emitter and the host call this with the same Info.
    const auto range = GetBindlessImageRange(info.sw_stage);
    u32 next = 0;
    for (u32 i = 0; i < info.images.size() && next < range.count; ++i) {
        if (BindlessImageKeyOf(info, info.images[i]) == NO_BINDLESS_IMAGE_KEY) {
            continue;
        }
        if (i == index) {
            return range.base + next;
        }
        ++next;
    }
    return NO_BINDLESS_IMAGE_SLOT;
}

bool UsesBindlessImages(const Info& info) noexcept {
    for (u32 i = 0; i < info.images.size(); ++i) {
        if (BindlessImageSlotOf(info, i) != NO_BINDLESS_IMAGE_SLOT) {
            return true;
        }
    }
    return false;
}

// sotc: SOTC_HEAP. The most images a heap stage takes (the memo's block bound, TableMemo kMaxImages).
constexpr u32 MaxHeapStageImages = 16;

bool HeapStaticRefusal(const Info& info) noexcept {
    if (!info.fmasks.empty() || info.images.size() > MaxHeapStageImages ||
        info.samplers.size() > NUM_SAMPLERS || (info.images.empty() && info.samplers.empty())) {
        return true;
    }
    for (u32 i = 0; i < info.images.size(); ++i) {
        const auto& image = info.images[i];
        // Storage images, descriptor arrays and mip fallbacks keep their descriptors (never in the heap).
        if (image.is_written || image.array_size != 0 ||
            image.mip_fallback_mode != MipStorageFallbackMode::None ||
            BindlessImageSlotOf(info, i) == NO_BINDLESS_IMAGE_SLOT) {
            return true;
        }
    }
    // The samplers come from the sampler heap, through the index table.
    return !info.samplers.empty() && !UsesBindlessSamplers(info);
}

bool LodFeedbackEnabled() {
    static const bool enabled = [] {
        // Off by default: using the measured mips made the game's streaming churn (one 150 s
        // run at 21 FPS average, 22 Oct). SOTC_LOD_FEEDBACK=1 turns it on for tests.
        const char* v = std::getenv("SOTC_LOD_FEEDBACK");
        return v && v[0] == '1';
    }();
    return enabled;
}

// SOTC_TSHARP_ARRAY=<n> (0 = off); SotC defaults to 16 like Pink's preset.
u32 DynamicTsharpArraySize() {
    static const u32 size = [] {
        if (const char* v = std::getenv("SOTC_TSHARP_ARRAY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        return Common::ElfInfo::Instance().IsSotc() ? 16u : 0u;
    }();
    return std::min(size, MAX_TSHARP_ARRAY_SIZE);
}

} // namespace Shader
