// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.9 (native-camera): the entry
// callback of RenderView::MainCamera (0xcaed0). The derivation lives in native_camera.{h,cpp}; this
// file is only the chain logic around it (docs/engine/native-render-map-cull-camera-shadow.md §4).
//
// Gate: MainCamera is the pass-graph orchestrator and the bodies it drives (RenderShadows, the
// G-buffer runner and the forward/particle passes) stay emulated until Phases 2.11/2.12. The
// derivation is therefore trace-only: it reads the frame structure, traces it under
// SOTC_RENDER_VERBOSE=1 and always chains to the guest body. No guest state is written here.
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_camera.h"
#include "native/native_renderer.h"
#include "sotc/hooks.h"

namespace Native {

namespace {

void* Original(u64 va) {
    return Core::SotcHooks::OriginalEntry(va);
}

bool Verbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

std::atomic<u64> g_trace_budget{64};

bool TraceAllowed() {
    return g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

} // namespace

// void f(long R, long view_cfg, long* ctx) -- rdi/rsi/rdx, the guest's SysV ABI (000caed0.c:5;
// callers FUN_000cefa0:339 passes (R, F+0xb8, ctx) and FUN_000ccdf0 passes (R, face_cfg, ctx)).
typedef void (*GuestCameraFn)(u64, u64, u64) __attribute__((sysv_abi));

} // namespace Native

extern "C" __attribute__((sysv_abi)) void NativeCamera_caed0(void* renderer, void* view_cfg,
                                                             void* ctx) {
    using namespace Native;
    const u64 r = reinterpret_cast<u64>(renderer);
    const u64 view = reinterpret_cast<u64>(view_cfg);
    const u64 ctx_addr = reinterpret_cast<u64>(ctx);
    if (Current() != nullptr) {
        CameraPlan plan;
        const char* gate = Camera::DeriveCamera(r, view, ctx_addr, plan);
        if (Verbose() && TraceAllowed()) {
            std::fprintf(
                stderr,
                "SOTC RENDER native: 0xcaed0 R=%#llx view_cfg=%#llx ctx=%#llx RC=%#llx V=%#llx "
                "slots=%#llx setup_wait=%#llx shadows=%u spots=%u setA=%ux%u wait=%#llx "
                "gbuf=%#llx particle=%#llx view=%u jobs=%#llx..%#llx guest chain (%s)\n",
                static_cast<unsigned long long>(r), static_cast<unsigned long long>(view),
                static_cast<unsigned long long>(ctx_addr),
                static_cast<unsigned long long>(plan.render_context),
                static_cast<unsigned long long>(plan.target_manager),
                static_cast<unsigned long long>(plan.label_slots),
                static_cast<unsigned long long>(plan.labels.frame_setup),
                plan.shadows_enabled ? 1u : 0u, plan.spot_shadows_enabled ? 1u : 0u,
                plan.set_a.width, plan.set_a.height,
                static_cast<unsigned long long>(plan.gbuffer_wait),
                static_cast<unsigned long long>(plan.gbuffer_call ? plan.draw_table : 0),
                static_cast<unsigned long long>(plan.particle_view.address),
                plan.particle_view_id, static_cast<unsigned long long>(plan.job_vector_begin),
                static_cast<unsigned long long>(plan.job_vector_end),
                gate != nullptr ? gate : "commit path absent");
        }
    }
    reinterpret_cast<Native::GuestCameraFn>(Native::Original(0xcaed0))(r, view, ctx_addr);
}
