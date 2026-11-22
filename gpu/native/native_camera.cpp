// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_camera implementation (Phase 2.9, native-camera).
//
// Contract: docs/engine/native-render-map-cull-camera-shadow.md §4 (MainCamera 0xcaed0, proven
// against out/re/decomp/000c/000caed0.c) plus §4.2 (matrix locations) and §4.3 (the per-view
// record block). DeriveCamera is READ-ONLY: it reads guest fields, decodes the SetA target array
// through the Phase 2.8 helper and returns the first failing gate. It never writes guest state.
#include "native/native_camera.h"

#include <cstring>

#include "native/native_shadow.h" // Shadow::DeriveTargetSet (the 0x92140 array decode, reused)

namespace Native {

namespace {

// ctx field offsets the derivation needs beyond Guest::ContextOff (all proven in 000caed0.c).
constexpr u64 CtxMainTarget = 0x20;       // ctx[4] -> copied to ctx[6] at :204
constexpr u64 CtxViewConstants0 = 0x100;  // ctx[0x20] (§4.2)
constexpr u64 CtxViewConstants1 = 0x140;  // ctx[0x28]
constexpr u64 CtxViewConstants2 = 0x180;  // ctx[0x30] (aliases [0x20])
constexpr u64 CtxViewId = 0x98;           // ctx+0x98: the uploaded view-constant VA/hint pair
constexpr u64 CtxParticleFlags = 0xfc;    // *(u8*)(ctx+0xfc), bit1 selects particle view 11

// RC / V / R field offsets.
constexpr u64 RcSpotCount = 0xfe0;        // *(int*)(RC+0xfe0): number of spot lights
constexpr u64 RcJobSlotsBegin = 0xe98;    // *(RC+0xe98): job-slot vector begin
constexpr u64 RcJobSlotsEnd = 0xea0;      // *(RC+0xea0): job-slot vector end
constexpr u64 RcFirstCounter = 0xe28;     // RC+0xe28..0xe80: the 12 fixed job counters
constexpr u64 VSetA = 0x29a0;             // V+0x29a0..0x29e8: the depth-only SetA array
constexpr u64 VViewRecords = 0x2400;      // V+0x2400+id*0x50: the per-view record block
constexpr u64 RFramePtr = 0x40;           // R+0x40 = F
constexpr u64 FFrameConfig = 0xdc0;       // F+0xdc0 -> particle lightmap state

// View-config (F+0xb8) fields used by the frame-setup flush checks.
constexpr u64 ViewListBegin = 0x1a8;      // stride 0xa8 entries
constexpr u64 ViewListEnd = 0x1b0;
constexpr u64 FlushBegin = 0x90;          // per-entry: begin != end => pending
constexpr u64 FlushEnd = 0x98;
constexpr u64 FurListBegin = 0x28;
constexpr u64 FurListEnd = 0x30;

// ctx[9] (per-frame config) fields.
constexpr u64 CfgSpotGate = 0x118;        // spot-light shadows enabled (000cc690.c:44)
constexpr u64 CfgShadowOverride = 0x188;  // RenderShadows override (000cc9b0.c:64)
constexpr u64 CfgFurGate = 0x454;         // fur pre-render (000caed0.c:235)
constexpr u64 CfgFurSuppress = 0x4d4;

inline u8 ReadU8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 ReadU32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}
inline u64 ReadU64(u64 addr, u64 off) {
    return *reinterpret_cast<const u64*>(addr + off);
}

/// The frame-setup flush check (000caed0.c:185-192 / :197-203): walk the 0xa8-stride view list
/// and report the first entry whose +0x90/+0x98 pair differs. Read-only; bounded so a malformed
/// list can never spin.
bool PendingViewFlush(u64 view_cfg) {
    if (view_cfg == 0) {
        return false;
    }
    const u64 begin = ReadU64(view_cfg, ViewListBegin);
    const u64 end = ReadU64(view_cfg, ViewListEnd);
    if (begin == 0 || end <= begin) {
        return false;
    }
    const u64 span = end - begin;
    if ((span % 0xa8) != 0 || span > 0x40000) {
        return true; // non-empty but malformed: treat as pending (the guest would iterate)
    }
    for (u64 entry = begin; entry != end; entry += 0xa8) {
        if (ReadU64(entry, FlushBegin) != ReadU64(entry, FlushEnd)) {
            return true;
        }
    }
    return false;
}

/// Reads one per-view record block (V+0x2400+id*0x50). The ten qwords are the four +0x00..0x18,
/// the four +0x20..0x38 and the pair +0x40/+0x48 (000caed0.c:490-504, 000cdbc0.c:32-45).
void ReadViewRecord(u64 target_manager, u32 id, ViewRecord& out) {
    out = {};
    out.id = id;
    out.address = target_manager + VViewRecords + u64(id) * 0x50;
    for (u32 i = 0; i < 4; ++i) {
        out.target[i] = ReadU64(out.address, u64(i) * 8);
        out.target[4 + i] = ReadU64(out.address, 0x20 + u64(i) * 8);
    }
    out.target[8] = ReadU64(out.address, 0x40);
    out.target[9] = ReadU64(out.address, 0x48);
}

} // namespace

