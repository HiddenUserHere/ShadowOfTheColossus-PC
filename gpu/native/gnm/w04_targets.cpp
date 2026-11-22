// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W04: Render targets.
// Class Targets (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w04_targets.h, and
// gpu/sotc/hook_gnm_w04.inc. See gnm_common.h for the contract.
//
// Each detour is an eboot emitter that writes the PM4 of one render-target bind. The guest body
// appends the packets to the GNM command buffer; the native replacement appends a NOP through
// EmitCp, and its op runs inline on the command processor and performs exactly what the
// SetContextReg handler (liverpool.cpp ~line 1496) does for the packets the guest would have
// written, from the decoded descriptor values.
//
// Coverage table (site | contract (what the PM4 packets do) | native op | evidence):
//   0x86a1a0 CB_COLORn surface block, emitter(cb, slot, desc)                               | REPLACED
//     surface path (desc != 0 and pred 0x87eda0(desc) != 0): one SET_CONTEXT_REG 0x0e-count
//       packet, offset 0x318 + 15*slot, 14 values = desc dwords 0..13, then a NOP (0xc0001000) and
//       the hint = desc dword 15. Native op: NativeSetRegs(0xA000+0x318+15*slot, desc[0..13], 14)
//       and NativeSetCbExtent(slot, desc[15]) (the handler's count 0x0e branch, col_buf_id = slot).
//     disabled path (otherwise): one SET_CONTEXT_REG count 1, offset 0x31c + 15*slot (CB_COLORn_INFO),
//       value 0. Native op: NativeSetRegs(0xA000+0x31c+15*slot, {0}, 1). No extent change: the
//       handler's switch does not handle the INFO register (no NOP, nothing to set).
//     slot outside 0..7 falls back to the guest body.
//     evidence: raw bytes of out/eboot.elf (file offset = VA + 0x4000, llvm-objdump -d):
//       0x86a1a0..0x86a281 checked: header C00E6900 (imm), offset = 5*slot*3 + 0x318 (lea),
//       vmovups (%r14) and 0x18(%r14) copy desc dwords 0..13 to payload, NOP C0001000 at +0x40,
//       hint = 0x3c(%r14) (desc dword 15); disabled header C0016900, offset 0x31c+15*slot, value 0.
//       The predicate 0x87eda0 (callee, bytes at 0x86a1c0 and 0x86a1fe) is called through the
//       guest body (Guest<>), so the decision is the guest's own. Decompile
//       out/re/decomp/0086/0086a1a0.c agrees. REA not used: no binary open (procedure_assembly
//       reported target_unavailable; open_binary is off-limits for W04).
//   0x86a290 DB block, emitter(cb, desc)                                                     | REPLACED
//     long path (desc != 0 and (pred 0x868710(desc) != 0 or pred 0x868ca0(desc) != 0)): 24 dwords:
//       SET_CONTEXT_REG count 8 at offset 0x10 (DB_Z_INFO) with desc dwords 0..7; SET_CONTEXT_REG
//       count 1 at 0x0f <- desc dword 11; at 0x02 <- desc dword 8; at 0x05 <- desc dword 9;
//       at 0x2af <- desc dword 10; then NOP (0xc0001000) and hint = desc dword 12.
//       Native op: NativeSetRegs(0xA010, desc[0..7], 8), NativeSetRegs(0xA00F, desc[11]),
//       NativeSetRegs(0xA002, desc[8]), NativeSetRegs(0xA005, desc[9]), NativeSetRegs(0xA2AF,
//       desc[10]), NativeSetDbExtent(desc[12]) (the handler's DbZInfo count-8 branch).
//     short path (otherwise): 6 dwords, two SET_CONTEXT_REG count 1 packets from rodata
//       0x128f9f0: offset 0x10 <- 0 and offset 0x11 <- 0. Native op: NativeSetRegs(0xA010, {0}),
//       NativeSetDbExtent(0) (DbZInfo with count != 8 sets the extent to 0), NativeSetRegs(0xA011,
//       {0}).
//     evidence: raw bytes of out/eboot.elf 0x86a290..0x86a3c2 (llvm-objdump -d): header C0086900
//       and offset 0x10, 32-byte copy of desc (vmovups (%rbx)), packets at +0x28..+0x5b as above
//       (value sources 0x2c, 0x20, 0x24, 0x28, 0x30 of desc), rodata 0x128f9f0 read with od
//       (c0016900 00000010 00000000 c0016900, then 0x11 and 0 stored by the code). Predicates
//       0x868710 / 0x868ca0 are called through the guest body with the guest's own short-circuit
//       order. Decompile out/re/decomp/0086/0086a290.c agrees. REA not used (see above).
//   0x85cbc0 SetRenderTarget (caller of 0x86a1a0, rel32 helpers 0x87f0d0 / 0x87efd0 patch the
//     descriptor)                                                                             | NOT replaced
//     the descriptor patch (0x87f0d0 / 0x87efd0) and the helpers it reads are not handled here.
//   0x87f0d0 / 0x87eda0 / 0x868710 / 0x868ca0 are pure predicates, called through the guest.
//   Round 3 (X4), context emitters D: SET_CONTEXT_REG count 1 emitters with no static callers.
//   Raw bytes: header C0016900, offset at +4, value at +8 (no extent hint: none is CB/DB base or DB_Z_INFO).
//   0x86e200 (cb, u32 v)            | REPLACED  SET_CONTEXT_REG 0x282 <- v & 0xffff
//   0x86e260 (cb, u32 hi, u32 lo)   | REPLACED  SET_CONTEXT_REG 0x280 <- (hi << 16) | (lo & 0xffff)
//   0x86e340 (cb, float xmm0)       | REPLACED  SET_CONTEXT_REG 0x2df <- float bits
//   0x86f0c0 (cb, u64 addr, u32 a, u32 b) | REPLACED (partly)  SET_CONTEXT_REG 0x2cc <- a, 0x2ca <- b,
//       then a 6-dword COPY_DATA (memory -> register 0xA2CB) that is NOT reproduced: liverpool.cpp
//       CopyData only logs "unhandled IT_COPY_DATA", so the handler's effect is nil.
//   0x86f170 (cb, u32 v)            | REPLACED  SET_CONTEXT_REG 0x2e5 <- v & 0xff
//   0x86fbd0 (cb, u32 v)            | REPLACED  SET_CONTEXT_REG 0x2a6 <- v
//   0x86fc90 (cb, u32 v)            | REPLACED  SET_CONTEXT_REG 0x2d4 <- v
//   0x86fcf0 (cb, u32 a, u32 b)     | REPLACED  SET_CONTEXT_REG 0x20d <- (a & 1) + (b & 1) * 2
//   0x86fd60 (cb, u32 v)            | REPLACED  SET_CONTEXT_REG 0xd8 <- v << 31
//   Native ops use EmitCp + Native::Gnm::WriteCtx (= NativeSetRegs(ContextRegWordOffset + reg)); a
//   failed EmitCp falls back to Guest<Fn>(va). No SOTC_W04_VERIFY comparison for these nine.
//
// Extent heuristic: the SetContextReg handler reads the trailing NOP hint (CB base count 0x0e,
// CMASK count 0x04 (no CMASK emitter replaced here), DB_Z_INFO count 8) into last_cb_extent /
// last_db_extent. NativeSetCbExtent / NativeSetDbExtent take the same values.
//
// No sceGnm* wrapper in this area; the table stays unchanged (RegisterW04 adds nothing).

