// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary worker W11: Constant engine, misc opcodes, markers.
// Class Ce (SOTC_NATIVE_GNM=...). Owned files: this one, an optional w11_ce_misc.h, and
// gpu/sotc/hook_gnm_w11.inc. See gnm_common.h for the contract.
//
// Coverage table (VA or sceGnm name | contract | native op | evidence):
//
//   sceGnmInsertPushMarker (Table.InsertPushMarker) | NOP DebugMarkerPush, label copied; the
//     handler (liverpool.cpp ProcessGraphics, DebugMarkerPush) calls rasterizer->ScopeMarkerBegin
//     only when guest markers are on AND the draw thread is off | native: EmitCpInSlot, op does the
//     same gate (EmulatorSettings.IsVkGuestMarkersEnabled, !Lv().IsBackThreaded()) and the call |
//     evidence: gnmdriver.cpp sceGnmInsertPushMarker (size check packet_size+2 == size),
//     liverpool.cpp:1438-1446; raw bytes n/a (sceGnm host code); REA n/a.
//   sceGnmInsertPushColorMarker (Table.InsertPushColorMarker) | NOP DebugColorMarkerPush, label +
//     colour | native: as above, ScopedMarkerInsertColor | liverpool.cpp:1447-1457.
//   sceGnmInsertPopMarker (Table.InsertPopMarker) | NOP DebugMarkerPop | native: ScopeMarkerEnd |
//     liverpool.cpp:1458-1465; gnmdriver size == 6 check.
//   sceGnmInsertSetMarker (Table.InsertSetMarker) | NOP DebugSetMarker, the handler has no case
//     (default: break), so no effect | native: no-op op in an EmitCpInSlot NOP | liverpool.cpp:1466
//     default branch; gnmdriver packet_size+2 == size check.
//   sceGnmInsertDingDongMarker (Table.InsertDingDongMarker) | NOP with payload 0, no effect | native:
//     no-op op | gnmdriver size == 4 check; liverpool.cpp NOP handler (count 0 -> break).
//   0x86c000 (opcode 0x2F NUM_INSTANCES, 2 dwords: header 0xc0002f00 + value; 11 callers in
//     decomp: FUN_00133e20 FUN_001338d0 FUN_0085c9b0 FUN_00094dd0 FUN_00133bd0 FUN_000db620 ...) |
//     handler sets regs.num_instances.num_instances and MarkRegs | native: EmitCp, op writes
//     Lv().NativeRegs().num_instances and NativeMarkField (same statements as the handler) |
//     evidence: out/re/decomp/0086/0086c000.c:17 (header 0xc0002f00, param_2 at +4),
//     liverpool.cpp:1808-1812 (handler), pm4_opcodes.h NumInstances = 0x2F; raw bytes: decomp only
//     (not read with read_bytes); REA not used.
//   0x86a880 (opcode 0x8E GET_LOD_STATS, 4 dwords: 0xc0028e00, size-0x20, addr>>6,
//     (addr>>38)&3 | 0xbfc00; caller FUN_0013d620) | handler (liverpool.cpp:2118-2160) fills the
//     256 bank entries into guest memory at base+0x40 + half*2048 for each half that fits in size,
//     from the draw thread
//     through BackOrRun(writes_memory=false), gated by SOTC_LOD_STATS | native: EmitCp, op reads the
//     same env gate, then Lv().NativeBack(..., false) with the same memory write (SotcLod::Collect,
//     runtime_memory_write_backing) | evidence: out/re/decomp/0086/0086a880.c:17-26 (header and
//     fields), liverpool.cpp:2118-2160. NOT replicated: SOTC_LOD_TRACE log lines (debug output only;
//     they are printed only when that env var is set).
//
//   0x865f00 INCREMENT_CE_COUNTER (0xc0008400, body dword 1; 2 dwords; out/re/decomp/0086/00865f00.c,
//     callers none listed) | ProcessCeUpdate IncrementCeCounter: ++cblock.ce_count (body unread) |
//     native: EmitCp on the CE stream, op ++Lv().NativeCblock().ce_count (inline, CP) | evidence:
//     raw bytes (llvm-mc on out/eboot.elf, file offset VA+0x4000): movabs $0x1C0008400 at [rax].
//   0x865f50 INCREMENT_CE_COUNTER (0xc0008400, body dword 3; 2 dwords; out/re/decomp/0086/00865f50.c,
//     caller 0x85c580) | same handler and op as 0x865f00 | raw bytes: movabs $0x3C0008400.
//   0x865ff0 DUMP_CONST_RAM (0xc0038300, 4 body dwords: offset, num_dw, addr lo, addr hi) |
//     ProcessCeUpdate DumpConstRam: DrainBack, memcpy constants_heap -> guest, NoteHostWrite(5) |
//     native: EmitCp (writes memory), op NativeDrainBack, the same memcpy and NoteHostWrite; the
//     clamp of the emitter is replicated (out of range: num_dw 0) | evidence: decomp 00865ff0.c,
//     raw bytes: cmp $0xBFFF / $0xC000 clamps, header 0xC0038300, addr & -4 in the two high dwords.
//   0x866090 WRITE_CONST_RAM (0xc0008100 | count << 16, offset body, count payload dwords; caller
//     none listed) | ProcessCeUpdate WriteConstRam: memcpy payload -> constants_heap | native: EmitCp,
//     the payload is copied at emit time (as the emitter's import_memcpy does), the op copies it
//     into constants_heap; the clamp (offset 0xbffc, count 1) is replicated | evidence: decomp
//     00866090.c, raw bytes: header store 0xC0008100 with count << 16, clamp constants 0xBFFC.
//   0x866150 WRITE_CONST_RAM, caller takes the payload pointer (returns it) | the same packet; the
//     payload is the NOP's body space (EmitWithSpace, align 4): the op reads it on the CP at run time,
//     exactly where the handler reads the PM4 payload | returns 0 (nothing written) for invalid
//     arguments and for a full buffer, like the emitter; an invalid call runs the guest body |
//     evidence: decomp 00866150.c, raw bytes: the validity compares (0xBFFF, 0xFFFC, 0x3000, 0xC000).
//
//   0x88 WAIT_ON_DE_COUNTER_DIFF (0x865fa0; args cb, u32 diff in esi; raw bytes: header 0xC0008800,
//     body diff; caller none listed) | ProcessCeUpdate WaitOnDeCounterDiff: `while ((de - ce) >=
//     diff) YIELD_CE` | native: EmitWait on the CE stream (ProcessCeUpdate's SotcNativeWait branch
//     polls and YIELD_CEs), predicate (de - ce) < diff in u32 via NativeCblock() | evidence:
//     liverpool.cpp:1266-1272, 1235-1240; llvm-objdump 0x865fa0 (esi stored at +4).
//   0x86 WAIT_ON_CE_COUNTER (0x86f490, 0x883f30; arg cb only; raw bytes: movabs 0x1C0008600, i.e.
//     header 0xC0008600, body 1, ignored by the handler; callers: FUN_0085c580) | ProcessGraphics
//     WaitOnCeCounter: `while (ce <= de && !ce_task.done) RESUME_GFX(ce_task)` | native: EmitWaitCe
//     on the DE stream (ProcessGraphics SotcNativeWaitCe drives ce_task and stops when it is done),
//     predicate ce > de via NativeCblock() | evidence: liverpool.cpp:2129-2134, 1432-1442.
//
//   Kept on PM4 (not replaced, reasons in the report):
//   0x85 INCREMENT_DE_COUNTER (0x86f4e0): owned by W07 (gpu/native/gnm/w07_sync.cpp, row in
//     hook_gnm_w07.inc); not replaced here, so the site is not installed twice.
//   0x22 COND_EXEC (0x866360, 0x86ea50, 0x883300): the following dwords are skipped by the parser,
//   a native op cannot skip them. 0x33 / 0x3F INDIRECT_BUFFER (0x866600, 0x866680, 0x884c40,
//   0x86fa00, 0x86fa80, 0x8848a0, 0x884cc0): nested parse, needs the parser. 0x39 MEM_SEMAPHORE
//   (0x86fac0, 0x86fb50, 0x8852a0) and 0x59 REWIND (0x866530, 0x86f930, 0x8847d0): the handler
//   yields the fiber while polling. 0x34 STRMOUT_BUFFER_UPDATE (0x86ef60, 0x86f010): no effect
//   in the port, but the handler logs a warning; the decomp shows no direct callers. 0x8c / 0x8d
//   (0x86b750, 0x86b810, 0x883ac0, 0x883bb0): no handler in ProcessGraphics or ProcessCeUpdate (the
//   default branch is UNREACHABLE), so they are not reached on a working path.
//
// N2-94 H07 (CE ops, inline placement). Coverage: item | done? | evidence
//   Every CE/marker op emitted here goes through EmitCp / EmitCpInSlot / EmitWithSpace with
//     Where::CommandProcessor, so StreamOps sets bit 30 (kRunOnCpBit, native_stream_ops.cpp:132-133)
//     and the op runs inline on the command processor, in parser order | done, no change needed.
//   Inline ops (bit 30): InsertPush/Pop/Set/DingDong markers (EmitCpInSlot), 0x86c000, 0x86a880,
//     0x865f00, 0x865f50, 0x865ff0 (writes memory), 0x866090, 0x866150 (EmitWithSpace on CP).
//   Waits (not ops, no bit 30 meaning): 0x865fa0 (EmitWait, CE stream) and 0x86f490 / 0x883f30
//     (EmitWaitCe, DE stream). The parser polls their predicate at the NOP; they run no CP code.
//   Nothing behavioural changed in this file (PM4 path untouched); W2 label recording is not
//     needed here because no CE op writes a 4/8-byte label (WRITE_CONST_RAM / DUMP_CONST_RAM write
//     the constant heap or a fixed-size dump, not a virtual label).

