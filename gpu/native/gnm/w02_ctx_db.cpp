// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W02: Context registers: depth / stencil.
// Class Db (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w02_ctx_db.h, and
// gpu/sotc/hook_gnm_w02.inc. See gnm_common.h for the contract.
//
// Each detour is an eboot emitter that appends one SET_CONTEXT_REG (0x69) packet to a GNM command
// buffer object (header 0xC0016900 / 0xC0026900 / ..., then the register offset relative to
// ContextRegWordOffset 0xA000, then the payloads). The native replacement appends a NOP through
// EmitCp; its op runs inline on the command processor and performs what the SetContextReg handler
// does for that packet (liverpool.cpp ~line 1485: memcpy into regs.reg_array, MarkRegs). None of
// the W02 registers has a special case in that switch (only the CB/DB surface extents do), so the
// op is a plain NativeSetRegs of the decoded, already masked values.
//
// Guest signatures: the detours take their arguments in SysV registers. Several decompiled
// parameter lists are misleading (Ghidra labels by declaration order): the float arguments of
// 0x869f50 / 0x869ee0 arrive in xmm0 / xmm1 (raw bytes: vmovss spills of xmm0 / xmm1), the
// command buffer object in rdi. Each signature below was checked against the raw bytes.
//
// Coverage table (site | contract (what the PM4 packet does) | native op | evidence):
//   0x869d60 | SET_CONTEXT_REG DB_DEPTH_CONTROL (0x200) = arg2 & 0xc07007ff, 1 value            |
//             NativeSetRegs(0xA200, v)                                                          |
//             evidence: out/re/decomp/0086/00869d60.c:9-18 (callers FUN_00091e10 FUN_00093960);
//             raw bytes checked y (reg C7 40 04 00 02 00 00; and 0xc07007ff); REA not used
//   0x869e80 | SET_CONTEXT_REG DB_DEPTH_CONTROL (0x200) = 0 (no value argument)                 |
//             NativeSetRegs(0xA200, 0)                                                          |
//             evidence: out/re/decomp/0086/00869e80.c:9-18; raw bytes checked y (store of 0);
//             REA not used
//   0x869dc0 | SET_CONTEXT_REG DB_STENCIL_CONTROL (0x10b) = arg2 & 0xffffff, 1 value             |
//             NativeSetRegs(0xA10B, v)                                                          |
//             evidence: out/re/decomp/0086/00869dc0.c:9-18 (callers FUN_00091e10); raw bytes
//             checked y (reg 0x10b, and 0xffffff); REA not used
//   0x869bb0 | SET_CONTEXT_REG 2 values: DB_STENCILREFMASK (0x10c) = arg2 (front),                |
//             DB_STENCILREFMASK_BF (0x10d) = arg3 (back); unmasked                              |
//             NativeSetRegs(0xA10C, {front, back}, 2)                                           |
//             evidence: out/re/decomp/0086/00869bb0.c:9-20 (6 callers, as the spec says);
//             raw bytes checked y (header C7 00 00 69 02 C0, reg 0x10c, two stores); REA not used
//   0x869b50 | SET_CONTEXT_REG 2 values, both = arg2: DB_STENCILREFMASK (0x10c) and BF (0x10d)     |
//             NativeSetRegs(0xA10C, {v, v}, 2)                                                  |
//             evidence: out/re/decomp/0086/00869b50.c:9-20; raw bytes checked y (reg 0x10c,
//             stored twice); REA not used
//   0x869f50 | SET_CONTEXT_REG DB_DEPTH_CLEAR (0xb) = float xmm0 (bits), 1 value                    |
//             NativeSetRegs(0xA00B, bits(xmm0))                                                 |
//             evidence: out/re/decomp/0086/00869f50.c:9-18 (5 callers); raw bytes checked y
//             (vmovss spill of xmm0; reg 0x0b; vmovss store into [cursor+8]); REA not used
//   0x869fb0 | SET_CONTEXT_REG DB_STENCIL_CLEAR (0xa) = arg2 & 0xff, 1 value                     |
//             NativeSetRegs(0xA00A, v)                                                          |
//             evidence: out/re/decomp/0086/00869fb0.c:9-18; raw bytes checked y (movzx of the low
//             byte, reg 0x0a); REA not used
//   0x869ee0 | SET_CONTEXT_REG 2 values: DB_DEPTH_BOUNDS_MIN (0x8) = float xmm0,                    |
//             DB_DEPTH_BOUNDS_MAX (0x9) = float xmm1 (bits)                                     |
//             NativeSetRegs(0xA008, {min, max}, 2)                                              |
//             evidence: out/re/decomp/0086/00869ee0.c:9-22; raw bytes checked y (vmovss of xmm0 and
//             xmm1 spills, cb in rdi, reg 0x08, count 2); REA not used
//   0x86dd20 | SET_CONTEXT_REG DB_RENDER_CONTROL (0x000) = arg2 & 0x1fff, 1 value                  |
//             NativeSetRegs(0xA000, v)                                                          |
//             evidence: out/re/decomp/0086/0086dd20.c:9-18 (callers FUN_00093ef0 FUN_00094600);
//             raw bytes checked y (and 0x1fff, reg 0); REA not used
//   0x86be10 | SET_CONTEXT_REG DB_RENDER_OVERRIDE (0x003) = arg2 & 0xfc1f1fff, 1 value            |
//             NativeSetRegs(0xA003, v)                                                          |
//             evidence: out/re/decomp/0086/0086be10.c:9-18 (callers FUN_00093ef0 FUN_00094600);
//             raw bytes checked y (and 0xfc1f1fff, reg 3); REA not used
//   0x86be70 | SET_CONTEXT_REG DB_RENDER_OVERRIDE2 (0x004) = arg2 & 0x60, 1 value                 |
//             NativeSetRegs(0xA004, v)                                                          |
//             evidence: out/re/decomp/0086/0086be70.c:9-18; raw bytes checked y (and 0x60, reg 4);
//             REA not used
//   0x86de50 | SET_CONTEXT_REG DB_COUNT_CONTROL (0x001) = ((arg3 & 7) << 4) + 0x11000100 +       |
//             ((arg2 & 1) << 1), 1 value                                                        |
//             NativeSetRegs(0xA001, v)                                                          |
//             evidence: out/re/decomp/0086/0086de50.c:9-20; raw bytes checked y (shl/and of edx
//             and esi, lea with 0x11000100, reg 1); REA not used
//   0x869e20 | SET_CONTEXT_REG DB_EQAA (0x201) = arg2 & 0x0f3f7777, 1 value                        |
//             NativeSetRegs(0xA201, v)                                                          |
//             evidence: out/re/decomp/0086/00869e20.c:9-18; raw bytes checked y (and 0x0f3f7777,
//             reg 0x201); REA not used
//   0x8699f0 | SET_CONTEXT_REG DB_ALPHA_TO_MASK (0x2dc) = arg2 (no mask), 1 value                   |
//             NativeSetRegs(0xA2DC, v)                                                          |
//             evidence: out/re/decomp/0086/008699f0.c:9-18; raw bytes checked y (reg 0x2dc,
//             unmasked store); REA not used
// Round 3 (X2, context emitters B). The following are NOT DB registers; they were assigned to this
// file by VA, so the rows use the db class. Register owners (for the lead's duplicate check): the
// scissor and VGT registers below belong to the raster / VGT workers (W03, W05). Raw bytes read
// from out/eboot.elf (file offset VA + 0x4000) with llvm-objdump; all checked y. REA not used.
//   0x86bb90 | SET_CONTEXT_REG 0x2a8 (VGT_INSTANCE_STEP_RATE_0), 2 values: arg2, arg3 unmasked       |
//             NativeSetRegs(0xA2A8, {a2, a3}, 2)                                                    |
//             evidence: 0x86bb90 decomp; raw bytes (header C0026900, reg 0x2a8, stores of esi, edx) |
//   0x86c4d0 | SET_CONTEXT_REG 0x290 (VGT_GS_MODE) = (a3 & 3) << 4 | (a2 & 0xffffffcf); when a2 == 0  |
//             a second SET_CONTEXT_REG 0x2e5 (VGT_STRMOUT_CONFIG) = 0 follows                       |
//             NativeSetRegs(0xA290, v, 1) and, if a2 == 0, NativeSetRegs(0xA2E5, 0, 1), one op     |
//             evidence: 0x86c4d0 decomp; raw bytes (and -0x31, and 0x30, second packet at +0xc)     |
//   0x86c570 | SET_CONTEXT_REG 0x291 (VGT_GS_ONCHIP_CONTROL) = (a3 & 0x7ff) << 11 | (a2 & 0x7ff)     |
//             NativeSetRegs(0xA291, v, 1)                                                           |
//             evidence: 0x86c570 decomp; raw bytes (and 0x3ff800, and 0x7ff)                        |
//   0x86c8a0 | SET_CONTEXT_REG 0x2a1 (ENABLE_PRIMITIVE_ID) = a2 & 0xff                               |
//             NativeSetRegs(0xA2A1, v, 1)                                                           |
//             evidence: 0x86c8a0 decomp; raw bytes (movzx of sil)                                   |
//   0x86d080 | SET_CONTEXT_REG 2 values at the register named by table[a2] (eboot 0x128fed0, absolute |
//             dword), values a3, a4 unmasked. Packet reg field = table[a2] - 0xA000                 |
//             NativeSetRegs(table[a2], {a3, a4}, 2); table read at detour time via GuestEntry;     |
//             a2 >= 0x100 or an entry outside [0xA000, NumRegs - 2] goes to the guest body          |
//             evidence: 0x86d080 decomp; raw bytes (lea of 0x128fed0, add 0xffff6000, index a2*4)  |
//   0x86d100 | SET_CONTEXT_REG 0x2e6 (VGT_STRMOUT_BUFFER_CONFIG) = *src (read by the emitter)        |
//             NativeSetRegs(0xA2E6, v, 1)                                                           |
//             evidence: 0x86d100 decomp; raw bytes (mov ecx, [r14]; header C0016900)                |
//   0x86d730 | SET_CONTEXT_REG 0x2f9 = (a4 & 1) | ((a2 & 7) * 8 + (a3 & 3) * 2)                     |
//             NativeSetRegs(0xA2F9, v, 1); 0x2f9 has no name in regs.cpp                            |
//             evidence: 0x86d730 decomp; raw bytes (and 7/3/1, lea [r12 + 2*r15])                  |
//   0x86d9e0 | SET_CONTEXT_REG 0x81 (PA_SC window scissor), 2 values: w0 = (a6 == 0) << 31 |        |
//             (a3 & 0x7fff) << 16 | (a2 & 0x7fff); w1 = (a5 & 0x7fff) << 16 | (a4 & 0x7fff)          |
//             NativeSetRegs(0xA081, {w0, w1}, 2)                                                    |
//             evidence: 0x86d9e0 decomp; raw bytes (a6 in r9d, sete / shl 0x1f, masks 0x7fff)      |
//   0x86da90 | same two words as 0x86d9e0, into 0x90 (PA_SC generic scissor)                        |
//             NativeSetRegs(0xA090, {w0, w1}, 2)                                                    |
//             evidence: 0x86da90 decomp; raw bytes (same instruction shape, reg 0x90)               |
//
// Emitters found and NOT replaced here (skipped_owned_by_other):
//   0x86e3a0 (0x2de), 0x86b6f0 (0x2dd), 0x86e340 (0x2df): first register is in the PA_SU polygon
//             offset block (regs_vertex.h PolygonOffset at 0xA2DF; 0x2de/0x2dd sit beside it), so
//             by the register-ownership rule these are W03's. The spec named 0x86e3a0 as a W02
//             example; it is not a DB register, so it is left to W03 (decision for the lead).
//   0x869890 (0x1ba, value (arg3 & 0x1fff) << 12 | (arg2 & 0xfff)): 0x1ba is not in the W02
//             register list. W01 wrote it down as a DB-block emitter; the owner must be decided
//             from the register, which is not in any DB struct here.
//   DB_SHADER_CONTROL (0x203): no eboot SET_CONTEXT_REG emitter for it was found in the decompile
//             tree. The host gnmdriver shader binders write it (gnmdriver.cpp ~1615-2717, the
//             ps_regs[10] writes), which belongs to W08 (shaders).
//   Boundary 2: the Table has no depth/stencil slot and gnmdriver.cpp has no sceGnm DB entry point,
//             so RegisterW02 sets nothing.
//
// REA (Ghidra session) was not open (no app loaded), so every check used the flat exports under
// out/re/decomp/0086/ and raw bytes read from out/eboot.elf (file offset = VA + 0x4000).

