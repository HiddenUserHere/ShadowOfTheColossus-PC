// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W09: Draws.
// Class Draws (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w09_draws.h, and
// gpu/sotc/hook_gnm_w09.inc. See gnm_common.h for the contract.
//
// Boundary 2 only: each function below is a Native::Gnm::Table entry that gnmdriver.cpp calls
// before its PM4 path. The table entries run on the guest thread while the guest builds the
// command buffer, so they validate the arguments exactly as the gnmdriver function does, decide
// the Neo-mode draw_initiator value there (sceKernelIsNeoMode is read at emission time, as the
// gnmdriver function does), and capture plain values into the native op. The op runs inline on
// the command processor (Where::CommandProcessor) and does what the PM4 handler does for the
// packet, through the Liverpool native facade.
//
// Predication: the PM4 header predicate bit (flags & 1 on the indirect packets) is not consulted
// by the command processor's draw handlers (liverpool.cpp ProcessGraphics has no predicate test),
// so the native ops ignore it as the PM4 path does.
//
// Debug markers: the handlers wrap each draw in rasterizer->ScopeMarker (a RenderDoc label that
// includes the packet address). The native ops do not emit that label: no guest-visible effect.
//
// Coverage table (boundary: gnmdriver entry | PM4 contract (gnmdriver.cpp + liverpool.cpp handler)
// | native op | evidence):
//   sceGnmDrawIndex (Table::DrawIndex) | size 10, index_addr != 0 and even, flags & 0x1ffffffe == 0;
//     DrawIndex2 writes max_size=index_count, index base lo/hi, index_count, draw_initiator
//     (Neo ? flags & 0xe0000000 : 0); handler sets regs.max_index_size, index_base_address,
//     num_indices, draw_initiator (MarkRegs each), then SubmitDraw(indexed, 0) if rasterizer
//     | native: same regs writes + NativeSubmitDraw(true, 0) | evidence: gnmdriver.cpp:475-498,
//     liverpool.cpp:1598-1621; raw bytes not checked (values come from the same struct fields
//     the handler reads, not hand-written packet bytes); REA not used
//   sceGnmDrawIndexAuto (Table::DrawIndexAuto) | size 7, flags & 0x1ffffffe == 0; DrawIndexAuto
//     body {index_count, draw_initiator = Neo ? flags & 0xe0000000 | 2 : 2}; handler sets
//     num_indices, draw_initiator, then SubmitDraw(false, 0) | native: same | evidence:
//     gnmdriver.cpp:500-516, liverpool.cpp:1645-1662
//   sceGnmDrawIndexOffset (Table::DrawIndexOffset) | size 9; DrawIndexOffset2 body {index_count
//     as max_size, index_offset, index_count, draw_initiator (Neo ? flags & 0xe0000000 : 0)};
//     handler sets max_index_size, num_indices, draw_initiator, then SubmitDraw(true,
//     index_offset) | native: same | evidence: gnmdriver.cpp:624-643, liverpool.cpp:1622-1644
//   sceGnmDrawIndirect (Table::DrawIndirect) | size 9, shader_stage < Max, both sgpr offsets < 0x10;
//     body {data_offset, vtx_sgpr (0 or (off & 0xffff) + stage base), inst_sgpr (same), auto-index
//     dword}; handler (no register writes) reads base_vtx_loc / start_inst_loc (low 16 bits of the
//     two sgpr dwords), then SubmitDrawIndirect(false, indirect_args_addr, data_offset,
//     sizeof(AmdGpu::DrawIndirectArgs), 1, 0, ...) | native: same, base read at run time from
//     NativeIndirectArgsAddr() | evidence: gnmdriver.cpp:645-672, liverpool.cpp:1664-1681
//   sceGnmDrawIndexIndirect (Table::DrawIndexIndirect) | same checks as DrawIndirect; body adds
//     draw_initiator (Neo ? flags & 0xe0000000 : 0); handler SubmitDrawIndirect(true, ..., stride
//     sizeof(AmdGpu::DrawIndexedIndirectArgs), 1, 0, ...) | native: same | evidence: gnmdriver.cpp:518-546,
//     liverpool.cpp:1703-1721
//   sceGnmDrawIndirectMulti (Table::DrawIndirectMulti) | size 11, shader_stage < Max, sgpr < 0x10;
//     body {data_offset, vtx_sgpr, inst_sgpr, max_count, sizeof(AmdGpu::DrawIndirectArgs), auto-index};
//     handler SubmitDrawIndirect(false, ..., data_offset, stride, count=max_count, 0, ...) |
//     native: same | evidence: gnmdriver.cpp:680-708, liverpool.cpp:1683-1701
//   sceGnmDrawIndexIndirectCountMulti (Table::DrawIndexIndirectCountMulti) | size 16, the Neo
//     compat gate (`!Neo || !UseNeoCompatSequences`, constant false, so always true), sgpr < 0x10,
//     shader_stage in {Vs, Es, Ls}; body {data_offset, vtx_sgpr, inst_sgpr, count_enable << 30,
//     max_count, count_addr (u64), sizeof(AmdGpu::DrawIndexedIndirectArgs), draw_initiator}; handler
//     SubmitDrawIndirect(true, ..., stride body[7], count body[4], count_indirect_enable ?
//     count_addr : 0, ...) | native: same; count_addr != 0 exactly when the enable bit is set, so
//     passing count_addr as-is is the handler's value | evidence: gnmdriver.cpp:548-587,
//     liverpool.cpp:1743-1765, pm4_cmds.h:1133-1160 (field order checked against the body writes)
//   sceGnmDrawIndexIndirectMulti (Table::DrawIndexIndirectMulti) | size 11, shader_stage in {Vs, Es, Ls},
//     sgpr < 0x10; body {data_offset, vtx_sgpr, inst_sgpr, count = max_count, stride =
//     sizeof(AmdGpu::DrawIndexedIndirectArgs), draw_initiator}; handler SubmitDrawIndirect(true, ...,
//     data_offset, stride body[4], count body[3], 0, base_vtx_loc, start_inst_loc) | native: same with
//     SubmitIndirectOp(true, ..., stride kDrawIndexedIndirectStride, count max_count, count_addr 0) |
//     evidence: gnmdriver.cpp:589-620 (gate and body order read from the source), liverpool.cpp:1734-1753
//     (handler), pm4_cmds.h:1118-1134 (field order). Raw bytes and REA not used.
//
// Shader stage ids and sgpr bases mirror gnmdriver.cpp (ShaderStages enum, indirect_sgpr_offsets),
// which are file-local there; a change there must be mirrored here.

