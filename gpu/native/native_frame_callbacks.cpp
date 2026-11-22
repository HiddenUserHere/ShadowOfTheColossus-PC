// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.11 (native-frame): the entry
// callbacks of the frame body (0xcefa0) and the per-frame reset/fence (0x90ea0). Derivations and
// the RecordFrame body live in native_frame.{h,cpp}; this file is the chain logic only.
//
// Emission gate: both callbacks derive read-only (native_frame.cpp), trace under
// SOTC_RENDER_VERBOSE=1 with a budget, and chain to the guest body. A native frame body would have
// to own the submit (0xc5560/0xd07c0) and every chained pass in between, so it is not ready and
// the chain always runs — the requirement is that the derived structure is real and visible.
#include <cstdio>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_frame.h"
#include "sotc/hooks.h"

namespace Native {

namespace {
void* Original(u64 va) {
    return Core::SotcHooks::OriginalEntry(va);
}
} // namespace

// void f(long R) — rdi, the guest's SysV ABI (000cefa0.c:5; caller FUN_000cef90 = the job entry).
typedef void (*GuestFrameFn)(u64) __attribute__((sysv_abi));

// void f(long owner, long backend) — rdi/rsi (00090ea0.c:3; the sole caller is 0xcefa0:483 with
// owner = DAT_02ed33f0 and backend = the job context).
typedef void (*GuestFenceFn)(u64, u64) __attribute__((sysv_abi));

} // namespace Native

extern "C" __attribute__((sysv_abi)) void NativeFrame_cefa0(void* renderer) {
    using namespace Native;
    const u64 r = reinterpret_cast<u64>(renderer);
    // RecordFrameNative derives the frame body and returns false while any step is emulated; its
    // trace names the first blocked step in call order (the guest's resume point).
    (void)RecordFrameNative(r);
    reinterpret_cast<GuestFrameFn>(Original(0xcefa0))(r);
}

extern "C" __attribute__((sysv_abi)) void NativeFrame_90ea0(void* owner, void* backend) {
    using namespace Native;
    const u64 owner_addr = reinterpret_cast<u64>(owner);
    const u64 backend_addr = reinterpret_cast<u64>(backend);
    FencePlan plan;
    const char* gate = DeriveFence(owner_addr, backend_addr, plan);
    if (FrameVerbose() && FrameTraceAllowed()) {
        if (gate == nullptr) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x90ea0 owner=%#llx backend=%#llx parity=%d "
                         "slot=%#llx fence=%#llx next=%#llx pending=%u queue=%#llx count=%u "
                         "guest chain (frame-pacing fence)\n",
                         static_cast<unsigned long long>(owner_addr),
                         static_cast<unsigned long long>(backend_addr), plan.parity,
                         static_cast<unsigned long long>(plan.frame_slot),
                         static_cast<unsigned long long>(plan.fence_ptr),
                         static_cast<unsigned long long>(plan.fence_next_ptr),
                         plan.fence_pending ? 1u : 0u,
                         static_cast<unsigned long long>(plan.flush_queue), plan.counter);
        } else {
            std::fprintf(stderr, "SOTC RENDER native: 0x90ea0 owner=%#llx guest chain (%s)\n",
                         static_cast<unsigned long long>(owner_addr), gate);
        }
    }
    reinterpret_cast<GuestFenceFn>(Original(0x90ea0))(owner_addr, backend_addr);
}