#include "native/gnm/gnm_common.h"

#include <array>
#include <cstring>

#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"

namespace Native::Gnm {

namespace {

// Context register dword offsets (relative to AmdGpu::Regs::ContextRegWordOffset, 0xA000).
constexpr u32 kDbRenderControl = 0x000;
constexpr u32 kDbCountControl = 0x001;
constexpr u32 kDbRenderOverride = 0x003;
constexpr u32 kDbRenderOverride2 = 0x004;
constexpr u32 kDbDepthBoundsMin = 0x008; // max is +1
constexpr u32 kDbStencilClear = 0x00a;
constexpr u32 kDbDepthClear = 0x00b;
constexpr u32 kDbStencilControl = 0x10b;
constexpr u32 kDbStencilRefMask = 0x10c; // back is +1
constexpr u32 kDbDepthControl = 0x200;
constexpr u32 kDbEqaa = 0x201;
constexpr u32 kDbAlphaToMask = 0x2dc;

// Registers of the round-3 block (0x86bb90 .. 0x86da90). None is a DB register: the owners are
// the VGT / PA_SC workers. Kept here because the emitters were assigned to this file by VA.
constexpr u32 kVgtInstanceStepRate0 = 0x2a8; // 2 values: 0x2a8, 0x2a9
constexpr u32 kEnablePrimitiveId = 0x2a1;
constexpr u32 kVgtGsMode = 0x290;
constexpr u32 kVgtGsOnchipControl = 0x291;
constexpr u32 kVgtStrmoutConfig = 0x2e5;
constexpr u32 kVgtStrmoutBufferConfig = 0x2e6;
constexpr u32 kReg2f9 = 0x2f9; // not named in regs.cpp (next to aa_config 0xA2F8)
constexpr u32 kWindowScissor = 0x081; // 2 values: 0x81, 0x82
constexpr u32 kGenericScissor = 0x090; // 2 values: 0x90, 0x91

// Table of absolute context dwords indexed by the first argument of 0x86d080 (eboot VA 0x128fed0,
// one u32 per index; entry 4 is 0, which the guest would write out of range).
constexpr u64 kIndexedRegTableVa = 0x128fed0;
// Indices at or above this stay on the guest body: the table length is not known from the eboot.
constexpr u32 kIndexedRegTableMaxIndex = 0x100;

// Guest SysV signatures of the detoured emitters (called from host code on the guest stack).
typedef void (*GuestW02Cb)(void*) __attribute__((sysv_abi));
typedef void (*GuestW02CbValue)(void*, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW02CbPair)(void*, unsigned int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW02FloatCb)(float, void*) __attribute__((sysv_abi));
typedef void (*GuestW02FloatPairCb)(float, float, void*) __attribute__((sysv_abi));
typedef void (*GuestW02CbTriple)(void*, unsigned int, unsigned int, unsigned int)
    __attribute__((sysv_abi));
typedef void (*GuestW02CbPtr)(void*, const unsigned int*) __attribute__((sysv_abi));
typedef void (*GuestW02CbQuint)(void*, unsigned int, unsigned int, unsigned int, unsigned int,
                                unsigned int) __attribute__((sysv_abi));

// Writes a value (or a run of values) into the context register file and marks it for the draw
// thread, exactly as the SetContextReg handler does. Runs on the command processor only.
void WriteCtx(u32 reg_offset, const u32* values, u32 count) {
    Lv().NativeSetRegs(AmdGpu::Regs::ContextRegWordOffset + reg_offset, values, count);
}

// The bit pattern of a float argument (the guest stores the xmm lane unchanged).
u32 FloatBits(float f) {
    u32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Writes into the register file at an absolute dword (the form the indexed emitter 0x86d080 uses).
void WriteCtxAbs(u32 abs_dword, const u32* values, u32 count) {
    Lv().NativeSetRegs(abs_dword, values, count);
}

// Absolute dword that 0x86d080 writes for table index `index` (its packet reg field is that dword
// minus ContextRegWordOffset, so the two forms are the same register). False when the native
// write would leave the register file: the guest body then runs as before.
bool IndexedCtxAbs(u32 index, u32 num_dwords, u32& abs_dword) {
    if (index >= kIndexedRegTableMaxIndex) {
        return false;
    }
    const auto* table = reinterpret_cast<const u32*>(GuestEntry(kIndexedRegTableVa));
    const u32 abs = table[index];
    if (abs < AmdGpu::Regs::ContextRegWordOffset ||
        abs > static_cast<u32>(AmdGpu::Regs::NumRegs) - num_dwords) {
        return false;
    }
    abs_dword = abs;
    return true;
}

// The two words of 0x86d9e0 / 0x86da90 (raw bytes): word 0 = (p6 == 0) << 31 | (p3 & 0x7fff) << 16
// | (p2 & 0x7fff); word 1 = (p5 & 0x7fff) << 16 | (p4 & 0x7fff).
std::array<u32, 2> ScissorPair(u32 p2, u32 p3, u32 p4, u32 p5, u32 p6) {
    const u32 w0 = (p6 == 0 ? 0x80000000u : 0u) | ((p3 & 0x7fffu) << 16) | (p2 & 0x7fffu);
    const u32 w1 = ((p5 & 0x7fffu) << 16) | (p4 & 0x7fffu);
    return {w0, w1};
}

} // namespace

} // namespace Native::Gnm

using Native::Gnm::Class;

// 0x869d60: SET_CONTEXT_REG DB_DEPTH_CONTROL, value = arg2 & 0xc07007ff.
extern "C" __attribute__((sysv_abi)) void GnmW02_869d60(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:869d60");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869d60)(cb, value);
        return;
    }
    const u32 v = value & 0xc07007ffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbDepthControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869d60)(cb, value);
}

