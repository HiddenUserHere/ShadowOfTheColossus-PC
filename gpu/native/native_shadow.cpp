// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_shadow implementation (Phase 2.8, native-shadow).
//
// Contract: docs/engine/native-render-map-cull-camera-shadow.md §2 (RenderShadows 0xcc9b0), §3
// (the G-buffer runner 0xc59d0 / depth pre-pass 0xc1860 and the depth-touch wrapper 0x93d70 /
// depth pass object 0x93ef0). Every Derive* here is read-only; the Commit* paths mirror the
// guest's guest-visible cache stores and record through Scheduler::RecordDeferred, below the
// caller's emission gate (the standing rule of native-render-gaps.md: never mutate guest state
// before all gates).
#include "native/native_shadow.h"

#include <cstring>

#include "native/native_bind.h"
#include "native/native_renderer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Native {

// -- Guest field offsets (all proven in the decomp files cited; map doc §2/§3) ------------------

/// Render-context RC = ctx[2].
namespace ShadowCtx {
constexpr u64 CascadeTargets = 0xdf8;  // +0xdf8 + k*8 downward: per-cascade depth target
constexpr u64 CascadeLabels = 0xe50;   // +0xe50 + k*8 downward: per-cascade completion label
constexpr u64 ShadowSetBase = 0x31c0;  // V+0x31c0..0x3208: the 10-qword shadow RT set
constexpr u64 TouchBase = 0x2120;      // V+0x2120 + 8k: the five post-loop touch descriptors
constexpr u64 MainDepthDesc = 0x1fb8;  // V+0x1fb8: kRTT_MainDepth descriptor
constexpr u64 SetADesc = 0x29a0;       // V+0x29a0: SetA (depth-only)
constexpr u64 SetAuxDesc = 0x2590;     // V+0x2590: auxiliary set (clear kind 1)
constexpr u64 SetBDesc = 0x2a40;       // V+0x2a40: SetB, the 7-colour G-buffer MRT + depth
constexpr u64 SetAux0Desc = 0x24a0;    // V+0x24a0: second auxiliary set
constexpr u64 PerViewRecords = 0x2400; // +0x2400 + (id+0x23)*0x50: per-cascade record block
constexpr u64 JobCounter = 0xe88;      // +0xe88 job counter (context for the trace)
} // namespace ShadowCtx

/// Shadow config (param_2 = *(R+0x8f8)).
namespace ShadowCfg {
constexpr u64 Splits = 0x134;         // +0x134 + k*8 downward: cascade split (near)
constexpr u64 Biases = 0x138;         // +0x138 + k*8 downward: cascade bias/far
constexpr u64 CullFlags = 0xacfc;     // ctx[7] + 0xacfc - k*0x50: per-cascade cull flag
constexpr u64 CullFlagStride = 0x50;
constexpr u64 CascadeEnable = 0x170;  // byte, bit0: cascades enabled
} // namespace ShadowCfg

/// Per-cascade job-context slots (ctx), as 0xcc9b0 writes them (000cc9b0.c:84-90).
namespace CascadeCtx {
constexpr u64 Target = 0x30;       // ctx[6]: per-cascade depth target (current-target slot)
constexpr u64 Split = 0x80;        // ctx+0x80/0x10 qword: split (near) float
constexpr u64 Bias = 0x84;         // ctx+0x84: bias (far) float
constexpr u64 CascadeId = 0x88;    // ctx+0x88 int: cascade index 4..0
constexpr u64 DrawTable = 0x60;    // ctx[0xc]: per-cascade draw table
constexpr u64 ScopeName = 0x70;    // ctx[0xe]: scope-name pointer
constexpr u64 MainDrawTable = 0xd8; // ctx+0xd8: the MainCamera draw table (chain restore)
} // namespace CascadeCtx

namespace {
constexpr u64 kShadowsVa = 0xcc9b0;
constexpr u64 kGBufferVa = 0xc59d0;
constexpr u64 kDepthClearVa = 0x93ef0;
constexpr u64 kScopeNamesVa = 0x1597a00; // PTR_s_RenderShadowCascade_4__Super_Clo_01597a00

inline u8 ReadU8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 ReadU32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}
inline void WriteU8(u64 addr, u64 off, u8 v) {
    *reinterpret_cast<u8*>(addr + off) = v;
}
inline u64 ReadU64(u64 addr, u64 off) {
    return *reinterpret_cast<const u64*>(addr + off);
}
inline void WriteU32(u64 addr, u64 off, u32 v) {
    *reinterpret_cast<u32*>(addr + off) = v;
}
inline void WriteU64(u64 addr, u64 off, u64 v) {
    *reinterpret_cast<u64*>(addr + off) = v;
}

/// The raw bit pattern of a float (the guest stores the split/bias floats as dwords).
inline u32 F32Bits(f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}
} // namespace

// -- Commit-path helpers (only reachable below the caller's emission gate) -----------------------

