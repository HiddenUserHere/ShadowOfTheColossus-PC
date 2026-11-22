// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W01: Context registers: colour / blend.
// Class Cb (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w01_ctx_cb.h, and
// gpu/sotc/hook_gnm_w01.inc. See gnm_common.h for the contract.
//
// Each detour is the eboot emitter that writes one SET_CONTEXT_REG (0x69) packet. The guest body
// appends header (0xC0016900 or 0xC0046900), register offset (relative to ContextRegWordOffset
// 0xA000) and the payload dwords to the GNM command buffer; the native replacement appends a NOP
// through EmitCp, and its op runs inline on the command processor and performs exactly what the
// SetContextReg handler does for that packet (liverpool.cpp ~line 1485: memcpy into regs.reg_array,
// MarkRegs; none of the W01 registers has a special case in that switch).
//
// Coverage table (site | contract (what the PM4 packet does) | native op | evidence):
//   0x869990 | SET_CONTEXT_REG 0x8e = CB_TARGET_MASK, one value dword = the 2nd argument            |
//             Native::Gnm::EmitCp: NativeSetRegs(0xA000+0x8e, value)                             |
//             evidence: out/re/decomp/0086/00869990.c (callers FUN_00093960 FUN_00093ef0
//             FUN_00091b50 FUN_000919e0); raw bytes checked y (header C7 00 00 69 01 C0, reg
//             C7 40 04 8E 00 00 00); REA not used (decomp + raw bytes sufficient)
//   0x869a50 | SET_CONTEXT_REG 0x1e0+index = CB_BLENDn_CONTROL, value = arg3 & 0xffff1fff          |
//             Native::Gnm::EmitCp: NativeSetRegs(0xA000+0x1e0+index, value & 0xffff1fff); index
//             outside 0..7 falls back to the guest body                                          |
//             evidence: out/re/decomp/0086/00869a50.c (callers FUN_00093960 FUN_00091b50
//             FUN_000919e0); raw bytes checked y (and 0xffff1fff immediate, add 0x1e0); REA not used
//   0x869ce0 | SET_CONTEXT_REG 0x202 = CB_COLOR_CONTROL, value = ((arg2 & 7) << 4) | ((arg3 & 0xff) << 16) |
//             Native::Gnm::EmitCp: NativeSetRegs(0xA000+0x202, value)                            |
//             evidence: out/re/decomp/0086/00869ce0.c (callers FUN_00093960 FUN_00093ef0
//             FUN_00094600); raw bytes checked y (shl 4 / shl 16 / masks 0x70, 0xff0000); REA not used
//   0x869ac0 | SET_CONTEXT_REG 0x105 = CB_BLEND_RED, 4 payload dwords (RED GREEN BLUE ALPHA)          |
//             Native::Gnm::EmitCp: NativeSetRegs(0xA000+0x105, {r,g,b,a}, 4)                    |
//             arguments are float (SSE) in xmm0..xmm3, stored as raw 32-bit lanes; decompile typed
//             them as undefined4, the raw code reads xmm0..3 (vmovss); evidence:
//             out/re/decomp/0086/00869ac0.c; raw bytes checked y (header C7 00 00 69 04 C0,
//             reg C7 40 04 05 01 00 00); no static direct callers found by Ghidra ("called by:"
//             empty), so the float ABI is taken from the callee only; REA not used
//
// Round 3 (X1) rows, decoded from the raw prologues (tools/re/gnm_abi_audit.py reads them too).
// The SetContextReg handler takes the register from DW1 bits 0..15 (BitField), so the CI index
// bits of the generic emitters never reach the register file:
//   0x869370 | generic SET_CONTEXT_REG: reg = esi - 0xA000 (16-bit), value = edx, index = ecx
//             (DW1 bits 28..31 only when FUN_00882ec0 = sceKernelIsNeoMode, ignored by the handler)
//             | EmitCtxWrite(reg, value); guest body if reg is an extent register or leaves the window
//             | raw: push/mov prologue, call 0x882ec0, shl ecx 0x1c, and esi 0xf0000000 | esi-0xa000
//   0x8694e0 | generic SET_CONTEXT_REG range: reg = esi - 0xA000, count = ecx, values = rdx (copied
//             at call time, as memcpy does) | EmitCp(WriteCtx(reg, copy, count)); guest body on the
//             same conditions | raw: shl ecx 0x10 | 0xC0006900; memcpy(payload = out + 8, rdx, count * 4)
//   0x869890 | SET_CONTEXT_REG 0x1ba (abs 0xA1BA): value = ((edx & 0x1fff) << 12) | (esi & 0xfff)
//             | EmitCtxWrite(0x1ba, value) | raw: header C0016900, dword 0x1ba (imm), and/shl masks.
//             Not in the DB register list of W02; kept here as the round-3 assignment says.
//   0x869c20 | SET_CONTEXT_REG 0x2b0 = esi | EmitCtxWrite(0x2b0, value) | raw: imm 0x2b0, mov [rax+8], r14d
//   0x869c80 | SET_CONTEXT_REG 0x2b1 = esi | EmitCtxWrite(0x2b1, value) | raw: imm 0x2b1
//   0x86a120 | SET_CONTEXT_REG 0x80 (PA_SC_WINDOW_SCISSOR_TL, abs 0xA080): value = (edx << 16) |
//             (esi & 0xffff) | EmitCtxWrite(0x80, value) | raw: imm 0x80, shl r14d 0x10, movzx r15w
//   0x86a3d0 | two packets: 0x30e = low dword and 0x30f = high dword of rsi (64-bit; PA_SC_AA_MASK)
//             | one two-dword WriteCtx(0x30e, {lo, hi}) (same effect, same order) | raw: imm 0x30e,
//             0x30f, shr r14 0x20, rsi used as 64-bit
//   0x86a970 | SET_CONTEXT_REG 0x102 = esi | EmitCtxWrite(0x102, value) | raw: imm 0x102
//   0x86bb30 | SET_CONTEXT_REG 0x2ad = (esi & 0xff) ^ 1 | EmitCtxWrite(0x2ad, value) | raw: movzx,
//             xor 1, imm 0x2ad
// None of the fixed registers above is a CB_COLORn / CB_COLORn_CMASK / DB_Z_INFO register, so no
// extent hint applies to them. The generic emitters 0x869370 and 0x8694e0 fall back to the guest body
// when their runtime register is one of those (the hint is read from the trailing NOP, which a native
// op cannot see; no static caller of either exists).
//
// Emitters of W01 registers NOT replaced by the rows above: 0x86a010 and 0x86c5f0 (not CB) ->
// W03; 0x86d080 (generic, register taken from the caller, not analysed) -> left to the owner.
//   0x8f (CB_SHADER_MASK): no emitter with a 0x8f register found in out/re/decomp/0085..0088.
//   0x86a1a0 (CB_COLORn surfaces, 0x318+n*0xf)       -> W04
//   0x869890/0x8699f0/0x869b50/0x869bb0 (DB block)   -> W02
//   0x86e5e0 (22-dword default-state block)          -> W05

