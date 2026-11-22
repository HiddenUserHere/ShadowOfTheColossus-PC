// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_cull implementation (Phase 2.7). See native_cull.h for the
// contract summary; docs/engine/native-render-map-cull-camera-shadow.md §1/§2 is the authority.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_bind.h"
#include "native/native_cull.h"
#include "native/native_draw.h"
#include "native/native_renderer.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Native {

namespace {

using namespace Guest;

// -- Offsets this loop reads (all proven, cull doc §1.2 + raw disassembly 0x11d3d0..0x11d5a3) ----

// The per-view record-vector array (`list` = ctx[0xc]): group g occupies the qword pair at
// +0x68+0x20*g (begin) / +0x70+0x20*g (end). Records are 0x20 bytes apart.
constexpr u64 kVectorBegin = 0x68; // +0x68+0x20*g: vector begin (proven 11d3fc/11d438)
constexpr u64 kVectorEnd = 0x70;   // +0x70+0x20*g: vector end   (proven 11d401/11d43c)
constexpr u64 kRecordStride = 0x20;

// One 0x20-byte draw record.
constexpr u64 RecDrawObject = 0x08;  // *(rec+8) = rec8, the draw-record object (proven 11d470)
constexpr u64 RecParamDword = 0x14;  // u32 arg to 0x123cc0 (proven 11d4ef: movl 0x14(%r15),%edx)

// The rec8 (draw-record object) fields the loop touches.
constexpr u64 R8IndexBlock = 0x10;  // rec8+0x10 -> 0x123cc0's block arg (proven 11d474)
constexpr u64 R8DrawBlock = 0x20;   // rec8+0x20 -> the indexed-draw block (0x128d20's param)
constexpr u64 R8IndirectRec = 0x28; // rec8+0x28 != 0 -> the indirect branch 0x189060 (11d4ea);
                                    // the same qword is 0x123cc0's inst flag (11d4f9)

// The indexed-draw block at *(rec8+0x20), consumed by 0x128d20 -> 0x94d00 (decomp-settled:
// 00128d20.c passes arg2=mode = lo32 of +0x38, arg5=index_offset = hi32 of +0x40 (= dword at
// +0x44), arg6=index_count = dword at +0x48; 0x94d00's body uses only args 5/6:
// 0x86b040(cb, index_offset, index_count, 0)).
constexpr u64 IdxBlockMode = 0x38;   // lo32: the `mode` int of 0x94d00 (prim-table selector)
constexpr u64 IdxBlockOffset = 0x44; // hi32 of the +0x40 qword: index_offset
constexpr u64 IdxBlockCount = 0x48;  // u32: index_count

constexpr u64 kPrimTableVa = 0x00f9f2e0; // DAT_00f9f2e0: mode -> VGT_PRIMITIVE_TYPE (image offset)
constexpr u64 kRootMaskGlobal = 0x02ed1f54; // DAT_02ed1f54: 0x95320's third arg (image offset)
constexpr u64 kCtxRootTableArg = 0x90;      // ctx+0x90: 0x95320's second arg (pointer into ctx)

// Sanity cap: the guest vectors are CPU-culled draw lists; anything past this is a misread
// structure, not a real frame.
constexpr u64 kMaxRecords = 4096;

inline u8 Read8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 Read32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}
inline u64 Read64(u64 addr, u64 off) {
    return *reinterpret_cast<const u64*>(addr + off);
}
inline void WriteU32(u64 addr, u64 off, u32 v) {
    *reinterpret_cast<u32*>(addr + off) = v;
}

/// A pointer-sized value that could be a real guest heap object (the PC memory model makes guest
/// VAs host pointers). 0 and the boot sentinel (0xffff...ffff) and sub-64K values are never
/// objects — the same envelope the PlausiblePtr guard in native_bind.cpp normalizes.
inline bool PlausiblePtr(u64 p) {
    return p != 0 && p != ~0ull && p >= 0x10000 && p < 0x800000000000ull;
}

