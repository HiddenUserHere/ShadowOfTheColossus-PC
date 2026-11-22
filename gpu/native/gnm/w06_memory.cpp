// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W06: WRITE_DATA / DMA_DATA.
// Class Memory (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w06_memory.h, and
// gpu/sotc/hook_gnm_w06.inc. See gnm_common.h for the contract.
//
// Specification: liverpool.cpp ProcessGraphics / ProcessCompute, cases PM4ItOpcode::WriteData and
// PM4ItOpcode::DmaData. Emitter layouts: out/re/decomp/<VA>.c (flat Ghidra export; REA had no open
// binary in this session, so every VA below was read from the flat export, not from REA).
//
// Coverage table (site | contract | native op | evidence):
//   0x86a440 | WRITE_DATA dst_sel 5, data copied to dst (dword-aligned), wr_confirm = p5&1 |
//            WriteDataOp (memcpy on draw thread, OnFence first, NoteHostWrite) | 0086a440.c:35-88;
//            handler liverpool.cpp:2004 (graphics) / 2469 (compute). Raw bytes: n (decomp only).
//   0x8663e0 | WRITE_DATA dst_sel 5, control 0x84000500 (bit 31 unused by handler), wr_confirm p5&1 |
//            WriteDataOp | 008663e0.c:35-88. Raw bytes: n.
//   0x883380 | WRITE_DATA dst_sel 5, control 0x4000500, wr_confirm p5&1 | WriteDataOp |
//            00883380.c:35-88. Raw bytes: n.
//   0x8834d0 | WRITE_DATA dst_sel 2, engine/cache bits 25-26 (ignored by handler), wr_confirm p6&1 |
//            WriteDataOp | 008834d0.c:36-87. Raw bytes: n.
//   0x86a590 | same layout as 0x8834d0 (dst_sel 2, wr_confirm p6&1) | WriteDataOp |
//            0086a590.c:37-90. Raw bytes: n.
//   0x884b70 | WRITE_DATA of one dword 0x80000000 to (p2-4)&~3 (dst_sel 5) | WriteDataOp with that
//            single dword | 00884b70.c:25-31. Raw bytes: n.
//   0x86a700 | DMA_DATA; src_sel = p4&3, dst_sel = p2&1 (Memory/Gds), num_bytes = p6&0x1fffff,
//            src = p5, dst = p3, cp_sync = p7 (ignored by handler). Dst lo 0x3022c is dropped by the
//            handler (no-op). Native: DmaOp with the handler's branch table; Mem->Mem runs the
//            indirect-dispatch check (SetIndirectPatch) before copying | 0086a700.c:18-27 |
//            Raw bytes: n. REA: n (no open binary).
//   0x883640 | identical body to 0x86a700 | DmaOp | 00883640.c:18-27. Raw bytes: n.
//   0x86a7f0 | DMA_DATA to dst 0x3022c (dropped by the handler) | nothing emitted (handler is a
//            no-op) | 0086a7f0.c:17-24. Raw bytes: n.
//   Round 3 (X8, compute RELEASE_MEM, PM4 0xc0054902 = type 3, opcode 0x49, count 5 = 7 dwords).
//   Every site is queued as one EmitCp op (writes_memory) that rebuilds PM4CmdReleaseMem from the
//   dwords below and runs Lv().NativeReleaseMem(release, Lv().NativeAscPipeId()), the handler body
//   of liverpool.cpp ProcessCompute case ReleaseMem, inline on the command processor. The dwords:
//     dw1 = (p1 & 0x3f) | idx | rest, idx = 0x600 if p1 == 0x2f else 0x500 (event index);
//     dw2 = int_sel << 24 | data_sel << 29 | dst_sel << 16 (the full form: dst_sel = p2 & 1);
//     dw3/dw4 = address lo/hi; dw5/dw6 = data lo/hi.
//   0x8846f0 | RELEASE_MEM, full form: dw1 rest = (p2 & 0x10) << 23 | (s1 & 0x3f) << 12 |
//            (s2 & 3) << 25 (s1, s2 = the two stack args); dw2 = (p4 & 7) << 29 | (p2 & 1) << 16 |
//            int_sel 3; address p3, data p5 (both 64-bit) | ReleaseMemNative | raw bytes: y
//            (llvm-objdump of 0x8846f0-0x8847cb, eboot file offset VA + 0x4000).
//   0x884610 | same as 0x8846f0 with int_sel 2 | ReleaseMemNative | raw bytes: y (0x884610-0x8846eb).
//   0x884580 | RELEASE_MEM, event only: dw1 = (p1 & 0x3f) | idx | (p2 & 0x3f) << 12; dw2 = 0x01000000
//            (int_sel 1); dw3..dw6 = 0 | ReleaseMemNative | raw bytes: y (0x884580-0x884608).
//   0x8848e0 | RELEASE_MEM, label write at p2 - 4 (64-bit): dw1 = (p1 & 0x3f) | idx | (p3 & 0x3f)
//            << 12 | (p4 & 3) << 25; dw2 = 0x23000000 (int_sel 3, data_sel 1 = 32-bit low data);
//            dw3/dw4 = (p2 - 4) lo/hi; dw5 = 0x80000000, dw6 = 0 | ReleaseMemNative | raw bytes: y
//            (0x8848e0-0x88499b; the refill check and the cursor advance are the PM4 path only).
//   0x8849a0 | same as 0x8848e0 with int_sel 2 (dw2 = 0x22000000) | ReleaseMemNative | raw bytes:
//            y (0x8849a0-0x884a5f).
//   The guest body's ring-space refill and cursor advance are not reproduced: the native op writes
//   no PM4 at all, only the NOP that carries its id (EmitCp).
//   Stage: the rows are "native-gnm:memory" (class Memory of this file). w07_sync.cpp's older note
//   that keeps 0x8846f0 on PM4 (no ASC pipe id) is stale: NativeAscPipeId now exists.
//
// N2-94 W2 (virtual labels, SOTC_NATIVE_VLABELS). Coverage: item | done? | evidence
//   WRITE_DATA 4-byte / 8-byte payload recorded on the CP (Labels::Record before NativeBack) and
//     retired on the draw thread right after the real memcpy (Labels::Retire), for every WRITE_DATA
//     site above (0x86a440, 0x8663e0, 0x883380, 0x8834d0, 0x86a590, 0x884b70) | done (only when
//     SOTC_NATIVE_VLABELS=1; token 0 otherwise, Retire(0) is a no-op) | WriteDataNative below.
//   Native::Progress::Notify() after every real WRITE_DATA memory write | done, gated on
//     Switch::CpEvents() (off by default: no draw-thread atomic) | WriteDataNative.
//   RELEASE_MEM sites (0x8846f0, 0x884610, 0x884580, 0x8848e0, 0x8849a0) | not touched here: they
//     go through Liverpool::NativeReleaseMem, which H02 records; no double record in this file.
//   DMA_DATA sites (0x86a700, 0x883640, 0x86a7f0) | not labels (DmaNative copies or fills, no
//     4/8-byte label record) | unchanged.
//   0x884b70 writes 0x80000000 at (slot_end - 4) & ~3 | confirmed by raw bytes of out/eboot.elf
//     (file offset VA + 0x4000, 0x888b70..0x888c3a): the WRITE_DATA header is `c7 00 00 37 03 c0`
//     (0xc0033700), dw1 `c7 40 04 00 05 00 04` (0x04000500), dst `...8 08` = lo of (p2 - 4) & ~3,
//     and the payload store `89 48 10` of ecx = `c7 45 dc 00 00 00 80` (0x80000000) | done.
//   Bit 31 of the id (writes memory) is set on every WRITE_DATA op; the op runs inline on the CP.
//
// Dispatch hazard (gnm_common.h SetIndirectPatch): when a Mem->Mem DMA has 12 bytes and its
// destination is the payload of a native op NOP (dst-4 = type-3 NOP header, dst = 0x68750f01), the
// op records the patch instead of copying. The check runs at op time on the command processor,
// when the whole buffer is already written.

