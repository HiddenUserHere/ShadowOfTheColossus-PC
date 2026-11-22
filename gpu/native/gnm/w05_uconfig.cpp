// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W05: UConfig / index / predication / config / default hardware state.
// Class Uconfig (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w05_uconfig.h, and
// gpu/sotc/hook_gnm_w05.inc. See gnm_common.h for the contract.
//
// Each eboot detour below replaces one emitter that appends a PM4 packet to a GNM command buffer.
// The replacement appends a NOP through EmitCp and its op runs inline on the command processor and
// performs what the matching PM4 handler in liverpool.cpp does (ProcessGraphics, ~1472..1830):
//   SET_UCONFIG_REG / SET_CONFIG_REG : memcpy into regs.reg_array + MarkRegs (reg offset = PM4
//                                      dword1 bits 0..15; index bits 28..31 are ignored by the
//                                      handler, so the detours drop them too)
//   INDEX_TYPE / INDEX_BASE / INDEX_BUFFER_SIZE : assignment to the index state + MarkField
//   SET_PREDICATION : the handler only warns once, so the op is empty; the emitter's flag write on
//                     the command buffer object (+0x28) is reproduced by the detour itself
//   DrawInitDefaultHardwareState : the init packet block of gnmdriver.cpp is decoded at emit time
//                     into a list of register writes (same handler semantics) and applied by one op
//
// Coverage table (site | contract (what the PM4 packet does) | native op | evidence):
//   0x86a900 | SET_UCONFIG_REG 0x242 = VGT_PRIMITIVE_TYPE, value = arg2 & 0x3f (the index bit of
//            |   dword1 is 0x10000000 when FUN_00882ec0()==1; the handler ignores it)
//            | NativeSetRegs(0xC000+0x242, value)
//            | decomp out/re/decomp/0086/0086a900.c; raw header 00 79 01 c0 at file 0x86e950 (y)
//   0x86b5c0 | SET_UCONFIG_REG count 3 at 0x41d: values {arg2, arg6, computed}; computed =
//            |   ((arg5&0xf)<<20 + (arg3&0x3fff)*4) | uVar1 | 0x80000000, uVar1 by arg4
//            | NativeSetRegs(0xC000+0x41d, {a,b,c}, 3)
//            | decomp 0086/0086b5c0.c; raw header 00 79 03 c0 (y); operands from decomp only
//   0x86bd60 | SET_UCONFIG_REG 0x382, packed byte value (arg4, arg3, arg6/arg2, arg7/arg5)
//            | NativeSetRegs(0xC000+0x382, value)
//            | decomp 0086/0086bd60.c; raw header 00 79 01 c0 (y); operands from decomp only
//   0x86fc30 | SET_UCONFIG_REG 0x248, value = arg2
//            | NativeSetRegs(0xC000+0x248, value)
//            | decomp 0086/0086fc30.c; raw header 00 79 01 c0 (y)
//   0x8693d0 | SET_UCONFIG_REG at (arg2-0xC000), index bits from arg4 (ignored), value arg3
//            | NativeSetRegs(0xC000+((arg2-0xC000)&0xffff), value)
//            | decomp 0086/008693d0.c (no refill check in the guest body); raw bytes: not checked
//   0x869480 | SET_UCONFIG_REG at (arg2-0xC000), value arg3 (same as 0x8693d0 without index)
//            | NativeSetRegs(0xC000+((arg2-0xC000)&0xffff), value)
//            | decomp 0086/00869480.c; raw bytes: not checked
//   0x86bed0 | INDEX_TYPE 0x2a (header 00 2a 00 c0 (y)), value = (arg3!=2 ? 0x400 | (arg3&3)<<6 : 0)
//            |   | (arg2 & 0xfffffb3f); handler: index_buffer_type.raw = value + MarkField
//            | Lv().NativeRegs().index_buffer_type.raw = value; NativeMarkField
//            | decomp 0086/0086bed0.c; raw header checked y (file 0x86fed0 region)
//   0x86bf50 | INDEX_BASE 0x26 (header 00 26 01 c0 (y)), lo = arg2 & ~1, hi = (arg2>>32)&0xff
//            | index_base_address.base_addr_lo/hi + NativeMarkField
//            | decomp 0086/0086bf50.c
//   0x86bfb0 | INDEX_BUFFER_SIZE 0x13 (header 00 13 00 c0 (y)), num_indices = arg2
//            | num_indices + NativeMarkField
//            | decomp 0086/0086bfb0.c. NOTE: 0x13 is IndexBufferSize; 0x2f is NumInstances
//   0x86e770 | SET_PREDICATION 0x20 (header 00 20 01 c0 (y)) + flag |= 1 at cb+0x28
//            | no-op op (handler only warns once); flag set after EmitCp succeeds
//            | decomp 0086/0086e770.c
//   0x86e800 | SET_PREDICATION clear (header 00 20 01 c0 (y)) + flag &= 0xfe at cb+0x28
//            | no-op op; flag cleared after EmitCp succeeds | decomp 0086/0086e800.c
//   0x869430 | SET_CONFIG_REG 0x68 one value, offset (arg2-0x2000) & 0xffff (header 00 68 01 c0 (y))
//            | NativeSetRegs(0x2000+off, value) | decomp 0086/00869430.c
//   0x883140 | identical to 0x869430 | same | decomp 0088/00883140.c (header 00 68 01 c0 (y))
//   0x869530 | SET_CONFIG_REG 0x68 count arg4, offset (arg2-0x2000), values copied from arg3
//            | NativeSetRegs(0x2000+off, copy, arg4) (the copy is taken at emit time, like the
//            |   guest memcpy) | decomp 0086/00869530.c; header built from arg4 (raw: not checked)
//   0x883210 | identical to 0x869530 | same | decomp 0088/00883210.c
//   0x8839c0 | SET_UCONFIG_REG 0x41d count 3 (header 00 79 03 c0 (y), raw bytes checked), values
//            |   {arg2, arg5, computed}; computed = (arg4&0xf)<<20 + (arg3&0x3fff)*4 + 0x80000000
//            | NativeSetRegs(0xC000+0x41d, {a,b,c}, 3) (same shape as 0x86b5c0)
//            | decomp 0088/008839c0.c; raw disasm (eboot file VA+0x4000) checked y, 5 int args
//   0x883ac0 | PA-off (sceGnmIsUserPaEnabled() == false, host import): the PM4 is the packet
//            |   0xc0008c00 (opcode 0x8c, no PM4 handler: ProcessGraphics hits UNREACHABLE, dropped
//            |   here) + NOP 0xc0011000 (no effect): emits nothing. PA-on: SET_UCONFIG_REG 0x342 =
//            |   (arg3&0x3fff) + 0x88000000 + ((arg2<<14)+0x3fc000)&0x3fc000 -> NativeSetRegs(0xC000+0x342)
//            | decomp 0088/00883ac0.c; raw disasm checked y (header immediates, operands, 4 int args)
//   0x883bb0 | PA-off: NOP 0xc0011000 + 0xc0048d02 (opcode 0x8d, no PM4 handler, dropped) + NOP
//            |   0xc0001000 (count 0, no effect): emits nothing. PA-on: SET_UCONFIG_REG 0x342 =
//            |   0x89000000 (the 0xc0048d02 packet dropped, host sceGnmInsertThreadTraceMarker is a
//            |   no-op returning ORBIS_OK) -> NativeSetRegs(0xC000+0x342)
//            | decomp 0088/00883bb0.c; raw disasm checked y (6 int args: rdi..r9)
//   DrawInitDefaultHardwareState variants 0 / 175 / 200 / 350 (gnmdriver.cpp ~710..850):
//            | the 0x100-dword block = ClearContextState (CONTEXT_CONTROL, CLEAR_STATE, ACQUIRE_MEM)
//            |   + the variant table from gnmdriver_init.h (skipping its 2-word preamble) + NOP.
//            |   Decoded at emit time into register writes that mirror the PM4 handlers (SET_SH_REG
//            |   >= 0x200 goes to the gfx compute state without MarkRegs; NUM_INSTANCES; CLEAR_STATE
//            |   -> NativeClearState). 200/350 pick the Neo table when sceKernelIsNeoMode() (same as
//            |   the guest; UseNeoCompatSequences is false). Any unknown opcode or extent-hint
//            |   register makes the variant decline (PM4 path). EmitCpInSlot(0x100 dwords), *written
//            |   = 0x100 | gnmdriver_init.h, gnmdriver.cpp
//
// Skipped or not claimed (see the report for the owner):
//   0x86e5e0 : 22-dword default-state block, SET_CONTEXT_REG 0x2fe..0x30d and 0x2f5. The W01 note
//              assigns it to W05, but by the first-register rule it is context state (W03).
//   0x86c000 : SET_NUM_INSTANCES 0x2f, a draw-state write, not in the W05 list (W09 likely owns it).
//   0x86cd40, 0x86fdc0, 0x86fe50, 0x86ff30 : table-driven bundles with 0x79 / 0x11000 headers,
//              not analysed in this pass.
//   0x86ad40 | SET_BASE (opcode 0x11) index 1, 4 dwords: header 0xC0021100 | (shader_type&1)<<1, dword1 1,
//            |   address0 = addr & ~7, address1 = (addr>>32)&0xff (raw bytes checked y: rdi cb, esi type,
//            |   rdx 64-bit address). Native: NativeSetIndirectArgsAddr(same 40-bit value the handler
//            |   builds). Compute-type header bit: guest body (no compute SET_BASE handler).
//            | decomp out/re/decomp/0086/0086ad40.c
//   DrawInitToDefaultContextState variants 0 and 400 (gnmdriver.cpp sceGnmDrawInitToDefaultContextState
//            |   and ...400): CtxInitSequence(Neo) 0x20 dwords, CtxInitSequence400(Neo) 0x100 dwords,
//            |   no ClearContextState prefix. Decoded like the W05 init tables (padded so the trailing
//            |   NOP ends at the packet end); Neo chosen by sceKernelIsNeoMode (UseNeoCompat false).
//            |   Exact-size calls only (the guest returns 0 otherwise). *written = packet size.
//   0x87xxxx/0x88xxxx bundles with 0x79 / 0x68 headers (about 129 decomp files contain the W05 headers,
//              e.g. out/re/decomp/0087/0087b900.c): not covered by this pass.

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/libraries/gnmdriver/gnmdriver_init.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/process.h"
#include "native/gnm/gnm_common.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"