#include <array>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>

#include "native/gnm/gnm_common.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"

namespace Native::Gnm {

namespace {

// Context register dword offsets, relative to AmdGpu::Regs::ContextRegWordOffset (0xA000).
constexpr u32 kCbColor0Base = 0x318;   // CB_COLOR0_BASE; CB_COLORn_BASE = 0x318 + 15 * n
constexpr u32 kCbColorStride = 0xf;    // CbColor1Base - CbColor0Base (liverpool.h)
constexpr u32 kDbZInfo = 0x10;         // DB_Z_INFO (ContextRegs::DbZInfo = 0xA010)
constexpr u32 kDbReg2 = 0x2;           // DB_DEPTH_VIEW
constexpr u32 kDbReg5 = 0x5;           // DB_HTILE_DATA_BASE
constexpr u32 kDbReg0f = 0xf;
constexpr u32 kDbReg2af = 0x2af;

// Guest SysV signatures of the detoured emitters and of the predicates they call.
typedef void (*GuestW04CbSurface)(void*, int, const void*) __attribute__((sysv_abi));
typedef void (*GuestW04Db)(void*, const void*) __attribute__((sysv_abi));
typedef unsigned int (*GuestW04Pred)(const void*) __attribute__((sysv_abi));

// Writes context registers (relative offset) and marks them for the draw thread, as the
// SetContextReg handler does. Command processor thread only.
void WriteCtx(u32 reg_offset, const u32* values, u32 count) {
    Lv().NativeSetRegs(AmdGpu::Regs::ContextRegWordOffset + reg_offset, values, count);
}


// SOTC_W04_VERIFY=1 (diagnostic, gap N2-79): the detours let the guest emit its PM4 and compare the
// packet it wrote, dword by dword, with the packet the native op is built from; mismatches are
// logged (bounded). No native op is emitted in this mode.
bool W04Verify() {
    static const bool on = [] {
        const char* e = std::getenv("SOTC_W04_VERIFY");
        return e && e[0] == '1';
    }();
    return on;
}

void W04Compare(const char* what, const u32* guest, const u32* expected, u32 count) {
    static std::atomic<int> logged{0};
    static std::atomic<int> calls{0};
    const int call = calls.fetch_add(1);
    bool same = true;
    for (u32 i = 0; i < count; ++i) {
        same = same && guest[i] == expected[i];
    }
    if ((same && call >= 4) || logged.fetch_add(1) >= 48) {
        return;
    }
    char line[1024];
    int n = std::snprintf(line, sizeof(line), "SOTC W04 VERIFY %s %s:", what, same ? "match" : "MISMATCH");
    for (u32 i = 0; i < count && n > 0 && n < static_cast<int>(sizeof(line)) - 24; ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, guest[i] == expected[i] ? " %x" : " [%x!=%x]",
                           guest[i], expected[i]);
    }
    std::fprintf(stderr, "%s\n", line);
}

} // namespace

} // namespace Native::Gnm

