// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_hooks: the host callbacks the RenderJob entry hooks
// (gpu/sotc/hook_table.inc) jump to. Each callback corresponds to one Phase 2 stage of
// docs/engine/native-render-map.md. Until a stage's replacement is fully written and verified, its
// callback chains to the original guest body (via Core::SotcHooks::OriginalEntry), so selecting the
// stage is non-breaking and the divergence checker can compare the two.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <xxhash.h>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_bind.h"
#include "native/native_cull.h"
#include "native/native_renderer.h"
#include "sotc/hooks.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Native {

namespace {

/// Runs the original guest function body for a hooked entry (its relocated prologue, then a jump
/// back into the function). Returns null when the hook was not installed.
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

/// A per-thread trace counter. The RenderJob functions run concurrently on several guest render
/// threads (Game Render, RenderFlip, Shadow Game Update), so an unbounded fprintf in the hot path
/// interleaves buffered writes and corrupts the stream (observed: a crash at 0x936c9 reading a
/// garbage pointer after ~13 s of verbose logging). Logging is therefore bounded to the first few
/// hits per thread and uses the atomic-counter gate, never a lock held across the write.
u64 TraceBudget() {
    static const u64 budget = [] {
        const char* e = std::getenv("SOTC_NATIVE_TRACE_BUDGET");
        return e && e[0] != '\0' ? std::strtoull(e, nullptr, 10) : u64{64};
    }();
    return budget;
}

std::atomic<u64> g_trace_budget{TraceBudget()};

bool TraceAllowed() noexcept {
    return g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

// Forward declaration of the external TraceAllowed below (lives past the next anonymous
// namespace, so a declaration in this file scope would be ambiguous with that one's).

/// SOTC_NATIVE_CLEAR: runtime A/B for the Phase 2.1 clear/fullscreen-rect replacement. Default on
/// (any value but "0") once the renderer is up; "0" makes the callback a pure guest chain without
/// a rebuild.
bool NativeClearWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_CLEAR");
        return !e || e[0] != '0';
    }();
    return wanted;
}

/// SOTC_NATIVE_DRAW: runtime A/B for the Phase 2.2 indexed/auto draw objects (0x94ec0/0x94d00/
/// 0x94dd0). Same semantics as SOTC_NATIVE_CLEAR: default on, "0" = pure guest chain.
bool NativeDrawWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_DRAW");
        return !e || e[0] != '0';
    }();
    return wanted;
}

/// SOTC_NATIVE_INDIRECT: runtime A/B for the Phase 2.3 indirect draw objects (0x10add0/0xdb620).
/// Same semantics as SOTC_NATIVE_CLEAR: default on, "0" = pure guest chain.
bool NativeIndirectWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_INDIRECT");
        return !e || e[0] != '0';
    }();
    return wanted;
}

// -- Draw-kind dispatch (docs/engine/native-render-map-draws.md §draw-kind switch) --------------

/// The indirect draw objects (0x10add0 / 0xdb620) switch on the draw kind at dev+0xc548 (= the
/// material block's +0xc260 word; docs/engine/native-render-map-bind.md §1). Per kind: which
/// geometry record holds the start-vertex/base-instance nibble byte and which GNM stage enum
/// (2=VS, 4=ES, 6=LS) the nibble's SGPR slots belong to (proven: 0010add0.c:20-41, 000db620.c:
/// 16-37, 00862510.c:12-36). Other kinds draw nothing in the guest body either.
struct DrawKindInfo {
    bool known;
    u32 stage;        // GNM shader-stage ordinal (2=VS, 4=ES, 6=LS)
    u64 record_off;   // cb offset of the geometry/record pointer the nibble byte is read from
    u64 nibble_off;   // offset of the nibble byte inside that record
};

constexpr DrawKindInfo DrawKind(u32 kind) {
    switch (kind) {
    case 0x0:    return {true, 2, Guest::DeviceOff::GeometryA, 0x27};
    case 0x45:
    case 0xad:   return {true, 6, Guest::DeviceOff::GeometryC, 0x19};
    case 0xb0:   return {true, 4, Guest::DeviceOff::GeometryB, 0x19};
    case 0x145:
    case 0x1ad:  return {true, 6, Guest::DeviceOff::GeometryC, 0x19};
    default:     return {false, 0, 0, 0};
    }
}

/// The record base offsets differ by accessor: the indirect draw objects read the geometry
/// descriptors through cb (+0xc398/+0xc3a8/+0xc3b8) while 0x862510 reads the same records
/// through the material block (mat = cb+0x2e8: +0xc0b0/+0xc0c0/+0xc0d0 — the bind doc §1
/// proves the two maps agree). DrawKindInfo::record_off is the cb-view; the mat-view would be
/// record_off - Guest::DeviceOff::MaterialBlock. (0x862510's own dispatch lives in the guest
/// body; the native 2.2 path only consumes its Vulkan result, the vertexOffset argument.)
[[maybe_unused]] constexpr u64 MatRecordOff(u64 cb_record_off) {
    return cb_record_off - Guest::DeviceOff::MaterialBlock;
}

// -- The Phase 2.4 bind seam (gap N2-2's draw-side sibling) ------------------------------------

/// Resolves the Vulkan pipeline the guest's material bind 0x85fc10 would flush for this draw,
/// via the Phase 2.4 content-key path (Native::Bind): hash the material block as staged, look
/// the key up in the fill-side map. Returns null on any derivation gate or a key miss — the
/// caller chains to the guest body (a draw against whatever pipeline the emulated path last
/// bound would corrupt the frame). On success `*key_out`/`*ff_out` carry the derivation the
/// callback needs for the trace and for future per-draw user-data work.
///
/// `pass` (the draw object's param_1) feeds the fixed-function caches; `ff` is the caller's
/// primitive/fixed-function derivation (the direct objects use the mode-table rule, the indirect
/// ones the cached primitive). `gate_out` (optional) receives the first failing gate for the
/// trace.
const Vulkan::Pipeline* ResolveDrawPipelineForPass(u64 cb, u64 pass, const Bind::FixedFunctionState& ff_probe,
                                                   Bind::MaterialKey* key_out,
                                                   Bind::FixedFunctionState* ff_out,
                                                   Bind::Gate* gate_out) {
    Bind::MaterialSnapshot snap{};
    if (!Bind::DeriveSnapshot(cb, snap)) {
        if (gate_out) {
            *gate_out = Bind::Gate::NoMaterial;
        }
        return nullptr;
    }
    // Every staged stage must validate before the key is meaningful; the first failing gate is
    // reported and the caller chains (never guess).
    for (u32 s = 0; s < Bind::kNumStages; ++s) {
        const Bind::Gate gate = Bind::ValidateStage(snap.stage[s]);
        if (gate != Bind::Gate::None) {
            if (gate_out) {
                *gate_out = gate;
            }
            return nullptr;
        }
    }
    // The pass caches must be complete before the key is trusted (PrimitiveUnknown /
    // PassCacheIncomplete both arrive through ff_probe.ok == false).
    if (!ff_probe.ok) {
        if (gate_out) {
            *gate_out = Bind::Gate::PassCacheIncomplete;
        }
        return nullptr;
    }
    const Bind::MaterialKey key = Bind::BuildKey(snap, ff_probe);
    if (key_out) {
        *key_out = key;
    }
    if (ff_out) {
        *ff_out = ff_probe;
    }
    return Bind::Lookup(Bind::HashKey(key));
}

/// Resolves the vk::Buffer the indirect arguments at `args` (a guest heap address, PC memory
/// model) live in, so vkCmdDraw*Indirect can reference it. TODO(Phase 2.4/2.6, descriptors):
/// route through the port's buffer cache (the same guest-VA -> VkBuffer mapping the emulated
/// path's rasterizer already maintains). Returning null gates the indirect callbacks to the
/// guest chain while nothing names the buffer.
vk::Buffer ResolveArgsBuffer(const void* args, u64 size) {
    (void)args;
    (void)size;
    return VK_NULL_HANDLE;
}

/// Budget-gated fallback trace for the draw callbacks (the 2.1 ChainToGuest, generalized to the
/// five draw objects). Always returns false so the gate call sites read as the fallback decision.
bool ChainToGuest(const char* name, u64 pass_addr, const char* reason) {
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: %s pass=%#llx guest chain (%s)\n", name,
                     static_cast<unsigned long long>(pass_addr), reason);
    }
    return false;
}

// -- Guest helpers (engine side, SysV) ----------------------------------------------------------
// The PC memory model (gpu/native/guest_bridge.h) makes a guest image offset a host pointer once
// Guest::GuestImageBase() is published, so engine-side helpers are callable at image+va with the
// guest's SysV convention. Only ENGINE helpers are reused this way: the 0x85xxxx/0x86xxxx GNM
// library is bypassed by the native backend (docs/engine/native-render-seam.md) — its calls are
// the specification, not work.

/// 0x95580: ensure the shader record slot is loaded (the guest body calls it every draw; the lazy
/// construct happens once, behind its own guard).
using GuestEnsureLoadedFn = void (PS4_SYSV_ABI*)(void* slot);
/// 0x14d7b0: the shared lazy record resolve of the draw chain (pass-map §7); returns the record.
using GuestResolveFn = void* (PS4_SYSV_ABI*)(void* lazy);

