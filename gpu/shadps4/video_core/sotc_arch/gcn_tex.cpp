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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
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

#include "../../../sotc/guest_mem.h" // sotc: SOTC_GCN_TEX_REGISTER_LOG (render target fields, backing view)

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
    u8 source{};     ///< sotc: SOTC_GCN_TEX_REGISTER, the Source that inserted the entry (Source::Op: the per-op path)
};

struct State {
    std::mutex mutex; ///< shadow, links and every mapping write
    std::vector<Shadow> shadow = std::vector<Shadow>(kCap);
    /// (link key << 32 | slot) -> shadow indices of the live entries that name the heap slot.
    std::unordered_multimap<u64, u32> links;
    /// texture cache image index -> shadow indices of the live entries whose binding is that image.
    std::unordered_multimap<u32, u32> image_links;
    /// sotc: SOTC_GCN_TEX_REGISTER only (empty otherwise): WordsKey of the T# -> shadow indices of the live image
    /// entries with those words, any class (the refresh finds the classes of an old T# through it).
    std::unordered_multimap<u64, u32> word_links;
    /// sotc: SOTC_GCN_TEX_REGISTER: T# shape signature (SigOf) -> class bits (ClassBit) the per-op path resolved.
    std::unordered_map<u32, u64> learned;
    /// sotc: SOTC_GCN_TEX_REGISTER: live entries inserted by a registration, per bindless key. Every live entry's slot is
    /// stamped each submit (StampLiveSlots), so registered entries pin heap slots: the drain stops at kRegisterSlotCap.
    std::array<u32, Shader::NUM_BINDLESS_IMAGE_KEYS> reg_live{};
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

// sotc: SOTC_GCN_TEX_REGISTER. Any source requested: the word links are kept (read once).
bool RegisterAny() {
    static const bool any = [] {
        for (u32 i = 1; i < static_cast<u32>(Source::Count); ++i) {
            if (RegisterRequested(static_cast<Source>(i))) {
                return true;
            }
        }
        return false;
    }();
    return any;
}

// sotc: SOTC_GCN_TEX_REGISTER. A 64-bit key of the 8 T# words (FNV-1a over the dwords); equal words give equal keys,
// the links still compare the words.
u64 WordsKey(const u32* words) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 w = 0; w < 8; ++w) {
        h = (h ^ words[w]) * 0x100000001b3ull;
    }
    return h;
}

