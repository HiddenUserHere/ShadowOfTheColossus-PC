// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W07: EOP / EOS / WAIT_REG_MEM / ACQUIRE_MEM / EVENT_WRITE / labels.
// Class Sync (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w07_sync.h, and
// gpu/sotc/hook_gnm_w07.inc. See gnm_common.h for the contract.
//
// Method: each eboot emitter is decoded from its Ghidra export (out/re/decomp/0086/*.c) and the
// PM4 dwords it would write are rebuilt exactly (same masks and shifts). The native op then feeds
// those decoded dwords to the same PM4 struct the command processor parses and runs the same
// handler logic (liverpool.cpp ProcessGraphics), through the Liverpool native facade. A case the
// facade cannot reproduce (private state of the handler) is decided at emit time and falls back to
// the guest body, so nothing is emitted twice.
//
// Coverage table (VA | contract of the PM4 packet | native op | evidence):
//   0x86d210 label slot (NOP-wrapped, returns slot) | Type-3 NOP carrying the slot; command processor
//            skips the body; caller zeroes the slot itself | EmitWithSpace(CP, empty op, body =
//            (size+3)/4 dwords, align = 1<<log2); returns slot, 0 -> guest body | decomp
//            out/re/decomp/0086/0086d210.c; raw bytes NOT checked; REA not open (flat export used).
//   0x86eb60 EVENT_WRITE_EOP (0xC0044700) | fence write + IRQ GfxEop, deferred to the draw thread
//            (liverpool.cpp:1882-1955, back_strict) | rebuild 6 dwords; op = NativeBack(OnFence;
//            SignalFence(NativeWriteFenceValue, IRQ GfxEop), SOTC_CP_STRICT) | decomp 0086/0086eb60.c;
//            raw bytes checked y (header 0xc0044700, DW1 int_sel<<25 via shl 0x19, store [rax+4]);
//            fallback when SOTC_VIS_DIRECT=1 or SOTC_EOP_DEFER=1 (handler paths need private state).
//   0x86edc0 WAIT_REG_MEM (0xC0053C00) | spin until (value & mask) matches ref, on CP | rebuild 7
//            dwords; op = spin on wait.Test(regs) (liverpool.cpp:2062-2080, VO special case omitted) |
//            decomp 0086/0086edc0.c; raw bytes NOT checked.
//   0x86eec0 WAIT_REG_MEM PFP engine (0xC0053C00 with engine bit) | same as above with DW1 0x105 |
//            decomp 0086/0086eec0.c; raw bytes NOT checked.
//   0x86ced0 EVENT_WRITE (0xC0004600) | SO_VGT_STREAMOUT_FLUSH sets cp_strmout_cntl.offset_update_done;
//            other events no-op (liverpool.cpp:1833-1861) | index from the guest table DAT_012901d0
//            (ImageQword), op = streamout flag or empty; fallback for PixelPipeStatDump+ZpassDone
//            (occlusion counters are private state) | decomp 0086/0086ced0.c; raw bytes NOT checked.
//   0x86f1d0 ACQUIRE_MEM (0xC0055800) + optional EVENT_WRITE 0x72a | both no-ops in the port
//            (liverpool.cpp:2047 / 2551, 0x2a is not SO_VGT_STREAMOUT_FLUSH) | empty op |
//            decomp 0086/0086f1d0.c; raw bytes NOT checked; callers 2.
//   0x86cf40 ACQUIRE_MEM (0xC0055800) | no-op in the port | empty op | decomp 0086/0086cf40.c;
//            raw bytes NOT checked; callers 3.
//   0x86ee70 PFP_SYNC_ME (0xC0004200) | no-op in the port (liverpool.cpp PfpSyncMe) | empty op |
//            decomp 0086/0086ee70.c; raw bytes NOT checked; callers 13.
//   0x86f5e0 EVENT_WRITE_EOS SignalFence (0xC0034800/02) | OnFence, then SignalFence write
//            (liverpool.cpp:1862-1881, back_strict) | rebuild 5 dwords; op = NativeBack(OnFence;
//            SignalFence) | decomp 0086/0086f5e0.c; raw bytes NOT checked; callers 2.
//   Not replaced: 0x86f530 (EOS GDS store, command 1; no callers in the export);
//            0x8852a0, 0x86fac0, 0x86fb50 (MEM_SEMAPHORE 0xC0013900; no callers in the export).
//   R2 (round 2, R5 compute and remaining sync):
//   0x883f80 label slot on queue 2 (NOP payload 0x68750000) | same as 0x86d210: EmitWithSpace(CP),
//            empty op, body = ceil(size/4) dwords, align 1 << log2; 0 -> guest | decomp
//            out/re/decomp/0088/00883f80.c; raw bytes checked (args rdi/rsi/edx, same body).
//   0x86f4e0 INCREMENT_DE_COUNTER (0xc0008500, 2 dwords) | ProcessGraphics IncrementDeCounter:
//            ++cblock.de_count, inline (EmitCp) | decomp out/re/decomp/0086/0086f4e0.c; raw bytes
//            checked (mov ecx, 0xc0008500 at [rax], cursor + 8); callers: 0x85c580 (gfx ring).
//   0x86f3f0 NOP marker, payload 0x68750012 (4 or 6 dwords) | no PM4 at all: the command processor's
//            NOP switch has no case for 0x68750012 (default break) and nothing in the port reads it;
//            the native path writes nothing | decomp 0086/0086f3f0.c; raw bytes checked (header
//            formula, payload at [rax + 4]); caller 0x85c580.
//   Kept on the guest body (still produce PM4), with the reason:
//   0x8846f0 RELEASE_MEM (0xc0054902, callers on the compute rings) | the handler signals an
//            interrupt with queue.pipe_id (liverpool.cpp ProcessCompute), and int_sel is always 3
//            (raw bytes: dw2 bits 24-25 = 3), so the IRQ id needs asc_queues[vqid].pipe_id, which the
//            facade does not expose. Needs core_request NativeAscPipeId (see the report).
//   0x86ea50 / 0x883300 COND_EXEC (0xc0032200) | the handler skips exec_count dwords of PM4 in the
//            parser (ProcessGraphics CondExec); a native op runs where it is placed and cannot skip,
//            and the count is measured from the guest cursor after the body (0x85cea0 / 0x85d260),
//            so only the PM4 body can be skipped. Left as is.
//   0x86f680 reservation of 5 dwords (called only by 0x85cea0): the COND_EXEC above is written over
//            these dwords in place (0x86ea50 writes at cursor - 0x14), so a native op there would be
//            overwritten and lost. Left as is.
//   R3 (round 3, X7: remaining WAIT_REG_MEM and the sceGnm compute wait). Raw bytes checked for every
//   row (eboot file offset VA + 0x4000, llvm-mc); each wait is an EmitWait (yielding poll, gap N2-76)
//   whose predicate is PM4CmdWaitRegMem::Test on the dwords the guest body writes:
//   0x86cff0 WAIT_REG_MEM register space, ME (0xC0053C00) | (cb, esi addr16, edx mask, ecx func,
//            r8d ref) | dw1 = func & 7, dw2 = addr & 0xffff, dw3 = 0, dw4 = ref, dw5 = mask, dw6 = 10 |
//            no static callers (function-pointer table) | raw bytes checked.
//   0x86e9e0 / 0x884b00 WAIT_REG_MEM generic, engine and poll from the caller | (cb, esi func,
//            edx engine, rcx addr64, r8d ref, r9d mask, [rsp+16] poll as word) | dw1 = (engine & 1) << 8 |
//            space << 4 | func & 7, dw2/dw3 = addr (mask 0xffff or 0xfffffffc), dw4 = ref, dw5 = mask,
//            dw6 = poll | the two bodies are byte-identical | raw bytes checked.
//   0x884a60 WAIT_REG_MEM label wait (0xC0053C00) | (cb, rsi p2) waits on the dword at p2 - 4
//            until (value & 0x80000000) == 0x80000000 (function 3 = Equal, ME, poll 10), space from
//            p2 - 4 > 0xffff | raw bytes checked.
//   sceGnmComputeWaitOnAddress (Table::ComputeWaitOnAddress, gnmdriver.cpp): the PM4 fallback writes
//            one 14-dword slot (NOP 4 + WAIT_REG_MEM 7 + trailing NOP 3) and returns -1 for any other size;
//            the native path only takes size == 0xe (else declines, so the fallback still returns -1),
//            fills the slot with EmitWaitInSlot and the same predicate (poll 10, space from addr > 0xffff).
//   Not replaced: 0x86e860 (WAIT_REG_MEM on the PFP engine when param_2 == 0 is preceded by a 256-byte
//            DMA_DATA; param_2 != 0 and the tail write EVENT_WRITE 0x138 / 0x139 with an address
//            (PIXEL_PIPE_STAT_DUMP + ZPASS_DONE on occlusion counters, private state) and a cache-flush
//            event). One emitter with several packets, the DMA semantics are not decoded here, so the
//            guest body stays.
//   REA: no binary open (procedure_* returned target_unavailable); used the flat exports instead.
//
//   W2 (N2-94, SOTC_NATIVE_VLABELS; every row is off when the switch is off, so the default is unchanged):
//   item | done? | evidence
//   EOP 0x86eb60 Record(masked addr, value, bytes) for data_sel 1 (4 bytes) and 2 (8 bytes), on the CP
//            inside the op, before NativeBack; data_sel 0 / 3 / 4 not recorded | yes | pm4_cmds.h
//            PM4CmdEventWriteEop::SignalFence (DataSelect None/Data32Low/Data64/GpuClock64/PerfCounter)
//   EOP draw-thread lambda: Retire(token) right after SignalFence, then Progress::Notify (token != 0 only) | yes | in lambda
//   EOS 0x86f5e0 Record(addr, value, 4) always (command SignalFence = 2, cmd_info 0x40000000) | yes | pm4_cmds.h
//            PM4CmdEventWriteEos::SignalFence; address = address_lo | address_hi << 32, same masks as the packet
//   EOS draw-thread lambda: Retire + Notify | yes | in lambda
//   Wait predicates (WaitPredicate): memory-space only (dw[1] bit 4), Satisfies(addr, mask, func, ref) first,
//            then PM4CmdWaitRegMem::Test | yes | used by 0x86edc0, 0x86eec0, 0x86e9e0, 0x884b00, 0x884a60,
//            ComputeWaitOnAddressNative; register-space 0x86cff0 unchanged (mem bit 0, never virtual)
//   Excluded here: game-CPU writes and deferred/late labels are handled by native_labels (Exclude) and by the
//            SOTC_EOP_DEFER / SOTC_VIS_DIRECT fallback (EOP always falls back then, so nothing is recorded)
//   REA producer/consumer check (N2-94, read-only, target eboot_pe_fde.exe, Ghidra 12.2):
//            0x884a60 callers: 0 resolved; resolve_native_call_targets reports "not-call" (no static call
//            site at that address) -> status unknown (computed calls only).
//            0x884b70 / 0x8848e0 / 0x8849a0 callers: 0 resolved each -> unknown (computed calls).
//            So the pairs 0x884a60 <-> 0x884b70 / 0x8848e0 / 0x8849a0 are UNKNOWN (not confirmed same queue
//            or cross queue). Their waits still go through Satisfies, which is true only for a pending write.
//            0x92540 callers: 28 resolved; 0x884d40 callers: 14 resolved. Producer slot writes of 0x92540 vs the
//            0x884d40 consumer slots were not matched per slot (not done in this slice) -> cross queue UNKNOWN.
//   Function-table note: 0x86eec0 WAIT_REG_MEM dw1 0x105 = engine PFP (0x100) + function 5 = GreaterThanEqual
//            (the old comment said NotEqual; corrected).

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/platform.h"
#include "native/gnm/gnm_common.h"
#include "native/guest_bridge.h"
#include "native/native_labels.h"
#include "native/native_progress.h"
#include "native/native_switches.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#define W07_SYSV __attribute__((sysv_abi))

