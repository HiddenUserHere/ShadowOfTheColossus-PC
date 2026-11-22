// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W12: Submit / flip.
// Class Submit (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w12_submit.h, and
// gpu/sotc/hook_gnm_w12.inc. See gnm_common.h for the contract.
//
// Scope of this worker. The four sceGnm* entries of this class are host code (gnmdriver.cpp), not
// eboot emitters, so they have no PM4 emitter to replace; they only hand the command buffers to
// the command processor queue (liverpool->SubmitGfx) or wait for the GPU. Nothing here can make a
// submission native without reordering it against other queue work, because the queue is consumed
// by ProcessGraphics on the command processor thread and the facade (liverpool.h) has no
// "run this queued op later in queue order" entry point. So the entries below measure instead of
// replacing: they scan each submitted buffer and count it as all-native or not (readiness metric),
// and always return false so the PM4 path runs unchanged.
//
// Coverage table (site | contract | native op | evidence):
//   sceGnmSubmitCommandBuffersForWorkload  | gnmdriver.cpp:2462-2504: validate sizes, WaitGpuIdle,
//     PerformSubmit -> liverpool->SubmitGfx(dcb, ccb) queues the buffers for ProcessGraphics |
//     none (queue-order constraint above); counter "w12:all-native-cb" + site fallback |
//     evidence: gnmdriver.cpp read y; raw bytes n/a (host code); REA n/a.
//   sceGnmSubmitAndFlipCommandBuffersForWorkload | gnmdriver.cpp:2402-2460: as above plus
//     PatchFlipRequest on the last DCB (sceVideoOutSubmitEopFlip, rewrites the 64-dword
//     prepareFlip NOP into PatchedFlip) | none; same counter (scan runs before the patch; the
//     patched dwords are non-native either way) | evidence: gnmdriver.cpp:2234-2316, 2402-2460 read y.
//   sceGnmInsertWaitFlipDone | gnmdriver.cpp:1248-1272: writes one WaitRegMem (type 3, opcode 0x3c,
//     count 5, raw 0x13 = engine ME, function ==, memory space; ref 0, mask all, poll_interval 10)
//     polling the VO buffer label | round 4: native wait (InsertWaitFlipDoneW12, N2-86). Round 1-3 note: the wait must yield the command processor
//     coroutine (ProcessGraphics WaitRegMem, liverpool.cpp:2062-2075, uses vo_port->WaitVoLabel and
//     YIELD_WAIT_ASC); a native op runs inline and the facade has no wait primitive. Site fallback
//     only | evidence: gnmdriver.cpp:1263-1270 read y; raw bytes n/a (host code); REA n/a.
//   sceGnmSubmitDone | gnmdriver.cpp:2513-2530: WaitGpuIdle, submission_lock, liverpool->SubmitDone,
//     send_init_packet = true | no PM4 is emitted, so there is nothing to replace; site fallback |
//     evidence: gnmdriver.cpp:2513-2530 read y.
//   Eboot flip emitters 0x86f760 / 0x86f7c0 / 0x86f830 / 0x86f8a0 (payloads 0x68750777 PrepareFlip,
//     0x68750778 PrepareFlipLabel, 0x68750780 PrepareFlipInterrupt, 0x68750781
//     PrepareFlipInterruptLabel; each writes a 64-dword tail: header 0xc03e1000, payload, fields) |
//     the tail is native since round 4 (FlipTailW12, N2-85); round 2 note (R9): the tail is consumed at submit time by gnmdriver
//     PatchFlipRequest (gnmdriver.cpp:2238-2320, host code, not owned by W12). It asserts
//     cmdbuf[size-64] == 0xc03e1000, reads backup[1..6] (payload type, label address, label data,
//     EOP control, interrupt params), calls sceVideoOutSubmitEopFlip (host, at submit), then
//     rewrites the tail into a WRITE_DATA lock, a label/EOP write and a PatchedFlip NOP. The CP
//     PatchedFlip handler (liverpool.cpp:1416-1423) only signals IrqC GfxFlip. A native op
//     emitted in the tail would break the assert; a native op elsewhere would lose the tail fields.
//     Needs core_requests R9-1 and R9-2 below. Evidence: decomp out/re/decomp/0086/0086f760.c,
//     0086f7c0.c, 0086f830.c, 0086f8a0.c read y; raw bytes y (file offset = VA + 0x4000, prologue
//     55 48 89 e5 at each VA; imm32 0xc03e1000 and per-emitter payload found in out/eboot.elf); REA
//     unavailable (no app open).
//   Eboot 0x68750013 placeholder NOP: 0x8850c0 (writes 3 fields: segment address, size, flags) and
//     0x885110 (predicate: is this a 5-dword 0xc0031000 NOP with payload 0x68750013) | NOT
//     replaced: these are submit-time helpers of the eboot submit wrappers (0x863310, 0x8636e0,
//     0x863af0), not PM4 emitters. They write guest dwords only and the wrapper skips a 0x14-byte
//     placeholder segment. The CP has no handler for 0x68750013 (default NOP case), and no host
//     code reads it (rg over gpu/ finds no reader). Nothing to reproduce natively. Evidence:
//     decomp 0088/008850c0.c, 0088/00885110.c, 0086/00863310.c (call sites); raw bytes y (header
//     0xc0031000 + payload 0x68750013 at both VAs).
//   Eboot padding NOP writer 0x885220 (callers 0x85c9b0, 0x85d690, 0x85d6f0, 0x85cb40, 0x85d260):
//     writes a zero-payload NOP of N dwords (header 0xc0001000 | (N-2) << 16) and advances the
//     cursor by N; N == 1 writes the single dword 0xffff1000 (not a flip marker). | NOT replaced: pure
//     padding with no effect to reproduce. Replacing it would change the cursor math used by the
//     callers and the submit wrappers' size computation, so the facade offers no safe equivalent.
//     Evidence: decomp 0088/00885220.c read y; raw bytes header 0xc0001000 at VA y.
//
// Readiness (w12:all-native-cb): the counter can read SiteNative only for a DCB/CCB whose every
// packet is a SotcNativeOp NOP or type-2 padding. Every frame's last DCB carries one of the
// 64-dword flip tails above (payload 0x6875077x, not SotcNativeOp), so that DCB always counts as
// SiteFallback. Frames with a flip therefore cannot reach 100 percent native while the flip
// emitters remain PM4 NOPs, and the counter is an honest signal of that, not a bug. The 0x68750013
// placeholder NOP also counts as fallback when it is inside a submitted buffer.
//
// core_requests (R9):
//   R9-1 (gnmdriver, host): a flip path that does not patch the buffer tail. SubmitAndFlip would
//     call a Native::Gnm table entry (or a facade entry) that takes vo_handle, buf_idx, flip_mode,
//     flip_arg, plus the label address, label data, EOP control and interrupt fields read from the
//     native marker op, and performs sceVideoOutSubmitEopFlip and the lock/label/EOP writes
//     itself, instead of PatchFlipRequest's in-buffer rewrite.
//   R9-2 (liverpool facade): a native flip-marker op for the CP (Where::CommandProcessor) that
//     signals IrqC GfxFlip (the PatchedFlip handler), and a native EOP write (interrupt variant)
//     equivalent to EventWriteEop with data_control 0x2200_0000 / 0x100_0000. NativeWriteFenceValue
//     (liverpool.h:458) may cover the label write; the EOP interrupt variant has no facade entry.
//