ViewConstants Camera::Read(VAddr block) {
    ViewConstants c{};
    const u32* d = reinterpret_cast<const u32*>(block);
    auto copy = [&](f32* dst, u32 dword_offset, u32 count) {
        std::memcpy(dst, d + dword_offset, count * sizeof(f32));
    };
    copy(c.world, 0x00, 16);
    copy(c.inv_world, 0x10, 16);
    copy(c.camera, 0x20, 16);
    copy(c.inv_camera, 0x30, 16);
    copy(c.view, 0x40, 16);
    copy(c.projection, 0x50, 16);
    c.width = reinterpret_cast<const f32*>(d + 0x80)[0];
    c.height = reinterpret_cast<const f32*>(d + 0x81)[0];
    c.inv_width = reinterpret_cast<const f32*>(d + 0x82)[0];
    c.inv_height = reinterpret_cast<const f32*>(d + 0x83)[0];
    c.near_plane = reinterpret_cast<const f32*>(d + 0x84)[0];
    c.far_plane = reinterpret_cast<const f32*>(d + 0x85)[0];
    c.far_minus_near = reinterpret_cast<const f32*>(d + 0x86)[0];
    c.near_times_far = reinterpret_cast<const f32*>(d + 0x87)[0];
    c.proj_scale = reinterpret_cast<const f32*>(d + 0x93)[0];
    copy(&c.frustum[0][0], 0xa0, 16);
    return c;
}