constexpr u64 kEnsureLoadedVa = 0x95580; // out/re/decomp/0009/00095580.c
constexpr u64 kResolveVa = 0x14d7b0;     // out/re/decomp/0014/0014d7b0.c

/// Resolves the record that 0x93690 caches its per-stage program in, exactly like the guest body
/// (00093690.c:10-12): DAT_02ee3ce0 -> +0x8a8 (shader list) -> +0x68 (holder), ensure the
/// holder's slot is loaded (0x95580), then RESOLVE the record (0x14d7b0) — the returned pointer is
/// the object the body indexes (`lVar3 = FUN_0014d7b0(*(holder+0x20))`), NOT the holder or the
/// object 0x93690 loads directly. The program is at returned+0x60 (EngineOff::RecordProgram).
/// Returns 0 (no crash, no log) while the image base or any link is missing, which chains to the
/// guest body.
u64 ResolveFullscreenRecord() {
    using namespace Guest;
    const u64 base = GuestImageBase();
    if (base == 0) {
        return 0;
    }
    const u64 root = *reinterpret_cast<const u64*>(base + EngineOff::RootGlobals);
    if (root == 0) {
        return 0;
    }
    const u64 list = *reinterpret_cast<const u64*>(root + EngineOff::ShaderList);
    const u64 holder = list ? *reinterpret_cast<const u64*>(list + EngineOff::ShaderHolder) : 0;
    if (holder == 0) {
        return 0;
    }
    reinterpret_cast<GuestEnsureLoadedFn>(base + kEnsureLoadedVa)(
        reinterpret_cast<void*>(holder + EngineOff::HolderLoader));
    const u64 lazy = *reinterpret_cast<const u64*>(holder + EngineOff::HolderLoader);
    if (lazy == 0) {
        return 0;
    }
    return reinterpret_cast<u64>(
        reinterpret_cast<GuestResolveFn>(base + kResolveVa)(reinterpret_cast<void*>(lazy)));
}

u64 ResolveFullscreenProgram() {
    using namespace Guest;
    const u64 record = ResolveFullscreenRecord();
    if (record == 0) {
        return 0;
    }
    return *reinterpret_cast<const u64*>(record + EngineOff::RecordProgram);
}

/// The pipeline the fullscreen draw (0x93690) binds: the shared fullscreen program is staged into
/// the material block by the 0x861520/0x861260/0x860bb0 setters the pass functions call before
/// the draw (00093690.c:19-30), so the draw's content key — the one the fill side (N2-10) fills
/// with the port's command-processor pipeline — names it. `pass` is the pass context (its +0x20 is
/// the stage descriptor the body dispatches on); the fixed function is the RectList clear state
/// the pass staged (prim 0x11, no depth test), derived from the pass's own caches.
const Vulkan::Pipeline* ResolvePassFullscreenPipeline(u64* pass) {
    using namespace Guest;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    const u64 cb = *pass;
    if (cb == 0) {
        return nullptr;
    }
    const u32 stage = *reinterpret_cast<const u32*>(pass_addr + PassOff::StageDescriptor2);
    // 0x93690 binds the record through the setters, by stage slot 2/4/6 (VS/ES/LS) and PS.
    if (stage > 6) {
        return nullptr;
    }
    Bind::MaterialSnapshot snap{};
    if (!Bind::DeriveSnapshot(cb, snap)) {
        return nullptr;
    }
    for (u32 s = 0; s < Bind::kNumStages; ++s) {
        const Bind::Gate gate = Bind::ValidateStage(snap.stage[s]);
        if (gate != Bind::Gate::None) {
            return nullptr;
        }
    }
    Bind::FixedFunctionState ff{};
    ff.primitive_type = static_cast<u32>(AmdGpu::PrimitiveType::RectList);
    ff.index_type = 0;
    ff.db_depth_control = *reinterpret_cast<const u32*>(pass_addr + 0x11c);
    ff.cb_target_mask = *reinterpret_cast<const u32*>(pass_addr + 0x1ac);
    ff.cb_blend0 = *reinterpret_cast<const u32*>(pass_addr + 0x88);
    ff.pa_su_sc_mode = *reinterpret_cast<const u32*>(pass_addr + 0x23);
    ff.ok = true;
    const Bind::MaterialKey key = Bind::BuildKey(snap, ff);
    return Bind::Lookup(Bind::HashKey(key));
}

/// Resolves the Vulkan pipeline to bind for the fullscreen draw, keyed by the guest shader
/// program. Returns null while the native core cannot name one, which chains to the guest body —
/// the authoritative emission until the gate opens.
///
/// Phase 2.4: consulted through the bind path's program-keyed secondary map (the fill side —
/// gap N2-10 — registers the pipeline the port's command-processor cache builds for the same
/// program). Empty map = null = chain, unchanged behaviour from the 2.1 gate.
const Vulkan::Pipeline* ResolveFullscreenPipeline(u64 program) {
    return Bind::LookupFullscreenProgram(program);
}

/// Traces (verbose, budget-gated) that the 0x93690 callback fell back to the guest chain and
/// why. Always returns false so the gate call sites read as the fallback decision itself.
bool ChainToGuest(const char* reason, u64 pass_addr) {
    return ChainToGuest("0x93690", pass_addr, reason);
}

/// Emits one draw object natively. The pipeline comes from the object's own seam (the material
/// content key, resolved by the caller); the draw then goes through the port's command-processor
/// GPU-state path on a copy of the registers the object itself would have emitted — the state
/// path reads them (targets, formats, blend masks, primitive type, index type, start vertex).
/// `first_vertex` is the non-indexed vertexOffset the object hands to 0x862510; `args`/`args_size`
/// name the indirect table the object would have read (0 = a plain draw).
bool IssueNativeDraw(u64* pass, const Vulkan::Pipeline* pipeline, u32 num_indices, bool is_indexed,
                     u32 index_offset, u32 first_vertex, VAddr args = 0, u32 args_size = 0,
                     bool fullscreen = false) {
    Renderer* renderer = Current();
    if (pipeline == nullptr || pass == nullptr || renderer == nullptr) {
        return false;
    }
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    // The registers the object would leave behind: the primitive type and the per-stage caches
    // the callbacks derive; everything else is the register image the frame is building. The copy
    // lives in this frame: two guest threads draw through the same renderer at once.
    AmdGpu::Regs regs = renderer->Draws().GuestRegs();
    regs.num_indices = num_indices;
    regs.primitive_type = static_cast<AmdGpu::PrimitiveType>(
        static_cast<u32>(*reinterpret_cast<const u32*>(pass_addr + Guest::PassOff::PrimitiveType)));
    if (is_indexed) {
        // The pass's index-type cache (0x94c60 writes it): 0 = 16-bit, nonzero = 32-bit. The draw
        // object resets it before the draw; the object's own value is what the packet carried.
        regs.index_buffer_type.index_type = static_cast<AmdGpu::IndexType>(
            *reinterpret_cast<const u32*>(pass_addr + Guest::PassOff::IndexTypeCache) != 0 ? 1 : 0);
    }
    Draw::DrawRequest request{};
    request.regs = &regs;
    request.pipeline = pipeline;
    request.is_indexed = is_indexed;
    request.args = args;
    request.args_size = args_size;
    request.fullscreen = fullscreen;
    request.index_offset = is_indexed ? index_offset : first_vertex;
    request.num_indices = num_indices;
    request.vertex_sgpr_offset = 0; // filled by the draw-kind preamble (0x862510) when it lands
    request.instance_sgpr_offset = 0;
    return renderer->Draws().IssueFromRegs(request);
}