// Counter semantics: "w12:all-native-cb" counts each submitted DCB (paired with its CCB) as
// SiteNative when every packet is a SotcNativeOp NOP or type-2 padding (would run without the
// command processor parsing it) and as SiteFallback otherwise. The buffer is still executed by the
// PM4 path; the counter only measures readiness.

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <span>

#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/platform.h"
#include "native/gnm/gnm_common.h"
#include "native/native_labels.h"
#include "native/native_oplist.h"
#include "native/native_progress.h"
#include "native/native_switches.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Native::Gnm {

namespace {

// PM4ItOpcode::Nop (type-3 header bits 8-15).
constexpr u32 kOpcodeNop = 0x10u;
// gnmdriver.cpp: dcbSizesInBytes / ccbSizesInBytes are limited to 0x3ffffc bytes.
constexpr u32 kMaxBufferBytes = 0x3ffffcu;

/// Pre-W3 scan (default path, every native switch off). True when every packet of the buffer is a
/// SotcNativeOp NOP (type 3, opcode Nop, count >= 1, data_block[0] == payload) or a type-2 padding
/// dword. Mirrors the parser of ProcessGraphics (liverpool.cpp): type 3 spans NumWords() + 1 dwords
/// with NumWords = (count + 1) & 0x3fff; any other packet type, opcode or a truncated packet ends the
/// scan with false. Kept byte-for-byte so default behaviour does not change (N2-94 rule 2).
bool IsAllNativeBufferScan(const u32* dw, u32 dwords) {
    u32 i = 0;
    while (i < dwords) {
        const u32 header = dw[i];
        const u32 type = header >> 30;
        if (type == 2 || header == 0xffff1000u) { // type-2 filler / one-dword NOP (count 0x3fff)
            ++i;
            continue;
        }
        if (type != 3) {
            return false;
        }
        const u32 body = (((header >> 16) & 0x3fffu) + 1) & 0x3fffu;
        if (body == 0 || body + 1 > dwords - i) {
            return false;
        }
        // Round 4 (N2-89): every NOP counts. The command processor acts only on the SotcNative*
        // payloads (native ops), PatchedFlip (now the FlipTail native op; the scan runs before the
        // patch, on the prepareFlip marker it replaces) and the debug-marker payloads (debug UI
        // only); every other payload (label slots, 0x68750013 placeholders, padding) is skipped.
        // Any non-NOP packet is PM4 work and makes the buffer non-native.
        const u32 opcode = (header >> 8) & 0xffu;
        if (opcode != kOpcodeNop) {
            return false;
        }
        i += body + 1;
    }
    return true;
}

/// True when the buffer is all-native for the counter. Default (no NativeCp / VirtualLabels switch):
/// the pre-W3 scan above, unchanged. With a switch on: the op list builder (native_oplist.h
/// BuildOpList), which uses the same NOP rules the command processor applies (only SotcNative*
/// payloads are ops; every other NOP payload is skipped). The op list is built into a thread-local
/// scratch (no heap allocation per submit after the first).
///
/// sotc: H04 (N2-94 plan, W3). Behaviour change, gated: with the switch on, a SotcNative* payload
/// with too few dwords (Op < 2, Wait/WaitCe < 2, Ib < 4) makes the buffer non-native, and a NOP with
/// count 0x3fff is non-native (BuildOpList rejects it, as the PM4 parser spans one dword for it).
bool IsAllNativeBuffer(const u32* dw, u32 dwords) {
    if (dw == nullptr || dwords == 0) {
        return false;
    }
    if (!Native::Switch::NativeCp() && !Native::Switch::VirtualLabels()) {
        return IsAllNativeBufferScan(dw, dwords);
    }
    thread_local Native::OpList list;
    if (!Native::BuildOpList(std::span<const u32>(dw, dwords), list)) {
        return false;
    }
    Native::CountOpList(list);
    return true;
}

/// The argument checks gnmdriver.cpp applies before it submits. A submission it rejects is not
/// counted (the driver returns its error through the PM4 path).
bool ValidSubmission(u32 count, const u32* const* dcb_addrs, const u32* dcb_sizes,
                     const u32* const* ccb_addrs, const u32* ccb_sizes) {
    if (count == 0 || dcb_addrs == nullptr || dcb_sizes == nullptr) {
        return false;
    }
    if (ccb_sizes != nullptr && ccb_addrs == nullptr) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        if (dcb_sizes[i] == 0 || dcb_sizes[i] > kMaxBufferBytes) {
            return false;
        }
        if (ccb_sizes != nullptr && ccb_sizes[i] > kMaxBufferBytes) {
            return false;
        }
    }
    return true;
}