/// Budget-gated trace counter, local copy of native_hooks.cpp's pattern (that one lives in a
/// closed anonymous namespace; sharing it would mean editing another section, which the file
/// ownership forbids). Same rationale: bounded writes, never a lock across fprintf.
std::atomic<u64> g_cull_trace_budget{64};

bool Verbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

bool TraceAllowed() {
    return g_cull_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

/// The chain decision, traced when verbose. Always returns false so call sites read as the
/// fallback decision itself (the existing hooks' pattern).
bool ChainToGuest(const char* reason, u64 list_addr, s32 group) {
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr, "SOTC RENDER native: 0x11d3d0 list=%#llx group=%d guest chain (%s)\n",
                     static_cast<unsigned long long>(list_addr), group, reason);
    }
    return false;
}

/// SOTC_NATIVE_CULL: runtime A/B for the Phase 2.7 group-loop replacement. Default on, "0" =
/// pure guest chain (same semantics as SOTC_NATIVE_CLEAR/DRAW/INDIRECT/RT).
struct CullSwitchInit {};
bool g_native_cull_wanted = true;

void InitCullSwitch() {
    static const CullSwitchInit init = [] {
        const char* e = std::getenv("SOTC_NATIVE_CULL");
        g_native_cull_wanted = !e || e[0] != '0';
        return CullSwitchInit{};
    }();
    (void)init;
}

// -- Guest engine helpers (SysV, callable at image+va per the PC memory model) -------------------
// Only ENGINE helpers are reused (0x9xxxx/0xaxxxxx/0xfxxxx/0x12xxxx/0x14xxxx); the 0x85xxxx/
// 0x86xxxx GNM library is bypassed by the native backend (docs/engine/native-render-seam.md) —
// its calls are the specification, not work. The loop's staging helpers ARE engine code: the
// guest body calls them per record, and the native commit path calls the identical functions so
// the DCB state (index binds, root tables) stays coherent for the rest of the frame.
using GuestEnsureLoadedFn = void (PS4_SYSV_ABI*)(void* slot);          // 0x95580
using GuestResolveFn = void* (PS4_SYSV_ABI*)(void* lazy);              // 0x14d7b0
using GuestBindRecordsFn = void (PS4_SYSV_ABI*)(void* mat, void* ctx); // 0xf8c90
using GuestInstBindFn = void (PS4_SYSV_ABI*)(void* block, void* ctx, u32 param, u32 inst_flag); // 0x123cc0
using GuestRootTableFn = void* (PS4_SYSV_ABI*)(u32 mask, void* ctx_arg, u32 global);            // 0x95320
using GuestRootBindFn = void (PS4_SYSV_ABI*)(void* cb, void* table);   // 0xae150

constexpr u64 kEnsureLoadedVa = 0x95580;
constexpr u64 kResolveVa = 0x14d7b0;
constexpr u64 kBindRecordsVa = 0xf8c90;
constexpr u64 kInstBindVa = 0x123cc0;
constexpr u64 kRootTableVa = 0x95320;
constexpr u64 kRootBindVa = 0xae150;

/// Resolves the vk::Buffer the indirect arguments live in (guest heap address). Mirrors the
/// native_hooks.cpp stub: null until the descriptors phase routes guest VAs through the port's
/// buffer cache, so indirect-path records chain to the guest body.
vk::Buffer ResolveArgsBuffer(const void* args, u64 size) {
    (void)args;
    (void)size;
    return VK_NULL_HANDLE;
}

/// One derived leaf draw (pass A output, read-only).
struct DerivedDraw {
    u64 rec8;
    bool indirect;
    u32 mode;         // indexed path: lo32 of the draw block's +0x38; indirect: 0 (0x94bf0's arg)
    u32 index_offset;
    u32 index_count;
    u32 inst_param;   // *(u32*)(rec+0x14), 0x123cc0's third arg
    u32 inst_flag;    // (rec8+0x28) != 0
    u64 inst_block;   // *(rec8+0x10)
    vk::Buffer args_buffer; // indirect path only: the resolved DRAW_INDEX_INDIRECT args
    VAddr args_va{0};       // indirect path only: the guest args table address
};