// 0x869e80: SET_CONTEXT_REG DB_DEPTH_CONTROL = 0.
extern "C" __attribute__((sysv_abi)) void GnmW02_869e80(void* cb) {
    static const u32 site = Native::Gnm::Site("w02:869e80");
    using Fn = Native::Gnm::GuestW02Cb;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869e80)(cb);
        return;
    }
    const u32 v = 0u;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbDepthControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869e80)(cb);
}

// 0x869dc0: SET_CONTEXT_REG DB_STENCIL_CONTROL, value = arg2 & 0xffffff.
extern "C" __attribute__((sysv_abi)) void GnmW02_869dc0(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:869dc0");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869dc0)(cb, value);
        return;
    }
    const u32 v = value & 0xffffffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbStencilControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869dc0)(cb, value);
}

// 0x869bb0: SET_CONTEXT_REG DB_STENCILREFMASK (front) and DB_STENCILREFMASK_BF (back), unmasked.
extern "C" __attribute__((sysv_abi)) void GnmW02_869bb0(void* cb, unsigned int front,
                                                         unsigned int back) {
    static const u32 site = Native::Gnm::Site("w02:869bb0");
    using Fn = Native::Gnm::GuestW02CbPair;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869bb0)(cb, front, back);
        return;
    }
    const std::array<u32, 2> v{front, back};
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbStencilRefMask, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869bb0)(cb, front, back);
}