/// Counts every DCB of a valid submission (with its CCB, when present) as all-native or not.
void CountSubmission(u32 count, const u32* const* dcb_addrs, const u32* dcb_sizes,
                     const u32* const* ccb_addrs, const u32* ccb_sizes) {
    static const u32 site = Site("w12:all-native-cb");
    for (u32 i = 0; i < count; ++i) {
        const bool dcb_native = IsAllNativeBuffer(dcb_addrs[i], dcb_sizes[i] >> 2);
        bool ccb_native = true;
        if (ccb_sizes != nullptr && ccb_sizes[i] != 0) {
            ccb_native = IsAllNativeBuffer(ccb_addrs != nullptr ? ccb_addrs[i] : nullptr,
                                           ccb_sizes[i] >> 2);
        }
        if (dcb_native && ccb_native) {
            ::Native::Stats::SiteNative(site);
        } else {
            ::Native::Stats::SiteFallback(site);
        }
    }
}

// sceGnmInsertWaitFlipDone (round 4, N2-86): the WAIT_REG_MEM (memory, function ==, ref 0, mask all)
// on the VO buffer label becomes a native wait in the same 7-dword slot. The command processor polls
// the predicate and yields between polls (StreamOps::EmitWaitInSlot), as the PM4 handler's generic
// loop does; the handler's VO-label sleep shortcut is not reproduced (it only saves CPU).
//
// sotc: H04 (N2-94 plan, W1). The predicate is unchanged. Wake-up: the VO buffer label is written by
// the video-out driver (SignalVoLabel in videoout/driver.h), which calls Progress::Notify (Step 0),
// so a command processor that sleeps on the progress epoch wakes when the flip completes. No change
// is needed here.
bool InsertWaitFlipDoneW12(u32* cmdbuf, u32 size, s32 vo_handle, u32 buf_idx) {
    static const u32 site = Site("w12:sceGnmInsertWaitFlipDone");
    if (!Enabled(Class::Submit) || cmdbuf == nullptr || size != 7) {
        ::Native::Stats::SiteFallback(site);
        return false;
    }
    uintptr_t label_addr{};
    if (Libraries::VideoOut::sceVideoOutGetBufferLabelAddress(vo_handle, &label_addr) != 16) {
        ::Native::Stats::SiteFallback(site);
        return false;
    }
    const uintptr_t addr = (label_addr + buf_idx * sizeof(uintptr_t)) & ~uintptr_t{3};
    if (!::Native::StreamOps::EmitWaitInSlot(cmdbuf, size, [addr]() {
            return *reinterpret_cast<const volatile u32*>(addr) == 0u;
        })) {
        ::Native::Stats::SiteFallback(site);
        return false;
    }
    ::Native::Stats::SiteNative(site);
    return true;
}

