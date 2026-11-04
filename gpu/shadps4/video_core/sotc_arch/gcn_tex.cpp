// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GCN_TEX (ARCH-GCN-NATIVE S3; decisions D7-D9 in GCN-NATIVE-DECISIONS.md). The GPU T#/S# table: a hash
// table in the globals buffer that maps (T# words, view class) to the heap slot of the view, and (S# words, sampler
// class) to the heap slot of the sampler. The shader probes it; the host fills it on the CPU's first sight of a
// binding (NoteResolved / NoteResolvedSampler) and on a shader miss (OnImageMiss / OnSamplerMiss, from the miss ring).
//
// Ownership: the CPU keeps a shadow of every entry (state, key, slot, image) and does all its bookkeeping on the
// shadow. The mapping is written only (plus Lookup and FindMapped, the verify read). Every mutation runs with the
// texture cache lock held or with the bindless heap lock held (the Release/Evict hooks); g_mutex serialises the shadow
// between them. Lock order: texture cache -> bindless heap -> g_mutex. Nothing takes g_mutex and then calls out of
// this file, except the texture cache lock is never taken under g_mutex.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h> // _mm_sfence
#endif

#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/memory.h"
#include "shader_recompiler/gcn_layout.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/sotc_arch/bindset_events.h"
#include "video_core/sotc_arch/gcn_native.h"
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/texture_cache/texture_cache.h"

namespace SotcGcn::Tex {
namespace {

using Shader::Gcn::IsSamplerClass;
using Shader::Gcn::kTexMaxProbes;
using Shader::Gcn::kTexTableCapacity;
using Shader::Gcn::TexEntry;

constexpr u32 kCap = kTexTableCapacity;
constexpr u32 kMask = kCap - 1;
constexpr u32 kNone = ~0u;
// Link key of the sampler entries: image keys are 16 bits (ViewClass), so this cannot collide with one.
constexpr u32 kSamplerLinkKey = 0xFFFFFFFFu;
constexpr u32 kMaxLogged = 16;

enum class CpuState : u8 { Empty, Live, Tombstone };

// CPU replica of one table entry. Bookkeeping reads this, never the mapping.
struct Shadow {
    u32 words[8]{};
    u32 view_class{};
    u32 slot{};
    CpuState state{CpuState::Empty};
    bool has_image{};
    VideoCore::ImageId image{};
    u64 image_uid{}; ///< 0 when the resolution did not supply the uid (NoteResolved): the image is then not checked
};

struct State {
    std::mutex mutex; ///< shadow, links and every mapping write
    std::vector<Shadow> shadow = std::vector<Shadow>(kCap);
    /// (link key << 32 | slot) -> shadow indices of the live entries that name the heap slot.
    std::unordered_multimap<u64, u32> links;
    /// texture cache image index -> shadow indices of the live entries whose binding is that image.
    std::unordered_multimap<u32, u32> image_links;
    std::atomic<u32> live{0};
    std::once_flag prepared;
    std::atomic<u32> logged{0};

