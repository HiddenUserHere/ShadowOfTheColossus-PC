// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). native_bind: the Phase 2.4
// material/shader bind path. The guest has no pipeline object: 0x85fc10 ("Material_BindGraphics
// Shaders_B", a dirty-flag command generator over the material block cb+0x2e8) drains whatever
// the record setters (0x861520/0x861600/0x860bb0/0x860c50/0x861260/0x8612b0/0x8616f0) staged and
// emits sceGnmSetXxxShader + SET_SH_REG packets. A "unique pipeline" is identified only by
// content (docs/engine/native-render-map-bind.md §2.9): the per-stage shader-program record
// images, the shader modifiers, the per-stage layout records, and the fixed-function state.
//
// This module derives that content key from the guest's material block AS STAGED (read-only,
// through guest_bridge — it does not reproduce the constant producers, gap N2-6 stays open),
// hashes it, and resolves it to the port's compiled vk::Pipeline through a fill-side cache
// (Lookup / Remember). The fill side does not have a producer yet (gap N2-10): the port's
// PipelineCache::GetGraphicsPipeline builds pipelines on the command-processor thread from
// liverpool->regs, which is stale + racy from the render threads where the hooks run — so in
// Phase 2.4 every lookup misses and every draw object chains to the guest body. The derivation
// still runs live (SOTC_RENDER_VERBOSE traces the exact key + the gate decision), which is what
// makes N2-2's gate state observable without guessing.
#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

#include "common/types.h"
#include "native/guest_bridge.h"

namespace Vulkan {
class Pipeline;
}

