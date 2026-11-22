// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_shadow: RenderShadows (0xcc9b0) / RenderSpotLightShadows
// (0xcc690) and the depth pre-pass (0xc59d0 / 0xc1860). Per docs/engine/native-render-map-frame.md
// §9/§10, shadows are 5 cascades (a depth target + a caster pass + a cull dispatch each) and the
// depth pre-pass renders 5 cull groups into kRTT_MainDepth.
//
// Phase 2.8 (native-shadow, docs/engine/native-render-map-cull-camera-shadow.md §2/§3) adds the
// read-only derivations and the native commit paths of the three entry points the stage hooks:
//   0xcc9b0 RenderShadows      — the 5-cascade orchestrator (base RT set + clear kind 2 + the
//                                cascade loop + the five DB_DEPTH_CLEAR/depth-touch pairs);
//   0xc59d0  G-buffer runner   — SetA depth-only + clear kind 6 -> RenderDepth -> SetB 7-colour
//                                MRT (structure only: the job bodies stay emulated, see the
//                                DeriveGBuffer gate note);
//   0x93ef0  depth pass object — the DB-block depth clear (0.5-depth viewport + fullscreen-rect
//                                draw + EOP/WAIT) the five touches reach through 0x93d70.
// Every derivation is read-only; the commits mirror the guest's cache stores and record through
// Scheduler::RecordDeferred, exactly like the Phase 2.5 RT binder (native_hooks.cpp).
#pragma once

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_device.h"
#include "native/native_draw.h"
#include "native/native_pass.h"

namespace Vulkan {
class Scheduler;
} // namespace Vulkan

namespace Native {

class Pass;

/// The number of shadow cascades (the guest loop runs 5 iterations, k = 0..4, cascade id 4-k).
constexpr u32 kCascadeCount = 5;

/// The 5 shadow cascades of RenderShadows (0xcc9b0). The guest loop runs with lVar5 = 0,-1,-2,-3,-4
/// (loop iteration k); iteration k reads split/bias/target/label at base - k*8 and drives cascade
/// id 4-k. Cascade index runs 4..0 in the guest loop.
struct ShadowCascade {
    VAddr target{0};        // *(RC + 0xdf8 - k*8): per-cascade depth target
    f32 split{0};           // *(shadow_cfg + 0x134 - k*8)
    f32 bias{0};            // *(shadow_cfg + 0x138 - k*8)
    VAddr draw_table{0};    // R + 0x4a40 - k*0x828 (stepped -0x828 per cascade)
    const char* name{nullptr}; // image 0x1597a00 - k*8 ("RenderShadowCascade_4" .. "_0")
    VAddr label{0};         // *(RC + 0xe50 - k*8) completion label/job slot
    u32 cascade_id{0};      // *(int*)(ctx + 0x88) = 4 - k (read back by the job body 0xcdbc0:26)
    bool cull_wanted{false}; // k != 0 && *(s8*)(ctx[7] + 0xacfc - k*0x50) != 0
};

/// One derived 0x93ef0 depth-clear (the 0x93d70 -> 0x93ef0 branch or the direct hook entry).
struct DepthClearPlan {
    TargetSet set;            ///< depth-only attachment set decoded from the view descriptor
    u64 pass{0};              ///< the pass context (its [0] = cb) the caches mirror into
    u64 view_desc{0};         ///< the view descriptor address (the +0x164 DB block cache mirror)
    u32 width{0};             ///< owner +0x194 low 16 bits
    u32 height{0};            ///< owner +0x194 high 16 bits
    float clear_depth{0.0f};  ///< DB_DEPTH_CLEAR the guest stages (0x869f50(0) before the draw)
    bool aliased_owner{false};///< view != owner: format words patched from the owner's block
    u32 flag{0};              ///< the guest's param_4 (0 from 0x93d70)
};

/// One cascade of ShadowsPlan (the per-cascade RT set comes from the per-view record block
/// ctx[3] + 0x2400 + (id+0x23)*0x50 exactly as the job body 0xcdbc0 builds it).
struct CascadePlan {
    ShadowCascade info;
    TargetSet set;       ///< 10-qword per-cascade target array, decoded
    bool set_derived{false};
};

/// The full read-only derivation of one 0xcc9b0 RenderShadows call.
struct ShadowsPlan {
    u64 pass{0};               ///< *ctx: the pass context the guest helpers/mirrors address
    u64 job_ctx{0};            ///< the job ctx (the cascade-loop context writes)
    u64 view_desc{0};          ///< the base set's depth descriptor (the +0x164 cache mirror)
    TargetSet base_set;        ///< V+0x31c0..0x3208: the one RT set bound for all cascades
    bool clear_base{false};    ///< the 0x932a0 kind-2 clear (depth-only to 0.0, 1.0 - 1.0f)
    bool cascades_enabled{false}; ///< *(s8*)(cfg+0xe9) && (shadow_cfg+0x170 & 1)
    CascadePlan cascade[kCascadeCount];   ///< guest order k=0..4 (cascade id 4..0)
    VAddr touch_desc[5]{};     ///< V+0x2120 + 8k: the five post-loop depth-touch descriptors
    bool touch_clear[5]{};     ///< the touch takes the 0x93ef0 branch (else ACQUIRE_MEM only)
};

/// The read-only derivation of one 0xc59d0 G-buffer call (structure only — the scheduled job
/// bodies are separate functions and stay emulated; see the DeriveGBuffer gate note).
struct GBufferPlan {
    u64 pass{0};               ///< *ctx: the pass context the guest helpers/mirrors address
    u64 job_ctx{0};            ///< the job ctx (the group-enable word write)
    u64 set_a_desc{0};         ///< the SetA depth descriptor (the +0x164 cache mirror)
    TargetSet set_a;           ///< V+0x29a0 (SetA, depth-only kRTT_MainDepth)
    bool clear_a{false};       ///< kind 6 (depth 0.0 + stencil 0) when *(s8*)(cfg+0x1c) == 0
    TargetSet set_aux;         ///< V+0x2590 + clear kind 1 (colour slots only)
    TargetSet set_b;           ///< V+0x2a40 (the 7-colour G-buffer set + depth)
    TargetSet set_aux0;        ///< V+0x24a0 (bound with set_b when the draw tables are empty)
    bool aux_clears{false};    ///< the conditional double bind+clear of set_b/set_aux0
    u32 group_mask{0};         ///< *(u32*)(ctx + 0xf8) := 0x1f (the DepthPrePass group-enable word)
    VAddr label_wait0{0};      ///< ctx[0x1d] + 0x18 WAIT_REG_MEM
    VAddr label_wait1{0};      ///< ctx[0x1d] + 0x30 WAIT_REG_MEM
    VAddr label_done{0};       ///< ctx[0x1d] + 0xd0 EOP
    VAddr main_depth_touch{0}; ///< V+0x1fb8: the kRTT_MainDepth touch descriptor
};

class Shadow {
public:
    /// The shadow module issues its passes through the renderer's Pass/Draw helpers (the same
    /// objects the RT binder and the draw objects use), so it holds references to them.
    Shadow(const Device& device, const Pass& pass, const Draw& draw)
        : device{device}, pass_ref{pass}, draw_ref{draw} {}