namespace Native::Gnm {

namespace {

// Guest SysV signatures of the detoured emitters (called from host code on the guest stack).
typedef void (*GuestW05Ptr)(void*, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW05Ctx3)(void*, unsigned int, unsigned int, int, unsigned int,
                             unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW05Packed)(void*, unsigned char, unsigned int, unsigned int, unsigned char,
                               int, int) __attribute__((sysv_abi));
typedef void (*GuestW05Index)(void*, unsigned int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW05Index64)(void*, unsigned long long) __attribute__((sysv_abi));
typedef void (*GuestW05Cfg)(void*, int, unsigned int) __attribute__((sysv_abi));
typedef void (*GuestW05CfgArr)(void*, int, unsigned long long, unsigned int)
    __attribute__((sysv_abi));
typedef void (*GuestW05Cfg4)(void*, int, unsigned int, int) __attribute__((sysv_abi));
typedef void (*GuestW05Pred)(void*) __attribute__((sysv_abi));
typedef void (*GuestW05Pred2)(void*, unsigned long long, unsigned int, unsigned int)
    __attribute__((sysv_abi));
// Round 2 (R6): total parameter counts of the compute-side UCONFIG emitters (cb included).
typedef void (*GuestW05Four)(void*, unsigned int, unsigned int, unsigned int)
    __attribute__((sysv_abi));
typedef void (*GuestW05Five)(void*, unsigned int, unsigned int, unsigned int, unsigned int)
    __attribute__((sysv_abi));
typedef void (*GuestW05Six)(void*, unsigned int, unsigned int, unsigned int, unsigned int,
                            unsigned int) __attribute__((sysv_abi));
// Round 3 (X9): SET_BASE emitter 0x86ad40 (rdi cb, esi shader type, rdx 64-bit address).
typedef void (*GuestW05SetBase)(void*, unsigned int, unsigned long long) __attribute__((sysv_abi));

// Register dword offsets (relative to the window base) of the registers written here.
constexpr u32 kVgtPrimitiveType = 0x242;
constexpr u32 kUconfigBlock41d = 0x41d;
constexpr u32 kUconfigPacked382 = 0x382;
constexpr u32 kUconfig248 = 0x248;
constexpr u32 kUconfigBase = AmdGpu::Regs::UconfigRegWordOffset;
constexpr u32 kConfigBase = AmdGpu::Regs::ConfigRegWordOffset;
constexpr u32 kContextBase = AmdGpu::Regs::ContextRegWordOffset;
constexpr u32 kShBase = AmdGpu::Regs::ShRegWordOffset;

// Context registers whose SET_CONTEXT_REG handler takes a surface-extent hint from the trailing
// NOP (liverpool.cpp SetContextReg): DB_Z_INFO and the CB colour/cmask block. The init tables
// never write them; a table that does is declined (PM4 path) instead of being replayed without
// the hint.
constexpr u32 kExtentHintLo = 0xA318;
constexpr u32 kExtentHintHi = 0xA33F;
constexpr u32 kDbZInfoAbs = 0xA010;

// True when `count` dwords at absolute dword `abs` stay inside the register file.
bool InRegFile(u32 abs, u32 count) {
    return abs < AmdGpu::Regs::NumRegs && count <= AmdGpu::Regs::NumRegs - abs;
}

// SET_CONFIG_REG / SET_UCONFIG_REG with `count` values copied from guest memory at emit time (the
// guest body's memcpy). The reg offset is PM4 dword1 bits 0..15, as the handler reads it.
bool EmitRegArray(u64 cb, u32 reg_base, u32 off_raw, u64 src, u32 count) {
    const u32 abs = reg_base + (off_raw & 0xffffu);
    if (count == 0 || count > 0x3fffu || !InRegFile(abs, count) || src == 0) {
        return false;
    }
    std::vector<u32> vals(count);
    std::memcpy(vals.data(), reinterpret_cast<const void*>(src), count * sizeof(u32));
    return Native::Gnm::EmitCp(cb, [abs, vals = std::move(vals)] {
        Native::Gnm::Lv().NativeSetRegs(abs, vals.data(), static_cast<u32>(vals.size()));
    });
}

// ---------------------------------------------------------------------------------------------
// Default hardware state (DrawInitDefaultHardwareState): decode the init tables once.

enum : u8 {
    kOpClearState,
    kOpCtx,
    kOpSh,
    kOpShCs,
    kOpUconfig,
    kOpNumInstances,
};

struct InitOp {
    u8 kind;
    u32 reg;   ///< absolute dword (kOpCtx/kOpSh/kOpUconfig) or compute-state dword (kOpShCs)
    u32 first; ///< index into InitProgram::vals
    u32 count;
};

struct InitProgram {
    std::vector<InitOp> ops;
    std::vector<u32> vals;
    bool native = false;
};

// Appends the effects of the PM4 packets in seq[0..n) to `out`. Returns false on any packet the
// replay does not reproduce exactly.
bool DecodeInit(const u32* seq, std::size_t n, InitProgram& out) {
    std::size_t i = 0;
    while (i < n) {
        const u32 h = seq[i];
        if ((h >> 30) != 3) {
            return false;
        }
        const u32 opcode = (h >> 8) & 0xffu;
        const u32 body = ((h >> 16) & 0x3fffu) + 1; // body dwords
        if (i + 1 + body > n) {
            return false;
        }
        const u32* b = seq + i + 1;
        switch (opcode) {
        case 0x10: // NOP
        case 0x28: // CONTEXT_CONTROL: no effect on the register file here
        case 0x58: // ACQUIRE_MEM: the command processor handler is empty
            break;
        case 0x12: // CLEAR_STATE: SetDefaults + MarkRegs(all)
            out.ops.push_back({kOpClearState, 0, 0, 0});
            break;
        case 0x2f: { // NUM_INSTANCES
            if (body != 1) {
                return false;
            }
            out.ops.push_back({kOpNumInstances, 0, static_cast<u32>(out.vals.size()), 1});
            out.vals.push_back(b[0]);
            break;
        }
        case 0x69: // SET_CONTEXT_REG
        case 0x76: // SET_SH_REG
        case 0x79: { // SET_UCONFIG_REG
            if (body < 2) {
                return false;
            }
            const u32 off = b[0] & 0xffffu;
            const u32 cnt = body - 1;
            const u32 first = static_cast<u32>(out.vals.size());
            if (opcode == 0x69) {
                const u32 abs = kContextBase + off;
                if (abs == kDbZInfoAbs || (abs >= kExtentHintLo && abs <= kExtentHintHi) ||
                    !InRegFile(abs, cnt)) {
                    return false;
                }
                out.ops.push_back({kOpCtx, abs, first, cnt});
            } else if (opcode == 0x76) {
                if (off >= 0x200 && off <= 0x200 + sizeof(AmdGpu::ComputeProgram) / 4) {
                    // Compute state of the gfx queue: memcpy only, no MarkRegs (handler SetShReg).
                    const u32 cs_off = off - 0x200;
                    if ((cs_off + cnt) * sizeof(u32) > sizeof(AmdGpu::ComputeProgram)) {
                        return false;
                    }
                    out.ops.push_back({kOpShCs, cs_off, first, cnt});
                } else {
                    const u32 abs = kShBase + off;
                    if (!InRegFile(abs, cnt)) {
                        return false;
                    }
                    out.ops.push_back({kOpSh, abs, first, cnt});
                }
            } else {
                const u32 abs = kUconfigBase + off;
                if (!InRegFile(abs, cnt)) {
                    return false;
                }
                out.ops.push_back({kOpUconfig, abs, first, cnt});
            }
            for (u32 k = 0; k < cnt; ++k) {
                out.vals.push_back(b[1 + k]);
            }
            break;
        }
        default:
            return false;
        }
        i += 1 + body;
    }
    return true;
}

// ClearContextState (gnmdriver.cpp) followed by the table after its 2-word preamble. The guest
// always uses clear_state = true for the four variants.
InitProgram BuildInit(const u32* seq, std::size_t n) {
    static constexpr std::array<u32, 12> kClearState{
        0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
        0x2ec47fc0u, 0xffffffffu, 0u,          0u,          0u, 10u,
    };
    InitProgram p;
    p.native = DecodeInit(kClearState.data(), kClearState.size(), p) &&
               (n >= 2) && DecodeInit(seq + 2, n - 2, p);
    return p;
}

const InitProgram& Init0() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence.data(),
                                           Libraries::GnmDriver::InitSequence.size());
    return p;
}
const InitProgram& Init175() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence175.data(),
                                           Libraries::GnmDriver::InitSequence175.size());
    return p;
}
const InitProgram& Init200() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence200.data(),
                                           Libraries::GnmDriver::InitSequence200.size());
    return p;
}
const InitProgram& Init200Neo() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence200Neo.data(),
                                           Libraries::GnmDriver::InitSequence200Neo.size());
    return p;
}
const InitProgram& Init350() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence350.data(),
                                           Libraries::GnmDriver::InitSequence350.size());
    return p;
}
const InitProgram& Init350Neo() {
    static const InitProgram p = BuildInit(Libraries::GnmDriver::InitSequence350Neo.data(),
                                           Libraries::GnmDriver::InitSequence350Neo.size());
    return p;
}