#include <array>
#include <bit>
#include <cstring>
#include <type_traits>
#include <vector>

#include "common/types.h"
#include "core/memory.h"
#include "native/gnm/gnm_common.h"
#include "native/native_labels.h"
#include "native/native_progress.h"
#include "native/native_switches.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Native::Gnm {
namespace {

typedef void (*Guest_WriteData)(u64, u64, const u32*, int, u32) __attribute__((sysv_abi));
typedef void (*Guest_WriteDataTo)(u64, u64, const u32*, int, u32, u32) __attribute__((sysv_abi));
typedef void (*Guest_Dma)(u64, u32, u64, u32, u64, u32, int) __attribute__((sysv_abi));
typedef void (*Guest_Dma7f0)(u64, u64, u32) __attribute__((sysv_abi));
typedef void (*Guest_884b70)(u64, u64) __attribute__((sysv_abi));

/// DMA destination the PM4 handler drops (liverpool.cpp DmaData: dst_addr_lo == 0x3022C).
constexpr u32 kDmaDroppedDst = 0x3022c;
/// SotcNativeOp payload type dword of a native op NOP (native_stream_ops.h packet layout).
constexpr u32 kNativeOpPayloadType = 0x68750f01u;
/// The DMA of the dispatch hazard: 12 bytes = dim_x..dim_z of a DISPATCH_DIRECT.
constexpr u32 kDispatchDimBytes = 12;

// DMA selector values (pm4_cmds.h DmaDataSrc / DmaDataDst).
constexpr u32 kSrcMemory = 0;
constexpr u32 kSrcGds = 1;
constexpr u32 kSrcData = 2;
constexpr u32 kSrcMemoryL2 = 3;
constexpr u32 kDstMemory = 0;
constexpr u32 kDstGds = 1;
constexpr u32 kDstMemoryL2 = 3;

/// PM4 type-3 NOP header: type 3 in bits 30-31, opcode 0x10 in bits 8-15.
bool IsNopHeader(u32 h) {
    return (h >> 30) == 3 && ((h >> 8) & 0xff) == 0x10;
}

/// Dispatch hazard: `dst` is the payload of a native op NOP, so the DMA becomes a dispatch patch
/// (see SetIndirectPatch). Returns true when recorded; then nothing is copied.
bool TryRecordIndirectPatch(VAddr dst, VAddr src) {
    if ((dst & 3) != 0) {
        return false;
    }
    const auto* words = reinterpret_cast<const u32*>(static_cast<uintptr_t>(dst));
    if (!IsNopHeader(words[-1]) || words[0] != kNativeOpPayloadType) {
        return false;
    }
    SetIndirectPatch(words[1], src);
    return true;
}

/// WRITE_DATA: the handler copies the payload dwords to the destination on the draw thread, after
/// OnFence, and notes the host write. The payload is captured now (the packet's own data).
bool WriteDataNative(u64 cb, u64 dst, const u32* data, int count) {
    if (count < 0) {
        return false;
    }
    const u32 num_dwords = static_cast<u32>(count);
    const u32 data_size = num_dwords * 4;
    const VAddr addr = dst & ~VAddr{3}; // the emitters mask the low two address bits
    std::vector<u32> payload(data, data + num_dwords);
    // sotc: N2-94 W2. A 4- or 8-byte write is a label candidate: its value is recorded on the
    // command processor (before NativeBack queues the real write) and retired on the draw thread.
    const bool track = Native::Switch::VirtualLabels() && (num_dwords == 1 || num_dwords == 2);
    // sotc: N2-94 W2. Progress::Notify wakes the CP when it sleeps on a label/event wait, so it is
    // gated on the CpEvents switch: with every switch off, a WRITE_DATA adds no draw-thread atomic.
    const bool notify = Native::Switch::CpEvents();
    return EmitCp(
        cb,
        [addr, data_size, payload = std::move(payload), track, notify]() {
            AmdGpu::Liverpool& lv = Lv();
            Vulkan::Rasterizer* rast = lv.NativeRasterizer();
            u64 token = 0;
            if (track) {
                // Little-endian: dword 0 is the low half of a 64-bit label.
                const u64 value = payload.size() == 1
                                      ? u64{payload[0]}
                                      : (u64{payload[0]} | (u64{payload[1]} << 32));
                token = Native::Labels::Record(addr, value, data_size);
            }
            lv.NativeBack(
                [rast, addr, data_size, payload, token, notify]() {
                    if (rast) {
                        rast->OnFence();
                    }
                    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                                payload.data(), data_size);
                    Core::MemoryManager::NoteHostWrite(addr, data_size, 4);
                    Native::Labels::Retire(token); // token 0: no-op
                    if (notify) {
                        Native::Progress::Notify();
                    }
                },
                true);
        },
        true);
}