/// The 0x93690 fullscreen-pipeline seam (mirror of native_hooks.cpp's ResolveFullscreenProgram +
/// Bind::LookupFullscreenProgram; kept local so the callbacks stay thin). The ensure-loaded
/// (0x95580) and resolve (0x14d7b0) chain touches the holder's lazy slot exactly like the guest
/// body does — commit path only, below the emission gate. Null when the fill side (gap N2-10)
/// has no pipeline for the program — the caller then falls back to the load-op clear.
const Vulkan::Pipeline* ResolveFullscreenPipelineChain() {
    using namespace Guest;
    const u64 base = GuestImageBase();
    if (base == 0) {
        return nullptr;
    }
    const u64 root = ImageQword(EngineOff::RootGlobals);
    if (root == 0) {
        return nullptr;
    }
    const u64 list = root != 0 ? ReadU64(root, EngineOff::ShaderList) : 0;
    const u64 holder = list != 0 ? ReadU64(list, EngineOff::ShaderHolder) : 0;
    if (holder == 0) {
        return nullptr;
    }
    using EnsureLoadedFn = void(__attribute__((sysv_abi))*)(void*);
    using ResolveFn = void* (__attribute__((sysv_abi))*)(void*);
    reinterpret_cast<EnsureLoadedFn>(base + 0x95580)(
        reinterpret_cast<void*>(holder + EngineOff::HolderLoader));
    const u64 lazy = ReadU64(holder, EngineOff::HolderLoader);
    if (lazy == 0) {
        return nullptr;
    }
    const u64 record = reinterpret_cast<u64>(
        reinterpret_cast<ResolveFn>(base + 0x14d7b0)(reinterpret_cast<void*>(lazy)));
    if (record == 0) {
        return nullptr;
    }
    return Bind::LookupFullscreenProgram(ReadU64(record, EngineOff::RecordProgram));
}

// -- The stub's original API ---------------------------------------------------------------------

bool Shadow::Enabled() {
    const u64 shadow_config =
        *reinterpret_cast<const u64*>(Guest::RendererPtr() + Guest::RendererOff::ShadowConfig);
    if (shadow_config == 0) {
        return false;
    }
    return (ReadU8(shadow_config, ShadowCfg::CascadeEnable) & 1) != 0;
}

ShadowCascade Shadow::ReadCascade(u32 k) {
    ShadowCascade c{};
    const u64 shadow_config =
        *reinterpret_cast<const u64*>(Guest::RendererPtr() + Guest::RendererOff::ShadowConfig);
    if (shadow_config == 0 || k >= CascadeCount) {
        return c;
    }
    // The guest loops with lVar5 = 0,-1,-2,-3,-4, so iteration k reads at base - k*8.
    const s64 stride = -static_cast<s64>(k) * 8;
    c.split = *reinterpret_cast<const f32*>(shadow_config + ShadowCfg::Splits + stride);
    c.bias = *reinterpret_cast<const f32*>(shadow_config + ShadowCfg::Biases + stride);
    return c;
}

// -- Target-array decode (the 0x92140 decode without its pass-context caches) --------------------

bool Shadow::DeriveTargetSet(const u64* targets, TargetSet& out) {
    if (targets == nullptr) {
        return false;
    }
    // Selector bit (qword [9] bit0): the +0xe4 vs +0x124 embedded CB block (bind-map §3.1).
    const u32 selector = static_cast<u32>(targets[9]) & 1;
    const u64 color_off = selector != 0 ? 0x124 : 0xe4;
    constexpr u64 DepthOff = 0x164;
    constexpr u64 WidthOff = 0x198;
    constexpr u64 HeightOff = 0x19c;

    u64 first_desc = 0;
    for (u32 i = 0; i < 8; ++i) {
        const u64 desc = targets[i];
        // Type gate (00092140.c:20): (type | 2) != 3 -> the slot resolves to NULL.
        if (desc == 0 || ((ReadU8(desc, 0x62) | 2) != 3)) {
            continue;
        }
        AmdGpu::ColorBuffer buffer{};
        u32 extent = 0;
        if (!Rt::DecodeColour(reinterpret_cast<const u64*>(desc + color_off), buffer, extent)) {
            continue; // invalid format words: the guest disables the slot (CB_COLORn_INFO = 0)
        }
        out.color_buffer[i] = buffer;
        out.color_extent_hint[i] = extent;
        out.color[i].format = Rt::ColourVkFormat(buffer);
        out.color[i].active = out.color[i].format != vk::Format::eUndefined;
        if (first_desc == 0) {
            first_desc = desc;
        }
    }

    const u64 depth_desc = targets[8];
    if (depth_desc != 0) {
        AmdGpu::DepthBuffer db{};
        AmdGpu::DepthView view{};
        VAddr htile = 0;
        u32 extent = 0;
        if (Rt::DecodeDepth(reinterpret_cast<const u64*>(depth_desc + DepthOff), db, view, htile,
                            extent)) {
            out.depth_buffer = db;
            out.depth_view = view;
            out.depth_htile = htile;
            out.depth_extent_hint = extent;
            out.depth.format = Rt::DepthVkFormat(db);
            out.depth_read_only = view.z_read_only != 0 && view.stencil_read_only != 0;
            out.depth.active = out.depth.format != vk::Format::eUndefined;
        }
        if (first_desc == 0) {
            first_desc = depth_desc; // the guest's lVar1 fallback (00092140.c:136-143)
        }
    }
    if (first_desc == 0) {
        return false;
    }

    out.selector = selector;
    // The 0x92140 step-5 RT-config code (00092140.c:179-184).
    const u32 sel = static_cast<u32>(targets[9]);
    out.rt_config_code = (sel & 1) != 0 ? 0 : ((~sel & 0xc) != 0 ? (sel * 2) & 8 : 0x10);
    // Surface extents (00092140.c:144-147 via 0x96f50/0x96f60), sanity-checked the way the RT
    // binder checks them.
    out.width = ReadU32(first_desc, WidthOff);
    out.height = ReadU32(first_desc, HeightOff);
    if (out.width == 0 || out.height == 0 || out.width > 8192 || out.height > 8192) {
        return false;
    }
    out.color_count = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (out.color[i].active) {
            out.color_count = i + 1;
        }
    }
    // The binder never clears (bind-map §3.5): clears are the callers' explicit fullscreen draws,
    // folded into the load ops by the commit paths below.
    out.load = vk::AttachmentLoadOp::eLoad;
    out.store = vk::AttachmentStoreOp::eStore;
    return true;
}

