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
// finds that are not perfect, stencil images, the composite probes. The UI linear rewrite is refused unless
// SOTC_HEAP_UI_LINEAR=1, which resolves it here (UiLinear: a key bit for the static part, CheckHit for the bound
// image, ResolveFresh for the sampler). Refused tables are counted.
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
//
// BINDSET (stage 1, switch SOTC_BINDSET: 0 off, 1 replay, 2 verify; needs SOTC_HEAP real mode). A stored entry whose
// resolution is replayable (not a candidate, no unmapped T#, every bound image settled: TouchBoundImage would do no
// UpdateImage, no slot of the heap at 0) is a BindSet. A hit on a BindSet does no CheckHit and no TouchBoundImage: it
// replays the indices and the bound images, with the per-hit work that stays (NoteUse, LOD marks, the access stamps,
// the LRU touch and the bind evidence of the bound image). Validity is kept by events, not by checks:
//  - a reverse index (image index -> links, heap (key, slot) -> links) names the BindSets each state depends on;
//    an event kills every live link of its image or slot (entry.valid = false). A link carries the entry's serial, so a
//    link to a replaced entry is stale and harmless; the links of an image are cleared when it is applied;
//  - events are queued by SotcArch::BindSet::Post* (any thread, own mutex, bounded: an overflow kills every BindSet);
//    the draw thread applies them in DrainEvents at the start of each Resolve, under the texture cache lock;
//  - the conditions CheckHit and TouchBoundImage test, and their events:
//      Miss::Generation  ImageAddrGeneration (addr_gens: Register/Unregister)  ImageSet (register over the range), ImageFree
//      Miss::Generation  TryGetImage (uid, registered)                         ImageFree (unregister, park, unpark)
//      Miss::Generation  GetAssociatedDepth(found) vs the stored redirect      ImageDepth (association made), ImageFree
//      Miss::Dirty       found/bound Dirty flags (Maybe/Cpu/Gpu)               ImageDirty (every site that sets them)
//      Miss::Dirty       bound needs_rebind                                    ImageRebind (ResolveOverlap, ExpandImage, FreeRebased)
//      Miss::Slot        Holds(key, slot, view, GENERAL)                       HeapSlot (Release, Evict)
//      Miss::Sampler     SamplerGeneration                                     Sampler (sampler GC)
//      TouchBoundImage   TexUpdateIsNoOp false: Dirty, Registered, track range ImageDirty, ImageFree, ImageTrack
//      Candidate (mip tail) what DecideRebase reads over the chain range: the images registered there, the GPU-written
//                        mark of the images there (Range: RegisterImage, UnregisterImage, MarkGpuModified and the
//                        clears of the mark). The global BindGeneration and gpu_modified_epoch are not used for a
//                        BindSet: they move on every registration and every first GPU write of any image.
//      Candidate         the time rule (TextureCache::BindSetTailTimeStale): asked on each replay, no event reports time
//    Unmapped T#s are not replayable and keep CheckHit on every hit.
//  - posting is gated (the link gate below): a post reaches the queue only while a live link holds its image, slot or
//    candidates. Gate and link are ordered: LinkEntry counts the link (seq_cst), then the store checks the entry again
//    (Replayable after LinkEntry, behind a seq_cst fence). A producer posts after its change, so either the producer
//    sees the link and posts, or the check sees the change.
//  - the transition of BindHeapImages is skipped when the image's backing already holds the state the read needs (no
//    barrier would come out of Image::GetBarriers): see TableMemo::NoteTransitSkipped.
// Mode 2 (verify): a would-be replay is classified by CheckHit, then resolved afresh as the full path does, and the
// fresh indices and bound images are compared with the stored ones; the fresh answer is used. Counted as checked/stale.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "common/types.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/pixel_format.h" // sotc: UiLinear::StageEligible (AmdGpu::IsInteger)
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_bindless.h" // sotc: BindlessImages::Holds (per-slot validity of a hit)
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/bindset_events.h" // sotc: SOTC_BINDSET event API
#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/sotc_stats.h" // sotc: SOTC_PR_KEYS counters (keys used)
#include "video_core/texture_cache/texture_cache.h"

namespace SotcArch {
// Specialist 9's null descriptors (heap.cpp). Declared here until their header carries them.

// sotc: the UI linear rule, shared by the classic bind (Rasterizer::UiLinearAsset, BindSamplers) and the heap resolve.
namespace UiLinear {
bool RenderResOn() {
    static const bool on = [] {
        const char* r = std::getenv("SOTC_RENDER_RES");
        return r && *r && std::strcmp(r, "1920x1080") != 0;
    }();
    return on;
}

bool StageEligible(const Shader::Info& stage) {
    return RenderResOn() && stage.images.size() == 1 && stage.images[0].array_size == 0 &&
           !AmdGpu::IsInteger(stage.images[0].GetSharp(stage).GetNumberFmt());
}

bool ImageEligible(const VideoCore::Image& image) {
    const auto& u = image.usage;
    return !(u.render_target || u.depth_target || u.storage || u.vo_surface ||
             image.binding.is_target || image.info.props.is_depth);
}

bool HeapOn() {
    static const bool on = SwitchMode("SOTC_HEAP_UI_LINEAR") == 1;
    return on;
}
} // namespace UiLinear
} // namespace SotcArch