/// DMA_DATA: the handler's branch table (liverpool.cpp DmaData), with the values the emitter wrote.
/// The `data` of a fill is the low dword of the source field, as the handler reads it.
bool DmaNative(u64 cb, u32 src_sel, u32 dst_sel, u64 src, u64 dst, u32 num_bytes) {
    const bool dst_is_memory = dst_sel == kDstMemory || dst_sel == kDstMemoryL2;
    return EmitCp(
        cb,
        [src_sel, dst_sel, src, dst, num_bytes]() {
            AmdGpu::Liverpool& lv = Lv();
            Vulkan::Rasterizer* rast = lv.NativeRasterizer();
            if (!rast) {
                return;
            }
            const bool src_is_memory = src_sel == kSrcMemory || src_sel == kSrcMemoryL2;
            const bool dst_is_mem = dst_sel == kDstMemory || dst_sel == kDstMemoryL2;
            const u32 src_lo = static_cast<u32>(src);
            const u32 dst_lo = static_cast<u32>(dst);
            if (src_sel == kSrcData && dst_sel == kDstGds) {
                lv.NativeBack(
                    [rast, dst_lo, num_bytes, src_lo]() {
                        rast->FillBuffer(dst_lo, num_bytes, src_lo, true);
                    },
                    false);
            } else if (src_is_memory && dst_sel == kDstGds) {
                lv.NativeBack(
                    [rast, dst_lo, src, num_bytes]() {
                        rast->CopyBuffer(dst_lo, src, num_bytes, true, false);
                    },
                    false);
            } else if (src_sel == kSrcData && dst_is_mem) {
                lv.NativeBack(
                    [rast, dst, num_bytes, src_lo]() {
                        rast->FillBuffer(dst, num_bytes, src_lo, false);
                    },
                    true);
            } else if (src_sel == kSrcGds && dst_is_mem) {
                lv.NativeBack(
                    [rast, dst, num_bytes, src_lo]() {
                        rast->CopyBuffer(dst, src_lo, num_bytes, false, true);
                    },
                    true);
            } else if (src_is_memory && dst_is_mem) {
                if (num_bytes == kDispatchDimBytes && TryRecordIndirectPatch(dst, src)) {
                    return;
                }
                lv.NativeBack(
                    [rast, dst, src, num_bytes]() {
                        rast->CopyBuffer(dst, src, num_bytes, false, false);
                    },
                    true);
            }
            // Other selector pairs are UNREACHABLE in the handler; nothing is done for them.
        },
        dst_is_memory);
}

} // namespace
} // namespace Native::Gnm

