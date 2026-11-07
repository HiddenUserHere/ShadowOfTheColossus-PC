// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: dynamic_tsharp_array_size from Pink-shadPS4 (luizgustavs): T# tables a shader indexes at
// run time (SotC's local IBL reads each reflection probe's cubemap from such a table).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/classes.h"
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

// sotc: SOTC_CP_SRT_PLAN=2: the walker's guest reads are recorded (srt.h). Walker code is the same;
// its page test passes for every page, so each guest load calls TraceClean, which records it and
// declines (the walker then loads the value itself).
bool SrtPlanTraceEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_CP_SRT_PLAN");
        return v && v[0] == '2';
    }();
    return enabled;
}

namespace {
thread_local std::vector<std::pair<u64, u32>> t_srt_trace;

bool TraceClean(u64 address, u32 size, u64*) {
    t_srt_trace.emplace_back(address, size);
    return false;
}
} // namespace

const SrtWalkerContext* SrtPlanTraceContext() {
    static const SrtWalkerContext context = [] {
        // One bit per 4 KiB page (as WalkerContext), all set: the walker calls TraceClean for every
        // load below SrtCleanPageLimit. Never freed.
        constexpr u64 words = SrtCleanPageLimit >> 18;
        auto* bits = static_cast<u64*>(std::malloc(words * sizeof(u64)));
        ASSERT_MSG(bits, "No memory for the SRT trace page bitmap");
        std::memset(bits, 0xFF, words * sizeof(u64));
        return SrtWalkerContext{bits, &TraceClean};
    }();
    return &context;
}

void SrtCopyWindowTraced(u32* dst, u64 src, u64 bytes) {
    t_srt_trace.emplace_back(src, static_cast<u32>(bytes));
    SrtCopyWindow(dst, src, bytes);
}

void SrtTraceReset() {
    t_srt_trace.clear();
}

void SrtTraceTake(std::vector<std::pair<u64, u32>>& out, u64 skip_begin, u64 skip_end) {
    out.clear();
    for (const auto& [address, size] : t_srt_trace) {
        if (address >= skip_begin && address < skip_end) {
            continue;
        }
        out.emplace_back(address, size);
    }
    t_srt_trace.clear();
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
    // sotc: the cached answer of a complete Info (FillBindlessCache); the rule below is its source.
    if (info.bindless.ready) {
        return info.bindless.bindless_samplers;
    }
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

// sotc: SOTC_BINDLESS_CACHE_VERIFY=1 compares every cached key used per draw with the rule. The key used stays the
// cached one; mismatches are counted and printed by ReportBindlessCache.
static bool BindlessCacheVerifyEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_BINDLESS_CACHE_VERIFY");
        return v && v[0] == '1';
    }();
    return enabled;
}
static std::atomic<u64> g_bindless_checks{};
static std::atomic<u64> g_bindless_mismatches{};
static std::atomic<u64> g_bindless_unstable{};