using Native::Gnm::Class;
using Native::Gnm::kCbColor0Base;
using Native::Gnm::kCbColorStride;
using Native::Gnm::kDbReg0f;
using Native::Gnm::kDbReg2;
using Native::Gnm::kDbReg2af;
using Native::Gnm::kDbReg5;
using Native::Gnm::kDbZInfo;

// 0x86a1a0: CB_COLORn surface block (desc != 0 and 0x87eda0(desc) != 0), else CB_COLORn_INFO = 0.
extern "C" __attribute__((sysv_abi)) void GnmW04_86a1a0(void* cb, int slot, const void* desc) {
    static const u32 site = Native::Gnm::Site("w04:86a1a0");
    using Fn = Native::Gnm::GuestW04CbSurface;
    if (!Native::Gnm::Enabled(Class::Targets) || slot < 0 || slot > 7 ||
        Native::Gnm::ForceFallback(0x86a1a0)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86a1a0)(cb, slot, desc);
        return;
    }
    // The guest's own predicate, evaluated on the same descriptor (pure: only reads it).
    const bool surface =
        desc != nullptr &&
        Native::Gnm::GuestDirect<Native::Gnm::GuestW04Pred>(0x87eda0)(desc) != 0;
    const u32 cb_slot = static_cast<u32>(slot);
    if (Native::Gnm::W04Verify()) {
        Native::Gnm::Guest<Fn>(0x86a1a0)(cb, slot, desc);
        const u32 count = surface ? 18u : 3u;
        const u32* guest = *reinterpret_cast<u32* const*>(reinterpret_cast<u64>(cb) + 0x10) - count;
        std::array<u32, 18> exp{};
        if (surface) {
            std::array<u32, 16> d{};
            std::memcpy(d.data(), desc, sizeof(d));
            exp[0] = 0xc00e6900u;
            exp[1] = kCbColor0Base + kCbColorStride * cb_slot;
            for (u32 i = 0; i < 14; ++i) {
                exp[2 + i] = d[i];
            }
            exp[16] = 0xc0001000u;
            exp[17] = d[15];
        } else {
            exp[0] = 0xc0016900u;
            exp[1] = kCbColor0Base + kCbColorStride * cb_slot + 4;
            exp[2] = 0;
        }
        Native::Gnm::W04Compare("86a1a0", guest, exp.data(), count);
        return;
    }
    if (!surface) {
        // CB_COLORn_INFO = 0. The handler ignores this register: no extent change.
        const u32 reg = kCbColor0Base + kCbColorStride * cb_slot + 4;
        if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [reg] {
                const u32 zero = 0;
                Native::Gnm::WriteCtx(reg, &zero, 1);
            })) {
            Native::Stats::SiteNative(site);
            return;
        }
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86a1a0)(cb, slot, desc);
        return;
    }
    // Surface block: words 0..13 are the 14 CB_COLORn registers from the base, word 15 is the
    // trailing NOP hint (the handler's count 0x0e branch sets last_cb_extent[n] = hint).
    std::array<u32, 16> d{};
    std::memcpy(d.data(), desc, sizeof(d));
    std::array<u32, 14> values{};
    std::memcpy(values.data(), d.data(), sizeof(values));
    const u32 base = kCbColor0Base + kCbColorStride * cb_slot;
    const u32 hint = d[15];
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [base, values, hint, cb_slot] {
            Native::Gnm::WriteCtx(base, values.data(), 14);
            Native::Gnm::Lv().NativeSetCbExtent(cb_slot, hint);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86a1a0)(cb, slot, desc);
}