/// Whether this 0x93690 call is a DEPTH CLEAR: its only GPU effect is the full-surface store of
/// the constant DB_DEPTH_CLEAR the calling pass stages (neither the pipeline nor the shaders
/// decide anything the store does not already decide). Two callers exist:
///   0x932a0 kind 2 (000cc9b0.c:77, the shadow cascades): the RT set is bound by 0x92140 just
///     before, the depth target is the shadow descriptor's DB block, and the clear value is
///     `1.0f - arg3` with arg3 = 0 (000932a0.c:37) = the 0.0 the guest stages at :36. The
///     fullscreen draw it performs comes down to that store: LoadDepthEnable is off in kind 2,
///     whose masks fold colour and stencil writes away, so only depth 0.0 lands.
///   0x93ef0 (00093ef0.c:39, :85): `FUN_0086a290(cb, +0x164)` + `FUN_00869f50(0)` stage
///     DB_DEPTH_CLEAR = 0.0, then this draw; the trailing `0x3f800000` (line 85) is the
///     immediate of the first 0x91e10 call (its param_3), not a clear value. The pass ctx's
///     +0x21 bit (0x86a290 caches the block there) marks the depth block bound from +0x164.
/// The pass context offsets are native_pass_callbacks.cpp's (pass-map §1.1/§1.4): the RT dirty
/// word's force bit (+0x8 bit0) and the depth-dirty byte (+0x11 bit0) are the guest's own
/// "re-emit the depth block" gates, so they are the signal that a clear just preceded.
bool NativeClearWantsDepthClear(u64* pass) {
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    const u8 force = *reinterpret_cast<const u8*>(pass_addr + Guest::PassOff::DirtyForce);
    if ((force & 1) != 0) {
        return true; // 0x92140's force path: the whole RT set is re-bound, clear or not
    }
    if ((*reinterpret_cast<const u8*>(pass_addr + 0x21) & 1) != 0) {
        return true; // 0x93ef0's depth block bind (0x86a290's +0x21 cache)
    }
    // 0x94600's clear path: kind 6 precedes this draw and every slot was re-bound; the shadow
    // base set (kind 2) is covered by the force bit during its own bind and by the depth-dirty
    // byte of the pass before it otherwise.
    return (*reinterpret_cast<const u8*>(pass_addr + Guest::PassOff::DepthDirty) & 1) != 0;
}
// Neither dirty bit is the signal: 0x97d50/0x97e20 set bits of the same words for their own
// state, and the 0x93690 chain ends by writing pass+0x11 = 0xfe (bit0 clear). What the clear
// draws is the pass's own depth target: for 0x93ef0 that is the descriptor's DB block
// (00093ef0.c:38), for the 0x932a0 cascades it is the bind 0x92140 just performed. A pass whose
// target decodes as a plain Z16/Z32F surface (an irregular format such as 0 = INVALID, or a
// read-only view, belongs to a deeper depth/clear path) gets the native store; the value is the
// 0.0 both callers stage.
bool NativeClearHasStagedDepth(u64* pass, u64 explicit_block) {
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    if (explicit_block != 0) {
        return true;
    }
    const u8 dirty = *reinterpret_cast<const u8*>(pass_addr + Guest::PassOff::DepthDirty);
    if ((dirty & 1) == 0) {
        return false;
    }
    const u64 cached = *reinterpret_cast<const u64*>(pass_addr + 0xa8);
    if (cached == 0) {
        return false;
    }
    AmdGpu::DepthBuffer db{};
    AmdGpu::DepthView view{};
    VAddr htile = 0;
    u32 extent = 0;
    if (!Rt::DecodeDepth(reinterpret_cast<const u64*>(cached), db, view, htile, extent)) {
        return false;
    }
    return db.z_info.format == AmdGpu::DepthBuffer::ZFormat::Z16 ||
           db.z_info.format == AmdGpu::DepthBuffer::ZFormat::Z32Float;
}

/// Derives the depth target this 0x93690 clear writes and runs the native clear. The block comes
/// from the caller: 0x94600 stages the DB block of its own descriptor (+0x164) right before the
/// draw (00094600.c:63-64) and caches a STACK COPY at pass+0xa8 whose first qwords are not a
/// DepthBuffer; 0x93ef0 caches the descriptor block it bound (00093ef0.c:47). Only a cached
/// DESCRIPTOR (pass+0x11 bit0 clear, written by 0x92140/0x93ef0 with that pointer) is trusted
/// from the cache; the clear value is the 0.0 both paths stage.
bool TryNativeDepthClear(u64* pass, u64 pass_addr, u64 explicit_block) {
    Renderer* renderer = Current();
    if (renderer == nullptr) {
        return false;
    }
    const bool cached_is_descriptor =
        (*reinterpret_cast<const u8*>(pass_addr + Guest::PassOff::DepthDirty) & 1) == 0;
    const u64 cached = *reinterpret_cast<const u64*>(pass_addr + 0xa8);
    const u64 block = explicit_block != 0 ? explicit_block : (cached_is_descriptor ? cached : 0);
    if (block == 0) {
        return false; // no descriptor block known: nothing to clear natively
    }
    AmdGpu::DepthBuffer db{};
    AmdGpu::DepthView view{};
    VAddr htile = 0;
    u32 extent = 0;
    const bool decoded = Rt::DecodeDepth(reinterpret_cast<const u64*>(block), db, view, htile, extent);
    TargetSet set{};
    set.depth_buffer = db;
    set.depth_view = view;
    set.depth_htile = htile;
    set.depth_extent_hint = extent;
    set.depth.format = Rt::DepthVkFormat(db);
    set.depth.active = decoded && set.depth.format != vk::Format::eUndefined;
    // pass[0] is the guest command buffer: the clear goes into the stream there (N2-65).
    const bool cleared = set.depth.active && renderer->Passes().ClearDepth(set, 0.0f, *pass);
    if (!cleared) {
        // Budgeted, not verbose-gated: the depth clear is the path the pass bodies depend on, so
        // a run must say which link failed (decode, format, or the texture cache's image).
        if (TraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x93690 depth clear declined block=%#llx decoded=%u "
                         "format=%d active=%u\n",
                         static_cast<unsigned long long>(block), decoded ? 1u : 0u,
                         static_cast<int>(set.depth.format), set.depth.active ? 1u : 0u);
        }
        return false;
    }
    // The guest body leaves the primitive-type cache at RectList (00093690.c:26-29) and the
    // index-type cache at 0; both caches are the pass's own bookkeeping, mirrored here so a
    // later chained object sees the guest's post-clear state.
    *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::PrimitiveType) = 0x11;
    *reinterpret_cast<u64*>(pass_addr + Guest::PassOff::IndexTypeCache) = 0;
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x93690 pass=%#llx depth clear block=%#llx depth=0.0 "
                     "(%ux%u) issued natively\n",
                     static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(block), extent & 0xffffu, extent >> 16);
    }
    return true;
}


