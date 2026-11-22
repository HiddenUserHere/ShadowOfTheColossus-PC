// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.8 (native-shadow): the entry
// callbacks of RenderShadows (0xcc9b0), the G-buffer runner (0xc59d0) and the depth pass object
// (0x93ef0). The derivations and the commit paths live in native_shadow.{h,cpp}; this file is only
// the chain logic around them (docs/engine/native-render-map-cull-camera-shadow.md §2/§3).
//
// Emission gate: each callback runs its module's read-only derivation and commits through the
// shadow module only when the derivation reports no gate reason. Until then it chains to the
// guest body with a budget-gated trace. 0x93ef0 is also hooked by the Phase 2.6 pass stage
// (native_pass_callbacks.cpp): enabling several stages in SOTC_RENDER is harmless — every
// callback chains to the same original body, and the installer keeps one detour per entry.
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_renderer.h"
#include "native/native_shadow.h"
#include "sotc/hooks.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

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

// void f(long R, long shadow_cfg, long unused, long* ctx) — rdi/rsi/rdx/rcx, the guest's SysV ABI
// (000cc9b0.c:1-5; the sole caller is MainCamera 0xcaed0:195).
typedef void (*GuestShadowsFn)(u64, u64, u64, u64*) __attribute__((sysv_abi));

// void f(long R, long view_cfg, long* ctx, long draw_table) — rdi/rsi/rdx/rcx (000c59d0.c:3, the
// sole caller is MainCamera 0xcaed0:272 with draw_table = R+0xd8).
typedef void (*GuestGBufferFn)(u64, u64, u64*, u64) __attribute__((sysv_abi));

} // namespace Native

extern "C" __attribute__((sysv_abi)) void NativeShadow_cc9b0(void* renderer, void* shadow_cfg,
                                                             void* unused, void* ctx) {
    using namespace Native;
    const u64 r = reinterpret_cast<u64>(renderer);
    const u64 cfg = reinterpret_cast<u64>(shadow_cfg);
    const u64 ctx_addr = reinterpret_cast<u64>(ctx);
    if (Renderer* native = Current(); native != nullptr) {
        ShadowsPlan plan;
        const char* gate = Shadow::DeriveShadows(r, cfg, reinterpret_cast<u64>(unused), ctx_addr, plan);
        if (gate == nullptr && native->Shadows().CommitShadows(native->Scheduler(), plan)) {
            return;
        }
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xcc9b0 R=%#llx cfg=%#llx ctx=%#llx cascades=%u "
                         "guest chain (%s)\n",
                         static_cast<unsigned long long>(r),
                         static_cast<unsigned long long>(cfg),
                         static_cast<unsigned long long>(ctx_addr),
                         plan.cascades_enabled ? 1u : 0u, gate != nullptr ? gate : "commit refused");
        }
    }
    reinterpret_cast<Native::GuestShadowsFn>(Native::Original(0xcc9b0))(
        r, cfg, reinterpret_cast<u64>(unused), static_cast<u64*>(ctx));
}

extern "C" __attribute__((sysv_abi)) void NativeShadow_c59d0(void* renderer, void* view_cfg,
                                                             void* ctx, void* draw_table) {
    using namespace Native;
    const u64 r = reinterpret_cast<u64>(renderer);
    const u64 view = reinterpret_cast<u64>(view_cfg);
    const u64 ctx_addr = reinterpret_cast<u64>(ctx);
    const u64 table = reinterpret_cast<u64>(draw_table);
    if (Renderer* native = Current(); native != nullptr) {
        GBufferPlan plan;
        const char* gate = Shadow::DeriveGBuffer(r, view, ctx_addr, table, plan);
        if (gate == nullptr && native->Shadows().CommitGBuffer(native->Scheduler(), plan)) {
            return;
        }
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xc59d0 R=%#llx view_cfg=%#llx ctx=%#llx "
                         "guest chain (%s)\n",
                         static_cast<unsigned long long>(r),
                         static_cast<unsigned long long>(view),
                         static_cast<unsigned long long>(ctx_addr),
                         gate != nullptr ? gate : "commit refused");
        }
    }
    reinterpret_cast<GuestGBufferFn>(Native::Original(0xc59d0))(
        r, view, static_cast<u64*>(ctx), table);
}

// void f(long* pass, long bound_desc, long target_desc, byte flags) — rdi/rsi/rdx/ecx
// (00093ef0.c); the depth pass object reached directly (2.6's native-pass stage) or through the
// 0x93d70 touch wrapper (whose hook is below).
typedef void (*GuestDepthFn)(u64*, u64, u64, u8) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void NativeShadow_93ef0(void* pass, unsigned long long bound,
                                                             unsigned long long target,
                                                             unsigned char flags) {
    using namespace Native;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    if (Renderer* native = Current(); native != nullptr) {
        DepthClearPlan plan;
        const char* gate =
            Shadow::DeriveDepthClear(pass_addr, target, target, static_cast<u32>(flags), plan);
        if (gate == nullptr && native->Shadows().CommitDepthClear(native->Scheduler(), plan)) {
            return;
        }
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x93ef0 pass=%#llx target=%#llx flags=%u guest "
                         "chain (%s)\n",
                         static_cast<unsigned long long>(pass_addr),
                         static_cast<unsigned long long>(target), static_cast<unsigned>(flags),
                         gate != nullptr ? gate : "commit refused");
        }
    }
    reinterpret_cast<GuestDepthFn>(Native::Original(0x93ef0))(
        static_cast<u64*>(pass), bound, target, flags);
}

// void f(long* pass, long view_desc) — the 0x93d70 depth-cache touch wrapper: either it runs the
// full 0x93ef0 depth clear (0x93ef0(pass, view, view, 0), the self-clear branch) or only the
// sub-rect / surface ACQUIRE_MEM packets (native: a barrier, or nothing — the native backend owns
// its own barrier placement, map §2.4). Both guest paths reach engine/GNM packets this stage does
// not reproduce yet, so the wrapper chains.
typedef void (*GuestTouchFn)(u64*, u64) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void NativeShadow_93d70(void* pass,
                                                            unsigned long long view_desc) {
    using namespace Native;
    if (Verbose() && TraceAllowed()) {
        bool runs_clear = false;
        bool acquire_only = false;
        Shadow::TouchBranches(static_cast<u64>(view_desc), &runs_clear, &acquire_only);
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x93d70 pass=%#llx view=%#llx clear=%u acq=%u guest "
                     "chain (touch packets)\n",
                     static_cast<unsigned long long>(reinterpret_cast<u64>(pass)),
                     static_cast<unsigned long long>(view_desc), runs_clear ? 1u : 0u,
                     acquire_only ? 1u : 0u);
    }
    reinterpret_cast<GuestTouchFn>(Native::Original(0x93d70))(static_cast<u64*>(pass), view_desc);
}