// 0x86a290: DB block (long path) or two SET_CONTEXT_REG count 1 writes (short path).
extern "C" __attribute__((sysv_abi)) void GnmW04_86a290(void* cb, const void* desc) {
    static const u32 site = Native::Gnm::Site("w04:86a290");
    using Fn = Native::Gnm::GuestW04Db;
    using Pred = Native::Gnm::GuestW04Pred;
    if (!Native::Gnm::Enabled(Class::Targets)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86a290)(cb, desc);
        return;
    }
    // Same decision and short-circuit order as the guest: 0x868710 first, then 0x868ca0.
    const u32* src = static_cast<const u32*>(desc);
    bool long_form = false;
    if (src != nullptr) {
        long_form = Native::Gnm::GuestDirect<Pred>(0x868710)(src) != 0 ||
                    Native::Gnm::GuestDirect<Pred>(0x868ca0)(src) != 0;
    }
    if (!long_form) {
        // Two packets from the guest's constant: offset 0x10 <- 0 and offset 0x11 <- 0.
        if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [] {
                const u32 zero = 0;
                Native::Gnm::WriteCtx(kDbZInfo, &zero, 1);
                // DB_Z_INFO with count 1 (not 8): the handler sets the extent to 0.
                Native::Gnm::Lv().NativeSetDbExtent(0);
                Native::Gnm::WriteCtx(0x11, &zero, 1);
            })) {
            Native::Stats::SiteNative(site);
            return;
        }
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86a290)(cb, desc);
        return;
    }
    // Words 0..12 of the 13-dword descriptor (the guest reads no further).
    std::array<u32, 13> d{};
    std::memcpy(d.data(), src, sizeof(d));
    if (Native::Gnm::W04Verify()) {
        Native::Gnm::Guest<Fn>(0x86a290)(cb, desc);
        const u32* guest = *reinterpret_cast<u32* const*>(reinterpret_cast<u64>(cb) + 0x10) - 24;
        const std::array<u32, 24> exp{0xc0086900u, kDbZInfo, d[0], d[1], d[2], d[3], d[4], d[5],
                                      d[6], d[7], 0xc0016900u, kDbReg0f, d[11], 0xc0016900u,
                                      kDbReg2, d[8], 0xc0016900u, kDbReg5, d[9], 0xc0016900u,
                                      kDbReg2af, d[10], 0xc0001000u, d[12]};
        Native::Gnm::W04Compare("86a290", guest, exp.data(), 24);
        return;
    }
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [d] {
            std::array<u32, 8> depth{};
            std::memcpy(depth.data(), d.data(), sizeof(depth));
            Native::Gnm::WriteCtx(kDbZInfo, depth.data(), 8);
            Native::Gnm::WriteCtx(kDbReg0f, &d[11], 1);
            Native::Gnm::WriteCtx(kDbReg2, &d[8], 1);
            Native::Gnm::WriteCtx(kDbReg5, &d[9], 1);
            Native::Gnm::WriteCtx(kDbReg2af, &d[10], 1);
            // DB_Z_INFO with count 8: the handler takes the NOP hint (word 12) as the extent.
            Native::Gnm::Lv().NativeSetDbExtent(d[12]);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86a290)(cb, desc);
}