// -- The 0x93d70 touch branches (map doc §3.3, proven 00093d70.c:15-33) ---------------------------

void Shadow::TouchBranches(u64 desc, bool* runs_clear, bool* acquire_only) {
    *runs_clear = false;
    *acquire_only = false;
    if (desc == 0) {
        return;
    }
    const u8 de = ReadU8(desc, 0xde);
    const u8 f167 = ReadU8(desc, 0x167);
    if ((de & 0x20) == 0) {
        if ((f167 & 0x20) != 0) {
            *runs_clear = true; // FUN_00093ef0(ctx, desc, desc, 0)
        }
        // else: nothing but the trailing surface ACQUIRE_MEM below.
    } else {
        *acquire_only = true; // the +0xde sub-rect ACQUIRE_MEM branch
    }
}

// -- 0x93ef0 depth pass object -------------------------------------------------------------------

/// The format/stencil word patch of 00093ef0.c:38-41, without the guest's bit-twiddling classes:
/// the guest masks the z-format field of block dword 1 (DB_Z_INFO, the port's `z_info.format`
/// 2-bit class — regs_depth.h) and the stencil-format field of dword 2 (DB_STENCIL_INFO,
/// `stencil_info.format`) to the owner descriptor's values. The native decode consumes exactly
/// those two fields, so a single 2-dword overlay of the owner's block reproduces the patch
/// (the address words / htile / the uninterpreted N2-11 words 10/11 stay the view's).
void PatchDepthFormatFromOwner(u64* block8, u64 owner) {
    const u64* owner_words = reinterpret_cast<const u64*>(owner + 0x164);
    block8[1] = owner_words[1]; // DB_Z_INFO | DB_STENCIL_INFO (dwords 2/3 of the 13-dword block)
    block8[2] = owner_words[2]; // DB_DEPTH_INFO | DB_DEPTH_VIEW low half
}

const char* Shadow::DeriveDepthClear(u64 pass, u64 view, u64 owner, u32 flag,
                                     DepthClearPlan& out) {
    if (view == 0 || owner == 0 || pass == 0) {
        return "null pass/descriptor";
    }
    // Guest skip case 1 (00093ef0.c:26): the view descriptor is not a depth target.
    if (ReadU8(view, 0x62) != 2) {
        return "not a depth-type descriptor";
    }
    // Guest skip case 2: the self-clear case (view == owner) with the flag set.
    if (view == owner && (flag ^ 1) == 0) {
        return "self-clear with flag set";
    }

    out.aliased_owner = view != owner;
    out.pass = pass;
    out.view_desc = view;
    out.flag = flag;
    out.width = ReadU32(owner, 0x194) & 0xffff;
    out.height = ReadU32(owner, 0x194) >> 0x10;
    if (out.width == 0 || out.height == 0 || out.width > 8192 || out.height > 8192) {
        return "owner dims out of range";
    }
    out.clear_depth = 0.0f; // FUN_00869f50(0, cb) before the fullscreen draw (00093ef0.c:47)

    // The 13-dword DB block copy (view +0x164, the guest's local_78 stack copy), with the
    // format/stencil words patched from the owner when they differ (see above). No guest memory
    // is written: the patch lands in this stack copy only.
    alignas(8) u64 block8[8];
    std::memcpy(block8, reinterpret_cast<const void*>(view + 0x164), sizeof(block8));
    if (out.aliased_owner) {
        PatchDepthFormatFromOwner(block8, owner);
    }
    AmdGpu::DepthBuffer db{};
    AmdGpu::DepthView depth_view{};
    VAddr htile = 0;
    u32 extent = 0;
    if (!Rt::DecodeDepth(block8, db, depth_view, htile, extent)) {
        return "depth block undecodable";
    }
    out.set = {};
    out.set.depth_buffer = db;
    out.set.depth_view = depth_view;
    out.set.depth_htile = htile;
    out.set.depth_extent_hint = extent;
    out.set.depth.format = Rt::DepthVkFormat(db);
    out.set.depth_read_only = depth_view.z_read_only != 0 && depth_view.stencil_read_only != 0;
    out.set.depth.active = out.set.depth.format != vk::Format::eUndefined;
    if (!out.set.depth.active) {
        return "depth format not nameable";
    }
    // The dimensions: the owner +0x194 pair (the guest's viewport source), sanity-checked the
    // way the RT binder checks the extents (the +0x198/+0x19c words of the view descriptor are
    // NOT the viewport source on this path — the guest reads them only for the RT sets).
    out.set.width = out.width;
    out.set.height = out.height;
    out.set.load = vk::AttachmentLoadOp::eLoad;
    out.set.store = vk::AttachmentStoreOp::eStore;
    return nullptr;
}

// -- 0xcc9b0 RenderShadows -----------------------------------------------------------------------

