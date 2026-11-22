// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_pass_callbacks: the entry shims for the three pass
// functions (0x93960 / 0x94600 / 0x93ef0, docs/engine/native-render-map-pass.md §2). Each derives
// the phase read-only (descriptor gates, dimensions, colour/depth blocks, the label/EOP fence),
// traces it under SOTC_RENDER_VERBOSE=1, and runs the native body when the derivation holds.
// Any unmet piece chains to the guest body (budget-gated trace), which stays the specification.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_draw.h"
#include "native/native_pass.h"
#include "native/native_renderer.h"
#include "native/native_stack.h"
#include "sotc/hooks.h"

namespace Native {

/// 0x93690 in C++ (native_hooks.cpp `RunNativeClear`).
extern bool RunNativeClearForPass(u64* pass);
/// The same body with the depth block the calling phase stages (0x94600's `desc+0x164`, before
/// the native body stages it the way the guest body does, 00094600.c:63-64).
extern bool RunNativeClearWithDepth(u64* pass, u64 depth_block);
/// native_hooks.cpp: the colour rect's pipeline resolves (no guest write).
extern bool NativeColourRectResolvable(u64* pass);
/// The shared, bounded trace budget of native_hooks.cpp (SOTC_NATIVE_TRACE_BUDGET overrides the
/// 64-line default): the pass hooks run after the frame hooks in a run, so one budget for all.
extern bool NativeTraceAllowed() noexcept;

namespace {

void* Original(u64 va) {
    return Core::SotcHooks::OriginalEntry(va);
}

// void f(long* pass, long desc) — rdi/rsi, the guest's SysV ABI (00093960.c, 00094600.c).
typedef void (*GuestPassFn)(u64*, u64) __attribute__((sysv_abi));

// void f(long* pass) — rdi, the draw-object shape (00093690.c).
typedef void (*GuestRectFn)(u64*) __attribute__((sysv_abi));

// void f(long* pass, long bound_desc, long target_desc, byte flags) — rdi/rsi/rdx/ecx, the
// guest's SysV ABI (00093ef0.c): the depth phase. `target_desc` supplies the dimensions; the
// stack-merged depth block built from `bound_desc`/`target_desc` is what 0x86a290 receives.
typedef void (*GuestDepthPassFn)(u64*, u64, u64, u8) __attribute__((sysv_abi));

/// Runs the native fullscreen rect for the pass: true when the child was native (the pass body then
/// continues).
bool PassFullscreenRect(u64 pass_addr) {
    return RunNativeClearForPass(reinterpret_cast<u64*>(pass_addr));
}

bool PassFullscreenRect(u64 pass_addr, u64 depth_block) {
    return RunNativeClearWithDepth(reinterpret_cast<u64*>(pass_addr), depth_block);
}

/// Budget-gated trace switch (SOTC_RENDER_VERBOSE=1).
bool Verbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

/// The phase's 0x93690 child when the native body cannot serve it: the guest body runs, exactly as
/// it would if the phase had chained, but ONLY the draw — the phase body has already made its own
/// writes up to this point, so re-running the whole phase would repeat them and emit the draw
/// twice (run 2026-11-21: the chained phase's second 0x93690 landed on a corrupt pass slot).
/// `depth_staged` says the phase already cached a depth BLOCK at pass+0xa8 (0x94600's stack
/// copy): then the depth dirty bit is set first, as 0x94600 does at the end of its own draw
/// sequence (00094600.c:63/68), so the guest body re-binds the block instead of trusting the
/// stale cache. After the child, the phase continues into its own fence.
void RunFullscreenRectGuest(u64 pass_addr, bool depth_block_staged) {
    constexpr u64 kVa = 0x93690;
    if (depth_block_staged) {
        *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) =
            static_cast<u8>(*reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) | 1);
    }
    void* body = Original(kVa);
    if (Verbose() && NativeTraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: 0x93690 child pass=%#llx body=%p enter\n",
                     static_cast<unsigned long long>(pass_addr), body);
    }
    if (body == nullptr) {
        return;
    }
    reinterpret_cast<GuestRectFn>(body)(reinterpret_cast<u64*>(pass_addr));
    if (Verbose() && NativeTraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: 0x93690 child pass=%#llx returned\n",
                     static_cast<unsigned long long>(pass_addr));
    }
}