#include "native/gnm/gnm_common.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"

namespace Native::Gnm {

namespace {

// Context register dword offsets (relative to AmdGpu::Regs::ContextRegWordOffset, 0xA000).
constexpr u32 kCbTargetMask = 0x8e;
constexpr u32 kCbBlend0Control = 0x1e0;
constexpr u32 kCbBlendRed = 0x105;
constexpr u32 kCbColorControl = 0x202;

// Guest SysV signatures of the detoured emitters (called from host code on the guest stack).
typedef void (*GuestW01Cb)(void*, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW01CbIdx)(void*, int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW01CbPair)(void*, unsigned int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW01CbFloat4)(void*, float, float, float, float) __attribute__((sysv_abi));
// Round 3 (X1) guest signatures, from the raw prologues (gnm_abi_audit.py).
typedef void (*GuestW01Ctx3)(void*, unsigned int, unsigned int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW01CtxRange)(void*, unsigned int, const unsigned int*, unsigned int)
    __attribute__((sysv_abi));
typedef void (*GuestW01CbU64)(void*, unsigned long long) __attribute__((sysv_abi));

// Writes a value (or a run of values) into the context register file and marks it for the draw
// thread, exactly as the SetContextReg handler does. Runs on the command processor only.
void WriteCtx(u32 reg_offset, const u32* values, u32 count) {
    Lv().NativeSetRegs(AmdGpu::Regs::ContextRegWordOffset + reg_offset, values, count);
}

// --- Round 3 (X1): context emitters A (generic and fixed-register SET_CONTEXT_REG) ----------------
// The SetContextReg handler (liverpool.cpp) takes the register from bits 0..15 of the packet's
// second dword only (PM4CmdSetData::reg_offset, BitField<0, 16>). Bits 28..31 (the CI+ index) are
// never read, so a native write needs no index, and the Neo-mode predicate of 0x869370 (guest
// FUN_00882ec0 = sceKernelIsNeoMode, which only decides those index bits) changes no register.
constexpr u32 kCtxRegWindow = AmdGpu::Regs::NumRegs - AmdGpu::Regs::ContextRegWordOffset; // 0x3000

// The context register dword offset (relative to 0xA000) that a packet built from the absolute
// address `reg_abs` addresses, as the handler decodes it.
u32 CtxRelOf(u32 reg_abs) {
    return (reg_abs - AmdGpu::Regs::ContextRegWordOffset) & 0xffffu;
}

// Whether a SetContextReg whose first register is `rel` takes a CB/DB extent hint from the guest's
// trailing NOP (liverpool.cpp SetContextReg switch). A native op cannot see that NOP, so such a
// packet keeps the guest body, and the handler then reads the hint itself.
bool TakesExtentHint(u32 rel) {
    using R = AmdGpu::Liverpool::ContextRegs;
    switch (AmdGpu::Regs::ContextRegWordOffset + rel) {
    case R::DbZInfo:
    case R::CbColor0Base:
    case R::CbColor1Base:
    case R::CbColor2Base:
    case R::CbColor3Base:
    case R::CbColor4Base:
    case R::CbColor5Base:
    case R::CbColor6Base:
    case R::CbColor7Base:
    case R::CbColor0Cmask:
    case R::CbColor1Cmask:
    case R::CbColor2Cmask:
    case R::CbColor3Cmask:
    case R::CbColor4Cmask:
    case R::CbColor5Cmask:
    case R::CbColor6Cmask:
    case R::CbColor7Cmask:
        return true;
    default:
        return false;
    }
}

// Whether the native op may write `count` dwords at context offset `rel`: inside the context
// register window of the register file and not an extent-hint register. `count` may be 0 (the
// packet then writes nothing, but an extent-hint register still changes the handler's extent).
bool CtxWriteOk(u32 rel, u32 count) {
    return rel <= kCtxRegWindow && count <= kCtxRegWindow - rel && !TakesExtentHint(rel);
}

// One native SET_CONTEXT_REG of context offset `rel` with `value`, emitted at the guest's position.
bool EmitCtxWrite(void* cb, u32 rel, u32 value) {
    return Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [rel, value] {
        WriteCtx(rel, &value, 1);
    });
}

} // namespace

} // namespace Native::Gnm