namespace SotcArch::TableMemo {
namespace {

// The bounds of Rasterizer::MaxBlockImages / MaxBlockSamplers: a larger stage takes the draw path.
constexpr u32 kMaxImages = 16;
constexpr u32 kMaxSamplers = Shader::NUM_SAMPLERS;
constexpr u32 kMaxBindings = kMaxImages + kMaxSamplers;
constexpr u32 kKeyWords = 3 + 5 * kMaxImages + 3 * kMaxSamplers;
static_assert(kKeyWords == PreKey::kMaxWords, "sotc: PreKey holds the words of KeyOf");
constexpr u32 kEntries = 4096;          // power of two; open addressing
constexpr u32 kProbe = 8;               // probe window; the least recently used entry of it is replaced
constexpr u32 kSections = 4;            // ring sections, one per tick modulo kSections
constexpr u64 kSectionBytes = 4ull << 20;
constexpr u64 kRingAlign = 16;
// sotc: SOTC_BINDSET. A link list is purged of stale links at purge_at entries (then purge_at is twice its live size).
constexpr u32 kPurgeMin = 32;
constexpr u32 kBindSetCauses = static_cast<u32>(BindSet::Cause::Count);
// sotc: SOTC_BINDSET. Queued events. Past this many (nothing draws them), every BindSet is killed instead.
constexpr u32 kQueueCap = 1u << 16;
// sotc: SOTC_BINDSET link gates. Image indices below kLinkGateImages have a gate (the number of links in their list);
// a larger index is always posted. Heap (key, slot) links are gated by a hashed bucket (a shared bucket only posts more).
constexpr u32 kLinkGateImages = 1u << 16;
constexpr u32 kLinkGateSlots = 1u << 12;

static_assert(sizeof(AmdGpu::Image) == 4 * sizeof(u64));
static_assert(sizeof(AmdGpu::Sampler) == 2 * sizeof(u64));

enum class Miss : u8 { None, Absent, Candidate, Generation, Slot, Dirty, Sampler, Unmapped, UiLinear };

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
    /// sotc: SOTC_BINDSET. A replayable resolution: its links are in the reverse index, and an event kills it (valid off).
    bool bindset{};
    /// sotc: SOTC_BINDSET. The store number of the entry; a reverse-index link is live while the entry carries its serial.
    u64 serial{};
    /// sotc: SOTC_BINDSET. The guest bytes a candidate depends on (its chain and the found images): a Range event that
    /// overlaps them kills it. Zero for other entries.
    u64 range_lo{};
    u64 range_hi{};
    /// sotc: SOTC_BINDSET. The tick (plus one) at which a replay last stamped the heap slots of the entry: a second replay
    /// in the same tick has nothing new to note (NoteUse stores the tick, which is the same).
    u64 noted_tick{};
    /// sotc: SOTC_HEAP_UI_LINEAR. ui_static is the static part of the rule (key bit 32 of key[0], UiLinear::StageEligible);
    /// ui_linear is the decision for the bound image, checked again on each hit (CheckHit).
    bool ui_static{};
    bool ui_linear{};
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
    /// sotc: SOTC_HEAP_UI_LINEAR: a hit whose bound image no longer decides the UI linear rule as it did.
    std::atomic<u64> miss_ui_linear{};
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
    /// sotc: SOTC_BINDSET. Replays, stores, resolutions that cannot be replayed, events posted and applied on overflow,
    /// kills by cause (BindSet::Cause), the verify mode (checked, stale, indices or images differing), and the
    /// transitions the replay skipped (mode 1) or checked (mode 2: barriers the prediction did not make).
    std::atomic<u64> bs_hits{};
    std::atomic<u64> bs_stored{};
    std::atomic<u64> bs_not_replayable{};
    std::atomic<u64> bs_posted{};
    /// sotc: events the link gate skipped (no live link held their image, slot or candidates), and the candidate
    /// replays the time rule turned into a fresh resolution (BindSetTailTimeStale).
    std::atomic<u64> bs_skipped{};
    std::atomic<u64> bs_time_stale{};
    std::atomic<u64> bs_overflow{};
    std::array<std::atomic<u64>, kBindSetCauses> bs_killed{};
    std::atomic<u64> bs_verify{};
    std::atomic<u64> bs_verify_stale{};
    std::atomic<u64> bs_verify_mismatch{};
    std::atomic<u64> transit_skipped{};
    std::atomic<u64> transit_verify{};
    std::atomic<u64> transit_stale{};
    /// sotc: SOTC_HEAP_UI_LINEAR: ops whose bound image takes the bilinear rewrite of the point samplers (hits and fresh).
    std::atomic<u64> ui_linear_ops{};
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

    // sotc: SOTC_BINDSET reverse index: image index -> links of the BindSets that replay the image, and heap
    // (key, slot) -> links of the BindSets that hold the slot. Used on the draw thread only, under the texture cache lock.
    struct Link {
        u32 entry{};
        u64 serial{};
    };
    struct LinkList {
        std::vector<Link> links;
        u32 purge_at{kPurgeMin};
        /// sotc: the gate this list counts into: its gate is the sum of the sizes of the lists that share it.
        std::atomic<u32>* gate{};
    };
    std::vector<LinkList> by_image;
    std::unordered_map<u64, LinkList> by_slot;
    u64 serial_counter{};

