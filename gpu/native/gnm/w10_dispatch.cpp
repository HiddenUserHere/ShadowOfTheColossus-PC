// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W10: Dispatches and compute queues.
// Class Dispatch (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w10_dispatch.h, and
// gpu/sotc/hook_gnm_w10.inc. See gnm_common.h for the contract.
//
// Boundary used: boundary 2 (sceGnm* host functions in gnmdriver.cpp) and, for the compute
// SET_SH_REG writers, boundary 1 (eboot detours at the end of this file, rows in
// gpu/sotc/hook_gnm_w10.inc). The eboot dispatch wrappers (0x86b9e0, 0x86bac0, 0x883870,
// 0x883950) only call the import, so no entry detour is registered for them.
//
// Specification: the PM4 handlers of liverpool.cpp (ProcessGraphics DispatchDirect / DispatchIndirect
// around line 1766-1807, ProcessCompute SetShReg / DispatchDirect / DispatchIndirect / DmaData around
// line 2355-2468) and the sceGnm* writers in gnmdriver.cpp (line 377-441).
//
// Coverage table (sceGnm function | PM4 contract (handler) | native op | evidence):
//   sceGnmDispatchDirect(cmdbuf, size 9, x, y, z, flags)
//       | PM4 DispatchDirect, slot = header, dim_x/y/z, initiator = (flags & 0x18) + 1, NOPs.
//         Handler: patched (ProcessCompute indirect_patches) -> SubmitDispatch(indirect, src, 0, 12)
//         with dims NOT written; else dims + initiator written to the queue cs state, and
//         SubmitDispatch(direct) only when rasterizer && (initiator & 1).
//       | DispatchDirect op: first TakeIndirectPatch(s[2]); patched -> NativeSubmitDispatch(true, src,
//         0, 12). Else NativeCsRegs() dims + initiator, then NativeSubmitDispatch(false) when
//         rasterizer && (initiator & 1).
//       | gnmdriver.cpp:377-396 (read), liverpool.cpp:1766-1786 and 2424-2449 (read); PM4 bytes not
//         raw-checked (the layout is taken from the sceGnm writer and the handler's field reads).
//   sceGnmDispatchIndirect(cmdbuf, size 7, data_offset, flags)
//       | PM4 DispatchIndirect: handler (graphics ring) reads the queue cs state initiator (packet
//         body word 2 is ignored by the handler), SubmitDispatch(indirect, SET_BASE address +
//         data_offset, 12) when rasterizer && (initiator & 1).
//       | DispatchIndirect op: reads NativeIndirectArgsAddr() at run time; same gating.
//       | gnmdriver.cpp:398-416 (read), liverpool.cpp:1787-1807 (read).
//   sceGnmDispatchIndirectOnMec(cmdbuf, size 8, args, modifier)
//       | MEC/ACE DispatchIndirect (0xc0021602 header, address at dwords 1-2): handler (ProcessCompute)
//         uses the ASC queue cs state initiator, SubmitDispatch(indirect, args, 0, 12) when
//         rasterizer && (initiator & 1). The predicate bit of the header (modifier & 1) is not used
//         by the handler and is not used here.
//       | DispatchIndirectOnMec op: NativeCsRegs() gating, NativeSubmitDispatch(true, args, 0, 12).
//       | gnmdriver.cpp:418-432 (read), liverpool.cpp:2450-2468 (read).
//   sceGnmDispatchInitDefaultHardwareState(cmdbuf, size >= 0x100)
//       | SET_SH_REG compute: 0x216 = ~0, 0x217 = ~0, (Neo only) 0x219 = ~0, 0x21a = ~0, 0x215 = 0x170;
//         ACQUIRE_MEM (no-op in the CP handler), NOP padding. Writes below 0x200 + sizeof(ComputeProgram)
//         /4 go to the queue cs state (handler SetShReg branch); other offsets are refused (PM4 path).
//       | DispatchInitDefaultHardwareState op: the same dwords into NativeCsRegs(); the slot is filled
//         with 0x100 dwords and *written = 0x100 on success. Neo mode is read at emission time.
//       | gnmdriver.cpp:434-473 (read), gnmdriver_init.h (read), liverpool.cpp:2400-2417 and
//         1559-1576 (read).
//   Eboot compute SET_SH_REG writers (boundary 1, rows "native-gnm:dispatch"; site | PM4 contract |
//   native op | evidence). Ring: a compute-range offset (0x200..0x200+sizeof/4) writes
//   NativeCsRegs(), the queue being processed; ProcessCompute SetShReg (liverpool.cpp:2421-2438)
//   and ProcessGraphics SetShReg (:1570-1587) do the same there. Other offsets: the SH image.
//   0x883730(cb) | NOP 0xc0051000, body[0] = 0 (not a PayloadType; the NOP default branch ignores
//       it, liverpool.cpp:2311-2336) | nothing written (SiteNative) | decomp 0088/00883730.c;
//       raw bytes (rdi = cb only)
//   0x883d60(cb, idx, value), 0x883de0(cb, idx, value) | SET_SH_REG 0xc0017602 at
//       DAT_012905c0[idx] - 0x2c00 = {0x216, 0x217, 0x219, 0x21a} (COMPUTE_STATIC_THREAD_MGMT_SE0..3),
//       value & 0xffff | CP op into NativeCsRegs(); idx >= 4 -> guest | decomp 0088/00883d60.c and
//       0088/00883de0.c (identical after address normalization); raw bytes (rdi/esi/edx); table bytes
//       from out/eboot.elf at VA + 0x4000; callers 0x85c580 (raw asm: idx 0..1 without Neo, 0..3 with
//       Neo). FUN_00882ec0 (Neo-mode cache) is not called: it has no stream effect and 0x85c580
//       calls it before these writers
//   0x8841d0(cb, reg_base, u64 value) | SET_SH_REG 0xc0027602 at reg_base + 0x240 (compute user
//       data), values lo, hi | CP op; guest if the words leave the ComputeProgram | decomp
//       0088/008841d0.c; raw bytes (rdi, esi, rdx = 64-bit)
//   0x884240(cb, reg_base, values, count) | NOP 0xc0001000 with payload 0x6875000d (ignored by the
//       CP), then SET_SH 0xc0007602 (count = count) at reg_base + 0x240 with count values; count 0
//       writes only NOPs (nothing written); count > 16 -> guest | CP op, values copied at emission |
//       decomp 0088/00884240.c; raw bytes (rdi, esi, rdx, ecx)
//   0x884360(cb, reg_base, value) | SET_SH 0xc0017602 at reg_base + 0x240, one value | CP op |
//       decomp 0088/00884360.c; raw bytes (rdi, esi, edx)
//   0x8832b0(cb, reg_abs, values, count) | SET_SH 0xc0007602 (count = count) at reg_abs - 0x2c00 (the
//       packet offset), count values copied from values; count 0 writes only the header (nothing
//       copied) | CP op on the queue being processed (NativeCsRegs() in compute range, SH image
//       otherwise); count > 16 -> guest | decomp 0088/008832b0.c; raw bytes (rdi, esi, rdx, ecx)
//   0x883780(cb, a, b) | SET_SH 0xc0017602 at compute register 0x218 (pad3[2] of ComputeProgram),
//       value (b & 0x1fff) << 12 | (a & 0xfff) | CP op | decomp 0088/00883780.c; raw bytes (rdi, esi, edx)
//       (the guest's 3-dword reserve is the stream's own, see EmitCp)
//   0x883ce0(cb, a, b, c) | SET_SH 0xc0017602 at compute register 0x215 (COMPUTE_RESOURCE_LIMITS),
//       value (c & 0x3f) << 16 | (b & 0xf) << 12 | (a & 0x3ff) | CP op | decomp 0088/00883ce0.c; raw
//       bytes (rdi, esi, edx, ecx)
//   0x884050(cb, reg_base, values) / 0x8840d0(...) / 0x884150(...) | NOP 0xc0001000 with payload
//       0x68750004 / 0x68750005 / 0x68750006 (ignored by the CP), then SET_SH 0xc0047602 / 0xc0087602 /
//       0xc0047602 at reg_base + 0x240 (compute user data) with 4 / 8 / 4 dwords copied from values |
//       CP op (the NOP has no effect and is not emitted) | decomp 0088/00884050.c, 0088/008840d0.c,
//       0088/00884150.c; raw bytes (rdi, esi, rdx as a pointer; xmm/ymm loads of 16/32/16 bytes)
//   Payload 0x6875000d is neither SotcNativeOp (0x68750f01) nor SotcNativeWait (0x68750f02), so the
//   NOP handler ignores it. REA: no app open; all eboot evidence is decomp + raw bytes.
//   Not replicated (debug only): DebugState register dumps and ScopeMarker labels of the handlers.
//   Self-modifying hazard: the DMA (W06) must record the patch with SetIndirectPatch(s[2], src) and
//   must not copy; this file takes it with TakeIndirectPatch(s[2], src).
//
// REA: not used. The four entries are host functions (gnmdriver.cpp), not eboot code.