/// The native replacement of one 0x93690 call (00093690.c, rewritten in C++): resolve the shared
/// fullscreen program the engine list holds, bind it for the call's stage, stage the draw's
/// primitive/index state, issue the fullscreen triangle, and mirror the guest-visible caches. It
/// is the only path for a hooked call: false means the call has to go to the guest body (a
/// missing link, the A/B switch, or GPU state the native core does not hold yet — reason traced).
bool RunNativeClear(u64* pass, u64 depth_block = 0) {
    using namespace Guest;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    Renderer* renderer = Current();
    // Entry marker, not a trace: a crash log then shows whether the native body ran at all
    // (a guest-body crash at 0x936cb with markers before it proves the port took over that call).
    // The thread id and the depth-clear sub-path are there for the same reason (runs of
    // 2026-11-21 stalled or crashed after the first depth-clear marker).
    if (TraceAllowed()) {
        const u64 cb_in = pass ? *pass : 0;
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x93690 enter pass=%#llx cb=%#llx renderer=%u depth=%u "
                     "tid=%lu\n",
                     static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(cb_in),
                     renderer != nullptr && NativeClearWanted() ? 1u : 0u,
                     depth_block != 0 ? 1u : 0u, static_cast<unsigned long>(GetCurrentThreadId()));
    }
    if (renderer == nullptr || !NativeClearWanted()) {
        return false;
    }
    // Guest ABI: param_1 = pass context (qword array); pass[0] = device / GNM cursor object.
    const u64 cb = pass ? *pass : 0;
    if (cb == 0) {
        return ChainToGuest("no device context", pass_addr);
    }
    // The stage descriptor at pass+0x20: its first dword selects the per-stage shader slot the
    // body binds (2/6/4 = the VS/LS/ES paths, 000937c0.c shares the mapping). A value outside the
    // slots, or a null descriptor, is what the engine leaves behind after its per-frame setup
    // (`pass+0x18` = PTR_DAT_01604408, which resets +0x20 to NULL): the shader-slot CACHE write
    // is then skipped, and the draw itself is unaffected (run 2026-11-21: native body entered with
    // `pass[4] == 0` on every pass clear, and the guest body faults on the same value).
    const u32 stage =
        *reinterpret_cast<const u32*>(reinterpret_cast<u64>(pass) + PassOff::StageDescriptor2);
    // Depth clear (the shadow cascades and the 0x93ef0 depth touches clear through this same
    // draw; its only effect is the full-surface store of the staged DB_DEPTH_CLEAR value,
    // 0x932a0 kind 2 / 00093ef0.c:85). The port clears the image directly.
    const bool wants_depth = NativeClearHasStagedDepth(pass, depth_block);
    if (Verbose() && TraceAllowed()) {
        const u64 cached = *reinterpret_cast<const u64*>(reinterpret_cast<u64>(pass) + 0xa8);
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x93690 depth-path pass=%#llx block=%#llx cached=%#llx "
                     "dirty=0x%02x force=0x%02x b21=0x%02x\n",
                     static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(depth_block),
                     static_cast<unsigned long long>(cached),
                     *reinterpret_cast<const u8*>(reinterpret_cast<u64>(pass) + PassOff::DepthDirty),
                     *reinterpret_cast<const u8*>(reinterpret_cast<u64>(pass) + PassOff::DirtyForce),
                     *reinterpret_cast<const u8*>(reinterpret_cast<u64>(pass) + 0x21));
    }
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: 0x93690 step=%s pass=%#llx\n",
                     wants_depth ? "depth" : "colour",
                     static_cast<unsigned long long>(pass_addr));
    }
    if (wants_depth) {
        if (TryNativeDepthClear(pass, pass_addr, depth_block)) {
            return true;
        }
        return ChainToGuest("depth clear unresolved", pass_addr);
    }
    // The draw's pipeline: the fill side names the content of what the setters staged, and the
    // shared fullscreen program the body resolves is the secondary source.
    const Vulkan::Pipeline* pipeline = ResolvePassFullscreenPipeline(pass);
    if (pipeline == nullptr) {
        const u64 program = ResolveFullscreenProgram();
        if (program == 0) {
            return ChainToGuest("fullscreen shader not resolved", pass_addr);
        }
        pipeline = ResolveFullscreenPipeline(program);
    }
    if (pipeline == nullptr) {
        return ChainToGuest("pipeline unresolved", pass_addr);
    }

    // --- Every guest write the body performs, in its order (00093690.c:14-37) ---

    // The per-stage program cache: pass[(u32)pass[4] + 0x3b] gets a shader record
    // (`mov [r14+rcx*8+0x1d8], rax`, raw bytes at VA 0x936ca). The port's draws take their shaders
    // from the pipeline, so the slot gets a record-shaped block that satisfies the binder's
    // `record[0x19] == 2` test; the record's host address is written where the body stages it.
    //
    // A call that cannot be served natively has to come back WITHOUT any guest write: the caller
    // then serves the call itself (the 0x93690 hook chains its own guest body) or chains only that
    // draw. Returning false after writing would make the caller repeat the write.
    const bool slot_ok = stage <= 6;
    u64 host_record = 0;
    if (slot_ok) {
        const u64 record = ResolveFullscreenRecord();
        if (record == 0) {
            return ChainToGuest("shader record unresolved", pass_addr);
        }
        host_record = record + EngineOff::RecordHostAddress;
    }

    // The draw: 0x86ac10(cb, 3, 0) -> DRAW_INDEX_AUTO count 3 -> a fullscreen triangle
    // (vkCmdDraw(3, 1, 0, 0)) through the port's GPU-state path (targets, transitions, the named
    // pipeline's descriptors, viewport/scissor, the bind).
    if (!IssueNativeDraw(pass, pipeline, 3, false, 0, 0, 0, 0, /*fullscreen=*/true)) {
        return ChainToGuest("fullscreen emission unavailable", pass_addr);
    }

    // --- Commit point: only what the body writes once its draw went out ---
    if (slot_ok) {
        if (pass[PassOff::StageDescriptor2 / 8] != 0) {
            auto* cache_stand_in = new u8[64];
            std::memset(cache_stand_in, 0, 64);
            *reinterpret_cast<u64*>(cache_stand_in) = host_record; // record[0]
            cache_stand_in[0x19] = 2;                              // the binder's record type
            *reinterpret_cast<u64*>(reinterpret_cast<u64>(pass) + PassOff::ShaderRecordCache +
                                    static_cast<u64>(stage) * 8) =
                reinterpret_cast<u64>(cache_stand_in);
        }
        // The primitive/index state the body stages (0x86a900/0x86bed0): RectList and the
        // index-type reset, gated by the pass's dirty/force word and caches. The caches stay in
        // step for the emulated objects that follow; the draw reads them from the pipeline.
        const u8 force = *reinterpret_cast<const u8*>(pass_addr + PassOff::DirtyForce);
        const s32 cached_index =
            static_cast<s32>(*reinterpret_cast<const u64*>(pass_addr + PassOff::IndexTypeCache));
        if ((force & 1) != 0 ||
            *reinterpret_cast<const u32*>(pass_addr + PassOff::PrimitiveType) != 0x11) {
            *reinterpret_cast<u32*>(pass_addr + PassOff::PrimitiveType) = 0x11;
        }
        if ((force & 1) != 0 || cached_index != 0) {
            *reinterpret_cast<u64*>(pass_addr + PassOff::IndexTypeCache) = 0;
        }
    }
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x93690 pass=%#llx cb=%#llx stage=%u record=%#llx "
                     "issued natively\n",
                     static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(cb), stage,
                     static_cast<unsigned long long>(host_record));
    }
    return true;
}

// The pass bodies (native_pass_callbacks.cpp) call 0x93690 as the guest bodies do; through the
// installed hook that would run the recorded body, so the child call is served here.
} // namespace

bool RunNativeClearForPass(u64* pass) {
    return RunNativeClear(pass);
}

bool RunNativeClearWithDepth(u64* pass, u64 depth_block) {
    return RunNativeClear(pass, depth_block);
}

/// Whether the colour fullscreen rect of a pass can be drawn natively right now: the same two
/// pipeline sources RunNativeClear's colour step tries, with no guest write. Pass A asks this
/// BEFORE committing anything (gap N2-62): its fallback to the guest rect after native commits
/// drew with a stale render target, because the native body does not replay the guest's
/// 0x85cbc0 colour-target bind.
bool NativeColourRectResolvable(u64* pass) {
    if (ResolvePassFullscreenPipeline(pass) != nullptr) {
        return true;
    }
    const u64 program = ResolveFullscreenProgram();
    return program != 0 && ResolveFullscreenPipeline(program) != nullptr;
}

// External linkage: the pass hooks (native_pass_callbacks.cpp) trace through the same budget,
// which the frame hooks would otherwise exhaust before the pass stage runs. The size comes from
// SOTC_NATIVE_TRACE_BUDGET (default 64).
bool NativeTraceAllowed() noexcept {
    return g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}



namespace {

} // namespace

/// SOTC_NATIVE_EMIT: the ladder from the per-stage recorders to the real GPU draws. "0" (default)
/// keeps every object on its recorder (the pre-cutover behaviour, needed for the A/B gates);
/// "1" issues through the port's own GPU-state path (Draw::IssueFromRegs -> Rasterizer: targets,
/// layout transitions, descriptors, user data, viewport/scissor, binds). A draw whose pipeline is
/// not nameable chains to the guest body in both modes.
bool NativeEmitWanted() {
    // Cutover default (phase 3): the ladder is ON whenever the native renderer is on, so a
    // native stage issues through the port's GPU-state path and never records duplicate draws on
    // top of a chained guest body. SOTC_NATIVE_EMIT=0 keeps the pre-cutover recorders for the
    // A/B gates, exactly like SOTC_NATIVE_CLEAR/DRAW/INDIRECT/RT/CULL=0.
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_EMIT");
        return !e || e[0] != '0';
    }();
    return wanted;
}

// -- Phase 2.1: 0x93690 clear / fullscreen rect draw object -------------------------------------
// Signature: void f(u64* pass) in the guest's SysV ABI. The callbacks are marked sysv_abi so the
// entry jump lands with the guest's convention intact (rdi = arg); calls back into Windows-ABI
// code (logging, the renderer) are emitted as ms_abi calls by the compiler. The guest body binds
// the material and draws 3 vertices (prim 0x11 RectList); the native path mirrors the body's
// guest-visible caches and issues the fullscreen triangle (Native::Draw::FullscreenRect).
typedef void (*GuestPassFn)(u64*) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_Clear_93690(u64* pass) {
    // Native path first; any missing piece falls back to chaining the guest body (the pure
    // passthrough of the hook-mechanism isolation test is SOTC_NATIVE_CLEAR=0 without a renderer).
    if (RunNativeClear(pass)) {
        return;
    }
    reinterpret_cast<GuestPassFn>(Original(0x93690))(pass);
}

// -- Phase 2.2 / 2.3: the indexed/auto/indirect draw objects ------------------------------------
// (docs/engine/native-render-map-draws.md). Signatures are the guest's SysV ABI; the callbacks
// are sysv_abi for the same reason as RenderJob_Clear_93690. Each native path derives everything
// from the pass/device state, mirrors the body's guest-visible caches at the commit point, and
// issues the docs' vkCmdDraw* through the scheduler. Any unmet precondition (no renderer, no cb,
// draw kind out of the dispatch table, unimplemented bind/args seam) chains to the guest body
// budget-gated, so a partially-covered frame still renders through the emulated path.



constexpr const char* kAutoName = "0x94ec0";
constexpr const char* kOffsetName = "0x94d00";
constexpr const char* kOffsetInstName = "0x94dd0";
constexpr const char* kIndirectName = "0x10add0";
constexpr const char* kIndexIndirectName = "0xdb620";

constexpr u64 PrimTableVa = 0x00f9f2e0; // DAT_00f9f2e0: mode -> VGT_PRIMITIVE_TYPE (image offset)

inline u8 ReadU8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 ReadU32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
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