// 0x869b50: SET_CONTEXT_REG DB_STENCILREFMASK and DB_STENCILREFMASK_BF, both = arg2.
extern "C" __attribute__((sysv_abi)) void GnmW02_869b50(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:869b50");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869b50)(cb, value);
        return;
    }
    const std::array<u32, 2> v{value, value};
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbStencilRefMask, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869b50)(cb, value);
}

// 0x869f50: SET_CONTEXT_REG DB_DEPTH_CLEAR = the float in xmm0 (raw bits).
extern "C" __attribute__((sysv_abi)) void GnmW02_869f50(float value, void* cb) {
    static const u32 site = Native::Gnm::Site("w02:869f50");
    using Fn = Native::Gnm::GuestW02FloatCb;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869f50)(value, cb);
        return;
    }
    const u32 v = Native::Gnm::FloatBits(value);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbDepthClear, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869f50)(value, cb);
}

// 0x869fb0: SET_CONTEXT_REG DB_STENCIL_CLEAR, value = arg2 & 0xff.
extern "C" __attribute__((sysv_abi)) void GnmW02_869fb0(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:869fb0");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869fb0)(cb, value);
        return;
    }
    const u32 v = value & 0xffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbStencilClear, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869fb0)(cb, value);
}

// 0x869ee0: SET_CONTEXT_REG DB_DEPTH_BOUNDS_MIN (xmm0) and DB_DEPTH_BOUNDS_MAX (xmm1), raw bits.
extern "C" __attribute__((sysv_abi)) void GnmW02_869ee0(float min, float max, void* cb) {
    static const u32 site = Native::Gnm::Site("w02:869ee0");
    using Fn = Native::Gnm::GuestW02FloatPairCb;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869ee0)(min, max, cb);
        return;
    }
    const std::array<u32, 2> v{Native::Gnm::FloatBits(min), Native::Gnm::FloatBits(max)};
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbDepthBoundsMin, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869ee0)(min, max, cb);
}

