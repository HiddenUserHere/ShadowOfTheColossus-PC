// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W08: Shader binds and user data.
// Class Shaders (SOTC_NATIVE_GNM=...). Owned files: this one, w08_shaders.h and
// gpu/sotc/hook_gnm_w08.inc. See gnm_common.h for the contract.
//
// Specification: the PM4 handlers of liverpool.cpp. SET_SH_REG (ProcessGraphics and ProcessCompute):
// an offset in [0x200, 0x200 + sizeof(ComputeProgram)/4] writes the compute state (NativeCsRegs),
// any other offset writes the SH image at ShRegWordOffset + offset and marks it. SET_CONTEXT_REG
// writes the context image at ContextRegWordOffset + offset and marks it (the CB/DB extent hints
// do not apply to any register written here). A NOP whose first body dword is not a PayloadType
// is ignored by the command processor (liverpool.cpp NOP handler, default case), so the
// sceGnmUpdate* context writes that are wrapped in such NOPs have no effect in this port; the
// native ops reproduce that and do not write those registers.
//
// Packet sizes (dwords): SET_SH_REG / SET_CONTEXT_REG = 2 + values; trailing NOP<N> = N + 1. Each
// native op fills exactly the slot the gnmdriver function (or the eboot emitter) fills, so the
// stream after it is unchanged. Slot totals are checked against the gnmdriver guard values.
//
// Eboot user-data table (stage -> absolute SH offset of USER_DATA_<stage>_0), read as u16 per stage
// from the .text table at VA 0x1290410 of out/re/eboot_pe_fde.exe (0x862490 reads it with
// `movzx r8d, word [rax+0x10]` plus DAT_01290410 + stage*2 in its callee 0x86d5e0):
//   0 CS 0x2e40, 1 PS 0x2c0c, 2 VS 0x2c4c, 3 ES 0x2c8c, 4 GS 0x2ccc, 5 HS 0x2d0c, 6 LS 0x2d4c.
// Stage 0 is compute: 0x86d5e0 sets the NOP header bit 1 (ShaderCompute) for stage 0 only.
//
// Coverage table (site | PM4 contract | native op | evidence):
//   sceGnmSetVsShader | SH 0x48 {vs0,0}; SH 0x4a {var,vs3}; CTX 0x207 {vs6}, 0x1b1 {vs4}, 0x1c3 {vs5}; 29 dw | batch via facade | gnmdriver.cpp:1965-2000; liverpool.cpp:1485-1491, 1559-1576 | raw bytes: n (host source only) | REA: n (no app open)
//   sceGnmSetEsShader | SH 0xc8 {es0,0}; SH 0xca {var,es3}; 20 dw | batch | gnmdriver.cpp:1679-1712 | REA n
//   sceGnmSetLsShader | SH 0x148 {ls0,0}; SH 0x14b {ls3}; SH 0x14a {var,ls3}; 23 dw | batch | gnmdriver.cpp:1785-1820 | REA n
//   sceGnmSetHsShader | SH 0x108 {hs0,0}; SH 0x10a {hs2,hs3}; CTX 0x286 {hs5,hs6}, 0x2db {hs4}, 0x2d6 {param4}; 30 dw | batch | gnmdriver.cpp:1751-1783 | REA n
//   sceGnmSetGsShader | SH 0x88 {gs0,0}; SH 0x8a {gs2,gs3}; CTX 0x2e5 {gs4}, 0x29b {gs5}, 0x2e4 {gs6}; 29 dw | batch | gnmdriver.cpp:1719-1749 | REA n
//   sceGnmSetPsShader | SH 0x08 {ps0,0}; SH 0x0a {ps2,ps3}; CTX 0x1c4 {ps4,ps5}, 0x1b3 {ps6,ps7}, 0x1b6 {ps8}, 0x1b8 {ps9}, 0x203 {ps10}, 0x8f {ps11}; 40 dw; null ps: SH 0x08 {0,0}, CTX 0x203 {0} | batch | gnmdriver.cpp:1822-1861 | REA n
//   sceGnmSetPsShader350 | as SetPsShader, null ps: SH 0x08 {0,0}, CTX 0x203 {0}, CTX 0x8f {0xf}; 40 dw | batch | gnmdriver.cpp:1863-1903 | REA n
//   sceGnmSetCsShader | SH 0x20c {cs0,0}; SH 0x212 {cs2,cs3}; SH 0x207 {cs4,cs5,cs6} (compute state); 25 dw | batch | gnmdriver.cpp:1482-1511 | REA n
//   sceGnmSetCsShaderWithModifier | as SetCsShader, rsrc1 = modifier-merged cs2; 25 dw | batch | gnmdriver.cpp:1513-1548 | REA n
//   sceGnmUpdateVsShader | SH 0x48, SH 0x4a as SetVsShader; CTX 0x207/0x1b1/0x1c3 are NOP-wrapped (dropped by the port); 29 dw | batch (SH only) | gnmdriver.cpp:2726-2760; liverpool.cpp:1464 | REA n
//   sceGnmUpdateGsShader | SH 0x88, SH 0x8a; 0x2e5/0x29b/0x2e4 NOP-wrapped (dropped); 29 dw | batch (SH only) | gnmdriver.cpp:2558-2591 | REA n
//   sceGnmUpdateHsShader | SH 0x108, SH 0x10a; 0x286/0x2db/0x2d6 NOP-wrapped (dropped); 30 dw | batch (SH only) | gnmdriver.cpp:2593-2628 | REA n
//   sceGnmUpdatePsShader | null: SH 0x08 {0,0}; non-null: SH 0x08, SH 0x0a; 0x1c4/0x1b3/0x1b6/0x1b8/0x203/0x8f NOP-wrapped (dropped); 40 dw | batch (SH only) | gnmdriver.cpp:2630-2675 | REA n
//   sceGnmUpdatePsShader350 | null: SH 0x08 {0,0}, CTX 0x8f {0xf} (real context write), 0x203 NOP-wrapped; non-null as UpdatePsShader; 40 dw | batch | gnmdriver.cpp:2677-2724 | REA n
//   sceGnmSetEmbeddedVsShader | SH/CTX as SetVsShader with the shader address of gnmdriver statics | NOT replaced (returns false, PM4 runs): the register values are host addresses of arrays inside gnmdriver.cpp, which this worker cannot reference | gnmdriver.cpp:1623-1676 | n/a
//   sceGnmSetEmbeddedPsShader | as above | NOT replaced (same reason) | gnmdriver.cpp:1550-1620 | n/a
//   0x869630 (eboot SET_SH x7) | SH 0x07 {s0}, 0x46 {s1}, 0x87 {s2}, 0xc7 {s3}, 0x107 {s4}, 0x147 {s5}, 0x47 {s6}; 21 dw, stack argument struct | EmitCp batch | out/re/eboot_pe_fde.exe disassembly 0x869630-0x869704 (checked: raw bytes y, REA n: no app open, flat decompile out/re/decomp/0086/00869630.c used only for the signature) | raw bytes: y
//   0x862490 (user-data writer) | per stage s: NOP, then SH (stage table) with cnt = byte 1 of the stage struct word, lo = byte 0; if s == 2 and cb+0xc613 set: compute SH lo2 + 0x240 with cnt2 values (callee 0x884240); values copied at entry, cnt <= 16 | EmitCp on cb1 (+0xc090) and cb2 (+0xc098) | out/re/eboot_pe_fde.exe disassembly 0x862490-0x86250d, 0x86d5e0 and 0x884240 (flat decompile out/re/decomp/0086/0086d5e0.c and 0088/00884240.c) | raw bytes: y | REA n (no app open)
//   0x86d4c0 (SET_SH x2, 64-bit value; stage 0 header 0xc0027602) | callers 0x85f220, 0x85fc10, 0x85f410 | header 0xc0027600, offset (p3 + table[s] - 0x2c00) & 0xffff, values lo/hi; EmitCp, RunGfxShWrite (NativeGfxCsRegs for the compute range, NativeSetRegs otherwise) | raw bytes y (86d4c0-86d547, lea at 86d528, movzx 86d511); decomp out/re/decomp/0086/0086d4c0.c; REA n (no app open)
//   0x86d550 (SET_SH x1) | callers 0x862c50, 0x862510, 0x85f410 | header 0xc0017600, one value (ecx); EmitCp, RunGfxShWrite | raw bytes y (86d550-86d5d0, lea 86d5b1, mov [rax+8], r14d 86d5bc); decomp 0086/0086d550.c; REA n
//   0x86d5e0 (SET_SH xn, n <= 16; NOP 0xc0001000 + payload 0x6875000d before it) | callers 0x8619a0, 0x861b40, 0x85f220, 0x861e60, 0x861fe0, 0x862160, 0x8611a0, 0x85fc10, 0x8622e0, 0x862490 (fallback), 0x861ce0, 0x85f410 | header 0xc0007600, count n, offset as above; the NOP pair is ignored by the CP (payload 0x6875000d is no PayloadType, liverpool.cpp NOP default) and is not reproduced; n == 0 emits nothing; EmitCp, RunGfxShWrite | raw bytes y (86d5e1-86d6f5, jump table on n at 86d68d); decomp 0086/0086d5e0.c; REA n. No double emit: the round-1 0x862490 native path never calls 0x86d5e0.
//   0x86b810 (opcode 0x8d packet 0xc0028d00 | pred + NOP 0xc0001000) | caller 0x862c50 (one call per loop iteration and the tail) | no CP handler for opcode 0x8d (ProcessGraphics default, liverpool.cpp ~2192): no state effect, so nothing is emitted; guest predicate 0x882dc0 (sceGnmIsUserPaEnabled = false, gnmdriver.cpp:1283) gates the thread-trace marker branch, which falls back when set; the guest refill (6 dwords) is not reproduced | raw bytes y (header or at 86b897, NOP at 86b8fc, predicate call 86b912, marker call 86b91b); decomp 0086/0086b810.c; REA n. W11 lists 0x86b810 as kept on PM4 in its comment (w11_ce_misc.cpp:51), it has no row, so no double install.
//   0x8695d0 (SET_SH xn, count in r8d, flag in esi; no static callers) | header 0xc0007600 | count << 16 | (esi & 1) << 1, body p3 - 0x2c00 (16 bits), count values from rcx; bit 1 set = compute ring (NativeCsRegs), clear = graphics (NativeGfxCsRegs for 0x200+); count 0 emits nothing, count > 16 falls back | EmitCp | raw bytes y (file offset VA+0x4000, push at 86d5d0, memcpy count at 86d6xx); decomp 0086/008695d0.c; REA n
//   0x869710 (SET_SH compute offset 0x215, one value; no static callers) | header 0xc0017602, value (p4 & 0x3f) << 16 | (p3 & 0xf) << 12 | (p2 & 0x3ff) | EmitCp, compute bit set | raw bytes y (mc of the slice: movl $0xc0017602 at +0x0a, value bits at 869710+0x52); decomp 0086/00869710.c; REA n. The guest space callback (cb+0x18) is not reproduced: EmitCp owns the space.
//   0x869790 / 0x869810 (SET_SH compute offset DAT_01290070[stage] - 0x2c00, stages 0..3 = 0x216..0x21a; no static callers) | header 0xc0017602, value p3 & 0xffff | EmitCp, compute bit set; the two calls of 0x882ec0 (cached sceKernelIsNeoMode, no effect) are not reproduced; stage >= 4 falls back | raw bytes y (table u32 at file offset 0x1294070: 0x2e16 0x2e17 0x2e19 0x2e1a); decomp 0086/00869790.c, 00869810.c; REA n
//   0x869910 (SET_SH compute offset 0x218, one value; no static callers) | header 0xc0017602, value (p3 & 0x1fff) << 12 | (p2 & 0xfff) | EmitCp, compute bit set | raw bytes y; decomp 0086/00869910.c; REA n
//   0x86d2e0 / 0x86d420 (SET_SH x4 of a stage, NOP 0xc0001000 + payload 0x68750004 / 0x68750006 before it, ignored by the CP) | header 0xc0047600 (+2 for stage 0), offset (p3 + USER_DATA[stage] - 0x2c00) & 0xffff, four values from rcx | EmitCp, compute bit = stage 0 | raw bytes y (u16 table at file offset 0x1294410, same as kUserDataBase); decomp 0086/0086d2e0.c, 0086d420.c; REA n
//   0x86d380 (SET_SH x8 of a stage, NOP 0xc0001000 + payload 0x68750005 before it, ignored) | header 0xc0087600 (+2 for stage 0), offset as above, eight values from rcx | EmitCp, compute bit = stage 0 | raw bytes y; decomp 0086/0086d380.c; REA n
//   0x884240 (other caller of the compute write) | reached from 0x862490 (stage 2 second write) | NOT replaced here; the PM4 path stays | n/a | n/a

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "native/gnm/gnm_common.h"
#include "native/gnm/w08_shaders.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/regs_shader.h"