/// The shared preamble of the indexed/auto draw objects (00094ec0.c:10-20, 00094d00.c:11-21,
/// 00094dd0.c:11-21): the primitive type — table lookup by mode (DAT_00f9f2e0, image-anchored:
/// read through the image base, the gap N2-3 lesson), or RectList 9 for the tessellation path
/// (the `(byte)(ctx+0x18) & 6` selector). Returns false (before any mutation) when the image is
/// unmapped or the mode is out of range, so the guest chain serves the draw.
bool DerivePrimitive(u64 pass_addr, int mode, u32* prim_out) {
    using namespace Guest;
    const u64 base = GuestImageBase();
    if (base == 0) {
        return false;
    }
    u32 prim;
    // The RectList selector is the low byte of the stage-descriptor qword (pass+0x18):
    // `*(byte *)(param_1 + 3) & 6` in 00094ec0.c:10, byte-verified `test byte [rbx+0x18], 6`.
    // (pass+0x14 is a different field — the draw-kind dword the indirect objects pass to
    // 0x86ad40; pass-map §1.1 lists both.)
    if ((ReadU8(pass_addr, PassOff::StageDescriptor) & 6) == 0) {
        if (mode < 0) {
            return false;
        }
        prim = ReadU32(base, PrimTableVa + static_cast<u64>(mode) * 4);
    } else {
        prim = 9; // RectList (the fullscreen-quad path)
    }
    *prim_out = prim;
    return true;
}

/// The common derivation + commit of the three indexed/auto objects. Returns true when the
/// native path issued the draw. Arguments per the draws doc:
///   0x94ec0: first_vertex = start_vertex (0x862510(material, start_vertex, 0)), arg5 unused,
///            arg6 = index_count; non-indexed draw.
///   0x94d00: first_vertex = 0, arg5 = index_offset, arg6 = index_count; indexed draw.
///   0x94dd0: as 0x94d00 with `instances` = the 7th argument (NumInstances bracket).
bool IndexedAutoNative(const char* name, u64* pass, int mode, u32 first_vertex, u32 arg5,
                       u32 arg6, u32 instances, bool with_instances) {
    using namespace Guest;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    Renderer* renderer = Current();
    if (renderer == nullptr || !NativeDrawWanted()) {
        return false;
    }
    const u64 cb = pass ? *pass : 0;
    if (cb == 0) {
        return ChainToGuest(name, pass_addr, "no device context");
    }
    u32 prim = 0;
    if (!DerivePrimitive(pass_addr, mode, &prim)) {
        return ChainToGuest(name, pass_addr, "primitive table unavailable");
    }
    // The pipeline (the material bind 0x85fc10 seam) must be resolvable before anything commits.
    // Phase 2.4: derive the material content key (Native::Bind) — the derivation runs live and
    // is traceable even though the lookup misses until the fill side (gap N2-10) registers the
    // port's pipelines.
    Bind::MaterialKey bind_key{};
    Bind::FixedFunctionState bind_ff{};
    Bind::Gate bind_gate = Bind::Gate::None;
    const bool tess_prim = (ReadU8(pass_addr, PassOff::StageDescriptor) & 6) != 0;
    const Bind::FixedFunctionState ff_probe =
        Bind::DeriveFixedFunction(pass_addr, static_cast<u32>(mode), tess_prim);
    const Vulkan::Pipeline* pipeline =
        ResolveDrawPipelineForPass(cb, pass_addr, ff_probe, &bind_key, &bind_ff, &bind_gate);
    if (pipeline == nullptr) {
        return ChainToGuest(name, pass_addr, Bind::GateName(
            bind_gate == Bind::Gate::None ? Bind::Gate::PipelineUnknown : bind_gate));
    }

    // --- Commit point: nothing above mutated any guest state; everything below mirrors what
    // the guest body would leave behind, then issues the Vulkan draw. ---

    // Primitive-type cache (00094ec0.c:12-20): the guest compares then writes; the native path
    // writes the derived value (equal to what the guest would hold).
    WriteU32(pass_addr, PassOff::PrimitiveType, prim);
    // INDEX_TYPE reset (00094ec0.c:21-24, 0x94ec0 only): the guest clears its cache under the
    // force/cache gate; the mirror write is idempotent (value 0).
    if (name == kAutoName) {
        WriteU64(pass_addr, PassOff::IndexTypeCache, 0);
    }
    // The start-vertex / base-instance SGPR writes (0x862510 before and after the draw) become
    // the vkCmdDraw* vertexOffset/baseInstance arguments (draws doc §0x862510) — nothing to
    // mirror in guest memory.
    if (NativeEmitWanted()) {
        const u32 count = arg6;
        const u32 offset = name == kAutoName ? 0u : arg5;
        if (IssueNativeDraw(pass, pipeline, count, name != kAutoName, offset,
                            name == kAutoName ? first_vertex : 0u)) {
            if (Verbose() && TraceAllowed()) {
                std::fprintf(stderr, "SOTC RENDER native: %s emitted\n", name);
            }
            return true;
        }
        return ChainToGuest(name, pass_addr, "draw emission unavailable");
    }

    Vulkan::Scheduler& scheduler = renderer->Scheduler();
    scheduler.RecordDeferred([&renderer, pipeline, name, first_vertex, arg5, arg6, instances,
                              with_instances](vk::CommandBuffer cmdbuf) {
        renderer->Draws().BindPipeline(cmdbuf, *pipeline);
        if (name == kAutoName) {
            // 0x86ac10 -> DRAW_INDEX_AUTO: non-indexed count, BaseVertex = start vertex.
            renderer->Draws().Auto(cmdbuf, arg6, instances, first_vertex, 0);
        } else {
            // 0x86b040 -> DRAW_INDEX_OFFSET_2 body {count, offset, count, flags}.
            renderer->Draws().IndexedOffset(cmdbuf, arg6, arg5, with_instances ? instances : 1,
                                            static_cast<s32>(first_vertex));
        }
    });
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: %s pass=%#llx cb=%#llx prim=%u issued natively\n", name,
                     static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(cb), prim);
    }
    return true;
}

/// The shared body of the two indirect draw objects (0010add0.c / 000db620.c): the SET_BASE
/// register write, the material bind, the draw-kind dispatch, the indirect draw and the
/// NumInstances(1) restore. `indexed` selects DrawIndexIndirect (0xdb620) over DrawIndirect
/// (0x10add0); the index-type reset preamble is 0x10add0-only (its cache mirror is the same
/// idempotent write as 0x94ec0's). Returns true when the native path issued the draw.
bool IndirectNative(const char* name, u64* pass, const u32* pair, u32 data_offset,
                    bool indexed) {
    using namespace Guest;
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    Renderer* renderer = Current();
    if (renderer == nullptr || !NativeIndirectWanted()) {
        return false;
    }
    const u64 cb = pass ? *pass : 0;
    if (cb == 0) {
        return ChainToGuest(name, pass_addr, "no device context");
    }
    if (pair == nullptr) {
        return ChainToGuest(name, pass_addr, "no indirect pair");
    }
    // Draw-kind dispatch (0010add0.c:20-41): an unknown kind draws nothing in the guest body
    // either, but that has never been observed live — gate it rather than assume.
    const u32 kind = ReadU32(cb, DeviceOff::DrawKind);
    const DrawKindInfo info = DrawKind(kind);
    if (!info.known) {
        return ChainToGuest(name, pass_addr, "draw kind not in dispatch table");
    }
    const u64 record = ReadU64(cb, info.record_off);
    if (record == 0) {
        return ChainToGuest(name, pass_addr, "geometry record null");
    }
    // The material bind seam (Phase 2.4): derive the content key from the staged block. The
    // indirect objects set no primitive type; the one in force is the pass's cached
    // VGT_PRIMITIVE_TYPE (DeriveFixedFunctionCachedPrim; 0/unknown chains).
    Bind::MaterialKey bind_key{};
    Bind::FixedFunctionState bind_ff{};
    Bind::Gate bind_gate = Bind::Gate::None;
    const Bind::FixedFunctionState ff_probe = Bind::DeriveFixedFunctionCachedPrim(pass_addr);
    const Vulkan::Pipeline* pipeline =
        ResolveDrawPipelineForPass(cb, pass_addr, ff_probe, &bind_key, &bind_ff, &bind_gate);
    if (pipeline == nullptr) {
        return ChainToGuest(name, pass_addr, Bind::GateName(
            bind_gate == Bind::Gate::None ? Bind::Gate::PipelineUnknown : bind_gate));
    }
    const u8 nibble = ReadU8(record, info.nibble_off);
    const u32 vtx_sgpr = nibble & 0xf; // low nibble: start-vertex SGPR slot
    const u32 inst_sgpr = nibble >> 4; // high nibble: base-instance SGPR slot
    (void)vtx_sgpr;
    (void)inst_sgpr; // consumed by the recompiled shader's indirect path; no Vulkan equivalent
                     // (draws doc §0x10add0 Vulkan note)

    // The indirect args buffer must be nameable before anything commits. The stride matches
    // liverpool.cpp's DrawIndirect/DrawIndexIndirect cases (sizeof(DrawIndirectArgs) = 16 /
    // sizeof(DrawIndexedIndirectArgs) = 20).
    const void* args = reinterpret_cast<const void*>(pair);
    const u64 args_size = indexed ? 20 : 16;
    if (NativeEmitWanted()) {
        // The native emission names the args in the port's buffer cache itself (the guest's
        // table is guest memory); a miss chains like any other gate. The counts stay in the
        // table, exactly as the guest's packets carry them.
        if (IssueNativeDraw(pass, pipeline, 0, indexed, data_offset, 0,
                            reinterpret_cast<VAddr>(pair), static_cast<u32>(args_size))) {
            if (Verbose() && TraceAllowed()) {
                std::fprintf(stderr, "SOTC RENDER native: %s emitted\n", name);
            }
            return true;
        }
        return ChainToGuest(name, pass_addr, "indirect emission unavailable");
    }
    const vk::Buffer args_buffer = ResolveArgsBuffer(args, args_size);
    if (args_buffer == VK_NULL_HANDLE) {
        // TODO(Phase 2.4/2.6): buffer-cache lookup pending; the guest chain serves the draw.
        return ChainToGuest(name, pass_addr, "args buffer unresolved");
    }

    // --- Commit point: nothing above mutated any guest state; everything below issues what the
    // guest body would have emitted as PM4, natively. ---

    // Index-type reset preamble (0010add0.c:12-15; 0x10add0 only — 0xdb620 has none): the mirror
    // write only after the gates — a pre-gate clear made the chained guest body skip its own
    // INDEX_TYPE reset packet (the depth-band regression of the first smoke run).
    if (!indexed) {
        WriteU64(pass_addr, PassOff::IndexTypeCache, 0);
    }

    // The 0x86ad40 SET_BASE packet and the 0x86c000(1) instance restore are CP-side state the
    // native core tracks itself (gap N2-8: liverpool reads the SET_BASE address as the
    // indirect-args table). No guest cache mirrors to write on this path.

    Vulkan::Scheduler& scheduler = renderer->Scheduler();
    scheduler.RecordDeferred([&renderer, args_buffer, data_offset, indexed,
                              args_size](vk::CommandBuffer cmdbuf) {
        if (indexed) {
            // 0x86b3d0 -> DRAW_INDEX_INDIRECT.
            renderer->Draws().IndexedIndirect(cmdbuf, args_buffer, data_offset,
                                              static_cast<u32>(args_size));
        } else {
            // 0x86b150 -> DRAW_INDIRECT.
            renderer->Draws().Indirect(cmdbuf, args_buffer, data_offset,
                                       static_cast<u32>(args_size));
        }
    });
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: %s pass=%#llx cb=%#llx kind=%#x stage=%u issued "
                     "natively\n",
                     name, static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(cb), kind, info.stage);
    }
    return true;
}