using Native::Gnm::Class;

// 0x869990: SET_CONTEXT_REG CB_TARGET_MASK (0x8e), one dword.
extern "C" __attribute__((sysv_abi)) void GnmW01_869990(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w01:869990");
    using Fn = Native::Gnm::GuestW01Cb;
    if (!Native::Gnm::Enabled(Class::Cb)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869990)(cb, value);
        return;
    }
    const u32 v = value;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kCbTargetMask, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869990)(cb, value);
}

// 0x869a50: SET_CONTEXT_REG CB_BLENDn_CONTROL (0x1e0 + n), value = arg3 & 0xffff1fff.
extern "C" __attribute__((sysv_abi)) void GnmW01_869a50(void* cb, int index, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w01:869a50");
    using Fn = Native::Gnm::GuestW01CbIdx;
    // Only the eight colour-buffer blend controls are handled natively; anything else keeps the
    // guest's exact register arithmetic.
    const bool in_range = index >= 0 && index < 8;
    if (!in_range || !Native::Gnm::Enabled(Class::Cb)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869a50)(cb, index, value);
        return;
    }
    const u32 reg = Native::Gnm::kCbBlend0Control + static_cast<u32>(index);
    const u32 v = value & 0xffff1fffu;
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [reg, v] {
            Native::Gnm::WriteCtx(reg, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869a50)(cb, index, value);
}