enum class Phase {
    ColourA, ///< 0x93960
    ClearB,  ///< 0x94600
    Depth,   ///< 0x93ef0
};

inline u32 Read32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}

/// Descriptor field offsets (native-render-map-pass.md §1.4).
namespace PassDescOff {
constexpr u64 Type = Guest::PassDescOff::Type;
constexpr u64 FlagsA = Guest::PassDescOff::FlagsA;
constexpr u64 FlagsB = Guest::PassDescOff::FlagsB;
constexpr u64 DimsA = Guest::PassDescOff::ViewportDims;
constexpr u64 DimsB = Guest::PassDescOff::ViewportDimsB;
constexpr u64 DepthBlock = Guest::PassDescOff::DepthTarget;
constexpr u64 ColorBlock = Guest::PassDescOff::ColorTargets;
} // namespace PassDescOff

/// The viewport/scissor state the pass's own 0x85de40 call leaves: viewport (x=0, y=0, w, h), depth
/// range [z_lo, z_hi] in the two floats at +0xc0/+0xc4 (00093960.c:55-66 expects 0.5/0.5,
/// 0x91fc0's guard expects 1.0/0.0) and the window scissor (0,0,w,h). These are the pass-context
/// caches the caller compares against.
///
/// Offsets are BYTE offsets: the guest's param_1 is a `long*`, so `(int)param_1[0x16]` is +0xb0 and
/// `*(uint*)(param_1 + 0x17)` is +0xb8 (raw bytes: 0x94832..0x94856 `mov [r13+0xb0/0xb4/0xb8/0xbc]`,
/// `mov qword [r13+0xc0]`; 0x93b96..0x93bbe the same on r12). This used to write +0x58/+0x5c
/// (0x16*4 / 0x17*4), clobbering the pass-context qword param_1[0xb] with {0, width} on every
/// native 0x94600 (gap N2-56).
void CommitPassViewport(u64 pass_addr, u32 width, u32 height, float z_lo = 0.5f,
                        float z_hi = 0.5f) {
    *reinterpret_cast<u32*>(pass_addr + 0xb0) = 0;
    *reinterpret_cast<u32*>(pass_addr + 0xb4) = 0;
    *reinterpret_cast<u32*>(pass_addr + 0xb8) = width;
    *reinterpret_cast<u32*>(pass_addr + 0xbc) = height;
    *reinterpret_cast<f32*>(pass_addr + 0xc0) = z_lo;
    *reinterpret_cast<f32*>(pass_addr + 0xc4) = z_hi;
}

/// The image-anchored cells PTR_DAT_01604408 / PTR_DAT_01604428 (the stage-descriptor pointers
/// the pass bodies assign to pass+0x18) and the static descriptor objects they hold: the objects'
/// dword 0 is 2/4/6, the per-stage shader slot 0x93690 and 0x937c0 dispatch on.
constexpr u64 kStageDescriptorA = 0x1604408;
constexpr u64 kStageDescriptorB = 0x1604428;

/// The static stage-descriptor table the engine assigns at `slot` (pass+0x18). CATALOGUED, not
/// used by the port: `pass+0x20` (descriptor+0x14) is the per-stage shader slot, and its table
/// values (0x16043f0 = all 0xffffffff) are outside the 2/4/6 slots 0x93690/0x937c0 dispatch on,
/// so assigning table A/B to a pass whose +0x20 still holds the default object makes the next
/// 0x93690 (or the -O3 0x92bd0 shader setter, which reads +0x14) write at
/// `pass + 0x1d8 - 3*8^0xffffffff`. The port therefore never assigns a descriptor to a pass whose
/// +0x20 is not a 2/4/6 slot; the native body skips the slot cache and draws.
///
/// The static stage-descriptor object the engine assigns at `slot` (pass+0x18): an image pointer
/// cell (0x1604408 / 0x1604428 hold PTR_DAT_01604370 / 0x16043f0, `ptr = *(u64*)(base + va)`).
void AssignStageSlot(u64 pass_addr, u64 slot, u64 image_cell_va) {
    const u64 base = Guest::GuestImageBase();
    if (base == 0 || image_cell_va == 0) {
        return;
    }
    const u64 descriptor = *reinterpret_cast<const u64*>(base + image_cell_va);
    if (descriptor == 0) {
        return;
    }
    const u32 stage = *reinterpret_cast<const u32*>(descriptor + 0x14);
    if (stage == 2 || stage == 4 || stage == 6) {
        *reinterpret_cast<u64*>(pass_addr + slot) = descriptor;
        *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::StageDescriptor) = stage;
    }
}