namespace {

// Guest body signatures (SysV), used by the fallback paths.
using NoArgFn = void (*W07_SYSV)(u64);
using EventFn = void (*W07_SYSV)(u64, u32);
using EopFn = void (*W07_SYSV)(u64, u32, u32, u64, s32, u64, u32, u32);
using WaitFn = void (*W07_SYSV)(u64, u64, u32, u32, u32);
using WaitPfpFn = void (*W07_SYSV)(u64, u64, u32, u32);
using AcquireFn = void (*W07_SYSV)(u64, u32, u32, u32, u32, u32, s32);
using Acquire2Fn = void (*W07_SYSV)(u64, u32, u32, s32);
using EosFn = void (*W07_SYSV)(u64, u32, u64, s32);
using LabelFn = u64 (*W07_SYSV)(u64, u64, s32);
// Round 3 guest bodies (raw-byte signatures, see the coverage table).
using RegWaitFn = void (*W07_SYSV)(u64, u32, u32, u32, u32);
using WaitGenFn = void (*W07_SYSV)(u64, u32, u32, u64, u32, u32, u16);
using LabelWaitFn = void (*W07_SYSV)(u64, u64);

// The env switches the handlers read (liverpool.cpp / sotc_stats.h). Both paths they gate need
// private Liverpool state, so the native op declines them at emit time.
bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}
const bool kEopDefer = EnvOn("SOTC_EOP_DEFER");
const bool kVisDirect = EnvOn("SOTC_VIS_DIRECT");
// SOTC_CP_STRICT: the handlers run their fence work with BackOrRun(..., back_strict).
const bool kBackStrict = EnvOn("SOTC_CP_STRICT");