// Context init tables (gnmdriver.cpp sceGnmDrawInitToDefaultContextState / ...400): plain register
// writes with no ClearContextState prefix. Each table ends with a NOP whose body runs to the end of
// the packet; the guest copies only the table, so the rest of the packet is never written and the
// NOP payload is skipped by the command processor. The zero padding below lets DecodeInit see that
// NOP end exactly at the packet end (a table whose last packet ends earlier fails the decode, so the
// PM4 path is kept).
InitProgram BuildCtxInit(const u32* seq, std::size_t n, std::size_t total) {
    InitProgram p;
    if (n > total) {
        return p;
    }
    std::vector<u32> buf(total, 0u);
    std::memcpy(buf.data(), seq, n * sizeof(u32));
    p.native = DecodeInit(buf.data(), total, p);
    return p;
}

// Packet sizes: 0x20 dwords for variant 0, 0x100 for variant 400 (gnmdriver.cpp checks the size).
const InitProgram& Ctx0() {
    static const InitProgram p = BuildCtxInit(Libraries::GnmDriver::CtxInitSequence.data(),
                                              Libraries::GnmDriver::CtxInitSequence.size(), 0x20);
    return p;
}
const InitProgram& Ctx0Neo() {
    static const InitProgram p = BuildCtxInit(Libraries::GnmDriver::CtxInitSequenceNeo.data(),
                                              Libraries::GnmDriver::CtxInitSequenceNeo.size(), 0x20);
    return p;
}
const InitProgram& Ctx400() {
    static const InitProgram p = BuildCtxInit(Libraries::GnmDriver::CtxInitSequence400.data(),
                                              Libraries::GnmDriver::CtxInitSequence400.size(), 0x100);
    return p;
}
const InitProgram& Ctx400Neo() {
    static const InitProgram p =
        BuildCtxInit(Libraries::GnmDriver::CtxInitSequence400Neo.data(),
                     Libraries::GnmDriver::CtxInitSequence400Neo.size(), 0x100);
    return p;
}

