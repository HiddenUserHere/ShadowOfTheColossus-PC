// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.11 (native-frame) + 2.12
// (RenderJob full body): the frame orchestrator. Per docs/engine/native-render-map-frame.md §5 and
// docs/engine/native-render-map-pass.md §5, one rendered frame is
//
//   0x25ae80 (game thread) -> 0xbcde0 (publish F) -> 0xcef90 -> 0xcefa0 (the frame body)
//
// 0xcefa0 (EndFrameViewParams) is the render graph root. On the GPU path it is a fixed sequence of
// phases, each of which already has (or is getting) a native reimplementation in gpu/native/:
//
//   1. reset        FUN_00032420(R+0x38) timer, memcpy of F+0x280 (0xac8 B config block)
//   2. setup        FUN_001a2cd0(R+0x50), FUN_00118440(G) -> 0x90860 (frame setup, view mode)
//   3. per-frame RT M+0x2b80 block -> FUN_00092140 (the Phase 2.5 binder)
//   4. copy labels  FUN_000cedf0(1/0xc/0xd), FUN_0019fed0 (video-out copy counts)
//   5. scene prep   FUN_0016c500 .. FUN_0016bb70 (engine scopes), G counters
//   6. view setup   FUN_000ca6d0 (Phase 2.10: per-sorted-view tables + label gates)
//   7. main scene   FUN_000caed0 (Phase 2.9 MainCamera: shadows -> G-buffer -> forward)
//   8. water        FUN_000e2d70 / 0x12a930 / 0xbe1f0, FUN_000d04e0
//   9. end-view     M+0x18..0xa0 matrices, FUN_0016bc50/16c520, M+0x24f0 block -> FUN_00092140
//  10. frame tail   FUN_00118470 (HUD, HUD resolve 0x93960 x2, flip wait 0x91f50)
//  11. submit       FUN_000c5560 (SubmitJob) -> FUN_000d07c0 (KickoffSubmitJob: 0x91f80/0x91f90)
//  12. release      FUN_000ccfd0 (job waits + frees)
//  13. fence        FUN_00090ea0 (per-frame reset/fence: parity frame slot + spin)
//
// The frame plan below is derived READ-ONLY from the guest's own structs (G/R/F/M and the job
// context). Renderer::RecordFrame records a frame natively only when the plan derives completely
// AND every step it would issue is native; otherwise it returns false and the emulated frame body
// runs (the caller falls back). Today every GPU step still chains, so RecordFrame always declines —
// this is by construction, not a failure: the derivation and the gate structure are what Phase 2.11
// delivers, and they are traceable with SOTC_RENDER_VERBOSE=1.
#pragma once

#include "common/types.h"

namespace Native {

/// One phase of the frame body. The values follow the guest's call order in 0xcefa0.
enum class FramePhase : u32 {
    Reset,        // 1
    Setup,        // 2
    FrameTargets, // 3  (0x92140 on M+0x2b80)
    Labels,       // 4
    ScenePrep,    // 5
    ViewSetup,    // 6  (0xca6d0)
    MainScene,    // 7  (0xcaed0)
    Water,        // 8
    EndViewParams,// 9
    FrameTail,    // 10 (0x118470)
    Submit,       // 11 (0xc5560)
    Release,      // 12 (0xccfd0)
    Fence,        // 13 (0x90ea0)
    Count,
};

/// The guest VA that is the canonical entry of each phase (0 = an inline phase with no single body).
VAddr FramePhaseVa(FramePhase phase);
/// The diagnostic name of a phase ("reset", "main-scene", ...).
const char* FramePhaseName(FramePhase phase);

/// Whether a phase's guest work is reproduced natively today. All false: each phase is either
/// still a chained guest call or has an emission gate that is closed (see native-render-gaps.md).
bool FramePhaseNative(FramePhase phase);

/// One step of the derived frame plan.
struct FrameStep {
    FramePhase phase{FramePhase::Reset};
    VAddr va{0};      ///< guest function VA the step calls (0 for an inline phase)
    u64 context{0};   ///< the step's main argument (R, M, ctx, F, G depending on the phase)
    u64 arg1{0};      ///< a second argument where the call has one (view config, target address)
    bool enabled{true}; ///< a branch condition of the guest body selected this step
};

/// The derived structure of one frame body call (0xcefa0's own path, not the thread/job wrap).
struct FramePlan {
    static constexpr u32 kMaxSteps = 32;