// Guest-table offset of the per-event-type index table read by 0x86ced0 (DAT_012901d0).
constexpr u64 kEventIndexTable = 0x12901d0;
// Event types the handlers test by value (pm4_cmds.h AmdGpu::EventType).
constexpr u32 kEventSoVgtStreamoutFlush = 31;
constexpr u32 kEventPixelPipeStatDump = 57;
constexpr u32 kEventIndexZpassDone = 1;

bool On() {
    return Native::Gnm::Enabled(Native::Gnm::Class::Sync);
}

// The seven dwords a WAIT_REG_MEM packet is made of (header first), as the guest bodies write them.
using WaitDwords = std::array<u32, 7>;

// WAIT_REG_MEM words for memory or register space (space chosen by the address, as the guest does).
WaitDwords WaitRegMemWords(u32 engine_bit, u32 func, u64 addr, u32 ref, u32 mask, u32 poll) {
    const bool mem_space = addr > 0xffffu;
    const u64 addr_mask = mem_space ? 0xfffffffcull : 0xffffull;
    return {
        0xc0053c00u,
        ((engine_bit & 1u) << 8) | (mem_space ? 0x10u : 0u) | (func & 7u),
        static_cast<u32>(addr & addr_mask),
        static_cast<u32>(addr >> 32) & 0xffffu,
        ref,
        mask,
        poll,
    };
}