// -- Phase 2.2: 0x94ec0 DrawIndexAuto draw object ------------------------------------------------
// void f(pass, int mode, u32 start_vertex, u32 index_count) — rdi/rsi/edx/ecx. Native: mirror the
// prim/index caches, issue vkCmdDraw(index_count, 1, start_vertex, 0).
typedef void (*GuestDrawAutoFn)(u64*, int, u32, u32) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_DrawAuto_94ec0(u64* pass, int mode,
                                                                   u32 start_vertex,
                                                                   u32 index_count) {
    if (IndexedAutoNative(kAutoName, pass, mode, start_vertex, 0, index_count, 1, false)) {
        return;
    }
    reinterpret_cast<GuestDrawAutoFn>(Original(0x94ec0))(pass, mode, start_vertex, index_count);
}

// -- Phase 2.2: 0x94d00 DrawIndexOffset draw object ----------------------------------------------
// void f(pass, int mode, unused, unused, u32 index_offset, u32 index_count) — rdi/rsi/rdx/rcx/
// r8d/r9d. Native: mirror the prim cache, issue vkCmdDrawIndexed(index_count, 1, index_offset, 0, 0).
typedef void (*GuestDrawOffsetFn)(u64*, int, u64, u64, u32, u32) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_DrawOffset_94d00(u64* pass, int mode, u64 a3,
                                                                     u64 a4, u32 index_offset,
                                                                     u32 index_count) {
    if (IndexedAutoNative(kOffsetName, pass, mode, 0, index_offset, index_count, 1, false)) {
        return;
    }
    reinterpret_cast<GuestDrawOffsetFn>(Original(0x94d00))(pass, mode, a3, a4, index_offset,
                                                           index_count);
}

// -- Phase 2.2: 0x94dd0 DrawIndexOffset + NumInstances bracket draw object -----------------------
// void f(pass, int mode, unused, unused, u32 index_offset, u32 index_count, u32 instances) —
// the 7th argument arrives on the stack ([rbp+0x10] after the prologue's push rbp). Zero static
// callers (gap N2-9: vtable-dispatched); the hook still covers the body for when the computed
// call sites land on this entry.
typedef void (*GuestDrawOffsetInstFn)(u64*, int, u64, u64, u32, u32, u32) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_DrawOffsetInst_94dd0(u64* pass, int mode,
                                                                         u64 a3, u64 a4,
                                                                         u32 index_offset,
                                                                         u32 index_count,
                                                                         u32 instances) {
    if (IndexedAutoNative(kOffsetInstName, pass, mode, 0, index_offset, index_count, instances,
                          true)) {
        return;
    }
    reinterpret_cast<GuestDrawOffsetInstFn>(Original(0x94dd0))(pass, mode, a3, a4, index_offset,
                                                               index_count, instances);
}

// -- Phase 2.3: 0x10add0 DrawIndirect mesh draw object --------------------------------------------
// void f(pass, u32* pair, u32 data_offset) — rdi/rsi/edx. The pair packs the 48-bit indirect-args
// base (SET_BASE, gap N2-8). Native: draw-kind dispatch, then vkCmdDrawIndirect.
typedef void (*GuestDrawIndirectFn)(u64*, const u32*, u32) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_DrawIndirect_10add0(u64* pass,
                                                                        const u32* pair,
                                                                        u32 data_offset) {
    if (IndirectNative(kIndirectName, pass, pair, data_offset, false)) {
        return;
    }
    reinterpret_cast<GuestDrawIndirectFn>(Original(0x10add0))(pass, pair, data_offset);
}

// -- Phase 2.3: 0xdb620 DrawIndexIndirect mesh draw object ----------------------------------------
// Same signature layout as 0x10add0. Native: draw-kind dispatch, then vkCmdDrawIndexedIndirect.
extern "C" __attribute__((sysv_abi)) void RenderJob_DrawIndexIndirect_db620(u64* pass,
                                                                            const u32* pair,
                                                                            u32 data_offset) {
    if (IndirectNative(kIndexIndirectName, pass, pair, data_offset, true)) {
        return;
    }
    reinterpret_cast<GuestDrawIndirectFn>(Original(0xdb620))(pass, pair, data_offset);
}

// -- Phase 2.4: 0x85fc10 Material_BindGraphicsShaders_B ------------------------------------------
// Signature: void f(u64* material) — one arg, the material block (cb+0x2e8) itself.
typedef void (*GuestMaterialBindFn)(u64*) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) void RenderJob_MaterialBind_85fc10(u64* material) {
    const u64 mat_addr = reinterpret_cast<u64>(material);
    // The derivation is read-only and runs regardless of the A/B switch (it is the observability
    // of the 2.4 gate); the switch only gates any future native emission.
    Bind::MaterialSnapshot snap{};
    if (material != nullptr && Bind::DeriveSnapshot(mat_addr, snap)) {
        Bind::Gate first_gate = Bind::Gate::None;
        for (u32 s = 0; s < Bind::kNumStages && first_gate == Bind::Gate::None; ++s) {
            first_gate = Bind::ValidateStage(snap.stage[s]);
        }
        if (first_gate == Bind::Gate::None) {
            if (Verbose() && TraceAllowed()) {
                // Trace-only key: without a pass pointer the fixed-function caches are not
                // readable on this hook, so the fixed-function key part is zeroed here; the
                // authoritative key is the draw-object callbacks' (ResolveDrawPipelineForPass).
                // The hash is computed WITHOUT Bind::HashKey: that call publishes the key into
                // the fill side's pending ring (N2-10), and a key whose fixed-function words are
                // zero never matches the command processor's pipeline. Publishing it would only
                // add a second (wrong) key next to the real one for the same shader signature
                // and turn every such signature ambiguous. The key bytes still come from
                // BuildKey; only the digest is local.
                const Bind::MaterialKey key = Bind::BuildKey(snap, Bind::FixedFunctionState{});
                const XXH128_hash_t trace_hash = XXH3_128bits(&key, sizeof(key));
                std::fprintf(stderr,
                             "SOTC RENDER native: 0x85fc10 mat=%#llx pending=%#llx bits=%#llx "
                             "key=%016llx%016llx (trace only, no native emission yet)\n",
                             static_cast<unsigned long long>(mat_addr),
                             static_cast<unsigned long long>(snap.group_pending),
                             static_cast<unsigned long long>(snap.pending_bits),
                             static_cast<unsigned long long>(trace_hash.high64),
                             static_cast<unsigned long long>(trace_hash.low64));
            }
        } else if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr, "SOTC RENDER native: 0x85fc10 mat=%#llx gate=%s\n",
                         static_cast<unsigned long long>(mat_addr), Bind::GateName(first_gate));
        }
    }
    // The guest body is the authoritative emission (the byte drains + PM4). Chain always in
    // Phase 2.4: the fill side has no pipelines to name yet (gap N2-10), and skipping the body's
    // byte clears would corrupt the emulated path's own state machine.
    reinterpret_cast<GuestMaterialBindFn>(Original(0x85fc10))(material);
}

