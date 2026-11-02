// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A4 table memo (SOTC_HEAP, ARCH-DRAW.md part 3). The command processor resolves a stage's T#/S# list
// to heap indices once per table content, instead of the draw thread resolving every binding of every draw.
//
// INDEX TABLE LAYOUT (read by the recompiler's heap path, ARCH-DRAW section 11, and by the packet, section 12).
// The table is an array of u32, 4 bytes per entry, in this order:
//   entry i                  = heap index of stage.images[i]: the view BindTexturesFull's second pass binds for
//                              that image (after its depth redirect and rebind), in the stage's list order;
//   entry images.size() + j  = heap index of stage.samplers[j]: the sampler BindSamplers binds for that S#.
// count = images.size() + samplers.size(). A null T# (address 0 or invalid format) or a null image entry holds
// specialist 9's NullImageIndex(key) (the key is Shader::BindlessImageKeyOf of the image), never InvalidIndex,
// which the shader does not guard. A null sampler cannot reach the index table: Answerable refuses invalid S#s.
// The table's device address is 16-byte aligned: the sections of the ring are 4 MiB apart and every table starts
// on a 16-byte boundary.
//
// PROPOSED CONTRACT CHANGE (sotc_arch.h must change before this file links into the call site):
//  - Resolve cannot take (table_dwords, stage_hash). The resolution needs the stage's Shader::Info (its image and
//    sampler resources decide which dwords are T#/S#, which flags they carry and where their sharps sit in the
//    flat buffer), the texture cache, and the draw's border color base. This file therefore implements
//        IndexTable TableMemo::Resolve(VideoCore::TextureCache&, const Shader::Info& stage,
//                                      AmdGpu::BorderColorBuffer border_color);
//    and the key is the T#/S# words the stage reads (a subset of the table dwords: unrelated words change no
//    resolution, so they must not miss the memo).
//  - Init(const Vulkan::Instance&, Vulkan::Scheduler&) is needed once: the ring is a device-addressed buffer, and
//    the scheduler gives the tick that frames the ring sections.
//
// Validity of a stored entry (checked on each hit, the same conditions as the texture block memo):
//  - the T#/S# words, the stage's identity (sw stage, pgm hash, counts), the border color base;
//  - per image: ImageAddrGeneration of its guest address, and both the found and the bound image are still the
//    registered images (their uid), neither is dirty and the bound one does not need a rebind;
//  - per sampler: SamplerGeneration (a destroyed sampler bumps it);
//  - per image slot (sotc, after a heap generation change only): each heap slot the entry names still holds the
//    view it was made for (BindlessImages::Holds). The generation alone is global and moves on every view release;
//  - a mip-tail candidate also needs BindGeneration and gpu_modified_epoch unchanged.
//
// Refused (no table, the draw path binds as today): storage or array images, mip fallbacks, fmasks, samplers the
// full bind rewrites, T#s whose guest mapping or format is not valid (a guest map may make them valid), perfect
// finds that are not perfect, stencil images, the UI linear sampler rewrite, the composite probes. Refused tables
// are counted.
//
// Side effects of a miss: the lookups of the first pass of BindTexturesFull (FindTextureIdentity, FindImage,
// GetAssociatedDepth), its rebind, FindTexture with the update (UpdateImage), the bound flag and the texture usage
// bit, the LOD marks, and GetSampler. Not made: the layout transition (A1 keeps every image in GENERAL), the
// per-draw flags of ResetBindings. A hit stamps the found images' access ticks as TryTextureBlock does.
//
// Real mode (SOTC_HEAP=1, binds != nullptr in Resolve): the classic bind of the heap stage is skipped (the
// Rasterizer's BindHeapImages). Resolve then also covers what the classic second pass does per bind: a hit runs
// TouchBoundImage on each bound image (UpdateImage's work, or its touch and bind evidence where that is a no-op),
// then checks the entry again; the entry records the bound image and view range of each T#, and Resolve hands
// them to the caller. The caller makes the bound flags, the barriers (Runtime::Transit) and the bound list.
// Hits are refused when a heap slot the entry names no longer holds its view (after a heap generation change; the
// miss is counted as "slot"), or the depth image of a stencil image changed. Counted: the skipped and the classic
// stage binds (NoteBindSkipped, NoteBindClassic).
//
// Switch: SOTC_HEAP (0 off, 1 on, 2 verify: a hit is resolved again and the indices compared; the fresh ones are
// used). Unset: none of this runs.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