// The detours are global extern "C" symbols (declared in hook_gnm_w06.inc).
using namespace Native::Gnm;

// ---- RELEASE_MEM emitters (boundary 1, compute rings) ------------------------------------------

namespace Native::Gnm {
namespace {

typedef void (*Guest_RelMemFull)(u64, u32, u32, u64, u32, u64, u32, u32) __attribute__((sysv_abi));
typedef void (*Guest_RelMemEvent)(u64, u32, u32) __attribute__((sysv_abi));
typedef void (*Guest_RelMemLabel)(u64, u32, u64, u32, u32) __attribute__((sysv_abi));

// The PM4 layout the handler reads (liverpool.cpp ProcessCompute, case ReleaseMem) must be the
// 28-byte, trivially copyable struct the emitters' packet occupies.
static_assert(sizeof(AmdGpu::PM4CmdReleaseMem) == 7 * sizeof(u32));
static_assert(std::is_trivially_copyable_v<AmdGpu::PM4CmdReleaseMem>);

/// RELEASE_MEM header dword: type 3, opcode 0x49, count 5 (7 dwords in all).
constexpr u32 kReleaseMemHeader = 0xc0054902u;
/// Interrupt select values the emitters write into dw2 bits 24-26 (int_sel).
constexpr u32 kIntSelOne = 1u;
constexpr u32 kIntSelTwo = 2u;
constexpr u32 kIntSelThree = 3u;
/// data_sel 1 (32-bit low data) in dw2 bits 29-31.
constexpr u32 kDataSelLow32 = 1u;

u32 Lo32(u64 v) {
    return static_cast<u32>(v);
}
u32 Hi32(u64 v) {
    return static_cast<u32>(v >> 32);
}

/// dw1 event part shared by all five sites: event type (low 6 bits) and event index (0x600 when
/// the event type is 0x2f, else 0x500), as the emitters compute them.
u32 ReleaseMemEvent(u32 event_type) {
    const u32 index = event_type == 0x2fu ? 0x600u : 0x500u;
    return (event_type & 0x3fu) | index;
}

/// Builds the packet from its seven dwords and queues the handler body on the command processor
/// (EmitCp runs it inline there, so the parser order is kept). `writes_memory`: the release
/// writes its fence / label from the CPU side. False (nothing written) when the op cannot be
/// queued; the caller then runs the guest body.
bool ReleaseMemNative(u64 cb, u32 dw1, u32 dw2, u32 dw3, u32 dw4, u32 dw5, u32 dw6) {
    const std::array<u32, 7> words{kReleaseMemHeader, dw1, dw2, dw3, dw4, dw5, dw6};
    const auto release = std::bit_cast<AmdGpu::PM4CmdReleaseMem>(words);
    return EmitCp(
        cb,
        [release]() {
            AmdGpu::Liverpool& lv = Lv();
            lv.NativeReleaseMem(release, lv.NativeAscPipeId());
        },
        true);
}

/// The full form (0x8846f0, 0x884610): p1 event, p2 bit 4 -> dw1 bit 27, the two stack args
/// s1 (dw1 bits 12-17) and s2 (dw1 bits 25-26, cache policy). dw2: p4 & 7 -> data_sel, p2 bit 0
/// -> dst_sel, int_sel given by the caller.
bool ReleaseMemFull(u64 cb, u32 p1, u32 p2, u64 addr, u32 p4, u64 data, u32 s1, u32 s2,
                    u32 int_sel) {
    const u32 dw1 = ReleaseMemEvent(p1) | ((p2 & 0x10u) << 23) | ((s1 & 0x3fu) << 12) |
                    ((s2 & 3u) << 25);
    const u32 dw2 = ((p4 & 7u) << 29) | ((p2 & 1u) << 16) | (int_sel << 24);
    return ReleaseMemNative(cb, dw1, dw2, Lo32(addr), Hi32(addr), Lo32(data), Hi32(data));
}

/// 0x884580: event only, no address and no data: dw1 bits 12-17 from p2 & 0x3f, dw2 int_sel 1.
bool ReleaseMemEventOnly(u64 cb, u32 p1, u32 p2) {
    const u32 dw1 = ReleaseMemEvent(p1) | ((p2 & 0x3fu) << 12);
    const u32 dw2 = kIntSelOne << 24;
    return ReleaseMemNative(cb, dw1, dw2, 0, 0, 0, 0);
}

/// 0x8848e0, 0x8849a0: a 32-bit label 0x80000000 at (p2 - 4), 64-bit address.
bool ReleaseMemLabel(u64 cb, u32 p1, u64 p2, u32 p3, u32 p4, u32 int_sel) {
    const u64 addr = p2 - 4;
    const u32 dw1 = ReleaseMemEvent(p1) | ((p3 & 0x3fu) << 12) | ((p4 & 3u) << 25);
    const u32 dw2 = (int_sel << 24) | (kDataSelLow32 << 29);
    return ReleaseMemNative(cb, dw1, dw2, Lo32(addr), Hi32(addr), 0x80000000u, 0);
}

} // namespace
} // namespace Native::Gnm