u32 BindlessImageKeyAt(const Info& info, u32 index) noexcept {
    if (info.bindless.ready && index < info.images.size()) {
        const u32 key = info.bindless.image_key[index];
        if (BindlessCacheVerifyEnabled()) {
            g_bindless_checks.fetch_add(1, std::memory_order_relaxed);
            if (key != BindlessImageKeyOf(info, info.images[index])) {
                g_bindless_mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return key;
    }
    return BindlessImageKeyOf(info, info.images[index]);
}

u32 BindlessImageSlotOf(const Info& info, u32 index) noexcept {
    // sotc: the cached slot of a complete Info (FillBindlessCache); the rule below is its source.
    if (info.bindless.ready && index < info.images.size()) {
        return info.bindless.image_slot[index];
    }
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
    if (info.bindless.ready) {
        return info.bindless.bindless_images;
    }
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
    if (info.bindless.ready) {
        return info.bindless.heap_refused;
    }
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

bool HeapStageRule(const Info& info) noexcept {
    // sotc: SOTC_HEAP. Only whole stages switch: a stage with an image or sampler outside the heap
    // keeps its push slots, and the table is then never read.
    // Only the heap variant of a stage (chosen by the pipeline cache, which also applies the
    // host rules) is a heap stage, and a stage with a static refusal never is.
    if (info.variant != StageVariantHeap || HeapStaticRefusal(info)) {
        return false;
    }
    if (info.images.empty() && info.samplers.empty()) {
        return false;
    }
    for (u32 i = 0; i < info.images.size(); ++i) {
        // Storage images stay on their descriptors: the index table carries sampled images only.
        if (info.images[i].is_written || BindlessImageSlotOf(info, i) == NO_BINDLESS_IMAGE_SLOT) {
            return false;
        }
    }
    return info.samplers.empty() || UsesBindlessSamplers(info);
}

static bool SameBindlessAnswers(const Info::BindlessCache& a, const Info::BindlessCache& b) {
    return a.num_images == b.num_images && a.num_buffers == b.num_buffers &&
           a.bindless_samplers == b.bindless_samplers && a.bindless_images == b.bindless_images &&
           a.heap_refused == b.heap_refused && a.heap_stage == b.heap_stage &&
           a.shader_addr_buffers == b.shader_addr_buffers && a.image_key == b.image_key &&
           a.image_slot == b.image_slot;
}

void CombineBindlessCache(Info::BindlessCache& stored, const Info::BindlessCache& fresh) {
    const auto mark_unstable = [&stored] {
        stored = {};
        stored.unstable = true;
        g_bindless_unstable.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Render_Recompiler, "SOTC bindless cache: permutations of a program disagree on image "
                                    "keys or slots; its shaders use the per-draw rules from now on");
    };
    if (stored.unstable) {
        stored.ready = false;
        return;
    }
    if (!fresh.filled || fresh.unstable) {
        mark_unstable();
        return;
    }
    if (!stored.filled) {
        stored = fresh;
        stored.ready = true;
        return;
    }
    if (!SameBindlessAnswers(stored, fresh)) {
        mark_unstable();
        return;
    }
    stored.ready = true;
}

void Info::FillBindlessCache() {
    // sotc: SOTC_BARRIER_STATIC. The static bit of the barrier rule: a written buffer (stores and atomics, GDS
    // appends included) or a written image (storage writes and image atomics) makes the stage a writer. Computed here
    // because this runs once the Info is complete, both after a translation and after a deserialization.
    writes_memory = false;
    for (const auto& buffer : buffers) {
        writes_memory |= buffer.is_written;
    }
    for (const auto& image : images) {
        writes_memory |= image.is_written;
    }
    // The rules above read the cache when it is ready, so it is off while the answers are computed.
    bindless.ready = false;
    BindlessCache fresh{};
    fresh.num_images = images.size();
    fresh.num_buffers = buffers.size();
    for (u32 i = 0; i < images.size(); ++i) {
        fresh.image_key[i] = BindlessImageKeyOf(*this, images[i]);
        fresh.image_slot[i] = BindlessImageSlotOf(*this, i);
    }
    fresh.bindless_samplers = UsesBindlessSamplers(*this);
    fresh.bindless_images = UsesBindlessImages(*this);
    fresh.heap_refused = HeapStaticRefusal(*this);
    fresh.heap_stage = HeapStageRule(*this);
    for (u32 i = 0; i < buffers.size(); ++i) {
        if (ShaderAddrCandidate(*this, buffers[i])) {
            fresh.shader_addr_buffers |= u64{1} << i;
        }
    }
    fresh.filled = true;
    fresh.ready = true;
    CombineBindlessCache(bindless, fresh);
}

// sotc: the SOTC_BINDLESS_CACHE_VERIFY line of the 5 s report: checks and mismatches since the previous call.
void ReportBindlessCache(std::string& out, double seconds) {
    if (!BindlessCacheVerifyEnabled()) {
        return;
    }
    static u64 prev_checks{};
    static u64 prev_mismatches{};
    const u64 checks = g_bindless_checks.load(std::memory_order_relaxed);
    const u64 mismatches = g_bindless_mismatches.load(std::memory_order_relaxed);
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC BINDLESS CACHE verify: %.1f s, checks %llu, mismatches %llu, unstable programs %llu\n",
                  seconds, static_cast<unsigned long long>(checks - prev_checks),
                  static_cast<unsigned long long>(mismatches - prev_mismatches),
                  static_cast<unsigned long long>(g_bindless_unstable.load(std::memory_order_relaxed)));
    out += line;
    prev_checks = checks;
    prev_mismatches = mismatches;
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