#include "common/types.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_bindless.h" // sotc: BindlessImages::Holds (per-slot validity of a hit)
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/texture_cache/texture_cache.h"

namespace SotcArch {
// Specialist 9's null descriptors (heap.cpp). Declared here until their header carries them.
} // namespace SotcArch

namespace SotcArch::TableMemo {
namespace {

// The bounds of Rasterizer::MaxBlockImages / MaxBlockSamplers: a larger stage takes the draw path.
constexpr u32 kMaxImages = 16;
constexpr u32 kMaxSamplers = Shader::NUM_SAMPLERS;
constexpr u32 kMaxBindings = kMaxImages + kMaxSamplers;
constexpr u32 kKeyWords = 3 + 5 * kMaxImages + 3 * kMaxSamplers;
constexpr u32 kEntries = 4096;          // power of two; open addressing
constexpr u32 kProbe = 8;               // probe window; the least recently used entry of it is replaced
constexpr u32 kSections = 4;            // ring sections, one per tick modulo kSections
constexpr u64 kSectionBytes = 4ull << 20;
constexpr u64 kRingAlign = 16;

static_assert(sizeof(AmdGpu::Image) == 4 * sizeof(u64));
static_assert(sizeof(AmdGpu::Sampler) == 2 * sizeof(u64));

enum class Miss : u8 { None, Absent, Candidate, Generation, Slot, Dirty, Sampler, Unmapped };

struct ImageRecord {
    bool null{};
    /// sotc: null because the guest mapping of the T# was not valid when it was resolved: a later map can make it
    /// valid, so a hit checks the mapping again (CheckHit). Other nulls are decided by the key words.
    bool unmapped{};
    bool lod{};
    /// sotc: bound is the depth image of found (a stencil attachment read through it).
    bool redirect{};
    u32 lod_bank{};
    VideoCore::ImageId found{};
    VideoCore::ImageId bound{};
    u64 found_uid{};
    u64 bound_uid{};
    VAddr addr{};
    u64 addr_gen{};
    /// sotc: the view range of the bind (the transition of the classic bind uses it).
    VideoCore::SubresourceRange range{};
    /// sotc: the heap key of the image and the view its heap slot (the Entry's index of this image) held when the
    /// op resolved. A hit after a heap generation change checks that the slot still holds this view (SlotsMiss).
    u32 key{};
    vk::ImageView view{};
};

struct Entry {
    bool valid{};
    bool candidate{};
    bool has_samplers{};
    u32 key_words{};
    u64 hash{};
    std::array<u64, kKeyWords> key{};
    u64 bind_generation{};
    u64 gpu_epoch{};
    u64 sampler_generation{};
    /// sotc: the heap generation the entry was last validated at. A release or an eviction moves the generation
    /// (global), and then the entry is checked slot by slot (SlotsMiss) and, when it holds, adopts the new one.
    u64 bindless_gen{};
    u32 num_images{};
    u32 num_bindings{};
    std::array<ImageRecord, kMaxImages> images{};
    std::array<u32, kMaxBindings> indices{};
    u64 last_use{};
    bool ringed{};     // the ring copy below is valid for ring_tick
    u64 ring_tick{};
    u64 ring_offset{}; // byte offset of the ring copy in the ring buffer
};

struct Section {
    u64 tick{};
    u64 used{};
};

struct Counters {
    std::atomic<u64> lookups{};
    std::atomic<u64> hits{};
    std::atomic<u64> miss_absent{};
    std::atomic<u64> miss_candidate{};
    std::atomic<u64> miss_generation{};
    /// sotc: a heap generation change where a slot the entry names no longer holds its view (SlotsMiss).
    std::atomic<u64> miss_slot{};
    std::atomic<u64> miss_dirty{};
    std::atomic<u64> miss_sampler{};
    std::atomic<u64> miss_unmapped{};
    /// sotc: SOTC_HEAP: T#s and S#s answered with the null slots instead of a refusal (fresh resolutions).
    std::atomic<u64> null_answers{};
    std::atomic<u64> null_unmapped{};
    std::atomic<u64> refused{};
    std::atomic<u64> not_stored{};
    std::atomic<u64> stores{};
    std::atomic<u64> evictions{};
    std::atomic<u64> ring_overflow{};
    std::atomic<u64> verify{};
    std::atomic<u64> verify_mismatch{};
    /// sotc: SOTC_HEAP real mode: heap-stage ops whose classic texture bind was skipped, and those that ran it
    /// (by reason, see NoteBindClassic).
    std::atomic<u64> bind_skipped{};
    std::array<std::atomic<u64>, 5> bind_classic{};
    /// sotc: SOTC_HEAP_VS: ops with a heap vertex stage whose image binds were skipped, and those that fell back to the
    /// classic bind (NoteVertexBind).
    std::atomic<u64> vs_skipped{};
    std::atomic<u64> vs_classic{};
};

struct State {
    bool ready{};
    Vulkan::Scheduler* scheduler{};
    // Process lifetime (never destroyed): the ring must not be released after the VMA allocator.
    VideoCore::Buffer* ring{};
    std::unique_ptr<Entry[]> entries;
    std::array<Section, kSections> sections{};
    u64 use_counter{};
    Counters counters;
};

State s;

int Mode() {
    static const int mode = SwitchMode("SOTC_HEAP");
    return mode;
}

// The composite and UI rules that BindTexturesFull applies outside the memo (vk_rasterizer.cpp: TakeHudless,
// DLSS probe, UiLinearAsset). A stage they can touch takes the draw path.
bool CompositeRule(const Shader::Info& stage) {
    // sotc: SOTC_HEAP_COMPOSITE_OP=1: the composite draws are sent to the classic bind per op (BindHeapImages).
    if (Heap::CompositeOpOn()) {
        return false;
    }
    static const bool probe = [] {
        const char* h = std::getenv("SOTC_FG_HUDLESS");
        return (h && h[0] == '1') || std::getenv("SOTC_DLSS_PROBE") != nullptr;
    }();
    return stage.sw_stage == Shader::SwStage::Fragment && stage.images.size() <= 2 &&
           (probe || Vulkan::Dlssg::SuperResolution());
}

bool UiLinearRule(const Shader::Info& stage) {
    static const bool ui_linear = [] {
        const char* r = std::getenv("SOTC_RENDER_RES");
        return r && *r && std::strcmp(r, "1920x1080") != 0;
    }();
    return ui_linear && stage.images.size() == 1;
}

// The stage is answerable unless it is refused: the static rules and the host rules are decided at pipeline creation
// (Heap::StageRefused, the same predicate the recompiler uses). The dynamic cases (an unmapped T#, an invalid S#)
// are answered with the null slots in ResolveFresh, so a heap stage never drops an op.
bool Answerable(const Shader::Info& stage) {
    return !Heap::StageRefused(stage);
}

u64 KeyOf(const Shader::Info& stage, AmdGpu::BorderColorBuffer border_color,
          std::array<u64, kKeyWords>& key) {
    u32 n = 0;
    key[n++] = u64{stage.images.size()} | u64{stage.samplers.size()} << 8 |
               u64(static_cast<u32>(stage.sw_stage)) << 24;
    key[n++] = stage.pgm_hash;
    key[n++] = border_color.Address();
    for (const auto& image_desc : stage.images) {
        const AmdGpu::Image tsharp = image_desc.GetSharp(stage);
        key[n++] = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                   u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                   u64{image_desc.constant_mip_index} << 8 |
                   u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
        std::memcpy(&key[n], &tsharp, sizeof(tsharp));
        n += 4;
    }
    for (const auto& sampler : stage.samplers) {
        const AmdGpu::Sampler ssharp = sampler.GetSharp(stage);
        key[n++] = u64{sampler.is_depth};
        std::memcpy(&key[n], &ssharp, sizeof(ssharp));
        n += 2;
    }
    u64 hash = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < n; ++i) {
        hash = (hash ^ key[i]) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= hash >> 29;
    }
    return hash;
}

u32 KeyWordsOf(const Shader::Info& stage) {
    return 3 + 5 * static_cast<u32>(stage.images.size()) + 3 * static_cast<u32>(stage.samplers.size());
}

Entry* FindEntry(u64 hash, const std::array<u64, kKeyWords>& key, u32 words) {
    for (u32 p = 0; p < kProbe; ++p) {
        Entry& e = s.entries[(hash + p) & (kEntries - 1)];
        if (e.valid && e.hash == hash && e.key_words == words &&
            std::memcmp(e.key.data(), key.data(), words * sizeof(u64)) == 0) {
            return &e;
        }
    }
    return nullptr;
}

// The slot a new entry for this key goes to: the entry holding the key, else an empty one of the probe window,
// else the least recently used one (replaced).
Entry& SlotFor(u64 hash, const std::array<u64, kKeyWords>& key, u32 words) {
    if (Entry* same = FindEntry(hash, key, words)) {
        return *same;
    }
    Entry* victim = nullptr;
    for (u32 p = 0; p < kProbe; ++p) {
        Entry& e = s.entries[(hash + p) & (kEntries - 1)];
        if (!e.valid) {
            return e;
        }
        if (!victim || e.last_use < victim->last_use) {
            victim = &e;
        }
    }
    s.counters.evictions.fetch_add(1, std::memory_order_relaxed);
    return *victim;
}

// The heap generation moved since the entry was validated (a view was released or a slot evicted, anywhere in the
// heap). The entry still answers while each slot it names holds the view it was made for: a slot that another view
// took, or one given back and not yet reclaimed, is what the generation guards against. Checked per slot, so a
// change elsewhere in the heap does not force a fresh resolution. Checks only; the heap lock is taken per image.
Miss SlotsMiss(const Entry& e) {
    VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
    if (!heap) {
        return Miss::Generation;
    }
    for (u32 i = 0; i < e.num_images; ++i) {
        const ImageRecord& r = e.images[i];
        if (r.null) { // the null slot holds no view and its index is static
            continue;
        }
        const u32 slot = e.indices[i];
        // Slot 0 with a non-null record: the heap was full (or absent) when it resolved. Not replayed.
        if (slot == 0 || !heap->Holds(r.key, slot, r.view, vk::ImageLayout::eGeneral)) {
            return Miss::Slot;
        }
    }
    return Miss::None;
}

// Why a stored entry no longer answers, or Miss::None when it does. Checks only; nothing is changed.
Miss CheckHit(VideoCore::TextureCache& tc, const Entry& e, u64 bindless_gen) {
    // A released or evicted heap slot since the entry was validated: the indices may name a slot given back.
    if (e.bindless_gen != bindless_gen) {
        const Miss slot_miss = SlotsMiss(e);
        if (slot_miss != Miss::None) {
            return slot_miss;
        }
    }
    if (e.candidate && (tc.BindGeneration() != e.bind_generation ||
                        VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) != e.gpu_epoch)) {
        return Miss::Candidate;
    }
    if (e.has_samplers && tc.SamplerGeneration() != e.sampler_generation) {
        return Miss::Sampler;
    }
    for (u32 i = 0; i < e.num_images; ++i) {
        const ImageRecord& r = e.images[i];
        if (r.unmapped) { // a guest map made the T# valid: the draw path binds its view now
            if (Core::Memory::Instance()->IsValidGpuMapping(r.addr, 0)) {
                return Miss::Unmapped;
            }
            continue;
        }
        if (r.null) {
            continue;
        }
        if (tc.ImageAddrGeneration(r.addr) != r.addr_gen) {
            return Miss::Generation;
        }
        VideoCore::Image* found = tc.TryGetImage(r.found, r.found_uid);
        const VideoCore::Image* bound = tc.TryGetImage(r.bound, r.bound_uid);
        if (!found || !bound) {
            return Miss::Generation;
        }
        // The depth image of a stencil one can appear or go away (the bind redirects through it, as TryTextureBlock).
        if (tc.GetAssociatedDepth(*found) != (r.redirect ? r.bound : VideoCore::ImageId{})) {
            return Miss::Generation;
        }
        const auto dirty = VideoCore::ImageFlagBits::Dirty;
        if ((found->flags & dirty) != VideoCore::ImageFlagBits::Empty ||
            (bound->flags & dirty) != VideoCore::ImageFlagBits::Empty ||
            bound->binding.needs_rebind) {
            return Miss::Dirty;
        }
    }
    return Miss::None;
}