const char* Shadow::DeriveShadows(u64 R, u64 shadow_cfg, u64 unused, u64 ctx, ShadowsPlan& out) {
    (void)unused;
    if (R == 0 || ctx == 0) {
        return "null renderer/context";
    }
    const u64 rc = ReadU64(ctx, Guest::ContextOff::RenderContext);  // ctx[2]
    const u64 cfg_flags = ReadU64(ctx, Guest::ContextOff::ConfigFlags); // ctx[9]
    if (rc == 0 || cfg_flags == 0) {
        return "null render-context/config";
    }
    // Guest gate 1 (000cc9b0.c:64): shadow config present && *(ctx[9]+0x188) == 0. When the
    // config is null the guest runs the scope bookkeeping only — chaining reproduces that.
    if (shadow_cfg == 0) {
        return "no shadow config";
    }
    if (ReadU64(cfg_flags, 0x188) != 0) {
        return "override flag set";
    }
    // ctx[0] is the pass context (its own [0] = the GNM cursor cb) the guest helpers and the
    // depth-touch mirrors address (the corrected job-ctx model: job_ctx[0] = pass ctx).
    out.pass = ReadU64(ctx, 0);
    out.job_ctx = ctx;
    // Guest gate 2 (000cc9b0.c:78): *(s8*)(ctx[9]+0xe9) != 0 && *(byte*)(shadow_cfg+0x170) & 1.
    // Both false keep the base set + clear + the five touches (they run outside this gate).
    const bool cascades = ReadU8(cfg_flags, 0xe9) != 0 &&
                          (ReadU8(shadow_cfg, ShadowCfg::CascadeEnable) & 1) != 0;
    out.cascades_enabled = cascades;

    // The base RT set at ctx[3] + 0x31c0..0x3208 (000cc9b0.c:65-76): ctx[3] is the target
    // manager V (Guest::ContextOff::TargetManager) — NOT the render context.
    const u64 v = ReadU64(ctx, Guest::ContextOff::TargetManager);
    if (v == 0) {
        return "null target manager";
    }
    alignas(8) u64 base_targets[10];
    for (u32 i = 0; i < 10; ++i) {
        base_targets[i] = ReadU64(v, ShadowCtx::ShadowSetBase + u64(i) * 8);
    }
    if (!DeriveTargetSet(base_targets, out.base_set)) {
        return "shadow base RT set undecodable";
    }
    out.view_desc = base_targets[8];
    // The kind-2 clear (000cc9b0.c:77 = 0x932a0(0, 1.0, 1.0, ctx, 2, 0)): depth-only, depth
    // cleared to 0.0 (1.0 - param_3 per 000932a0.c:37), stencil 0.
    out.clear_base = true;
    out.base_set.clear_depth = 0.0f;

    // The cascade loop (000cc9b0.c:79-126). Derivation only — the per-cascade context writes
    // happen at the commit (they are guest-visible state).
    // ctx[7] (the decomp's param_4[7], qword offset 7*8) carries the per-cascade cull-flag block
    // pointer the loop reads at +0xacfc - k*0x50 (000cc9b0.c:92).
    const u64 ctx7 = ReadU64(ctx, 7 * 8);
    for (u32 k = 0; k < CascadeCount; ++k) {
        const s64 stride = -static_cast<s64>(k) * 8;
        CascadePlan& cp = out.cascade[k];
        cp.info = ReadCascade(k);
        // RC+0xdf8/-0xe50 hold the per-cascade target/label VALUES (000cc9b0.c:84/91: the loop
        // reads *(RC + base + lVar5*8) with lVar5 = 0,-1,-2,...).
        cp.info.target = ReadU64(rc, ShadowCtx::CascadeTargets + stride);
        cp.info.label = ReadU64(rc, ShadowCtx::CascadeLabels + stride);
        cp.info.draw_table = R + 0x4a40 + stride;
        cp.info.cascade_id = 4 - k;
        cp.info.cull_wanted = k != 0 && ctx7 != 0 &&
                              ReadU8(ctx7, ShadowCfg::CullFlags + s64(k) * -s64(ShadowCfg::CullFlagStride)) != 0;
        cp.set_derived = false;
        // The per-cascade RT set (the RenderShadowCascade job body's array, 000cdbc0.c:33-45):
        // 4 qwords at ctx[3] + 0x2400 + (id+0x23)*0x50, 4 more at +0x20, then +0x40/+0x48.
        // Derived here for the trace and for the commit's view resolution.
        if (v != 0) {
            const u64 blk = v + ShadowCtx::PerViewRecords + u64(cp.info.cascade_id + 0x23) * 0x50;
            alignas(8) u64 targets[10];
            targets[0] = ReadU64(blk, 0x00);
            targets[1] = ReadU64(blk, 0x08);
            targets[2] = ReadU64(blk, 0x10);
            targets[3] = ReadU64(blk, 0x18);
            targets[4] = ReadU64(blk, 0x20);
            targets[5] = ReadU64(blk, 0x28);
            targets[6] = ReadU64(blk, 0x30);
            targets[7] = ReadU64(blk, 0x38);
            targets[8] = ReadU64(blk, 0x40);
            targets[9] = ReadU64(blk, 0x48);
            // N2-16: the per-view record block's producer is untraced; a slot holding a null or
            // non-depth pointer keeps the whole cascade on the guest chain (never guess).
            if (targets[8] != 0 && DeriveTargetSet(targets, cp.set)) {
                cp.set.clear_depth = 0.0f;
                cp.set_derived = cp.set.depth.active;
            }
        }
    }

    // The five post-loop touches (000cc9b0.c:128-137): DB_DEPTH_CLEAR = 0.0 then the 0x93d70
    // wrapper over ctx[3] + 0x2120 + 8k.
    for (u32 i = 0; i < CascadeCount; ++i) {
        out.touch_desc[i] = ReadU64(v, ShadowCtx::TouchBase + u64(i) * 8);
        TouchBranches(out.touch_desc[i], &out.touch_clear[i], nullptr);
    }

    // The standing emission gate: the cascade caster (0xce850 -> the 0x11d3d0 group loops and
    // the 0x11d7a0 XPR batch draws) and the cull job are not reissued natively in this phase, so
    // committing the cascade passes here would drop the depth writes between them. Trace-only
    // until the caster group loops land behind the native-draw/indirect seams.
    return "cascade caster/cull jobs still emulated";
}