namespace Native::Gnm {
namespace {

constexpr u32 kMaxPkts = 8;

// One SET_SH_REG or SET_CONTEXT_REG packet of a batch: `off` is relative to its register base.
struct Pkt {
    bool ctx;
    u32 off;
    u32 n;
    u32 v[3];
};

// The register writes of one gnmdriver function, captured by value at call time.
struct Batch {
    u32 count = 0;
    std::array<Pkt, kMaxPkts> pkts{};

    Batch& Sh(u32 off, std::initializer_list<u32> vals) {
        return Add(false, off, vals);
    }
    Batch& Ctx(u32 off, std::initializer_list<u32> vals) {
        return Add(true, off, vals);
    }

private:
    Batch& Add(bool ctx, u32 off, std::initializer_list<u32> vals) {
        Pkt& p = pkts[count++];
        p.ctx = ctx;
        p.off = off;
        p.n = static_cast<u32>(vals.size());
        u32 i = 0;
        for (const u32 v : vals) {
            p.v[i++] = v;
        }
        return *this;
    }
};

// SET_SH_REG as the handler does it (liverpool.cpp, both ProcessGraphics and ProcessCompute).
void ApplyShReg(AmdGpu::Liverpool& lv, u32 off, const u32* vals, u32 n) {
    constexpr u32 kCsWords = sizeof(AmdGpu::ComputeProgram) / 4;
    if (off >= 0x200 && off <= 0x200 + kCsWords) {
        // The handler ASSERTs on the size; a write that would leave the struct is skipped here.
        if (off - 0x200 + n > kCsWords) {
            return;
        }
        u32* dst = reinterpret_cast<u32*>(&lv.NativeCsRegs()) + (off - 0x200);
        std::memcpy(dst, vals, n * sizeof(u32));
        return;
    }
    lv.NativeSetRegs(AmdGpu::Regs::ShRegWordOffset + off, vals, n);
}

// Runs inside the command processor (Where::CommandProcessor).
void RunBatch(const Batch& b) {
    AmdGpu::Liverpool& lv = Lv();
    for (u32 i = 0; i < b.count; ++i) {
        const Pkt& p = b.pkts[i];
        if (p.ctx) {
            lv.NativeSetRegs(AmdGpu::Regs::ContextRegWordOffset + p.off, p.v, p.n);
        } else {
            ApplyShReg(lv, p.off, p.v, p.n);
        }
    }
}

// Emits the batch into the fixed slot of a gnmdriver function. `dwords` is the size the PM4 path
// writes; a smaller caller buffer keeps the PM4 path (which then behaves as before).
Shadow ShadowOf(const Batch& b); // below (prefetch shadow, gap N2-88)

bool EmitBatch(u32* cmdbuf, u32 size, u32 dwords, const Batch& b) {
    if (!cmdbuf || size < dwords) {
        return false;
    }
    return EmitCpInSlotShadow(cmdbuf, dwords, [b] { RunBatch(b); }, ShadowOf(b));
}

// The user-data writer of 0x862490: one SET_SH_REG of up to 16 values, at a fixed offset.
struct UserDataWrite {
    u32 off;
    u32 count;
    std::array<u32, 16> v;
};

void RunUserDataWrite(const UserDataWrite& w) {
    ApplyShReg(Lv(), w.off, w.v.data(), w.count);
}

// Prefetch shadows (gap N2-88): the compute-range SET_SH_REG packets of a batch / a write.
Shadow ShadowOf(const Batch& b) {
    Shadow sh;
    for (u32 i = 0; i < b.count; ++i) {
        if (!b.pkts[i].ctx) {
            sh.Sh(b.pkts[i].off, b.pkts[i].v, b.pkts[i].n);
        }
    }
    return sh;
}
Shadow ShadowOf(const UserDataWrite& w) {
    Shadow sh;
    sh.Sh(w.off, w.v.data(), w.count);
    return sh;
}

// Absolute SH offset of USER_DATA_<stage>_0 for the eboot stages 0..6 (see the header comment).
constexpr std::array<u32, 7> kUserDataBase = {0x2e40, 0x2c0c, 0x2c4c, 0x2c8c,
                                              0x2ccc, 0x2d0c, 0x2d4c};

// Guest struct accessors of 0x862490 (offsets from the disassembly; the struct is the r15 argument).
constexpr u32 kStageWordPtrs = 0xc180; // u8* [8]: per stage, a struct whose +0x10 is u16 (lo, count)
constexpr u32 kCbMain = 0xc090;        // GNM command buffer for the stage writes
constexpr u32 kCbCompute = 0xc098;     // command buffer of the second (compute) write
constexpr u32 kSecondFlag = 0xc613;    // byte: second write enabled (stage 2 only)
constexpr u32 kSecondWordPtr = 0xc1b8; // u8*: the struct of the second write

u16 ReadWordAt10(const u8* st) {
    u16 w;
    std::memcpy(&w, st + 0x10, sizeof(w));
    return w;
}

u8* LoadPtr(const u8* base, u32 off) {
    u8* p;
    std::memcpy(&p, base + off, sizeof(p));
    return p;
}

// Guest body of the 0x862490 writer (fallback). 0x862490 takes its arguments in registers.
using UserDataFn = void(__attribute__((sysv_abi)) *)(void*, int, const u32*);
// Guest body of the 0x869630 emitter: cb in rdi, the struct on the stack.
using Emit7Fn = void(__attribute__((sysv_abi)) *)(void*, SotcW08Stack7);

// ---- sceGnm* table entries (boundary 2) ----

bool NativeSetVsShader(u32* cmdbuf, u32 size, const u32* vs, u32 mod) {
    static const u32 site = Site("w08:sceGnmSetVsShader");
    if (!Enabled(Class::Shaders) || !vs || vs[1] != 0 || (mod & 0xfcfffc3fu)) {
        return false;
    }
    const u32 var = mod == 0 ? vs[2] : ((vs[2] & 0xfcfffc3fu) | mod);
    Batch b;
    b.Sh(0x48, {vs[0], 0u}).Sh(0x4a, {var, vs[3]}).Ctx(0x207, {vs[6]}).Ctx(0x1b1, {vs[4]})
        .Ctx(0x1c3, {vs[5]});
    if (!EmitBatch(cmdbuf, size, 29, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetEsShader(u32* cmdbuf, u32 size, const u32* es, u32 mod) {
    static const u32 site = Site("w08:sceGnmSetEsShader");
    if (!Enabled(Class::Shaders) || !es || es[1] != 0 || (mod & 0xfcfffc3fu)) {
        return false;
    }
    const u32 var = mod == 0 ? es[2] : ((es[2] & 0xfcfffc3fu) | mod);
    Batch b;
    b.Sh(0xc8, {es[0], 0u}).Sh(0xca, {var, es[3]});
    if (!EmitBatch(cmdbuf, size, 20, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetLsShader(u32* cmdbuf, u32 size, const u32* ls, u32 mod) {
    static const u32 site = Site("w08:sceGnmSetLsShader");
    if (!Enabled(Class::Shaders) || !ls) {
        return false;
    }
    const u32 mask = ((mod & 0xfffffc3fu) == 0) ? 0xfffffc3fu : 0xfcfffc3fu;
    if (mod & mask) {
        return false;
    }
    if (ls[1] != 0) {
        return false;
    }
    const u32 var = mod == 0 ? ls[2] : ((ls[2] & mask) | mod);
    Batch b;
    b.Sh(0x148, {ls[0], 0u}).Sh(0x14b, {ls[3]}).Sh(0x14a, {var, ls[3]});
    if (!EmitBatch(cmdbuf, size, 23, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetHsShader(u32* cmdbuf, u32 size, const u32* hs, u32 param4) {
    static const u32 site = Site("w08:sceGnmSetHsShader");
    if (!Enabled(Class::Shaders) || !hs || hs[1] != 0) {
        return false;
    }
    Batch b;
    b.Sh(0x108, {hs[0], 0u}).Sh(0x10a, {hs[2], hs[3]}).Ctx(0x286, {hs[5], hs[6]})
        .Ctx(0x2db, {hs[4]}).Ctx(0x2d6, {param4});
    if (!EmitBatch(cmdbuf, size, 30, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetGsShader(u32* cmdbuf, u32 size, const u32* gs) {
    static const u32 site = Site("w08:sceGnmSetGsShader");
    if (!Enabled(Class::Shaders) || !gs || gs[1] != 0) {
        return false;
    }
    Batch b;
    b.Sh(0x88, {gs[0], 0u}).Sh(0x8a, {gs[2], gs[3]}).Ctx(0x2e5, {gs[4]}).Ctx(0x29b, {gs[5]})
        .Ctx(0x2e4, {gs[6]});
    if (!EmitBatch(cmdbuf, size, 29, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

// Pixel shader registers shared by SetPsShader, SetPsShader350 and their non-null branch.
Batch PsBatch(const u32* ps) {
    Batch b;
    b.Sh(0x08, {ps[0], 0u}).Sh(0x0a, {ps[2], ps[3]}).Ctx(0x1c4, {ps[4], ps[5]})
        .Ctx(0x1b3, {ps[6], ps[7]}).Ctx(0x1b6, {ps[8]}).Ctx(0x1b8, {ps[9]})
        .Ctx(0x203, {ps[10]}).Ctx(0x8f, {ps[11]});
    return b;
}

bool NativeSetPsShader(u32* cmdbuf, u32 size, const u32* ps) {
    static const u32 site = Site("w08:sceGnmSetPsShader");
    if (!Enabled(Class::Shaders)) {
        return false;
    }
    Batch b;
    if (!ps) {
        b.Sh(0x08, {0u, 0u}).Ctx(0x203, {0u});
    } else {
        if (ps[1] != 0) {
            return false;
        }
        b = PsBatch(ps);
    }
    if (!EmitBatch(cmdbuf, size, 40, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetPsShader350(u32* cmdbuf, u32 size, const u32* ps) {
    static const u32 site = Site("w08:sceGnmSetPsShader350");
    if (!Enabled(Class::Shaders)) {
        return false;
    }
    Batch b;
    if (!ps) {
        b.Sh(0x08, {0u, 0u}).Ctx(0x203, {0u}).Ctx(0x8f, {0xfu});
    } else {
        if (ps[1] != 0) {
            return false;
        }
        b = PsBatch(ps);
    }
    if (!EmitBatch(cmdbuf, size, 40, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetCsShader(u32* cmdbuf, u32 size, const u32* cs) {
    static const u32 site = Site("w08:sceGnmSetCsShader");
    if (!Enabled(Class::Shaders) || !cs || cs[1] != 0) {
        return false;
    }
    Batch b;
    b.Sh(0x20c, {cs[0], 0u}).Sh(0x212, {cs[2], cs[3]}).Sh(0x207, {cs[4], cs[5], cs[6]});
    if (!EmitBatch(cmdbuf, size, 25, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeSetCsShaderWithModifier(u32* cmdbuf, u32 size, const u32* cs, u32 mod) {
    static const u32 site = Site("w08:sceGnmSetCsShaderWithModifier");
    if (!Enabled(Class::Shaders) || !cs || (mod & 0xfffffc3fu) || cs[1] != 0) {
        return false;
    }
    const u32 rsrc1 = mod == 0 ? cs[2] : ((cs[2] & 0xfffffc3fu) | mod);
    Batch b;
    b.Sh(0x20c, {cs[0], 0u}).Sh(0x212, {rsrc1, cs[3]}).Sh(0x207, {cs[4], cs[5], cs[6]});
    if (!EmitBatch(cmdbuf, size, 25, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

// The Update* functions: their context writes are NOP-wrapped (see the header comment), so only
// the SET_SH_REG packets and the real SET_CONTEXT_REG packets of each function are applied.
bool NativeUpdateVsShader(u32* cmdbuf, u32 size, const u32* vs, u32 mod) {
    static const u32 site = Site("w08:sceGnmUpdateVsShader");
    if (!Enabled(Class::Shaders) || !vs || vs[1] != 0 || (mod & 0xfcfffc3fu)) {
        return false;
    }
    const u32 var = mod == 0 ? vs[2] : ((vs[2] & 0xfcfffc3fu) | mod);
    Batch b;
    b.Sh(0x48, {vs[0], 0u}).Sh(0x4a, {var, vs[3]});
    if (!EmitBatch(cmdbuf, size, 29, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeUpdateGsShader(u32* cmdbuf, u32 size, const u32* gs) {
    static const u32 site = Site("w08:sceGnmUpdateGsShader");
    if (!Enabled(Class::Shaders) || !gs || gs[1] != 0) {
        return false;
    }
    Batch b;
    b.Sh(0x88, {gs[0], 0u}).Sh(0x8a, {gs[2], gs[3]});
    if (!EmitBatch(cmdbuf, size, 29, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeUpdateHsShader(u32* cmdbuf, u32 size, const u32* hs, u32 ls_hs_config) {
    static const u32 site = Site("w08:sceGnmUpdateHsShader");
    (void)ls_hs_config; // only written inside a NOP-wrapped packet, which the port ignores
    if (!Enabled(Class::Shaders) || !hs || hs[1] != 0) {
        return false;
    }
    Batch b;
    b.Sh(0x108, {hs[0], 0u}).Sh(0x10a, {hs[2], hs[3]});
    if (!EmitBatch(cmdbuf, size, 30, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeUpdatePsShader(u32* cmdbuf, u32 size, const u32* ps) {
    static const u32 site = Site("w08:sceGnmUpdatePsShader");
    if (!Enabled(Class::Shaders)) {
        return false;
    }
    Batch b;
    if (!ps) {
        b.Sh(0x08, {0u, 0u});
    } else {
        if (ps[1] != 0) {
            return false;
        }
        b.Sh(0x08, {ps[0], 0u}).Sh(0x0a, {ps[2], ps[3]});
    }
    if (!EmitBatch(cmdbuf, size, 40, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool NativeUpdatePsShader350(u32* cmdbuf, u32 size, const u32* ps) {
    static const u32 site = Site("w08:sceGnmUpdatePsShader350");
    if (!Enabled(Class::Shaders)) {
        return false;
    }
    Batch b;
    if (!ps) {
        b.Sh(0x08, {0u, 0u}).Ctx(0x8f, {0xfu});
    } else {
        if (ps[1] != 0) {
            return false;
        }
        b.Sh(0x08, {ps[0], 0u}).Sh(0x0a, {ps[2], ps[3]});
    }
    if (!EmitBatch(cmdbuf, size, 40, b)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

} // namespace

void RegisterW08(Table& t) {
    t.SetVsShader = &NativeSetVsShader;
    t.SetEsShader = &NativeSetEsShader;
    t.SetLsShader = &NativeSetLsShader;
    t.SetHsShader = &NativeSetHsShader;
    t.SetGsShader = &NativeSetGsShader;
    t.SetPsShader = &NativeSetPsShader;
    t.SetPsShader350 = &NativeSetPsShader350;
    t.SetCsShader = &NativeSetCsShader;
    t.SetCsShaderWithModifier = &NativeSetCsShaderWithModifier;
    t.UpdateVsShader = &NativeUpdateVsShader;
    t.UpdateGsShader = &NativeUpdateGsShader;
    t.UpdateHsShader = &NativeUpdateHsShader;
    t.UpdatePsShader = &NativeUpdatePsShader;
    t.UpdatePsShader350 = &NativeUpdatePsShader350;
    // SetEmbeddedVsShader / SetEmbeddedPsShader: not replaced (their register values are addresses
    // of arrays inside gnmdriver.cpp); the PM4 path stays.
}

} // namespace Native::Gnm

// ---- eboot detours (boundary 1); declared in gpu/sotc/hook_gnm_w08.inc ----

// 0x869630: SET_SH_REG x7 (stage user-data and shader-register tail), one struct argument.
extern "C" __attribute__((sysv_abi)) void GnmW08_869630(void* cb, SotcW08Stack7 s) {
    static const u32 site = ::Native::Gnm::Site("w08:0x869630");
    const auto guest = ::Native::Gnm::Guest<::Native::Gnm::Emit7Fn>(0x869630);
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders)) {
        ::Native::Stats::SiteFallback(site);
        guest(cb, s);
        return;
    }
    ::Native::Gnm::Batch b;
    b.Sh(0x07, {s.v[0]}).Sh(0x46, {s.v[1]}).Sh(0x87, {s.v[2]}).Sh(0xc7, {s.v[3]})
        .Sh(0x107, {s.v[4]}).Sh(0x147, {s.v[5]}).Sh(0x47, {s.v[6]});
    const u64 cb_addr = static_cast<u64>(reinterpret_cast<uintptr_t>(cb));
    if (!::Native::Gnm::EmitCpShadow(cb_addr, [b] { ::Native::Gnm::RunBatch(b); }, ::Native::Gnm::ShadowOf(b))) {
        ::Native::Stats::SiteFallback(site);
        guest(cb, s);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x862490: user-data writer of one stage (stage 0 = compute). Its guest callees are 0x86d5e0
// (the stage write) and, for stage 2 with the flag set, 0x884240 (a compute user-data write).
extern "C" __attribute__((sysv_abi)) void GnmW08_862490(void* state, int stage, const u32* values) {
    static const u32 site = ::Native::Gnm::Site("w08:0x862490");
    const auto guest = ::Native::Gnm::Guest<::Native::Gnm::UserDataFn>(0x862490);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(state, stage, values);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders)) {
        fallback();
        return;
    }
    if (!state || stage < 0 || stage >= static_cast<int>(::Native::Gnm::kUserDataBase.size())) {
        fallback();
        return;
    }
    using namespace ::Native::Gnm;
    u8* const s = static_cast<u8*>(state);

    // Stage write: the per-stage struct word (lo = start index, count = value count).
    const u8* const st = LoadPtr(s, kStageWordPtrs + static_cast<u32>(stage) * 8u);
    if (!st) {
        fallback();
        return;
    }
    const u16 w1 = ReadWordAt10(st);
    const u32 lo1 = w1 & 0xffu;
    const u32 cnt1 = w1 >> 8;

    // Second write (stage 2 only, enabled by a flag byte of the state).
    const bool second = stage == 2 && s[kSecondFlag] != 0;
    u32 lo2 = 0;
    u32 cnt2 = 0;
    if (second) {
        const u8* const st2 = LoadPtr(s, kSecondWordPtr);
        if (!st2) {
            fallback();
            return;
        }
        const u16 w2 = ReadWordAt10(st2);
        lo2 = w2 & 0xffu;
        cnt2 = w2 >> 8;
    }
    if (cnt1 > 16 || cnt2 > 16) {
        fallback();
        return;
    }
    const u32 max_cnt = cnt1 > cnt2 ? cnt1 : cnt2;
    if (max_cnt != 0 && !values) {
        fallback();
        return;
    }

    if (cnt1 == 0 && cnt2 == 0) {
        // The guest writes only NOP headers here, which the command processor ignores.
        ::Native::Stats::SiteNative(site);
        return;
    }

    // Offsets as the guest computes them: lo + table[stage] - 0x2c00 (the header carries the low
    // 16 bits of this value in its offset dword), and lo2 + 0x240 for the compute write.
    const u32 off1 = (lo1 + kUserDataBase[static_cast<u32>(stage)] - 0x2c00u) & 0xffffu;
    const u32 off2 = (lo2 + 0x240u) & 0xffffu;

    UserDataWrite w1op{off1, cnt1, {}};
    std::memcpy(w1op.v.data(), values, cnt1 * sizeof(u32));
    UserDataWrite w2op{off2, cnt2, {}};
    std::memcpy(w2op.v.data(), values, cnt2 * sizeof(u32));

    // The command buffers: +0xc090 for the stage write, +0xc098 for the second write.
    u8* const cb1 = LoadPtr(s, kCbMain);
    u8* const cb2 = LoadPtr(s, kCbCompute);
    if ((cnt1 != 0 && !cb1) || (second && cnt2 != 0 && !cb2)) {
        fallback();
        return;
    }

    if (cnt1 != 0) {
        // Nothing is emitted yet, so a refused first emission can fall back to the whole guest call.
        if (!EmitCpShadow(static_cast<u64>(reinterpret_cast<uintptr_t>(cb1)),
                    [w1op] { RunUserDataWrite(w1op); }, ShadowOf(w1op))) {
            fallback();
            return;
        }
    }
    if (second && cnt2 != 0) {
        // A refused second emission is the same as the guest callee returning early on refill.
        EmitCpShadow(static_cast<u64>(reinterpret_cast<uintptr_t>(cb2)),
                    [w2op] { RunUserDataWrite(w2op); }, ShadowOf(w2op));
    }
    ::Native::Stats::SiteNative(site);
}

// ---- graphics-ring SET_SH_REG user-data writers (boundary 1, round 2) ----
// 0x86d4c0, 0x86d550, 0x86d5e0 write SET_SH_REG packets into the graphics command buffer. The
// command processor handler is ProcessGraphics' SetShReg (liverpool.cpp, the case at ~1570): the
// shader-type bit of the header is not looked at, so stage 0 (compute) is the same write as the
// others. Offsets in the compute range go to the graphics queue's compute state (NativeGfxCsRegs).

namespace Native::Gnm {
namespace {

// ProcessGraphics SetShReg, applied on the graphics ring.
void ApplyShRegGfx(AmdGpu::Liverpool& lv, u32 off, const u32* vals, u32 n) {
    constexpr u32 kCsWords = sizeof(AmdGpu::ComputeProgram) / 4;
    if (off >= 0x200 && off <= 0x200 + kCsWords) {
        // The handler ASSERTs on the size; a write that would leave the struct is skipped here.
        if (off - 0x200 + n > kCsWords) {
            return;
        }
        u32* dst = reinterpret_cast<u32*>(&lv.NativeGfxCsRegs()) + (off - 0x200);
        std::memcpy(dst, vals, n * sizeof(u32));
        return;
    }
    lv.NativeSetRegs(AmdGpu::Regs::ShRegWordOffset + off, vals, n);
}

// Runs inside the command processor (Where::CommandProcessor).
void RunGfxShWrite(const UserDataWrite& w) {
    ApplyShRegGfx(Lv(), w.off, w.v.data(), w.count);
}

// The register offset the eboot writers put in the header: low 16 bits of
// p3 + USER_DATA table[stage] - 0x2c00 (movzx from DAT_01290410 + stage*2, then lea; raw bytes of
// 0x86d4c0 at 86d511/86d528, 0x86d550 at 86d5a1/86d5b1, 0x86d5e0 at 86d648/86d66c). False for a stage
// outside the table, which the guest would read past.
bool StageOffset(u32 stage, u32 p3, u32& off) {
    if (stage >= kUserDataBase.size()) {
        return false;
    }
    off = (p3 + kUserDataBase[stage] - 0x2c00u) & 0xffffu;
    return true;
}

} // namespace
} // namespace Native::Gnm

// 0x86d4c0: SET_SH_REG of two dwords (a 64-bit value in rcx), one stage. Header 0xc0027600 (plus 2
// for stage 0), count 2. Callers 0x85f220, 0x85fc10, 0x85f410.
extern "C" __attribute__((sysv_abi)) void GnmW08_86d4c0(void* cb, unsigned int stage, int p3,
                                                        unsigned long long value) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d4c0");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, unsigned long long);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d4c0);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, value);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb) {
        fallback();
        return;
    }
    u32 off = 0;
    if (!::Native::Gnm::StageOffset(stage, static_cast<u32>(p3), off)) {
        fallback();
        return;
    }
    ::Native::Gnm::UserDataWrite w{off, 2, {}};
    w.v[0] = static_cast<u32>(value);
    w.v[1] = static_cast<u32>(value >> 32);
    const u64 cb_addr = static_cast<u64>(reinterpret_cast<uintptr_t>(cb));
    if (!::Native::Gnm::EmitCpShadow(cb_addr, [w] { ::Native::Gnm::RunGfxShWrite(w); }, ::Native::Gnm::ShadowOf(w))) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86d550: SET_SH_REG of one dword (ecx). Header 0xc0017600 (plus 2 for stage 0), count 1.
// Callers 0x862c50, 0x862510, 0x85f410.
extern "C" __attribute__((sysv_abi)) void GnmW08_86d550(void* cb, unsigned int stage, int p3,
                                                        int p4) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d550");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d550);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, p4);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb) {
        fallback();
        return;
    }
    u32 off = 0;
    if (!::Native::Gnm::StageOffset(stage, static_cast<u32>(p3), off)) {
        fallback();
        return;
    }
    ::Native::Gnm::UserDataWrite w{off, 1, {}};
    w.v[0] = static_cast<u32>(p4);
    const u64 cb_addr = static_cast<u64>(reinterpret_cast<uintptr_t>(cb));
    if (!::Native::Gnm::EmitCpShadow(cb_addr, [w] { ::Native::Gnm::RunGfxShWrite(w); }, ::Native::Gnm::ShadowOf(w))) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86d5e0: the stage user-data writer (callee of 0x862490). Header 0xc0007600 (plus 2 for stage 0),
// count = n (0..16), values read from `values`. For n == 0 the guest writes only two NOPs, which the
// command processor ignores, so nothing is emitted. The round-1 0x862490 native path never calls
// this function, so a call from 0x862490 reaches exactly one of the two paths.
extern "C" __attribute__((sysv_abi)) void GnmW08_86d5e0(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values, int count) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d5e0");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, const unsigned int*, int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d5e0);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, values, count);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb || count < 0 ||
        count > 16 || (count > 0 && !values)) {
        fallback();
        return;
    }
    u32 off = 0;
    if (!::Native::Gnm::StageOffset(stage, static_cast<u32>(p3), off)) {
        fallback();
        return;
    }
    if (count == 0) {
        ::Native::Stats::SiteNative(site);
        return;
    }
    ::Native::Gnm::UserDataWrite w{off, static_cast<u32>(count), {}};
    std::memcpy(w.v.data(), values, static_cast<u32>(count) * sizeof(u32));
    const u64 cb_addr = static_cast<u64>(reinterpret_cast<uintptr_t>(cb));
    if (!::Native::Gnm::EmitCpShadow(cb_addr, [w] { ::Native::Gnm::RunGfxShWrite(w); }, ::Native::Gnm::ShadowOf(w))) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86b810: writes a type-3 packet with opcode 0x8d (header 0xc0028d00 | predicate, body of three
// dwords, raw bytes at 86b897) and a NOP 0xc0001000 with payload 0 (86b8fc). The command processor
// has no handler for opcode 0x8d: ProcessGraphics reaches its default branch (liverpool.cpp ~2192,
// UNREACHABLE_MSG), so the packet changes no port state. The only other effect is the thread-trace
// marker, taken when the guest predicate 0x882dc0 (sceGnmIsUserPaEnabled, cached) is set; this port
// returns false there (gnmdriver.cpp sceGnmIsUserPaEnabled), so that branch falls back to the guest.
// The refill check of the guest (6 dwords, callback at cb+0x18) is not reproduced: nothing is written.
extern "C" __attribute__((sysv_abi)) void GnmW08_86b810(void* cb, unsigned int p2,
                                                        unsigned int p3, int p4, int p5, int p6,
                                                        unsigned int p7, unsigned int p8, int p9) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86b810");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, unsigned int, int, int, int,
                                                 unsigned int, unsigned int, int);
    using PaFn = unsigned int(__attribute__((sysv_abi)) *)();
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86b810);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, p2, p3, p4, p5, p6, p7, p8, p9);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb) {
        fallback();
        return;
    }
    const auto user_pa = ::Native::Gnm::GuestDirect<PaFn>(0x882dc0);
    if ((user_pa() & 0xffu) != 0) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// ---- SET_SH_REG emitters without static callers (round 3, X5) ----
// 0x8695d0, 0x869710, 0x869790, 0x869810, 0x869910, 0x86d2e0, 0x86d380, 0x86d420 each write one
// SET_SH_REG packet (the last three also write a NOP that the command processor ignores). Header
// bit 1 is the shader-type bit: set = compute-ring packet (ProcessCompute, NativeCsRegs); clear =
// graphics packet (ProcessGraphics, NativeGfxCsRegs for the 0x200+ range). The body offset is the
// 16-bit reg_offset of PM4CmdSetData, so the native op masks it to 16 bits.

namespace Native::Gnm {
namespace {

// Stage offsets of 0x869790 / 0x869810 (eboot DAT_01290070, u32 per stage, stages 0..3 only; the
// entries after them are not offsets). Read from out/eboot.elf at VA + 0x4000.
constexpr std::array<u32, 4> kSlotOffset70 = {0x2e16, 0x2e17, 0x2e19, 0x2e1a};

// One SET_SH_REG packet of an emitter, captured by value at emit time.
struct ShPkt {
    u32 off;
    u32 count;
    bool compute;
    std::array<u32, 16> v;
};

// Runs inside the command processor (Where::CommandProcessor).
void RunShPkt(const ShPkt& p) {
    constexpr u32 kCsWords = sizeof(AmdGpu::ComputeProgram) / 4;
    if (p.count == 0 || p.count > p.v.size()) {
        return;
    }
    const bool cs_range = p.off >= 0x200 && p.off <= 0x200 + kCsWords;
    if (!cs_range && static_cast<u64>(AmdGpu::Regs::ShRegWordOffset) + p.off + p.count >
                         AmdGpu::Regs::NumRegs) {
        return; // outside the register image: the guest would write past it
    }
    if (p.compute) {
        ApplyShReg(Lv(), p.off, p.v.data(), p.count);
    } else {
        ApplyShRegGfx(Lv(), p.off, p.v.data(), p.count);
    }
}

// Emits one packet of `count` values; `off` is masked to the 16-bit reg_offset field.
bool EmitShPkt(void* cb, u32 off, bool compute, const u32* values, u32 count) {
    if (!cb || count == 0 || count > 16 || !values) {
        return false;
    }
    ShPkt p{};
    p.off = off & 0xffffu;
    p.count = count;
    p.compute = compute;
    std::memcpy(p.v.data(), values, count * sizeof(u32));
    const u64 cb_addr = static_cast<u64>(reinterpret_cast<uintptr_t>(cb));
    Shadow sh;
    sh.Sh(p.off, p.v.data(), p.count);
    return EmitCpShadow(cb_addr, [p] { RunShPkt(p); }, sh);
}

// The stage writers 0x86d2e0 / 0x86d380 / 0x86d420: the offset is p3 + USER_DATA[stage] - 0x2c00,
// the compute bit is the stage-0 test of the guest (header + 2 for stage 0).
bool EmitStageWrite(void* cb, u32 stage, u32 p3, const u32* values, u32 count) {
    u32 off = 0;
    if (!StageOffset(stage, p3, off)) {
        return false;
    }
    return EmitShPkt(cb, off, stage == 0, values, count);
}

// 0x869790 / 0x869810: one value (p3 & 0xffff) at the compute offset of stage p2.
bool EmitSlotWrite(void* cb, u32 p2, u32 p3) {
    if (p2 >= kSlotOffset70.size()) {
        return false;
    }
    const u32 off = kSlotOffset70[p2] - 0x2c00u;
    const u32 value = p3 & 0xffffu;
    return EmitShPkt(cb, off, true, &value, 1);
}

} // namespace
} // namespace Native::Gnm

// 0x8695d0: SET_SH_REG of `count` values (r8d, copied from rcx). Header 0xc0007600 | count << 16 |
// (flag & 1) << 1, body offset p3 - 0x2c00. Bit 1 is the low bit of esi (the compute packet when
// set). The guest writes nothing for count 0 beyond the header (the CP handler copies no value).
extern "C" __attribute__((sysv_abi)) void GnmW08_8695d0(void* cb, unsigned int flag, int p3,
                                                        const unsigned int* values,
                                                        unsigned int count) {
    static const u32 site = ::Native::Gnm::Site("w08:0x8695d0");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, const unsigned int*,
                                                 unsigned int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x8695d0);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, flag, p3, values, count);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb || count > 16 ||
        (count != 0 && !values)) {
        fallback();
        return;
    }
    if (count == 0) {
        ::Native::Stats::SiteNative(site);
        return;
    }
    const u32 off = static_cast<u32>(p3) - 0x2c00u;
    if (!::Native::Gnm::EmitShPkt(cb, off, (flag & 1u) != 0, values, count)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x869710: SET_SH_REG of one value at compute offset 0x215. Header 0xc0017602 (compute).
extern "C" __attribute__((sysv_abi)) void GnmW08_869710(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4) {
    static const u32 site = ::Native::Gnm::Site("w08:0x869710");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, unsigned int, unsigned int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x869710);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, p2, p3, p4);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb) {
        fallback();
        return;
    }
    const u32 value = ((p4 & 0x3fu) << 16) | ((p3 & 0xfu) << 12) | (p2 & 0x3ffu);
    if (!::Native::Gnm::EmitShPkt(cb, 0x215u, true, &value, 1)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x869790: SET_SH_REG of p3 & 0xffff at the compute offset of stage p2. Header 0xc0017602.
extern "C" __attribute__((sysv_abi)) void GnmW08_869790(void* cb, unsigned int p2, unsigned int p3) {
    static const u32 site = ::Native::Gnm::Site("w08:0x869790");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, unsigned int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x869790);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, p2, p3);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb ||
        !::Native::Gnm::EmitSlotWrite(cb, p2, p3)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x869810: the same body as 0x869790 (its own copy in the eboot).
extern "C" __attribute__((sysv_abi)) void GnmW08_869810(void* cb, unsigned int p2, unsigned int p3) {
    static const u32 site = ::Native::Gnm::Site("w08:0x869810");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, unsigned int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x869810);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, p2, p3);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb ||
        !::Native::Gnm::EmitSlotWrite(cb, p2, p3)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x869910: SET_SH_REG of one value at compute offset 0x218. Header 0xc0017602 (compute).
extern "C" __attribute__((sysv_abi)) void GnmW08_869910(void* cb, unsigned int p2, unsigned int p3) {
    static const u32 site = ::Native::Gnm::Site("w08:0x869910");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, unsigned int);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x869910);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, p2, p3);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !cb) {
        fallback();
        return;
    }
    const u32 value = ((p3 & 0x1fffu) << 12) | (p2 & 0xfffu);
    if (!::Native::Gnm::EmitShPkt(cb, 0x218u, true, &value, 1)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86d2e0: SET_SH_REG of four values at USER_DATA[stage] + p3 - 0x2c00 (NOP 0x68750004 ignored).
extern "C" __attribute__((sysv_abi)) void GnmW08_86d2e0(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d2e0");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, const unsigned int*);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d2e0);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, values);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !values ||
        !::Native::Gnm::EmitStageWrite(cb, stage, static_cast<u32>(p3), values, 4)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86d380: SET_SH_REG of eight values at USER_DATA[stage] + p3 - 0x2c00 (NOP 0x68750005 ignored).
extern "C" __attribute__((sysv_abi)) void GnmW08_86d380(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d380");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, const unsigned int*);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d380);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, values);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !values ||
        !::Native::Gnm::EmitStageWrite(cb, stage, static_cast<u32>(p3), values, 8)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x86d420: SET_SH_REG of four values at USER_DATA[stage] + p3 - 0x2c00 (NOP 0x68750006 ignored).
extern "C" __attribute__((sysv_abi)) void GnmW08_86d420(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values) {
    static const u32 site = ::Native::Gnm::Site("w08:0x86d420");
    using Fn = void(__attribute__((sysv_abi)) *)(void*, unsigned int, int, const unsigned int*);
    const auto guest = ::Native::Gnm::Guest<Fn>(0x86d420);
    const auto fallback = [&] {
        ::Native::Stats::SiteFallback(site);
        guest(cb, stage, p3, values);
    };
    if (!::Native::Gnm::Enabled(::Native::Gnm::Class::Shaders) || !values ||
        !::Native::Gnm::EmitStageWrite(cb, stage, static_cast<u32>(p3), values, 4)) {
        fallback();
        return;
    }
    ::Native::Stats::SiteNative(site);
}