// -- Phase 2.5: 0x92140 render-target set binder (native-rt) -------------------------------------
// docs/engine/native-render-map-bind.md §3: void 0x92140(pass_ctx, long* targets) — targets[0..7]
// = colour pass-target descriptors, targets[8] = depth descriptor, byte at targets+0x48 (qword [9]
// bit0) = the +0xe4 vs +0x124 embedded-block selector. The SetRenderTarget helper 0x85cbc0 is
// internal to this flow (its only callers are 0x92140 and the pass functions 0x93960/0x94600), so
// this hook covers it. Native path: derive the TargetSet from the descriptor blocks, resolve the
// attachment views through the port's texture cache (the same ImageDesc path as
// Rasterizer::PrepareRenderState), record the native rendering commit through the scheduler, and
// mirror the guest's pointer-identity dirty caches exactly (bind-map §3.4).

namespace {

constexpr const char* kTargetsName = "0x92140";

/// SOTC_NATIVE_RT: runtime A/B for the 2.5 RT-set replacement. Default on, "0" = pure guest chain
/// (same semantics as SOTC_NATIVE_CLEAR/DRAW/INDIRECT).
bool NativeRtWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("SOTC_NATIVE_RT");
        return !e || e[0] != '0';
    }();
    return wanted;
}

inline u8 RtRead8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 RtRead32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}
inline u64 RtRead64(u64 addr, u64 off) {
    return *reinterpret_cast<const u64*>(addr + off);
}

/// Pass-context offsets specific to the RT binder (bind-map §3.1/§5, all proven in 00092140.c).
/// The "+0xac" in the task brief is a doc-confirmed typo for +0x1ac (CB_TARGET_MASK), which
/// 0x93960 writes and 0x92140 never reads — carried on TargetSet for the trace only.
namespace RtPassOff {
constexpr u64 ColorTargetCache = 0x48; // +0x48+8i: cached colour block pointer, slots 0..7
constexpr u64 DepthTargetCache = 0xa8; // param_1[0x15]: cached depth block pointer
constexpr u64 TargetMaskCache = 0x1ac; // cached CB_TARGET_MASK (0x93960:38)
} // namespace RtPassOff

/// Pass-target descriptor offsets (bind-map §3.3; guest_bridge.h PassDescOff has the pass-function
/// variants. 0x92140 additionally reads the surface extents at +0x198/+0x19c via 0x96f50/0x96f60).
namespace TargetDescOff {
constexpr u64 Type = 0x62;           // type byte: (b | 2) == 3 -> colour target (types 1/3)
constexpr u64 ColorBlock = 0xe4;     // 8-qword CB_COLORn block (== AmdGpu::ColorBuffer, N2-7)
constexpr u64 ColorBlockAlt = 0x124; // the alternate block (selector bit0 = 1)
constexpr u64 DepthBlock = 0x164;    // 13-dword DB block
constexpr u64 Width = 0x198;         // u32 surface width (0x96f50)
constexpr u64 Height = 0x19c;        // u32 surface height (0x96f60)
} // namespace TargetDescOff

/// The 0x92140 step-5 RT-config code (00092140.c:179-184): sel&1 set -> 0; sel&1 clear -> 0x10,
/// overridden to (sel*2)&8 when (~sel & 0xc) != 0.
constexpr u32 RtConfigCode(u32 sel) {
    if ((sel & 1) != 0) {
        return 0;
    }
    return (~sel & 0xc) != 0 ? (sel * 2) & 8 : 0x10;
}

/// The per-slot guest cache values: the RESOLVED BLOCK pointer (descriptor + selector), not the
/// descriptor pointer — the guest caches desc+0xe4/0x124/+0x164 or 0 for a type-gate failure
/// (00092140.c:20-29/160/176), regardless of whether the block's format words decode (an invalid
/// block is still emitted, as CB_COLORn_INFO = 0).
struct RtBlockPtrs {
    u64 color[8]{};
    u64 depth{};
};

/// Resolves the 9 descriptors to block pointers + the decoded TargetSet (bind-map §3.1 steps 1-3,
/// 5). Returns false (nothing touched) when nothing is bound or the extents look wrong, so the
/// caller chains to the guest body. The descriptor pointers are host addresses (PC memory model);
/// the runners pass sub-ranges of the target manager M = *(F+0xd98).
bool RtDerive(const u64* targets, u64 pass_addr, TargetSet& set, RtBlockPtrs& blocks) {
    const u32 selector = static_cast<u32>(targets[9]) & 1;
    const u64 color_off = selector != 0 ? TargetDescOff::ColorBlockAlt : TargetDescOff::ColorBlock;

    u64 first_desc = 0;
    for (u32 i = 0; i < 8; ++i) {
        const u64 desc = targets[i];
        // Type gate (00092140.c:20 etc.): (type | 2) != 3 -> the slot resolves to NULL.
        if (desc == 0 || ((*reinterpret_cast<const u8*>(desc + TargetDescOff::Type) | 2) != 3)) {
            continue;
        }
        blocks.color[i] = desc + color_off;
        if (first_desc == 0) {
            first_desc = desc;
        }
        AmdGpu::ColorBuffer buffer{};
        u32 extent = 0;
        if (!Rt::DecodeColour(reinterpret_cast<const u64*>(blocks.color[i]), buffer, extent)) {
            continue; // invalid format words: the guest disables the slot (CB_COLORn_INFO = 0)
        }
        set.color_buffer[i] = buffer;
        set.color_extent_hint[i] = extent;
        set.color[i].format = Rt::ColourVkFormat(buffer);
        set.color[i].active = set.color[i].format != vk::Format::eUndefined;
    }

    const u64 depth_desc = targets[8];
    if (depth_desc != 0) {
        blocks.depth = depth_desc + TargetDescOff::DepthBlock;
        AmdGpu::DepthBuffer db{};
        AmdGpu::DepthView view{};
        VAddr htile = 0;
        u32 extent = 0;
        if (Rt::DecodeDepth(reinterpret_cast<const u64*>(blocks.depth), db, view, htile, extent)) {
            set.depth_buffer = db;
            set.depth_view = view;
            set.depth_htile = htile;
            set.depth_extent_hint = extent;
            set.depth.format = Rt::DepthVkFormat(db);
            // DB_DEPTH_VIEW read-only bits (depth block dword 8, bits 22/23): the native depth
            // attachment is read-only only when both aspects are.
            set.depth_read_only = view.z_read_only != 0 && view.stencil_read_only != 0;
            set.depth.active = set.depth.format != vk::Format::eUndefined;
        }
        if (first_desc == 0) {
            first_desc = depth_desc; // the guest's lVar1 fallback (00092140.c:136-143)
        }
    }
    if (first_desc == 0) {
        return false;
    }

    set.selector = selector;
    set.rt_config_code = RtConfigCode(static_cast<u32>(targets[9]));
    // Surface extents from the first non-null descriptor (00092140.c:144-147, 0x96f50/0x96f60);
    // they feed the viewport/scissor packets (0x91fc0 -> 0x85de40) on the guest side.
    set.width = *reinterpret_cast<const u32*>(first_desc + TargetDescOff::Width);
    set.height = *reinterpret_cast<const u32*>(first_desc + TargetDescOff::Height);
    if (set.width == 0 || set.height == 0 || set.width > 8192 || set.height > 8192) {
        return false; // the +0x198/+0x19c reading is wrong for this descriptor shape
    }
    set.target_mask_cache = RtRead32(pass_addr, RtPassOff::TargetMaskCache);
    set.color_count = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (set.color[i].active) {
            set.color_count = i + 1;
        }
    }
    return true;
}

/// Computes which slots the guest would (re)emit, replicating 0x92140's dirty/pointer-identity
/// cache exactly (00092140.c:149-178): colour slot i rebinds when the force bit (pass+0x8 bit0)
/// is set, dirty bit i of pass+0x10 is set, or the cached pointer pass+0x48+8i differs from the
/// resolved block pointer. Depth (bit 8 = pass+0x11 bit0, aliased into the +0x10 u32) likewise
/// against the +0x164 block pointer.
u32 RtRebindMask(u64 pass_addr, const RtBlockPtrs& blocks) {
    const u8 force = RtRead8(pass_addr, Guest::PassOff::DirtyForce);
    const u32 color_dirty = RtRead32(pass_addr, Guest::PassOff::ColorDirty);
    const u8 depth_dirty = RtRead8(pass_addr, Guest::PassOff::DepthDirty);
    u32 mask = 0;
    for (u32 i = 0; i < 8; ++i) {
        if ((force & 1) != 0 || ((color_dirty >> i) & 1) != 0 ||
            RtRead64(pass_addr, RtPassOff::ColorTargetCache + 8 * i) != blocks.color[i]) {
            mask |= 1u << i;
        }
    }
    if ((force & 1) != 0 || (depth_dirty & 1) != 0 ||
        RtRead64(pass_addr, RtPassOff::DepthTargetCache) != blocks.depth) {
        mask |= 1u << 8;
    }
    return mask;
}