// ---- Context emitters D (round 3, X4). Each is a SET_CONTEXT_REG count 1 emitter (header
// 0xC0016900, offset at +4, value at +8) with no static callers. Decoded from the raw bytes of
// out/eboot.elf at file offset VA + 0x4000 (tools/re/gnm_abi_audit.py disasm), which agree with
// out/re/decomp/0086/<va>.c. Offsets are relative to ContextRegWordOffset (0xA000).

namespace {

// One SET_CONTEXT_REG count 1 packet as a native op: the handler's default case (no extent hint).
bool W04EmitContextReg(void* cb, u32 reg, u32 value) {
    return Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [reg, value] {
        Native::Gnm::WriteCtx(reg, &value, 1);
    });
}

} // namespace

// 0x86e200: SET_CONTEXT_REG 0x282 <- (u16)esi. Raw: movzx ecx, r14w.
extern "C" __attribute__((sysv_abi)) void GnmW04_86e200(void* cb, u32 value) {
    static const u32 site = Native::Gnm::Site("w04:86e200");
    typedef void (*Fn)(void*, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86e200)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86e200)(cb, value);
        return;
    }
    if (W04EmitContextReg(cb, 0x282, value & 0xffffu)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86e200)(cb, value);
}

// 0x86e260: SET_CONTEXT_REG 0x280 <- (esi << 16) | (u16)edx. Raw: shl r14d,16; movzx ecx,r15w; or.
extern "C" __attribute__((sysv_abi)) void GnmW04_86e260(void* cb, u32 hi, u32 lo) {
    static const u32 site = Native::Gnm::Site("w04:86e260");
    typedef void (*Fn)(void*, u32, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86e260)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86e260)(cb, hi, lo);
        return;
    }
    if (W04EmitContextReg(cb, 0x280, (hi << 16) | (lo & 0xffffu))) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86e260)(cb, hi, lo);
}

// 0x86e340: SET_CONTEXT_REG 0x2df <- float in xmm0 (raw bits, vmovss). The guest takes no integer
// argument: the signature is (cb in rdi, float in xmm0).
extern "C" __attribute__((sysv_abi)) void GnmW04_86e340(void* cb, float value) {
    static const u32 site = Native::Gnm::Site("w04:86e340");
    typedef void (*Fn)(void*, float) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86e340)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86e340)(cb, value);
        return;
    }
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (W04EmitContextReg(cb, 0x2df, bits)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86e340)(cb, value);
}