#include "native/gnm/gnm_common.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_lod_stats.h"

// sotc: writes guest memory through the backing view (runtime_memory.c), as liverpool.cpp does.
extern "C" int runtime_memory_write_backing(unsigned long long address, const void* data,
                                            unsigned long long size);

namespace Native::Gnm {

namespace {

// The PM4 marker handlers' gate (liverpool.cpp DebugMarkerPush / Pop): guest markers on and the
// draw thread off. Called only inside a command-processor op.
bool GuestMarkersLive() {
    return EmulatorSettings.IsVkGuestMarkersEnabled() && !Lv().IsBackThreaded();
}

void MarkerPush(const std::string& label) {
    if (!GuestMarkersLive()) {
        return;
    }
    if (auto* rast = Lv().NativeRasterizer()) {
        rast->ScopeMarkerBegin(label, true);
    }
}

void MarkerPushColor(const std::string& label, u32 color) {
    if (!GuestMarkersLive()) {
        return;
    }
    if (auto* rast = Lv().NativeRasterizer()) {
        rast->ScopedMarkerInsertColor(label, color, true);
    }
}

void MarkerPop() {
    if (!GuestMarkersLive()) {
        return;
    }
    if (auto* rast = Lv().NativeRasterizer()) {
        rast->ScopeMarkerEnd(true);
    }
}

// SOTC_LOD_STATS gate of liverpool.cpp GET_LOD_STATS (read once, same rule).
bool LodStatsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_LOD_STATS");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// Emits `op` into the sceGnm fixed slot and counts the site. False: nothing was written, so the
// caller's PM4 path runs.
template <typename Op>
bool MarkerSlot(u32 site, u32* cmdbuf, u32 size, Op&& op) {
    if (!Enabled(Class::Ce) || !EmitCpInSlot(cmdbuf, size, std::forward<Op>(op))) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

// Places `op` on the command processor in the stream of constant buffer `cb` (CE or DE). False:
// nothing was written and the caller runs the guest body.
bool CpOp(u32 site, void* cb, Common::UniqueFunction<void>&& op, bool writes_memory = false) {
    if (!Enabled(Class::Ce) || !EmitCp(reinterpret_cast<u64>(cb), std::move(op), writes_memory)) {
        Stats::SiteFallback(site);
        return false;
    }
    Stats::SiteNative(site);
    return true;
}

// Driver packet size of the push / set markers (gnmdriver.cpp sceGnmInsertPushMarker, SetMarker).
u32 MarkerPacketSize(const char* marker) {
    const auto len = std::strlen(marker);
    return static_cast<u32>(((len + 8) >> 2) + ((len + 0xc) >> 3) * 2);
}

// Driver packet size of the push colour marker (gnmdriver.cpp sceGnmInsertPushColorMarker).
u32 ColorMarkerPacketSize(const char* marker) {
    const auto len = std::strlen(marker);
    return static_cast<u32>(((len + 0xc) >> 2) + ((len + 0x10) >> 3) * 2);
}

bool InsertPushMarkerNative(u32* cmdbuf, u32 size, const char* marker) {
    static const u32 site = Site("w11:InsertPushMarker");
    if (!cmdbuf || !marker || MarkerPacketSize(marker) + 2 != size) {
        return false; // the PM4 path returns its own error
    }
    return MarkerSlot(site, cmdbuf, size,
                      [label = std::string(marker)] { MarkerPush(label); });
}

bool InsertPushColorMarkerNative(u32* cmdbuf, u32 size, const char* marker, u32 color) {
    static const u32 site = Site("w11:InsertPushColorMarker");
    if (!cmdbuf || !marker || ColorMarkerPacketSize(marker) + 2 != size) {
        return false;
    }
    return MarkerSlot(site, cmdbuf, size, [label = std::string(marker), color] {
        MarkerPushColor(label, color);
    });
}

bool InsertPopMarkerNative(u32* cmdbuf, u32 size) {
    static const u32 site = Site("w11:InsertPopMarker");
    if (!cmdbuf || size != 6) {
        return false;
    }
    return MarkerSlot(site, cmdbuf, size, [] { MarkerPop(); });
}

bool InsertSetMarkerNative(u32* cmdbuf, u32 size, const char* marker) {
    static const u32 site = Site("w11:InsertSetMarker");
    if (!cmdbuf || !marker || MarkerPacketSize(marker) + 2 != size) {
        return false;
    }
    // liverpool.cpp handles DebugSetMarker with no case (default: break): no effect.
    return MarkerSlot(site, cmdbuf, size, [] {});
}

bool InsertDingDongMarkerNative(u32* cmdbuf, u32 size) {
    static const u32 site = Site("w11:InsertDingDongMarker");
    if (!cmdbuf || size != 4) {
        return false;
    }
    // A NOP with no payload: the command processor does nothing for it.
    return MarkerSlot(site, cmdbuf, size, [] {});
}

using GuestSetNumInstances = void(__attribute__((sysv_abi)) *)(void*, unsigned int);
using GuestGetLodStats = void(__attribute__((sysv_abi)) *)(void*, unsigned long long, int);
using GuestVoidCb = void(__attribute__((sysv_abi)) *)(void*);
using GuestDumpConstRam = void(__attribute__((sysv_abi)) *)(void*, unsigned long long, unsigned int,
                                                            unsigned int);
using GuestWriteConstRam = void(__attribute__((sysv_abi)) *)(void*, unsigned int,
                                                            unsigned long long, unsigned int);
using GuestWriteConstRamSpace = unsigned long long(__attribute__((sysv_abi)) *)(void*, unsigned int,
                                                                               unsigned int);

} // namespace

} // namespace Native::Gnm

// Eboot emitter 0x86c000 (opcode 0x2F NUM_INSTANCES): FUN_0086c000 writes 0xc0002f00, value.
// out/re/decomp/0086/0086c000.c:17. Called by 11 guest functions; the handler is
// liverpool.cpp:1808-1812.
extern "C" __attribute__((sysv_abi)) void GnmW11_86c000(void* cb, unsigned int value) {
    static const u32 site = Native::Gnm::Site("0x86c000 W11 NUM_INSTANCES");
    const bool ok = Native::Gnm::Enabled(Native::Gnm::Class::Ce) &&
                    Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [value] {
                        auto& regs = Native::Gnm::Lv().NativeRegs();
                        regs.num_instances.num_instances = value;
                        Native::Gnm::Lv().NativeMarkField(&regs.num_instances,
                                                          sizeof(regs.num_instances));
                    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestSetNumInstances>(0x86c000)(cb, value);
}

// Eboot emitter 0x86a880 (opcode 0x8E GET_LOD_STATS): FUN_0086a880 writes 0xc0028e00, size - 0x20,
// addr >> 6, (addr >> 38) & 3 | 0xbfc00. out/re/decomp/0086/0086a880.c:17-26. The handler is
// liverpool.cpp:2118-2160 (the size is the argument itself: (size - 0x20) + 0x20).
extern "C" __attribute__((sysv_abi)) void GnmW11_86a880(void* cb, unsigned long long addr,
                                                        int size) {
    static const u32 site = Native::Gnm::Site("0x86a880 W11 GET_LOD_STATS");
    // The handler's base: (d[1] << 6) | ((d[2] & 3) << 38), with d[1] = addr >> 6 (32-bit).
    const u64 base = (static_cast<u64>(static_cast<u32>(addr >> 6)) << 6) |
                     ((static_cast<u64>(addr >> 38) & 3u) << 38);
    const u32 bytes = static_cast<u32>(size);
    const bool ok = Native::Gnm::Enabled(Native::Gnm::Class::Ce) &&
                    Native::Gnm::EmitCp(reinterpret_cast<u64>(cb), [base, bytes] {
                        if (!Native::Gnm::LodStatsEnabled()) {
                            return;
                        }
                        Native::Gnm::Lv().NativeBack(
                            [base, bytes] {
                                u64 entries[256];
                                SotcLod::Collect(entries);
                                for (u32 half = 0; 0x40 + (half + 1) * sizeof(entries) <= bytes;
                                     ++half) {
                                    runtime_memory_write_backing(
                                        base + 0x40 + half * sizeof(entries), entries,
                                        sizeof(entries));
                                }
                            },
                            false);
                    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<Native::Gnm::GuestGetLodStats>(0x86a880)(cb, addr, size);
}

// Eboot emitters 0x865f00 and 0x865f50 (opcode 0x84 INCREMENT_CE_COUNTER): each writes a 2-dword
// packet, header 0xC0008400 and one body dword (1 and 3; the handler never reads it). Handler:
// liverpool.cpp ProcessCeUpdate, IncrementCeCounter: ++cblock.ce_count. The op runs inline on the CE
// stream, where the NOP sits.
extern "C" __attribute__((sysv_abi)) void GnmW11_865f00(void* cb) {
    static const u32 site = Native::Gnm::Site("0x865f00 W11 INCREMENT_CE_COUNTER");
    if (Native::Gnm::CpOp(site, cb, [] { ++Native::Gnm::Lv().NativeCblock().ce_count; })) {
        return;
    }
    Native::Gnm::Guest<Native::Gnm::GuestVoidCb>(0x865f00)(cb);
}

extern "C" __attribute__((sysv_abi)) void GnmW11_865f50(void* cb) {
    static const u32 site = Native::Gnm::Site("0x865f50 W11 INCREMENT_CE_COUNTER");
    if (Native::Gnm::CpOp(site, cb, [] { ++Native::Gnm::Lv().NativeCblock().ce_count; })) {
        return;
    }
    Native::Gnm::Guest<Native::Gnm::GuestVoidCb>(0x865f50)(cb);
}

// Eboot emitter 0x865ff0 (opcode 0x83 DUMP_CONST_RAM): arguments (cb, addr 64-bit in rsi, offset in
// edx, num_dw in ecx; raw bytes). Header 0xC0038300, body: offset, num_dw & 0x7fff, addr & ~3 (lo, hi).
// Out-of-range requests are emitted with num_dw 0. Handler: liverpool.cpp ProcessCeUpdate
// DumpConstRam: DrainBack, memcpy constants_heap -> guest, NoteHostWrite(5).
extern "C" __attribute__((sysv_abi)) void GnmW11_865ff0(void* cb, unsigned long long addr,
                                                       unsigned int offset, unsigned int num_dw) {
    static const u32 site = Native::Gnm::Site("0x865ff0 W11 DUMP_CONST_RAM");
    // The emitter's clamp (offset > 0xbfff, or num_dw * 4 beyond the 48 KiB heap) gives num_dw 0.
    const bool bad = offset > 0xbfffu || u64{num_dw} * 4 > 0xc000u ||
                     u64{num_dw} * 4 + (offset & 0xffffu) > 0xc000u;
    const u32 heap_off = offset & 0xffffu;
    const u32 size = bad ? 0u : (num_dw & 0x7fffu) * 4u;
    const u64 dst = addr & ~u64{3};
    const bool ok = Native::Gnm::CpOp(
        site, cb,
        [heap_off, size, dst] {
            // Same order as the handler: queued draws may still read the old constants first.
            Native::Gnm::Lv().NativeDrainBack();
            if (size != 0) {
                auto& ce = Native::Gnm::Lv().NativeCblock();
                std::memcpy(reinterpret_cast<void*>(dst), ce.constants_heap.data() + heap_off,
                            size);
            }
            Core::MemoryManager::NoteHostWrite(dst, size, 5);
        },
        true);
    if (ok) {
        return;
    }
    Native::Gnm::Guest<Native::Gnm::GuestDumpConstRam>(0x865ff0)(cb, addr, offset, num_dw);
}

// Eboot emitter 0x866090 (opcode 0x81 WRITE_CONST_RAM, payload copied from `src` at emit time):
// header 0xC0008100 | count << 16, body: offset & 0xffff, then count payload dwords. The out-of-range
// clamp sets offset 0xbffc and count 1. Handler: liverpool.cpp ProcessCeUpdate WriteConstRam.
extern "C" __attribute__((sysv_abi)) void GnmW11_866090(void* cb, unsigned int offset,
                                                       unsigned long long src, unsigned int count) {
    static const u32 site = Native::Gnm::Site("0x866090 W11 WRITE_CONST_RAM");
    const bool bad = offset > 0xbfffu || u64{count} * 4 > 0xc000u ||
                     u64{count} * 4 + (offset & 0xffffu) > 0xc000u;
    const u32 heap_off = bad ? 0xbffcu : (offset & 0xffffu);
    const u32 dwords = bad ? 1u : count;
    // The payload is taken now, as the emitter's memcpy does, so later guest writes do not change it.
    std::vector<u32> data(dwords);
    std::memcpy(data.data(), reinterpret_cast<const void*>(src), size_t{dwords} * 4);
    const bool ok = Native::Gnm::CpOp(site, cb, [heap_off, data = std::move(data)] {
        auto& ce = Native::Gnm::Lv().NativeCblock();
        std::memcpy(ce.constants_heap.data() + heap_off, data.data(), data.size() * 4);
    });
    if (ok) {
        return;
    }
    Native::Gnm::Guest<Native::Gnm::GuestWriteConstRam>(0x866090)(cb, offset, src, count);
}

// Eboot emitter 0x866150 (opcode 0x81 WRITE_CONST_RAM, caller-filled payload): the guest writes the
// count payload dwords at the returned pointer, which is the NOP's body space (EmitWithSpace). The
// op reads that space on the command processor (the handler reads the same dwords from the PM4
// packet, at the same point of the CE stream). Invalid arguments: the guest body, which writes
// nothing and returns 0 (raw bytes: the 0xBFFF, 0xFFFC, 0x3000 and 0xC000 compares).
extern "C" __attribute__((sysv_abi)) unsigned long long GnmW11_866150(void* cb, unsigned int offset,
                                                                    unsigned int count) {
    static const u32 site = Native::Gnm::Site("0x866150 W11 WRITE_CONST_RAM (payload space)");
    const bool valid = offset < 0xc000u && (offset & 0xfffcu) == offset && count <= 0x3000u &&
                       u64{offset & 0xffffu} + u64{count} * 4 <= 0xc000u;
    if (!valid || !Native::Gnm::Enabled(Native::Gnm::Class::Ce)) {
        Native::Stats::SiteFallback(site);
        return Native::Gnm::Guest<Native::Gnm::GuestWriteConstRamSpace>(0x866150)(cb, offset, count);
    }
    // The op learns the payload address only after EmitWithSpace returns; the CP runs it later.
    auto body = std::make_shared<u64>(0);
    const u32 heap_off = offset & 0xffffu;
    const u64 at = Native::StreamOps::EmitWithSpace(
        reinterpret_cast<u64>(cb),
        [body, heap_off, count] {
            const u64 src = *body;
            if (src == 0) {
                return;
            }
            auto& ce = Native::Gnm::Lv().NativeCblock();
            std::memcpy(ce.constants_heap.data() + heap_off, reinterpret_cast<const void*>(src),
                        size_t{count} * 4);
        },
        count, 4, false, Native::StreamOps::Where::CommandProcessor);
    if (at == 0) {
        Native::Stats::SiteFallback(site);
        return Native::Gnm::Guest<Native::Gnm::GuestWriteConstRamSpace>(0x866150)(cb, offset, count);
    }
    *body = at;
    Native::Stats::SiteNative(site);
    return at;
}

// Eboot emitter 0x865fa0 (opcode 0x88 WAIT_ON_DE_COUNTER_DIFF, CE stream): arguments (cb in rdi, diff
// in esi, 32-bit; raw bytes: movq %rdi,%rbx, movl %esi,%r14d). Packet: header 0xC0008800, body diff.
// Handler: liverpool.cpp ProcessCeUpdate WaitOnDeCounterDiff, `while ((de - ce) >= diff) YIELD_CE`.
// The op is a native CE wait (EmitWait): ProcessCeUpdate polls the predicate at the NOP and yields
// while it is false, so the predicate is the negation of the handler's loop test, in u32 arithmetic
// (both counters and diff are u32 in liverpool.h / the handler).
extern "C" __attribute__((sysv_abi)) void GnmW11_865fa0(void* cb, unsigned int diff) {
    static const u32 site = Native::Gnm::Site("0x865fa0 W11 WAIT_ON_DE_COUNTER_DIFF");
    const bool ok = Native::Gnm::Enabled(Native::Gnm::Class::Ce) &&
                    Native::StreamOps::EmitWait(reinterpret_cast<u64>(cb), [diff] {
                        const auto& ce = Native::Gnm::Lv().NativeCblock();
                        return static_cast<u32>(ce.de_count - ce.ce_count) < diff;
                    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    using GuestWaitDeDiff = void(__attribute__((sysv_abi)) *)(void*, unsigned int);
    Native::Gnm::Guest<GuestWaitDeDiff>(0x865fa0)(cb, diff);
}

// Eboot emitters 0x86f490 and 0x883f30 (opcode 0x86 WAIT_ON_CE_COUNTER, DE stream): arguments (cb in
// rdi only; raw bytes: movq %rdi,%rbx). Packet: movabs 0x1C0008600 (header 0xC0008600, body 1; the
// handler ignores the body). Handler: liverpool.cpp ProcessGraphics WaitOnCeCounter,
// `while (ce <= de && !ce_task.done) RESUME_GFX(ce_task)`. The op is EmitWaitCe: the graphics parser
// polls the predicate at the NOP and resumes the CE task while it is false, and stops when the CE
// task is done (the same loop exit as the handler).
extern "C" __attribute__((sysv_abi)) void GnmW11_86f490(void* cb) {
    static const u32 site = Native::Gnm::Site("0x86f490 W11 WAIT_ON_CE_COUNTER");
    const bool ok = Native::Gnm::Enabled(Native::Gnm::Class::Ce) &&
                    Native::StreamOps::EmitWaitCe(reinterpret_cast<u64>(cb), [] {
                        const auto& c = Native::Gnm::Lv().NativeCblock();
                        return c.ce_count > c.de_count;
                    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    using GuestVoidCb = void(__attribute__((sysv_abi)) *)(void*);
    Native::Gnm::Guest<GuestVoidCb>(0x86f490)(cb);
}

extern "C" __attribute__((sysv_abi)) void GnmW11_883f30(void* cb) {
    static const u32 site = Native::Gnm::Site("0x883f30 W11 WAIT_ON_CE_COUNTER");
    const bool ok = Native::Gnm::Enabled(Native::Gnm::Class::Ce) &&
                    Native::StreamOps::EmitWaitCe(reinterpret_cast<u64>(cb), [] {
                        const auto& c = Native::Gnm::Lv().NativeCblock();
                        return c.ce_count > c.de_count;
                    });
    if (ok) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    using GuestVoidCb = void(__attribute__((sysv_abi)) *)(void*);
    Native::Gnm::Guest<GuestVoidCb>(0x883f30)(cb);
}

namespace Native::Gnm {

void RegisterW11(Table& t) {
    t.InsertPushMarker = &InsertPushMarkerNative;
    t.InsertPushColorMarker = &InsertPushColorMarkerNative;
    t.InsertPopMarker = &InsertPopMarkerNative;
    t.InsertSetMarker = &InsertSetMarkerNative;
    t.InsertDingDongMarker = &InsertDingDongMarkerNative;
}

} // namespace Native::Gnm
