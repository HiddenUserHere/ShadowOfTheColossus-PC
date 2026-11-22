// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W03: Context registers: raster / viewport / other.
// Class Raster (SOTC_NATIVE_GNM=...). Owned files: this one and gpu/sotc/hook_gnm_w03.inc.
// See gnm_common.h for the contract.
//
// Every detour below is an eboot GNM emitter that writes SET_CONTEXT_REG (0x69) packets into a
// GNM command buffer object. The native op runs inline on the command processor (NativeSetRegs
// = the SetContextReg handler's memcpy + MarkRegs, with the same absolute dword
// Regs::ContextRegWordOffset + offset). Values are decoded and captured at the detour; the guest
// writes nothing else (no cb state beyond the cursor, no return value, no caches).
//
// Evidence legend: "decomp" = out/re/decomp/<4hex>/<8hex>.c (Ghidra export, REA was not open
// in this run: "No app is open"), "raw" = header immediates found in the eboot bytes
// (out/eboot.elf, file offset = VA + 0x4000) by a scan of the function body, PM4 offsets are the
// raw dword offsets stored in the packet (abs dword = 0xA000 + offset).
//
// Coverage table (VA | contract of the emitter | native op | evidence):
//   0x86d7b0 | viewport n: 2 dw at 0xb4+2n (Z min/max) then 6 dw at 0x10f+6n (x/y/z scale and
//            offset, interleaved), two SET_CONTEXT_REG packets, 12 dwords | two NativeSetRegs in
//            one op | decomp 0086/0086d7b0.c:1-48; raw 0xc0026900 + 0xc0066900 checked y; callers
//            FUN_0085de40
//   0x86d950 | scissor TL/BR: reg 0x0c, 2 dw, each 16-bit pair (y<<16 | x) | NativeSetRegs(0x0c,2)
//            | decomp 0086/0086d950.c:1-24; raw 0xc0026900 checked y
//   0x86e1a0 | reg 0x205 = arg & 0x393fff | NativeSetRegs(0x205,1) | decomp 0086/0086e1a0.c:1-23;
//            raw 0xc0016900 checked y
//   0x86e410 | reg 0x2e0 = (p1, p2) | NativeSetRegs(0x2e0,2) | decomp 0086/0086e410.c; raw checked y
//   0x86e480 | reg 0x2e2 = (p1, p2) | NativeSetRegs(0x2e2,2) | decomp 0086/0086e480.c; raw checked y
//   0x86d160 | reg 0x2d5 = value, written only when value is one of the filter set (0/0x45/0xad
//            below 0xb0, 0xb0/0x145 below 0x1ad, 0x200/0x1ad above); other values emit nothing.
//            The guest then only runs its cb space check (refill), so the detour calls the guest
//            body for filtered-out values | NativeSetRegs(0x2d5,1) | decomp 0086/0086d160.c:1-33;
//            raw 0xc0016900 checked y; 13+ callers
//   0x86cc20 | four packets from the 16-byte vector v (dwords d0..d3) and p3: reg 0x2d7 x4 = d_i &
//            0x7fff (mask constant at VA 0x128fa10 = 0x7fff per dword, read from out/eboot.elf);
//            reg 0x298 x3 = (d0*p3)&0x7fff, ((d0+d1)*p3)&0x7fff, ((d0+d1+d2)*p3)&0x7fff; reg 0x2ce
//            = p3&0x7ff; reg 0x2ac = ((d0+d1+d2+d3)*p3)&0x7fff | one op, same write order
//            | decomp 0086/0086cc20.c:1-54; raw headers 0xc0046900/0xc0036900/0xc0016900 checked
//            y. Alias: 0x85c9a0 is a 5-byte jmp (e9 7b 02 01 00) to 0x86cc20, so it is covered
//            by this detour and gets no row of its own (decomp 0085/0085c9a0.c identical).
//   0x86e2d0 | reg 0x281 = (p3 << 16) | (p2 & 0xffff) | NativeSetRegs(0x281,1) | decomp
//            0086/0086e2d0.c; raw checked y; no callers in the export
//   0x86db40 | reg 0x206 = arg & 0xf3f | decomp 0086/0086db40.c; raw checked n (same header
//            family as 0x86e1a0; header not re-scanned)
//   0x86ded0 | reg 0x204 = arg & 0xf7fe03f | decomp 0086/0086ded0.c; raw checked n; caller
//            FUN_00090470
//   0x86cbc0 | reg 0x2ab = arg & 0x7fff | decomp 0086/0086cbc0.c; raw checked n; caller
//            FUN_00863140
//   0x86b6f0 | reg 0x2dd = arg | decomp 0086/0086b6f0.c; raw checked n; caller FUN_00862c50
//   0x86e4f0 | reg 0x292 = (p2 & 1) + (p3 & 1) * 2 | decomp 0086/0086e4f0.c; raw checked n;
//            callers FUN_0023c7c0, FUN_0023c980, FUN_0023d750, FUN_0023c9aa, FUN_00178cb0
//   0x86c650 | reg 0x290 = ((p2 & 3) << 4) | 0x180003 | decomp 0086/0086c650.c; raw checked n;
//            caller FUN_00860c50
//   0x86c5f0 | reg 0x290 = 0 then reg 0x2e5 = 0 (two packets; decomp shows an 8-byte store whose
//            high dword is the second header) | decomp 0086/0086c5f0.c; raw checked n; many callers
//   0x86e560 | reg 0x2f8 = ((p2 & 7) << 0x14) | (p2 & 7) | ((p3 & 0xf) << 0xd) | decomp
//            0086/0086e560.c; raw checked n; caller FUN_0085ddb0
//   0x86a090 | reg 0x2fa x4 = (p2, p4, p1, p3) | decomp 0086/0086a090.c; raw checked n;
//            caller FUN_0085de40
//   0x869450 | NOT W03: the header is 0xC0017600 (+2 bit), i.e. SET_SH_REG (opcode 0x76), not
//            SET_CONTEXT_REG; the decomp is a 1-byte stub. Left to the shader workers (skipped).
//   Round 2 (R2), same evidence legend; every row checked against the raw bytes of out/eboot.elf
//   (file offset = VA + 0x4000) and the SetContextReg handler (liverpool.cpp ~1496):
//   0x86a010 | reg 0x8d = ((p3 & 0x1fc) << 16) | (p2 & 0x1fc) (PA_SU_HARDWARE_SCREEN_OFFSET;
//            W01 notes it as W03's) | NativeSetRegs(0x8d,1) | decomp 0086/0086a010.c; raw
//            0xc0016900 + 0x8d and the masks checked y; caller FUN_0085de40
//   0x86d890 | 0xc0026900: reg 0x94+2*p2 = (p3&0x7fff) | (p4&0x7fff)<<16 | (p7==0)<<31, then
//            (p5&0x7fff) | (p6&0x7fff)<<16 (p7 = 7th, stack argument, [rbp+0x10]) |
//            NativeSetRegs(2 regs) | decomp 0086/0086d890.c; raw checked y. Fallback when the pair
//            touches CB/DB extent-hint registers (0x10, 0x318..0x388)
//   0x86e5e0 | 0xc0106900 at reg 0x2fe: 16 dwords copied from the 72-byte block p2 (nine qwords);
//            then 0xc0026900 at reg 0x2f5 with p2[8] (lo, hi) | two NativeSetRegs | decomp
//            0086/0086e5e0.c; raw: header 0xc0106900 + offset 0x2fe (imm64 0x2fec0106900) and
//            0x2f5 (+0x4c) checked y. Ownership: W05's notes name this block as its default
//            state; no row in W05's .inc, so it is here (lead to confirm)
//   0x86c6c0 | reg 0x291 = ((p4 & 0x7ff) << 11) | (p3 & 0x7ff), then reg 0x290 = ((p2 & 3) << 4) |
//            0x600003 | two NativeSetRegs | decomp 0086/0086c6c0.c; raw checked y; caller
//            FUN_00860ea0
//   0x86c840 | reg 0x20 = (u32)(p2 >> 8) (p2 is 64-bit) | NativeSetRegs(0x20,1) | decomp
//            0086/0086c840.c; raw checked y (shr 8 on the qword); caller FUN_00090470
//   0x86bc00 | 0xc0006900 | count << 16 at reg 0x191, count values from p2; count 0 = guest NOP
//            only (native: empty op); count > 0x20 -> guest body | NativeSetRegs(0x191, count) |
//            decomp 0086/0086bc00.c; raw: header imm 0xc0006900 with count<<16 and the switch
//            checked y; caller FUN_00094ab0
//   0x86e3a0 | reg 0x2de = DAT_0128fee0[p2] (u32 table, read at the detour from
//            GuestImageBase() + 0x128fee0, the address the guest's lea uses; table length not
//            needed) | NativeSetRegs(0x2de,1) | decomp 0086/0086e3a0.c; raw: lea at +0x3b
//            resolves to 0x128fee0, header 0xc0016900 checked y; callers FUN_00092140,
//            FUN_00093ef0, FUN_00094600
//   Round 3 (X3): context emitters with no static callers (function-pointer tables only). ABI and
//   register offsets from the raw bytes; the Ghidra signatures differ: 0x86df30 takes its four
//   values as floats in xmm0..3 (u32 index in esi), 0x86dc70 masks every byte argument and reads a
//   seventh stack argument. None of the ten touches a CB/DB extent-hint register (0x10, 0x318..0x388).
//   0x86dba0 | 0xc0036900 at reg 0xe8 = (p3, p2, p4) | NativeSetRegs(0xe8,3) | raw checked y
//   0x86dc10 | 0xc0026900 at reg 0xeb = 0xff00ff00, reg 0xec = 0x0000ff00 (constant) | raw y
//   0x86dc70 | 0xc0026900 at reg 0xeb = (p5<<24)|(p4<<16)|(p3<<8)|p2 (bytes), reg 0xec = (p7<<8)|p6
//            | raw checked y (movzbl on each argument, stack byte at [rbp+0x10])
//   0x86dd80 | 0xc0016900 at reg 0x293 = ((p2&1)<<16)|0x6020000 | raw checked y
//   0x86ddf0 | 0xc0016900 at reg 0x6 = p2 & 0xffff | raw checked y
//   0x86df30 | 0xc0046900 at reg 0x16f+4*index = xmm0..xmm3 as bits | raw checked y
//   0x86dfd0 | 0xc0026900 at reg 0x84+2*index = (p4&0x7fff)<<16|(p3&0x7fff), then
//            (p6&0x7fff)<<16|(p5&0x7fff) | raw checked y
//   0x86e080 | 0xc0016900 at reg 0x83 = p2 & 0xffff | raw checked y
//   0x86e0e0 | 0xc0016900 at reg 0x2a5 = p2 & 0xff (VGT_MULTI_PRIM_IB_RESET_EN) | raw checked y
//   0x86e140 | 0xc0016900 at reg 0x103 = p2 (VGT_MULTI_PRIM_IB_RESET_INDX) | raw checked y
//   Not done (candidates from the 0x69 inventory, not implemented, no change to PM4): 0x86e340,
//     0x86e260, 0x86e200, 0x86da90, 0x86d9e0, 0x86d730, 0x86d100, 0x86c8a0, 0x86c570, 0x86c4d0,
//     0x86bb90, 0x86bb30, 0x86a970, 0x86a120, 0x86fxxx group (no callers, regs 0x20d/0x2d4/
//     0x2a6/0x2cc/0x2ca/0x2e5/0x2e6). Their headers come from the same eboot raw scan; they need
//     the same per-register proof before a detour.
//   REA: not used in this round (not verified). Fallback: decomp exports and out/eboot.elf bytes.