#include <cstdint>

#include "common/types.h"
#include "core/libraries/kernel/process.h"
#include "native/gnm/gnm_common.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace Native::Gnm {

namespace {

// Mirrors gnmdriver.cpp ShaderStages (Cs, Ps, Vs, Gs, Es, Hs, Ls, Max).
constexpr u32 kStageVs = 2;
constexpr u32 kStageEs = 4;
constexpr u32 kStageLs = 6;
constexpr u32 kStageMax = 7;
// Mirrors gnmdriver.cpp indirect_sgpr_offsets (indexed by shader stage).
constexpr u32 kIndirectSgprOffsets[] = {0u, 0u, 0x4cu, 0u, 0xccu, 0u, 0x14cu};
constexpr u32 kDrawIndirectStride = sizeof(AmdGpu::DrawIndirectArgs);
constexpr u32 kDrawIndexedIndirectStride = sizeof(AmdGpu::DrawIndexedIndirectArgs);

// Sgpr operand as gnmdriver writes it into the packet body: 0 stays 0, else the low 16 bits of
// the offset plus the stage's base. The handlers read it back through a 16-bit BitField, so the
// u16 of this value is what they pass on.
inline u32 SgprBody(u32 sgpr_offset, u32 stage) {
    return sgpr_offset == 0 ? 0u : (sgpr_offset & 0xffffu) + kIndirectSgprOffsets[stage];
}

// Shared by the indirect draws. The op does what liverpool.cpp's indirect handlers do: no register
// writes, then SubmitDrawIndirect with the current indirect base, unless there is no rasterizer.
// Must run on the command processor (NativeSubmitDrawIndirect / NativeIndirectArgsAddr).
void SubmitIndirectOp(bool is_indexed, u32 data_offset, u32 stride, u32 max_count,
                      u64 count_address, u16 vertex_sgpr, u16 instance_sgpr) {
    auto& lv = Lv();
    if (!lv.NativeRasterizer()) {
        return;
    }
    lv.NativeSubmitDrawIndirect(is_indexed, lv.NativeIndirectArgsAddr(), data_offset, stride,
                                max_count, count_address, vertex_sgpr, instance_sgpr);
}

} // namespace

