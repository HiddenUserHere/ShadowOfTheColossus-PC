// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_camera: RenderView::MainCamera (0xcaed0) reimplementation.
//
// Contract: docs/engine/native-render-map-cull-camera-shadow.md §4 (MainCamera, 6077 B), with
// §4.1 viewport/scissor, §4.2 where the matrices live and §4.3 the per-view record block. The
// body computes no matrices -- SetViewParams (0xbe1f0) does -- and is the pass-graph orchestrator:
// the frame-setup WAIT -> spot/cascade shadows -> shadows-done EOP -> main RT bind -> SetViewParams
// -> the G-buffer/forward calls -> the trailing CPU job-counter waits.
//
// Phase 2.9 (native-camera) derives that structure READ-ONLY for the trace. The bodies it
// orchestrates stay emulated until Phases 2.11/2.12, so DeriveCamera returns a standing gate
// reason and the entry callback (native_camera_callbacks.cpp) chains to the guest body. No path
// here mutates guest state.
#pragma once

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_device.h"
#include "native/native_pass.h"

namespace Native {

/// The view/projection constants SetViewParams produces (§4.2): a 0x1228-byte block, read from the
/// guest's own per-frame block (no recomputation; the recompiler's shaders expect these values).
struct ViewConstants {
    f32 world[16];       // [0x00..0x1f]
    f32 inv_world[16];   // [0x20..0x2f]
    f32 camera[16];      // [0x30..0x3f]
    f32 inv_camera[16];  // [0x40..0x4f]
    f32 view[16];        // [0x50..0x5f]
    f32 projection[16];  // [0x50..0x5f]
    f32 width;           // [0x80]
    f32 height;          // [0x81]
    f32 inv_width;       // [0x82]
    f32 inv_height;      // [0x83]
    f32 near_plane;      // [0x84]
    f32 far_plane;       // [0x85]
    f32 far_minus_near;  // [0x86]
    f32 near_times_far;  // [0x87]
    f32 proj_scale;      // [0x93] = F+0xd8c
    f32 frustum[4][4];   // [0xa0..0xaf]
};

/// MainCamera's EOP/WAIT label slots: ctx[0x1d] + offset. The slot array is 0x27 8-byte GPU
/// labels (allocated and set to 1 by 0x92500 at render-context setup; see
/// docs/engine/native-render-gaps.md N2-17/N2-31).
struct CameraLabels {
    VAddr frame_setup{0};   // ctx[0x1d]+0x38: WAIT_REG_MEM at 000caed0.c:193
    VAddr shadows_done{0};  // ctx[0x1d]+0x80: EOP at 000caed0.c:196
    VAddr gbuffer_ready{0}; // ctx[0x1d]+0xa8: WAIT_REG_MEM at 000caed0.c:270
    VAddr main_done{0};     // ctx[0x1d]+0xc0: EOP at 000caed0.c:628
};

/// One 0x50-byte per-view record block at V+0x2400+id*0x50 (§4.3). The readers bind the ten qwords
/// as a 0x92140 target array: +0x00..0x1f = four descriptors, +0x20..0x3f = four more, +0x40/+0x48
/// = the trailing pair (proven 000cdbc0.c:32-45, 000caed0.c:490-504). V is ctx[3] (the target
/// manager), NOT ctx[2] (RC). The producer is 0xa0a60 (register row N2-30).
struct ViewRecord {
    u32 id{0};            ///< view id (index into the V+0x2400 table)
    VAddr address{0};     ///< V + 0x2400 + id*0x50
    u64 target[10]{};     ///< the ten descriptor qwords, in bind order
};

/// The read-only derivation of one 0xcaed0 MainCamera call.
struct CameraPlan {
    u64 renderer{0};        ///< R (param_1)
    u64 view_cfg{0};        ///< F+0xb8 (param_2), the view list/state block
    u64 ctx{0};             ///< the job context (param_3)
    u64 render_context{0};  ///< ctx[2] = RC
    u64 target_manager{0};  ///< ctx[3] = V
    u64 per_frame_cfg{0};   ///< ctx[9]
    u64 shadow_cfg{0};      ///< *(R+0x8f8)
    u64 label_slots{0};     ///< ctx[0x1d]
    CameraLabels labels;