// 0x86f0c0: SET_CONTEXT_REG 0x2cc <- edx, SET_CONTEXT_REG 0x2ca <- ecx, then a 6-dword COPY_DATA
// (header 0xC0044000, control 1 = memory source, source address = rsi (64-bit), destination
// register 0xA2CB, dst high 0). Raw: movabs 0x1c0044000 at +0x18, source address stores at +0x20/+0x24,
// 0xa2cb at +0x28, 0 at +0x2c; total 12 dwords. The COPY_DATA is not reproduced: the command
// processor handler logs "unhandled IT_COPY_DATA" and does nothing else (liverpool.cpp CopyData).
extern "C" __attribute__((sysv_abi)) void GnmW04_86f0c0(void* cb, u64 addr, u32 a, u32 b) {
    static const u32 site = Native::Gnm::Site("w04:86f0c0");
    typedef void (*Fn)(void*, u64, u32, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86f0c0)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86f0c0)(cb, addr, a, b);
        return;
    }
    (void)addr; // only the COPY_DATA source uses it, and the handler ignores that packet
    const std::array<u32, 2> values{a, b};
    if (Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [values] {
            Native::Gnm::WriteCtx(0x2cc, &values[0], 1);
            Native::Gnm::WriteCtx(0x2ca, &values[1], 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86f0c0)(cb, addr, a, b);
}

// 0x86f170: SET_CONTEXT_REG 0x2e5 <- (u8)esi. Raw: movzx ecx, r14b.
extern "C" __attribute__((sysv_abi)) void GnmW04_86f170(void* cb, u32 value) {
    static const u32 site = Native::Gnm::Site("w04:86f170");
    typedef void (*Fn)(void*, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86f170)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86f170)(cb, value);
        return;
    }
    if (W04EmitContextReg(cb, 0x2e5, value & 0xffu)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86f170)(cb, value);
}

// 0x86fbd0: SET_CONTEXT_REG 0x2a6 <- esi.
extern "C" __attribute__((sysv_abi)) void GnmW04_86fbd0(void* cb, u32 value) {
    static const u32 site = Native::Gnm::Site("w04:86fbd0");
    typedef void (*Fn)(void*, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86fbd0)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86fbd0)(cb, value);
        return;
    }
    if (W04EmitContextReg(cb, 0x2a6, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86fbd0)(cb, value);
}

// 0x86fc90: SET_CONTEXT_REG 0x2d4 <- esi.
extern "C" __attribute__((sysv_abi)) void GnmW04_86fc90(void* cb, u32 value) {
    static const u32 site = Native::Gnm::Site("w04:86fc90");
    typedef void (*Fn)(void*, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86fc90)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86fc90)(cb, value);
        return;
    }
    if (W04EmitContextReg(cb, 0x2d4, value)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86fc90)(cb, value);
}

// 0x86fcf0: SET_CONTEXT_REG 0x20d <- (esi & 1) + 2 * (edx & 1). Raw: and r15d,1; and r14d,1; lea.
extern "C" __attribute__((sysv_abi)) void GnmW04_86fcf0(void* cb, u32 a, u32 b) {
    static const u32 site = Native::Gnm::Site("w04:86fcf0");
    typedef void (*Fn)(void*, u32, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86fcf0)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86fcf0)(cb, a, b);
        return;
    }
    if (W04EmitContextReg(cb, 0x20d, (a & 1u) + (b & 1u) * 2u)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86fcf0)(cb, a, b);
}

// 0x86fd60: SET_CONTEXT_REG 0xd8 <- esi << 31. Raw: shl r14d, 0x1f.
extern "C" __attribute__((sysv_abi)) void GnmW04_86fd60(void* cb, u32 value) {
    static const u32 site = Native::Gnm::Site("w04:86fd60");
    typedef void (*Fn)(void*, u32) __attribute__((sysv_abi));
    if (!Native::Gnm::Enabled(Class::Targets) || Native::Gnm::ForceFallback(0x86fd60)) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<Fn>(0x86fd60)(cb, value);
        return;
    }
    if (W04EmitContextReg(cb, 0xd8, value << 31)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Fn>(0x86fd60)(cb, value);
}

namespace Native::Gnm {

void RegisterW04(Table& t) {
    (void)t; // no sceGnm* wrapper in this area; the emitters above are boundary 1 only
}

} // namespace Native::Gnm