void UnlinkWordsLocked(State& s, u32 idx) {
    const Shadow& e = s.shadow[idx];
    if (!RegisterAny() || IsSamplerClass(e.view_class)) {
        return;
    }
    auto range = s.word_links.equal_range(WordsKey(e.words));
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == idx) {
            s.word_links.erase(it);
            return;
        }
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
               u64 image_uid, u8 source = 0) {
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
    e.source = source;
    if (source != 0 && !IsSamplerClass(view_class) && (view_class >> 8) < Shader::NUM_BINDLESS_IMAGE_KEYS) {
        ++s.reg_live[view_class >> 8]; // sotc: SOTC_GCN_TEX_REGISTER
    }
    LinkSlotLocked(s, idx);
    LinkImageLocked(s, idx);
    if (RegisterAny() && n == 8) { // sotc: SOTC_GCN_TEX_REGISTER
        s.word_links.emplace(WordsKey(e.words), idx);
    }
    s.live.fetch_add(1, std::memory_order_relaxed);
    s.inserts.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Entry `idx` no longer names a valid view. g_mutex held.
void TombstoneLocked(State& s, u32 idx) {
    UnlinkSlotLocked(s, idx);
    UnlinkImageLocked(s, idx);
    UnlinkWordsLocked(s, idx); // sotc: SOTC_GCN_TEX_REGISTER
    Shadow& e = s.shadow[idx];
    if (e.source != 0 && !IsSamplerClass(e.view_class) && (e.view_class >> 8) < Shader::NUM_BINDLESS_IMAGE_KEYS &&
        s.reg_live[e.view_class >> 8] > 0) {
        --s.reg_live[e.view_class >> 8]; // sotc: SOTC_GCN_TEX_REGISTER
    }
    e.source = 0;
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
    // sotc: the mapping is ReBAR VRAM (uncached PCIe reads, ~10 loads per probe), so verify reads it for 1 resolve in
    // 64 and uses the host shadow (a mirror of every mapping write) for the rest; this cost 100-250 ms/s of draw time.
    static u32 verify_tick = 0;
    if (Verify() && (++verify_tick & 63) == 0) {
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

// ---------------------------------------------------------------------------------------------------------------
// sotc: SOTC_GCN_TEX_REGISTER (GCN-NATIVE-PLAN-2026-11-09.md step 2). The queue between the engine hooks (game threads)
// and the draw thread, the seen set that keeps per-bind hooks from queueing a T# again, the per-op hit counters (the
// gate for step 4) and the render-target log. Lock order: texture cache -> g_mutex; the queue mutex and the render
// target mutex are leaves (nothing is called while they are held).

constexpr u32 kSources = static_cast<u32>(Source::Count);
constexpr size_t kQueueCap = 16384;  ///< registrations waiting (a larger burst is dropped and counted)
constexpr size_t kRefreshCap = 8192; ///< distinct T# addresses with a pending refresh
constexpr u32 kSeenSlots = 16384;    ///< lock-free seen set of the hook side (power of two)
constexpr u32 kSeenProbes = 8;
constexpr u32 kRtLogMax = 24;   ///< render target log lines for the run
constexpr size_t kRtCap = 4096; ///< candidate base addresses of render targets kept for the log
constexpr u32 kMaxClasses = 64;
/// Registered live entries per bindless key at most (half of the heap array: the per-op path keeps the rest for LRU).
constexpr u32 kRegisterSlotCap = VideoCore::BindlessImageCapacity / 2;

struct PendingReg {
    u32 words[8];
    u8 source;
};
struct PendingRefresh {
    u32 old_words[8];
    u32 new_words[8];
};

struct RegState {
    std::mutex mutex;              ///< queue, head and refreshes
    std::vector<PendingReg> queue; ///< FIFO; the drain takes from `head`
    size_t head{};
    std::unordered_map<u64, PendingRefresh> refreshes; ///< by guest address of the T#
    std::atomic<size_t> pending{0}; ///< queue.size() - head + refreshes.size(), read without the lock
    std::array<std::atomic<u64>, kSeenSlots> seen{};

    std::mutex rt_mutex;                   ///< rt_bases, rt_logged
    std::unordered_map<u64, u64> rt_bases; ///< candidate base address -> render target object
    std::unordered_set<u64> rt_logged;     ///< (object, class) pairs already logged
    std::atomic<u32> rt_lines{0};
    std::atomic<u64> rt_objects{0};

    std::atomic<u64> queued[kSources]{};
    std::atomic<u64> inserted[kSources]{};
    std::atomic<u64> present[kSources]{};
    std::atomic<u64> failed[kSources]{};
    std::atomic<u64> dup{0};
    std::atomic<u64> invalid{0};
    std::atomic<u64> dropped{0};
    std::atomic<u64> no_class{0};
    std::atomic<u64> refresh_queued{0};
    std::atomic<u64> refresh_coalesced{0};
    std::atomic<u64> refresh_applied{0};
    std::atomic<u64> refresh_unknown{0};
    std::atomic<u64> refresh_tombstoned{0};
    std::atomic<u64> budget_stops{0};
    std::atomic<u64> slot_cap{0}; ///< classes not registered: kRegisterSlotCap reached for their key
    // The per-op path (TableMemo ResolveFresh -> NoteResolved): what its lookup found in the table.
    std::atomic<u64> op_lookups{0};
    std::atomic<u64> op_hit_registered{0}; ///< present with the same slot, inserted by a registration
    std::atomic<u64> op_hit_op{0};         ///< present with the same slot, inserted by the per-op path or a miss
    std::atomic<u64> op_slot_diff{0};      ///< present, another slot
    std::atomic<u64> op_absent_class{0};   ///< absent, but the words are live under another class (class guess)
    std::atomic<u64> op_absent{0};
};

RegState& R() {
    static RegState state;
    return state;
}

/// The source the current OnImageMiss call resolves for (DrainRegistered sets it around sink.image).
thread_local u8 t_source = 0;

// The shape signature of a T#: what the view class of a sampled binding depends on besides the binding's flags.
u32 SigOf(const AmdGpu::Image& t) {
    return (static_cast<u32>(t.GetType()) & 0xFu) | ((static_cast<u32>(t.GetDataFmt()) & 0x3Fu) << 4) |
           ((static_cast<u32>(t.GetNumberFmt()) & 0xFu) << 10);
}

// A sampled image class (TableImage: never written) as one of 4 * NUM_BINDLESS_IMAGE_KEYS bits; ~0u for any other.
u32 ClassBit(u32 view_class) {
    const u32 key = view_class >> 8;
    if (IsSamplerClass(view_class) || (view_class & 1u) || key >= Shader::NUM_BINDLESS_IMAGE_KEYS) {
        return ~0u;
    }
    return key * 4 + ((view_class >> 1) & 3u); // bit 0 is_depth, bit 1 is_array
}

u32 ClassOfBit(u32 bit) {
    return Shader::Gcn::ViewClass(false, (bit & 1u) != 0, (bit & 2u) != 0, bit / 4);
}

// The class the shader most likely uses for a T# never seen per op: not depth, array when the T# type is an array or
// a cube, with the bindless key BindlessImageKeyOf gives a sampled image of that shape (the same rule, info.cpp).
u32 GuessClass(const AmdGpu::Image& t) {
    if (!Shader::BindlessImagesEnabled()) {
        return ~0u;
    }
    const auto type = t.GetType();
    const bool is_array = type == AmdGpu::ImageType::Color1DArray || type == AmdGpu::ImageType::Color2DArray ||
                          type == AmdGpu::ImageType::Cube;
    u32 base{};
    switch (AmdGpu::GetNumberClass(t.GetNumberFmt())) {
    case AmdGpu::NumberClass::Float:
        base = 0;
        break;
    case AmdGpu::NumberClass::Uint:
        base = 3;
        break;
    default:
        return ~0u;
    }
    u32 shape{};
    switch (t.GetViewType(is_array)) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color2D:
        shape = 0;
        break;
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        shape = 1;
        break;
    case AmdGpu::ImageType::Color3D:
        shape = 2;
        break;
    default:
        return ~0u;
    }
    return Shader::Gcn::ViewClass(false, false, is_array, base + shape);
}

// The classes to register a T# under: every class the per-op path resolved for its shape, else the guess. g_mutex held.
u32 ClassesLocked(State& s, const u32 (&words)[8], u32 (&out)[kMaxClasses]) {
    AmdGpu::Image t{};
    std::memcpy(&t, words, sizeof(t));
    u32 n = 0;
    if (const auto it = s.learned.find(SigOf(t)); it != s.learned.end()) {
        for (u64 mask = it->second; mask != 0 && n < kMaxClasses; mask &= mask - 1) {
            out[n++] = ClassOfBit(static_cast<u32>(__builtin_ctzll(mask)));
        }
    }
    if (n == 0) {
        const u32 guess = GuessClass(t);
        if (guess != ~0u) {
            out[n++] = guess;
        }
    }
    return n;
}

// The live image entries with exactly these words (any class). g_mutex held.
u32 LiveWithWordsLocked(State& s, const u32 (&words)[8], u32 (&out)[kMaxClasses]) {
    u32 n = 0;
    const auto range = s.word_links.equal_range(WordsKey(words));
    for (auto it = range.first; it != range.second && n < kMaxClasses; ++it) {
        const Shadow& e = s.shadow[it->second];
        if (e.state == CpuState::Live && SameWords(e.words, words, 8)) {
            out[n++] = it->second;
        }
    }
    return n;
}

bool UsableWords(const u32 (&words)[8]) {
    AmdGpu::Image t{};
    std::memcpy(&t, words, sizeof(t));
    return t.Address() != 0 && t.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid;
}

// The hook-side dedupe. True when the words were not in the set (and are now). Benign races: a T# may be queued twice.
bool SeenInsert(RegState& r, u64 key) {
    const u64 tag = key | 1u; // 0 marks a free slot
    const u32 h = static_cast<u32>(key ^ (key >> 32));
    for (u32 i = 0; i < kSeenProbes; ++i) {
        std::atomic<u64>& slot = r.seen[(h + i) & (kSeenSlots - 1)];
        u64 cur = slot.load(std::memory_order_relaxed);
        if (cur == 0 && slot.compare_exchange_strong(cur, tag, std::memory_order_relaxed)) {
            return true;
        }
        if (cur == tag) {
            return false;
        }
    }
    return true; // the window is full: queue it (the drain skips a T# that is present)
}

std::string WordsText(const u32* words) {
    std::string out;
    char buf[12];
    for (u32 w = 0; w < 8; ++w) {
        std::snprintf(buf, sizeof buf, w ? " %08x" : "%08x", words[w]);
        out += buf;
    }
    return out;
}

// SOTC_GCN_TEX_REGISTER_LOG: the per-op resolve met a T# whose base is a candidate base of a render target object.
// Prints the op's words next to the object's four candidate 32-byte fields, read now. Draw thread, no lock of this
// file held.
void LogRenderTargetSample(const u32 (&words)[8], u32 view_class, u32 slot) {
    RegState& r = R();
    if (r.rt_lines.load(std::memory_order_relaxed) >= kRtLogMax) {
        return;
    }
    AmdGpu::Image t{};
    std::memcpy(&t, words, sizeof(t));
    u64 object = 0;
    {
        std::scoped_lock lk{r.rt_mutex};
        const auto it = r.rt_bases.find(t.Address());
        if (it == r.rt_bases.end() || !r.rt_logged.insert(it->second ^ (u64{view_class} << 40)).second) {
            return;
        }
        object = it->second;
    }
    const u32 line = r.rt_lines.fetch_add(1, std::memory_order_relaxed);
    if (line >= kRtLogMax) {
        return;
    }
    static constexpr u32 kFields[4] = {0xc4, 0xe4, 0x124, 0x164};
    std::string fields;
    std::string equal;
    for (const u32 off : kFields) {
        u32 f[8]{};
        const bool ok = SotcGuest::Read(object + off, f, sizeof(f));
        char head[24];
        std::snprintf(head, sizeof head, " | +%#x ", off);
        fields += head;
        fields += ok ? WordsText(f) : std::string("unreadable");
        char name[24];
        if (ok && SameWords(f, words, 8)) {
            std::snprintf(name, sizeof name, "%s+%#x", equal.empty() ? "" : ",", off);
            equal += name;
        } else if (ok && f[0] == words[0]) {
            std::snprintf(name, sizeof name, "%s+%#x(dw0)", equal.empty() ? "" : ",", off);
            equal += name;
        }
    }
    LOG_INFO(Render, "SOTC GCN TEX REGISTER: rt sample {} object {:#x} class {:#x} slot {}: op T# {}{} | equal: {}",
             line, object, view_class, slot, WordsText(words), fields, equal.empty() ? "none" : equal);
}

// One registration item for the drain: the classes not yet live are resolved through sink.image with t_source set.
// False when the budget ran out first (the caller keeps the item; the classes resolved by then are present next time).
bool RegisterOne(const Globals::MissSink& sink, const u32 (&words)[8], const u32* classes, u32 n, u8 source,
                 u32& budget) {
    State& s = S();
    RegState& r = R();
    for (u32 c = 0; c < n; ++c) {
        bool live = false;
        bool capped = false;
        {
            std::scoped_lock lk{s.mutex};
            live = ProbeShadow(words, 8, classes[c]).found != kCap;
            const u32 key = classes[c] >> 8;
            capped = key >= Shader::NUM_BINDLESS_IMAGE_KEYS || s.reg_live[key] >= kRegisterSlotCap;
        }
        if (live) {
            r.present[source].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (capped) {
            r.slot_cap.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (budget == 0) {
            r.budget_stops.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        --budget;
        t_source = source;
        sink.image(sink.user, words, classes[c]); // Tex::OnImageMiss: texture cache lock, then g_mutex
        t_source = 0;
    }
    return true;
}

// The per-op path's lookup of one binding (NoteResolved), before NoteLocked: what the table held, by source, and the
// class learned for the T#'s shape. g_mutex held.
void NoteOpLocked(State& s, const u32 (&words)[8], u32 view_class, u32 slot) {
    RegState& r = R();
    r.op_lookups.fetch_add(1, std::memory_order_relaxed);
    const Probe p = ProbeShadow(words, 8, view_class);
    if (p.found != kCap) {
        const Shadow& e = s.shadow[p.found];
        if (e.slot != slot) {
            r.op_slot_diff.fetch_add(1, std::memory_order_relaxed);
        } else if (e.source != 0) {
            r.op_hit_registered.fetch_add(1, std::memory_order_relaxed);
        } else {
            r.op_hit_op.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        u32 live[kMaxClasses];
        const u32 n = LiveWithWordsLocked(s, words, live);
        bool other_class = false;
        for (u32 i = 0; i < n; ++i) {
            other_class = other_class || s.shadow[live[i]].source != 0;
        }
        (other_class ? r.op_absent_class : r.op_absent).fetch_add(1, std::memory_order_relaxed);
    }
    if (const u32 bit = ClassBit(view_class); bit != ~0u) {
        AmdGpu::Image t{};
        std::memcpy(&t, words, sizeof(t));
        s.learned[SigOf(t)] |= u64{1} << bit;
    }
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
    // sotc: SOTC_GCN_TEX_REGISTER. A registration drained at submit start comes through here too (t_source set): its
    // outcome is counted per source, not as a GPU miss.
    const u8 source = t_source;
    const auto fail = [&s, source] {
        if (source != 0) {
            R().failed[source].fetch_add(1, std::memory_order_relaxed);
        } else {
            s.image_failed.fetch_add(1, std::memory_order_relaxed);
        }
    };
    const u32 key = view_class >> 8;
    AmdGpu::Image tsharp{};
    std::memcpy(&tsharp, tsharp_words, sizeof(tsharp));
    if (IsSamplerClass(view_class) || key >= Shader::NUM_BINDLESS_IMAGE_KEYS || !UsableTSharp(tsharp)) {
        fail();
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
        fail();
        return;
    }

    VideoCore::ImageId bound = found;
    if (const auto depth = texture_cache.GetAssociatedDepth(texture_cache.GetImage(found))) {
        bound = depth; // a stencil attachment is read through its depth image
    }
    // sotc: SOTC_GCN_TEX_REGISTER. A registration is not a bind of the next op: is_bound is cleared only for the op's
    // bound list (Rasterizer::ResetBindings), so it is left as it was.
    if (source == 0) {
        texture_cache.GetImage(bound).binding.is_bound = 1u;
    }
    if (texture_cache.GetImage(bound).binding.needs_rebind) {
        texture_cache.GetImage(bound).binding = {};
        bound = texture_cache.FindImage(desc);
        if (!bound) {
            fail();
            return;
        }
    }
    VideoCore::ImageView& view = texture_cache.FindTexture(bound, desc, true);
    texture_cache.GetImage(bound).usage.texture |= 1u;

    // GENERAL: the same layout the heap path writes (SOTC_LAYOUT_UNIFIED keeps every image there).
    const u32 slot = SotcArch::Heap::IndexOf(view, key, vk::ImageLayout::eGeneral);
    if (slot == SotcArch::Heap::NullImageIndex(key)) {
        fail();
        return;
    }
    const VideoCore::Image& image = texture_cache.GetImage(bound);
    std::scoped_lock lk{s.mutex};
    if (PutLocked(s, tsharp_words, 8, view_class, slot, bound, image.image_uid, source)) {
        if (source != 0) {
            R().inserted[source].fetch_add(1, std::memory_order_relaxed);
        } else {
            s.image_resolved.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        fail();
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
    if (RegisterAny()) { // sotc: SOTC_GCN_TEX_REGISTER, the per-op hit counters and the render target log
        if (RegisterLog()) {
            LogRenderTargetSample(tsharp, view_class, slot);
        }
        std::scoped_lock lk{s.mutex};
        NoteOpLocked(s, tsharp, view_class, slot);
        NoteLocked(s, tsharp, 8, view_class, slot, image);
        return;
    }
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

// ---------------------------------------------------------------------------------------------------------------
// sotc: SOTC_GCN_TEX_REGISTER (see gcn_native.h).

namespace {

struct RegConfig {
    bool on{};
    u32 sources{};
    bool log{};
    u32 budget{};
};

const RegConfig& Config() {
    static const RegConfig config = [] {
        RegConfig c;
        c.on = SotcArch::SwitchMode("SOTC_GCN_TEX") != 0 && SotcArch::SwitchMode("SOTC_GCN_TEX_REGISTER") != 0;
        c.sources = 0xFu;
        if (const char* v = std::getenv("SOTC_GCN_TEX_REGISTER_SOURCES"); v && *v) {
            c.sources = static_cast<u32>(std::strtoul(v, nullptr, 0)) & 0xFu;
        }
        const char* log = std::getenv("SOTC_GCN_TEX_REGISTER_LOG");
        c.log = c.on && log && log[0] == '1';
        c.budget = 64;
        if (const char* v = std::getenv("SOTC_GCN_TEX_REGISTER_BUDGET"); v && *v) {
            c.budget = std::max<u32>(1u, static_cast<u32>(std::strtoul(v, nullptr, 0)));
        }
        return c;
    }();
    return config;
}

// pending = registrations not taken + refreshes not applied. Queue mutex held.
void StorePendingLocked(RegState& r) {
    r.pending.store(r.queue.size() - r.head + r.refreshes.size(), std::memory_order_relaxed);
}

} // namespace

bool RegisterRequested(Source source) {
    const RegConfig& c = Config();
    const u32 bit = static_cast<u32>(source);
    return c.on && bit >= 1 && bit < kSources && (c.sources & (1u << (bit - 1))) != 0;
}

bool RegisterOn(Source source) {
    return RegisterRequested(source) && Mode(Stage::Tex) != 0;
}

bool RegisterLog() {
    return Config().log;
}

void QueueRegister(const u32 (&tsharp)[8], Source source) {
    if (!RegisterOn(source)) {
        return;
    }
    RegState& r = R();
    if (!UsableWords(tsharp)) {
        r.invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!SeenInsert(r, WordsKey(tsharp))) {
        r.dup.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PendingReg item{};
    std::memcpy(item.words, tsharp, sizeof(item.words));
    item.source = static_cast<u8>(source);
    std::scoped_lock lk{r.mutex};
    if (r.queue.size() - r.head >= kQueueCap) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    r.queue.push_back(item);
    StorePendingLocked(r);
    r.queued[item.source].fetch_add(1, std::memory_order_relaxed);
}

void QueueRefresh(u64 tsharp_addr, const u32 (&old)[8], const u32 (&now)[8]) {
    if (!RegisterOn(Source::Refresh) || SameWords(old, now, 8)) {
        return;
    }
    RegState& r = R();
    std::scoped_lock lk{r.mutex};
    if (const auto it = r.refreshes.find(tsharp_addr); it != r.refreshes.end()) {
        std::memcpy(it->second.new_words, now, sizeof(it->second.new_words)); // the first old, the last now
        r.refresh_coalesced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r.refreshes.size() >= kRefreshCap) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PendingRefresh ref{};
    std::memcpy(ref.old_words, old, sizeof(ref.old_words));
    std::memcpy(ref.new_words, now, sizeof(ref.new_words));
    r.refreshes.emplace(tsharp_addr, ref);
    StorePendingLocked(r);
    r.refresh_queued.fetch_add(1, std::memory_order_relaxed);
}

void NoteRenderTarget(u64 object) {
    if (!RegisterLog() || Mode(Stage::Tex) == 0) {
        return;
    }
    // Candidate base addresses: the T# at +0xc4 (Address), the colour descriptors at +0xe4 and +0x124 (dword 0 is the
    // base >> 8, as CB_COLOR_BASE) and every dword of the depth descriptor at +0x164 read as a base >> 8 (its layout is
    // not settled; a value below 64 KiB is not taken). A false candidate only adds a log line.
    u32 tsharp[8]{};
    u32 color0 = 0;
    u32 color1 = 0;
    u32 depth[8]{};
    if (!SotcGuest::Read(object + 0xc4, tsharp, sizeof(tsharp))) {
        return;
    }
    SotcGuest::Read(object + 0xe4, &color0, sizeof(color0));
    SotcGuest::Read(object + 0x124, &color1, sizeof(color1));
    SotcGuest::Read(object + 0x164, depth, sizeof(depth));
    AmdGpu::Image t{};
    std::memcpy(&t, tsharp, sizeof(t));
    u64 bases[11]{};
    u32 n = 0;
    bases[n++] = t.Address();
    bases[n++] = u64{color0} << 8;
    bases[n++] = u64{color1} << 8;
    for (const u32 d : depth) {
        bases[n++] = u64{d} << 8;
    }
    RegState& r = R();
    r.rt_objects.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lk{r.rt_mutex};
    for (u32 i = 0; i < n; ++i) {
        if (bases[i] >= 0x10000 && r.rt_bases.size() < kRtCap) {
            r.rt_bases[bases[i]] = object;
        }
    }
}

void DrainRegistered(const Globals::MissSink& sink) {
    if (!RegisterAny() || !sink.image) {
        return;
    }
    RegState& r = R();
    if (r.pending.load(std::memory_order_relaxed) == 0 || !On()) {
        return;
    }
    State& s = S();
    u32 budget = Config().budget;

    // 1. In-place rewrites: the new T# takes the classes the old one is live under, then the old entries die.
    std::vector<std::pair<u64, PendingRefresh>> refreshes;
    {
        std::scoped_lock lk{r.mutex};
        refreshes.assign(r.refreshes.begin(), r.refreshes.end());
        r.refreshes.clear();
        StorePendingLocked(r);
    }
    std::vector<std::pair<u64, PendingRefresh>> kept;
    for (const auto& [addr, ref] : refreshes) {
        if (SameWords(ref.old_words, ref.new_words, 8)) {
            continue;
        }
        u32 classes[kMaxClasses];
        u32 n = 0;
        {
            std::scoped_lock lk{s.mutex};
            u32 live[kMaxClasses];
            n = LiveWithWordsLocked(s, ref.old_words, live);
            for (u32 i = 0; i < n; ++i) {
                classes[i] = s.shadow[live[i]].view_class;
            }
        }
        if (n == 0) {
            r.refresh_unknown.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!RegisterOne(sink, ref.new_words, classes, n, static_cast<u8>(Source::Refresh), budget)) {
            kept.emplace_back(addr, ref);
            continue;
        }
        std::scoped_lock lk{s.mutex};
        u32 live[kMaxClasses];
        const u32 dead = LiveWithWordsLocked(s, ref.old_words, live);
        for (u32 i = 0; i < dead; ++i) {
            TombstoneLocked(s, live[i]);
        }
        r.refresh_tombstoned.fetch_add(dead, std::memory_order_relaxed);
        r.refresh_applied.fetch_add(1, std::memory_order_relaxed);
    }
    if (!kept.empty()) {
        std::scoped_lock lk{r.mutex};
        for (const auto& [addr, ref] : kept) {
            const auto [it, inserted] = r.refreshes.try_emplace(addr, ref);
            if (!inserted) {
                // A newer rewrite of the same T# arrived meanwhile: keep the oldest old and the newest now.
                std::memcpy(it->second.old_words, ref.old_words, sizeof(ref.old_words));
            }
        }
        StorePendingLocked(r);
    }

    // 2. Registrations, in queue order, while the budget lasts. The item stays at the head until all its classes are
    // done, so a budget stop resumes it at the next submit.
    while (budget > 0) {
        PendingReg item{};
        {
            std::scoped_lock lk{r.mutex};
            if (r.head == r.queue.size()) {
                r.queue.clear();
                r.head = 0;
                break;
            }
            item = r.queue[r.head];
        }
        u32 classes[kMaxClasses];
        u32 n = 0;
        {
            std::scoped_lock lk{s.mutex};
            n = ClassesLocked(s, item.words, classes);
        }
        if (n == 0) {
            r.no_class.fetch_add(1, std::memory_order_relaxed);
        } else if (!RegisterOne(sink, item.words, classes, n, item.source, budget)) {
            break;
        }
        std::scoped_lock lk{r.mutex};
        ++r.head;
        if (r.head == r.queue.size()) {
            r.queue.clear();
            r.head = 0;
        } else if (r.head >= 4096 && r.head * 2 >= r.queue.size()) {
            r.queue.erase(r.queue.begin(), r.queue.begin() + static_cast<std::ptrdiff_t>(r.head));
            r.head = 0;
        }
        StorePendingLocked(r);
    }
}

namespace {

// The 5 s line of the registration (its own log line: the Tex line and this one do not fit the report buffer).
// Counters reset; the seen set is cleared, so a T# whose entry died meanwhile can be queued again.
void ReportRegister(double seconds) {
    RegState& r = R();
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    u64 queued[kSources], inserted[kSources], present[kSources], failed[kSources];
    for (u32 i = 0; i < kSources; ++i) {
        queued[i] = take(r.queued[i]);
        inserted[i] = take(r.inserted[i]);
        present[i] = take(r.present[i]);
        failed[i] = take(r.failed[i]);
    }
    const u64 lookups = take(r.op_lookups);
    const u64 hit_reg = take(r.op_hit_registered);
    const u64 hit_op = take(r.op_hit_op);
    const u64 slot_diff = take(r.op_slot_diff);
    const u64 absent_class = take(r.op_absent_class);
    const u64 absent = take(r.op_absent);
    for (auto& slot : r.seen) {
        slot.store(0, std::memory_order_relaxed);
    }
    const auto u = [](u64 v) { return static_cast<unsigned long long>(v); };
    const double hit_pct = lookups ? 100.0 * static_cast<double>(hit_reg) / static_cast<double>(lookups) : 0.0;
    const double any_pct =
        lookups ? 100.0 * static_cast<double>(hit_reg + hit_op) / static_cast<double>(lookups) : 0.0;
    char line[768];
    std::snprintf(
        line, sizeof line,
        "SOTC GCN TEX REGISTER (%.1fs, material/refresh/rt/pass): queued %llu/%llu/%llu/%llu, inserted "
        "%llu/%llu/%llu/%llu, present %llu/%llu/%llu/%llu, failed %llu/%llu/%llu/%llu, dup %llu invalid %llu "
        "dropped %llu no-class %llu budget-stops %llu slot-cap %llu, pending %zu; refresh queued %llu coalesced %llu applied %llu "
        "unknown %llu tombstoned %llu; table live %u; per-op lookups %llu: registered %llu (%.1f%%), per-op %llu "
        "(present %.1f%%), slot-diff %llu, absent %llu (other class %llu); rt objects %llu logged %u",
        seconds, u(queued[1]), u(queued[2]), u(queued[3]), u(queued[4]), u(inserted[1]), u(inserted[2]),
        u(inserted[3]), u(inserted[4]), u(present[1]), u(present[2]), u(present[3]), u(present[4]), u(failed[1]),
        u(failed[2]), u(failed[3]), u(failed[4]), u(take(r.dup)), u(take(r.invalid)), u(take(r.dropped)),
        u(take(r.no_class)), u(take(r.budget_stops)), u(take(r.slot_cap)), r.pending.load(std::memory_order_relaxed),
        u(take(r.refresh_queued)), u(take(r.refresh_coalesced)), u(take(r.refresh_applied)),
        u(take(r.refresh_unknown)), u(take(r.refresh_tombstoned)), S().live.load(std::memory_order_relaxed),
        u(lookups), u(hit_reg), hit_pct, u(hit_op), any_pct, u(slot_diff), u(absent + absent_class),
        u(absent_class), u(r.rt_objects.load(std::memory_order_relaxed)),
        std::min(r.rt_lines.load(std::memory_order_relaxed), kRtLogMax));
    LOG_INFO(Render, "{}", line);
}

} // namespace

void Report(char* out, u32 size, double seconds) {
    if (Mode(Stage::Tex) == 0) {
        if (size > 0) {
            out[0] = '\0';
        }
        return;
    }
    if (RegisterAny()) {
        ReportRegister(seconds); // sotc: SOTC_GCN_TEX_REGISTER
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