    // -- frame setup (000caed0.c:167-203) --
    bool flush_before{false};        ///< the +0x1a8/+0x1b0 view-list flush check (:185-192)
    bool spot_shadow_call{false};    ///< 0xcc690 is called (:194), unconditionally
    bool spot_shadows_enabled{false};///< *(u8*)(ctx[9]+0x118) != 0 (000cc690.c:44)
    u32 spot_count{0};               ///< *(u32*)(RC+0xfe0) when enabled (000cc690.c:58)
    bool shadows_call{false};        ///< 0xcc9b0 is called (:195), unconditionally
    bool shadows_enabled{false};     ///< shadow_cfg != 0 && *(u64*)(ctx[9]+0x188) == 0
    bool flush_after_shadows{false}; ///< the same check after the shadows-done EOP (:197-203)

    // -- main RT / pass bind (000caed0.c:204-217) --
    u64 main_target{0};      ///< ctx[4] (read at :168, stored to ctx[6] at :204)
    u32 pass_slot{0};        ///< ctx+0xf4 = 1 (:205)
    TargetSet set_a;         ///< V+0x29a0..+0x29e8 decoded (the depth-only SetA)
    bool set_a_derived{false};

    // -- SetViewParams (000caed0.c:218) and where the matrices live (§4.2) --
    bool set_view_params{false};  ///< 0xbe1f0(ctx, shadow_cfg) is called
    u64 view_constants[3]{};      ///< ctx[0x20]/[0x28]/[0x30]: the 0x1228-byte blocks
    u64 view_id{0};               ///< ctx+0x98: {upload VA, size/dword hint}

    // -- forward jobs (000caed0.c:220-264) --
    bool forward_clusters{false};   ///< *(R+0x78) != 0 -> 0xaaf80 CreateClustersJob
    bool fur_pre{false};            ///< ctx[9]+0x454 && !ctx[9]+0x4d4 && view_cfg+0x28 != +0x30
    bool particle_lightmaps{false}; ///< *(*(R+0x40)+0xdc0) != 0 -> 0x107510

    // -- G-buffer (000caed0.c:270-272) --
    bool gbuffer_call{false};  ///< 0xc59d0(R, view_cfg, ctx, R+0xd8) is called
    u64 draw_table{0};         ///< R+0xd8 (:271)
    VAddr gbuffer_wait{0};     ///< ctx[0x1d]+0xa8 (:270)

    // -- the per-view record block MainCamera itself reads (000caed0.c:429,490-504) --
    u32 particle_view_id{0};   ///< 10 | ((*(u8*)(ctx+0xfc) & 2) >> 1) -> 10 or 11
    ViewRecord particle_view;  ///< V+0x2400+id*0x50

    // -- trailing CPU job waits / counters (000caed0.c:629-713) --
    VAddr job_counters[12]{};  ///< RC+0xe28,0xe30,...,0xe80 (12 fixed counters)
    VAddr job_vector_begin{0}; ///< *(RC+0xe98)
    VAddr job_vector_end{0};   ///< *(RC+0xea0)
    VAddr main_done_eop{0};    ///< ctx[0x1d]+0xc0 (:628)
    u32 draw_record_counts[9]{}; ///< the nine draw-table pair counts ((end-begin)>>5)
};

class Camera {
public:
    explicit Camera(const Device& device) : device{device} {}

    /// Reads the view constants from the guest's block at `block` (the pfVar27 of SetViewParams).
    /// No matrix is recomputed: the guest already built them, and the recompiler's shaders expect
    /// exactly these values. Commit-path helper; DeriveCamera does not dereference the pointers.
    static ViewConstants Read(VAddr block);

    /// The projection-override toggle (F+0xd90): orthographic when set.
    static bool ProjectionOverride() {
        return *reinterpret_cast<const u32*>(Guest::FramePtr() + Guest::FrameOff::ProjOverride) != 0;
    }

    /// The derived projection constant (F+0xd8c).
    static f32 ProjectionConstant() {
        return *reinterpret_cast<const f32*>(Guest::FramePtr() + Guest::FrameOff::ProjConstant);
    }

    /// The Phase 2.9 entry. Derives MainCamera's structure and view constants READ-ONLY. Returns
    /// nullptr when the call could be reproduced natively, otherwise the first failing gate (a
    /// static string). Until the frame bodies it orchestrates are native (Phases 2.11/2.12) the
    /// standing return value is "frame bodies still emulated (2.11/2.12)".
    static const char* DeriveCamera(u64 R, u64 view_cfg, u64 ctx, CameraPlan& out);

private:
    const Device& device;
};

} // namespace Native