bool SubmitCommandBuffersForWorkloadW12(u32 workload, u32 count, const u32* dcb_gpu_addrs[],
                                        u32* dcb_sizes_in_bytes, const u32* ccb_gpu_addrs[],
                                        u32* ccb_sizes_in_bytes, s32* result) {
    static const u32 site = Site("w12:sceGnmSubmitCommandBuffersForWorkload");
    (void)workload;
    (void)result;
    if (Enabled(Class::Submit) && ValidSubmission(count, dcb_gpu_addrs, dcb_sizes_in_bytes,
                                                  ccb_gpu_addrs, ccb_sizes_in_bytes)) {
        CountSubmission(count, dcb_gpu_addrs, dcb_sizes_in_bytes, ccb_gpu_addrs,
                        ccb_sizes_in_bytes);
    }
    ::Native::Stats::SiteFallback(site);
    return false;
}

bool SubmitAndFlipCommandBuffersForWorkloadW12(u32 workload, u32 count, u32* dcb_gpu_addrs[],
                                               u32* dcb_sizes_in_bytes, u32* ccb_gpu_addrs[],
                                               u32* ccb_sizes_in_bytes, u32 vo_handle,
                                               u32 buf_idx, u32 flip_mode, s64 flip_arg,
                                               s32* result) {
    static const u32 site = Site("w12:sceGnmSubmitAndFlipCommandBuffersForWorkload");
    (void)workload;
    (void)vo_handle;
    (void)buf_idx;
    (void)flip_mode;
    (void)flip_arg;
    (void)result;
    if (Enabled(Class::Submit) && ValidSubmission(count, dcb_gpu_addrs, dcb_sizes_in_bytes,
                                                  ccb_gpu_addrs, ccb_sizes_in_bytes)) {
        CountSubmission(count, dcb_gpu_addrs, dcb_sizes_in_bytes, ccb_gpu_addrs,
                        ccb_sizes_in_bytes);
    }
    ::Native::Stats::SiteFallback(site);
    return false;
}