/// Pass A: the read-only structural walk (cull doc §1.2 steps 1-4, pointers only — no guest
/// calls, no writes). Returns false after tracing why the group cannot go native.
bool DeriveGroup(u64 list_addr, u64 P, s32 group, u64 count, DerivedDraw* out) {
    const u64 begin = Read64(list_addr, kVectorBegin + 0x20 * group);
    // The draw-object gate 0x1222e0 tests: rax = [ctx] = P; test byte [rax+8], 4 (0x1222ed..f4).
    const u8 gate = Read8(P, PassOff::DirtyForce); // pass+0x8 force/dirty qword byte 0
    for (u64 i = 0; i < count; ++i) {
        const u64 rec = begin + i * kRecordStride;
        const u64 rec8 = Read64(rec, RecDrawObject);
        if (!PlausiblePtr(rec8)) {
            return ChainToGuest("record object implausible", list_addr, group);
        }
        DerivedDraw& d = out[i];
        d.rec8 = rec8;
        d.inst_block = Read64(rec8, R8IndexBlock);
        d.inst_param = Read32(rec, RecParamDword);
        d.inst_flag = Read64(rec8, R8IndirectRec) != 0;
        // 0x123cc0 is called for EVERY record (0011d3d0.c: the call sits outside the material
        // branch), so its block argument must be nameable regardless of the draw path.
        if (!PlausiblePtr(d.inst_block)) {
            return ChainToGuest("instance/index bind block implausible", list_addr, group);
        }
        // The indirect branch (0x1222f6..0x189060) is taken when the record's indirect slot is
        // NON-NULL: 0x1222f6 loads [rec8+0x28] into rdi and 0x122313 tests it against 0; the
        // NULL case falls through to the indexed path.
        const u64 indirect_rec = Read64(rec8, R8IndirectRec);
        if ((gate & 4) == 0 && indirect_rec != 0) {
            // The indirect branch (0x189060 -> 0xdb620 DrawIndexIndirect). The args buffer is
            // not nameable until the descriptors phase resolves guest VAs through the port's
            // buffer cache — gate now, before any staging side effect.
            d.args_buffer =
                ResolveArgsBuffer(reinterpret_cast<const void*>(indirect_rec + 0x38), 20);
            if (d.args_buffer == VK_NULL_HANDLE) {
                return ChainToGuest("indirect args buffer unresolved", list_addr, group);
            }
            d.indirect = true;
            d.args_va = reinterpret_cast<VAddr>(indirect_rec + 0x38);
            d.mode = 0; // 0x189060 calls 0x94bf0(P, 0): primitive for mode 0
            d.index_offset = 0;
            d.index_count = 0;
            continue;
        }
        const u64 block = Read64(rec8, R8DrawBlock);
        if (!PlausiblePtr(block)) {
            return ChainToGuest("indexed-draw block implausible", list_addr, group);
        }
        d.indirect = false;
        d.mode = Read32(block, IdxBlockMode);
        d.index_offset = Read32(block, IdxBlockOffset);
        d.index_count = Read32(block, IdxBlockCount);
        d.args_buffer = VK_NULL_HANDLE;
    }
    return true;
}