// 0x869ce0: SET_CONTEXT_REG CB_COLOR_CONTROL (0x202),
// value = ((arg2 & 7) << 4) | ((arg3 & 0xff) << 16).
extern "C" __attribute__((sysv_abi)) void GnmW01_869ce0(void* cb, unsigned int mode,
                                                         unsigned int rop) {
    static const u32 site = Native::Gnm::Site("w01:869ce0");
    using Fn = Native::Gnm::GuestW01CbPair;
    if (!Native::Gnm::Enabled(Class::Cb)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869ce0)(cb, mode, rop);
        return;
    }
    const u32 v = ((mode & 7u) << 4) | ((rop & 0xffu) << 16);
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kCbColorControl, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869ce0)(cb, mode, rop);
}

// 0x869ac0: SET_CONTEXT_REG CB_BLEND_RED (0x105), four payload dwords RED GREEN BLUE ALPHA.
// The four arguments arrive in xmm0..xmm3 (raw bytes: vmovss spills of xmm0..3 into the
// payload), so they are declared as float and copied bit for bit.
extern "C" __attribute__((sysv_abi)) void GnmW01_869ac0(void* cb, float red, float green,
                                                         float blue, float alpha) {
    static const u32 site = Native::Gnm::Site("w01:869ac0");
    using Fn = Native::Gnm::GuestW01CbFloat4;
    if (!Native::Gnm::Enabled(Class::Cb)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x869ac0)(cb, red, green, blue, alpha);
        return;
    }
    std::array<u32, 4> v{};
    std::memcpy(&v[0], &red, sizeof(u32));
    std::memcpy(&v[1], &green, sizeof(u32));
    std::memcpy(&v[2], &blue, sizeof(u32));
    std::memcpy(&v[3], &alpha, sizeof(u32));
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(Native::Gnm::kCbBlendRed, v.data(), 4);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869ac0)(cb, red, green, blue, alpha);
}

// 0x869370: generic SET_CONTEXT_REG. Raw bytes: rdi = cb, esi = absolute register (0xA000 + reg),
// edx = value, ecx = index. DW1 = (index << 28 when Neo mode) | (esi - 0xA000); DW2 = value.
// The native write uses the handler's decode of DW1 (bits 0..15), so index is not needed.
extern "C" __attribute__((sysv_abi)) void GnmW01_869370(void* cb, unsigned int reg_abs,
                                                         unsigned int value, unsigned int index) {
    static const u32 site = Native::Gnm::Site("w01:869370");
    using Fn = Native::Gnm::GuestW01Ctx3;
    const u32 rel = Native::Gnm::CtxRelOf(reg_abs);
    (void)index; // only the DW1 index bits, which the SetContextReg handler never reads
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::CtxWriteOk(rel, 1) &&
        Native::Gnm::EmitCtxWrite(cb, rel, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869370)(cb, reg_abs, value, index);
}

// 0x8694e0: SET_CONTEXT_REG range. Raw bytes: rdi = cb, esi = absolute register, rdx = values
// (copied by memcpy at call time), ecx = count. Header = 0xC0006900 | count << 16, DW1 = esi - 0xA000.
extern "C" __attribute__((sysv_abi)) void GnmW01_8694e0(void* cb, unsigned int reg_abs,
                                                         const unsigned int* values,
                                                         unsigned int count) {
    static const u32 site = Native::Gnm::Site("w01:8694e0");
    using Fn = Native::Gnm::GuestW01CtxRange;
    const u32 rel = Native::Gnm::CtxRelOf(reg_abs);
    if (!Native::Gnm::Enabled(Class::Cb) || !Native::Gnm::CtxWriteOk(rel, count)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x8694e0)(cb, reg_abs, values, count);
        return;
    }
    // The guest's buffer is only valid during the call: copy the payload now, as the packet does.
    std::vector<u32> data;
    if (count != 0) {
        data.assign(values, values + count);
    }
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [rel, data = std::move(data)] {
            if (!data.empty()) {
                Native::Gnm::WriteCtx(rel, data.data(), static_cast<u32>(data.size()));
            }
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x8694e0)(cb, reg_abs, values, count);
}