// 0x86dd20: SET_CONTEXT_REG DB_RENDER_CONTROL, value = arg2 & 0x1fff.
extern "C" __attribute__((sysv_abi)) void GnmW02_86dd20(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:86dd20");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86dd20)(cb, value);
        return;
    }
    const u32 v = value & 0x1fffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbRenderControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86dd20)(cb, value);
}

// 0x86be10: SET_CONTEXT_REG DB_RENDER_OVERRIDE, value = arg2 & 0xfc1f1fff.
extern "C" __attribute__((sysv_abi)) void GnmW02_86be10(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:86be10");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86be10)(cb, value);
        return;
    }
    const u32 v = value & 0xfc1f1fffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbRenderOverride, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86be10)(cb, value);
}

// 0x86be70: SET_CONTEXT_REG DB_RENDER_OVERRIDE2, value = arg2 & 0x60.
extern "C" __attribute__((sysv_abi)) void GnmW02_86be70(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:86be70");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86be70)(cb, value);
        return;
    }
    const u32 v = value & 0x60u;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbRenderOverride2, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86be70)(cb, value);
}

// 0x86de50: SET_CONTEXT_REG DB_COUNT_CONTROL,
// value = ((mode & 7) << 4) + 0x11000100 + ((bit & 1) << 1), where bit = esi, mode = edx.
extern "C" __attribute__((sysv_abi)) void GnmW02_86de50(void* cb, unsigned int bit,
                                                         unsigned int mode) {
    static const u32 site = Native::Gnm::Site("w02:86de50");
    using Fn = Native::Gnm::GuestW02CbPair;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86de50)(cb, bit, mode);
        return;
    }
    const u32 v = ((mode & 7u) << 4) + 0x11000100u + ((bit & 1u) << 1);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbCountControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86de50)(cb, bit, mode);
}