/// The depth clear value the depth phase stages (DB_DEPTH_CLEAR = 0.0).
constexpr float kPassDepthClear = 0.0f;

/// The label/EOP fence both pass functions end with (0x86d210 size + 0x86eb60 event + 0x86edc0
/// WAIT): the native mirror reserves the guest label, signals it and completes the wait.
bool CommitPassFence(u64 cb, u32 size_dwords) {
    // SOTC_NATIVE_PASS_FENCE=0 (bisection, gap N2-59): no label is reserved in the guest command
    // buffer, signalled or waited on; everything else of the native pass body stays.
    static const bool fence = [] {
        const char* e = std::getenv("SOTC_NATIVE_PASS_FENCE");
        return !e || e[0] != '0';
    }();
    if (!fence) {
        return true;
    }
    const u64 slot = Rt::Eop::ReserveLabel(cb, size_dwords);
    if (slot == 0) {
        return false;
    }
    const Rt::Eop::Label label{.address = slot, .wait_mask = 0xffffffffu};
    Rt::Eop::SignalLabel(label);
    return Rt::Eop::WaitLabel(label, cb); // placed in the stream after the pass's ops (N2-65)
}

// -- Derivations (read-only) -------------------------------------------------------------------

/// The pass structure the phases share: descriptor gates, viewport dimensions, colour slots and
/// the depth bound. Read-only.
struct PassPlan {
    const char* name{"pass"};
    u32 width{0};
    u32 height{0};
    u32 kind_word{0};
    u32 prim_cache{0};
    u32 color_slots{0};
    u32 depth_bound{0};
    u32 label{0};
    u32 eop_event{0};
    u32 depth_clear_low{0};
    u32 state_restore{0};
    bool valid{false};
};

/// Reads `desc_addr` for one phase without touching guest state. The descriptor type/flags gates
/// are the bodies' own ((type | 2) == 3 for 0x93960, type == 2 for 0x94600, plus the 0x20 flag),
/// the dimensions come from +0x120 (A) or +0x194 (B), the colour slots are the valid colour
/// blocks, and the depth bound is the +0x164 block's format class.
bool DerivePassPlan(Phase phase, u64 pass_addr, u64 desc_addr, PassPlan& plan) {
    plan = {};
    // 0x93ef0 has its own gate on the BOUND descriptor (DepthPassOp below) and never comes here;
    // a Depth phase reaching this derivation would otherwise be traced under 0x94600's name.
    plan.name = phase == Phase::ColourA ? "0x93960" : phase == Phase::ClearB ? "0x94600" : "0x93ef0";
    if (phase == Phase::Depth) {
        return false;
    }
    if (desc_addr == 0) {
        return false;
    }
    const u8 type = *reinterpret_cast<const u8*>(desc_addr + PassDescOff::Type);
    const u8 flags = *reinterpret_cast<const u8*>(
        desc_addr + (phase == Phase::ColourA ? PassDescOff::FlagsA : PassDescOff::FlagsB));
    if (phase == Phase::ColourA) {
        if ((type | 2) != 3) {
            return false;
        }
    } else if (type != 2) {
        return false;
    }
    if ((flags & 0x20) == 0) {
        return false;
    }
    const u32 dims =
        Read32(desc_addr, phase == Phase::ColourA ? PassDescOff::DimsA : PassDescOff::DimsB);
    plan.width = dims & 0xffffu;
    plan.height = dims >> 16;
    plan.kind_word = Read32(desc_addr, 0x124);
    plan.prim_cache = Read32(pass_addr, Guest::PassOff::PrimitiveType);
    AmdGpu::ColorBuffer buf{};
    u32 extent = 0;
    for (u32 slot = 0; slot < 8; ++slot) {
        const u64 block = desc_addr + PassDescOff::ColorBlock + 0x40 * slot;
        if (Rt::DecodeColour(reinterpret_cast<const u64*>(block), buf, extent)) {
            ++plan.color_slots;
        }
    }
    AmdGpu::DepthBuffer db{};
    AmdGpu::DepthView view{};
    VAddr htile = 0;
    const u64 depth_block = desc_addr + PassDescOff::DepthBlock;
    plan.depth_bound = Rt::DecodeDepth(reinterpret_cast<const u64*>(depth_block), db, view, htile,
                                       extent) &&
                       Rt::DepthVkFormat(db) != vk::Format::eUndefined;
    plan.valid = true;
    return true;
}