// Round 4 (N2-85, core request R9-1/R9-2): the flip tail. PatchFlipRequest (gnmdriver.cpp) used to
// rewrite the 64-dword prepareFlip NOP into WRITE_DATA (VO surface lock = 1), a PatchedFlip NOP
// (IrqC GfxFlip) and, for PrepareFlipLabel, a WRITE_DATA of the guest label. Here the whole tail
// becomes one native op on the command processor that does the same three steps in the same order
// on the draw thread (the CP handlers' BackOrRun): OnFence + memcpy + NoteHostWrite for each
// write, the GfxFlip signal between them. The interrupt variants (EOP) stay on the PM4 path.
//
// sotc: H04 (N2-94 plan, W2). With SOTC_NATIVE_VLABELS the write is recorded on the command
// processor thread (Labels::Record, when the op runs) and retired on the draw thread right after the
// real write; every real write then wakes the command processor (Progress::Notify), so a wait on
// this address that sleeps on the progress epoch sees it. Token 0 (switch off / excluded) is a no-op.
//
// Wake-up is gated on SOTC_CP_EVENTS (fix round 1): only a command processor sleeping on the progress
// epoch needs the notify, and with the switch off nothing sleeps on it, so the default path keeps its
// old cost (no atomic per flip-tail write).
namespace {
void NotifyProgressIfEvents() {
    if (Native::Switch::CpEvents()) {
        Native::Progress::Notify();
    }
}
} // namespace

void WriteDwordBack(AmdGpu::Liverpool& lv, VAddr addr, u32 value) {
    Vulkan::Rasterizer* rast = lv.NativeRasterizer();
    const u64 token = Native::Switch::VirtualLabels()
                          ? Native::Labels::Record(static_cast<u64>(addr), value, 4)
                          : 0;
    lv.NativeBack(
        [rast, addr, value, token]() {
            if (rast) {
                rast->OnFence();
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &value, 4);
            Core::MemoryManager::NoteHostWrite(addr, 4, 4);
            Native::Labels::Retire(token);
            NotifyProgressIfEvents();
        },
        true);
}

// The env switches the CP EOP handler reads; both need private Liverpool state, so the interrupt
// variants decline them (as W07's 0x86eb60 does).
bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}