// sceGnmDrawIndex: DrawIndex2 (gnmdriver.cpp:475-498, liverpool.cpp:1598-1621).
static bool W09DrawIndex(u32* cmdbuf, u32 size, u32 index_count, uintptr_t index_addr, u32 flags,
                         u32 type) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndex");
    (void)type; // not written by the PM4 path
    if (!Enabled(Class::Draws) || !cmdbuf || size != 10 || index_addr == 0 ||
        (index_addr & 1) != 0 || (flags & 0x1ffffffeu) != 0) {
        Stats::SiteFallback(site);
        return false;
    }
    const u32 draw_initiator = Libraries::Kernel::sceKernelIsNeoMode() ? flags & 0xe0000000u : 0u;
    const u32 base_lo = u32(index_addr);
    const u32 base_hi = u32(index_addr >> 32);
    const bool ok = EmitCpInSlot(cmdbuf, size, [index_count, base_lo, base_hi, draw_initiator] {
        auto& lv = Lv();
        auto& regs = lv.NativeRegs();
        regs.max_index_size = index_count;
        regs.index_base_address.base_addr_lo = base_lo;
        regs.index_base_address.base_addr_hi = base_hi;
        regs.num_indices = index_count;
        regs.draw_initiator = draw_initiator;
        lv.NativeMarkField(&regs.max_index_size, sizeof(regs.max_index_size));
        lv.NativeMarkField(&regs.index_base_address, sizeof(regs.index_base_address));
        lv.NativeMarkField(&regs.num_indices, sizeof(regs.num_indices));
        lv.NativeMarkField(&regs.draw_initiator, sizeof(regs.draw_initiator));
        if (!lv.NativeRasterizer()) {
            return;
        }
        lv.NativeSubmitDraw(true, 0);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndexAuto (gnmdriver.cpp:500-516, liverpool.cpp:1645-1662).
static bool W09DrawIndexAuto(u32* cmdbuf, u32 size, u32 index_count, u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndexAuto");
    if (!Enabled(Class::Draws) || !cmdbuf || size != 7 || (flags & 0x1ffffffeu) != 0) {
        Stats::SiteFallback(site);
        return false;
    }
    // Auto-index bit (2) is part of the draw initiator, as the packet body writes it.
    const u32 draw_initiator = Libraries::Kernel::sceKernelIsNeoMode() ? (flags & 0xe0000000u) | 2u : 2u;
    const bool ok = EmitCpInSlot(cmdbuf, size, [index_count, draw_initiator] {
        auto& lv = Lv();
        auto& regs = lv.NativeRegs();
        regs.num_indices = index_count;
        regs.draw_initiator = draw_initiator;
        lv.NativeMarkField(&regs.num_indices, sizeof(regs.num_indices));
        lv.NativeMarkField(&regs.draw_initiator, sizeof(regs.draw_initiator));
        if (!lv.NativeRasterizer()) {
            return;
        }
        lv.NativeSubmitDraw(false, 0);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndexOffset (gnmdriver.cpp:624-643, liverpool.cpp:1622-1644). The packet body is
// {index_count as max_size, index_offset, index_count, draw_initiator}.
static bool W09DrawIndexOffset(u32* cmdbuf, u32 size, u32 index_offset, u32 index_count,
                               u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndexOffset");
    if (!Enabled(Class::Draws) || !cmdbuf || size != 9) {
        Stats::SiteFallback(site);
        return false;
    }
    const u32 draw_initiator = Libraries::Kernel::sceKernelIsNeoMode() ? flags & 0xe0000000u : 0u;
    const bool ok = EmitCpInSlot(cmdbuf, size, [index_offset, index_count, draw_initiator] {
        auto& lv = Lv();
        auto& regs = lv.NativeRegs();
        regs.max_index_size = index_count;
        regs.num_indices = index_count;
        regs.draw_initiator = draw_initiator;
        lv.NativeMarkField(&regs.max_index_size, sizeof(regs.max_index_size));
        lv.NativeMarkField(&regs.num_indices, sizeof(regs.num_indices));
        lv.NativeMarkField(&regs.draw_initiator, sizeof(regs.draw_initiator));
        if (!lv.NativeRasterizer()) {
            return;
        }
        lv.NativeSubmitDraw(true, index_offset);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndirect (gnmdriver.cpp:645-672, liverpool.cpp:1664-1681). The handler takes only the
// two sgpr fields and the data offset; it writes no registers.
static bool W09DrawIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage,
                            u32 vertex_sgpr_offset, u32 instance_sgpr_offset, u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndirect");
    (void)flags; // predication not consulted by the CP (see file header)
    if (!Enabled(Class::Draws) || !cmdbuf || size != 9 || shader_stage >= kStageMax ||
        vertex_sgpr_offset >= 0x10u || instance_sgpr_offset >= 0x10u) {
        Stats::SiteFallback(site);
        return false;
    }
    const u16 vertex_sgpr = u16(SgprBody(vertex_sgpr_offset, shader_stage));
    const u16 instance_sgpr = u16(SgprBody(instance_sgpr_offset, shader_stage));
    const bool ok = EmitCpInSlot(cmdbuf, size, [data_offset, vertex_sgpr, instance_sgpr] {
        SubmitIndirectOp(false, data_offset, kDrawIndirectStride, 1, 0, vertex_sgpr,
                         instance_sgpr);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndexIndirect (gnmdriver.cpp:518-546, liverpool.cpp:1703-1721).
static bool W09DrawIndexIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage,
                                 u32 vertex_sgpr_offset, u32 instance_sgpr_offset, u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndexIndirect");
    (void)flags;
    if (!Enabled(Class::Draws) || !cmdbuf || size != 9 || shader_stage >= kStageMax ||
        vertex_sgpr_offset >= 0x10u || instance_sgpr_offset >= 0x10u) {
        Stats::SiteFallback(site);
        return false;
    }
    const u16 vertex_sgpr = u16(SgprBody(vertex_sgpr_offset, shader_stage));
    const u16 instance_sgpr = u16(SgprBody(instance_sgpr_offset, shader_stage));
    const bool ok = EmitCpInSlot(cmdbuf, size, [data_offset, vertex_sgpr, instance_sgpr] {
        SubmitIndirectOp(true, data_offset, kDrawIndexedIndirectStride, 1, 0, vertex_sgpr,
                         instance_sgpr);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndirectMulti (gnmdriver.cpp:680-708, liverpool.cpp:1683-1701). Packet body:
// {data_offset, vtx_sgpr, inst_sgpr, max_count, sizeof(AmdGpu::DrawIndirectArgs), draw_initiator}; the
// handler reads count = max_count and stride = sizeof(AmdGpu::DrawIndirectArgs).
static bool W09DrawIndirectMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                                 u32 shader_stage, u32 vertex_sgpr_offset,
                                 u32 instance_sgpr_offset, u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndirectMulti");
    (void)flags;
    if (!Enabled(Class::Draws) || !cmdbuf || size != 11 || shader_stage >= kStageMax ||
        vertex_sgpr_offset >= 0x10u || instance_sgpr_offset >= 0x10u) {
        Stats::SiteFallback(site);
        return false;
    }
    const u16 vertex_sgpr = u16(SgprBody(vertex_sgpr_offset, shader_stage));
    const u16 instance_sgpr = u16(SgprBody(instance_sgpr_offset, shader_stage));
    const bool ok = EmitCpInSlot(cmdbuf, size, [data_offset, max_count, vertex_sgpr,
                                                instance_sgpr] {
        SubmitIndirectOp(false, data_offset, kDrawIndirectStride, max_count, 0, vertex_sgpr,
                         instance_sgpr);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndexIndirectCountMulti (gnmdriver.cpp:548-587, liverpool.cpp:1743-1765). The
// gnmdriver's Neo compat gate is `!Neo || !UseNeoCompatSequences` with UseNeoCompatSequences
// constant false, so that part is always true and is not repeated here.
// Packet body: {data_offset, vtx_sgpr, inst_sgpr, count_enable << 30, max_count, count_addr (u64),
// sizeof(AmdGpu::DrawIndexedIndirectArgs), draw_initiator}. The handler passes count_addr only when the
// enable bit is set; the enable bit is set exactly when count_addr != 0, so passing count_addr
// directly gives the same value.
static bool W09DrawIndexIndirectCountMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                                           u64 count_addr, u32 shader_stage,
                                           u32 vertex_sgpr_offset, u32 instance_sgpr_offset,
                                           u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndexIndirectCountMulti");
    (void)flags;
    if (!Enabled(Class::Draws) || !cmdbuf || size != 16 || vertex_sgpr_offset >= 0x10u ||
        instance_sgpr_offset >= 0x10u ||
        (shader_stage != kStageVs && shader_stage != kStageEs && shader_stage != kStageLs)) {
        Stats::SiteFallback(site);
        return false;
    }
    const u16 vertex_sgpr = u16(SgprBody(vertex_sgpr_offset, shader_stage));
    const u16 instance_sgpr = u16(SgprBody(instance_sgpr_offset, shader_stage));
    const bool ok = EmitCpInSlot(cmdbuf, size, [data_offset, max_count, count_addr, vertex_sgpr,
                                                instance_sgpr] {
        SubmitIndirectOp(true, data_offset, kDrawIndexedIndirectStride, max_count, count_addr,
                         vertex_sgpr, instance_sgpr);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmDrawIndexIndirectMulti (gnmdriver.cpp:589-620, liverpool.cpp:1734-1753). Packet body:
// {data_offset, vtx_sgpr, inst_sgpr, count = max_count, stride = sizeof(AmdGpu::DrawIndexedIndirectArgs),
// draw_initiator}. The gnm gate is size == 11 and shader_stage in {Vs, Es, Ls} (unlike DrawIndirectMulti,
// which accepts any stage below Max). The handler passes count_indirect = 0, so count_addr is 0 here.
// draw_initiator is written by the packet but never read by the handler (no register write), as in
// the other indirect draws.
static bool W09DrawIndexIndirectMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count,
                                      u32 shader_stage, u32 vertex_sgpr_offset,
                                      u32 instance_sgpr_offset, u32 flags) {
    static const u32 site = Native::Gnm::Site("w09:sceGnmDrawIndexIndirectMulti");
    (void)flags; // predication not consulted by the CP (see file header)
    if (!Enabled(Class::Draws) || !cmdbuf || size != 11 || vertex_sgpr_offset >= 0x10u ||
        instance_sgpr_offset >= 0x10u ||
        (shader_stage != kStageVs && shader_stage != kStageEs && shader_stage != kStageLs)) {
        Stats::SiteFallback(site);
        return false;
    }
    const u16 vertex_sgpr = u16(SgprBody(vertex_sgpr_offset, shader_stage));
    const u16 instance_sgpr = u16(SgprBody(instance_sgpr_offset, shader_stage));
    const bool ok = EmitCpInSlot(cmdbuf, size, [data_offset, max_count, vertex_sgpr,
                                                instance_sgpr] {
        SubmitIndirectOp(true, data_offset, kDrawIndexedIndirectStride, max_count, 0, vertex_sgpr,
                         instance_sgpr);
    });
    if (ok) {
        Stats::SiteNative(site);
    } else {
        Stats::SiteFallback(site);
    }
    return ok;
}

void RegisterW09(Table& t) {
    t.DrawIndex = &W09DrawIndex;
    t.DrawIndexAuto = &W09DrawIndexAuto;
    t.DrawIndexOffset = &W09DrawIndexOffset;
    t.DrawIndirect = &W09DrawIndirect;
    t.DrawIndexIndirect = &W09DrawIndexIndirect;
    t.DrawIndirectMulti = &W09DrawIndirectMulti;
    t.DrawIndexIndirectMulti = &W09DrawIndexIndirectMulti;
    t.DrawIndexIndirectCountMulti = &W09DrawIndexIndirectCountMulti;
}

} // namespace Native::Gnm