    u64 renderer{0};     ///< R (0xcefa0's param_1)
    u64 frame{0};        ///< F = *(R+0x40)
    u64 target_manager{0}; ///< M = *(F+0xd98)
    u64 ctx{0};          ///< the job context (0xcefa0's local_260) when it could be read
    u64 water_ctx{0};    ///< *(F+0xdc8) (water subsystem, inside the water step)
    u64 device_ptr{0};   ///< *(R+0x50) (the device / mesh-tessellator sub-renderer)
    u64 view_cfg{0};     ///< F+0xb8 (2nd arg of MainCamera / 0xca6d0)

    u32 view_count{0};   ///< the [F+0x1a8, F+0x1b0) per-view job list length
    bool water_step{false};  ///< the `lVar3 != 0` water branch
    bool debug_step{false};  ///< the config +0x4d4 branch (0xc4de0)
    bool particle_ctx{false}; ///< *(F+0xdc0) != 0 (the 0x109970 particle step)
    bool mesh_tessellator{false}; ///< R+0xc8 != 0 (the 0x18a000/0x18a170 MeshTessellator branch)

    FrameStep steps[kMaxSteps]{};
    u32 step_count{0};

    /// Why the plan cannot be issued natively (nullptr = every step native). `DeriveFrame` always
    /// fills this; it is the precise fallback condition RecordFrame reports.
    const char* gate{nullptr};
    /// The first step (call order) whose guest work is not native, or kMaxSteps when all are.
    u32 first_blocked_step{kMaxSteps};
    /// The config projection-override height (F+0x280+0x42c; < 0 selects the main-depth height).
    s32 view_height{0};
    bool derived{false};
};

/// Walks the frame body structure read-only and fills `plan`. Returns the gate reason (nullptr when
/// the structure derives completely and every step is native), or a non-null reason otherwise.
/// `R` is 0xcefa0's param_1. Never writes guest memory.
const char* DeriveFrame(u64 R, FramePlan& plan);

/// Derives and, when the plan is fully native, records the frame; otherwise returns false. Its
/// false path is the caller's fallback to the emulated frame. Traces under SOTC_RENDER_VERBOSE=1
/// with a shared, budgeted trace.
bool RecordFrameNative(u64 R);

/// Budget helper shared with native_frame_callbacks.cpp (one trace budget per process).
bool FrameTraceAllowed();
bool FrameVerbose();

/// Derivation for the 0x90ea0 per-frame reset/fence entry. param_1 = the video-out owner
/// (0xcefa0:483 passes the image-anchored DAT_02ed33f0), param_2 = the job context local_260 (its
/// +0x18 is [3] = M, not a "render backend" — see gap N2-27). Traced by native_frame_callbacks.cpp.
struct FencePlan {
    u64 owner{0};       ///< 0x90ea0's param_1
    u64 backend{0};     ///< 0x90ea0's param_2 (the job context)
    s32 parity{0};      ///< owner+0x7b0 after the toggle (0/1)
    u64 frame_slot{0};  ///< the slot at owner+0x20 + parity*0x1d0 (stored to owner+0x18)
    u64 fence_ptr{0};   ///< *(owner + 0x770 + parity*8)
    u64 fence_next_ptr{0}; ///< *(owner + 0x780 + parity*8)
    u64 lock{0};        ///< owner+0x7c0
    u64 flush_queue{0}; ///< *(owner+0x7d0): 0x8ef20's first argument (deferred-memory flush)
    u32 last_us{0};     ///< owner+0x4 after the wait
    u32 counter{0};     ///< owner+0x10 after FUN_0008d730 (+1)
    bool fence_pending{false}; ///< *fence_ptr != *fence_next_ptr (the spin the guest would take)
    bool derived{false};
};
const char* DeriveFence(u64 owner, u64 backend, FencePlan& plan);

} // namespace Native
