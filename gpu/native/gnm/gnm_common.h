// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary (plan "100% native in one phase").
//
// Every GPU command the game produces passes through one of two boundaries:
//   1. the eboot's GNM emitter functions (VA 0x85xxxx-0x88xxxx) that write PM4 into a GNM command
//      buffer object: replaced by entry detours listed in gpu/sotc/hook_gnm_wNN.inc;
//   2. the sceGnm* driver functions, which are HOST code in this port
//      (gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp): each calls the matching pointer of
//      Native::Gnm::Table first and skips its PM4 when the pointer returns true.
// A replacement emits a native op (Native::StreamOps) instead of PM4. State and submission ops
// run inline on the command processor thread (Where::CommandProcessor) through the Liverpool
// native facade (liverpool.h "sotc: native backend facade"); Vulkan-only work may run on the draw
// thread. Twelve worker files gpu/native/gnm/wNN_*.cpp own one area each and fill the table in
// their RegisterWNN().
//
// Class switch: SOTC_NATIVE_GNM is a comma list of the classes below ("all" = every class). A
// class that is off makes its detours call the guest body and its table entries return false,
// so each class can be bisected at runtime.
#pragma once

#include <array>
#include <cstdint>

#include "common/types.h"
#include "common/unique_function.h"
#include "native/native_stack.h"
#include "native/native_stats.h"
#include "native/native_stream_ops.h"

namespace AmdGpu {
class Liverpool;
}

namespace Native::Gnm {

enum class Class : u32 {
    Cb,       ///< W01 context regs: colour / blend
    Db,       ///< W02 context regs: depth / stencil
    Raster,   ///< W03 context regs: raster / viewport / other
    Targets,  ///< W04 render targets
    Uconfig,  ///< W05 uconfig / index / predication / config / default hw state
    Memory,   ///< W06 WRITE_DATA / DMA_DATA
    Sync,     ///< W07 EOP / EOS / WAIT_REG_MEM / ACQUIRE_MEM / EVENT_WRITE / labels
    Shaders,  ///< W08 shader binds and user data
    Draws,    ///< W09 draws
    Dispatch, ///< W10 dispatches and compute queues
    Ce,       ///< W11 constant engine, misc opcodes, markers
    Submit,   ///< W12 submit / flip
    Ib,       ///< W13 INDIRECT_BUFFER (native nested buffers)
    Count,
};

/// Whether `c` is on (SOTC_NATIVE_GNM). Cheap after the first call.
bool Enabled(Class c);

/// SOTC_NATIVE_GNM_FALLBACK: comma list of guest VAs (hex) whose detours stay installed but always
/// take the guest body (bisects the detour mechanics against the native op of one site).
bool ForceFallback(u64 va);

/// The command processor (call only from inside a Where::CommandProcessor op).
AmdGpu::Liverpool& Lv();

/// Stats slot of a boundary site (call once per site: `static const u32 slot = Site("...")`).
inline u32 Site(const char* name) {
    return Stats::RegisterSite(name);
}

/// The guest body of a detoured emitter (for fallback when the class is off or the native path
/// declines). Cast to the emitter's own SysV signature.
void* GuestBody(u64 va);
template <typename Fn>
Fn Guest(u64 va) {
    return reinterpret_cast<Fn>(GuestBody(va));
}
/// A guest function that is NOT detoured, called at its own entry (image + va): no child
/// trampoline (gap N2-81). Use it for guest helpers a replacement calls (predicates, decoders).
void* GuestEntry(u64 va);
template <typename Fn>
Fn GuestDirect(u64 va) {
    return reinterpret_cast<Fn>(GuestEntry(va));
}

/// Emits a state/submit op for the command processor, placed in the GNM command buffer object
/// `cb` (boundary 1).
inline bool EmitCp(u64 cb, Common::UniqueFunction<void>&& op, bool writes_memory = false) {
    return StreamOps::Emit(cb, std::move(op), writes_memory, StreamOps::Where::CommandProcessor);
}
/// The same into a sceGnm* fixed slot (boundary 2).
inline bool EmitCpInSlot(u32* slot, u32 size_dwords, Common::UniqueFunction<void>&& op,
                         bool writes_memory = false) {
    return StreamOps::EmitInSlot(slot, size_dwords, std::move(op), writes_memory,
                                 StreamOps::Where::CommandProcessor);
}

/// Prefetch shadow (gap N2-88). The command processor's look-ahead walker
/// (Liverpool::PrefetchComputeShaders) starts compute shader translations a whole buffer ahead from
/// the SET_SH_REG / DISPATCH packets it sees; a native op is opaque to it. A shadow is a copy of the
/// replaced compute packets carried in the op's NOP body after a tag dword (StreamOps
/// kShadowTag): the walker follows it, the command processor skips the body as for any NOP. Only
/// packets that touch the compute state (SH offsets 0x200..0x200 + sizeof(ComputeProgram)/4) and
/// dispatches are recorded.
struct Shadow {
    static constexpr u32 kMaxDwords = 60;
    std::array<u32, kMaxDwords> dw{};
    u32 n = 0;

