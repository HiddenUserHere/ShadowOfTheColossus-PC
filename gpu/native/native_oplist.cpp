// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. See native_oplist.h. Owner: H04 (N2-94 plan, W3 + W2 pieces).
//
// Coverage table (item | done? | evidence):
//   BuildOpList walks type-3 packets (header>>30 == 3, opcode (h>>8)&0xff must be Nop 0x10,
//     body = ((h>>16)&0x3fff)+1 dwords, packet = body+1; count 0x3fff fails, fix round 1) | yes | this file
//   type-2 filler and the one-dword NOP 0xffff1000 skipped                | yes | this file
//   any non-NOP packet or truncated packet fails (out cleared, false)    | yes | this file
//   payload 0x68750f01 -> Op(id=body[1], body=&body[2], body_dwords=body-2) | yes | fails if body<2
//   payload 0x68750f02 -> Wait(id=body[1]); 0x68750f03 -> WaitCe(id=body[1]) | yes | fails if body<2
//   payload 0x68750f04 -> Ib(addr=body[1]|body[2]<<32, size=body[3])    | yes | fails if body<4
//   every other NOP payload skipped (flip markers, 0x68750013, padding) | yes | this file
//   CountOpList per-kind relaxed counters (local; no reader, see note)  | yes | this file
//   Thread safety: no shared state except the relaxed counters          | yes | this file
//
// Constants: PM4CmdNop::PayloadType in gpu/shadps4/video_core/amdgpu/pm4_cmds.h:248-258
// (SotcNativeOp/Wait/WaitCe/Ib). Duplicated here so this file does not pull pm4_cmds.h.
//
// Evidence on the guest side (REA: eboot_pe_fde.exe, Ghidra summary views only; the complete
// bodies were read from out/re/decomp and raw bytes at file offset VA + 0x4000):
//   0x885220 padding writer: header 0xc0001000 | (N-2) << 16 with payload dword 0, N == 1 writes
//   0xffff1000; both are skipped here. 0x8850c0 / 0x885110 only touch the 0xc0031000 placeholder
//   NOP whose payload is 0x68750013; skipped here as well.

#include <atomic>
#include <array>

#include "native/native_oplist.h"

namespace Native {

namespace {

// PM4 opcode Nop (type-3 header bits 8..15).
constexpr u32 kOpcodeNop = 0x10u;
// Type-3 header of the one-dword NOP filler (count 0x3fff, opcode 0x10) written by 0x885220 for N == 1.
constexpr u32 kOneDwordNop = 0xffff1000u;

// NOP payload ids (PM4CmdNop::PayloadType, pm4_cmds.h:248-258).
constexpr u32 kPayloadOp = 0x68750f01u;
constexpr u32 kPayloadWait = 0x68750f02u;
constexpr u32 kPayloadWaitCe = 0x68750f03u;
constexpr u32 kPayloadIb = 0x68750f04u;

// Relaxed per-kind counters of CountOpList (no reader yet: the SOTC NATIVE STATS line lives in
// native_stats.cpp and H04 does not own it).
std::array<std::atomic<u64>, 4> g_kind_counts{};

} // namespace

bool BuildOpList(std::span<const u32> cb, OpList& out) {
    out.Clear();
    const u32 dwords = static_cast<u32>(cb.size());
    if (cb.data() == nullptr || dwords == 0) {
        return false;
    }
    out.dwords = dwords;

    u32 i = 0;
    while (i < dwords) {
        const u32 header = cb[i];
        const u32 type = header >> 30;
        if (type == 2 || header == kOneDwordNop) {
            ++i;
            continue;
        }
        if (type != 3) {
            out.Clear();
            return false;
        }
        // sotc: H04 fix round 1. The PM4 parser computes NumWords() = (count + 1) & 0x3fff, which is 0
        // for count 0x3fff, and spans one dword (liverpool.cpp NextPacket). This scan does not model that
        // case, so it fails here and the buffer takes the PM4 path (the old scan also rejected it).
        const u32 count = (header >> 16) & 0x3fffu;
        if (count == 0x3fffu) {
            out.Clear();
            return false;
        }
        const u32 body = count + 1;
        // The packet spans body + 1 dwords; anything past the end is a truncated packet.
        if (body + 1 > dwords - i) {
            out.Clear();
            return false;
        }
        const u32 opcode = (header >> 8) & 0xffu;
        if (opcode != kOpcodeNop) {
            out.Clear();
            return false;
        }

        const u32* b = &cb[i + 1];
        switch (b[0]) {
        case kPayloadOp:
            if (body < 2) {
                out.Clear();
                return false;
            }
            out.items.push_back(OpItem{
                .kind = OpKind::Op,
                .id = b[1],
                .ib_addr = 0,
                .ib_size = 0,
                .body = &b[2],
                .body_dwords = body - 2,
            });
            break;
        case kPayloadWait:
            if (body < 2) {
                out.Clear();
                return false;
            }
            out.items.push_back(OpItem{.kind = OpKind::Wait, .id = b[1]});
            break;
        case kPayloadWaitCe:
            if (body < 2) {
                out.Clear();
                return false;
            }
            out.items.push_back(OpItem{.kind = OpKind::WaitCe, .id = b[1]});
            break;
        case kPayloadIb:
            if (body < 3) {
                out.Clear();
                return false;
            }
            // sotc: 4-dword native IB (N2-108): addr lo, addr hi 8 bits | size << 8.
            out.items.push_back(OpItem{
                .kind = OpKind::Ib,
                .id = 0,
                .ib_addr = static_cast<u64>(b[1]) | (static_cast<u64>(b[2] & 0xffu) << 32),
                .ib_size = b[2] >> 8,
            });
            break;
        default:
            // Flip markers, the 0x68750013 placeholder, label slots and padding: not ops.
            break;
        }
        if (!out.items.empty() && out.items.back().dw_end == 0) {
            out.items.back().dw_end = i + body + 1; // sotc: N2-94 integration (compute read pointer)
        }
        i += body + 1;
    }
    return true;
}

void CountOpList(const OpList& list) {
    for (const OpItem& item : list.items) {
        g_kind_counts[static_cast<size_t>(item.kind)].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Native