// The command processor's own test of the packet (PM4CmdWaitRegMem::Test, memory or register space).
// sotc: N2-94 W2. A memory-space wait is first checked against the virtual label table: a pending
// write to the awaited address that already satisfies the predicate passes the wait without a poll
// (the write is queued ahead on the single draw ring). Register-space waits are never virtual.
auto WaitPredicate(const WaitDwords& dw) {
    const bool mem_space = (dw[1] & 0x10u) != 0;
    return [dw, mem_space] {
        const auto wait = std::bit_cast<AmdGpu::PM4CmdWaitRegMem>(dw);
        if (wait.Test(Native::Gnm::Lv().NativeRegs().reg_array)) {
            return true;
        }
        // sotc: N2-112: never pass early (N2-110 flicker). A pending write that will satisfy the
        // wait only lets the command processor park it without re-polling until that write.
        if (mem_space && Native::Switch::VirtualLabels()) {
            const u64 addr = static_cast<u64>(dw[2]) | (static_cast<u64>(dw[3]) << 32);
            Native::Labels::Satisfies(addr, dw[5], dw[1] & 7u, dw[4]);
        }
        return false;
    };
}

} // namespace

static_assert(sizeof(AmdGpu::PM4CmdEventWriteEop) == 6 * sizeof(u32), "EVENT_WRITE_EOP packet size");
static_assert(sizeof(AmdGpu::PM4CmdWaitRegMem) == 7 * sizeof(u32), "WAIT_REG_MEM packet size");
static_assert(sizeof(AmdGpu::PM4CmdEventWriteEos) == 5 * sizeof(u32), "EVENT_WRITE_EOS packet size");

namespace Native::Gnm {

// sceGnmComputeWaitOnAddress (gnmdriver.cpp): a WAIT_REG_MEM on the compute ring in one 14-dword slot.
// The PM4 fallback writes that slot only for size == 0xe (it returns -1 otherwise, writing nothing), so
// any other size declines here and the fallback keeps its result.
bool ComputeWaitOnAddressNative(u32* cmdbuf, u32 size, uintptr_t addr, u32 mask, u32 cmp_func,
                                u32 ref) {
    static const u32 site = Native::Gnm::Site("w07:compute_wait_on_address");
    if (!On() || cmdbuf == nullptr || size != 0xe) {
        return false;
    }
    const WaitDwords dw = WaitRegMemWords(0, cmp_func, static_cast<u64>(addr), ref, mask, 10u);
    const bool ok = Native::StreamOps::EmitWaitInSlot(cmdbuf, size, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
    } else {
        Native::Stats::SiteFallback(site);
    }
    return ok;
}

void RegisterW07(Table& t) {
    // Table entry for sceGnmComputeWaitOnAddress. The other sync functions are not in gnmdriver.cpp's
    // W07 set, so they have no entries.
    t.ComputeWaitOnAddress = &ComputeWaitOnAddressNative;
}

} // namespace Native::Gnm