#include <array>
#include <cstring>

#include "common/types.h"
#include "core/libraries/gnmdriver/gnmdriver_init.h"
#include "core/libraries/kernel/process.h"
#include "native/gnm/gnm_common.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/regs.h"

namespace Native::Gnm {

namespace {

using Libraries::GnmDriver::HwInitPacketSize;
using Libraries::Kernel::sceKernelIsNeoMode;
using AmdGpu::ComputeProgram;

// Slot sizes in dwords, as the gnmdriver.cpp writers fill them.
constexpr u32 kDispatchDirectSlot = 9;
constexpr u32 kDispatchIndirectSlot = 7;
constexpr u32 kDispatchMecSlot = 8;
// sizeof(PM4CmdDispatchIndirect::GroupDimensions): three dwords.
constexpr u32 kGroupDimensionBytes = 12;

bool W10DispatchDirect(u32* cmdbuf, u32 size, u32 threads_x, u32 threads_y, u32 threads_z,
                       u32 flags) {
    static const u32 site = Site("w10:DispatchDirect");
    if (!Enabled(Class::Dispatch)) {
        Stats::SiteFallback(site);
        return false;
    }
    // Same argument check as sceGnmDispatchDirect: a negative dimension takes the PM4 path.
    if (!cmdbuf || size != kDispatchDirectSlot ||
        static_cast<s32>(threads_x | threads_y | threads_z) < 0) {
        return false;
    }
    // Initiator as the sceGnm writer stores it: bit 0 set, ordered-append bits from flags.
    const u32 initiator = (flags & 0x18u) + 1u;
    Shadow sh; // prefetch shadow (gap N2-88)
    sh.DispatchDirect(threads_x, threads_y, threads_z, initiator);
    const bool ok = EmitCpInSlotShadow(
        cmdbuf, kDispatchDirectSlot,
        [s = cmdbuf, threads_x, threads_y, threads_z, initiator] {
            // The NOP id dword sits at slot[2] (dst + 4 of the DMA patch layout).
            VAddr src = 0;
            if (TakeIndirectPatch(s[2], src)) {
                // Patched by a compute DMA (ProcessCompute indirect_patches): indirect dispatch,
                // dims not written, as the handler does.
                if (Lv().NativeRasterizer()) {
                    Lv().NativeSubmitDispatch(true, src, 0, kGroupDimensionBytes);
                }
                return;
            }
            auto& cs = Lv().NativeCsRegs();
            cs.dim_x = threads_x;
            cs.dim_y = threads_y;
            cs.dim_z = threads_z;
            cs.dispatch_initiator = initiator;
            if (!Lv().NativeRasterizer() || (initiator & 1) == 0) {
                return;
            }
            Lv().NativeSubmitDispatch(false, 0, 0, 0);
        },
        sh);
    if (!ok) {
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool W10DispatchIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 flags) {
    static const u32 site = Site("w10:DispatchIndirect");
    (void)flags; // The handler takes the initiator from the queue state, not from the packet.
    if (!Enabled(Class::Dispatch)) {
        Stats::SiteFallback(site);
        return false;
    }
    if (!cmdbuf || size != kDispatchIndirectSlot) {
        return false;
    }
    Shadow sh; // prefetch shadow (gap N2-88); the walker takes the initiator from its state
    sh.DispatchIndirect(data_offset, 1u);
    const bool ok = EmitCpInSlotShadow(cmdbuf, kDispatchIndirectSlot, [data_offset] {
        auto& cs = Lv().NativeCsRegs();
        if (!Lv().NativeRasterizer() || (cs.dispatch_initiator & 1) == 0) {
            return;
        }
        // The args base is SET_BASE state: read it when the op runs, not at emission.
        Lv().NativeSubmitDispatch(true, Lv().NativeIndirectArgsAddr(), data_offset,
                                  kGroupDimensionBytes);
    }, sh);
    if (!ok) {
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool W10DispatchIndirectOnMec(u32* cmdbuf, u32 size, VAddr args, u32 modifier) {
    static const u32 site = Site("w10:DispatchIndirectOnMec");
    (void)modifier; // Header predicate bit: not used by the ProcessCompute handler either.
    if (!Enabled(Class::Dispatch)) {
        Stats::SiteFallback(site);
        return false;
    }
    // Same argument check as sceGnmDispatchIndirectOnMec.
    if (!cmdbuf || size != kDispatchMecSlot || args == 0 || (args & 3u) != 0) {
        return false;
    }
    Shadow sh; // prefetch shadow (gap N2-88)
    sh.DispatchIndirect(0u, 1u);
    const bool ok = EmitCpInSlotShadow(cmdbuf, kDispatchMecSlot, [args] {
        auto& cs = Lv().NativeCsRegs();
        if (!Lv().NativeRasterizer() || (cs.dispatch_initiator & 1) == 0) {
            return;
        }
        Lv().NativeSubmitDispatch(true, args, 0, kGroupDimensionBytes);
    }, sh);
    if (!ok) {
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

bool W10DispatchInitDefaultHardwareState(u32* cmdbuf, u32 size, u32* written) {
    static const u32 site = Site("w10:DispatchInitDefaultHardwareState");
    if (!Enabled(Class::Dispatch)) {
        Stats::SiteFallback(site);
        return false;
    }
    if (!cmdbuf || !written || size < HwInitPacketSize) {
        return false;
    }
    struct Write {
        u32 reg;   // SH-relative register offset as the SetShReg packet carries it
        u32 value;
    };
    // Compute SH writes of the sceGnm writer, in packet order.
    std::array<Write, 6> writes{};
    u32 count = 0;
    writes[count++] = {0x216u, 0xffffffffu}; // COMPUTE_STATIC_THREAD_MGMT_SE0
    writes[count++] = {0x217u, 0xffffffffu}; // COMPUTE_STATIC_THREAD_MGMT_SE1
    if (sceKernelIsNeoMode()) {
        writes[count++] = {0x219u, 0xffffffffu}; // COMPUTE_STATIC_THREAD_MGMT_SE2
        writes[count++] = {0x21au, 0xffffffffu}; // COMPUTE_STATIC_THREAD_MGMT_SE3
    }
    writes[count++] = {0x215u, 0x170u}; // COMPUTE_RESOURCE_LIMITS
    // Only the queue cs state branch of the SetShReg handler is replicated; any other offset
    // would need the shader-register image, so the PM4 path keeps those.
    for (u32 i = 0; i < count; ++i) {
        const u32 reg = writes[i].reg;
        if (reg < 0x200u || reg - 0x200u >= sizeof(ComputeProgram) / 4) {
            return false;
        }
    }
    // ACQUIRE_MEM and the trailing NOP have no command processor effect (handler: no-op).
    Shadow sh; // prefetch shadow (gap N2-88)
    for (u32 i = 0; i < count; ++i) {
        sh.Sh(writes[i].reg, &writes[i].value, 1);
    }
    const bool ok = EmitCpInSlotShadow(cmdbuf, HwInitPacketSize, [writes, count] {
        auto& cs = Lv().NativeCsRegs();
        auto* cs_dwords = reinterpret_cast<u32*>(&cs);
        for (u32 i = 0; i < count; ++i) {
            cs_dwords[writes[i].reg - 0x200u] = writes[i].value;
        }
    }, sh);
    if (!ok) {
        return false;
    }
    *written = HwInitPacketSize;
    Stats::SiteNative(site);
    return true;
}

// ---- eboot compute SET_SH_REG writers: shared helpers (the detours are below) ----

using ShWriteFn = void(__attribute__((sysv_abi)) *)(void*, u32, u32);
using Set2Fn = void(__attribute__((sysv_abi)) *)(void*, u32, u64);
using SetNFn = void(__attribute__((sysv_abi)) *)(void*, u32, const u32*, int);
using Nop7Fn = void(__attribute__((sysv_abi)) *)(void*);
using ShRegNFn = void(__attribute__((sysv_abi)) *)(void*, u32, const u32*, u32);
using Sh3Fn = void(__attribute__((sysv_abi)) *)(void*, u32, u32, u32);
using UserDataFn = void(__attribute__((sysv_abi)) *)(void*, u32, const unsigned long long*);

// COMPUTE_STATIC_THREAD_MGMT_SE0..SE3 offsets: the guest table DAT_012905c0 (0x2e16, 0x2e17, 0x2e19,
// 0x2e1a, read from out/eboot.elf) minus 0x2c00.
constexpr std::array<u32, 4> kThreadMgmtRegs = {0x216u, 0x217u, 0x219u, 0x21au};
// Compute user data of the SET_SH_REG writers: the packets carry reg_base + 0x240.
constexpr u32 kCsUserDataBase = 0x240u;
constexpr u32 kMaxSetValues = 16;
constexpr u32 kCsRegWords = sizeof(ComputeProgram) / 4;

// The packet's offset test: an offset in [0x200, 0x200 + sizeof/4] lands in the compute state.
// The handler ASSERTs past the struct, so a write that would leave it stays on the PM4 path.
bool ShRegFits(u32 reg, u32 n) {
    if (reg >= 0x200u && reg <= 0x200u + kCsRegWords) {
        return reg - 0x200u + n <= kCsRegWords;
    }
    return true;
}

// SET_SH_REG as the handler does it, on the queue being processed (NativeCsRegs()).
void ApplyShReg(u32 reg, const u32* vals, u32 n) {
    AmdGpu::Liverpool& lv = Lv();
    if (reg >= 0x200u && reg <= 0x200u + kCsRegWords) {
        u32* dst = reinterpret_cast<u32*>(&lv.NativeCsRegs()) + (reg - 0x200u);
        std::memcpy(dst, vals, n * sizeof(u32));
        return;
    }
    lv.NativeSetRegs(AmdGpu::Regs::ShRegWordOffset + reg, vals, n);
}

// Dwords of the SH register image past ShRegWordOffset: a write must stay inside regs.reg_array
// (NativeSetRegs), so a wrapped or oversized offset takes the guest body.
constexpr u32 kShSpanDwords = AmdGpu::Regs::NumRegs - AmdGpu::Regs::ShRegWordOffset;

// Emits one SET_SH_REG of n >= 1 values as a command processor op. The values are copied now.
// False when the op cannot be written (the caller then runs the guest body).
bool EmitShWrite(void* cb, u32 reg, const u32* vals, u32 n) {
    if (!cb || !vals || n == 0 || n > kMaxSetValues || !ShRegFits(reg, n)) {
        return false;
    }
    if (reg > kShSpanDwords || n > kShSpanDwords - reg) {
        return false;
    }
    std::array<u32, kMaxSetValues> v{};
    std::memcpy(v.data(), vals, n * sizeof(u32));
    Shadow sh; // prefetch shadow (gap N2-88)
    sh.Sh(reg, v.data(), n);
    return EmitCpShadow(static_cast<u64>(reinterpret_cast<uintptr_t>(cb)),
                        [reg, v, n] { ApplyShReg(reg, v.data(), n); }, sh);
}

// 0x883d60 / 0x883de0: SET_SH_REG of one thread-management register, value & 0xffff.
bool EmitThreadMgmt(void* cb, u32 index, u32 value) {
    if (!Enabled(Class::Dispatch) || index >= kThreadMgmtRegs.size()) {
        return false;
    }
    const u32 word = value & 0xffffu;
    return EmitShWrite(cb, kThreadMgmtRegs[index], &word, 1);
}

// 0x8832b0: SET_SH_REG of `count` values at the absolute SH register reg_abs; the packet carries
// reg_abs - ShRegWordOffset. count 0 writes only the header, which the handler copies nothing for.
bool EmitShRegN(void* cb, u32 reg_abs, const u32* values, u32 count) {
    if (!Enabled(Class::Dispatch)) {
        return false;
    }
    if (count == 0) {
        return cb != nullptr;
    }
    if (reg_abs < AmdGpu::Regs::ShRegWordOffset) {
        return false;
    }
    return EmitShWrite(cb, reg_abs - AmdGpu::Regs::ShRegWordOffset, values, count);
}

// 0x884050 / 0x8840d0 / 0x884150: SET_SH_REG of n_dwords values at reg_base + 0x240 (compute user
// data). The NOP that precedes the packet has no command processor effect and is not emitted.
bool EmitUserData(void* cb, u32 reg_base, const unsigned long long* values, u32 n_dwords) {
    if (!Enabled(Class::Dispatch) || reg_base > kShSpanDwords) {
        return false;
    }
    return EmitShWrite(cb, reg_base + kCsUserDataBase,
                       reinterpret_cast<const u32*>(values), n_dwords);
}

} // namespace

void RegisterW10(Table& t) {
    t.DispatchDirect = &W10DispatchDirect;
    t.DispatchIndirect = &W10DispatchIndirect;
    t.DispatchIndirectOnMec = &W10DispatchIndirectOnMec;
    t.DispatchInitDefaultHardwareState = &W10DispatchInitDefaultHardwareState;
}

// ---- eboot detours (boundary 1); declared in gpu/sotc/hook_gnm_w10.inc ----

// 0x883730: NOP 0xc0051000 with body[0] = 0. The command processor ignores it, so nothing is
// written; the guest cursor stays where it is (hardware skips NOPs too).
extern "C" __attribute__((sysv_abi)) void GnmW10_883730(void* cb) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x883730");
    if (!Enabled(Class::Dispatch)) {
        ::Native::Stats::SiteFallback(site);
        Guest<Nop7Fn>(0x883730)(cb);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x883d60 / 0x883de0: SET_SH_REG of the compute thread-management register for stage index.
extern "C" __attribute__((sysv_abi)) void GnmW10_883d60(void* cb, unsigned int index,
                                                        unsigned int value) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x883d60");
    if (!EmitThreadMgmt(cb, index, value)) {
        ::Native::Stats::SiteFallback(site);
        Guest<ShWriteFn>(0x883d60)(cb, index, value);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW10_883de0(void* cb, unsigned int index,
                                                        unsigned int value) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x883de0");
    if (!EmitThreadMgmt(cb, index, value)) {
        ::Native::Stats::SiteFallback(site);
        Guest<ShWriteFn>(0x883de0)(cb, index, value);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x8841d0: SET_SH_REG of two values (a 64-bit word, low dword first) at reg_base + 0x240.
extern "C" __attribute__((sysv_abi)) void GnmW10_8841d0(void* cb, unsigned int reg_base,
                                                        unsigned long long value) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x8841d0");
    const u32 vals[2] = {static_cast<u32>(value), static_cast<u32>(value >> 32)};
    const bool ok = Enabled(Class::Dispatch) &&
                    EmitShWrite(cb, reg_base + kCsUserDataBase, vals, 2);
    if (!ok) {
        ::Native::Stats::SiteFallback(site);
        Guest<Set2Fn>(0x8841d0)(cb, reg_base, value);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x884240: NOP + payload 0x6875000d (no CP effect), then SET_SH_REG of `count` values at
// reg_base + 0x240. count 0 has only the NOPs, so nothing is written.
extern "C" __attribute__((sysv_abi)) void GnmW10_884240(void* cb, unsigned int reg_base,
                                                        const unsigned int* values, int count) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x884240");
    const u32 reg = reg_base + kCsUserDataBase;
    bool ok = false;
    if (Enabled(Class::Dispatch) && count >= 0 && static_cast<u32>(count) <= kMaxSetValues) {
        if (count == 0) {
            ok = true;
        } else {
            ok = EmitShWrite(cb, reg, values, static_cast<u32>(count));
        }
    }
    if (!ok) {
        ::Native::Stats::SiteFallback(site);
        Guest<SetNFn>(0x884240)(cb, reg_base, values, count);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x884360: SET_SH_REG of one value at reg_base + 0x240.
extern "C" __attribute__((sysv_abi)) void GnmW10_884360(void* cb, unsigned int reg_base,
                                                        unsigned int value) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x884360");
    const bool ok = Enabled(Class::Dispatch) &&
                    EmitShWrite(cb, reg_base + kCsUserDataBase, &value, 1);
    if (!ok) {
        ::Native::Stats::SiteFallback(site);
        Guest<ShWriteFn>(0x884360)(cb, reg_base, value);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x8832b0: SET_SH_REG of count values at the absolute register reg_abs (rdi = cb, esi = reg_abs,
// rdx = values, ecx = count).
extern "C" __attribute__((sysv_abi)) void GnmW10_8832b0(void* cb, unsigned int reg_abs,
                                                        const unsigned int* values,
                                                        unsigned int count) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x8832b0");
    if (!EmitShRegN(cb, reg_abs, values, count)) {
        ::Native::Stats::SiteFallback(site);
        Guest<ShRegNFn>(0x8832b0)(cb, reg_abs, values, count);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x883780: SET_SH_REG of compute register 0x218 (rdi = cb, esi = a, edx = b).
extern "C" __attribute__((sysv_abi)) void GnmW10_883780(void* cb, unsigned int a, unsigned int b) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x883780");
    const u32 word = ((b & 0x1fffu) << 12) | (a & 0xfffu);
    const bool ok = Enabled(Class::Dispatch) && EmitShWrite(cb, 0x218u, &word, 1);
    if (!ok) {
        ::Native::Stats::SiteFallback(site);
        Guest<ShWriteFn>(0x883780)(cb, a, b);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x883ce0: SET_SH_REG of compute register 0x215, COMPUTE_RESOURCE_LIMITS (rdi = cb, esi = a,
// edx = b, ecx = c).
extern "C" __attribute__((sysv_abi)) void GnmW10_883ce0(void* cb, unsigned int a, unsigned int b,
                                                        unsigned int c) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x883ce0");
    const u32 word = ((c & 0x3fu) << 16) | ((b & 0xfu) << 12) | (a & 0x3ffu);
    const bool ok = Enabled(Class::Dispatch) && EmitShWrite(cb, 0x215u, &word, 1);
    if (!ok) {
        ::Native::Stats::SiteFallback(site);
        Guest<Sh3Fn>(0x883ce0)(cb, a, b, c);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x884050: NOP (payload 0x68750004) then SET_SH_REG of 2 u64 (4 dwords) at reg_base + 0x240.
extern "C" __attribute__((sysv_abi)) void GnmW10_884050(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x884050");
    if (!EmitUserData(cb, reg_base, values, 4)) {
        ::Native::Stats::SiteFallback(site);
        Guest<UserDataFn>(0x884050)(cb, reg_base, values);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x8840d0: NOP (payload 0x68750005) then SET_SH_REG of 4 u64 (8 dwords) at reg_base + 0x240.
extern "C" __attribute__((sysv_abi)) void GnmW10_8840d0(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x8840d0");
    if (!EmitUserData(cb, reg_base, values, 8)) {
        ::Native::Stats::SiteFallback(site);
        Guest<UserDataFn>(0x8840d0)(cb, reg_base, values);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

// 0x884150: NOP (payload 0x68750006) then SET_SH_REG of 2 u64 (4 dwords) at reg_base + 0x240.
extern "C" __attribute__((sysv_abi)) void GnmW10_884150(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values) {
    using namespace ::Native::Gnm;
    static const u32 site = Site("w10:0x884150");
    if (!EmitUserData(cb, reg_base, values, 4)) {
        ::Native::Stats::SiteFallback(site);
        Guest<UserDataFn>(0x884150)(cb, reg_base, values);
        return;
    }
    ::Native::Stats::SiteNative(site);
}

} // namespace Native::Gnm