/// The commit path of one record: the guest's own staging helpers via SysV (identical functions
/// the guest body calls — the DCB keeps its state packets, so the rest of the frame stays
/// coherent), then the pipeline resolve from the freshly staged block. Returns null (with the
/// reason traced) while the pipeline for this record's material cannot be named.
///
/// Three pointer levels, proven by 0011d3d0.c and 0x11d3d0's raw disassembly:
///   ctx (this loop's param_3)  -> 0xf8c90 / 0x123cc0 / 0x95320 (ctx+0x90)
///   P = *ctx (the pass ctx)    -> 0xae150, and the +0x8 / +0x18 / +0x1cc caches
///   cb = *P  (the device)      -> the material block at cb+0x2e8
/// `prim_out` receives the primitive type the guest's 0x94d00/0x94bf0 would write to P+0x1cc.
const Vulkan::Pipeline* StageAndResolve(u64 list_addr, s32 group, u64 ctx, u64 P, u64 cb, u64 base,
                                        const DerivedDraw& d, u64& prev_material, u32& prim_out) {
    // 0x95580(rec8): ensure the record slot is loaded (the guest body calls it every record).
    reinterpret_cast<GuestEnsureLoadedFn>(base + kEnsureLoadedVa)(
        reinterpret_cast<void*>(d.rec8));
    // R' = 0x14d7b0(*rec8): the lazy material resolve.
    const u64 lazy = Read64(d.rec8, 0);
    if (!PlausiblePtr(lazy)) {
        ChainToGuest("material lazy slot implausible", list_addr, group);
        return nullptr;
    }
    const u64 material = reinterpret_cast<u64>(
        reinterpret_cast<GuestResolveFn>(base + kResolveVa)(reinterpret_cast<void*>(lazy)));
    if (!PlausiblePtr(material)) {
        return nullptr;
    }
    // Material change (cull doc §1.2 step 3): the guest skips rebinding when the material is the
    // same as the previous record's and already bound; mirror the skip. Markers (push/pop,
    // gated on ctx[9]+0x28 & 4) are debug PM4 — the native path omits them and their pops.
    if (material != prev_material) {
        reinterpret_cast<GuestBindRecordsFn>(base + kBindRecordsVa)(
            reinterpret_cast<void*>(material), reinterpret_cast<void*>(ctx));
        prev_material = material;
    }
    // Instance/index/stencil bind + root table (cull doc §1.2 steps 4-5): every record.
    reinterpret_cast<GuestInstBindFn>(base + kInstBindVa)(
        reinterpret_cast<void*>(d.inst_block), reinterpret_cast<void*>(ctx), d.inst_param,
        d.inst_flag ? 1u : 0u);
    const u32 root_mask = Read32(material, 0x468); // R'+0x468 root-table mask (proven 11d508)
    void* table = reinterpret_cast<GuestRootTableFn>(base + kRootTableVa)(
        root_mask, reinterpret_cast<void*>(ctx + kCtxRootTableArg), Read32(base, kRootMaskGlobal));
    reinterpret_cast<GuestRootBindFn>(base + kRootBindVa)(reinterpret_cast<void*>(P), table);

    // The pipeline seam: derive the content key from the freshly staged material block exactly
    // like ResolveDrawPipelineForPass does (public Bind composition), then look it up. A miss
    // here chains the WHOLE group before any native draw was queued — the guest body then reruns
    // from its entry, re-staging identical state (idempotent) and emitting its own draws. That
    // rerun is what keeps the standing rule intact: nothing above made the guest skip its own
    // emission; the staged state it would rebuild is byte-identical.
    Bind::MaterialSnapshot snap{};
    if (!Bind::DeriveSnapshot(cb, snap)) {
        return nullptr;
    }
    for (u32 s = 0; s < Bind::kNumStages; ++s) {
        const Bind::Gate gate = Bind::ValidateStage(snap.stage[s]);
        if (gate != Bind::Gate::None) {
            if (Verbose() && TraceAllowed()) {
                std::fprintf(stderr, "SOTC RENDER native: 0x11d3d0 stage gate (%s)\n",
                             Bind::GateName(gate));
            }
            return nullptr;
        }
    }
    // Fixed function: the primitive comes from THIS record's mode (0x94d00's prim-table rule,
    // 00094d00.c:10-20) — the indirect branch forces mode 0 (0x189060 -> 0x94bf0(P, 0)). The
    // pass's cached +0x1cc is the *result* of the previous record, not an input.
    const bool tess_prim = (Read8(P, PassOff::StageDescriptor) & 6) != 0;
    const Bind::FixedFunctionState ff =
        Bind::DeriveFixedFunction(P, d.indirect ? 0u : d.mode, tess_prim);
    if (!ff.ok) {
        return nullptr;
    }
    prim_out = ff.primitive_type;
    return Bind::Lookup(Bind::HashKey(Bind::BuildKey(snap, ff)));
}

} // namespace