// -- 0xc59d0 G-buffer runner ---------------------------------------------------------------------

const char* Shadow::DeriveGBuffer(u64 R, u64 view_cfg, u64 ctx, u64 draw_table, GBufferPlan& out) {
    (void)R;
    if (ctx == 0 || view_cfg == 0 || draw_table == 0) {
        return "null renderer/context/view-config";
    }
    const u64 cfg_flags = ReadU64(ctx, Guest::ContextOff::ConfigFlags); // ctx[9]
    if (cfg_flags == 0) {
        return "null per-frame config";
    }
    const u64 v = ReadU64(ctx, Guest::ContextOff::TargetManager); // ctx[3]
    if (v == 0) {
        return "null target manager";
    }
    const u64 label_slots = ReadU64(ctx, Guest::ContextOff::LabelSlots); // ctx[0x1d]
    if (label_slots == 0) {
        return "null label slots";
    }

    // Step 2 (000c59d0.c:161-187): SetA (V+0x29a0), clear kind 6 when *(s8*)(ctx[9]+0x1c) == 0,
    // else the kRTT_MainDepth-reuse bind without the clear.
    out.clear_a = ReadU8(cfg_flags, 0x1c) == 0;
    alignas(8) u64 targets[10];
    for (u32 i = 0; i < 10; ++i) {
        targets[i] = ReadU64(v, ShadowCtx::SetADesc + u64(i) * 8);
    }
    if (!DeriveTargetSet(targets, out.set_a)) {
        return "SetA descriptor undecodable";
    }
    out.set_a.clear_depth = out.clear_a ? 0.0f : 1.0f;

    // Step 7 (000c59d0.c:215-227): V+0x2590 + clear kind 1 (colour slots only).
    for (u32 i = 0; i < 10; ++i) {
        targets[i] = ReadU64(v, ShadowCtx::SetAuxDesc + u64(i) * 8);
    }
    if (!DeriveTargetSet(targets, out.set_aux)) {
        return "V+0x2590 descriptor undecodable";
    }

    // Steps 8-9 (000c59d0.c:228-278): the conditional SetB (V+0x2a40) + V+0x24a0 double
    // bind+clear, then the unconditional final SetB bind. The condition: all nine 0x20-stride
    // vector pairs of the draw table empty (begin == end at +0x248 + 0x20*i), or the two
    // per-frame override flags.
    bool tables_empty = true;
    for (u32 i = 0; i < 9; ++i) {
        const u64 base = 0x248 + u64(i) * 0x20;
        if (ReadU64(draw_table, base) != ReadU64(draw_table, base + 8)) {
            tables_empty = false;
            break;
        }
    }
    out.aux_clears = tables_empty || ReadU32(cfg_flags, 0x3c) != 0 ||
                     ReadU8(cfg_flags, 0x198) != 0;
    for (u32 i = 0; i < 10; ++i) {
        targets[i] = ReadU64(v, ShadowCtx::SetBDesc + u64(i) * 8);
    }
    if (!DeriveTargetSet(targets, out.set_b)) {
        return "SetB descriptor undecodable";
    }
    if (out.aux_clears) {
        for (u32 i = 0; i < 10; ++i) {
            targets[i] = ReadU64(v, ShadowCtx::SetAux0Desc + u64(i) * 8);
        }
        if (!DeriveTargetSet(targets, out.set_aux0)) {
            return "V+0x24a0 descriptor undecodable";
        }
    }

    // Labels (000c59d0.c:265-266/296): WAIT +0x18/+0x30 before the final SetB bind, EOP +0xd0
    // after the kRTT_MainDepth touch.
    out.label_wait0 = label_slots + 0x18;
    out.label_wait1 = label_slots + 0x30;
    out.label_done = label_slots + 0xd0;
    out.main_depth_touch = ReadU64(v, ShadowCtx::MainDepthDesc);
    if (out.main_depth_touch == 0) {
        return "null kRTT_MainDepth descriptor";
    }
    // Step 3 (000c59d0.c:188): the DepthPrePass group-enable word (5 groups, bits 0..4).
    out.group_mask = 0x1f;

    // The standing emission gate: the RenderDepth job body (0xc1860 — the five kRS_Depth_*
    // group loops + the XPR batch draws) and the scheduled follow-ups (fur, decals, AO,
    // particles, skybox) stay emulated in this phase, so a native SetB scope here would race
    // the emulated job draws on the same attachments. Trace-only until those groups land.
    return "depth pre-pass job groups still emulated";
}

// -- Commits (below the emission gate only) ------------------------------------------------------