/// Whether a pass body may run natively. Only with the emission on: the phase's draw is then a
/// native clear/fullscreen (the ladder's seam names its pipeline) and no guest chain happens
/// inside the body — a native phase replaces the guest's RT/state/fence packets.
bool NativePassEmissionReady() {
    return NativeEmitWanted();
}

/// SOTC_PASS_TEMPLATE=1: every pass call goes straight to the guest body without the native
/// attempt, so a run shows exactly the guest's own sequence (its 0x93690 calls included) next to
/// whatever the native path did in the same spot. Trace through the shared budget.
bool PassTemplateWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_PASS_TEMPLATE");
        return e && e[0] == '1';
    }();
    return wanted;
}

void TraceTemplateEntry(const char* fmt, ...) {
    if (Verbose() && NativeTraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: template ");
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fprintf(stderr, " guest body\n");
    }
}

bool PassCanRunNatively(Phase phase, u64 pass_addr, u64 desc_addr) {
    // Untraced: the scene runs on the native body without the derivation's descriptor work. The
    // bodies re-check what they commit (guarded writes), so the hot path pays nothing for the
    // trace-only decode.
    if (!Verbose()) {
        return NativePassEmissionReady();
    }
    PassPlan plan;
    const bool derived = DerivePassPlan(phase, pass_addr, desc_addr, plan);
    if (!derived) {
        if (Verbose() && NativeTraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: pass=%#llx desc=%#llx guest chain (descriptor "
                         "gate)\n",
                         static_cast<unsigned long long>(pass_addr),
                         static_cast<unsigned long long>(desc_addr));
        }
        return false;
    }
    if (Verbose() && NativeTraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: %s pass=%#llx desc=%#llx dims=%ux%u kind=%u prim=%#x "
                     "color=%u depth=%u label=%u eop=%#x dclr=%u restore=%u (trace)\n",
                     plan.name, static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(desc_addr), plan.width, plan.height,
                     plan.kind_word, plan.prim_cache, plan.color_slots, plan.depth_bound,
                     plan.label, plan.eop_event, plan.depth_clear_low, plan.state_restore);
    }
    // With the emission on the pass body runs to the end (no guest chain): its children are native
    // too (PassFullscreenRect), and a derivation that holds means every write it makes is
    // reproduced below.
    return NativePassEmissionReady();
}

} // namespace

// -- Native bodies (SOTC_NATIVE_EMIT=1) --------------------------------------------------------