// Runs inline on the command processor, in stream order.
void RunInit(const InitProgram& p) {
    auto& lv = Native::Gnm::Lv();
    for (const InitOp& op : p.ops) {
        switch (op.kind) {
        case kOpClearState:
            lv.NativeClearState();
            break;
        case kOpCtx:
        case kOpSh:
        case kOpUconfig:
            lv.NativeSetRegs(op.reg, &p.vals[op.first], op.count);
            break;
        case kOpShCs: {
            auto& cs = lv.NativeGfxCsRegs();
            std::memcpy(reinterpret_cast<u32*>(&cs) + op.reg, &p.vals[op.first],
                        op.count * sizeof(u32));
            break;
        }
        case kOpNumInstances: {
            auto& regs = lv.NativeRegs();
            regs.num_instances.num_instances = p.vals[op.first];
            lv.NativeMarkField(&regs.num_instances, sizeof(regs.num_instances));
            break;
        }
        default:
            break;
        }
    }
}

bool W05DrawInitDefaultHardwareState(u32* cmdbuf, u32 size, u32 variant, u32* written) {
    static const u32 site = Native::Gnm::Site("w05:DrawInitDefaultHardwareState");
    if (!Native::Gnm::Enabled(Class::Uconfig)) {
        return false;
    }
    const u32 packet_dwords = Libraries::GnmDriver::HwInitPacketSize;
    if (cmdbuf == nullptr || written == nullptr || size < packet_dwords) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    const bool neo = Libraries::Kernel::sceKernelIsNeoMode() != 0;
    const InitProgram* prog = nullptr;
    switch (variant) {
    case 0:
        prog = &Init0();
        break;
    case 175:
        prog = &Init175();
        break;
    case 200:
        prog = neo ? &Init200Neo() : &Init200();
        break;
    case 350:
        prog = neo ? &Init350Neo() : &Init350();
        break;
    default:
        Native::Stats::SiteFallback(site);
        return false;
    }
    if (!prog->native) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    if (!Native::Gnm::EmitCpInSlot(cmdbuf, packet_dwords, [prog] { RunInit(*prog); })) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    *written = packet_dwords;
    Native::Stats::SiteNative(site);
    return true;
}