    static bool CsRange(u32 off, u32 count);
    /// One SET_SH_REG (offset relative to the SH base, as the packet carries it).
    Shadow& Sh(u32 off, const u32* vals, u32 count);
    Shadow& DispatchDirect(u32 x, u32 y, u32 z, u32 initiator);
    Shadow& DispatchIndirect(u32 data_offset, u32 initiator);
};

/// EmitCp / EmitCpInSlot with a prefetch shadow (an empty shadow is a plain emit; a slot too small
/// for it keeps the op without the shadow).
bool EmitCpShadow(u64 cb, Common::UniqueFunction<void>&& op, const Shadow& shadow,
                  bool writes_memory = false);
bool EmitCpInSlotShadow(u32* slot, u32 size_dwords, Common::UniqueFunction<void>&& op,
                        const Shadow& shadow, bool writes_memory = false);

/// Self-modifying compute command buffers (liverpool.cpp ProcessCompute DmaData: a 12-byte
/// memory->memory DMA whose destination is the dim_x..dim_z of a following DISPATCH_DIRECT in the
/// same buffer turns that dispatch into an indirect one, `indirect_patches`). Once W10 replaces the
/// dispatch with a native op, that destination is the op's NOP (dst - 4 = NOP header, dst = the
/// SotcNativeOp payload, dst + 4 = the op id): W06's native DMA must then NOT copy, and record the
/// patch here instead, keyed by that op id; W10's dispatch op takes it and dispatches indirectly
/// from `src`. Both run inline on the command processor, in stream order (DMA first).
void SetIndirectPatch(u32 op_id_dword, VAddr src);
/// Returns true and the source address when op `op_id_dword` has a pending patch (and clears it).
bool TakeIndirectPatch(u32 op_id_dword, VAddr& src);

/// Boundary 2: the sceGnm* replacement table. Each pointer, when set and returning true, has
/// written the whole slot (normally one EmitCpInSlot) and the PM4 path is skipped. Signatures are
/// the gnmdriver.cpp ones plus, for functions returning a value, an out parameter.
struct Table {
    // W09 draws
    bool (*DrawIndex)(u32* cmdbuf, u32 size, u32 index_count, uintptr_t index_addr, u32 flags,
                      u32 type) = nullptr;
    bool (*DrawIndexAuto)(u32* cmdbuf, u32 size, u32 index_count, u32 flags) = nullptr;
    bool (*DrawIndexOffset)(u32* cmdbuf, u32 size, u32 index_offset, u32 index_count,
                            u32 flags) = nullptr;
    bool (*DrawIndirect)(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage,
                         u32 vertex_sgpr_offset, u32 instance_sgpr_offset, u32 flags) = nullptr;
    bool (*DrawIndexIndirect)(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage,
                              u32 vertex_sgpr_offset, u32 instance_sgpr_offset,
                              u32 flags) = nullptr;
    bool (*DrawIndirectMulti)(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                              u32 shader_stage, u32 vertex_sgpr_offset, u32 instance_sgpr_offset,
                              u32 flags) = nullptr;
    bool (*DrawIndexIndirectMulti)(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                                   u32 shader_stage, u32 vertex_sgpr_offset,
                                   u32 instance_sgpr_offset, u32 flags) = nullptr;
    bool (*DrawIndexIndirectCountMulti)(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                                        u64 count_addr, u32 shader_stage, u32 vertex_sgpr_offset,
                                        u32 instance_sgpr_offset, u32 flags) = nullptr;
    // Round 3: context default state (returns the dwords written; variant 0 or 400) and the
    // compute wait on an address (ComputeWaitOnAddress: WAIT_REG_MEM on the ASC ring).
    bool (*DrawInitToDefaultContextState)(u32* cmdbuf, u32 size, u32 variant, u32* written) =
        nullptr;
    bool (*ComputeWaitOnAddress)(u32* cmdbuf, u32 size, uintptr_t addr, u32 mask, u32 cmp_func,
                                 u32 ref) = nullptr;
    // W10 dispatch
    bool (*DispatchDirect)(u32* cmdbuf, u32 size, u32 threads_x, u32 threads_y, u32 threads_z,
                           u32 flags) = nullptr;
    bool (*DispatchIndirect)(u32* cmdbuf, u32 size, u32 data_offset, u32 flags) = nullptr;
    bool (*DispatchIndirectOnMec)(u32* cmdbuf, u32 size, VAddr args, u32 modifier) = nullptr;
    bool (*DispatchInitDefaultHardwareState)(u32* cmdbuf, u32 size, u32* written) = nullptr;
    // W08 shaders
    bool (*SetVsShader)(u32* cmdbuf, u32 size, const u32* vs_regs, u32 shader_modifier) = nullptr;
    bool (*SetEsShader)(u32* cmdbuf, u32 size, const u32* es_regs, u32 shader_modifier) = nullptr;
    bool (*SetLsShader)(u32* cmdbuf, u32 size, const u32* ls_regs, u32 shader_modifier) = nullptr;
    bool (*SetHsShader)(u32* cmdbuf, u32 size, const u32* hs_regs, u32 param4) = nullptr;
    bool (*SetGsShader)(u32* cmdbuf, u32 size, const u32* gs_regs) = nullptr;
    bool (*SetPsShader)(u32* cmdbuf, u32 size, const u32* ps_regs) = nullptr;
    bool (*SetPsShader350)(u32* cmdbuf, u32 size, const u32* ps_regs) = nullptr;
    bool (*SetCsShader)(u32* cmdbuf, u32 size, const u32* cs_regs) = nullptr;
    bool (*SetCsShaderWithModifier)(u32* cmdbuf, u32 size, const u32* cs_regs,
                                    u32 modifier) = nullptr;
    bool (*UpdateVsShader)(u32* cmdbuf, u32 size, const u32* vs_regs,
                           u32 shader_modifier) = nullptr;
    bool (*UpdateGsShader)(u32* cmdbuf, u32 size, const u32* gs_regs) = nullptr;
    bool (*UpdateHsShader)(u32* cmdbuf, u32 size, const u32* hs_regs, u32 ls_hs_config) = nullptr;
    bool (*UpdatePsShader)(u32* cmdbuf, u32 size, const u32* ps_regs) = nullptr;
    bool (*UpdatePsShader350)(u32* cmdbuf, u32 size, const u32* ps_regs) = nullptr;
    bool (*SetEmbeddedVsShader)(u32* cmdbuf, u32 size, u32 shader_id,
                                u32 shader_modifier) = nullptr;
    bool (*SetEmbeddedPsShader)(u32* cmdbuf, u32 size, u32 shader_id,
                                u32 shader_modifier) = nullptr;
    // W05 default hardware state (returns the dwords written)
    bool (*DrawInitDefaultHardwareState)(u32* cmdbuf, u32 size, u32 variant, u32* written) =
        nullptr; ///< variant: 0 = base, 175, 200, 350
    /// The per-frame init packet of the submit path (variant 0, 200, 350): writes a native op into
    /// `slot` (>= 3 dwords) that the caller submits in place of the PM4 InitSequence table.
    bool (*SubmitInitPacket)(u32 variant, u32* slot, u32 slot_dwords) = nullptr;
    // W11 markers
    bool (*InsertPushMarker)(u32* cmdbuf, u32 size, const char* marker) = nullptr;
    bool (*InsertPushColorMarker)(u32* cmdbuf, u32 size, const char* marker, u32 color) = nullptr;
    bool (*InsertPopMarker)(u32* cmdbuf, u32 size) = nullptr;
    bool (*InsertSetMarker)(u32* cmdbuf, u32 size, const char* marker) = nullptr;
    bool (*InsertDingDongMarker)(u32* cmdbuf, u32 size) = nullptr;
    // W12 submit / flip
    bool (*InsertWaitFlipDone)(u32* cmdbuf, u32 size, s32 vo_handle, u32 buf_idx) = nullptr;
    bool (*SubmitCommandBuffersForWorkload)(u32 workload, u32 count, const u32* dcb_gpu_addrs[],
                                            u32* dcb_sizes_in_bytes, const u32* ccb_gpu_addrs[],
                                            u32* ccb_sizes_in_bytes, s32* result) = nullptr;
    bool (*SubmitAndFlipCommandBuffersForWorkload)(u32 workload, u32 count, u32* dcb_gpu_addrs[],
                                                   u32* dcb_sizes_in_bytes, u32* ccb_gpu_addrs[],
                                                   u32* ccb_sizes_in_bytes, u32 vo_handle,
                                                   u32 buf_idx, u32 flip_mode, s64 flip_arg,
                                                   s32* result) = nullptr;
    bool (*SubmitDone)(s32* result) = nullptr;
    /// The flip tail of SubmitAndFlip (64 dwords at the end of the last DCB): writes one native op
    /// doing the VO lock write, the GfxFlip signal and the label write. backup = the tail's first 7
    /// dwords before the rewrite (payload in [1]). False keeps PatchFlipRequest's PM4 rewrite.
    bool (*FlipTail)(u32* tail, u32 tail_dwords, u64 lock_addr, const u32* backup) = nullptr;
};

/// The table, filled once (thread-safe) by RegisterW01..RegisterW12.
const Table& GetTable();

// One per worker file; each fills only its own table entries.
void RegisterW01(Table& t);
void RegisterW02(Table& t);
void RegisterW03(Table& t);
void RegisterW04(Table& t);
void RegisterW05(Table& t);
void RegisterW06(Table& t);
void RegisterW07(Table& t);
void RegisterW08(Table& t);
void RegisterW09(Table& t);
void RegisterW10(Table& t);
void RegisterW11(Table& t);
void RegisterW12(Table& t);

} // namespace Native::Gnm