// 0x86d210(cb, size_bytes, log2_align) -> payload slot. Label allocation in a NOP-wrapped span.
extern "C" W07_SYSV u64 GnmW07_86d210(u64 cb, u64 size_bytes, s32 log2_align) {
    static const u32 site = Native::Gnm::Site("w07:86d210");
    if (On() && log2_align >= 2 && log2_align <= 20 && size_bytes <= 64) {
        const u32 body_dwords = static_cast<u32>((size_bytes + 3) >> 2);
        const u32 align = 1u << log2_align;
        const u64 slot = Native::StreamOps::EmitWithSpace(
            cb, [] {}, body_dwords, align, false, Native::StreamOps::Where::CommandProcessor);
        if (slot != 0) {
            Native::Stats::SiteNative(site);
            return slot;
        }
    }
    Native::Stats::SiteFallback(site);
    return Native::Gnm::Guest<LabelFn>(0x86d210)(cb, size_bytes, log2_align);
}

// 0x86eb60(cb, event_type, flags, addr, data_sel, data, f7, int_sel): EVENT_WRITE_EOP.
extern "C" W07_SYSV void GnmW07_86eb60(u64 cb, u32 event_type, u32 flags, u64 addr, s32 data_sel,
                                       u64 data, u32 f7, u32 int_sel) {
    static const u32 site = Native::Gnm::Site("w07:86eb60");
    if (!On() || kVisDirect || kEopDefer) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<EopFn>(0x86eb60)(cb, event_type, flags, addr, data_sel, data, f7, int_sel);
        return;
    }
    // Rebuild the packet the guest body writes (same masks as the decompile: the address mask
    // depends on data_sel == 1, and the high address bits are kept up to bit 39).
    const u64 mask = data_sel == 1 ? 0xfffffffffcULL : 0xfffffffff8ULL;
    const u64 masked = addr & mask;
    const std::array<u32, 6> dw = {
        0xc0044700u,
        ((int_sel & 3u) << 25) | 0x500u | ((flags & 0x10u) << 23) | (event_type & 0x3fu) |
            ((f7 & 0x3fu) << 12),
        static_cast<u32>(masked),
        ((flags & 1u) << 16) | (static_cast<u32>(data_sel) << 29) |
            static_cast<u32>(masked >> 32),
        static_cast<u32>(data),
        static_cast<u32>(data >> 32),
    };
    // sotc: N2-94 W2. Only the value-writing data selects are virtual labels (1 = 32-bit, 2 = 64-bit);
    // None (interrupt only) and the clock / perf-counter selects have no value known on the CP.
    const bool vl_write = Native::Switch::VirtualLabels() && (data_sel == 1 || data_sel == 2);
    const u32 vl_bytes = data_sel == 2 ? 8u : 4u;
    const u64 vl_value = data_sel == 2 ? data : static_cast<u64>(static_cast<u32>(data));
    const bool ok = Native::Gnm::EmitCp(
        cb,
        [dw, masked, vl_write, vl_bytes, vl_value] {
            const auto eop = std::bit_cast<AmdGpu::PM4CmdEventWriteEop>(dw);
            AmdGpu::Liverpool& lv = Native::Gnm::Lv();
            // sotc: N2-94 W2. The write is queued to the draw ring now (this op runs on the CP).
            const u64 token = vl_write ? Native::Labels::Record(masked, vl_value, vl_bytes) : 0;
            // liverpool.cpp EventWriteEop handler: BackOrRun(..., back_strict) with OnFence and the
            // fence write / GfxEop interrupt (the non-deferred branch).
            lv.NativeBack(
                [eop, &lv, token] {
                    if (auto* rasterizer = lv.NativeRasterizer()) {
                        rasterizer->OnFence();
                    }
                    eop.SignalFence(
                        [&lv](void* address, u64 value, u32 num_bytes) {
                            lv.NativeWriteFenceValue(address, value, num_bytes);
                        },
                        [] {
                            Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                        });
                    // sotc: N2-94 W2. The real write is done: retire the pending entry, wake the CP.
                    if (token != 0) {
                        Native::Labels::Retire(token);
                        Native::Progress::Notify();
                    }
                },
                kBackStrict);
        },
        true);
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<EopFn>(0x86eb60)(cb, event_type, flags, addr, data_sel, data, f7, int_sel);
}