// Round 3 (X9): sceGnmDrawInitToDefaultContextState (variant 0) and ...400 (variant 400). The guest
// returns 0 unless size is exactly the packet size, so only that size is taken natively.
bool W05DrawInitToDefaultContextState(u32* cmdbuf, u32 size, u32 variant, u32* written) {
    static const u32 site = Native::Gnm::Site("w05:DrawInitToDefaultContextState");
    if (!Native::Gnm::Enabled(Class::Uconfig)) {
        return false;
    }
    const u32 packet_dwords = variant == 400 ? 0x100u : 0x20u;
    if (cmdbuf == nullptr || written == nullptr || (variant != 0 && variant != 400) ||
        size != packet_dwords) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    // Neo selection as the guest: UseNeoCompatSequences is false, so the Neo table is used whenever
    // sceKernelIsNeoMode() is set.
    const bool neo = Libraries::Kernel::sceKernelIsNeoMode() != 0;
    const InitProgram& prog = variant == 400 ? (neo ? Ctx400Neo() : Ctx400())
                                             : (neo ? Ctx0Neo() : Ctx0());
    if (!prog.native) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    const InitProgram* p = &prog;
    if (!Native::Gnm::EmitCpInSlot(cmdbuf, packet_dwords, [p] { RunInit(*p); })) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    *written = packet_dwords;
    Native::Stats::SiteNative(site);
    return true;
}

// Round 4 (N2-84): the per-frame init packet of gnmdriver's submit path. After every SubmitDone the
// next submission is preceded by liverpool->SubmitGfx(InitSequenceNNN): CLEAR_STATE followed by
// the same table the DrawInitDefaultHardwareState variants copy after their ClearContextState
// prefix (CONTEXT_CONTROL and ACQUIRE_MEM of that prefix have no register-file effect), so the
// InitNNN programs reproduce it exactly. Writes one 3-dword SotcNativeOp NOP into `slot`; the
// caller submits that slot instead of the PM4 table. variant: 0, 200 or 350 (the NeoCompat tables
// are not decoded and stay on the PM4 path).
bool W05SubmitInitPacket(u32 variant, u32* slot, u32 slot_dwords) {
    static const u32 site = Native::Gnm::Site("w05:SubmitInitPacket");
    if (!Native::Gnm::Enabled(Class::Submit) && !Native::Gnm::Enabled(Class::Uconfig)) {
        return false;
    }
    const bool neo = Libraries::Kernel::sceKernelIsNeoMode() != 0;
    const InitProgram* prog = nullptr;
    switch (variant) {
    case 0:
        prog = &Init0();
        break;
    case 200:
        prog = neo ? &Init200Neo() : &Init200();
        break;
    case 350:
        prog = neo ? &Init350Neo() : &Init350();
        break;
    default:
        break;
    }
    if (prog == nullptr || !prog->native || slot == nullptr || slot_dwords < 3) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    if (!Native::Gnm::EmitCpInSlot(slot, slot_dwords, [prog] { RunInit(*prog); })) {
        Native::Stats::SiteFallback(site);
        return false;
    }
    Native::Stats::SiteNative(site);
    return true;
}

} // namespace

} // namespace Native::Gnm