    // sotc: SOTC_BINDSET link gates (read by any thread, written by the draw thread). image_gate[i] is the number of
    // links in by_image[i]; slot_gate[b] the number of links in the by_slot lists hashed to bucket b. Both are exact:
    // every push counts once and every removal (purge, kill, clear) subtracts what it removed.
    std::unique_ptr<std::atomic<u32>[]> image_gate;
    std::unique_ptr<std::atomic<u32>[]> slot_gate;
    // sotc: the live candidate BindSets (entry, serial), the Range events scan them. candidate_gate is their count
    // (stale ones leave at the next purge or range scan, so it may over-count, never under-count).
    std::vector<Link> candidates;
    u32 candidates_purge_at{kPurgeMin};
    std::atomic<u32> candidate_gate{};

    // sotc: SOTC_BINDSET event queue. Posted from any thread under queue_mutex; drained by the draw thread.
    struct PendingEvent {
        u8 kind; // 0 image, 1 heap slot, 2 samplers, 3 guest range [lo, hi)
        u8 cause;
        u32 a;
        u32 b;
        u64 lo;
        u64 hi;
    };
    std::mutex queue_mutex;
    std::vector<PendingEvent> queue;
    bool queue_overflow{}; // under queue_mutex
    std::atomic<bool> queue_pending{};
    std::atomic<bool> post_on{};
    std::vector<PendingEvent> drain_batch; // draw thread scratch
    std::vector<std::pair<u64, u64>> range_batch; // draw thread scratch: the Range events of the batch
};

State s;

int Mode() {
    static const int mode = SwitchMode("SOTC_HEAP");
    return mode;
}

// sotc: SOTC_BINDSET (read once): 0 off, 1 replay, 2 verify. Off unless the table memo is up.
int BindSetSwitch() {
    static const int mode = SwitchMode("SOTC_BINDSET");
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
    // sotc: SOTC_HEAP_UI_LINEAR=1: the rule is decided per op (ResolveFresh), so the stage is not refused here.
    return !UiLinear::HeapOn() && UiLinear::RenderResOn() && stage.images.size() == 1;
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
    // sotc: bit 32 is the static part of the UI linear rule (the decision on the bound image is checked on each hit).
    key[n++] = u64{stage.images.size()} | u64{stage.samplers.size()} << 8 |
               u64(static_cast<u32>(stage.sw_stage)) << 24 |
               (UiLinear::StageEligible(stage) ? u64{1} << 32 : u64{0});
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
// `events_cover_candidates`: a BindSet's candidate is judged by its Range events and the time rule, not by the global
// BindGeneration and gpu_modified_epoch (verify mode asks the same question of the stored BindSet with it set).
Miss CheckHit(VideoCore::TextureCache& tc, const Entry& e, u64 bindless_gen,
              bool events_cover_candidates = false) {
    // A released or evicted heap slot since the entry was validated: the indices may name a slot given back.
    if (e.bindless_gen != bindless_gen) {
        const Miss slot_miss = SlotsMiss(e);
        if (slot_miss != Miss::None) {
            return slot_miss;
        }
    }
    if (e.candidate && !events_cover_candidates &&
        (tc.BindGeneration() != e.bind_generation ||
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
        // sotc: SOTC_HEAP_UI_LINEAR: the bound image decides the sampler rewrite as it did at the resolution.
        if (e.ui_static && i == 0 && UiLinear::ImageEligible(*bound) != e.ui_linear) {
            return Miss::UiLinear;
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
    // sotc: SOTC_BINDSET. The guest bytes the candidate decisions read: the chain of each candidate T# and the found
    // image's chain (a tail's dropped bytes included). Their Range events kill the entry (rec.range_lo/hi).
    u64 cand_lo = ~u64{0};
    u64 cand_hi = 0;

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
        const u64 chain_lo = desc.info.guest_address; // the full chain (desc.info before any rebase)
        const u64 chain_hi = chain_lo + desc.info.guest_size;
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
            const u64 found_lo = found.info.guest_address - found.info.sotc_dropped_bytes;
            cand_lo = std::min({cand_lo, chain_lo, found_lo});
            cand_hi = std::max({cand_hi, chain_hi, u64{found.info.guest_address} + found.info.guest_size});
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

    if (rec.candidate) {
        rec.range_lo = cand_lo;
        rec.range_hi = cand_hi;
    }
    // sotc: SOTC_HEAP_UI_LINEAR: the classic bind's decision (BindTexturesFull, UiLinearAsset) on the single bound image.
    rec.ui_static = UiLinear::StageEligible(stage);
    rec.ui_linear = rec.ui_static && num_images == 1 && !rec.images[0].null &&
                    UiLinear::ImageEligible(tc.GetImage(rec.images[0].bound));
    for (u32 j = 0; j < stage.samplers.size(); ++j) {
        const auto& sampler = stage.samplers[j];
        AmdGpu::Sampler ssharp = sampler.GetSharp(stage);
        // sotc: the classic BindSamplers' rewrite: a point magnification of a non-depth sampler becomes bilinear.
        if (rec.ui_linear && ssharp.xy_mag_filter.Value() == AmdGpu::Filter::Point && !sampler.is_depth) {
            ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
        }
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
    if (rec.ui_linear) {
        s.counters.ui_linear_ops.fetch_add(1, std::memory_order_relaxed);
    }
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

// sotc: SOTC_BINDSET. The mode in effect: 0 when the table memo is not up (its events would never be drained).
int ActiveBindSetMode() {
    return s.ready ? BindSetSwitch() : 0;
}

// sotc: SOTC_BINDSET. A link is live while its entry is a BindSet stored under the link's serial: not killed, not replaced.
bool LinkLive(const State::Link& l) {
    const Entry& e = s.entries[l.entry];
    return e.valid && e.bindset && e.serial == l.serial;
}

// Adds a link to a list and counts it into the list's gate (a seq_cst RMW: see PostImageGated). A purge drops the stale
// links first, and subtracts what it dropped.
void AddLink(State::LinkList& list, const State::Link& link, std::atomic<u32>* gate) {
    if (list.links.size() >= list.purge_at) {
        const size_t before = list.links.size();
        std::erase_if(list.links, [](const State::Link& l) { return !LinkLive(l); });
        const u32 dropped = static_cast<u32>(before - list.links.size());
        if (list.gate && dropped) {
            list.gate->fetch_sub(dropped, std::memory_order_seq_cst);
        }
        list.purge_at = std::max<u32>(kPurgeMin, 2u * static_cast<u32>(list.links.size()));
    }
    list.gate = gate;
    list.links.push_back(link);
    if (gate) {
        gate->fetch_add(1, std::memory_order_seq_cst);
    }
}

// Kills every live BindSet of the list and clears it (a killed entry needs none of its links).
void KillList(State::LinkList& list, BindSet::Cause cause) {
    for (const State::Link& l : list.links) {
        if (LinkLive(l)) {
            s.entries[l.entry].valid = false;
            s.counters.bs_killed[static_cast<u32>(cause)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (list.gate && !list.links.empty()) {
        list.gate->fetch_sub(static_cast<u32>(list.links.size()), std::memory_order_seq_cst);
    }
    list.links.clear();
    list.purge_at = kPurgeMin;
}

u64 SlotKey(u32 key, u32 slot) {
    return (u64{key} << 32) | slot;
}

// The slot gate bucket of a (key, slot) link: a hash of SlotKey (collisions share a gate, so they only post more).
u32 SlotBucket(u64 slot_key) {
    return static_cast<u32>((slot_key * 0x9E3779B97F4A7C15ULL) >> (64 - 12)) & (kLinkGateSlots - 1);
}

// Links a stored BindSet from each of its bound images (the found one too: its state is part of the resolution)
// and from each heap slot it holds.
void LinkEntry(u32 index, const Entry& e) {
    const State::Link link{index, e.serial};
    for (u32 i = 0; i < e.num_images; ++i) {
        const ImageRecord& r = e.images[i];
        if (r.null) {
            continue;
        }
        const u32 found = r.found.index;
        const u32 bound = r.bound.index;
        if (found >= s.by_image.size() || bound >= s.by_image.size()) {
            s.by_image.resize(std::max(found, bound) + 1);
        }
        const auto image_gate = [](u32 image) -> std::atomic<u32>* {
            return image < kLinkGateImages ? &s.image_gate[image] : nullptr;
        };
        AddLink(s.by_image[found], link, image_gate(found));
        if (bound != found) {
            AddLink(s.by_image[bound], link, image_gate(bound));
        }
        const u64 slot_key = SlotKey(r.key, e.indices[i]);
        AddLink(s.by_slot[slot_key], link, &s.slot_gate[SlotBucket(slot_key)]);
    }
}

// Stores a candidate BindSet in the candidate list (the Range events scan it) and publishes the count.
void AddCandidate(u32 index, u64 serial) {
    if (s.candidates.size() >= s.candidates_purge_at) {
        std::erase_if(s.candidates, [](const State::Link& l) { return !LinkLive(l); });
        s.candidates_purge_at = std::max<u32>(kPurgeMin, 2u * static_cast<u32>(s.candidates.size()));
    }
    s.candidates.push_back(State::Link{index, serial});
    s.candidate_gate.store(static_cast<u32>(s.candidates.size()), std::memory_order_seq_cst);
}

// Kills the live candidate BindSets whose bytes overlap one of the ranges [first, second) of the drained batch (draw
// thread, texture cache lock held). The ranges are sorted and merged, so each candidate is one binary search.
void KillCandidates(std::vector<std::pair<u64, u64>>& ranges) {
    if (ranges.empty()) {
        return;
    }
    std::sort(ranges.begin(), ranges.end());
    size_t n = 0;
    for (const auto& r : ranges) {
        if (n && r.first <= ranges[n - 1].second) {
            ranges[n - 1].second = std::max(ranges[n - 1].second, r.second);
        } else {
            ranges[n++] = r;
        }
    }
    ranges.resize(n); // disjoint and sorted: the ends are sorted too
    for (const State::Link& l : s.candidates) {
        if (!LinkLive(l)) {
            continue;
        }
        Entry& e = s.entries[l.entry];
        // The first range that ends past the candidate's start; it kills the candidate when it starts before the end.
        const auto it = std::lower_bound(ranges.begin(), ranges.end(), e.range_lo,
                                         [](const std::pair<u64, u64>& r, u64 lo) { return r.second <= lo; });
        if (it != ranges.end() && it->first < e.range_hi) {
            e.valid = false;
            s.counters.bs_killed[static_cast<u32>(BindSet::Cause::Range)].fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    std::erase_if(s.candidates, [](const State::Link& l) { return !LinkLive(l); });
    s.candidate_gate.store(static_cast<u32>(s.candidates.size()), std::memory_order_seq_cst);
}

// Empties every link list and the candidate list, and zeroes the gates (overflow: every BindSet is already killed).
void ClearLinks() {
    for (u32 i = 0; i < kLinkGateImages; ++i) {
        s.image_gate[i].store(0, std::memory_order_seq_cst);
    }
    for (u32 b = 0; b < kLinkGateSlots; ++b) {
        s.slot_gate[b].store(0, std::memory_order_seq_cst);
    }
    s.by_image.clear();
    s.by_slot.clear();
    s.candidates.clear();
    s.candidates_purge_at = kPurgeMin;
    s.candidate_gate.store(0, std::memory_order_seq_cst);
}

// sotc: SOTC_BINDSET. Whether a resolution may be replayed (stored as a BindSet, or checked after its links are made).
// Not when a T# is unmapped (a guest map changes it, CheckHit watches it), when the heap was full (slot 0 holds a view),
// or when the state CheckHit tests does not hold now: the found image registered and clean, the bound one registered,
// clean, not due a rebind and settled (TouchBoundImage would do no UpdateImage), the depth association as the redirect
// says, the address generation of each image as resolved, the sampler generation, and each heap slot still holding its
// view. A candidate (mip tail) is replayable too: its decision reads the registered images and the GPU-written marks over
// its chain, which the Range events cover, and its time rule, which a replay asks (BindSetTailTimeStale).
bool Replayable(VideoCore::TextureCache& tc, const Entry& rec, const std::array<u32, kMaxBindings>& indices) {
    for (u32 i = 0; i < rec.num_images; ++i) {
        const ImageRecord& r = rec.images[i];
        if (r.unmapped) {
            return false;
        }
        if (r.null) {
            continue;
        }
        VideoCore::Image* found = tc.TryGetImage(r.found, r.found_uid);
        const VideoCore::Image* bound = tc.TryGetImage(r.bound, r.bound_uid);
        if (!found || !bound || indices[i] == 0) {
            return false;
        }
        if ((found->flags & VideoCore::ImageFlagBits::Dirty) != VideoCore::ImageFlagBits::Empty ||
            bound->binding.needs_rebind || !tc.BoundImageUpdateIsNoOp(r.bound)) {
            return false;
        }
        if (tc.GetAssociatedDepth(*found) != (r.redirect ? r.bound : VideoCore::ImageId{})) {
            return false;
        }
        if (tc.ImageAddrGeneration(r.addr) != r.addr_gen) {
            return false;
        }
    }
    if (rec.has_samplers && tc.SamplerGeneration() != rec.sampler_generation) {
        return false;
    }
    return SlotsMiss(rec) == Miss::None;
}

// sotc: SOTC_BINDSET. A candidate whose found image the time rule now replaces (BindSetTailTimeStale): not replayed.
bool TimeStale(VideoCore::TextureCache& tc, const Entry& e) {
    if (!e.candidate) {
        return false;
    }
    for (u32 i = 0; i < e.num_images; ++i) {
        const ImageRecord& r = e.images[i];
        if (!r.null && tc.BindSetTailTimeStale(r.found)) {
            return true;
        }
    }
    return false;
}

bool BindSetArmed() {
    return s.post_on.load(std::memory_order_acquire);
}

// Queues an event (any thread). Nothing is queued while the layer is off. Past kQueueCap the queue is dropped and an
// overflow is recorded: the draw thread then kills every BindSet.
void PostEvent(u8 kind, u8 cause, u32 a, u32 b, u64 lo = 0, u64 hi = 0) {
    if (!s.post_on.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lk{s.queue_mutex};
    if (s.queue.size() >= kQueueCap) {
        s.queue.clear();
        s.queue_overflow = true;
    } else {
        s.queue.push_back(State::PendingEvent{kind, cause, a, b, lo, hi});
    }
    s.queue_pending.store(true, std::memory_order_release);
    s.counters.bs_posted.fetch_add(1, std::memory_order_relaxed);
}

// sotc: SOTC_BINDSET link gate, the posting side (any thread). The producer has made its change before the call; the
// seq_cst fence orders that change before the gate read, and a link counts into its gate with a seq_cst RMW before its
// entry is checked (TableMemo::Resolve, the store). So for a change and a link: either the gate read sees the link and
// the event is queued, or the link's check (run after the gate was counted) reads the change. A skipped post is
// counted (bs_skipped). An index without a gate always posts.
void PostImageGated(u32 image_index, u8 cause) {
    if (!s.post_on.load(std::memory_order_acquire)) {
        return;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (image_index < kLinkGateImages && s.image_gate[image_index].load(std::memory_order_seq_cst) == 0) {
        s.counters.bs_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PostEvent(0, cause, image_index, 0);
}

void PostSlotGated(u32 key, u32 slot) {
    if (!s.post_on.load(std::memory_order_acquire)) {
        return;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (s.slot_gate[SlotBucket(SlotKey(key, slot))].load(std::memory_order_seq_cst) == 0) {
        s.counters.bs_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PostEvent(1, 0, key, slot);
}

void PostRangeGated(u64 lo, u64 hi) {
    if (!s.post_on.load(std::memory_order_acquire)) {
        return;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (s.candidate_gate.load(std::memory_order_seq_cst) == 0) {
        s.counters.bs_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PostEvent(3, static_cast<u8>(BindSet::Cause::Range), 0, 0, lo, hi);
}

// Applies the queued events (draw thread, texture cache lock held): each kills the live BindSets that depend on the image,
// the heap slot, the sampler cache, or the guest range (candidates).
void DrainEvents() {
    if (!s.queue_pending.load(std::memory_order_acquire)) {
        return;
    }
    bool overflow = false;
    {
        std::scoped_lock lk{s.queue_mutex};
        s.drain_batch.swap(s.queue);
        s.queue.clear();
        overflow = s.queue_overflow;
        s.queue_overflow = false;
        s.queue_pending.store(false, std::memory_order_release);
    }
    if (overflow) {
        // Events were lost, so no BindSet can be trusted: all are killed, and the reverse index is emptied with them.
        for (u32 i = 0; i < kEntries; ++i) {
            Entry& e = s.entries[i];
            if (e.valid && e.bindset) {
                e.valid = false;
                s.counters.bs_overflow.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ClearLinks();
    }
    for (const State::PendingEvent& ev : s.drain_batch) {
        if (ev.kind == 0) {
            if (ev.a < s.by_image.size()) {
                KillList(s.by_image[ev.a], static_cast<BindSet::Cause>(ev.cause));
            }
        } else if (ev.kind == 1) {
            const auto it = s.by_slot.find(SlotKey(ev.a, ev.b));
            if (it != s.by_slot.end()) {
                KillList(it->second, BindSet::Cause::HeapSlot);
                s.by_slot.erase(it);
            }
        } else if (ev.kind == 3) {
            s.range_batch.emplace_back(ev.lo, ev.hi); // applied together below
        } else {
            for (u32 i = 0; i < kEntries; ++i) {
                Entry& e = s.entries[i];
                if (e.valid && e.bindset && e.has_samplers) {
                    e.valid = false;
                    s.counters.bs_killed[static_cast<u32>(BindSet::Cause::Sampler)].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
    }
    KillCandidates(s.range_batch);
    s.range_batch.clear();
    s.drain_batch.clear();
}

// sotc: SOTC_BINDSET mode 1. A BindSet that no event killed is replayed as stored: no CheckHit, no TouchBoundImage. What
// stays per hit is what the classic hit keeps: the heap use stamp, the LOD mark, the access stamps of the found image, the
// LRU touch and the bind evidence of the bound image (BindSetTouchBound). The bound flags, the transitions and the bound
// list are made by Rasterizer::BindHeapImages from the binds.
IndexTable BindSetReplay(VideoCore::TextureCache& tc, Entry& e, u32 count, u64 tick, TableBinds* binds) {
    s.counters.hits.fetch_add(1, std::memory_order_relaxed);
    s.counters.bs_hits.fetch_add(1, std::memory_order_relaxed);
    e.last_use = ++s.use_counter;
    VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active();
    // sotc: the heap slots of the entry were stamped with this tick by an earlier replay of the same tick (NoteUse stores
    // the tick; a slot's stamp can only be the tick now), so that store is not repeated.
    const bool slots_noted = e.noted_tick == tick + 1;
    for (u32 i = 0; i < e.num_images; ++i) {
        const ImageRecord& r = e.images[i];
        if (r.null) {
            continue;
        }
        if (heap && !slots_noted) {
            heap->NoteUse(r.key, e.indices[i]);
        }
        if (r.lod) {
            SotcLod::Mark(r.lod_bank); // the first pass marks every bind
        }
        VideoCore::Image& found = tc.GetImage(r.found);
        found.tick_prev_accessed = found.tick_accessed_last;
        found.tick_accessed_last = tick;
        tc.BindSetTouchBound(r.bound);
    }
    e.noted_tick = tick + 1;
    if (binds) {
        FillBinds(e, binds);
    }
    return Publish(&e, e.indices, count, tick);
}

// sotc: SOTC_BINDSET mode 2. The stored BindSet against the full path: the fresh resolution's indices and bound images
// must equal the stored ones, and CheckHit (`full`) must find the stored entry valid. Any difference is a stale answer,
// which means an event is missing.
void CompareReplay(const Entry& stored, Miss full, const Entry& fresh, const std::array<u32, kMaxBindings>& indices,
                   bool store, u32 count) {
    bool mismatch = !store;
    for (u32 i = 0; i < count; ++i) {
        mismatch |= stored.indices[i] != indices[i];
    }
    for (u32 i = 0; i < stored.num_images; ++i) {
        const ImageRecord& a = stored.images[i];
        const ImageRecord& b = fresh.images[i];
        mismatch |= a.null != b.null || (!a.null && a.bound != b.bound);
    }
    if (mismatch) {
        s.counters.bs_verify_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    if (mismatch || full != Miss::None) {
        s.counters.bs_verify_stale.fetch_add(1, std::memory_order_relaxed);
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
    // sotc: SOTC_BINDSET. Events are queued from now on; the draw thread applies them in Resolve.
    if (ActiveBindSetMode() != 0) {
        s.image_gate = std::make_unique<std::atomic<u32>[]>(kLinkGateImages);
        s.slot_gate = std::make_unique<std::atomic<u32>[]>(kLinkGateSlots);
        s.post_on.store(true, std::memory_order_release);
        std::fprintf(stderr, "SOTC BINDSET: mode %d\n", ActiveBindSetMode());
    }
}

int BindSetMode() {
    return ActiveBindSetMode();
}

void NoteTransitSkipped() {
    s.counters.transit_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteTransitVerify(bool barrier) {
    s.counters.transit_verify.fetch_add(1, std::memory_order_relaxed);
    if (barrier) {
        s.counters.transit_stale.fetch_add(1, std::memory_order_relaxed);
    }
}

// sotc: SOTC_PR_KEYS. A GpuPrep thread computes the same KeyOf the draw thread would (the stage's flat buffer comes
// from this thread's Shader::g_info_slot, the border base from the op's registers). No shared state is read.
bool ComputeKey(const Shader::Info& stage, AmdGpu::BorderColorBuffer border_color, PreKey& out) {
    out.valid = false;
    out.stage = &stage;
    out.border = border_color.Address();
    const u32 words = KeyWordsOf(stage);
    if (words > PreKey::kMaxWords) {
        return false;
    }
    out.hash = KeyOf(stage, border_color, out.key);
    out.words = words;
    out.valid = true;
    return true;
}

IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color, u64 bindless_gen, TableBinds* binds) {
    return Resolve(texture_cache, stage, border_color, bindless_gen, binds, nullptr);
}

IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color, u64 bindless_gen, TableBinds* binds,
                   const PreKey* pre) {
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
    u64 hash = 0;
    // sotc: SOTC_PR_KEYS. A record made for this stage and border base replaces KeyOf; the words past `words` stay
    // zero, as KeyOf leaves them. Anything else (no record, another stage, another border) computes it here.
    if (pre && pre->valid && pre->stage == &stage && pre->border == border_color.Address() &&
        pre->words == words) {
        hash = pre->hash;
        std::memcpy(key.data(), pre->key.data(), words * sizeof(u64));
        if (SotcStats::kFpsLog) {
            SotcStats::pr_keys_used.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        hash = KeyOf(stage, border_color, key);
    }
    const u32 count = static_cast<u32>(stage.images.size() + stage.samplers.size());
    const u64 tick = s.scheduler->CurrentTick();
    const int mode = Mode();
    s.counters.lookups.fetch_add(1, std::memory_order_relaxed);

    // sotc: SOTC_BINDSET. The events posted since the last op take effect before any entry is looked up.
    const bool bs_on = binds != nullptr && ActiveBindSetMode() != 0;
    if (bs_on) {
        DrainEvents();
    }
    Entry* hit = FindEntry(hash, key, words);
    // sotc: SOTC_BINDSET. A BindSet is valid while it is stored (an event kills it, and FindEntry skips killed entries):
    // mode 1 replays it as it is; mode 2 keeps it for the comparison with the fresh resolution below.
    Entry* bs_verify = nullptr;
    Miss bs_full = Miss::None;
    Miss miss = Miss::Absent;
    if (bs_on && hit && hit->bindset) {
        if (ActiveBindSetMode() == 1) {
            if (!TimeStale(texture_cache, *hit)) {
                return BindSetReplay(texture_cache, *hit, count, tick, binds);
            }
            // sotc: the time rule of a candidate changed its answer: the entry is killed and the op resolves afresh.
            s.counters.bs_time_stale.fetch_add(1, std::memory_order_relaxed);
            hit->valid = false;
            hit = nullptr;
            miss = Miss::Candidate;
        } else {
            bs_verify = hit;
            // A candidate is judged by its events (the global generations would not be the question it answers).
            bs_full = CheckHit(texture_cache, *bs_verify, bindless_gen, true);
            if (bs_full == Miss::None && TimeStale(texture_cache, *bs_verify)) {
                bs_full = Miss::Candidate;
            }
            // The replay assumes TouchBoundImage would do no update for each bound image (track range, registration, dirty).
            // Tested here, before the fresh resolution below makes the update: a difference is a stale answer.
            for (u32 i = 0; i < bs_verify->num_images; ++i) {
                const ImageRecord& r = bs_verify->images[i];
                if (!r.null && bs_full == Miss::None && !texture_cache.BoundImageUpdateIsNoOp(r.bound)) {
                    bs_full = Miss::Dirty;
                }
            }
            s.counters.bs_verify.fetch_add(1, std::memory_order_relaxed);
            hit = nullptr;
        }
    }
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
        if (hit->ui_linear) { // sotc: SOTC_HEAP_UI_LINEAR, the replayed sampler rewrite
            s.counters.ui_linear_ops.fetch_add(1, std::memory_order_relaxed);
        }
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
    } else if (!bs_verify) {
        switch (miss) {
        case Miss::Absent: s.counters.miss_absent.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Candidate: s.counters.miss_candidate.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Generation: s.counters.miss_generation.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Slot: s.counters.miss_slot.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Dirty: s.counters.miss_dirty.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Sampler: s.counters.miss_sampler.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::Unmapped: s.counters.miss_unmapped.fetch_add(1, std::memory_order_relaxed); break;
        case Miss::UiLinear: s.counters.miss_ui_linear.fetch_add(1, std::memory_order_relaxed); break;
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
    if (bs_verify) {
        CompareReplay(*bs_verify, bs_full, fresh, indices, store, count);
    }

    if (!store) {
        if (hit) {
            hit->valid = false; // a stale answer must not be replayed
        }
        if (bs_verify) {
            bs_verify->valid = false; // sotc: SOTC_BINDSET: the stored BindSet must not be replayed either
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
    // sotc: SOTC_BINDSET. A replayable resolution becomes a BindSet: a new serial (the links of the entry it replaces go
    // stale) and links from its images and heap slots, so the events reach it.
    fresh.serial = ++s.serial_counter;
    // sotc: SOTC_HEAP_UI_LINEAR decisions are checked on every hit (CheckHit: the bound image's usage), which a replay
    // skips, so such entries are not replayed.
    fresh.bindset = bs_on && !fresh.ui_static && Replayable(texture_cache, fresh, indices);
    if (bs_on && !fresh.bindset) {
        s.counters.bs_not_replayable.fetch_add(1, std::memory_order_relaxed);
    }
    Entry& slot = SlotFor(hash, key, words);
    slot = fresh;
    if (fresh.bindset) {
        // sotc: SOTC_BINDSET. Link first, then check again: the gate (PostImageGated) may have skipped a post for a change
        // made before the link, and the check reads that change. A failing check leaves the entry a plain memo entry.
        const u32 index = static_cast<u32>(&slot - s.entries.get());
        LinkEntry(index, slot);
        // The candidate list is counted before the check too (PostRangeGated reads its gate after a change). A candidate
        // that fails the check stays listed as dead until the next scan: the gate over-counts, which only posts more.
        if (slot.candidate) {
            AddCandidate(index, slot.serial);
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (Replayable(texture_cache, slot, indices)) {
            s.counters.bs_stored.fetch_add(1, std::memory_order_relaxed);
        } else {
            slot.bindset = false;
            s.counters.bs_not_replayable.fetch_add(1, std::memory_order_relaxed);
        }
    }
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
        "classic %llu; UI linear ops %llu, misses %llu\n",
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
        static_cast<unsigned long long>(take(s.counters.vs_classic)),
        static_cast<unsigned long long>(take(s.counters.ui_linear_ops)),
        static_cast<unsigned long long>(take(s.counters.miss_ui_linear)));
}

// sotc: SOTC_BINDSET. One line for the 5 s report (counters are reset on read); empty when the layer is off.
void BindSetReport(char* out, u32 size, double seconds) {
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    const int mode = ActiveBindSetMode();
    if (mode == 0) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    u32 live = 0;
    for (u32 i = 0; i < kEntries; ++i) {
        live += (s.entries[i].valid && s.entries[i].bindset) ? 1u : 0u;
    }
    const auto killed = [&](BindSet::Cause cause) {
        return static_cast<unsigned long long>(take(s.counters.bs_killed[static_cast<u32>(cause)]));
    };
    std::snprintf(
        out, size,
        "SOTC BINDSET %.1f s: mode %d, hits %llu, stored %llu, not replayable %llu, posted %llu, "
        "overflow %llu, live %u; killed by event (image free %llu, dirty %llu, track %llu, rebind %llu, "
        "depth %llu, image set %llu, slot %llu, sampler %llu); verify checked %llu, stale %llu, "
        "mismatches %llu; transit skipped %llu, checked %llu, stale %llu; gate skipped %llu, "
        "time stale %llu, killed by range %llu",
        seconds, mode, static_cast<unsigned long long>(take(s.counters.bs_hits)),
        static_cast<unsigned long long>(take(s.counters.bs_stored)),
        static_cast<unsigned long long>(take(s.counters.bs_not_replayable)),
        static_cast<unsigned long long>(take(s.counters.bs_posted)),
        static_cast<unsigned long long>(take(s.counters.bs_overflow)), live,
        killed(BindSet::Cause::ImageFree), killed(BindSet::Cause::ImageDirty),
        killed(BindSet::Cause::ImageTrack), killed(BindSet::Cause::ImageRebind),
        killed(BindSet::Cause::ImageDepth), killed(BindSet::Cause::ImageSet),
        killed(BindSet::Cause::HeapSlot), killed(BindSet::Cause::Sampler),
        static_cast<unsigned long long>(take(s.counters.bs_verify)),
        static_cast<unsigned long long>(take(s.counters.bs_verify_stale)),
        static_cast<unsigned long long>(take(s.counters.bs_verify_mismatch)),
        static_cast<unsigned long long>(take(s.counters.transit_skipped)),
        static_cast<unsigned long long>(take(s.counters.transit_verify)),
        static_cast<unsigned long long>(take(s.counters.transit_stale)),
        static_cast<unsigned long long>(take(s.counters.bs_skipped)),
        static_cast<unsigned long long>(take(s.counters.bs_time_stale)), killed(BindSet::Cause::Range));
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

namespace SotcArch::BindSet {

bool Armed() noexcept {
    return TableMemo::BindSetArmed();
}

void PostImage(u32 image_index, Cause cause) {
    TableMemo::PostImageGated(image_index, static_cast<u8>(cause));
}

void PostSlot(u32 key, u32 slot) {
    TableMemo::PostSlotGated(key, slot);
}

void PostSamplers() {
    TableMemo::PostEvent(2, static_cast<u8>(Cause::Sampler), 0, 0);
}

void PostRange(u64 lo, u64 hi) {
    TableMemo::PostRangeGated(lo, hi);
}

} // namespace SotcArch::BindSet