/// Pass A (0x93960), the whole body in C++ (out/re/decomp/0009/00093960.c): the pass descriptor
/// gates, the state caches, the PS-header reset and the fullscreen rect with its fence.
bool RunPassA(u64 pass_addr, u64 desc_addr) {
    // SOTC_NATIVE_PASS_A=0 (bisection, gap N2-61): the derivation and traces still run, the body
    // goes to the guest (same as SOTC_NATIVE_PASS_B for 0x94600).
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_PASS_A");
        return !e || e[0] != '0';
    }();
    if (!wanted) {
        return false;
    }
    // The guest body (00093960.c) binds the pass's colour target (0x85cbc0(cb, 0, desc+0xe4)),
    // runs 0x93840 and stages blend / depth-control / mode state before its rect; this body
    // replays none of that, so its only valid outcome is a NATIVE rect. If the rect cannot be
    // native, the whole pass goes to the guest before any commit (gap N2-62: the old fallback
    // drew the guest rect into the previously bound target and corrupted guest data).
    if (!NativeColourRectResolvable(reinterpret_cast<u64*>(pass_addr))) {
        if (Verbose() && NativeTraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x93960 pass=%#llx colour rect not native, whole "
                         "pass to the guest\n",
                         static_cast<unsigned long long>(pass_addr));
        }
        return false;
    }
    const u64 cb = *reinterpret_cast<const u64*>(pass_addr + Guest::PassOff::Device);
    if (cb == 0) {
        return false;
    }
    const u64 image_base = Guest::GuestImageBase();
    if (image_base == 0) {
        return false;
    }
    // The CB_TARGET_MASK value 0x93840 stages for the pass (00093840.c): the low 11 bits of
    // desc+0xf0 are the active colour-slot mask; the body keeps that mask (00093960.c:38).
    const u32 slots = Read32(desc_addr, 0xf0) & 0x7ffu;
    const u32 target_mask =
        slots == 0 ? 0u : (slots | (slots >> 1) | (slots >> 2) | (slots >> 3));
    // 0x85de40(z_lo = 0.5f, z_hi = 0.5f, cb, 0, 0, w, h) (00093960.c:59, always emitted under the
    // body's guard: the caches start at 0 so the comparison fails unless +0xc4 is already 0.5).
    // The VM state path's per-draw viewport commit applies the pass's own dims. Record the caches
    // the call leaves first: reusing the ENGINE helper 0x91fc0 would stage 1.0/0.0 here (its own
    // guard, used by the RT binder 0x92140), and the next 0x93690 then lands on a NULL stage.
    const u32 width = Read32(desc_addr, PassDescOff::DimsA) & 0xffffu;
    const u32 height = Read32(desc_addr, PassDescOff::DimsA) >> 16;
    CommitPassViewport(pass_addr, width, height, 1.0f, 0.0f);
    // The body's state caches (00093960.c:38-49): CB_TARGET_MASK, the state-dirty clear, the
    // primitive (RectList), the draw kind and the stage descriptor the primitive reset assigns
    // (`param_1[4] = PTR_DAT_01604408`, 00093960.c:91, bin 0x93cf2; the 0x93690 that follows
    // reads pass+0x20[0] as its stage).
    *reinterpret_cast<u32*>(pass_addr + 0x1ac) = target_mask;
    *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::StateDirty) =
        static_cast<u8>(*reinterpret_cast<u8*>(pass_addr + Guest::PassOff::StateDirty) & 0xcf);
    *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::DrawKind) = 0;
    AssignStageSlot(pass_addr, Guest::PassOff::StageDescriptor2, kStageDescriptorA);
    // The pass mode the body caches at +0x18 (bin 0x93cad `mov dword [r12+0x18], 0x11`), written
    // after the slot assignment, which stages the descriptor's stage number there. +0x1cc (the
    // VGT_PRIMITIVE_TYPE cache) belongs to the 0x93690 child, which sets it itself.
    *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::StageDescriptor) = 0x11;
    // The material PS-header reset (0x861600(cb+0x2e8, 0, 0), 00093960.c:94).
    using GuestPsHeaderFn = void(PS4_SYSV_ABI*)(void*, u64, u64);
    reinterpret_cast<GuestPsHeaderFn>(image_base + 0x861600)(
        reinterpret_cast<void*>(cb + Guest::DeviceOff::MaterialBlock), 0, 0);
    // The fullscreen rect (0x93690) natively, then the fence. Pass A binds no depth block of its
    // own, so the child gets no depth override.
    if (!PassFullscreenRect(pass_addr)) {
        RunFullscreenRectGuest(pass_addr, /*depth_block_staged=*/false);
    }
    return CommitPassFence(cb, 4);
}