// 0x86edc0(cb, addr, mask, func, ref): WAIT_REG_MEM, memory or register space, engine ME.
extern "C" W07_SYSV void GnmW07_86edc0(u64 cb, u64 addr, u32 mask, u32 func, u32 ref) {
    static const u32 site = Native::Gnm::Site("w07:86edc0");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<WaitFn>(0x86edc0)(cb, addr, mask, func, ref);
        return;
    }
    const bool mem_space = addr > 0xffffu;
    const u64 addr_mask = mem_space ? 0xfffffffcull : 0xffffull;
    const std::array<u32, 7> dw = {
        0xc0053c00u,
        (mem_space ? 0x10u : 0u) | (func & 7u),
        static_cast<u32>(addr & addr_mask),
        static_cast<u32>(addr >> 32) & 0xffffu,
        ref,
        mask,
        10u,
    };
    // A wait the command processor polls and YIELDS on (StreamOps::EmitWait, gap N2-76), as the
    // WaitRegMem handler does; spinning inside an ordinary op froze the whole command processor.
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<WaitFn>(0x86edc0)(cb, addr, mask, func, ref);
}

// 0x86eec0(cb, addr, mask, ref): WAIT_REG_MEM, function GreaterThanEqual (5), PFP engine (0x105).
extern "C" W07_SYSV void GnmW07_86eec0(u64 cb, u64 addr, u32 mask, u32 ref) {
    static const u32 site = Native::Gnm::Site("w07:86eec0");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<WaitPfpFn>(0x86eec0)(cb, addr, mask, ref);
        return;
    }
    const bool mem_space = addr > 0xffffu;
    const u64 addr_mask = mem_space ? 0xfffffffcull : 0xffffull;
    const std::array<u32, 7> dw = {
        0xc0053c00u,
        (mem_space ? 0x10u : 0u) | 0x105u,
        static_cast<u32>(addr & addr_mask),
        static_cast<u32>(addr >> 32) & 0xffffu,
        ref,
        mask,
        10u,
    };
    // A wait the command processor polls and YIELDS on (StreamOps::EmitWait, gap N2-76), as the
    // WaitRegMem handler does; spinning inside an ordinary op froze the whole command processor.
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<WaitPfpFn>(0x86eec0)(cb, addr, mask, ref);
}