// sotc: H04 (N2-94 plan, W2/W1) coverage for the flip tail:
//   item | done? | evidence
//   lock write (WriteDwordBack) recorded on CP, retired on draw thread | yes | WriteDwordBack
//   label write (WriteDwordBack, PrepareFlipLabel) recorded / retired  | yes | WriteDwordBack
//   Progress::Notify after each real WriteDwordBack write (CpEvents)   | yes | NotifyProgressIfEvents
//   EOP interrupt variant label (NativeWriteFenceValue) recorded       | no  | not recorded:
//     the write happens inside the EOP SignalFence callback; its waits keep polling real memory
//     (safe, the table only ever holds writes that are queued).
//   Progress::Notify after the EOP SignalFence write (CpEvents)        | yes | fix round 1,
//     FlipTailW12 interrupt lambda (lost wake-up otherwise)
//   Default path unchanged (all switches off)                          | yes | IsAllNativeBufferScan,
//     Notify gated on CpEvents (no atomic when off)
//   Scan verdicts with NativeCp / VirtualLabels on, count 0x3fff NOP   | yes | BuildOpList rejects it
//   Flip markers 0x68750777/778/780/781 at tail dword 1                | yes | REA + decomp, see
//     out/re/decomp/0086/0086f760.c, 0086f7c0.c, 0086f830.c, 0086f8a0.c, raw bytes at VA + 0x4000
bool FlipTailW12(u32* tail, u32 tail_dwords, u64 lock_addr, const u32* backup) {
    static const u32 site = Site("w12:FlipTail");
    static const bool eop_private = EnvOn("SOTC_EOP_DEFER") || EnvOn("SOTC_VIS_DIRECT");
    static const bool back_strict = EnvOn("SOTC_CP_STRICT");
    constexpr u32 kPrepareFlip = 0x68750777u;
    constexpr u32 kPrepareFlipLabel = 0x68750778u;
    constexpr u32 kPrepareFlipInterrupt = 0x68750780u;
    constexpr u32 kPrepareFlipInterruptLabel = 0x68750781u;
    if (!Enabled(Class::Submit) || tail == nullptr || backup == nullptr || tail_dwords < 3) {
        return false;
    }
    const u32 payload = backup[1];
    const bool label = payload == kPrepareFlipLabel;
    const bool interrupt = payload == kPrepareFlipInterrupt || payload == kPrepareFlipInterruptLabel;
    if ((payload != kPrepareFlip && !label && !interrupt) || (interrupt && eop_private)) {
        ::Native::Stats::SiteFallback(site);
        return false;
    }
    const VAddr lock = lock_addr & ~VAddr{3};
    const VAddr label_addr =
        (static_cast<VAddr>(backup[3]) << 32) | static_cast<VAddr>(backup[2] & 0xfffffffcu);
    const u32 label_value = backup[4];
    // The EVENT_WRITE_EOP PatchFlipRequest writes at tail + 0x3a for the interrupt variants
    // (gnmdriver.cpp): with label (address lo only, data_control | 0x2200'0000) or interrupt only.
    std::array<u32, 6> eop_dw{};
    if (interrupt) {
        const bool with_label = payload == kPrepareFlipInterruptLabel;
        eop_dw[0] = 0xc0044700u;
        eop_dw[1] = (backup[5] & 0x3fu) + 0x500u + (backup[6] & 0x3fu) * 0x1000u;
        eop_dw[2] = with_label ? (backup[2] & 0xfffffffcu) : 0u;
        eop_dw[3] = with_label ? ((backup[3] & 0xffffu) | 0x22000000u) : 0x1000000u;
        eop_dw[4] = with_label ? backup[4] : 0u;
        eop_dw[5] = 0u;
    }
    const bool ok = EmitCpInSlot(
        tail, tail_dwords,
        [lock, label, label_addr, label_value, interrupt, eop_dw]() {
            AmdGpu::Liverpool& lv = Lv();
            WriteDwordBack(lv, lock, 1u);
            lv.NativeBack(
                [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip); },
                false);
            if (label) {
                WriteDwordBack(lv, label_addr, label_value);
            }
            if (interrupt) {
                // liverpool.cpp EventWriteEop handler, the non-deferred branch (as W07 0x86eb60).
                const auto eop = std::bit_cast<AmdGpu::PM4CmdEventWriteEop>(eop_dw);
                lv.NativeBack(
                    [eop, &lv] {
                        if (auto* rasterizer = lv.NativeRasterizer()) {
                            rasterizer->OnFence();
                        }
                        eop.SignalFence(
                            [&lv](void* address, u64 value, u32 num_bytes) {
                                lv.NativeWriteFenceValue(address, value, num_bytes);
                            },
                            [] {
                                Platform::IrqC::Instance()->Signal(
                                    Platform::InterruptId::GfxEop);
                            });
                        // sotc: H04 fix round 1 (N2-94 W2 rule). SignalFence writes the EOP label
                        // for real; wake a command processor sleeping on the progress epoch.
                        NotifyProgressIfEvents();
                    },
                    back_strict);
            }
        },
        true);
    if (ok) {
        ::Native::Stats::SiteNative(site);
    } else {
        ::Native::Stats::SiteFallback(site);
    }
    return ok;
}

// sceGnmSubmitDone has no PM4 to replace: it waits for the GPU and syncs the queue.
bool SubmitDoneW12(s32* result) {
    static const u32 site = Site("w12:sceGnmSubmitDone");
    (void)result;
    ::Native::Stats::SiteFallback(site);
    return false;
}

} // namespace

void RegisterW12(Table& t) {
    t.InsertWaitFlipDone = &InsertWaitFlipDoneW12;
    t.SubmitCommandBuffersForWorkload = &SubmitCommandBuffersForWorkloadW12;
    t.SubmitAndFlipCommandBuffersForWorkload = &SubmitAndFlipCommandBuffersForWorkloadW12;
    t.SubmitDone = &SubmitDoneW12;
    t.FlipTail = &FlipTailW12;
}

} // namespace Native::Gnm
