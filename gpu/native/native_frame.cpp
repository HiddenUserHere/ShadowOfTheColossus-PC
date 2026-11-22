// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.11/2.12 frame orchestrator. See
// native_frame.h for the phase map. This file walks the frame body (0xcefa0) and the per-frame
// reset/fence (0x90ea0) read-only, in call order, from the guest's own structs, and is the body of
// Renderer::RecordFrame (native_renderer.cpp delegates here).
//
// Nothing in this file writes guest memory. The plan's gate (`FramePlan::gate`) is the precise
// fallback condition: it is non-null whenever the structure does not derive OR any step's guest
// work is not native, and RecordFrame then returns false so the emulated body serves the frame.
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_device.h"
#include "native/native_frame.h"
#include "native/native_draw.h"

namespace Native {

namespace {

// -- Guest state reads (read-only) --------------------------------------------------------------

/** The image-anchored video-out owner (0x90ea0's param_1); null while the image is unmapped. */
u64 VideoOutOwner() {
    return Guest::ImageQword(0x2ed33f0);
}

inline u64 ReadU64(VAddr addr) {
    return *reinterpret_cast<const u64*>(addr);
}
inline u32 ReadU32(VAddr addr) {
    return *reinterpret_cast<const u32*>(addr);
}
inline u8 ReadU8(VAddr addr) {
    return *reinterpret_cast<const u8*>(addr);
}
inline s32 ReadS32(VAddr addr) {
    return *reinterpret_cast<const s32*>(addr);
}

// 0xcefa0's own view: R+0x40 = F, F+0xd98 = M, the sub-renderer slots the body calls.
namespace FrameBodyOff {
constexpr u64 Device = 0x50;         // *(R+0x50): 0x1a2cd0 (setup) and 0x19fed0 (copy counts)
constexpr u64 MainView = 0xb8;       // *(R+0xb8) 0x109970 / 0x1038a0 / the particle ctx
constexpr u64 Water = 0xb0;          // *(R+0xb0) 0xe2d70 / 0xe48b0
constexpr u64 MeshTess = 0xc8;       // *(R+0xc8) 0x18a000/0x18a170 (MeshTessellator::Pre/Post),
                                     //  guarded by `*(R+0xc8) != 0` at 000cefa0.c:312
constexpr u64 WaterCtx = 0xdc8;      // *(F+0xdc8) (0xca6d0's 0x129580)
constexpr u64 ParticleCtx = 0xdc0;   // *(F+0xdc0)
constexpr u64 ViewListBegin = 0x1a8; // per-view job records (0xa8 stride)
constexpr u64 ViewListEnd = 0x1b0;
constexpr u64 CfgProjHeight = 0x42c; // the projection-override height constant
} // namespace FrameBodyOff

// 0x90ea0's owner struct (the payload behind the image-anchored 0x2ed33f0).
namespace FenceOff {
constexpr u64 Counter = 0x10;       // incremented by 0x8d730
constexpr u64 SlotParam = 0x20;     // frame slot base: owner+0x20 + parity*0x1d0 (stored at +0x18)
constexpr u64 LastUs = 0x04;        // elapsed microseconds
constexpr u64 Parity = 0x7b0;       // toggles 0/1 each call
constexpr u64 FenceBegin = 0x770;   // + parity*8: the frame-fence label the guest spins on
constexpr u64 FenceEnd = 0x780;     // + parity*8
constexpr u64 Lock = 0x7c0;         // deferred-memory lock word
constexpr u64 FlushQueue = 0x7d0;   // 0x8ef20 argument (deferred-memory flush)
constexpr u64 SlotStride = 0x1d0;
} // namespace FenceOff

// The sub-renderers the frame body dispatches by slot (R offsets, docs native-render-map-frame.md
// §0). Each is a guest-emulated object; until its module is native the step cannot be issued
// natively. Kept as a table so a phase's gate names the object it waits for.
// (R sub-renderer slots, docs native-render-map-frame.md §0: 0x48/0x50/0x58/0x68/0x78/0x80/0x90/
// 0x98/0xa8/0xb0/0xb8. The ones the frame body itself calls are named in FrameBodyOff above.)

void PushStep(FramePlan& plan, FramePhase phase, VAddr va, u64 context, u64 arg1 = 0,
              bool enabled = true) {
    if (plan.step_count >= FramePlan::kMaxSteps) {
        return;
    }
    FrameStep& s = plan.steps[plan.step_count++];
    s.phase = phase;
    s.va = va;
    s.context = context;
    s.arg1 = arg1;
    s.enabled = enabled;
}

} // namespace

// -- Public table helpers -----------------------------------------------------------------------

VAddr FramePhaseVa(FramePhase phase) {
    switch (phase) {
    case FramePhase::Reset: return 0;         // inline: 0x32420 + memcpy F+0x280
    case FramePhase::Setup: return 0x118440;  // -> 0x90860 (frame setup / view mode)
    case FramePhase::FrameTargets: return 0x92140;
    case FramePhase::Labels: return 0xcedf0;
    case FramePhase::ScenePrep: return 0x16c500;
    case FramePhase::ViewSetup: return 0xca6d0;
    case FramePhase::MainScene: return 0xcaed0;
    case FramePhase::Water: return 0xe2d70;
    case FramePhase::EndViewParams: return 0x16bc50;
    case FramePhase::FrameTail: return 0x118470;
    case FramePhase::Submit: return 0xc5560;
    case FramePhase::Release: return 0xccfd0;
    case FramePhase::Fence: return 0x90ea0;
    default: return 0;
    }
}

const char* FramePhaseName(FramePhase phase) {
    switch (phase) {
    case FramePhase::Reset: return "reset";
    case FramePhase::Setup: return "setup";
    case FramePhase::FrameTargets: return "frame-targets";
    case FramePhase::Labels: return "labels";
    case FramePhase::ScenePrep: return "scene-prep";
    case FramePhase::ViewSetup: return "view-setup";
    case FramePhase::MainScene: return "main-scene";
    case FramePhase::Water: return "water";
    case FramePhase::EndViewParams: return "end-view-params";
    case FramePhase::FrameTail: return "frame-tail";
    case FramePhase::Submit: return "submit";
    case FramePhase::Release: return "release";
    case FramePhase::Fence: return "fence";
    default: return "?";
    }
}

// No frame phase is native yet. Two families of blocker (docs/engine/native-render-gaps.md):
//   * the submit path (0xc5560/0xd07c0, gap N2-25) is where the guest hands every command buffer
//     of the frame to the emulated scheduler — it cannot be replaced while any pass is emulated;
//   * every drawing phase runs a chained body (0x92140 -> RtEmissionReady closed, N2-2; 0xcaed0
//     2.9; 0xca6d0 2.10; 0x118470 HUD, N2-23), so a native frame would mix two recording models.
// The function is a single switch so a phase flips here the day its module lands.
bool FramePhaseNative(FramePhase phase) {
    (void)phase;
    return false;
}

// -- The frame derivation ------------------------------------------------------------------------

const char* DeriveFrame(u64 R, FramePlan& plan) {
    plan = {};
    plan.renderer = R;
    if (R == 0) {
        return "renderer R is null";
    }
    const u64 F = ReadU64(R + Guest::RendererOff::FrameObject);
    plan.frame = F;
    if (F == 0) {
        return "frame object F is null (RenderThread_Kick has not published it)";
    }
    const u64 M = ReadU64(F + Guest::FrameOff::TargetManager);
    plan.target_manager = M;
    if (M == 0) {
        return "target manager M is null (F+0xd98)";
    }

    // Root fields 0xcefa0 reads (000c/000cefa0.c lines 147, 148, 154, 220-222, 279, 299, 344, 356,
    // 403, 475).
    plan.device_ptr = ReadU64(R + FrameBodyOff::Device);
    plan.water_ctx = ReadU64(F + FrameBodyOff::WaterCtx);
    plan.view_cfg = F + Guest::FrameOff::ViewConfig;
    plan.particle_ctx = ReadU64(F + FrameBodyOff::ParticleCtx) != 0;
    plan.mesh_tessellator = ReadU64(R + FrameBodyOff::MeshTess) != 0;
    plan.water_step = plan.water_ctx != 0;

    const u64 view_begin = ReadU64(F + FrameBodyOff::ViewListBegin);
    const u64 view_end = ReadU64(F + FrameBodyOff::ViewListEnd);
    plan.view_count = view_end >= view_begin ? static_cast<u32>((view_end - view_begin) / 0xa8) : 0;

    // The config block (F+0x280, 0xac8 B) selects several steps; only fields are read, not copied.
    const u64 cfg = F + Guest::FrameOff::ConfigBlock;
    const s32 proj_height = ReadS32(cfg + FrameBodyOff::CfgProjHeight);
    plan.debug_step = ReadU8(cfg + 0x4d4) == 0;

    // 1. reset: FUN_00032420(R+0x38) timestamp + memcpy(local_d28, F+0x280, 0xac8) + the
    //    projection-override branch clearing local_980 (line 155-157).
    PushStep(plan, FramePhase::Reset, 0, R + 0x38, cfg);

    // 2. setup: FUN_001a2cd0(*(R+0x50), cfg) then FUN_00118440(G) -> 0x90860 (frame setup: view
    //    mode M+0x3b70 from S+0x4b8/+0x4c0, the DCB/CCB context builder 0x90470).
    PushStep(plan, FramePhase::Setup, 0x1a2cd0, plan.device_ptr, cfg);
    PushStep(plan, FramePhase::Setup, 0x118440, Guest::RendererPtr(), cfg);

    // 3. per-frame RT block M+0x2b80 .. M+0x2bc8 (10 qwords) -> the Phase 2.5 binder 0x92140.
    PushStep(plan, FramePhase::FrameTargets, 0x92140, M, M + 0x2b80);

    // 4. copy-count labels (config +0x1e4/+0x1e8/+0x1ec) via 0xcedf0, then 0x19fed0 (video-out).
    PushStep(plan, FramePhase::Labels, 0xcedf0, 1, ReadU32(cfg + 0x1e4));
    PushStep(plan, FramePhase::Labels, 0xcedf0, 0xc, ReadU32(cfg + 0x1e8));
    PushStep(plan, FramePhase::Labels, 0xcedf0, 0xd, ReadU32(cfg + 0x1ec));
    PushStep(plan, FramePhase::Labels, 0x19fed0, M);

    // 5. scene prep: 0x16c500(DAT_02ee3e08), 0x187510(*(R+0x98)), 0x8b7f0(*(R+0xd0)), 0x16bb70
    //    scope, per-frame counter G+0x220358 = *(R+0x30).
    PushStep(plan, FramePhase::ScenePrep, 0x16c500, M);
    PushStep(plan, FramePhase::ScenePrep, 0x187510, ReadU64(R + 0x98));
    PushStep(plan, FramePhase::ScenePrep, 0x8b7f0, ReadU64(R + 0xd0));
    PushStep(plan, FramePhase::ScenePrep, 0x16bb70, R + 0x30);
    if (plan.mesh_tessellator) {
        // 0x18a000 -> "MeshTessellator::PreRenderJob" (0xc12a0) -> 0x18a170.
        PushStep(plan, FramePhase::ScenePrep, 0x18a000, ReadU64(R + FrameBodyOff::MeshTess));
    }

    // 6. view setup: the water/view runner 0x129580 (when the water ctx exists), then the per-view
    //    sorted tables 0xca6d0 (Phase 2.10) and 0xd0040.
    if (plan.water_step) {
        PushStep(plan, FramePhase::ViewSetup, 0x129580, plan.water_ctx, plan.view_cfg);
    }
    PushStep(plan, FramePhase::ViewSetup, 0xca6d0, R, plan.view_cfg);
    PushStep(plan, FramePhase::ViewSetup, 0xd0040, R);

    // 7. main scene: "main cam" scope -> 0xcaed0 (Phase 2.9 MainCamera) with arg2 = F+0xb8. The
    //    projection-override height that drives SetViewParams comes from cfg+0x42c or, when
    //    negative, the main-depth 0x96f60(M+0x1fb8) (line 279-283); recorded in plan.view_height.
    PushStep(plan, FramePhase::MainScene, 0xcaed0, R, plan.view_cfg);
    plan.view_height = proj_height;

    // 8. water: 0xe2d70(*(R+0xb0), ctx, *(R+0xb8)); the "WaterRendering::UpdateJob" 0xc12a0
    //    (config +0x246, line 344-354) is guarded by the water ctx AND (cfg+0x174 != 0 ||
    //    cfg+0x163 != 0) (line 355-357) — both flags are recorded, and 0xd04e0 always runs.
    if (plan.water_step) {
        PushStep(plan, FramePhase::Water, 0xe2d70, ReadU64(R + FrameBodyOff::Water));
        const bool water_job = ReadU32(cfg + 0x174) != 0 || ReadU8(cfg + 0x163) != 0;
        PushStep(plan, FramePhase::Water, 0xc12a0, R, ReadU8(cfg + 0x246), water_job);
    }
    PushStep(plan, FramePhase::Water, 0xd04e0, R);

    // 9. end-of-frame view params: M+0x18..0xa0 from auStack_230 (the camera block), 0x16bc50 and
    //    0x16c520 finalize, the M+0x24f0 block -> 0x92140, a 0xbe1f0 upload.
    PushStep(plan, FramePhase::EndViewParams, 0x16bc50, M);
    PushStep(plan, FramePhase::EndViewParams, 0x16c520, M);
    PushStep(plan, FramePhase::EndViewParams, 0x92140, M, M + 0x24f0);
    PushStep(plan, FramePhase::EndViewParams, 0xbe1f0, M);

    // 10. frame tail: "EndFramesWaterAndParticle" 0xda110(*(R+0x80)), 0xe48b0(*(R+0xb0)),
    //     optional 0x109970(*(R+0xb8)), 0x1038a0(*(R+0xb8)), then 0x118470 (HUD + HUD resolve
    //     0x93960 x2 + the 0x91f50 flip wait).
    PushStep(plan, FramePhase::FrameTail, 0xda110, ReadU64(R + 0x80));
    PushStep(plan, FramePhase::FrameTail, 0xe48b0, ReadU64(R + FrameBodyOff::Water));
    if (plan.particle_ctx) {
        PushStep(plan, FramePhase::FrameTail, 0x109970, ReadU64(R + FrameBodyOff::MainView));
    }
    PushStep(plan, FramePhase::FrameTail, 0x1038a0, ReadU64(R + FrameBodyOff::MainView));
    PushStep(plan, FramePhase::FrameTail, 0x118470, Guest::RendererPtr(), 1);

    // 11. submit: 0xc5560 packages every emulated command buffer of the frame and 0xd07c0
    //     kicks it (0x91f80 / 0x91f90 sceGnmSubmit*). This is the hand-off RecordFrame must own.
    PushStep(plan, FramePhase::Submit, 0xc5560, R);
    PushStep(plan, FramePhase::Submit, 0xd07c0, M);

    // 12. release: 0xccfd0 waits and frees the frame's job counters (RC+0xe98..0xe80).
    PushStep(plan, FramePhase::Release, 0xccfd0, M);

    // 13. fence: 0x90ea0(*(0x2ed33f0), ctx) — the parity frame-slot spin + deferred-memory flush.
    PushStep(plan, FramePhase::Fence, 0x90ea0, VideoOutOwner(), M);

    // A second frame-pacing fence the main loop takes (0x25ae80 -> 0xbd0a0 -> 0x3b4a0/0x3b450) and
    // the RESETTLEDEFERREDWAITS 0x3b450 at the end of 0xcefa0 (lines 484-485) are CPU-only: they
    // gate the render thread's own producer/consumer locks, not GPU work, so they stay in the
    // guest. They are recorded here as steps of the Fence phase so the trace is complete.
    PushStep(plan, FramePhase::Fence, 0x3b450, R + 0x10);
    PushStep(plan, FramePhase::Fence, 0x3b450, R + 0x20);

    plan.derived = true;

    // Gate: the first non-native step decides the fallback, in call order, so the trace names the
    // exact point the emulated body must resume at.
    for (u32 i = 0; i < plan.step_count; ++i) {
        const FrameStep& s = plan.steps[i];
        if (!s.enabled) {
            continue;
        }
        if (!FramePhaseNative(s.phase)) {
            plan.first_blocked_step = i;
            // A stable literal per phase (no static buffer): the caller formats the step index and
            // VA from the plan, so concurrent render threads never race over a reason string.
            return "a frame step is not native";
        }
    }
    return nullptr;
}

// -- The per-frame reset/fence (0x90ea0) ---------------------------------------------------------

const char* DeriveFence(u64 owner, u64 backend, FencePlan& plan) {
    plan = {};
    plan.owner = owner;
    plan.backend = backend;
    if (owner == 0) {
        return "video-out owner (0x2ed33f0) is null";
    }
    // 00090ea0.c lines 18-21: the new parity is the low bit of parity+1 (verified for the reachable
    // 0/1 domain). Line 21 then stores `owner+0x18 = owner+0x20 + parity*0x1d0` (the frame slot);
    // the slot address below is that value, not a field at owner+0x18.
    const s32 parity = ReadS32(owner + FenceOff::Parity);
    plan.parity = (parity + 1) & 1;
    plan.frame_slot = owner + FenceOff::SlotParam + static_cast<u64>(plan.parity) * FenceOff::SlotStride;
    const u64 pb = static_cast<u64>(plan.parity) * 8;
    const u64 fence_ptr = ReadU64(owner + FenceOff::FenceBegin + pb);
    const u64 fence_next_ptr = ReadU64(owner + FenceOff::FenceEnd + pb);
    plan.fence_ptr = fence_ptr;
    plan.fence_next_ptr = fence_next_ptr;
    plan.lock = owner + FenceOff::Lock;
    plan.flush_queue = ReadU64(owner + FenceOff::FlushQueue);
    plan.last_us = ReadU32(owner + FenceOff::LastUs);
    plan.counter = ReadU32(owner + FenceOff::Counter) + 1;
    // Lines 27-31: spin while *fence_ptr != *fence_next_ptr. Read-only observation of the pending
    // state (the guest spin itself is the native frame-pacing fence, map-pass §5.1).
    plan.fence_pending = fence_ptr != 0 && fence_next_ptr != 0 && ReadU64(fence_ptr) != ReadU64(fence_next_ptr);
    plan.derived = true;
    return nullptr;
}

// -- Tracing and the RecordFrame body ------------------------------------------------------------

namespace {

std::atomic<u64> g_trace_budget{32};

} // namespace

bool FrameVerbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

bool FrameTraceAllowed() {
    return g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

bool RecordFrameNative(u64 R) {
    FramePlan plan;
    const char* gate = DeriveFrame(R, plan);

    // The native frame is ready only when the plan derives and every step is native, and only on
    // the stage that enables the native renderer at all. Until then the guest body serves.
    //
    // The cutover ladder (SOTC_NATIVE_EMIT=1) flips this: the frame is the guest's own body —
    // every phase it calls is hooked and hands its work to the native backend — so returning
    // false would let the guest body run a SECOND time over the same hooks and double every
    // draw. The frame is therefore "owned" whenever the native renderer is on and the plan
    // derives; the hooks remain the per-phase gate (a phase that cannot go native chains to its
    // own guest body, exactly once).
    const bool emit_ladder = NativeEmitWanted();
    const bool record_ready = plan.derived && Device::Enabled() &&
                              (emit_ladder || gate == nullptr);

    if (FrameVerbose() && FrameTraceAllowed()) {
        if (plan.first_blocked_step < plan.step_count) {
            const FrameStep& b = plan.steps[plan.first_blocked_step];
            std::fprintf(stderr,
                         "SOTC RENDER native: frame R=%#llx F=%#llx M=%#llx views=%u steps=%u "
                         "water=%u debug=%u particles=%u mesh=%u -> guest chain (blocked at step "
                         "%u: %s %#llx)\n",
                         static_cast<unsigned long long>(plan.renderer),
                         static_cast<unsigned long long>(plan.frame),
                         static_cast<unsigned long long>(plan.target_manager), plan.view_count,
                         plan.step_count, plan.water_step ? 1u : 0u, plan.debug_step ? 1u : 0u,
                         plan.particle_ctx ? 1u : 0u, plan.mesh_tessellator ? 1u : 0u,
                         plan.first_blocked_step, FramePhaseName(b.phase),
                         static_cast<unsigned long long>(b.va));
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: frame R=%#llx F=%#llx M=%#llx views=%u steps=%u "
                         "water=%u debug=%u particles=%u mesh=%u -> %s (%s)\n",
                         static_cast<unsigned long long>(plan.renderer),
                         static_cast<unsigned long long>(plan.frame),
                         static_cast<unsigned long long>(plan.target_manager), plan.view_count,
                         plan.step_count, plan.water_step ? 1u : 0u, plan.debug_step ? 1u : 0u,
                         plan.particle_ctx ? 1u : 0u, plan.mesh_tessellator ? 1u : 0u,
                         record_ready ? "native" : "guest chain",
                         gate != nullptr ? gate : (plan.derived ? "all steps native" : "not derived"));
        }
        // The ordered phase list (the derived structure), so it is inspectable without a debugger.
        for (u32 i = 0; i < plan.step_count; ++i) {
            const FrameStep& s = plan.steps[i];
            std::fprintf(stderr, "SOTC RENDER native:   [%u] %s %#llx ctx=%#llx arg=%#llx\n", i,
                         FramePhaseName(s.phase), static_cast<unsigned long long>(s.va),
                         static_cast<unsigned long long>(s.context),
                         static_cast<unsigned long long>(s.arg1));
        }
    }

    if (!record_ready) {
        return false;
    }
    if (emit_ladder) {
        // The guest frame body is the native frame in this mode: it runs once, through the hooks.
        return false;
    }
    // Commit point (unreachable until every frame step is native): begin the native frame — reset
    // the per-frame state, record every native phase in call order on the Scheduler, then own the
    // submit (vkQueueSubmit + present) in place of 0xc5560/0xd07c0.
    return false;
}

} // namespace Native