namespace Native::Bind {

/// The GNM shader-stage ordinal of the SET_SH_REG stage index / the material block's slot arrays
/// (bind doc §1: 0=CS, 1=PS, 2=VS, 3=GS, 4=ES, 5=HS, 6=LS, 7=spare). This is NOT the port's
/// SwStage order (Fragment first); every guest offset below is indexed by this ordinal.
constexpr u32 kNumStages = 7;

/// The per-stage record image length the guest's own setters consume
/// (core/libraries/gnmdriver/gnmdriver.cpp: SetCsShaderWithModifier:1409 (cs_regs[0..6] read,
/// [1] validated), SetPsShader350:1727 (ps_regs[0..11]), SetVsShader:1825 (vs_regs[0..6]),
/// SetGsShader:1599 (gs_regs[0..6]), SetEsShader:1563 (es_regs[0..3]), SetHsShader:1627
/// (hs_regs[0..6]), SetLsShader:1657 (ls_regs[0..3])).
///
/// The record objects store the image at record+8 (the setters pass `rec+2` in dword units,
/// 0085fc10.c:37 `puVar12 = puVar13 + 2`); image[0] = 32-bit code VA, image[1] must be 0,
/// image[2..3] = PGM_RSRC1/2, the tail = the per-stage state words the bind doc §2.9 level-1
/// lists.
u32 RecordImageWords(u32 stage);

/// Why a draw object fell back to the guest chain (the derivation gates, in check order).
enum class Gate : u32 {
    None = 0,
    NoDevice,           ///< pass[0] == 0 (no cb)
    NoMaterial,         ///< cb+0x2e8 block unavailable
    RecordImageInvalid, ///< record dword1 != 0 (the guest driver rejects this too)
    NoLayoutRecord,     ///< a staged record has no layout record
    PrimitiveUnknown,   ///< VGT_PRIMITIVE_TYPE value outside the GNM enum
    PassCacheIncomplete,///< the fixed-function pass caches are not readable
    PipelineUnknown,    ///< no pipeline registered for the content key (N2-10 fill pending)
};

const char* GateName(Gate gate);

/// One GNM stage of the derived bind state (the record image the guest's setters staged).
struct StageRecord {
    u64 record{0};      ///< guest pointer of the shader record (0 = absent)
    u64 layout{0};      ///< guest pointer of the layout record (+0xc180+8*stage)
    u32 image[12]{};    ///< the record's register image at record+8 (what the setters pass)
    u32 words{0};       ///< valid dwords of `image` (RecordImageWords)
    u32 modifier{0};    ///< the staged shader modifier (+0xc168/70/78)
    bool ok{false};     ///< image validated: present -> image[1] == 0 and layout != 0
};

/// The material block (cb+0x2e8) derived as staged. All offsets proven in
/// docs/engine/native-render-map-bind.md §1; the read is read-only.
struct MaterialSnapshot {
    u64 cb{0};           ///< the device/cursor object the block belongs to
    u64 material{0};     ///< cb+0x2e8 (a host address; guest VA == host pointer)
    u32 group_pending{0};///< +0xc260 (== cb+0xc548 draw kind / group-dirty word)
    u16 pending_bits{0}; ///< +0xc264 accumulator (slot s staged-but-unflushed bits)
    u8 changed[kNumStages]{};         ///< +0xc248+s record-changed bytes
    u8 ud_pending[kNumStages]{};      ///< +0xc250+s user-data pending bytes
    u8 layout_identical[kNumStages]{};///< +0xc258+s layout-identical bytes (PS/VS/GS/HS)
    StageRecord stage[kNumStages]{};
    u32 tex_size_product{0};          ///< (hi16*w)*(lo16*h) of +0xc5e8 (bind doc §2.3)
    bool ok{false};
};

/// The fixed-function deltas the bind doc §2.9 lists for the key (read from the pass-context
/// caches the pass functions maintain; native-render-map-pass.md §1.1 / bind doc §5).
struct FixedFunctionState {
    u32 primitive_type{0};  ///< VGT_PRIMITIVE_TYPE the draw object would set (0x86a900, reg 0x242)
    u32 index_type{0};      ///< INDEX_TYPE after the draw object's reset (0x86bed0, reg 0x2a)
    u32 db_depth_control{0};///< pass+0x11c cache (0x869d60)
    u32 cb_target_mask{0};  ///< pass+0x1ac cache (0x869990)
    u32 cb_blend0{0};       ///< pass+0x88 cache (CB_BLEND0_CONTROL, 0x869a50)
    u32 pa_su_sc_mode{0};   ///< pass+0x23 cache (0x86e1a0)
    bool ok{false};
};

/// The four-level content key (bind doc §2.9) + the fixed-function deltas, packed into a stable
/// byte layout so the fill side (N2-10) can build the same key on the command-processor thread.
/// kKeyVersion bumps whenever the layout changes (a stale saved map must never hit).
constexpr u32 kKeyVersion = 1;

struct StageKey {
    u64 layout_ptr{0};   ///< guest pointer identity of the layout record (0 = absent)
    u64 layout_hash{0};  ///< XXH3_64 of layout-record bytes [0, 0x24) (the window the flush
                         ///< provably reads; full u16-table semantics are N2-6/N2-13)
    u32 code_va{0};      ///< record image dword0 (32-bit GPU VA, byte address = <<8)
    u32 rsrc1{0};        ///< image dword2 with the modifier folded (the driver's own formula)
    u32 rsrc2{0};        ///< image dword3
    u32 image_hash{0};   ///< XXH3_64 low dword of image dwords [4, words)
    u32 image_words{0};  ///< RecordImageWords (the hashed span)
    u32 modifier{0};     ///< the raw staged modifier (also folded into rsrc1)
    u32 present{0};      ///< record != 0
    u32 pad{0};          ///< explicit so the key struct has no indeterminate bytes (it is hashed
                         ///< with XXH3 over its raw bytes; padding would be garbage)
};

struct MaterialKey {
    StageKey stage[kNumStages];
    u32 primitive_type{0};
    u32 index_type{0};
    u32 db_depth_control{0};
    u32 cb_target_mask{0};
    u32 cb_blend0{0};
    u32 pa_su_sc_mode{0};
    u32 draw_kind{0};
    u32 version{kKeyVersion}; ///< prefix field: never hash across layout versions
};
static_assert(std::has_unique_object_representations_v<MaterialKey>);

/// 128-bit hash of a MaterialKey (XXH3_128).
struct Key128 {
    u64 lo{0};
    u64 hi{0};
    bool operator==(const Key128& o) const noexcept { return lo == o.lo && hi == o.hi; }
};

struct Key128Hash {
    size_t operator()(const Key128& k) const noexcept {
        // lo is already a full-width XXH3 digest half; fold hi in so no bit is dropped.
        return static_cast<size_t>(k.lo ^ (k.hi * 0x9e3779b97f4a7c15ull));
    }
};

Key128 HashKey(const MaterialKey& key);

/// Reads the material block (cb+0x2e8) as staged into `out`. Read-only; missing links (a record
/// whose image dword1 != 0, a present record without a layout record) leave the per-stage `ok`
/// flag clear and are reported through ValidateSnapshot / the hook gates. Returns false only
/// when `cb` itself is unusable.
bool DeriveSnapshot(u64 cb, MaterialSnapshot& out);

/// Per-stage gate check (RecordImageInvalid / NoLayoutRecord / None).
Gate ValidateStage(const StageRecord& stage);

/// Reads the fixed-function pass caches (`pass` = the draw object's param_1) and computes the
/// primitive type the draw object would set. `tess_prim` reproduces each body's selection rule:
/// when `tess_prim` is true the body sets primitive 9 (PatchPrimitive) instead of the draw
/// mode's table value (0x94ec0:10-20: `*(u8*)(pass+0x18) & 6`). The draw-mode table
/// DAT_00f9f2e0 is image-anchored (read via Guest::ImageQword, gap N2-3 rule); an unmapped
/// image or a table value outside the GNM PrimitiveType enum leaves ok=false.
FixedFunctionState DeriveFixedFunction(u64 pass, u32 draw_mode, bool tess_prim);

/// The indirect draw objects (0x10add0/0xdb620) set no primitive type of their own: the type in
/// force is the last VGT_PRIMITIVE_TYPE the draw objects cached at pass+0x1cc. 0 (never set in
/// this pass) leaves ok=false -> the caller chains rather than keying on an unknown topology.
FixedFunctionState DeriveFixedFunctionCachedPrim(u64 pass);

/// Builds the key from a validated snapshot + fixed-function state.
MaterialKey BuildKey(const MaterialSnapshot& snap, const FixedFunctionState& ff);

/// -- Pipeline cache keyed on the guest block's bytes ------------------------------------------
///
/// The value is the PORT's pipeline (the GCN->SPIR-V recompiled shaders with the recompiler's
/// descriptor-set layout). The native backend never builds a pipeline itself (the recompiler's
/// machinery is not callable from the render threads — vk_pipeline_cache.cpp reads live
/// liverpool->regs; report/requested change N2-10). Until the fill side registers entries,
/// Lookup always misses and every draw chains to the guest body.

/// The registered pipeline for a content key, or null (miss -> chain to the guest).
const Vulkan::Pipeline* Lookup(const Key128& key);

/// Fill-side entry (N2-10): the command-processor side registers the pipeline it built for the
/// same content. Thread-safe with Lookup (the render threads call it from the hooks).
void Remember(const Key128& key, const Vulkan::Pipeline* pipeline);

/// Secondary map for the 0x93690 fullscreen gate (N2-2): the clear draw resolves the engine
/// program (record+0x60) before its body stages the block, so its key is program-keyed.
const Vulkan::Pipeline* LookupFullscreenProgram(u64 program);
void RememberFullscreenProgram(u64 program, const Vulkan::Pipeline* pipeline);

/// Cache counters for the verbose trace / future divergence checker.
struct Stats {
    u64 lookups;      ///< Lookup calls
    u64 hits;         ///< keys found in the map
    u64 remembers;    ///< Remember calls
    u64 distinct;     ///< distinct keys seen (map size)
};
Stats StatsSnapshot();

/// Derives the start-vertex / base-instance SGPR slots the draw preamble (0x862510) would write
/// for `kind` (cb+0xc548): kind -> geometry descriptor record (+0xc398 byte +0x27 for kind 0,
/// +0xc3a8 byte +0x19 for 0xb0, +0xc3b8 byte +0x19 for the other nonzero kinds) split into the
/// low (vertex SGPR) and high (instance SGPR) nibbles. Kinds outside the decoded set (0, 0x45,
/// 0xad, 0xb0, 0x145, 0x1ad — the rest is gap N2-5) return false.
struct StartVertexSlots {
    u32 stage{0};        ///< the SGPR stage ordinal the slots are written into (2=VS, 4=ES, 6=LS)
    u32 vertex_slot{0};  ///< low nibble (0 = not written)
    u32 instance_slot{0};///< high nibble (0 = not written)
};
bool DeriveStartVertexSlots(u64 cb, u32 kind, StartVertexSlots& out);

} // namespace Native::Bind