/// Pass B (0x94600): the colour + depth clear of the target set the caller bound, fenced.
bool RunPassB(u64 pass_addr, u64 desc_addr) {
    // SOTC_NATIVE_PASS_B=0 (bisection, gap N2-60): the derivation and traces still run, the body
    // goes to the guest. Separates RunPassB's own effects from the shim path itself.
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_PASS_B");
        return !e || e[0] != '0';
    }();
    if (!wanted) {
        return false;
    }
    const u64 cb = *reinterpret_cast<const u64*>(pass_addr + Guest::PassOff::Device);
    if (cb == 0) {
        return false;
    }
    const u32 dims = Read32(desc_addr, PassDescOff::DimsB);
    const u32 width = dims & 0xffffu;
    const u32 height = dims >> 16;
    // --- Commit point ---
    CommitPassViewport(pass_addr, width, height);
    *reinterpret_cast<u32*>(pass_addr + 0x14) = 1; // draw kind
    // `param_1[4] = PTR_DAT_01604428; *(u32*)(param_1+3) = 0x20` (00094600.c:26-30, bin
    // 0x94690-0x946a6): the stage descriptor B's own 0x93690 reads, then the pass mode at +0x18
    // (bin 0x946a6 `mov dword [r13+0x18], 0x20`; this used to land in +0x1cc, the child's
    // VGT_PRIMITIVE_TYPE cache).
    AssignStageSlot(pass_addr, Guest::PassOff::StageDescriptor2, kStageDescriptorB);
    *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::StageDescriptor) = 0x20;
    // The depth target of B's own descriptor (00094600.c:63-64, bin 0x94656/0x9465d): the body
    // stages it into pass+0xa8 as a STACK COPY before the clear draw, so the native path takes the
    // block from here and not from that cache.
    const u64 depth_block = desc_addr + PassDescOff::DepthBlock;
    *reinterpret_cast<u64*>(pass_addr + 0xa8) = depth_block;
    // The clear draw (0x93690) — the colour clear and the depth store of the staged block — then
    // the EOP and the depth-dirty byte (00094600.c:85-96).
    if (!PassFullscreenRect(pass_addr, depth_block)) {
        RunFullscreenRectGuest(pass_addr, /*depth_block_staged=*/true);
    }
    *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) =
        static_cast<u8>(*reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) | 1);
    return CommitPassFence(cb, 8);
}

/// What one 0x93ef0 call does (out/re/decomp/0009/00093ef0.c; PM4 registers checked in the raw
/// bytes of 0x86dd20 / 0x86be10: SET_CONTEXT_REG 0xc0016900 offsets 0 and 3). The body is NOT a
/// clear. Gate: `bound+0x62 == 2 && (bound != target || flags != 1)`. It stages a copy of BOUND's DB
/// block (+0x164) whose Z/stencil WRITE bases (dwords 4/5) are TARGET's read bases (0x868710 /
/// 0x868ca0 -> 0x8688e0 / 0x868e90), sets DB_RENDER_CONTROL depth/stencil COMPRESS_DISABLE = !flags
/// and, when bound != target, DB_RENDER_OVERRIDE FORCE_Z/STENCIL_DIRTY|VALID (+PRESERVE_COMPRESSION
/// = !flags), CB_TARGET_MASK 0, then draws the 0x93690 rect and fences. So bound == target is an
/// in-place HTILE decompress (a no-op for the port's uncompressed images) and bound != target is a
/// depth-stencil copy bound -> target (shadPS4: Rasterizer::DepthStencilCopy on the emulated path).
enum class DepthPassOp { Skip, Decompress, Copy };

DepthPassOp ClassifyDepthPass(u64 bound, u64 target, u8 flags) {
    if (bound == 0 || target == 0 || *reinterpret_cast<const u8*>(bound + PassDescOff::Type) != 2) {
        return DepthPassOp::Skip;
    }
    if (bound == target) {
        return flags != 1 ? DepthPassOp::Decompress : DepthPassOp::Skip;
    }
    return DepthPassOp::Copy;
}

const char* DepthPassOpName(DepthPassOp op) {
    switch (op) {
    case DepthPassOp::Skip:
        return "skip";
    case DepthPassOp::Decompress:
        return "in-place decompress";
    case DepthPassOp::Copy:
        return "depth copy bound->target";
    }
    return "?";
}

/// SOTC_NATIVE_PASS_DEPTH=1: run the OLD native 0x93ef0 body (a depth CLEAR of the target, which is
/// not what the guest does — see DepthPassOp). Kept only as a bisection switch for the
/// nvoglv64+0xe1373a fault (gap N2-49); default off = the shim chains to the guest body.
bool NativePassDepthClearWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_PASS_DEPTH");
        return e && e[0] == '1';
    }();
    return wanted;
}