// Resolves the stage as the bind does (see the file comment for the list of calls). Fills `out` with the
// entries of the index table and `rec` with what a later hit checks. `store` is cleared when this resolution
// must not be replayed (an overlap resolution, a stencil image, a rebind).
void ResolveFresh(VideoCore::TextureCache& tc, const Shader::Info& stage,
                  AmdGpu::BorderColorBuffer border_color, u64 bindless_gen,
                  std::array<u32, kMaxBindings>& out, Entry& rec, bool& store) {
    store = true;
    rec.num_images = static_cast<u32>(stage.images.size());
    rec.has_samplers = !stage.samplers.empty();
    rec.bindless_gen = bindless_gen;
    rec.bind_generation = tc.BindGeneration();
    rec.gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
    rec.candidate = false;
    const u32 num_images = rec.num_images;

    for (u32 index = 0; index < num_images; ++index) {
        const auto& image_desc = stage.images[index];
        ImageRecord& r = rec.images[index];
        r = {};
        // The heap key of the image: static per stage image, computed once for the op (every path below uses it).
        const u32 key = Shader::BindlessImageKeyAt(stage, index);
        r.key = key;
        const AmdGpu::Image tsharp = image_desc.GetSharp(stage);
        if (tsharp.lod_hw_cnt_en) {
            r.lod = true;
            r.lod_bank = tsharp.counter_bank_id;
            SotcLod::Mark(tsharp.counter_bank_id); // GET_LOD_STATS, as the first pass
        }
        if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            r.null = true;
            out[index] = Heap::NullImageIndex(key);
            s.counters.null_answers.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // The classic bind rejects a T# with invalid enums or an invalid guest mapping and binds a null descriptor
        // (BindTexturesFull). The heap answers with the null slot, the same way. The mapping can change: such a
        // record is checked again on each hit (CheckHit). The enums are part of the key, so they are static.
        const bool valid_enums = magic_enum::enum_contains(tsharp.GetDataFmt()) &&
                                 magic_enum::enum_contains(tsharp.GetNumberFmt());
        if (!valid_enums || !Core::Memory::Instance()->IsValidGpuMapping(tsharp.Address(), 0)) {
            r.null = true;
            r.unmapped = valid_enums;
            r.addr = tsharp.Address();
            out[index] = Heap::NullImageIndex(key);
            s.counters.null_answers.fetch_add(1, std::memory_order_relaxed);
            if (valid_enums) {
                s.counters.null_unmapped.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        // The first pass of BindTexturesFull: the texture identity lookup of a sampled T#.
        VideoCore::TextureCache::ImageDesc desc(tsharp, image_desc);
        VideoCore::TextureCache::TexIdentityKey tkey{};
        std::memcpy(tkey.words.data(), &tsharp, sizeof(tsharp));
        tkey.extra = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                     u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                     u64{image_desc.constant_mip_index} << 8 |
                     u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
        r.found = tc.FindTextureIdentity(tkey, desc).image_id;
        if (!r.found) {
            store = false;
            out[index] = Heap::NullImageIndex(key);
            r.null = true;
            s.counters.null_answers.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        r.addr = tsharp.Address();
        r.addr_gen = tc.ImageAddrGeneration(r.addr);
        if (!tc.IsPerfectFind(r.found, desc.info)) {
            store = false; // an overlap resolution, not a perfect match
        }
        const VideoCore::Image& found = tc.GetImage(r.found);
        r.found_uid = found.image_uid;
        if (found.info.sotc_first_mip != 0 ||
            (desc.rebase_mip != 0 && VideoCore::MipTailMode() != 0)) {
            rec.candidate = true;
        }
        r.bound = r.found;
        if (auto depth = tc.GetAssociatedDepth(tc.GetImage(r.found))) {
            r.bound = depth; // a stencil attachment is read through its depth image
        }
        tc.GetImage(r.bound).binding.is_bound = 1u;

        // The second pass: a rebind (the image was updated after the first lookup), then the view with
        // its update, and the texture usage bit.
        if (tc.GetImage(r.bound).binding.needs_rebind) {
            tc.GetImage(r.bound).binding = {};
            store = false;
            r.bound = tc.FindImage(desc);
            if (!r.bound) {
                out[index] = Heap::NullImageIndex(key);
                r.null = true;
                s.counters.null_answers.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        auto& bound = tc.GetImage(r.bound);
        if (bound.info.props.has_stencil) {
            store = false; // its transition depends on the draw's depth registers
        }
        VideoCore::ImageView& view = tc.FindTexture(r.bound, desc, true);
        bound.usage.texture |= 1u;
        r.bound_uid = bound.image_uid;
        // The heap descriptor is written in GENERAL: the memo makes no layout transition, so SOTC_HEAP needs
        // SOTC_LAYOUT_UNIFIED (every texture-cache image in GENERAL).
        out[index] = Heap::IndexOf(view, key, vk::ImageLayout::eGeneral);
        // The view IndexOf wrote (the slot holds it in eGeneral): SlotsMiss checks the slot still holds it.
        r.view = view.image_view ? *view.image_view : vk::ImageView{};
        r.range = desc.view_info.range; // the classic bind's transition range
        r.redirect = r.bound != r.found;
    }

    for (u32 j = 0; j < stage.samplers.size(); ++j) {
        const auto& sampler = stage.samplers[j];
        const AmdGpu::Sampler ssharp = sampler.GetSharp(stage);
        // An invalid S# (or a custom border without its base) answers with the null sampler slot: the classic bind
        // rewrites it as well and logs it. Static: both inputs are in the key.
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                border_color.Address() == 0)) {
            out[num_images + j] = Heap::NullSamplerIndex();
            s.counters.null_answers.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        u32 slot = Heap::NullSamplerIndex();
        (void)tc.GetSampler(ssharp, border_color, sampler.is_depth, &slot); // the bindless sampler slot
        out[num_images + j] = slot;
    }
    rec.num_bindings = num_images + static_cast<u32>(stage.samplers.size());
    rec.sampler_generation = tc.SamplerGeneration();
}

// The section of the ring for `tick`. A section is reused only after the tick it held has completed.
Section& SectionFor(u64 tick) {
    Section& sec = s.sections[tick % kSections];
    if (sec.tick != tick) {
        if (sec.tick && !s.scheduler->IsFree(sec.tick)) {
            s.scheduler->Wait(sec.tick);
        }
        sec.tick = tick;
        sec.used = 0;
    }
    return sec;
}

// The index table of `tick`: the entry's copy when it was written this tick, else a new copy of `indices`.
// Returns {0, 0} when the ring section is full: the stage then takes the draw path.
IndexTable Publish(Entry* entry, const std::array<u32, kMaxBindings>& indices, u32 count, u64 tick) {
    if (entry && entry->ringed && entry->ring_tick == tick) {
        return {s.ring->BufferDeviceAddress() + entry->ring_offset, count};
    }
    Section& sec = SectionFor(tick);
    const u64 bytes = u64{count} * sizeof(u32);
    const u64 used = (sec.used + kRingAlign - 1) & ~(kRingAlign - 1);
    if (used + bytes > kSectionBytes) {
        s.counters.ring_overflow.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const u64 offset = (tick % kSections) * kSectionBytes + used;
    std::memcpy(s.ring->mapped_data.data() + offset, indices.data(), bytes);
    s.ring->Flush(offset, bytes);
    sec.used = used + bytes;
    if (entry) {
        entry->ringed = true;
        entry->ring_tick = tick;
        entry->ring_offset = offset;
    }
    return {s.ring->BufferDeviceAddress() + offset, count};
}

// The images of an entry for the op (SOTC_HEAP real mode): the bound image of each non-null T#, with its range.
void FillBinds(const Entry& e, TableBinds* binds) {
    binds->count = e.num_images;
    for (u32 i = 0; i < e.num_images; ++i) {
        binds->image[i] = e.images[i].null ? VideoCore::ImageId{} : e.images[i].bound;
        binds->range[i] = e.images[i].range;
    }
}

} // namespace

bool On() {
    return Heap::On();
}

void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    if (!On() || s.ready) {
        return;
    }
    // A device-addressed, host-visible ring: the shader reads the indices through the address the
    // packet carries. Without buffer device addresses the memo stays off.
    auto* ring = new VideoCore::Buffer(instance, 0, kSections * kSectionBytes,
                                       VideoCore::MemoryType::HostCached, "SotC Table Memo");
    if (!ring->HasBufferDeviceAddress() || ring->mapped_data.empty()) {
        delete ring;
        std::fprintf(stderr, "SOTC TABLEMEMO: off (no buffer device address or mapped ring)\n");
        return;
    }
    s.ring = ring;
    s.scheduler = &scheduler;
    s.entries = std::make_unique<Entry[]>(kEntries);
    s.ready = true;
}

IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color, u64 bindless_gen, TableBinds* binds) {
    if (!s.ready) {
        return {};
    }
    if (!Answerable(stage)) { // never for a stage the pipeline cache chose as heap (Heap::StageRefused)
        s.counters.refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const auto lock = texture_cache.BatchLock();
    const u32 words = KeyWordsOf(stage);
    std::array<u64, kKeyWords> key{};
    const u64 hash = KeyOf(stage, border_color, key);
    const u32 count = static_cast<u32>(stage.images.size() + stage.samplers.size());
    const u64 tick = s.scheduler->CurrentTick();
    const int mode = Mode();
    s.counters.lookups.fetch_add(1, std::memory_order_relaxed);

    Entry* hit = FindEntry(hash, key, words);
    Miss miss = Miss::Absent;
    if (hit) {
        miss = CheckHit(texture_cache, *hit, bindless_gen);
        // sotc: SOTC_HEAP real mode. The classic second pass runs UpdateImage on every bound image at every bind, so a
        // hit does it too (TouchBoundImage: the same work where it changes nothing). The check runs again after it:
        // the update may dirty an image or give back a heap slot, and then the op resolves afresh.
        if (miss == Miss::None && binds) {
            for (u32 i = 0; i < hit->num_images; ++i) {
                if (!hit->images[i].null) {
                    texture_cache.TouchBoundImage(hit->images[i].bound);
                }
            }
            miss = CheckHit(texture_cache, *hit, bindless_gen);
        }
    }
    std::array<u32, kMaxBindings> hit_indices{};
    if (hit && miss == Miss::None) {
        s.counters.hits.fetch_add(1, std::memory_order_relaxed);
        hit->last_use = ++s.use_counter;
        // The slots were checked against this generation (CheckHit): later ops at it skip the per-slot check.
        hit->bindless_gen = bindless_gen;
        // The replayed slots count as used now, as IndexOf's Use does on a fresh resolution: otherwise Evict could
        // take a slot whose last use is old while the draws of this tick still read it.
        VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
        for (u32 i = 0; i < hit->num_images; ++i) {
            const ImageRecord& r = hit->images[i];
            if (heap && !r.null) {
                heap->NoteUse(r.key, hit->indices[i]);
            }
            if (r.lod) {
                SotcLod::Mark(r.lod_bank); // the first pass marks every bind
            }
            if (!r.null) { // the stamps TryTextureBlock leaves on the found images
                VideoCore::Image& found = texture_cache.GetImage(r.found);
                found.tick_prev_accessed = found.tick_accessed_last;
                found.tick_accessed_last = tick;
            }
        }
        if (mode != 2) {
            if (binds) {
                FillBinds(*hit, binds);
            }
            return Publish(hit, hit->indices, count, tick);
        }
        hit_indices = hit->indices;
    } else {
        switch (miss) {
        case Miss::Absent: s.counters.miss_absent.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Candidate: s.counters.miss_candidate.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Generation: s.counters.miss_generation.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Slot: s.counters.miss_slot.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Dirty: s.counters.miss_dirty.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Sampler: s.counters.miss_sampler.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Unmapped: s.counters.miss_unmapped.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::None: break;
        }
    }

    // The resolution. In verify mode a would-be hit is compared with it, and the fresh indices are used.
    std::array<u32, kMaxBindings> indices{};
    Entry fresh{};
    bool store = false;
    ResolveFresh(texture_cache, stage, border_color, bindless_gen, indices, fresh, store);
    if (hit && miss == Miss::None) {
        s.counters.verify.fetch_add(1, std::memory_order_relaxed);
        if (!std::equal(hit_indices.begin(), hit_indices.begin() + count, indices.begin())) {
            s.counters.verify_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!store) {
        if (hit) {
            hit->valid = false; // a stale answer must not be replayed
        }
        s.counters.not_stored.fetch_add(1, std::memory_order_relaxed);
        if (binds) {
            FillBinds(fresh, binds);
        }
        return Publish(nullptr, indices, count, tick);
    }
    fresh.valid = true;
    fresh.hash = hash;
    fresh.key_words = words;
    fresh.key = key;
    fresh.indices = indices;
    fresh.last_use = ++s.use_counter;
    Entry& slot = SlotFor(hash, key, words);
    slot = fresh;
    s.counters.stores.fetch_add(1, std::memory_order_relaxed);
    if (binds) {
        FillBinds(slot, binds);
    }
    return Publish(&slot, indices, count, tick);
}

void NoteBindSkipped() {
    s.counters.bind_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteBindClassic(u32 reason) {
    // sotc: 4 is the composite op (SOTC_HEAP_COMPOSITE_OP); anything past the array counts there too.
    s.counters.bind_classic[std::min<u32>(reason, 4u)].fetch_add(1, std::memory_order_relaxed);
}

void NoteVertexBind(bool skipped) {
    (skipped ? s.counters.vs_skipped : s.counters.vs_classic).fetch_add(1, std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    if (!On() || size == 0) {
        if (size) {
            out[0] = '\0';
        }
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    u32 live = 0;
    if (s.entries) {
        for (u32 i = 0; i < kEntries; ++i) {
            live += s.entries[i].valid ? 1u : 0u;
        }
    }
    const u64 lookups = take(s.counters.lookups);
    const u64 hits = take(s.counters.hits);
    std::snprintf(
        out, size,
        "SOTC TABLEMEMO %.1f s: lookups %llu, hits %llu (%.0f%%), misses: absent %llu, "
        "generation %llu, slot %llu, dirty %llu, sampler %llu, candidate %llu, unmapped %llu; refused %llu, "
        "not stored %llu; stored %llu, evicted %llu, live %u/%u; null answers %llu (unmapped T# %llu); "
        "ring overflow %llu; verify %llu, mismatches %llu; stage binds skipped %llu, classic "
        "(verify %llu, target %llu, stencil %llu, rebind %llu, composite %llu); heap vertex skipped %llu, "
        "classic %llu\n",
        seconds, static_cast<unsigned long long>(lookups), static_cast<unsigned long long>(hits),
        lookups ? 100.0 * static_cast<double>(hits) / static_cast<double>(lookups) : 0.0,
        static_cast<unsigned long long>(take(s.counters.miss_absent)),
        static_cast<unsigned long long>(take(s.counters.miss_generation)),
        static_cast<unsigned long long>(take(s.counters.miss_slot)),
        static_cast<unsigned long long>(take(s.counters.miss_dirty)),
        static_cast<unsigned long long>(take(s.counters.miss_sampler)),
        static_cast<unsigned long long>(take(s.counters.miss_candidate)),
        static_cast<unsigned long long>(take(s.counters.miss_unmapped)),
        static_cast<unsigned long long>(take(s.counters.refused)),
        static_cast<unsigned long long>(take(s.counters.not_stored)),
        static_cast<unsigned long long>(take(s.counters.stores)),
        static_cast<unsigned long long>(take(s.counters.evictions)), live, kEntries,
        static_cast<unsigned long long>(take(s.counters.null_answers)),
        static_cast<unsigned long long>(take(s.counters.null_unmapped)),
        static_cast<unsigned long long>(take(s.counters.ring_overflow)),
        static_cast<unsigned long long>(take(s.counters.verify)),
        static_cast<unsigned long long>(take(s.counters.verify_mismatch)),
        static_cast<unsigned long long>(take(s.counters.bind_skipped)),
        static_cast<unsigned long long>(take(s.counters.bind_classic[0])),
        static_cast<unsigned long long>(take(s.counters.bind_classic[1])),
        static_cast<unsigned long long>(take(s.counters.bind_classic[2])),
        static_cast<unsigned long long>(take(s.counters.bind_classic[3])),
        static_cast<unsigned long long>(take(s.counters.bind_classic[4])),
        static_cast<unsigned long long>(take(s.counters.vs_skipped)),
        static_cast<unsigned long long>(take(s.counters.vs_classic)));
}

} // namespace SotcArch::TableMemo

namespace SotcArch::Heap {

bool StageRefused(const Shader::Info& stage) {
    // The static rules (the recompiler's HeapStaticRefusal) and the composite and UI rules of the draw path, which
    // the memo does not answer. Both are fixed for the run, so the pipeline cache decides once per program.
    return Shader::HeapStaticRefusal(stage) || TableMemo::CompositeRule(stage) ||
           TableMemo::UiLinearRule(stage);
}

} // namespace SotcArch::Heap