// 0x86ced0(cb, event_type): EVENT_WRITE. The event index comes from the guest's own table.
extern "C" W07_SYSV void GnmW07_86ced0(u64 cb, u32 event_type) {
    static const u32 site = Native::Gnm::Site("w07:86ced0");
    if (!On() || Native::Guest::GuestImageBase() == 0) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<EventFn>(0x86ced0)(cb, event_type);
        return;
    }
    const u32 index = static_cast<u32>(Native::Guest::ImageQword(kEventIndexTable + 4ull * event_type));
    const u32 ev = event_type & 0x3fu;
    // PixelPipeStatDump + ZpassDone writes occlusion counters (private state): guest body.
    if (ev == kEventPixelPipeStatDump && (index & 0xfu) == kEventIndexZpassDone) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<EventFn>(0x86ced0)(cb, event_type);
        return;
    }
    const bool ok = Native::Gnm::EmitCp(cb, [ev] {
        if (ev != kEventSoVgtStreamoutFlush) {
            return; // every other event is a no-op in the command processor
        }
        AmdGpu::Liverpool& lv = Native::Gnm::Lv();
        auto& regs = lv.NativeRegs();
        regs.cp_strmout_cntl.offset_update_done = 1;
        lv.NativeMarkField(&regs.cp_strmout_cntl, sizeof(regs.cp_strmout_cntl));
    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<EventFn>(0x86ced0)(cb, event_type);
}

// 0x86f1d0(cb, p2, p3, p4): ACQUIRE_MEM, plus an optional EVENT_WRITE 0x72a. Both are no-ops in the
// command processor (liverpool.cpp ProcessGraphics AcquireMem, and 0x2a is not a handled event).
extern "C" W07_SYSV void GnmW07_86f1d0(u64 cb, u32 p2, u32 p3, s32 p4) {
    static const u32 site = Native::Gnm::Site("w07:86f1d0");
    if (On() && Native::Gnm::EmitCp(cb, [] {})) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Acquire2Fn>(0x86f1d0)(cb, p2, p3, p4);
}

// 0x86cf40(cb, p2, p3, p4, p5, p6, p7): ACQUIRE_MEM, a no-op in the command processor.
extern "C" W07_SYSV void GnmW07_86cf40(u64 cb, u32 p2, u32 p3, u32 p4, u32 p5, u32 p6, s32 p7) {
    static const u32 site = Native::Gnm::Site("w07:86cf40");
    if (On() && Native::Gnm::EmitCp(cb, [] {})) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<AcquireFn>(0x86cf40)(cb, p2, p3, p4, p5, p6, p7);
}

// 0x86ee70(cb): PFP_SYNC_ME, a no-op in the command processor.
extern "C" W07_SYSV void GnmW07_86ee70(u64 cb) {
    static const u32 site = Native::Gnm::Site("w07:86ee70");
    if (On() && Native::Gnm::EmitCp(cb, [] {})) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<NoArgFn>(0x86ee70)(cb);
}

// 0x86f5e0(cb, event_type, addr, value): EVENT_WRITE_EOS with the SignalFence command.
extern "C" W07_SYSV void GnmW07_86f5e0(u64 cb, u32 event_type, u64 addr, s32 value) {
    static const u32 site = Native::Gnm::Site("w07:86f5e0");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<EosFn>(0x86f5e0)(cb, event_type, addr, value);
        return;
    }
    const u32 header = event_type != 0x30u ? 0xc0034802u : 0xc0034800u;
    const std::array<u32, 5> dw = {
        header,
        (event_type & 0x3fu) | 0x600u,
        static_cast<u32>(addr) & 0xfffffffcu,
        (static_cast<u32>(addr >> 32) & 0xffu) | 0x40000000u,
        static_cast<u32>(value),
    };
    // sotc: N2-94 W2. The EOS packet always carries command SignalFence (cmd_info bits 29-31 = 2),
    // a 4-byte write at address_lo | address_hi << 32 (the same masks the packet stores).
    const u64 eos_addr = static_cast<u64>(static_cast<u32>(addr) & 0xfffffffcu) |
                         (static_cast<u64>((addr >> 32) & 0xffu) << 32);
    const bool vl_write = Native::Switch::VirtualLabels();
    const u32 eos_value = static_cast<u32>(value);
    const bool ok = Native::Gnm::EmitCp(
        cb,
        [dw, eos_addr, vl_write, eos_value] {
            const auto eos = std::bit_cast<AmdGpu::PM4CmdEventWriteEos>(dw);
            AmdGpu::Liverpool& lv = Native::Gnm::Lv();
            // sotc: N2-94 W2. The write is queued to the draw ring now (this op runs on the CP).
            const u64 token = vl_write ? Native::Labels::Record(eos_addr, eos_value, 4) : 0;
            // liverpool.cpp EventWriteEos handler: BackOrRun(..., back_strict) with OnFence and the
            // fence write (command SignalFence; the GDS store command is not emitted here).
            lv.NativeBack(
                [eos, &lv, token] {
                    if (auto* rasterizer = lv.NativeRasterizer()) {
                        rasterizer->OnFence();
                    }
                    eos.SignalFence([&lv](void* address, u64 value, u32 num_bytes) {
                        lv.NativeWriteFenceValue(address, value, num_bytes);
                    });
                    // sotc: N2-94 W2. The real write is done: retire the pending entry, wake the CP.
                    if (token != 0) {
                        Native::Labels::Retire(token);
                        Native::Progress::Notify();
                    }
                },
                kBackStrict);
        },
        true);
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<EosFn>(0x86f5e0)(cb, event_type, addr, value);
}

// 0x883f80(cb, size_bytes, log2_align) -> payload slot: the label allocator of 0x86d210 on the other
// queue. Raw bytes (eboot file offset VA + 0x4000): the prologue reads rdi = cb, rsi = size (64-bit,
// add 3 / shr 2), edx = log2 align (32-bit); the body is the same as 0x86d210's, so the same
// EmitWithSpace span is used. The NOP the guest writes (payload 0x68750000) has no CP handler.
extern "C" W07_SYSV u64 GnmW07_883f80(u64 cb, u64 size_bytes, s32 log2_align) {
    static const u32 site = Native::Gnm::Site("w07:883f80");
    if (On() && log2_align >= 2 && log2_align <= 20 && size_bytes <= 64) {
        const u32 body_dwords = static_cast<u32>((size_bytes + 3) >> 2);
        const u32 align = 1u << log2_align;
        const u64 slot = Native::StreamOps::EmitWithSpace(
            cb, [] {}, body_dwords, align, false, Native::StreamOps::Where::CommandProcessor);
        if (slot != 0) {
            Native::Stats::SiteNative(site);
            return slot;
        }
    }
    Native::Stats::SiteFallback(site);
    return Native::Gnm::Guest<LabelFn>(0x883f80)(cb, size_bytes, log2_align);
}