bool Shadow::CommitDepthClear(Vulkan::Scheduler& scheduler, const DepthClearPlan& plan) const {
    const u64 pass_addr = plan.pass;
    if (pass_addr == 0) {
        return false;
    }
    const u64 cb = ReadU64(pass_addr, Guest::PassOff::Device);
    if (cb == 0) {
        return false;
    }
    // Resolve the depth view FIRST (all-or-nothing: ResolveViews mutates texture-cache state).
    TargetSet set = plan.set;
    if (!pass_ref.ResolveViews(set)) {
        return false;
    }
    if (!set.depth.active || set.depth.view == vk::ImageView{}) {
        return false;
    }

    // --- Commit point (guest-visible stores below) ---
    // 00093ef0.c:48-49 (the guarded emit's cache: the DB block pointer + depth-dirty clear —
    // the same pointer-identity contract the RT binder mirrors) and :67-70 (CB_TARGET_MASK = 0
    // + cache + state-dirty bit clear).
    WriteU64(pass_addr, 0xa8, plan.view_desc + 0x164);
    WriteU8(pass_addr, Guest::PassOff::DepthDirty, 0);
    WriteU32(pass_addr, 0x1ac, 0);
    WriteU8(pass_addr, Guest::PassOff::StateDirty,
            ReadU8(pass_addr, Guest::PassOff::StateDirty) & 0xef);

    // The 0x85de40(0.5, 0.5, ...) viewport/scissor mirror (00093ef0.c:81-87).
    WriteU32(pass_addr, 0x58, 0);
    WriteU32(pass_addr, 0xb4, 0);
    WriteU32(pass_addr, 0x5c, plan.width);
    WriteU32(pass_addr, 0xbc, plan.height);
    WriteU64(pass_addr, 0xc0, 0x3f0000003f000000ull);

    // The prim reset (00093ef0.c:89-103): draw-kind 0, RectList 0x11, ctx+0x14 = 0, ctx[4] =
    // the RectList stage descriptor (PTR_DAT_01604408, image-anchored — read through the image
    // base, the N2-3 lesson).
    WriteU32(cb, Guest::DeviceOff::DrawKind, 0);
    WriteU32(pass_addr, 0x14, 0);
    const u64 image_base = Guest::GuestImageBase();
    if (image_base != 0) {
        WriteU64(pass_addr, 0x20, ReadU64(image_base, 0x1604408));
    }
    WriteU32(pass_addr, 0x18, 0x11);

    // The pass: depth-only, load-op clear 0.0 (the DB_DEPTH_CLEAR = 0.0 the guest co-emits),
    // store. The fullscreen-rect draw (0x93690) rides inside the scope when the pipeline seam
    // resolves; either way the cleared attachment is what the guest's depth surface holds.
    PassAttachments atts;
    atts.color_count = 0;
    atts.depth = set.depth.view;
    atts.width = plan.width;
    atts.height = plan.height;
    atts.depth_load = vk::AttachmentLoadOp::eClear;
    atts.depth_store = vk::AttachmentStoreOp::eStore;
    atts.depth_read_only = false;
    atts.clear_depth = plan.clear_depth;
    const Vulkan::Pipeline* pipeline = ResolveFullscreenPipelineChain();
    const bool draw_rect = pipeline != nullptr;
    scheduler.RecordDeferred([this, atts, draw_rect, pipeline](vk::CommandBuffer cmdbuf) {
        pass_ref.Begin(cmdbuf, atts);
        pass_ref.SetPassViewportScissor(cmdbuf, atts.width, atts.height);
        if (draw_rect) {
            draw_ref.BindPipeline(cmdbuf, *pipeline);
            draw_ref.FullscreenRect(cmdbuf);
        }
        pass_ref.End(cmdbuf);
    });
    return true;
}

