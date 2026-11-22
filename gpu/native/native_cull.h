// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_cull: Phase 2.7 — the per-sort-group draw loop ("occluder
// pass") 0x11d3d0, reimplemented natively.
//
// Contract: docs/engine/native-render-map-cull-camera-shadow.md §1/§2 and closed gap N2-14.
// The verdict that shapes this file: there is NO live predication in this title. The enable
// emitter 0x86e770, both COND_EXEC drivers 0x85cea0/0x85d260 and all three emitters
// 0x86ea50/0x883300/0x866360 have zero static callers AND zero data references (whole-image scan,
// doc §1.6). The only live predication packet is the SET_PREDICATION *disable* (address 0) the
// loop emits unconditionally at 0x11d570 — and liverpool.cpp:1558 ignores it (logs once). So the
// native path mirrors that disable as a documented no-op; it does NOT implement occlusion
// predication machinery, VK_EXT_conditional_rendering or query gates. All culling decisions are
// made before the CP: CPU cull jobs trim the per-group record vectors, and this loop simply draws
// whatever survived. "Replicating the CPU culling decision" therefore means replicating the
// loop's iteration over the already-trimmed vector — the survivor set is read from the vector,
// not recomputed here.
//
// The loop body (proven 0011d3d0.c + raw disassembly):
//   group g selects the record vector [list+0x68+0x20*g, list+0x70+0x20*g); empty -> return 0.
//   scope push 0x16b8c0(marker_slot, kRS_Depth_*[g], ctx, 1) — conditional debug marker.
//   per 0x20-byte record:
//     rec8 = *(rec+8)                    (the draw-record object)
//     0x95580(rec8) ensure-loaded; R = 0x14d7b0(*rec8) lazy material resolve
//     material-change handling (pop/push per-material marker, bind records via 0xf8c90)
//     0x123cc0(rec8+0x10, ctx, *(u32*)(rec+0x14), rec8[5] != 0) instance/index/stencil bind
//     T = 0x95320(*(u32*)(R+0x468), ctx+0x90, DAT_02ed1f54); 0xae150(cb, T) root-table bind
//     0x1222e0(rec8, ctx) the draw: if cb+0x28 bit4 clear, optionally 0x189060 (indirect path,
//       gated on rec8+0x28 != 0) else 0x128d20(rec8+0x20, ctx) -> 0x94d00 DrawIndexOffset
//   scope pop 0x16b900; SET_PREDICATION disable 0x86e800(cb).
// Returns (end - begin) >> 5 = the record count.
//
// Native strategy: derive everything read-only, gate, then issue through the existing draw seam
// (ResolveDrawPipelineForPass + Native::Draw helpers + Scheduler::RecordDeferred). Any unmet
// precondition chains to the guest body budget-gated — the standing rule (never mutate guest
// state before all gates) holds: every write below happens after the commit point.
#pragma once

#include "common/types.h"
#include "native/native_device.h"

namespace Native {

class Cull {
public:
    explicit Cull(const Device& device) : device{device} {}

    /// Native replacement of one 0x11d3d0 call. `list` is the per-view record-vector array base
    /// (ctx[0xc], the draw table), `ctx` the job context, `group` the sort-group index (0..7, the
    /// kRS_Depth_* / kRS_GBuffer_Ambient_* names). Returns true and sets *count_out to the record
    /// count (the guest's return value, (end-begin)>>5) when the native path issued the group's
    /// draws; false means "chain to the guest body" (reason traced, never silent at the gate) and
    /// *count_out is untouched.
    bool GroupNative(u64 list_addr, u64* ctx, s32 group, u64* count_out);

    /// Number of groups the loop understands (the name-array length, doc §1.2 step 2).
    static constexpr s32 kGroupCount = 8;

private:
    const Device& device;
};

/// SOTC_NATIVE_CULL: runtime A/B for the Phase 2.7 group-loop replacement. Default on, "0" =
/// pure guest chain (same semantics as SOTC_NATIVE_CLEAR/DRAW/INDIRECT/RT).
bool NativeCullWanted();

} // namespace Native