// 0x86f4e0(cb): INCREMENT_DE_COUNTER. Raw bytes: `mov ecx, 0xc0008500` stored at [rax], then the
// cursor advances by 8 (2 dwords); opcode 0x85. liverpool.cpp ProcessGraphics IncrementDeCounter:
// ++cblock.de_count, the same op run inline on the command processor in stream order.
extern "C" W07_SYSV void GnmW07_86f4e0(u64 cb) {
    static const u32 site = Native::Gnm::Site("w07:86f4e0");
    const bool ok = On() && Native::Gnm::EmitCp(cb, [] {
        ++Native::Gnm::Lv().NativeCblock().de_count;
    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<NoArgFn>(0x86f4e0)(cb);
}

// 0x86f3f0(cb, p2, p3): a NOP with payload 0x68750012 (4 dwords, or 6 when p3 != 0). Raw bytes: the
// header is (count << 16) + 0x3ffe0000 | 0xc0001000 with count = 4 + (p3 != 0) + (p3 != 0) and the
// payload 0x68750012 at [rax + 4]. The command processor has no handler for that payload
// (liverpool.cpp NOP switch: default break) and nothing in the port reads these words, so the
// native path writes nothing. The guest's own refill request is also skipped; the guest writers
// check the space themselves, so no later packet depends on it.
extern "C" W07_SYSV void GnmW07_86f3f0(u64 cb, u64 p2, u64 p3) {
    static const u32 site = Native::Gnm::Site("w07:86f3f0");
    if (On()) {
        (void)cb;
        (void)p2;
        (void)p3;
        Native::Stats::SiteNative(site);
        return;
    }
    using Nop3Fn = void (*W07_SYSV)(u64, u64, u64);
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Nop3Fn>(0x86f3f0)(cb, p2, p3);
}

// 0x86cff0(cb, addr, mask, func, ref): WAIT_REG_MEM on a register, ME engine. Raw bytes: esi = addr
// (stored as a 16-bit register offset), edx = mask, ecx = func, r8d = ref. No static callers.
extern "C" W07_SYSV void GnmW07_86cff0(u64 cb, u32 addr, u32 mask, u32 func, u32 ref) {
    static const u32 site = Native::Gnm::Site("w07:86cff0");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<RegWaitFn>(0x86cff0)(cb, addr, mask, func, ref);
        return;
    }
    // Register space is forced (no mem_space bit) and the offset is 16 bits, as the guest writes it.
    const WaitDwords dw = {0xc0053c00u, func & 7u, addr & 0xffffu, 0u, ref, mask, 10u};
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<RegWaitFn>(0x86cff0)(cb, addr, mask, func, ref);
}

// 0x86e9e0 and 0x884b00 (byte-identical bodies): WAIT_REG_MEM with the engine, function and poll
// interval given by the caller. Raw bytes: esi = func, edx = engine (bit 0), rcx = addr (64-bit),
// r8d = ref, r9d = mask, the 7th argument (poll) is a 16-bit word on the stack.
extern "C" W07_SYSV void GnmW07_86e9e0(u64 cb, u32 func, u32 engine, u64 addr, u32 ref, u32 mask,
                                       u16 poll) {
    static const u32 site = Native::Gnm::Site("w07:86e9e0");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<WaitGenFn>(0x86e9e0)(cb, func, engine, addr, ref, mask, poll);
        return;
    }
    const WaitDwords dw = WaitRegMemWords(engine, func, addr, ref, mask, poll);
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<WaitGenFn>(0x86e9e0)(cb, func, engine, addr, ref, mask, poll);
}

extern "C" W07_SYSV void GnmW07_884b00(u64 cb, u32 func, u32 engine, u64 addr, u32 ref, u32 mask,
                                       u16 poll) {
    static const u32 site = Native::Gnm::Site("w07:884b00");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<WaitGenFn>(0x884b00)(cb, func, engine, addr, ref, mask, poll);
        return;
    }
    const WaitDwords dw = WaitRegMemWords(engine, func, addr, ref, mask, poll);
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<WaitGenFn>(0x884b00)(cb, func, engine, addr, ref, mask, poll);
}

// 0x884a60(cb, p2): WAIT_REG_MEM on the dword at p2 - 4 until its bit 31 is set (function 3 = Equal,
// reference and mask 0x80000000, ME, poll 10). Raw bytes: rsi = p2 (64-bit), `add rbx, -4`.
extern "C" W07_SYSV void GnmW07_884a60(u64 cb, u64 p2) {
    static const u32 site = Native::Gnm::Site("w07:884a60");
    if (!On()) {
        Native::Stats::SiteFallback(site);
        Native::Gnm::Guest<LabelWaitFn>(0x884a60)(cb, p2);
        return;
    }
    const WaitDwords dw = WaitRegMemWords(0, 3u, p2 - 4, 0x80000000u, 0x80000000u, 10u);
    const bool ok = Native::StreamOps::EmitWait(cb, WaitPredicate(dw));
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<LabelWaitFn>(0x884a60)(cb, p2);
}