    std::atomic<u64> inserts{0};
    std::atomic<u64> tombstones{0};
    std::atomic<u64> image_resolved{0};
    std::atomic<u64> image_failed{0};
    std::atomic<u64> sampler_misses{0};
    std::atomic<u64> full{0};
    std::atomic<u64> match{0};
    std::atomic<u64> mismatch{0};
    std::atomic<u64> absent{0};
};

State& S() {
    static State state;
    return state;
}

u64 LinkOf(u32 link_key, u32 slot) {
    return (u64{link_key} << 32) | slot;
}

u32 LinkKeyOf(u32 view_class) {
    return IsSamplerClass(view_class) ? kSamplerLinkKey : view_class >> 8;
}

// The table in the globals buffer, nullptr when the buffer does not exist.
TexEntry* Entries() {
    u8* mapped = Globals::Mapped();
    return mapped ? reinterpret_cast<TexEntry*>(mapped + Shader::Gcn::kTexTableOffset) : nullptr;
}

template <typename T>
std::atomic_ref<T> Ref(T& x) {
    return std::atomic_ref<T>(x);
}

// Every entry starts Empty: the buffer is not assumed to be zeroed.
void PrepareTable() {
    TexEntry* base = Entries();
    if (!base) {
        return;
    }
    for (u32 i = 0; i < kCap; ++i) {
        Ref(base[i].state).store(Shader::Gcn::kTexEmpty, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
}

bool SameWords(const u32* a, const u32* b, u32 n) {
    return std::memcmp(a, b, n * sizeof(u32)) == 0;
}

// The default samplers (heap full, or no slot) are not a sampler of any S#.
bool IsDefaultSampler(u32 slot) {
    return slot == VideoCore::BindlessSamplers::DefaultSlot(false) ||
           slot == VideoCore::BindlessSamplers::DefaultSlot(true);
}

// Probe window on the shadow. `found` is the live entry with the key, `free` the first position an insert may take
// (a tombstone, or the empty entry that ends the chain). Both kCap when absent.
struct Probe {
    u32 found{kCap};
    u32 free{kCap};
};

Probe ProbeShadow(const u32* words, u32 n, u32 view_class) {
    std::vector<Shadow>& sh = S().shadow;
    Probe p;
    const u32 h = Shader::Gcn::Hash(words, n, view_class);
    for (u32 i = 0; i < kTexMaxProbes; ++i) {
        const u32 idx = (h + i) & kMask;
        const Shadow& e = sh[idx];
        if (e.state == CpuState::Live) {
            if (e.view_class == view_class && SameWords(e.words, words, n)) {
                p.found = idx;
                return p;
            }
            continue;
        }
        if (p.free == kCap) {
            p.free = idx;
        }
        if (e.state == CpuState::Empty) {
            break;
        }
    }
    return p;
}

// The mapping's answer (the shader's view of the table): the slot of the live entry, or kNone.
u32 FindMapped(const u32* words, u32 n, u32 view_class) {
    TexEntry* base = Entries();
    if (!base) {
        return kNone;
    }
    const u32 h = Shader::Gcn::Hash(words, n, view_class);
    for (u32 i = 0; i < kTexMaxProbes; ++i) {
        TexEntry& e = base[(h + i) & kMask];
        const u32 state = Ref(e.state).load(std::memory_order_acquire);
        if (state == Shader::Gcn::kTexLive) {
            bool same = Ref(e.view_class).load(std::memory_order_relaxed) == view_class;
            for (u32 w = 0; same && w < n; ++w) {
                same = Ref(e.words[w]).load(std::memory_order_relaxed) == words[w];
            }
            if (same) {
                return Ref(e.slot).load(std::memory_order_relaxed);
            }
            continue;
        }
        if (state == Shader::Gcn::kTexEmpty) {
            break;
        }
    }
    return kNone;
}

// Removes the heap-slot link of shadow entry `idx`.
void UnlinkSlotLocked(State& s, u32 idx) {
    const Shadow& e = s.shadow[idx];
    const u64 link = LinkOf(LinkKeyOf(e.view_class), e.slot);
    auto range = s.links.equal_range(link);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == idx) {
            s.links.erase(it);
            return;
        }
    }
}

void LinkSlotLocked(State& s, u32 idx) {
    const Shadow& e = s.shadow[idx];
    s.links.emplace(LinkOf(LinkKeyOf(e.view_class), e.slot), idx);
}

// Removes the image link of shadow entry `idx` (only when it has one: call before its image changes).
void UnlinkImageLocked(State& s, u32 idx) {
    const Shadow& e = s.shadow[idx];
    if (!e.has_image) {
        return;
    }
    auto range = s.image_links.equal_range(e.image.index);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == idx) {
            s.image_links.erase(it);
            return;
        }
    }
}

void LinkImageLocked(State& s, u32 idx) {
    const Shadow& e = s.shadow[idx];
    if (e.has_image) {
        s.image_links.emplace(e.image.index, idx);
    }
}

void StoreSlot(TexEntry* base, u32 idx, u32 slot) {
    if (base) {
        Ref(base[idx].slot).store(slot, std::memory_order_release);
    }
}