using Native::Gnm::Class;

// 0x86a900: SET_UCONFIG_REG VGT_PRIMITIVE_TYPE (0x242), value = arg2 & 0x3f.
extern "C" __attribute__((sysv_abi)) void GnmW05_86a900(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w05:86a900");
    const u32 v = value & 0x3fu;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + Native::Gnm::kVgtPrimitiveType,
                                            &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Ptr>(0x86a900)(cb, value);
}

// 0x86b5c0: SET_UCONFIG_REG count 3 at 0x41d: {arg2, arg6, computed}.
extern "C" __attribute__((sysv_abi)) void GnmW05_86b5c0(void* cb, unsigned int p2,
                                                        unsigned int p3, int p4, unsigned int p5,
                                                        unsigned int p6) {
    static const u32 site = Native::Gnm::Site("w05:86b5c0");
    const u32 uvar1 = p4 == 1 ? 0u : (p4 == 2 ? 0x10000u : 0x20000u);
    const u32 v4 = (((p5 & 0xfu) * 0x100000u) + ((p3 & 0x3fffu) * 4u)) | uvar1 | 0x80000000u;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [p2, p6, v4] {
            const u32 vals[3] = {p2, p6, v4};
            Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + Native::Gnm::kUconfigBlock41d,
                                            vals, 3);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Ctx3>(0x86b5c0)(cb, p2, p3, p4, p5, p6);
}

// 0x86bd60: SET_UCONFIG_REG 0x382, packed byte value.
extern "C" __attribute__((sysv_abi)) void GnmW05_86bd60(void* cb, unsigned char p2,
                                                        unsigned int p3, unsigned int p4,
                                                        unsigned char p5, int p6, int p7) {
    static const u32 site = Native::Gnm::Site("w05:86bd60");
    const u32 v = (static_cast<u32>(p4 & 0xffu) << 16) | (static_cast<u32>(p3 & 0xffu) << 8) |
                  static_cast<u32>((p6 != 0) | (p2 & 0xfe)) |
                  (static_cast<u32>((p7 != 0) | (p5 & 0xfe)) << 24);
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + Native::Gnm::kUconfigPacked382,
                                            &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Packed>(0x86bd60)(cb, p2, p3, p4, p5, p6, p7);
}

// 0x86fc30: SET_UCONFIG_REG 0x248, value = arg2.
extern "C" __attribute__((sysv_abi)) void GnmW05_86fc30(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w05:86fc30");
    const u32 v = value;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + Native::Gnm::kUconfig248, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Ptr>(0x86fc30)(cb, value);
}

// 0x86bed0: INDEX_TYPE (0x2a), whole dword written to index_buffer_type.raw.
extern "C" __attribute__((sysv_abi)) void GnmW05_86bed0(void* cb, unsigned int p2,
                                                        unsigned int p3) {
    static const u32 site = Native::Gnm::Site("w05:86bed0");
    const u32 uvar3 = p3 != 2 ? ((p3 & 3u) << 6) : 0u;
    const u32 v = (p3 != 2 ? 0x400u : 0u) | uvar3 | (p2 & 0xfffffb3fu);
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            auto& lv = Native::Gnm::Lv();
            auto& regs = lv.NativeRegs();
            regs.index_buffer_type.raw = v;
            lv.NativeMarkField(&regs.index_buffer_type, sizeof(regs.index_buffer_type));
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Index>(0x86bed0)(cb, p2, p3);
}

// 0x86bf50: INDEX_BASE (0x26), address with bit 0 cleared (lo) and the high byte (hi).
extern "C" __attribute__((sysv_abi)) void GnmW05_86bf50(void* cb, unsigned long long p2) {
    static const u32 site = Native::Gnm::Site("w05:86bf50");
    const u32 lo = static_cast<u32>(p2) & 0xfffffffeu;
    const u32 hi = static_cast<u32>(p2 >> 32) & 0xffu;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [lo, hi] {
            auto& lv = Native::Gnm::Lv();
            auto& regs = lv.NativeRegs();
            regs.index_base_address.base_addr_lo = lo;
            regs.index_base_address.base_addr_hi = hi;
            lv.NativeMarkField(&regs.index_base_address, sizeof(regs.index_base_address));
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Index64>(0x86bf50)(cb, p2);
}

// 0x86bfb0: INDEX_BUFFER_SIZE (0x13), num_indices = arg2.
extern "C" __attribute__((sysv_abi)) void GnmW05_86bfb0(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("w05:86bfb0");
    const u32 v = value;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
            auto& lv = Native::Gnm::Lv();
            auto& regs = lv.NativeRegs();
            regs.num_indices = v;
            lv.NativeMarkField(&regs.num_indices, sizeof(regs.num_indices));
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Ptr>(0x86bfb0)(cb, value);
}

// 0x86e770: SET_PREDICATION (0x20) with an address. The command processor only warns, so the
// native op is empty; the guest's flag write on the command buffer object is kept.
extern "C" __attribute__((sysv_abi)) void GnmW05_86e770(void* cb, unsigned long long p2,
                                                        unsigned int p3, unsigned int p4) {
    static const u32 site = Native::Gnm::Site("w05:86e770");
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [] {})) {
        *(reinterpret_cast<u8*>(cb) + 0x28) |= 1;
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Pred2>(0x86e770)(cb, p2, p3, p4);
}