/// Mirrors the guest's cache stores for the committed slots (00092140.c:160-177): cache the
/// resolved block pointer, clear the slot's dirty bit, cache the depth block pointer and clear
/// the depth-dirty bit; then encode the RT-config code into pass+0x8 bits 3-4 (00092140.c:184 —
/// the force bit itself is NOT cleared by 0x92140; it stays for the state writers).
void RtCommitCaches(u64 pass_addr, const TargetSet& set, const RtBlockPtrs& blocks) {
    for (u32 i = 0; i < 8; ++i) {
        if ((set.rebind_mask & (1u << i)) == 0) {
            continue;
        }
        *reinterpret_cast<u64*>(pass_addr + RtPassOff::ColorTargetCache + 8 * i) = blocks.color[i];
        *reinterpret_cast<u32*>(pass_addr + Guest::PassOff::ColorDirty) &= ~(1u << i);
    }
    if ((set.rebind_mask & (1u << 8)) != 0) {
        *reinterpret_cast<u64*>(pass_addr + RtPassOff::DepthTargetCache) = blocks.depth;
        *reinterpret_cast<u8*>(pass_addr + Guest::PassOff::DepthDirty) &= static_cast<u8>(~1u);
    }
    u32* force_word = reinterpret_cast<u32*>(pass_addr + Guest::PassOff::DirtyForce);
    *force_word = (*force_word & 0xffffffe7u) | set.rt_config_code;
}

/// Budget-gated chain trace (the draws' ChainToGuest lives in a closed anonymous namespace
/// above; this is the RT binder's own copy). Always returns false.
bool RtChainToGuest(u64 pass_addr, const char* reason) {
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: %s pass=%#llx guest chain (%s)\n", kTargetsName,
                     static_cast<unsigned long long>(pass_addr), reason);
    }
    return false;
}

/// Whether the native path may commit an RT set. Mirrors the 2.1 ResolveFullscreenPipeline gate:
/// everything above it is derived without touching state; the commit below replaces the guest's
/// PM4 CB_COLORn/DB register emission. TODO(Phase 2.6/2.2): the pass functions (0x93960/0x94600)
/// and the draw objects are still guest-emulated — a native rendering scope opened here would be
/// torn down by the first emulated scheduler.BeginRendering, and the register packets this hook
/// replaces would never reach liverpool (stale render-target registers for every emulated draw).
/// Until 2.6 lands, this stays false and the derivation runs trace-only (SOTC_RENDER_VERBOSE=1).
bool RtEmissionReady() {
    return false;
}

} // namespace

// void f(u64* pass, long* targets) — rdi/rsi, the guest's SysV ABI.
typedef void (*GuestTargetsFn)(u64*, long*) __attribute__((sysv_abi));

/// 0x91fc0: the viewport/scissor packet driver (0x85de40 + PA_SC_WINDOW_SCISSOR) with its own
/// pass-ctx caches (pass+0xb0..0xd8, 00091fc0.c). Engine-side (0x0009xxxx, not the 0x85/86 GNM
/// library), so the native RT path calls it for the viewport part of the bind — the part the RT
/// stage does not replace — exactly like 2.1 reuses the engine's 0x95580/0x14d7b0.
using GuestViewportFn = void (PS4_SYSV_ABI*)(u64*, s32, s32, s32, s32);
constexpr u64 kViewportVa = 0x91fc0; // out/re/decomp/0009/00091fc0.c

extern "C" __attribute__((sysv_abi)) void RenderJob_Targets_92140(u64* pass, long* targets) {
    const u64 pass_addr = reinterpret_cast<u64>(pass);
    Renderer* renderer = Current();
    if (renderer == nullptr || !NativeRtWanted() || pass == nullptr || targets == nullptr ||
        *pass == 0) {
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }

    // -- Derivation (no state touched): descriptor blocks, formats, extents, rebind mask. --
    TargetSet set;
    RtBlockPtrs blocks;
    if (!RtDerive(reinterpret_cast<const u64*>(targets), pass_addr, set, blocks)) {
        RtChainToGuest(pass_addr, "no usable target descriptor");
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }
    set.rebind_mask = RtRebindMask(pass_addr, blocks);
    if (set.rebind_mask == 0) {
        // Fully cached bind: the guest emits no target packets (only the 0x91fc0 viewport cache
        // check + the bits-3-4 store). The guest fast path is cheapest and keeps the viewport
        // caches coherent — chain.
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }
    if (!RtEmissionReady()) {
        // The derivation is traceable; the guest body serves the bind until 2.6/2.2 are native.
        if (Verbose() && TraceAllowed()) {
            std::fprintf(stderr,
                         "SOTC RENDER native: %s pass=%#llx colors=%u depth=%u rebind=%#06x "
                         "sel=%u dims=%ux%u mask=%#x cfg=%u (trace; gate closed)\n",
                         kTargetsName, static_cast<unsigned long long>(pass_addr),
                         set.color_count, set.depth.active ? 1u : 0u, set.rebind_mask,
                         set.selector, set.width, set.height, set.target_mask_cache,
                         set.rt_config_code);
        }
        RtChainToGuest(pass_addr, "pass/draw stages emulated (2.6/2.2 pending)");
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }

    // --- Commit point: nothing above mutated any guest state; everything below mirrors what the
    // guest body would leave behind, then records the native attachment set. ---

    // Attachment views through the port's texture cache (mutates cache state: FindImage /
    // FindRenderTarget / FindDepthTarget — only reachable with the gate open).
    if (!renderer->Passes().ResolveViews(set)) {
        RtChainToGuest(pass_addr, "attachment views unresolved");
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }

    // The guest runs the viewport/scissor packets (0x91fc0) before the slot loop; the RT stage
    // does not replace them, so the engine helper keeps emitting them with its own caches.
    const u64 image_base = Guest::GuestImageBase();
    if (image_base == 0) {
        RtChainToGuest(pass_addr, "guest image unmapped");
        reinterpret_cast<GuestTargetsFn>(Original(0x92140))(pass, targets);
        return;
    }
    reinterpret_cast<GuestViewportFn>(image_base + kViewportVa)(
        pass, 0, 0, static_cast<s32>(set.width), static_cast<s32>(set.height));
    RtCommitCaches(pass_addr, set, blocks);

    // Record the native rendering (Pass::Begin/End around nothing for now — no native draws exist
    // in 2.5; Phase 2.6 keeps the scope open across the pass body). All ops are eLoad/eStore, so
    // the empty scope is a GPU no-op beyond the attachment layouts.
    PassAttachments atts;
    atts.color_count = set.color_count;
    for (u32 i = 0; i < set.color_count; ++i) {
        atts.color[i] = set.color[i].view;
    }
    atts.depth = set.depth.view;
    atts.width = set.width;
    atts.height = set.height;
    atts.color_load = set.load;
    atts.depth_load = set.load;
    atts.color_store = set.store;
    atts.depth_store = set.store;
    atts.depth_read_only = set.depth_read_only;
    atts.clear_depth = set.clear_depth;
    atts.clear_stencil = set.clear_stencil;

    Vulkan::Scheduler& scheduler = renderer->Scheduler();
    scheduler.RecordDeferred([&renderer, atts](vk::CommandBuffer cmdbuf) {
        renderer->Passes().Begin(cmdbuf, atts);
        renderer->Passes().End(cmdbuf);
    });
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: %s pass=%#llx cb=%#llx colors=%u depth=%u "
                     "rebind=%#06x sel=%u dims=%ux%u issued natively\n",
                     kTargetsName, static_cast<unsigned long long>(pass_addr),
                     static_cast<unsigned long long>(*pass), set.color_count,
                     set.depth.active ? 1u : 0u, set.rebind_mask, set.selector, set.width,
                     set.height);
    }
}

// -- Phase 2.7: 0x11d3d0 per-sort-group draw loop (native-cull) ---------------------------------
// ulong f(param_1, param_2, param_3, param_4) — rdi/rsi/rdx/ecx. The guest's own body
// (0011d3d0.c) draws every record of one sort group: the job context is param_3, the record
// vector array is param_2, the group index is param_4, and param_1 is unused by the body.
// Returns the record count ((end - begin) >> 5).
//
// The native replacement lives in gpu/native/native_cull.cpp (the loop is long enough that it
// gets its own translation unit); this callback is the entry shim: it asks the cull module
// whether the group can be issued natively and chains to the guest body otherwise.
typedef u64 (*GuestOccluderFn)(u64, u64, u64*, s32) __attribute__((sysv_abi));

extern "C" __attribute__((sysv_abi)) u64 RenderJob_OccluderLoop_11d3d0(u64 unused, u64 list,
                                                                        u64* ctx, s32 group) {
    Renderer* renderer = Current();
    if (renderer != nullptr) {
        u64 count = 0;
        if (renderer->Culling().GroupNative(list, ctx, group, &count)) {
            return count;
        }
    }
    return reinterpret_cast<GuestOccluderFn>(Original(0x11d3d0))(unused, list, ctx, group);
}

} // namespace