// Writes the entry (new, or the slot and image of a live one with the same key). g_mutex held. False when the window is
// full or the table does not exist. The mapping is written words, class, slot, then state (release) last.
bool PutLocked(State& s, const u32* words, u32 n, u32 view_class, u32 slot, VideoCore::ImageId image,
               u64 image_uid) {
    TexEntry* base = Entries();
    if (!base) {
        return false;
    }
    const bool has_image = static_cast<bool>(image);
    const Probe p = ProbeShadow(words, n, view_class);
    if (p.found != kCap) {
        Shadow& e = s.shadow[p.found];
        if (e.slot != slot) {
            UnlinkSlotLocked(s, p.found);
            e.slot = slot;
            LinkSlotLocked(s, p.found);
            StoreSlot(base, p.found, slot);
        }
        UnlinkImageLocked(s, p.found);
        e.has_image = has_image;
        e.image = image;
        e.image_uid = image_uid;
        LinkImageLocked(s, p.found);
        return true;
    }
    if (p.free == kCap) {
        s.full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u32 idx = p.free;
    TexEntry& m = base[idx];
    for (u32 w = 0; w < 8; ++w) {
        Ref(m.words[w]).store(w < n ? words[w] : 0u, std::memory_order_relaxed);
    }
    Ref(m.view_class).store(view_class, std::memory_order_relaxed);
    Ref(m.slot).store(slot, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
#if defined(__x86_64__) || defined(_M_X64)
    // sotc: SOTC_GCN_TEX. A release fence is only a compiler barrier on x86; the mapping may be write-combined
    // (device-local host-visible memory), where the stores can leave the CPU out of order. sfence keeps the state last.
    _mm_sfence();
#endif
    Ref(m.state).store(Shader::Gcn::kTexLive, std::memory_order_release);

    Shadow& e = s.shadow[idx];
    for (u32 w = 0; w < 8; ++w) {
        e.words[w] = w < n ? words[w] : 0u;
    }
    e.view_class = view_class;
    e.slot = slot;
    e.state = CpuState::Live;
    e.has_image = has_image;
    e.image = image;
    e.image_uid = image_uid;
    LinkSlotLocked(s, idx);
    LinkImageLocked(s, idx);
    s.live.fetch_add(1, std::memory_order_relaxed);
    s.inserts.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Entry `idx` no longer names a valid view. g_mutex held.
void TombstoneLocked(State& s, u32 idx) {
    UnlinkSlotLocked(s, idx);
    UnlinkImageLocked(s, idx);
    Shadow& e = s.shadow[idx];
    e.state = CpuState::Tombstone;
    e.has_image = false;
    if (TexEntry* base = Entries()) {
        Ref(base[idx].state).store(Shader::Gcn::kTexTombstone, std::memory_order_release);
    }
    s.live.fetch_sub(1, std::memory_order_relaxed);
    s.tombstones.fetch_add(1, std::memory_order_relaxed);
}

// Null T# or unresolvable: the same test the heap path makes (table_memo.cpp ResolveFresh).
bool UsableTSharp(const AmdGpu::Image& tsharp) {
    if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
        return false;
    }
    const bool valid_enums = magic_enum::enum_contains(tsharp.GetDataFmt()) &&
                             magic_enum::enum_contains(tsharp.GetNumberFmt());
    return valid_enums && Core::Memory::Instance()->IsValidGpuMapping(tsharp.Address(), 0);
}

// The sampler the heap path builds from an S# and its class: the post op of SamplerResource::GetSharp (the class bits
// carry it, bits 1..6), with DisableAnisoIfSingleLod's run-time fact in bit 7 instead of the T# it reads.
AmdGpu::Sampler SamplerOf(const u32 (&words)[4], u32 sampler_class) {
    AmdGpu::Sampler sampler{};
    std::memcpy(&sampler, words, sizeof(words));
    const auto post_op = static_cast<Shader::SharpFetchPostOp>((sampler_class >> 1) & 0x3Fu);
    if (post_op == Shader::SharpFetchPostOp::DisableAnisoIfSingleLod) {
        if (sampler_class & 0x80u) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
        }
    } else if (post_op == Shader::SharpFetchPostOp::ForceRepeatXyzClamp) {
        sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
        sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
        sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
    } else if (post_op == Shader::SharpFetchPostOp::ForceLastTexelXyClamp) {
        sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
        sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
        sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
    } else if (post_op == Shader::SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
        sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
        sampler.aniso_threshold.Assign(0);
    }
    return sampler;
}

// The sampler the table may hold: valid, and not one whose colours or rewrite the key does not carry. A custom border
// needs the border colour base (not in the key); point magnification is rewritten by the UI linear rule under the stage.
bool SamplerTableable(const AmdGpu::Sampler& sampler) {
    if (!sampler.Valid() || sampler.border_color_type.Value() == AmdGpu::BorderColor::Custom) {
        return false;
    }
    // Only a heap stage can be UI-linear eligible (SOTC_HEAP_UI_LINEAR=1, HeapOn); without it the stage is refused as a
    // heap stage, so no heap stage gets the point-to-bilinear rewrite and the point sampler is genuinely point.
    return !(SotcArch::UiLinear::HeapOn() && SotcArch::UiLinear::RenderResOn() &&
             sampler.xy_mag_filter.Value() == AmdGpu::Filter::Point);
}

// The shared part of NoteResolved and NoteResolvedSampler. g_mutex held.
void NoteLocked(State& s, const u32* words, u32 n, u32 view_class, u32 slot, VideoCore::ImageId image) {
    if (Verify()) {
        const u32 got = FindMapped(words, n, view_class);
        if (got == slot) {
            s.match.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (got == kNone) {
            s.absent.fetch_add(1, std::memory_order_relaxed);
        } else {
            s.mismatch.fetch_add(1, std::memory_order_relaxed);
            if (s.logged.fetch_add(1, std::memory_order_relaxed) < kMaxLogged) {
                LOG_WARNING(Render,
                            "SOTC GCN TEX: verify mismatch: table slot {} (class {:#x}), CPU slot {}", got,
                            view_class, slot);
            }
        }
    } else {
        const Probe p = ProbeShadow(words, n, view_class);
        if (p.found != kCap && s.shadow[p.found].slot == slot) {
            return;
        }
    }
    // Absent, or the table names another slot than the one the CPU just resolved: the fresh answer wins.
    PutLocked(s, words, n, view_class, slot, image, 0);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------

bool On() {
    if (Mode(Stage::Tex) == 0) {
        return false;
    }
    if (!Entries()) {
        Refuse(Stage::Tex, "the globals buffer is not created");
        return false;
    }
    std::call_once(S().prepared, PrepareTable);
    return true;
}

bool Verify() {
    return Mode(Stage::Tex) == 2;
}

u8 ProfileValue() {
    const int mode = Mode(Stage::Tex);
    if (mode == 0) {
        return 0;
    }
    if (!SotcArch::Heap::On()) {
        Refuse(Stage::Tex, "needs SOTC_HEAP on");
        return 0;
    }
    if (!SotcArch::Layouts::Active()) {
        Refuse(Stage::Tex, "needs the unified image layouts active");
        return 0;
    }
    if (!Entries()) {
        Refuse(Stage::Tex, "the globals buffer is not created");
        return 0;
    }
    // The UI linear rewrite of a point sampler is decided per op (ResolveFresh); the table cannot carry it.
    if (SotcArch::UiLinear::RenderResOn() && SotcArch::UiLinear::HeapOn()) {
        Refuse(Stage::Tex, "SOTC_HEAP_UI_LINEAR=1 needs the per-op rewrite");
        return 0;
    }
    return static_cast<u8>(mode);
}

u32 ViewClassOf(const Shader::Info& info, const Shader::ImageResource& desc) {
    return Shader::Gcn::ViewClass(desc.is_written, desc.is_depth, desc.is_array,
                                  Shader::BindlessImageKeyOf(info, desc));
}

u32 SamplerClassOf(const Shader::SamplerResource& desc, bool single_lod) {
    const bool single = single_lod && desc.post_op == Shader::SharpFetchPostOp::DisableAnisoIfSingleLod;
    return Shader::Gcn::SamplerClass(desc.is_depth, static_cast<u32>(desc.post_op), single);
}

bool Insert(const u32 (&tsharp)[8], u32 view_class, u32 slot) {
    if (!On()) {
        return false;
    }
    // Slot 0 is the null view of every key: an entry for it would hide a miss.
    if (slot == 0 || IsSamplerClass(view_class) || (view_class >> 8) >= Shader::NUM_BINDLESS_IMAGE_KEYS) {
        return false;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    return PutLocked(s, tsharp, 8, view_class, slot, {}, 0);
}

bool InsertSampler(const u32 (&ssharp)[4], u32 sampler_class, u32 slot) {
    if (!On() || !IsSamplerClass(sampler_class) || IsDefaultSampler(slot)) {
        return false;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    return PutLocked(s, ssharp, 4, sampler_class, slot, {}, 0);
}

u32 Lookup(const u32 (&tsharp)[8], u32 view_class) {
    if (!On()) {
        return kNone;
    }
    return FindMapped(tsharp, 8, view_class);
}

void OnSlotReleased(u32 key, u32 slot) {
    if (Mode(Stage::Tex) == 0) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    const auto range = s.links.equal_range(LinkOf(key, slot));
    std::vector<u32> victims;
    for (auto it = range.first; it != range.second; ++it) {
        victims.push_back(it->second);
    }
    for (const u32 idx : victims) {
        TombstoneLocked(s, idx);
    }
}

void OnSamplerSlotReleased(u32 slot) {
    OnSlotReleased(kSamplerLinkKey, slot);
}

void OnImageMiss(VideoCore::TextureCache& texture_cache, const u32 (&tsharp_words)[8], u32 view_class) {
    if (!On()) {
        return;
    }
    State& s = S();
    const u32 key = view_class >> 8;
    AmdGpu::Image tsharp{};
    std::memcpy(&tsharp, tsharp_words, sizeof(tsharp));
    if (IsSamplerClass(view_class) || key >= Shader::NUM_BINDLESS_IMAGE_KEYS || !UsableTSharp(tsharp)) {
        s.image_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The resolution the heap path makes for one binding (table_memo.cpp ResolveFresh). The class is the only
    // part of the resource the miss knows: the other flags (atomic, r128, mip fallback, post op) take their
    // defaults here.
    const auto lock = texture_cache.BatchLock();
    if (tsharp.lod_hw_cnt_en) {
        SotcLod::Mark(tsharp.counter_bank_id);
    }
    Shader::ImageResource res{};
    res.is_written = (view_class & 1u) != 0;
    res.is_depth = (view_class & 2u) != 0;
    res.is_array = (view_class & 4u) != 0;

    VideoCore::TextureCache::ImageDesc desc(tsharp, res);
    VideoCore::TextureCache::TexIdentityKey tkey{};
    std::memcpy(tkey.words.data(), &tsharp, sizeof(tsharp));
    tkey.extra = u64{res.is_depth} | (u64{res.is_array} << 2);
    const VideoCore::ImageId found = texture_cache.FindTextureIdentity(tkey, desc).image_id;
    if (!found) {
        s.image_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    VideoCore::ImageId bound = found;
    if (const auto depth = texture_cache.GetAssociatedDepth(texture_cache.GetImage(found))) {
        bound = depth; // a stencil attachment is read through its depth image
    }
    texture_cache.GetImage(bound).binding.is_bound = 1u;
    if (texture_cache.GetImage(bound).binding.needs_rebind) {
        texture_cache.GetImage(bound).binding = {};
        bound = texture_cache.FindImage(desc);
        if (!bound) {
            s.image_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    VideoCore::ImageView& view = texture_cache.FindTexture(bound, desc, true);
    texture_cache.GetImage(bound).usage.texture |= 1u;

    // GENERAL: the same layout the heap path writes (SOTC_LAYOUT_UNIFIED keeps every image there).
    const u32 slot = SotcArch::Heap::IndexOf(view, key, vk::ImageLayout::eGeneral);
    if (slot == SotcArch::Heap::NullImageIndex(key)) {
        s.image_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const VideoCore::Image& image = texture_cache.GetImage(bound);
    std::scoped_lock lk{s.mutex};
    if (PutLocked(s, tsharp_words, 8, view_class, slot, bound, image.image_uid)) {
        s.image_resolved.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.image_failed.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnSamplerMiss(VideoCore::TextureCache& texture_cache, const u32 (&ssharp_words)[4], u32 sampler_class) {
    if (!On()) {
        return;
    }
    State& s = S();
    s.sampler_misses.fetch_add(1, std::memory_order_relaxed);
    if (!IsSamplerClass(sampler_class)) {
        return;
    }
    const AmdGpu::Sampler ssharp = SamplerOf(ssharp_words, sampler_class);
    // Refused (no slot is inserted, the shader keeps the null sampler): see SamplerTableable.
    if (!SamplerTableable(ssharp)) {
        return;
    }
    const auto lock = texture_cache.BatchLock();
    // The border colour base is not known here: a non-custom border needs none, so the sampler is built with no base.
    u32 slot = SotcArch::Heap::NullSamplerIndex();
    (void)texture_cache.GetSampler(ssharp, AmdGpu::BorderColorBuffer{}, (sampler_class & 1u) != 0, &slot);
    if (IsDefaultSampler(slot)) {
        return;
    }
    std::scoped_lock lk{s.mutex};
    PutLocked(s, ssharp_words, 4, sampler_class, slot, {}, 0);
}

void NoteResolved(const u32 (&tsharp)[8], u32 view_class, u32 slot, VideoCore::ImageId image) {
    if (!On()) {
        return;
    }
    if (IsSamplerClass(view_class) || slot == SotcArch::Heap::NullImageIndex(view_class >> 8)) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    NoteLocked(s, tsharp, 8, view_class, slot, image);
}

void NoteResolvedSampler(const u32 (&ssharp)[4], u32 sampler_class, u32 slot) {
    if (!On() || !IsSamplerClass(sampler_class) || IsDefaultSampler(slot)) {
        return;
    }
    if (!SamplerTableable(SamplerOf(ssharp, sampler_class))) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    NoteLocked(s, ssharp, 4, sampler_class, slot, {});
}

void OnImageEvent(u32 image_index, u8 cause) {
    if (!On()) {
        return;
    }
    // With Sync on, the submit batch refreshes a dirty image in place: the entries stay valid.
    if (static_cast<SotcArch::BindSet::Cause>(cause) == SotcArch::BindSet::Cause::ImageDirty && Sync::On()) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.mutex};
    const auto range = s.image_links.equal_range(image_index);
    std::vector<u32> victims;
    for (auto it = range.first; it != range.second; ++it) {
        victims.push_back(it->second);
    }
    for (const u32 idx : victims) {
        TombstoneLocked(s, idx);
    }
}

void ForEachReferencedImage(VideoCore::TextureCache& texture_cache, void (*fn)(void* user, VideoCore::Image&),
                            void* user) {
    if (!On() || S().live.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const auto lock = texture_cache.BatchLock();
    State& s = S();
    std::vector<std::pair<VideoCore::ImageId, u64>> images;
    {
        std::scoped_lock lk{s.mutex};
        std::unordered_set<u32> seen;
        for (const Shadow& e : s.shadow) {
            if (e.state != CpuState::Live || !e.has_image || !seen.insert(e.image.index).second) {
                continue;
            }
            images.emplace_back(e.image, e.image_uid);
        }
    }
    for (const auto& [id, uid] : images) {
        // uid 0: the entry was resolved by the CPU without its uid. Its image is valid while no ImageFree event has
        // tombstoned the entry (OnImageEvent), so the id alone is used.
        VideoCore::Image* image = uid ? texture_cache.TryGetImage(id, uid) : &texture_cache.GetImage(id);
        if (image) {
            fn(user, *image);
        }
    }
}

void StampLiveSlots() {
    if (Mode(Stage::Tex) == 0) {
        return;
    }
    VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
    if (!heap) {
        return;
    }
    // The distinct (key, slot) pairs of the live image links, copied out under g_mutex. Equal keys are adjacent in an
    // unordered_multimap, so one pass dedupes; a repeat is harmless anyway. Samplers are not stamped: the sampler heap
    // never releases or reuses a slot (no LRU eviction), so there is no use tick to keep.
    std::vector<std::pair<u32, u32>> live;
    {
        State& s = S();
        std::scoped_lock lk{s.mutex};
        u64 last = ~u64{0};
        for (const auto& [link, idx] : s.links) {
            (void)idx;
            if (link == last) {
                continue;
            }
            last = link;
            const u32 key = static_cast<u32>(link >> 32);
            if (key == kSamplerLinkKey) {
                continue;
            }
            live.emplace_back(key, static_cast<u32>(link));
        }
    }
    // Outside g_mutex: NoteUse takes no lock (draw thread only, see vk_bindless.h).
    for (const auto& [key, slot] : live) {
        heap->NoteUse(key, slot);
    }
}

void Report(char* out, u32 size, double seconds) {
    if (Mode(Stage::Tex) == 0) {
        if (size > 0) {
            out[0] = '\0';
        }
        return;
    }
    State& s = S();
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const double window = seconds > 0.0 ? seconds : 1.0;
    const u64 inserts = take(s.inserts);
    const u64 tombstones = take(s.tombstones);
    const u64 resolved = take(s.image_resolved);
    const u64 failed = take(s.image_failed);
    const u64 sampler = take(s.sampler_misses);
    const u64 full = take(s.full);
    const u64 match = take(s.match);
    const u64 mismatch = take(s.mismatch);
    const u64 absent = take(s.absent);
    std::snprintf(out, size,
                  "SOTC GCN TEX: entries live %u, inserts %.1f/s, tombstones %.1f/s, image misses resolved "
                  "%.1f/s (failed %llu), sampler misses %.1f/s, full %llu, verify match %llu mismatch %llu "
                  "absent %llu",
                  s.live.load(std::memory_order_relaxed), static_cast<double>(inserts) / window,
                  static_cast<double>(tombstones) / window, static_cast<double>(resolved) / window,
                  static_cast<unsigned long long>(failed), static_cast<double>(sampler) / window,
                  static_cast<unsigned long long>(full), static_cast<unsigned long long>(match),
                  static_cast<unsigned long long>(mismatch), static_cast<unsigned long long>(absent));
}

} // namespace SotcGcn::Tex