#include <array>
#include <cstring>

#include <bit>

#include "native/gnm/gnm_common.h"
#include "native/guest_bridge.h"
#include "video_core/amdgpu/liverpool.h"

namespace {

// Absolute dword of the context register space (SET_CONTEXT_REG offset is relative to it).
constexpr u32 kCtxWordOffset = AmdGpu::Regs::ContextRegWordOffset;

// Queues `op`; counts the boundary site as native on success, fallback when the op was dropped.
bool Queue(u32 site, u64 cb, Common::UniqueFunction<void>&& op) {
    if (Native::Gnm::EmitCp(cb, std::move(op))) {
        Native::Stats::SiteNative(site);
        return true;
    }
    Native::Stats::SiteFallback(site);
    return false;
}

// The op body: one SET_CONTEXT_REG equivalent (copy + mark, as the PM4 handler does).
template <size_t N>
void WriteCtx(u32 reg, const std::array<u32, N>& values) {
    Native::Gnm::Lv().NativeSetRegs(kCtxWordOffset + reg, values.data(), static_cast<u32>(N));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// ABI (gap N2-73): zmin/zmax arrive in xmm0/xmm1 (SysV floats), cb in rdi, index in esi,
// scale/offset pointers in rdx/rcx; the raw-byte check confirmed the vmovss spills.
// Viewport n: Z min/max at 0xb4+2n, then x/y/z scale and offset (interleaved) at 0x10f+6n.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86d7b0(float zmin, float zmax, u64 cb, s32 index,
                                                       const u32* scale, const u32* offset) {
    static const u32 site = Native::Gnm::Site("w03:86d7b0");
    using Body = void (__attribute__((sysv_abi)) *)(float, float, u64, s32, const u32*, const u32*);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86d7b0)(zmin, zmax, cb, index, scale, offset);
        return;
    }
    const u32 zreg = 0xb4u + static_cast<u32>(index) * 2u;
    const u32 xreg = 0x10fu + static_cast<u32>(index) * 6u;
    const std::array<u32, 2> z{std::bit_cast<u32>(zmin), std::bit_cast<u32>(zmax)};
    const std::array<u32, 6> xf{scale[0], offset[0], scale[1], offset[1], scale[2], offset[2]};
    if (Queue(site, cb, [zreg, xreg, z, xf]() {
            WriteCtx(zreg, z);
            WriteCtx(xreg, xf);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86d7b0)(zmin, zmax, cb, index, scale, offset);
}

// ---------------------------------------------------------------------------------------------
// Scissor: reg 0x0c, two dwords (y << 16 | x) for top-left and bottom-right.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86d950(u64 cb, u32 p2, s32 p3, u32 p4, s32 p5) {
    static const u32 site = Native::Gnm::Site("w03:86d950");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, s32, u32, s32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86d950)(cb, p2, p3, p4, p5);
        return;
    }
    const std::array<u32, 2> v{(static_cast<u32>(p3) << 16) | (p2 & 0xffffu),
                               (static_cast<u32>(p5) << 16) | (p4 & 0xffffu)};
    if (Queue(site, cb, [v]() { WriteCtx(0x0c, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86d950)(cb, p2, p3, p4, p5);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x205 = arg & 0x393fff.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e1a0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86e1a0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e1a0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0x393fffu};
    if (Queue(site, cb, [v]() { WriteCtx(0x205, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e1a0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2e0 = (p1, p2) (poly offset, front).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e410(float p1, float p2, u64 cb) {
    static const u32 site = Native::Gnm::Site("w03:86e410");
    using Body = void (__attribute__((sysv_abi)) *)(float, float, u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e410)(p1, p2, cb);
        return;
    }
    const std::array<u32, 2> v{std::bit_cast<u32>(p1), std::bit_cast<u32>(p2)};
    if (Queue(site, cb, [v]() { WriteCtx(0x2e0, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e410)(p1, p2, cb);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2e2 = (p1, p2) (poly offset, back).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e480(float p1, float p2, u64 cb) {
    static const u32 site = Native::Gnm::Site("w03:86e480");
    using Body = void (__attribute__((sysv_abi)) *)(float, float, u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e480)(p1, p2, cb);
        return;
    }
    const std::array<u32, 2> v{std::bit_cast<u32>(p1), std::bit_cast<u32>(p2)};
    if (Queue(site, cb, [v]() { WriteCtx(0x2e2, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e480)(p1, p2, cb);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2d5 = value, for the filtered values only. Other values emit no packet; the guest body
// still runs its cb space check (refill callback), so it is called for them.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86d160(u64 cb, s32 value) {
    static const u32 site = Native::Gnm::Site("w03:86d160");
    using Body = void (__attribute__((sysv_abi)) *)(u64, s32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86d160)(cb, value);
        return;
    }
    // Same filter as the guest (signed comparisons, as in the decompilation).
    bool wanted;
    if (value < 0xb0) {
        wanted = value == 0 || value == 0x45 || value == 0xad;
    } else if (value < 0x1ad) {
        wanted = value == 0xb0 || value == 0x145;
    } else {
        wanted = value == 0x200 || value == 0x1ad;
    }
    if (!wanted) {
        // No packet: the guest body only evaluates its cb space check.
        Native::Stats::SiteNative(site);
        Native::Gnm::Guest<Body>(0x86d160)(cb, value);
        return;
    }
    const std::array<u32, 1> v{static_cast<u32>(value)};
    if (Queue(site, cb, [v]() { WriteCtx(0x2d5, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86d160)(cb, value);
}

// ---------------------------------------------------------------------------------------------
// Four packets from the 16-byte vector at p2 and the scalar p3 (also reached from 0x85c9a0, a
// jmp to this entry). Writes: 0x2d7 x4 (dword & 0x7fff), 0x298 x3 (prefix sums * p3 & 0x7fff),
// 0x2ce (p3 & 0x7ff), 0x2ac (sum of four * p3 & 0x7fff).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86cc20(u64 cb, const u32* p2, u32 p3) {
    static const u32 site = Native::Gnm::Site("w03:86cc20");
    using Body = void (__attribute__((sysv_abi)) *)(u64, const u32*, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86cc20)(cb, p2, p3);
        return;
    }
    const u32 d0 = p2[0];
    const u32 d1 = p2[1];
    const u32 d2 = p2[2];
    const u32 d3 = p2[3];
    const std::array<u32, 4> masked{d0 & 0x7fffu, d1 & 0x7fffu, d2 & 0x7fffu, d3 & 0x7fffu};
    const std::array<u32, 3> prefix{(d0 * p3) & 0x7fffu, ((d0 + d1) * p3) & 0x7fffu,
                                    ((d0 + d1 + d2) * p3) & 0x7fffu};
    const std::array<u32, 1> ce{p3 & 0x7ffu};
    const std::array<u32, 1> sum{((d0 + d1 + d2 + d3) * p3) & 0x7fffu};
    if (Queue(site, cb, [masked, prefix, ce, sum]() {
            WriteCtx(0x2d7, masked);
            WriteCtx(0x298, prefix);
            WriteCtx(0x2ce, ce);
            WriteCtx(0x2ac, sum);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86cc20)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x281 = (p3 << 16) | (p2 & 0xffff). No callers in the export; implemented for completeness.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e2d0(u64 cb, u32 p2, s32 p3) {
    static const u32 site = Native::Gnm::Site("w03:86e2d0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, s32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e2d0)(cb, p2, p3);
        return;
    }
    const std::array<u32, 1> v{(static_cast<u32>(p3) << 16) | (p2 & 0xffffu)};
    if (Queue(site, cb, [v]() { WriteCtx(0x281, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e2d0)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x206 = arg & 0xf3f.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86db40(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86db40");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86db40)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0xf3fu};
    if (Queue(site, cb, [v]() { WriteCtx(0x206, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86db40)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x204 = arg & 0xf7fe03f.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86ded0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86ded0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86ded0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0xf7fe03fu};
    if (Queue(site, cb, [v]() { WriteCtx(0x204, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86ded0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2ab = arg & 0x7fff.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86cbc0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86cbc0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86cbc0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0x7fffu};
    if (Queue(site, cb, [v]() { WriteCtx(0x2ab, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86cbc0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2dd = arg.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86b6f0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86b6f0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86b6f0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2};
    if (Queue(site, cb, [v]() { WriteCtx(0x2dd, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86b6f0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x292 = (p2 & 1) + (p3 & 1) * 2.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e4f0(u64 cb, u32 p2, u32 p3) {
    static const u32 site = Native::Gnm::Site("w03:86e4f0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e4f0)(cb, p2, p3);
        return;
    }
    const std::array<u32, 1> v{(p2 & 1u) + (p3 & 1u) * 2u};
    if (Queue(site, cb, [v]() { WriteCtx(0x292, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e4f0)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x290 = ((p2 & 3) << 4) | 0x180003.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86c650(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86c650");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86c650)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{((p2 & 3u) << 4) | 0x180003u};
    if (Queue(site, cb, [v]() { WriteCtx(0x290, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86c650)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Regs 0x290 = 0 and 0x2e5 = 0 (two packets, in this order).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86c5f0(u64 cb) {
    static const u32 site = Native::Gnm::Site("w03:86c5f0");
    using Body = void (__attribute__((sysv_abi)) *)(u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86c5f0)(cb);
        return;
    }
    const std::array<u32, 1> zero{0u};
    if (Queue(site, cb, [zero]() {
            WriteCtx(0x290, zero);
            WriteCtx(0x2e5, zero);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86c5f0)(cb);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2f8 = ((p2 & 7) << 0x14) | (p2 & 7) | ((p3 & 0xf) << 0xd).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e560(u64 cb, u32 p2, u32 p3) {
    static const u32 site = Native::Gnm::Site("w03:86e560");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e560)(cb, p2, p3);
        return;
    }
    const std::array<u32, 1> v{((p2 & 7u) << 0x14) | (p2 & 7u) | ((p3 & 0xfu) << 0xd)};
    if (Queue(site, cb, [v]() { WriteCtx(0x2f8, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e560)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2fa x4 = (p2, p4, p1, p3), in this order.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86a090(float p1, float p2, float p3, float p4, u64 cb) {
    static const u32 site = Native::Gnm::Site("w03:86a090");
    using Body = void (__attribute__((sysv_abi)) *)(float, float, float, float, u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86a090)(p1, p2, p3, p4, cb);
        return;
    }
    const std::array<u32, 4> v{std::bit_cast<u32>(p2), std::bit_cast<u32>(p4),
                               std::bit_cast<u32>(p1), std::bit_cast<u32>(p3)};
    if (Queue(site, cb, [v]() { WriteCtx(0x2fa, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86a090)(p1, p2, p3, p4, cb);
}

// ---------------------------------------------------------------------------------------------
// Round 2 (R2): the remaining SET_CONTEXT_REG emitters of the raster class. Each op repeats the
// SetContextReg handler (liverpool.cpp, case PM4ItOpcode::SetContextReg): NativeSetRegs copies
// the values to the context block at ContextRegWordOffset + reg and marks them, the same as the
// PM4 handler's memcpy + MarkRegs. The handler's CB/DB extent hint (reads a trailing NOP for
// DbZInfo 0x10 and the CbColorN base/cmask 0x318..0x388) is not reproduced: the detours hand any
// packet that touches those registers back to the guest body.
// ---------------------------------------------------------------------------------------------
namespace {

// The packet's registers must lie inside the context block (guest body is the fallback).
bool CtxFits(u64 reg, u64 count) {
    return reg + count <= AmdGpu::Regs::NumRegs - kCtxWordOffset;
}

// True when the packet touches the registers whose handler branch reads the trailing NOP.
bool TouchesExtentHint(u64 reg, u64 count) {
    const u64 last = reg + count - 1;
    return (reg <= 0x10 && last >= 0x10) || (last >= 0x318 && reg <= 0x388);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Reg 0x8d = ((p3 & 0x1fc) << 16) | (p2 & 0x1fc): PA_SU_HARDWARE_SCREEN_OFFSET (raw bytes: header
// 0xc0016900, offset 0x8d, value from edx/esi masks 0x1fc). Caller FUN_0085de40.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86a010(u64 cb, u32 p2, u32 p3) {
    static const u32 site = Native::Gnm::Site("w03:86a010");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86a010)(cb, p2, p3);
        return;
    }
    const std::array<u32, 1> v{((p3 & 0x1fcu) << 16) | (p2 & 0x1fcu)};
    if (Queue(site, cb, [v]() { WriteCtx(0x8d, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86a010)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Reg pair at 0x94 + 2 * index (two dwords, header 0xc0026900). Dword 0 = (p7 == 0) << 31 |
// (p4 & 0x7fff) << 16 | (p3 & 0x7fff); dword 1 = (p6 & 0x7fff) << 16 | (p5 & 0x7fff). p7 is the
// seventh (stack) argument. Callers 0x23c7c0, 0x23d750, 0x91fc0, 0x23c9aa, 0x178cb0.
// ---------------------------------------------------------------------------------------------
// The index arrives in rsi and the guest spills all of rsi (raw: mov [rbp-0x30], rsi), so it is
// declared 64-bit and passed through unchanged; only its low dword enters the register number
// (the guest's lea ecx, [rsi+rsi+0x94] is 32-bit).
extern "C" __attribute__((sysv_abi)) void GnmW03_86d890(u64 cb, u64 index, u32 p3, u32 p4,
                                                       u32 p5, u32 p6, s32 p7) {
    static const u32 site = Native::Gnm::Site("w03:86d890");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u64, u32, u32, u32, u32, s32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86d890)(cb, index, p3, p4, p5, p6, p7);
        return;
    }
    const u32 reg = static_cast<u32>(index) * 2u + 0x94u;
    if (!CtxFits(reg, 2) || TouchesExtentHint(reg, 2)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86d890)(cb, index, p3, p4, p5, p6, p7);
        return;
    }
    const std::array<u32, 2> v{(p7 == 0 ? 0x80000000u : 0u) | ((p4 & 0x7fffu) << 16) | (p3 & 0x7fffu),
                               ((p6 & 0x7fffu) << 16) | (p5 & 0x7fffu)};
    if (Queue(site, cb, [reg, v]() { WriteCtx(reg, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86d890)(cb, index, p3, p4, p5, p6, p7);
}

// ---------------------------------------------------------------------------------------------
// One SET_CONTEXT_REG packet of 16 dwords at 0x2fe (regs 0x2fe..0x30d) copied from the 72-byte
// block at p2 (nine qwords), then a second packet of two dwords at 0x2f5 from p2[8] (low dword
// first). The block is read at the detour, as the guest does while it writes the packets.
// Caller FUN_0085ddb0. The eboot's default-state block (W05 names it too; no row there).
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e5e0(u64 cb, const u64* p2) {
    static const u32 site = Native::Gnm::Site("w03:86e5e0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, const u64*);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e5e0)(cb, p2);
        return;
    }
    std::array<u32, 16> block{};
    std::memcpy(block.data(), p2, sizeof(block));
    const std::array<u32, 2> tail{static_cast<u32>(p2[8]), static_cast<u32>(p2[8] >> 32)};
    if (Queue(site, cb, [block, tail]() {
            WriteCtx(0x2fe, block);
            WriteCtx(0x2f5, tail);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e5e0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Two packets: reg 0x291 = ((p4 & 0x7ff) << 11) | (p3 & 0x7ff), then reg 0x290 = ((p2 & 3) << 4) |
// 0x600003. Caller FUN_00860ea0.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86c6c0(u64 cb, u32 p2, u32 p3, u32 p4) {
    static const u32 site = Native::Gnm::Site("w03:86c6c0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86c6c0)(cb, p2, p3, p4);
        return;
    }
    const std::array<u32, 1> a{((p4 & 0x7ffu) << 11) | (p3 & 0x7ffu)};
    const std::array<u32, 1> b{((p2 & 3u) << 4) | 0x600003u};
    if (Queue(site, cb, [a, b]() {
            WriteCtx(0x291, a);
            WriteCtx(0x290, b);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86c6c0)(cb, p2, p3, p4);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x20 = (u32)(p2 >> 8) (p2 is a 64-bit argument). Caller FUN_00090470.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86c840(u64 cb, u64 p2) {
    static const u32 site = Native::Gnm::Site("w03:86c840");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86c840)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{static_cast<u32>(p2 >> 8)};
    if (Queue(site, cb, [v]() { WriteCtx(0x20, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86c840)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Multi-register SET_CONTEXT_REG (header 0xc0006900 | count << 16) at 0x191: `count` values copied
// from p2[0..count-1]. count 0 writes only a NOP in the guest (native: an empty op). The guest's
// switch covers counts 0..0x20; larger counts go to the guest body.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86bc00(u64 cb, const u32* p2, s32 count) {
    static const u32 site = Native::Gnm::Site("w03:86bc00");
    using Body = void (__attribute__((sysv_abi)) *)(u64, const u32*, s32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86bc00)(cb, p2, count);
        return;
    }
    const u32 n = static_cast<u32>(count);
    if (n > 0x20 || !CtxFits(0x191, n) || TouchesExtentHint(0x191, n)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86bc00)(cb, p2, count);
        return;
    }
    if (n == 0) {
        if (Queue(site, cb, []() {})) {
            return;
        }
        Native::Gnm::Guest<Body>(0x86bc00)(cb, p2, count);
        return;
    }
    std::array<u32, 0x20> v{};
    std::memcpy(v.data(), p2, n * sizeof(u32));
    if (Queue(site, cb, [v, n]() {
            Native::Gnm::Lv().NativeSetRegs(kCtxWordOffset + 0x191u, v.data(), n);
        })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86bc00)(cb, p2, count);
}

// ---------------------------------------------------------------------------------------------
// Reg 0x2de = DAT_0128fee0[p2] (u32 table in the eboot image; the guest indexes it with the
// zero-extended argument). The table is read at the detour from GuestImageBase() + 0x128fee0,
// the same address the guest's lea uses (raw bytes at 0x86e3a0 +0x3b). Callers FUN_00092140,
// FUN_00093ef0, FUN_00094600. Table length is not needed: the read is what the guest does.
// ---------------------------------------------------------------------------------------------
extern "C" __attribute__((sysv_abi)) void GnmW03_86e3a0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86e3a0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    constexpr u64 kTableVa = 0x128fee0;
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e3a0)(cb, p2);
        return;
    }
    const u64 image = Native::Guest::GuestImageBase();
    if (image == 0) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e3a0)(cb, p2);
        return;
    }
    u32 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(image + kTableVa + static_cast<u64>(p2) * 4u),
                sizeof(value));
    const std::array<u32, 1> v{value};
    if (Queue(site, cb, [v]() { WriteCtx(0x2de, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e3a0)(cb, p2);
}

// ---------------------------------------------------------------------------------------------
// Round 3 (X3): context emitters with no static callers (function-pointer tables only). The ABI
// is taken from the raw bytes at each VA (out/eboot.elf, file offset VA + 0x4000): float
// arguments arrive in xmm registers, byte arguments are read with movzbl (masked here), and the
// seventh argument of 0x86dc70 is the stack slot at [rbp+0x10] on entry. None of these registers
// is a CB/DB extent hint (0x10, 0x318..0x388), so no extent call is needed.
// ---------------------------------------------------------------------------------------------

// 0x86dba0: header 0xc0036900 at reg 0xe8: values (p3, p2, p4) in regs 0xe8, 0xe9, 0xea.
extern "C" __attribute__((sysv_abi)) void GnmW03_86dba0(u64 cb, u32 p2, u32 p3, u32 p4) {
    static const u32 site = Native::Gnm::Site("w03:86dba0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dba0)(cb, p2, p3, p4);
        return;
    }
    const std::array<u32, 3> v{p3, p2, p4};
    if (Queue(site, cb, [v]() { WriteCtx(0xe8, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86dba0)(cb, p2, p3, p4);
}

// 0x86dc10: header 0xc0026900 at reg 0xeb, constant qword from the raw movabs 0xff00ff00ff00:
// low dword 0xff00ff00 in reg 0xeb, high dword 0x0000ff00 in reg 0xec.
extern "C" __attribute__((sysv_abi)) void GnmW03_86dc10(u64 cb) {
    static const u32 site = Native::Gnm::Site("w03:86dc10");
    using Body = void (__attribute__((sysv_abi)) *)(u64);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dc10)(cb);
        return;
    }
    const std::array<u32, 2> v{0xff00ff00u, 0x0000ff00u};
    if (Queue(site, cb, [v]() { WriteCtx(0xeb, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86dc10)(cb);
}

// 0x86dc70: header 0xc0026900 at reg 0xeb: byte fields (p5 << 24) | (p4 << 16) | (p3 << 8) | p2;
// reg 0xec: (p7 << 8) | p6. Every argument is read as a byte (movzbl); p7 is the stack argument.
extern "C" __attribute__((sysv_abi)) void GnmW03_86dc70(u64 cb, u32 p2, u32 p3, u32 p4, u32 p5,
                                                       u32 p6, u32 p7) {
    static const u32 site = Native::Gnm::Site("w03:86dc70");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, u32, u32, u32, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dc70)(cb, p2, p3, p4, p5, p6, p7);
        return;
    }
    const std::array<u32, 2> v{((p5 & 0xffu) << 24) | ((p4 & 0xffu) << 16) | ((p3 & 0xffu) << 8) |
                                   (p2 & 0xffu),
                               ((p7 & 0xffu) << 8) | (p6 & 0xffu)};
    if (Queue(site, cb, [v]() { WriteCtx(0xeb, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86dc70)(cb, p2, p3, p4, p5, p6, p7);
}

// 0x86dd80: header 0xc0016900 at reg 0x293: ((p2 & 1) << 16) | 0x6020000.
extern "C" __attribute__((sysv_abi)) void GnmW03_86dd80(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86dd80");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dd80)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{((p2 & 1u) << 16) | 0x6020000u};
    if (Queue(site, cb, [v]() { WriteCtx(0x293, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86dd80)(cb, p2);
}

// 0x86ddf0: header 0xc0016900 at reg 0x6: p2 & 0xffff.
extern "C" __attribute__((sysv_abi)) void GnmW03_86ddf0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86ddf0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86ddf0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0xffffu};
    if (Queue(site, cb, [v]() { WriteCtx(0x6, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86ddf0)(cb, p2);
}

// 0x86df30: header 0xc0046900 at reg 0x16f + 4 * index, four consecutive regs from xmm0..xmm3
// (the float bit patterns, SysV: no integer register carries a value). The index is the esi
// argument (leal 0x16f(,%r14,4)).
extern "C" __attribute__((sysv_abi)) void GnmW03_86df30(u64 cb, u32 index, float v0, float v1,
                                                       float v2, float v3) {
    static const u32 site = Native::Gnm::Site("w03:86df30");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32, float, float, float, float);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86df30)(cb, index, v0, v1, v2, v3);
        return;
    }
    const u64 reg = 0x16fu + 4u * static_cast<u64>(index);
    if (!CtxFits(reg, 4) || TouchesExtentHint(reg, 4)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86df30)(cb, index, v0, v1, v2, v3);
        return;
    }
    const u32 base = static_cast<u32>(reg);
    const std::array<u32, 4> v{std::bit_cast<u32>(v0), std::bit_cast<u32>(v1),
                               std::bit_cast<u32>(v2), std::bit_cast<u32>(v3)};
    if (Queue(site, cb, [base, v]() { WriteCtx(base, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86df30)(cb, index, v0, v1, v2, v3);
}

// 0x86dfd0: header 0xc0026900 at reg 0x84 + 2 * index: two dwords, each a pair of 15-bit fields:
// (p4 & 0x7fff) << 16 | (p3 & 0x7fff), then (p6 & 0x7fff) << 16 | (p5 & 0x7fff). The index is a
// 64-bit argument in the raw bytes (the prologue spills rsi as a qword); the guest's leal uses its
// low 32 bits only, so the offset is computed from those.
extern "C" __attribute__((sysv_abi)) void GnmW03_86dfd0(u64 cb, u64 index, u32 p3, u32 p4, u32 p5,
                                                       u32 p6) {
    static const u32 site = Native::Gnm::Site("w03:86dfd0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u64, u32, u32, u32, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dfd0)(cb, index, p3, p4, p5, p6);
        return;
    }
    const u32 idx = static_cast<u32>(index);
    const u64 reg = 0x84u + 2u * static_cast<u64>(idx);
    if (!CtxFits(reg, 2) || TouchesExtentHint(reg, 2)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86dfd0)(cb, index, p3, p4, p5, p6);
        return;
    }
    const u32 base = static_cast<u32>(reg);
    const std::array<u32, 2> v{((p4 & 0x7fffu) << 16) | (p3 & 0x7fffu),
                               ((p6 & 0x7fffu) << 16) | (p5 & 0x7fffu)};
    if (Queue(site, cb, [base, v]() { WriteCtx(base, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86dfd0)(cb, index, p3, p4, p5, p6);
}

// 0x86e080: header 0xc0016900 at reg 0x83: p2 & 0xffff.
extern "C" __attribute__((sysv_abi)) void GnmW03_86e080(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86e080");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e080)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0xffffu};
    if (Queue(site, cb, [v]() { WriteCtx(0x83, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e080)(cb, p2);
}

// 0x86e0e0: header 0xc0016900 at reg 0x2a5 (VGT_MULTI_PRIM_IB_RESET_EN): p2 & 0xff (movzbl).
extern "C" __attribute__((sysv_abi)) void GnmW03_86e0e0(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86e0e0");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e0e0)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2 & 0xffu};
    if (Queue(site, cb, [v]() { WriteCtx(0x2a5, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e0e0)(cb, p2);
}

// 0x86e140: header 0xc0016900 at reg 0x103 (VGT_MULTI_PRIM_IB_RESET_INDX): p2 unchanged.
extern "C" __attribute__((sysv_abi)) void GnmW03_86e140(u64 cb, u32 p2) {
    static const u32 site = Native::Gnm::Site("w03:86e140");
    using Body = void (__attribute__((sysv_abi)) *)(u64, u32);
    if (!Native::Gnm::Enabled(Native::Gnm::Class::Raster)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Body>(0x86e140)(cb, p2);
        return;
    }
    const std::array<u32, 1> v{p2};
    if (Queue(site, cb, [v]() { WriteCtx(0x103, v); })) {
        return;
    }
    Native::Gnm::Guest<Body>(0x86e140)(cb, p2);
}

namespace Native::Gnm {

// Boundary 2 (sceGnm* driver functions): none of the W03 entries sit behind a sceGnm function;
// all of them are eboot emitters (boundary 1, hook_gnm_w03.inc).
void RegisterW03(Table& t) {
    (void)t;
}

} // namespace Native::Gnm