extern "C" __attribute__((sysv_abi)) void GnmW06_8846f0(u64 cb, u32 p1, u32 p2, u64 p3, u32 p4,
                                                       u64 p5, u32 s1, u32 s2) {
    static const u32 site = Site("w06:8846f0");
    if (!Enabled(Class::Memory) ||
        !ReleaseMemFull(cb, p1, p2, p3, p4, p5, s1, s2, kIntSelThree)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_RelMemFull>(0x8846f0)(cb, p1, p2, p3, p4, p5, s1, s2);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_884580(u64 cb, u32 p1, u32 p2) {
    static const u32 site = Site("w06:884580");
    if (!Enabled(Class::Memory) || !ReleaseMemEventOnly(cb, p1, p2)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_RelMemEvent>(0x884580)(cb, p1, p2);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_884610(u64 cb, u32 p1, u32 p2, u64 p3, u32 p4,
                                                       u64 p5, u32 s1, u32 s2) {
    static const u32 site = Site("w06:884610");
    if (!Enabled(Class::Memory) ||
        !ReleaseMemFull(cb, p1, p2, p3, p4, p5, s1, s2, kIntSelTwo)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_RelMemFull>(0x884610)(cb, p1, p2, p3, p4, p5, s1, s2);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_8848e0(u64 cb, u32 p1, u64 p2, u32 p3, u32 p4) {
    static const u32 site = Site("w06:8848e0");
    if (!Enabled(Class::Memory) || !ReleaseMemLabel(cb, p1, p2, p3, p4, kIntSelThree)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_RelMemLabel>(0x8848e0)(cb, p1, p2, p3, p4);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_8849a0(u64 cb, u32 p1, u64 p2, u32 p3, u32 p4) {
    static const u32 site = Site("w06:8849a0");
    if (!Enabled(Class::Memory) || !ReleaseMemLabel(cb, p1, p2, p3, p4, kIntSelTwo)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_RelMemLabel>(0x8849a0)(cb, p1, p2, p3, p4);
        return;
    }
    Native::Stats::SiteNative(site);
}

// ---- WRITE_DATA emitters (boundary 1) ----------------------------------------------------------

extern "C" __attribute__((sysv_abi)) void GnmW06_86a440(u64 cb, u64 dst, const u32* data,
                                                       int count, u32 wr_confirm) {
    static const u32 site = Site("w06:86a440");
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, dst, data, count)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_WriteData>(0x86a440)(cb, dst, data, count, wr_confirm);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_8663e0(u64 cb, u64 dst, const u32* data,
                                                       int count, u32 wr_confirm) {
    static const u32 site = Site("w06:8663e0");
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, dst, data, count)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_WriteData>(0x8663e0)(cb, dst, data, count, wr_confirm);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_883380(u64 cb, u64 dst, const u32* data,
                                                       int count, u32 wr_confirm) {
    static const u32 site = Site("w06:883380");
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, dst, data, count)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_WriteData>(0x883380)(cb, dst, data, count, wr_confirm);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_8834d0(u64 cb, u64 dst, const u32* data,
                                                       int count, u32 engine, u32 wr_confirm) {
    static const u32 site = Site("w06:8834d0");
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, dst, data, count)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_WriteDataTo>(0x8834d0)(cb, dst, data, count, engine, wr_confirm);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_86a590(u64 cb, u64 dst, const u32* data,
                                                       int count, u32 engine, u32 wr_confirm) {
    static const u32 site = Site("w06:86a590");
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, dst, data, count)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_WriteDataTo>(0x86a590)(cb, dst, data, count, engine, wr_confirm);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_884b70(u64 cb, u64 slot_end) {
    static const u32 site = Site("w06:884b70");
    // One dword 0x80000000 to (slot_end - 4), as the emitter writes it.
    static const u32 kValue = 0x80000000u;
    if (!Enabled(Class::Memory) || !WriteDataNative(cb, slot_end - 4, &kValue, 1)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_884b70>(0x884b70)(cb, slot_end);
        return;
    }
    Native::Stats::SiteNative(site);
}

// ---- DMA_DATA emitters (boundary 1) ------------------------------------------------------------

extern "C" __attribute__((sysv_abi)) void GnmW06_86a700(u64 cb, u32 p2, u64 dst, u32 p4, u64 src,
                                                       u32 p6, int p7) {
    static const u32 site = Site("w06:86a700");
    (void)p7; // cp_sync: the handler does not read it
    if (!Enabled(Class::Memory)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_Dma>(0x86a700)(cb, p2, dst, p4, src, p6, p7);
        return;
    }
    if (static_cast<u32>(dst) == kDmaDroppedDst) {
        // The handler drops this DMA entirely: nothing to emit.
        Native::Stats::SiteNative(site);
        return;
    }
    if (!DmaNative(cb, p4 & 3, p2 & 1, src, dst, p6 & 0x1fffff)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_Dma>(0x86a700)(cb, p2, dst, p4, src, p6, p7);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_883640(u64 cb, u32 p2, u64 dst, u32 p4, u64 src,
                                                       u32 p6, int p7) {
    static const u32 site = Site("w06:883640");
    (void)p7;
    if (!Enabled(Class::Memory)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_Dma>(0x883640)(cb, p2, dst, p4, src, p6, p7);
        return;
    }
    if (static_cast<u32>(dst) == kDmaDroppedDst) {
        Native::Stats::SiteNative(site);
        return;
    }
    if (!DmaNative(cb, p4 & 3, p2 & 1, src, dst, p6 & 0x1fffff)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_Dma>(0x883640)(cb, p2, dst, p4, src, p6, p7);
        return;
    }
    Native::Stats::SiteNative(site);
}

extern "C" __attribute__((sysv_abi)) void GnmW06_86a7f0(u64 cb, u64 src, u32 num_bytes) {
    static const u32 site = Site("w06:86a7f0");
    // Always DMA to 0x3022c, which the handler drops: nothing is emitted (the packet had no effect).
    (void)src;
    (void)num_bytes;
    if (!Enabled(Class::Memory)) {
        Native::Stats::SiteFallback(site);
        Guest<Guest_Dma7f0>(0x86a7f0)(cb, src, num_bytes);
        return;
    }
    Native::Stats::SiteNative(site);
}

namespace Native::Gnm {

void RegisterW06(Table& t) {
    // WRITE_DATA / DMA_DATA are eboot emitters only (boundary 1, hook_gnm_w06.inc). No sceGnm*
    // wrapper of this class is in the Table, so no slots are filled.
    (void)t;
}

} // namespace Native::Gnm