bool Shadow::CommitShadows(Vulkan::Scheduler& scheduler, const ShadowsPlan& plan) const {
    const u64 pass = plan.pass;
    if (pass == 0) {
        return false;
    }
    const u64 cb = ReadU64(pass, Guest::PassOff::Device);
    if (cb == 0) {
        return false;
    }
    // All-or-nothing view resolution FIRST (ResolveViews mutates texture-cache state): the base
    // set, then every enabled cascade's set. A single miss keeps everything on the guest chain.
    TargetSet base = plan.base_set;
    if (!pass_ref.ResolveViews(base)) {
        return false;
    }
    constexpr u32 kCascades = 5;
    TargetSet cascade[kCascades];
    for (u32 k = 0; k < kCascades; ++k) {
        cascade[k] = plan.cascade[k].set;
        // A cascade whose per-view record block did not decode to a depth target (N2-16: the
        // producer of that block is untraced) keeps the whole call on the guest chain: the
        // caster would otherwise render with no attachment.
        if (plan.cascades_enabled &&
            (!plan.cascade[k].set_derived || !pass_ref.ResolveViews(cascade[k]))) {
            return false;
        }
    }

    // --- Commit point (guest-visible stores below) ---
    // The cascade-loop context writes (000cc9b0.c:84-90): the guest body wrote them BEFORE the
    // jobs were scheduled; the native path writes the same values (then skips the emulated
    // scheduling), so the mirrored frame state agrees.
    const u64 job_ctx = plan.job_ctx;
    if (job_ctx != 0) {
        const u64 rc = ReadU64(job_ctx, Guest::ContextOff::RenderContext);
        (void)rc;
        const u64 shadow_cfg = ReadU64(Guest::RendererPtr(), Guest::RendererOff::ShadowConfig);
        (void)shadow_cfg;
        for (u32 k = 0; k < kCascades; ++k) {
            const ShadowCascade& ci = plan.cascade[k].info;
            WriteU64(job_ctx, CascadeCtx::Target, ci.target);
            WriteU32(job_ctx, CascadeCtx::Split, F32Bits(ci.split));
            WriteU32(job_ctx, CascadeCtx::Bias, F32Bits(ci.bias));
            WriteU32(job_ctx, CascadeCtx::CascadeId, ci.cascade_id);
            WriteU64(job_ctx, CascadeCtx::DrawTable, ci.draw_table);
            const u64 image_base = Guest::GuestImageBase();
            if (image_base != 0) {
                WriteU64(job_ctx, CascadeCtx::ScopeName,
                         ReadU64(image_base, kScopeNamesVa - u64(k) * 8));
            }
        }
    }

    // The base RT-set cache mirrors (000cc9b0.c:76 -> the 0x92140 hook's own caches): the RT
    // binder hook (native-rt stage) is not active under SOTC_RENDER=native-shadow, so nothing
    // else maintains them. Depth block pointer + dirty clear, the pointer-identity contract.
    const u64 view = plan.view_desc;
    WriteU64(pass, 0xa8, view + 0x164);
    WriteU8(pass, Guest::PassOff::DepthDirty, 0);

    // The base depth-only pass with the kind-2 clear folded into the load op (0x932a0 kind 2 =
    // depth 0.0 + stencil 0; 000cc9b0.c:77).
    PassAttachments base_atts;
    base_atts.color_count = 0;
    base_atts.depth = base.depth.view;
    base_atts.width = base.width;
    base_atts.height = base.height;
    base_atts.depth_load = plan.clear_base ? vk::AttachmentLoadOp::eClear
                                           : vk::AttachmentLoadOp::eLoad;
    base_atts.depth_store = vk::AttachmentStoreOp::eStore;
    base_atts.clear_depth = base.clear_depth;
    base_atts.clear_stencil = 0;

    // The five per-cascade depth-only passes (the RenderShadowCascade bodies' RT binds; the
    // caster draws stay emulated — this records the attachment layouts and the clears only).
    struct CascadeAtt {
        bool active{false};
        PassAttachments atts;
    };
    CascadeAtt cas[kCascades];
    for (u32 k = 0; k < kCascades; ++k) {
        cas[k].active = plan.cascades_enabled && plan.cascade[k].set_derived;
        if (!cas[k].active) {
            continue;
        }
        cas[k].atts.color_count = 0;
        cas[k].atts.depth = cascade[k].depth.view;
        cas[k].atts.width = cascade[k].width;
        cas[k].atts.height = cascade[k].height;
        cas[k].atts.depth_load = vk::AttachmentLoadOp::eClear;
        cas[k].atts.depth_store = vk::AttachmentStoreOp::eStore;
        cas[k].atts.clear_depth = 0.0f;
    }

    // The five touches (000cc9b0.c:128-137): DB_DEPTH_CLEAR = 0.0 + the 0x93d70 wrapper per
    // cascade target. When the touch takes the 0x93ef0 branch it re-clears the surface — the
    // native mirror folds that into its own depth load-op clear (0x932a0's clearing behaviour,
    // §2.4 "load-op clear on the next depth pass"); the ACQUIRE_MEM halves are the native
    // pipeline barriers' business (the scheduler owns the submission order).
    struct TouchAtt {
        bool active{false};
        PassAttachments atts;
    };
    TouchAtt touch[kCascades];
    for (u32 i = 0; i < kCascades; ++i) {
        touch[i].active = plan.touch_clear[i] && plan.touch_desc[i] != 0;
        if (!touch[i].active) {
            continue;
        }
        TargetSet tset;
        DepthClearPlan tplan;
        tplan.pass = pass;
        if (DeriveDepthClear(pass, plan.touch_desc[i], plan.touch_desc[i], 0, tplan) == nullptr) {
            tset = tplan.set;
            if (pass_ref.ResolveViews(tset)) {
                touch[i].atts.color_count = 0;
                touch[i].atts.depth = tset.depth.view;
                touch[i].atts.width = tplan.width;
                touch[i].atts.height = tplan.height;
                touch[i].atts.depth_load = vk::AttachmentLoadOp::eClear;
                touch[i].atts.depth_store = vk::AttachmentStoreOp::eStore;
                touch[i].atts.clear_depth = 0.0f;
            } else {
                touch[i].active = false;
            }
        } else {
            touch[i].active = false;
        }
    }

    scheduler.RecordDeferred([this, base_atts, cas, touch](vk::CommandBuffer cmdbuf) {
        pass_ref.Begin(cmdbuf, base_atts);
        pass_ref.End(cmdbuf);
        for (u32 k = 0; k < 5; ++k) {
            if (cas[k].active) {
                pass_ref.Begin(cmdbuf, cas[k].atts);
                pass_ref.End(cmdbuf);
            }
        }
        for (u32 i = 0; i < 5; ++i) {
            if (touch[i].active) {
                pass_ref.Begin(cmdbuf, touch[i].atts);
                pass_ref.End(cmdbuf);
            }
        }
    });
    (void)cb;
    return true;
}