/// One fprintf per line, with the thread id: the pass functions run on more than one guest thread
/// at once (run probe-passonly-18 interleaved a 0x93ef0 line with tid 22300's 0x93690 line).
void TraceDepthPass(const char* fmt, ...) {
    if (Verbose() && NativeTraceAllowed()) {
        char line[384];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        std::fprintf(stderr, "SOTC RENDER native: 0x93ef0 %s tid=%lu\n", line,
                     static_cast<unsigned long>(GetCurrentThreadId()));
    }
}

/// The OLD depth-phase body (0x93ef0) as a native depth clear of the target plus the fence. Wrong
/// semantics (the guest decompresses / copies, DepthPassOp); reached only with
/// SOTC_NATIVE_PASS_DEPTH=1 for the fault bisection.
bool RunPassDepth(u64 pass_addr, u64 target_desc) {
    const u64 cb = *reinterpret_cast<const u64*>(pass_addr + Guest::PassOff::Device);
    Renderer* renderer = Current();
    if (cb == 0 || renderer == nullptr) {
        TraceDepthPass("pass=%#llx declined (no cb/renderer)",
                       static_cast<unsigned long long>(pass_addr));
        return false;
    }
    // -- Derivation (read-only) --
    const u32 dims = Read32(target_desc, PassDescOff::DimsB);
    const u32 width = dims & 0xffffu;
    const u32 height = dims >> 16;
    if (width == 0 || height == 0) {
        TraceDepthPass("pass=%#llx declined (zero dims)", static_cast<unsigned long long>(pass_addr));
        return false;
    }
    AmdGpu::DepthBuffer db{};
    AmdGpu::DepthView view{};
    VAddr htile = 0;
    u32 extent = 0;
    const u64 block = target_desc + PassDescOff::DepthBlock;
    if (!Rt::DecodeDepth(reinterpret_cast<const u64*>(block), db, view, htile, extent)) {
        TraceDepthPass("pass=%#llx declined (DecodeDepth)",
                       static_cast<unsigned long long>(pass_addr));
        return false;
    }
    TargetSet set{};
    set.depth_buffer = db;
    set.depth_view = view;
    set.depth_htile = htile;
    set.depth_extent_hint = extent;
    set.depth.format = Rt::DepthVkFormat(db);
    set.depth.active = set.depth.format != vk::Format::eUndefined;
    if (!set.depth.active) {
        TraceDepthPass("pass=%#llx declined (depth format)",
                       static_cast<unsigned long long>(pass_addr));
        return false;
    }
    // -- Commit point: clear first (it may fail on view resolution), then the guest writes --
    if (!renderer->Passes().ClearDepth(set, kPassDepthClear, cb)) {
        TraceDepthPass("pass=%#llx declined (ClearDepth)",
                       static_cast<unsigned long long>(pass_addr));
        return false;
    }
    TraceDepthPass("pass=%#llx target=%#llx depth clear %ux%u issued natively (old body)",
                   static_cast<unsigned long long>(pass_addr),
                   static_cast<unsigned long long>(target_desc), width, height);
    CommitPassViewport(pass_addr, width, height);
    *reinterpret_cast<u32*>(pass_addr + 0x1ac) = 0; // CB_TARGET_MASK = 0 (00093ef0.c:50)
    *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::StateDirty) =
        static_cast<u8>(*reinterpret_cast<u8*>(pass_addr + Guest::PassOff::StateDirty) & 0xef);
    *reinterpret_cast<u32*>(pass_addr + 0x14) = 0;      // draw kind
    *reinterpret_cast<u32*>(pass_addr + 0x18) = 0x11;   // primitive cache
    *reinterpret_cast<u64*>(pass_addr + 0xa8) = block;  // depth-block cache (0x86a290's)
    *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) =
        static_cast<u8>(*reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) & 0xfe);
    CommitPassFence(cb, 4);
    return true;
}

// The entry shims below stay in the global namespace so the hook table's extern "C" declarations
// match them exactly (C language linkage, the guest's SysV ABI).

} // namespace Native

// Every shim runs its native part (derivation, traces, native body) on the large host stack
// (native_stack.h): the hooks are entered from 16-64 KiB guest job fibers (gap N2-63). Only the
// guest body call stays on the guest's own stack.