    /// True when RenderShadows runs this frame (gate: shadow config present and not disabled).
    static bool Enabled();

    /// Number of cascades (the guest loops 5: lVar5 = 0,-1,-2,-3,-4).
    static constexpr u32 CascadeCount = kCascadeCount;

    /// Reads cascade k (0..4, the guest loop iteration order) from the shadow config
    /// (R+0x8f8) and render context. Split/bias only; the orchestrator derivation fills the rest.
    static ShadowCascade ReadCascade(u32 k);

    /// The depth pre-pass (0xc1860): group count and the pass slot it uses.
    static constexpr u32 DepthPrePassGroups = 5;

    /// Decodes a 10-qword pass-target descriptor array (bind-map §3 shape: 8 colour slots +
    /// depth + selector) into a TargetSet — the 0x92140 decode WITHOUT its pass-context caches
    /// (no rebind mask / cache mirrors; those are the RT binder hook's business). Returns false
    /// (nothing written) when no descriptor is usable or the extents look wrong.
    static bool DeriveTargetSet(const u64* targets, TargetSet& out);

    /// Which branch the 0x93d70 depth-touch wrapper takes for descriptor `desc`
    /// (proven 00093d70.c:15-27): `runs_clear` = the +0xde bit0x20 clear && +0x167 bit0x20 set
    /// (the wrapper calls 0x93ef0(ctx, desc, desc, 0)); `acquire_only` = the +0xde sub-rect
    /// ACQUIRE_MEM branch (no clear). Both false = only the trailing surface ACQUIRE_MEM runs.
    static void TouchBranches(u64 desc, bool* runs_clear, bool* acquire_only);

    // -- Phase 2.8 derivations (read-only; return nullptr when committable, else the gate reason) --

    /// 0xcc9b0 RenderShadows. `unused` is the guest's third argument (MainCamera's extraout_RDX,
    /// never read by the body). Returns the first failing gate, or nullptr. The standing commit
    /// gate (the cascade caster's draws are not native yet) is the return value when everything
    /// else derived: "cascade draws not native ...".
    static const char* DeriveShadows(u64 R, u64 shadow_cfg, u64 unused, u64 ctx, ShadowsPlan& out);

    /// 0xc59d0 G-buffer. Returns the first failing gate, or nullptr. The scheduled job bodies
    /// (RenderDepth 0xc1860 groups, fur, decals, particles, skybox) are out of Phase 2.8 scope,
    /// so the standing return value is "depth pre-pass draws not native ...".
    static const char* DeriveGBuffer(u64 R, u64 view_cfg, u64 ctx, u64 draw_table, GBufferPlan& out);

    /// 0x93ef0 depth pass object. Returns the first failing gate, or nullptr. The guest's silent
    /// skip cases (non-depth type byte, self-clear with flag != 0) return their reason too —
    /// chaining there reproduces the guest's no-op.
    static const char* DeriveDepthClear(u64 pass, u64 view, u64 owner, u32 flag, DepthClearPlan& out);

    // -- Phase 2.8 commits (call only after the caller's emission gate opened; all-or-nothing:
    //    every attachment view resolves before any guest-visible write) --

    /// Mirrors the guest's cache stores and records the base set + per-cascade depth-only passes
    /// + the five touches. Returns false when an attachment view cannot be resolved (caller
    /// chains; nothing was written to guest state).
    bool CommitShadows(Vulkan::Scheduler& scheduler, const ShadowsPlan& plan) const;

    /// G-buffer structure commit: SetA depth-only pass (clear folded into the load op), the group
    /// word, the SetB binds and the kRTT_MainDepth touch. The draw jobs stay emulated.
    bool CommitGBuffer(Vulkan::Scheduler& scheduler, const GBufferPlan& plan) const;

    /// One 0x93ef0 depth clear: dynamic rendering scope on the decoded depth attachment with
    /// load-op clear 0.0 (or the fullscreen-rect draw when the N2-10 pipeline seam resolves),
    /// then the pass-context cache mirrors the guest body would leave.
    bool CommitDepthClear(Vulkan::Scheduler& scheduler, const DepthClearPlan& plan) const;

private:
    const Device& device;
    const Pass& pass_ref;
    const Draw& draw_ref;
};

} // namespace Native