bool Shadow::CommitGBuffer(Vulkan::Scheduler& scheduler, const GBufferPlan& plan) const {
    const u64 pass = plan.pass;
    if (pass == 0) {
        return false;
    }
    const u64 cb = ReadU64(pass, Guest::PassOff::Device);
    if (cb == 0) {
        return false;
    }
    // All-or-nothing view resolution FIRST (ResolveViews mutates texture-cache state).
    TargetSet set_a = plan.set_a;
    TargetSet set_aux = plan.set_aux;
    TargetSet set_b = plan.set_b;
    TargetSet set_aux0 = plan.set_aux0;
    if (!pass_ref.ResolveViews(set_a) || !pass_ref.ResolveViews(set_aux) ||
        !pass_ref.ResolveViews(set_b)) {
        return false;
    }
    if (plan.aux_clears && !pass_ref.ResolveViews(set_aux0)) {
        return false;
    }

    // --- Commit point (guest-visible stores below) ---
    // The group-enable word (000c59d0.c:188; the DepthPrePass job body reads it — emulated, so
    // the word is maintained exactly as the guest would hold it).
    WriteU32(plan.job_ctx, 0xf8, plan.group_mask);

    // The RT-cache mirrors for SetA (the kRTT_MainDepth pointer-identity contract; the guest's
    // other cache slots are the RT binder hook's business — its hook may not be active under
    // this stage, but nothing else reads those slots on this path).
    WriteU64(pass, 0xa8, plan.set_a_desc + 0x164);
    WriteU8(pass, Guest::PassOff::DepthDirty, 0);

    // SetA depth-only pass, clear kind 6 folded into the load op (000c59d0.c:172-173: depth
    // 0.0, stencil 0 when clearing; :176-186 without).
    PassAttachments atts_a;
    atts_a.color_count = 0;
    atts_a.depth = set_a.depth.view;
    atts_a.width = set_a.width;
    atts_a.height = set_a.height;
    atts_a.depth_load = plan.clear_a ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
    atts_a.depth_store = vk::AttachmentStoreOp::eStore;
    atts_a.clear_depth = plan.set_a.clear_depth;

    // The auxiliary set + clear kind 1 (colour slots only; 000c59d0.c:226-227).
    PassAttachments atts_aux;
    atts_aux.color_count = set_aux.color_count;
    for (u32 i = 0; i < set_aux.color_count; ++i) {
        atts_aux.color[i] = set_aux.color[i].view;
    }
    atts_aux.width = set_aux.width;
    atts_aux.height = set_aux.height;
    atts_aux.color_load = vk::AttachmentLoadOp::eClear;
    atts_aux.color_store = vk::AttachmentStoreOp::eStore;
    for (u32 i = 0; i < 8; ++i) {
        atts_aux.clear_color[i][0] = 1.0f;
        atts_aux.clear_color[i][1] = 1.0f;
        atts_aux.clear_color[i][2] = 1.0f;
        atts_aux.clear_color[i][3] = 1.0f;
    }

    // The conditional SetB + V+0x24a0 binds with clear kind 1 (000c59d0.c:249-263), then the
    // final SetB bind (load — the clears above only run in the conditional block).
    PassAttachments atts_b = {};
    atts_b.color_count = set_b.color_count;
    for (u32 i = 0; i < set_b.color_count; ++i) {
        atts_b.color[i] = set_b.color[i].view;
    }
    atts_b.depth = set_b.depth.view;
    atts_b.width = set_b.width;
    atts_b.height = set_b.height;
    atts_b.depth_load = vk::AttachmentLoadOp::eLoad;
    atts_b.depth_store = vk::AttachmentStoreOp::eStore;

    PassAttachments atts_aux0 = {};
    atts_aux0.color_count = set_aux0.color_count;
    for (u32 i = 0; i < set_aux0.color_count; ++i) {
        atts_aux0.color[i] = set_aux0.color[i].view;
    }
    atts_aux0.width = set_aux0.width;
    atts_aux0.height = set_aux0.height;
    atts_aux0.color_load = vk::AttachmentLoadOp::eClear;
    atts_aux0.color_store = vk::AttachmentStoreOp::eStore;
    for (u32 i = 0; i < 8; ++i) {
        atts_aux0.clear_color[i][0] = 1.0f;
        atts_aux0.clear_color[i][1] = 1.0f;
        atts_aux0.clear_color[i][2] = 1.0f;
        atts_aux0.clear_color[i][3] = 1.0f;
    }

    // The kRTT_MainDepth touch (000c59d0.c:295) when it takes the 0x93ef0 branch.
    PassAttachments atts_touch{};
    bool touch_active = false;
    {
        DepthClearPlan tplan;
        tplan.pass = pass;
        if (DeriveDepthClear(pass, plan.main_depth_touch, plan.main_depth_touch, 0, tplan) ==
            nullptr) {
            TargetSet tset = tplan.set;
            if (pass_ref.ResolveViews(tset)) {
                touch_active = true;
                atts_touch.depth = tset.depth.view;
                atts_touch.width = tplan.width;
                atts_touch.height = tplan.height;
                atts_touch.depth_load = vk::AttachmentLoadOp::eClear;
                atts_touch.depth_store = vk::AttachmentStoreOp::eStore;
                atts_touch.clear_depth = 0.0f;
            }
        }
    }

    scheduler.RecordDeferred([this, atts_a, atts_aux, atts_b, atts_aux0, atts_touch, touch_active,
                              clears = plan.aux_clears](vk::CommandBuffer cmdbuf) {
        pass_ref.Begin(cmdbuf, atts_a);
        pass_ref.End(cmdbuf);
        pass_ref.Begin(cmdbuf, atts_aux);
        pass_ref.End(cmdbuf);
        if (clears) {
            pass_ref.Begin(cmdbuf, atts_b);
            pass_ref.End(cmdbuf);
            pass_ref.Begin(cmdbuf, atts_aux0);
            pass_ref.End(cmdbuf);
        }
        if (touch_active) {
            pass_ref.Begin(cmdbuf, atts_touch);
            pass_ref.End(cmdbuf);
        }
    });
    (void)cb;
    return true;
}

} // namespace Native