// 0x86e800: SET_PREDICATION clear. Empty native op; flag cleared on the command buffer object.
extern "C" __attribute__((sysv_abi)) void GnmW05_86e800(void* cb) {
    static const u32 site = Native::Gnm::Site("w05:86e800");
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [] {})) {
        *(reinterpret_cast<u8*>(cb) + 0x28) &= 0xfeu;
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Pred>(0x86e800)(cb);
}

// SET_CONFIG_REG of one value at (arg2 - 0x2000); 0x869430 and 0x883140 are the same code.
namespace {
bool EmitConfigOne(void* cb, int p2, unsigned int p3) {
    const u32 abs = Native::Gnm::kConfigBase + (static_cast<u32>(p2 - 0x2000) & 0xffffu);
    if (!Native::Gnm::InRegFile(abs, 1)) {
        return false;
    }
    const u32 v = p3;
    return Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [abs, v] {
        Native::Gnm::Lv().NativeSetRegs(abs, &v, 1);
    });
}
} // namespace

// 0x869430: SET_CONFIG_REG one value.
extern "C" __attribute__((sysv_abi)) void GnmW05_869430(void* cb, int p2, unsigned int p3) {
    static const u32 site = Native::Gnm::Site("w05:869430");
    if (Native::Gnm::Enabled(Class::Uconfig) && EmitConfigOne(cb, p2, p3)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Cfg>(0x869430)(cb, p2, p3);
}

// 0x883140: same as 0x869430.
extern "C" __attribute__((sysv_abi)) void GnmW05_883140(void* cb, int p2, unsigned int p3) {
    static const u32 site = Native::Gnm::Site("w05:883140");
    if (Native::Gnm::Enabled(Class::Uconfig) && EmitConfigOne(cb, p2, p3)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Cfg>(0x883140)(cb, p2, p3);
}

// 0x869530: SET_CONFIG_REG count arg4 at (arg2 - 0x2000); 0x883210 is the same code.
extern "C" __attribute__((sysv_abi)) void GnmW05_869530(void* cb, int p2,
                                                        unsigned long long p3, unsigned int p4) {
    static const u32 site = Native::Gnm::Site("w05:869530");
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitRegArray(reinterpret_cast<u64>(cb), Native::Gnm::kConfigBase,
                                  static_cast<u32>(p2) - 0x2000u, p3, p4)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05CfgArr>(0x869530)(cb, p2, p3, p4);
}

// 0x883210: same as 0x869530.
extern "C" __attribute__((sysv_abi)) void GnmW05_883210(void* cb, int p2,
                                                        unsigned long long p3, unsigned int p4) {
    static const u32 site = Native::Gnm::Site("w05:883210");
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::EmitRegArray(reinterpret_cast<u64>(cb), Native::Gnm::kConfigBase,
                                  static_cast<u32>(p2) - 0x2000u, p3, p4)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05CfgArr>(0x883210)(cb, p2, p3, p4);
}

// 0x8693d0: SET_UCONFIG_REG at (arg2 - 0xC000); the index bits from arg4 are ignored by the handler.
extern "C" __attribute__((sysv_abi)) void GnmW05_8693d0(void* cb, int p2, unsigned int p3,
                                                        int p4) {
    static const u32 site = Native::Gnm::Site("w05:8693d0");
    const u32 abs = Native::Gnm::kUconfigBase + (static_cast<u32>(p2 - 0xc000) & 0xffffu);
    const u32 v = p3;
    if (Native::Gnm::Enabled(Class::Uconfig) && Native::Gnm::InRegFile(abs, 1) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [abs, v] {
            Native::Gnm::Lv().NativeSetRegs(abs, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Cfg4>(0x8693d0)(cb, p2, p3, p4);
}

// 0x869480: SET_UCONFIG_REG at (arg2 - 0xC000), one value.
extern "C" __attribute__((sysv_abi)) void GnmW05_869480(void* cb, int p2, unsigned int p3) {
    static const u32 site = Native::Gnm::Site("w05:869480");
    const u32 abs = Native::Gnm::kUconfigBase + (static_cast<u32>(p2 - 0xc000) & 0xffffu);
    const u32 v = p3;
    if (Native::Gnm::Enabled(Class::Uconfig) && Native::Gnm::InRegFile(abs, 1) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [abs, v] {
            Native::Gnm::Lv().NativeSetRegs(abs, &v, 1);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Cfg>(0x869480)(cb, p2, p3);
}

// ---------------------------------------------------------------------------------------------
// Round 2 (R6): compute-side UCONFIG emitters (called from 0x85c580 and 0x862c50).

// sceGnmIsUserPaEnabled() is the guest's FUN_00882dc0 (cached import result). The port's host
// function returns false, so the PA branches of 0x883ac0 / 0x883bb0 are off in practice.
static bool UserPaEnabled() {
    return Libraries::GnmDriver::sceGnmIsUserPaEnabled();
}

// 0x8839c0: SET_UCONFIG_REG 0x41d, count 3: {arg2, arg5, computed}. The guest adds 0x80000000 to a
// sum below 2^31, so the add and the OR of the same bit agree.
extern "C" __attribute__((sysv_abi)) void GnmW05_8839c0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4, unsigned int p5) {
    static const u32 site = Native::Gnm::Site("w05:8839c0");
    const u32 v4 = ((p4 & 0xfu) * 0x100000u) + ((p3 & 0x3fffu) * 4u) + 0x80000000u;
    if (Native::Gnm::Enabled(Class::Uconfig) &&
        Native::Gnm::InRegFile(Native::Gnm::kUconfigBase + Native::Gnm::kUconfigBlock41d, 3) &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [p2, p5, v4] {
            const u32 vals[3] = {p2, p5, v4};
            Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + Native::Gnm::kUconfigBlock41d,
                                            vals, 3);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Five>(0x8839c0)(cb, p2, p3, p4, p5);
}

// 0x883ac0: PM4 = 0xc0008c00 (no PM4 handler in ProcessGraphics, so it is dropped) followed by
// SET_UCONFIG_REG 0x342 when PA is enabled, or NOP 0xc0011000 when not (no effect).
extern "C" __attribute__((sysv_abi)) void GnmW05_883ac0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4) {
    static const u32 site = Native::Gnm::Site("w05:883ac0");
    if (Native::Gnm::Enabled(Class::Uconfig)) {
        if (!UserPaEnabled()) {
            // PA off: nothing left to emit (the 0x8c packet has no handler, the NOP does nothing).
            Native::Stats::SiteNative(site);
            return;
        }
        const u32 v = (p3 & 0x3fffu) + 0x88000000u + (((p2 * 0x4000u) + 0x3fc000u) & 0x3fc000u);
        if (Native::Gnm::InRegFile(Native::Gnm::kUconfigBase + 0x342u, 1) &&
            Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
                Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + 0x342u, &v, 1);
            })) {
            Native::Stats::SiteNative(site);
            return;
        }
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Four>(0x883ac0)(cb, p2, p3, p4);
}

// 0x883bb0: PM4 = NOP 0xc0011000 or SET_UCONFIG_REG 0x342 (PA on), then 0xc0048d02 (no PM4 handler,
// dropped), NOP 0xc0001000 (count 0, no effect), and on PA on sceGnmInsertThreadTraceMarker, a host
// no-op that returns ORBIS_OK without writing.
extern "C" __attribute__((sysv_abi)) void GnmW05_883bb0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4, unsigned int p5,
                                                        unsigned int p6) {
    static const u32 site = Native::Gnm::Site("w05:883bb0");
    if (Native::Gnm::Enabled(Class::Uconfig)) {
        if (!UserPaEnabled()) {
            Native::Stats::SiteNative(site);
            return;
        }
        const u32 v = 0x89000000u;
        if (Native::Gnm::InRegFile(Native::Gnm::kUconfigBase + 0x342u, 1) &&
            Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [v] {
                Native::Gnm::Lv().NativeSetRegs(Native::Gnm::kUconfigBase + 0x342u, &v, 1);
            })) {
            Native::Stats::SiteNative(site);
            return;
        }
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05Six>(0x883bb0)(cb, p2, p3, p4, p5, p6);
}

// 0x86ad40: SET_BASE (opcode 0x11), base index 1 (DrawIndexIndirPatchTable). The emitted packet is
// 4 dwords: header 0xC0021100 | (shader_type & 1) << 1, dword1 = 1, address0 = addr & ~7 (low 32 bits),
// address1 = (addr >> 32) & 0xff. ProcessGraphics (liverpool.cpp ~1857) stores
// indirect_args_addr = address0 | (address1 & 0xffff) << 32, which is what NativeSetIndirectArgsAddr
// takes. Raw bytes (eboot file VA+0x4000): rdi cb, esi shader type, rdx address (64-bit), header via
// lea -0x3ffdef00(r15,r15). The compute-type header bit has no SET_BASE handler on the compute path,
// so that case keeps the guest body.
extern "C" __attribute__((sysv_abi)) void GnmW05_86ad40(void* cb, unsigned int shader_type,
                                                        unsigned long long addr) {
    static const u32 site = Native::Gnm::Site("w05:86ad40");
    const u64 value = static_cast<u64>(static_cast<u32>(addr) & 0xfffffff8u) |
                      (static_cast<u64>((addr >> 32) & 0xffu) << 32);
    if (Native::Gnm::Enabled(Class::Uconfig) && (shader_type & 1u) == 0 &&
        Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [value] {
            Native::Gnm::Lv().NativeSetIndirectArgsAddr(value);
        })) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestW05SetBase>(0x86ad40)(cb, shader_type, addr);
}

namespace Native::Gnm {

void RegisterW05(Table& t) {
    t.DrawInitDefaultHardwareState = &W05DrawInitDefaultHardwareState;
    t.DrawInitToDefaultContextState = &W05DrawInitToDefaultContextState;
    t.SubmitInitPacket = &W05SubmitInitPacket;
}

} // namespace Native::Gnm