// 0x869e20: SET_CONTEXT_REG DB_EQAA, value = arg2 & 0x0f3f7777.
extern "C" __attribute__((sysv_abi)) void GnmW02_869e20(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:869e20");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869e20)(cb, value);
        return;
    }
    const u32 v = value & 0x0f3f7777u;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbEqaa, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869e20)(cb, value);
}

// 0x8699f0: SET_CONTEXT_REG DB_ALPHA_TO_MASK, value = arg2 (unmasked).
extern "C" __attribute__((sysv_abi)) void GnmW02_8699f0(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w02:8699f0");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x8699f0)(cb, value);
        return;
    }
    const u32 v = value;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kDbAlphaToMask, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x8699f0)(cb, value);
}

// 0x86bb90: SET_CONTEXT_REG 2 values at 0x2a8 (VGT_INSTANCE_STEP_RATE_0): arg2, arg3, unmasked.
extern "C" __attribute__((sysv_abi)) void GnmW02_86bb90(void* cb, unsigned int a2,
                                                         unsigned int a3) {
    static const u32 site = Native::Gnm::Site("w02:86bb90");
    using Fn = Native::Gnm::GuestW02CbPair;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86bb90)(cb, a2, a3);
        return;
    }
    const std::array<u32, 2> v{a2, a3};
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kVgtInstanceStepRate0, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86bb90)(cb, a2, a3);
}