// 0x869890: SET_CONTEXT_REG 0x1ba (absolute 0xA1BA). Raw bytes: rdi = cb, esi = arg a, edx = arg b.
// Value = ((b & 0x1fff) << 12) | (a & 0xfff). Not an extent register.
extern "C" __attribute__((sysv_abi)) void GnmW01_869890(void* cb, unsigned int a, unsigned int b) {
    static const u32 site = Native::Gnm::Site("w01:869890");
    using Fn = Native::Gnm::GuestW01CbPair;
    const u32 v = ((b & 0x1fffu) << 12) | (a & 0xfffu);
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x1ba, v)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869890)(cb, a, b);
}

// 0x869c20: SET_CONTEXT_REG 0x2b0 (absolute 0xA2B0), value = arg2 (esi).
extern "C" __attribute__((sysv_abi)) void GnmW01_869c20(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w01:869c20");
    using Fn = Native::Gnm::GuestW01Cb;
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x2b0, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869c20)(cb, value);
}

// 0x869c80: SET_CONTEXT_REG 0x2b1 (absolute 0xA2B1), value = arg2 (esi).
extern "C" __attribute__((sysv_abi)) void GnmW01_869c80(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w01:869c80");
    using Fn = Native::Gnm::GuestW01Cb;
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x2b1, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x869c80)(cb, value);
}

// 0x86a120: SET_CONTEXT_REG 0x80 (PA_SC_WINDOW_SCISSOR_TL, absolute 0xA080). Raw bytes: rdi = cb,
// esi = arg a, edx = arg b. Value = (b << 16) | (a & 0xffff).
extern "C" __attribute__((sysv_abi)) void GnmW01_86a120(void* cb, unsigned int a, unsigned int b) {
    static const u32 site = Native::Gnm::Site("w01:86a120");
    using Fn = Native::Gnm::GuestW01CbPair;
    const u32 v = (b << 16) | (a & 0xffffu);
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x80, v)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86a120)(cb, a, b);
}

// 0x86a3d0: two SET_CONTEXT_REG packets, 0x30e (low dword) and 0x30f (high dword) of a 64-bit
// argument (PA_SC_AA_MASK). Raw bytes: rdi = cb, rsi = 64-bit value. Both registers are consecutive,
// so one two-dword write has the same effect as the two packets, in the same order.
extern "C" __attribute__((sysv_abi)) void GnmW01_86a3d0(void* cb, unsigned long long value) {
    static const u32 site = Native::Gnm::Site("w01:86a3d0");
    using Fn = Native::Gnm::GuestW01CbU64;
    const std::array<u32, 2> v{static_cast<u32>(value), static_cast<u32>(value >> 32)};
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::WriteCtx(0x30e, v.data(), 2);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86a3d0)(cb, value);
}

// 0x86a970: SET_CONTEXT_REG 0x102 (absolute 0xA102), value = arg2 (esi).
extern "C" __attribute__((sysv_abi)) void GnmW01_86a970(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w01:86a970");
    using Fn = Native::Gnm::GuestW01Cb;
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x102, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86a970)(cb, value);
}

// 0x86bb30: SET_CONTEXT_REG 0x2ad (absolute 0xA2AD), value = (arg2 & 0xff) ^ 1 (esi; movzx + xor 1).
extern "C" __attribute__((sysv_abi)) void GnmW01_86bb30(void* cb, unsigned int arg) {
    static const u32 site = Native::Gnm::Site("w01:86bb30");
    using Fn = Native::Gnm::GuestW01Cb;
    const u32 v = (arg & 0xffu) ^ 1u;
    if (Native::Gnm::Enabled(Class::Cb) && Native::Gnm::EmitCtxWrite(cb, 0x2ad, v)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86bb30)(cb, arg);
}

namespace Native::Gnm {

void RegisterW01(Table& t) {
    (void)t; // no sceGnm* wrapper in this area; the emitters above are boundary 1 only
}

} // namespace Native::Gnm