extern "C" __attribute__((sysv_abi)) void NativePass_93960(void* pass, unsigned long long desc) {
    constexpr u64 kVa = 0x93960;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    const u64 desc_addr = static_cast<u64>(desc);
    const bool ran = Native::RunOnHostStack([&]() -> bool {
        if (Native::PassTemplateWanted()) {
            Native::TraceTemplateEntry("0x93960 pass=%#llx desc=%#llx", pass_addr, desc_addr);
        } else if (Native::PassCanRunNatively(Native::Phase::ColourA, pass_addr, desc_addr) &&
                   Native::RunPassA(pass_addr, desc_addr)) {
            return true;
        }
        if (Native::Verbose() && Native::NativeTraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x93960 body handed to the guest pass=%#llx\n",
                         static_cast<unsigned long long>(pass_addr));
        }
        return false;
    });
    if (ran) {
        return;
    }
    void* body = Native::Original(kVa);
    if (body == nullptr) {
        return;
    }
    reinterpret_cast<Native::GuestPassFn>(body)(static_cast<u64*>(pass), desc_addr);
}

extern "C" __attribute__((sysv_abi)) void NativePass_94600(void* pass, unsigned long long desc) {
    constexpr u64 kVa = 0x94600;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    const u64 desc_addr = static_cast<u64>(desc);
    const bool ran = Native::RunOnHostStack([&]() -> bool {
        const bool traced = Native::Verbose() && Native::NativeTraceAllowed();
        if (traced) {
            std::fprintf(stderr,
                         "SOTC RENDER native: NativePass_94600 enter pass=%#llx desc=%#llx\n",
                         static_cast<unsigned long long>(pass_addr),
                         static_cast<unsigned long long>(desc_addr));
        }
        const bool can = !Native::PassTemplateWanted() &&
                         Native::PassCanRunNatively(Native::Phase::ClearB, pass_addr, desc_addr);
        const bool done = can && Native::RunPassB(pass_addr, desc_addr);
        if (traced) {
            std::fprintf(stderr, "SOTC RENDER native: NativePass_94600 can=%u ran=%u -> %s\n",
                         can ? 1u : 0u, done ? 1u : 0u, done ? "native" : "guest body");
        }
        return done;
    });
    if (ran) {
        return;
    }
    reinterpret_cast<Native::GuestPassFn>(Native::Original(kVa))(static_cast<u64*>(pass),
                                                                 desc_addr);
}

extern "C" __attribute__((sysv_abi)) void NativePass_93ef0(void* pass, unsigned long long bound,
                                                            unsigned long long target,
                                                            unsigned char flags) {
    constexpr u64 kVa = 0x93ef0;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    const u64 target_addr = static_cast<u64>(target);
    const u64 bound_addr = static_cast<u64>(bound);
    const bool ran = Native::RunOnHostStack([&]() -> bool {
        if (Native::PassTemplateWanted()) {
            Native::TraceTemplateEntry("0x93ef0 pass=%#llx bound=%#llx target=%#llx flags=%u",
                                       pass_addr, bound_addr, target_addr,
                                       static_cast<u32>(flags));
            return false;
        }
        const Native::DepthPassOp op = Native::ClassifyDepthPass(bound_addr, target_addr, flags);
        const u32 dims = target_addr != 0 ? Native::Read32(target_addr, 0x194) : 0u;
        Native::TraceDepthPass("pass=%#llx bound=%#llx target=%#llx flags=%u op=%s dims=%ux%u",
                               static_cast<unsigned long long>(pass_addr),
                               static_cast<unsigned long long>(bound_addr),
                               static_cast<unsigned long long>(target_addr),
                               static_cast<unsigned>(flags), Native::DepthPassOpName(op),
                               dims & 0xffffu, dims >> 16);
        if (op != Native::DepthPassOp::Skip && Native::NativePassDepthClearWanted() &&
            Native::NativePassEmissionReady() && Native::RunPassDepth(pass_addr, target_addr)) {
            return true;
        }
        Native::TraceDepthPass("pass=%#llx body handed to the guest",
                               static_cast<unsigned long long>(pass_addr));
        return false;
    });
    if (ran) {
        return;
    }
    reinterpret_cast<Native::GuestDepthPassFn>(Native::Original(kVa))(
        static_cast<u64*>(pass), bound, target_addr, flags);
}