bool NativeCullWanted() {
    InitCullSwitch();
    return g_native_cull_wanted;
}

bool Cull::GroupNative(u64 list_addr, u64* ctx, s32 group, u64* count_out) {
    using namespace Guest;
    if (!NativeCullWanted()) {
        return false;
    }
    Renderer* renderer = Current();
    if (renderer == nullptr) {
        return false; // hooks not armed (same silent case as the draw hooks' Current() gate)
    }
    if (ctx == nullptr) {
        return ChainToGuest("null ctx", list_addr, group);
    }
    // Pointer levels (proven 0011d3d0.c:13 `puVar2 = *param_3` and raw disassembly):
    //   ctx        the loop's job context (param_3) — 0xf8c90/0x123cc0/0x95320's second arg
    //   P = *ctx   the pass context (0x11d430 saves it; 0xae150's param_1; 0x1222e0's
    //              `*param_2`; 0x94d00's param_1 — its +0x1cc primitive cache and +0x18 stage
    //              descriptor)
    //   cb = *P    the device / GNM emitter (0x11d5xx `0x86e800(*puVar2)`; 0x94d00 reads the
    //              material block at cb+0x2e8)
    const u64 P = *ctx;
    if (!PlausiblePtr(P)) {
        return ChainToGuest("pass context implausible", list_addr, group);
    }
    const u64 cb = Read64(P, 0);
    if (!PlausiblePtr(cb)) {
        return ChainToGuest("no device context", list_addr, group);
    }
    const u64 ctx_addr = reinterpret_cast<u64>(ctx);
    if (group < 0 || group >= kGroupCount) {
        return ChainToGuest("group out of range", list_addr, group);
    }
    const u64 base = GuestImageBase();
    if (base == 0) {
        return ChainToGuest("guest image not mapped", list_addr, group);
    }
    const u64 begin = Read64(list_addr, kVectorBegin + 0x20 * group);
    const u64 end = Read64(list_addr, kVectorEnd + 0x20 * group);
    if (!PlausiblePtr(begin) || !PlausiblePtr(end) || end < begin ||
        ((end - begin) % kRecordStride) != 0) {
        return ChainToGuest("record vector implausible", list_addr, group);
    }
    const u64 count = (end - begin) / kRecordStride;
    if (count == 0) {
        // The guest body early-returns 0 on an empty vector (0011d3d0.c:19-23): no markers, no
        // draws, and the trailing SET_PREDICATION disable (0x11d570) never runs. The port
        // ignores that disable anyway (liverpool.cpp:1558), so "issued nothing" is exact.
        *count_out = 0;
        return true;
    }
    if (count > kMaxRecords) {
        return ChainToGuest("group too large", list_addr, group);
    }
    // N2-10 gate: while nothing fills the pipeline map, every lookup would miss and the staging
    // below would be pure overhead on a hot path. One cheap atomic read skips it; the map
    // filling is what activates the native path.
    if (Bind::StatsSnapshot().distinct == 0) {
        return ChainToGuest("pipeline map empty (N2-10 fill side pending)", list_addr, group);
    }

    // Pass A (read-only): derive every leaf draw before touching anything. The arrays live on the
    // heap: 4096 records x (DerivedDraw ~56 B + pointer 8 B) is ~256 KiB, far too much for the
    // render threads' stacks.
    std::vector<DerivedDraw> draws(count);
    if (!DeriveGroup(list_addr, P, group, count, draws.data())) {
        return false; // reason already traced
    }

    // Pass B1 (staging + pipeline resolve): guest engine helpers via SysV per record; the
    // pipeline gate runs before any native draw is queued, so a mid-group miss still chains
    // cleanly (the guest rerun redraws everything; no native draw exists yet).
    u64 prev_material = ~0ull;
    std::vector<const Vulkan::Pipeline*> pipelines(count);
    std::vector<u32> prims(count);
    for (u64 i = 0; i < count; ++i) {
        pipelines[i] = StageAndResolve(list_addr, group, ctx_addr, P, cb, base, draws[i],
                                       prev_material, prims[i]);
        if (pipelines[i] == nullptr) {
            return ChainToGuest("pipeline unresolved (record staged)", list_addr, group);
        }
    }

    if (NativeEmitWanted()) {
        // The cutover ladder: the ports (target state, descriptors, user data, viewport/scissor,
        // binds) are the emulated draw's own path; every record's counts and offsets are the
        // guest's. The pipelines above came from the material content key.
        for (u64 i = 0; i < count; ++i) {
            const DerivedDraw& d = draws[i];
            Draw::DrawRequest request{};
            // This record's own image: the shared guest register block is handed to the request by
            // value and the native path copies it synchronously, so the loop must not hand out a
            // pointer it mutates right after (it no longer does: IssueFromRegs copies up front).
            request.regs = &renderer->Draws().GuestRegs();
            request.pipeline = pipelines[i];
            request.is_indexed = !d.indirect;
            WriteU32(P, PassOff::PrimitiveType, prims[i]); // the guest's cache write, in order
            if (d.indirect) {
                // The args table is the guest's own record (a loader-bias guest pointer that the
                // buffer cache names): the indirect path already gated on it in pass A.
                request.args = d.args_va;
                request.args_size = 20;
            } else {
                request.index_offset = d.index_offset;
                request.num_indices = d.index_count;
            }
            if (!renderer->Draws().IssueFromRegs(request)) {
                return ChainToGuest("draw emission unavailable", list_addr, group);
            }
        }
        *count_out = count;
        return true;
    }

    // Pass B2 (commit): no gate can fail past this point. The trailing SET_PREDICATION disable
    // (0x11d570 -> 0x86e800) is mirrored as the documented no-op it is (liverpool.cpp:1558
    // ignores it — the only live predication packet in the build, address 0).
    Vulkan::Scheduler& scheduler = renderer->Scheduler();
    for (u64 i = 0; i < count; ++i) {
        // Primitive-type cache (00094d00.c:10-20, 00094bf0.c:8-19): the guest writes the derived
        // value before the draw. The native path emits no packet, so mirror the cache write —
        // the draws after this loop consult it. Done in record order; the draws are deferred.
        WriteU32(P, PassOff::PrimitiveType, prims[i]);
        const DerivedDraw d = draws[i]; // by value: the lambda outlives this frame's stack
        const Vulkan::Pipeline* pipeline = pipelines[i];
        scheduler.RecordDeferred([&renderer, pipeline, d](vk::CommandBuffer cmdbuf) {
            renderer->Draws().BindPipeline(cmdbuf, *pipeline);
            if (d.indirect) {
                if (d.args_buffer != VK_NULL_HANDLE) {
                    renderer->Draws().IndexedIndirect(cmdbuf, d.args_buffer, 0, 20);
                }
                return;
            }
            renderer->Draws().IndexedOffset(cmdbuf, d.index_count, d.index_offset);
        });
    }
    *count_out = count; // the guest's own return value: (end - begin) >> 5
    if (Verbose() && TraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: 0x11d3d0 list=%#llx group=%d issued %llu records "
                     "natively\n",
                     static_cast<unsigned long long>(list_addr), group,
                     static_cast<unsigned long long>(count));
    }
    return true;
}

} // namespace Native