const char* Camera::DeriveCamera(u64 R, u64 view_cfg, u64 ctx, CameraPlan& out) {
    out = {};
    if (R == 0 || ctx == 0) {
        return "null renderer/context";
    }
    out.renderer = R;
    out.view_cfg = view_cfg;
    out.ctx = ctx;

    // The object model (000caed0.c:167-169): ctx[2] RC, ctx[3] V, ctx[9] per-frame config,
    // ctx[0x1d] the label-slot array, R+0x8f8 the shadow config.
    out.render_context = ReadU64(ctx, Guest::ContextOff::RenderContext);
    out.target_manager = ReadU64(ctx, Guest::ContextOff::TargetManager);
    out.per_frame_cfg = ReadU64(ctx, Guest::ContextOff::ConfigFlags);
    out.label_slots = ReadU64(ctx, Guest::ContextOff::LabelSlots);
    if (out.render_context == 0) {
        return "null render-context";
    }
    if (out.target_manager == 0) {
        return "null target manager";
    }
    if (out.per_frame_cfg == 0) {
        return "null per-frame config";
    }
    if (out.label_slots == 0) {
        return "null label slots";
    }
    out.shadow_cfg = ReadU64(R, Guest::RendererOff::ShadowConfig);

    // The label slots (:193/:196/:270/:628).
    out.labels.frame_setup = out.label_slots + 0x38;
    out.labels.shadows_done = out.label_slots + 0x80;
    out.labels.gbuffer_ready = out.label_slots + 0xa8;
    out.labels.main_done = out.label_slots + 0xc0;

    // Frame setup: the flush checks (:185-192, :197-203), the two shadow calls (:194-195) and the
    // shadows-done EOP (:196).
    out.flush_before = PendingViewFlush(view_cfg);
    out.spot_shadow_call = true; // 0xcc690(R, ctx) is unconditional
    out.spot_shadows_enabled = ReadU8(out.per_frame_cfg, CfgSpotGate) != 0;
    if (out.spot_shadows_enabled) {
        out.spot_count = ReadU32(out.render_context, RcSpotCount);
    }
    out.shadows_call = true; // 0xcc9b0(R, shadow_cfg, unused, ctx) is unconditional
    out.shadows_enabled =
        out.shadow_cfg != 0 && ReadU64(out.per_frame_cfg, CfgShadowOverride) == 0;
    out.flush_after_shadows = PendingViewFlush(view_cfg);

    // Main RT / pass bind (:204-217): ctx[6] = ctx[4], ctx+0xf4 = 1, SetA at V+0x29a0.
    out.main_target = ReadU64(ctx, CtxMainTarget);
    out.pass_slot = 1;
    alignas(8) u64 targets[10];
    for (u32 i = 0; i < 10; ++i) {
        targets[i] = ReadU64(out.target_manager, VSetA + u64(i) * 8);
    }
    out.set_a_derived = Shadow::DeriveTargetSet(targets, out.set_a);
    if (!out.set_a_derived) {
        return "SetA (V+0x29a0) undecodable";
    }

    // SetViewParams (:218) and the view-constant locations (§4.2): the two 0x1228-byte blocks and
    // the uploaded view id. The pointers are recorded, never dereferenced here (SetViewParams has
    // not run yet at the entry hook).
    out.set_view_params = true;
    out.view_constants[0] = ReadU64(ctx, CtxViewConstants0);
    out.view_constants[1] = ReadU64(ctx, CtxViewConstants1);
    out.view_constants[2] = ReadU64(ctx, CtxViewConstants2);
    out.view_id = ReadU64(ctx, CtxViewId);

    // Forward jobs (:220-264).
    out.forward_clusters = ReadU64(R, 0x78) != 0;
    out.fur_pre = ReadU8(out.per_frame_cfg, CfgFurGate) != 0 &&
                  ReadU8(out.per_frame_cfg, CfgFurSuppress) == 0 &&
                  ReadU64(view_cfg, FurListBegin) != ReadU64(view_cfg, FurListEnd);
    const u64 frame_obj = ReadU64(R, RFramePtr);
    out.particle_lightmaps = frame_obj != 0 && ReadU64(frame_obj, FFrameConfig) != 0;

    // G-buffer (:270-272): WAIT +0xa8, ctx[0xc] = R+0xd8, then 0xc59d0(R, view_cfg, ctx, R+0xd8).
    out.gbuffer_wait = out.labels.gbuffer_ready;
    out.draw_table = R + Guest::RendererOff::DrawTable;
    out.gbuffer_call = true;

    // The per-view record block MainCamera itself reads (:429, :490-504): view 10 or 11.
    out.particle_view_id =
        10u | ((static_cast<u32>(ReadU8(ctx, CtxParticleFlags)) & 2u) >> 1);
    ReadViewRecord(out.target_manager, out.particle_view_id, out.particle_view);

    // Trailing CPU job waits (:629-686): the 12 fixed counters and the job-slot vector.
    for (u32 i = 0; i < 12; ++i) {
        out.job_counters[i] = out.render_context + RcFirstCounter + u64(i) * 8;
    }
    out.job_vector_begin = ReadU64(out.render_context, RcJobSlotsBegin);
    out.job_vector_end = ReadU64(out.render_context, RcJobSlotsEnd);
    out.main_done_eop = out.labels.main_done;

    // The draw-counter accumulation (:687-713): the nine pairs, (end-begin)>>5 records. Sampled
    // read-only for the trace; MainCamera only adds them into G+0x220384.. later.
    static constexpr u64 kDrawPairs[9][2] = {
        {0x120, 0x128}, {0x948, 0x950}, {0x1170, 0x1178}, {0x1998, 0x19a0}, {0x21c0, 0x21c8},
        {0x29e8, 0x29f0}, {0x3210, 0x3218}, {0x3a38, 0x3a40}, {0x4260, 0x4268},
    };
    for (u32 i = 0; i < 9; ++i) {
        const u64 begin = ReadU64(R, kDrawPairs[i][0]);
        const u64 end = ReadU64(R, kDrawPairs[i][1]);
        if (end > begin && (end - begin) >= 0x20) {
            out.draw_record_counts[i] = static_cast<u32>((end - begin) >> 5);
        }
    }

    // Standing gate: the frame body it orchestrates (shadows, G-buffer, forward/particle passes)
    // stays emulated until Phases 2.11/2.12 land, so the callback only traces and chains.
    return "frame bodies still emulated (2.11/2.12)";
}

} // namespace Native