// 0x86c4d0: SET_CONTEXT_REG VGT_GS_MODE (0x290) = (a3 & 3) << 4 | (a2 & 0xffffffcf). When a2 == 0
// the emitter also writes a second packet, 0x2e5 (VGT_STRMOUT_CONFIG) = 0, in the same buffer.
extern "C" __attribute__((sysv_abi)) void GnmW02_86c4d0(void* cb, unsigned int a2,
                                                         unsigned int a3) {
    static const u32 site = Native::Gnm::Site("w02:86c4d0");
    using Fn = Native::Gnm::GuestW02CbPair;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86c4d0)(cb, a2, a3);
        return;
    }
    const u32 v = ((a3 & 3u) << 4) | (a2 & 0xffffffcfu);
    const bool strmout_zero = (a2 == 0);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v, strmout_zero] {
            Native::Gnm::WriteCtx(Native::Gnm::kVgtGsMode, &v, 1);
            if (strmout_zero) {
                const u32 zero = 0;
                Native::Gnm::WriteCtx(Native::Gnm::kVgtStrmoutConfig, &zero, 1);
            }
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86c4d0)(cb, a2, a3);
}

// 0x86c570: SET_CONTEXT_REG VGT_GS_ONCHIP_CONTROL (0x291) = (a3 & 0x7ff) << 11 | (a2 & 0x7ff).
extern "C" __attribute__((sysv_abi)) void GnmW02_86c570(void* cb, unsigned int a2,
                                                         unsigned int a3) {
    static const u32 site = Native::Gnm::Site("w02:86c570");
    using Fn = Native::Gnm::GuestW02CbPair;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86c570)(cb, a2, a3);
        return;
    }
    const u32 v = ((a3 & 0x7ffu) << 11) | (a2 & 0x7ffu);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kVgtGsOnchipControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86c570)(cb, a2, a3);
}

// 0x86c8a0: SET_CONTEXT_REG ENABLE_PRIMITIVE_ID (0x2a1) = arg2 & 0xff.
extern "C" __attribute__((sysv_abi)) void GnmW02_86c8a0(void* cb, unsigned int a2) {
    static const u32 site = Native::Gnm::Site("w02:86c8a0");
    using Fn = Native::Gnm::GuestW02CbValue;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86c8a0)(cb, a2);
        return;
    }
    const u32 v = a2 & 0xffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kEnablePrimitiveId, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86c8a0)(cb, a2);
}

// 0x86d080: SET_CONTEXT_REG 2 values at the register that table entry a2 names (eboot 0x128fed0),
// values a3, a4 unmasked. The packet reg field is that absolute dword minus 0xA000.
extern "C" __attribute__((sysv_abi)) void GnmW02_86d080(void* cb, unsigned int a2,
                                                         unsigned int a3, unsigned int a4) {
    static const u32 site = Native::Gnm::Site("w02:86d080");
    using Fn = Native::Gnm::GuestW02CbTriple;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86d080)(cb, a2, a3, a4);
        return;
    }
    u32 abs = 0;
    if (Native::Gnm::IndexedCtxAbs(a2, 2, abs) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [abs, a3, a4] {
            const std::array<u32, 2> v{a3, a4};
            Native::Gnm::WriteCtxAbs(abs, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86d080)(cb, a2, a3, a4);
}

// 0x86d100: SET_CONTEXT_REG VGT_STRMOUT_BUFFER_CONFIG (0x2e6) = *src (the guest reads it before
// the packet is written; the native path reads it at the same point).
extern "C" __attribute__((sysv_abi)) void GnmW02_86d100(void* cb, const unsigned int* src) {
    static const u32 site = Native::Gnm::Site("w02:86d100");
    using Fn = Native::Gnm::GuestW02CbPtr;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86d100)(cb, src);
        return;
    }
    const u32 v = *src;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kVgtStrmoutBufferConfig, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86d100)(cb, src);
}

// 0x86d730: SET_CONTEXT_REG 0x2f9 = (a4 & 1) | (a2 & 7) * 8 + (a3 & 3) * 2.
extern "C" __attribute__((sysv_abi)) void GnmW02_86d730(void* cb, unsigned int a2,
                                                         unsigned int a3, unsigned int a4) {
    static const u32 site = Native::Gnm::Site("w02:86d730");
    using Fn = Native::Gnm::GuestW02CbTriple;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86d730)(cb, a2, a3, a4);
        return;
    }
    const u32 v = (a4 & 1u) | ((a2 & 7u) * 8u + (a3 & 3u) * 2u);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kReg2f9, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86d730)(cb, a2, a3, a4);
}

// 0x86d9e0: SET_CONTEXT_REG 2 values at PA_SC window scissor (0x81): ScissorPair(a2..a6).
extern "C" __attribute__((sysv_abi)) void GnmW02_86d9e0(void* cb, unsigned int a2,
                                                         unsigned int a3, unsigned int a4,
                                                         unsigned int a5, unsigned int a6) {
    static const u32 site = Native::Gnm::Site("w02:86d9e0");
    using Fn = Native::Gnm::GuestW02CbQuint;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86d9e0)(cb, a2, a3, a4, a5, a6);
        return;
    }
    const std::array<u32, 2> v = Native::Gnm::ScissorPair(a2, a3, a4, a5, a6);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kWindowScissor, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86d9e0)(cb, a2, a3, a4, a5, a6);
}

// 0x86da90: SET_CONTEXT_REG 2 values at PA_SC generic scissor (0x90): ScissorPair(a2..a6).
extern "C" __attribute__((sysv_abi)) void GnmW02_86da90(void* cb, unsigned int a2,
                                                         unsigned int a3, unsigned int a4,
                                                         unsigned int a5, unsigned int a6) {
    static const u32 site = Native::Gnm::Site("w02:86da90");
    using Fn = Native::Gnm::GuestW02CbQuint;
    if (!Native::Gnm::Enabled(Class::Db)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86da90)(cb, a2, a3, a4, a5, a6);
        return;
    }
    const std::array<u32, 2> v = Native::Gnm::ScissorPair(a2, a3, a4, a5, a6);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kGenericScissor, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86da90)(cb, a2, a3, a4, a5, a6);
}

// The rest of the file: RegisterW02 has nothing to add to the sceGnm* table. The Table has no
// depth/stencil slot and gnmdriver.cpp has no sceGnm entry point that writes these registers.
namespace Native::Gnm {

void RegisterW02(Table& t) {
    (void)t;
}

} // namespace Native::Gnm
