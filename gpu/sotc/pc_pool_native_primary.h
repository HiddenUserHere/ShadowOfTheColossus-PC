// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native primary-set node hit (pad or split) of the game's pool allocator (pool_alloc_region 0x19e00 ->
// pool_take_free_run 0x1a800 -> pool_node_alloc 0x19660). Companion of pc_pool_native.h, which covers the exact
// hit (no node taken). This file covers the Path::PrimaryHitNode hits only. Its node pops go through
// SotcPcPoolNodes::PopNode (pc_pool_native_nodes.h), so a pop that takes the pool count from 3 to 2 refills natively.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_native.h"

namespace SotcPcPoolNative {

/// Applies a primary-set (S1 for kind 1, S2 otherwise) hit that pads or splits its free run, in the game's order,
/// through mem. pred must be Predicted with path PrimaryHitNode; size, align and kind are the wrapper's arguments
/// (as for ApplyExactHit), obj is rdi (the owner family, or 0). overhead_addr: SotcPcPoolNodes::OverheadAddr(image base),
/// needed when a pop refills (0 refuses such a call).
///
/// Returns the block node (rax of pool_alloc_region), or 0 when refused. A refusal writes nothing and is one of:
///   - the pool is not this path (exact hit, flexible set, a start that is not pred.va, a guard that does not hold);
///   - a node pop cannot run (empty pool, list shape not as the pop expects, a refill that is not an exact hit, or a
///     refill that would take the call's own run);
///   - a guest read or write is unmapped.
/// The whole sequence runs on an Overlay (pc_pool_native.h): mem is written only at the commit, after every step has
/// passed. A failed base write during the commit returns 0 with the earlier writes in place (the caller must treat
/// the state as unknown), as in ApplyExactHit.
///
/// A pop that refills runs the nested exact hit on the overlay's state at that point (after the call's writes that
/// precede the pop: none for the first pop, the tail W1-W15 for a split's second pop). The game's order is: pop,
/// overhead add, nested exact hit, carve; the writer does the nested hit and the pop stores in the other order (the
/// final image is the same: they touch disjoint memory), see SotcPcPoolNodes::PopNode.
///
/// Stores, eboot addresses (pool_alloc_region 0x19e00, pool_take_free_run 0x1a800, pool_node_alloc 0x19660). S is
/// the set (root, root+0xa0 or root+0x140), P the pop, E = node+0x20 the free-list element:
///   Pop of the node pool (0x19660, 0x1968b-0x196b7), per pop, before any other write of the call that uses it:
///   P1  [N+0x38]  := 0                      0x1968f
///   P2  [N+0x20]  := 0                      0x19697
///   P3  [prev+0x10] := next                 0x196a7  prev = [N+0x28], next = [N+0x30]; the head is root+0x1f0 when prev is the sentinel root+0x1e0
///   P4  [next+8]  := prev                   0x196af
///   P5  [N+0x30]  := E                      0x196b3
///   P6  [N+0x28]  := E                      0x196b7
///   P7  [root+0x200] := count - 1 (i32)     0x196c3
///   Pad path only (the first pop is the pad node's block, 0x1a8b1), in this order:
///   Q1  [M+0x68]  := [r+0x68] (byte)        0x1a8cb  M = the popped node, r = the free-run node
///   Q2  [M+0x58]  := [r+0x58] - pad         0x1a8ce
///   Q3  [M+0x60]  := [r+0x60] + pad         0x1a8d2
///   Q4  [M+0x69]  := 0 (byte)               0x1a8d6
///   Q5  [M+0x40]  := 0                      0x1a8da
///   Q6  [M+0x48]  := -1 (i32)               0x1a8e2
///   Q7  [M+0x50]  := [r+0x58] - pad         0x1a8f1
///   Q8  [r+0x58]  := pad                    0x1a8f5
///   Q9  [r+0x50]  := pad                    0x1a8fa
///   Insert M after r in the free-VA list (0x1a902-0x1a961), then the exact tail removes it again:
///   I1  [M+0x20]  := M                      0x1a917 / 0x1a941
///   I2  [M+0x30]  := E(next) or S+0x28      0x1a91b / 0x1a945
///   I3  [M+0x28]  := [next+0x28] or [S+0x30] (the old tail)   0x1a923 / 0x1a94d
///   I4  [next+0x28] := E(M)  or  [S+0x30] := E(M)   0x1a927 / 0x1a951
///   I5  [prev+0x10] := E(M)                 0x1a92f / 0x1a959   prev = [M+0x28]
///   I6  [S+0x48]  += 1                      0x1a933 / 0x1a95d
///   I7  [M+0x38]  := S+0x28                 0x1a961
///   Exact tail on the block B (0x1a974-0x1a9fd), B = M (pad path) or the free-run node (no pad):
///   W1  [prev+0x10] := next                 0x1a983   prev = [B+0x28], next = [B+0x30] (read before W1)
///   W2  [next+8]  := prev                   0x1a98b
///   W3  [B+0x30]  := E                      0x1a98f
///   W4  [S+0x48]  -= 1 (i32)                0x1a993
///   W5  [E]       := B                      0x1a997
///   W6  [E+8]     := S                      0x1a99b
///   W7  [E+0x10]  := old region head        0x1a9a3   old head = [S+0x10]
///   W8  [S+0x10]  := E                      0x1a9a7
///   W9  [old head+8] := E                   0x1a9af
///   W10 [S+0x20]  += 1 (i32)                0x1a9b3
///   W11 [B+0x38]  := S                      0x1a9b7
///   W12 [S+0x90]  += size                   0x1a9c5
///   W13 [S+0x98]  := [S+0x90] (if above the peak)   0x1a9d5
///   W14 [S+0x80]  += size                   0x1a9e6
///   W15 [S+0x88]  := [S+0x80] (if above the peak)   0x1a9f6
///   Split of B (backed [B+0x50] > size, 0x1aa01), with the remainder node N2 from a second pop (0x1aa0f):
///   T1  [N2+0x68] := [B+0x68] (byte)        0x1aa32
///   T2  [N2+0x58] := [B+0x58] - size        0x1aa35
///   T3  [N2+0x60] := [B+0x60] + size        0x1aa39
///   T4  [N2+0x69] := 0 (byte)               0x1aa3d
///   T5  [N2+0x40] := 0                      0x1aa41
///   T6  [N2+0x48] := -1 (i32)               0x1aa49
///   T7  [N2+0x50] := [B+0x58] - size        0x1aa61
///   T8  [B+0x58]  := size                   0x1aa65
///   T9  [B+0x50]  := size                   0x1aa69
///   Insert N2 before the next free-run node r' (0x1aa6d-0x1aa9e; r' = node of [B+0x30] read after W1-W15; the
///   tail append when r' is null, 0x1aaae-0x1aad2):
///   J1  [N2+0x20] := N2                     0x1aa74 / 0x1aaae
///   J2  [N2+0x30] := E(r') or S+0x28        0x1aa78 / 0x1aab6
///   J3  [N2+0x28] := [r'+0x28] or [S+0x30]  0x1aa81 / 0x1aabe
///   J4  [r'+0x28] := E(N2) or [S+0x30] := E(N2)   0x1aa85 / 0x1aac2
///   J5  [prev+0x10] := E(N2)                0x1aa8e / 0x1aaca   prev = [N2+0x28]
///   J6  [S+0x48]  += 1                      0x1aa92 / 0x1aace
///   J7  [N2+0x38] := S+0x28                 0x1aa9a / 0x1aad2
///   Node fields of the block B (0x1aada-0x1aaf3), B = the block after any split:
///   W16 [B+0x68]  := [S+0x78] (byte)        0x1aae4
///   W17 [B+0x58]  := size                   0x1aae7
///   W18 [B+0x69]  := 1                      0x1aaea
///   W19 [B+0x40]  := obj                    0x1aaef
///   W20 [B+0x48]  := -1 (i32)               0x1aaf3
///   Owner tags (0x1ab04-0x1abc6): the byte tag = [obj+0x1c] (0 when obj is 0) over the MiB units of B,
///   W21 [root+0x111908 + (B >> 20) ...] := tag (count bytes, count = last - first + 1)
///   W22 [S+0x70]  -= pages (i32), the last store of the path (0x1abd2)
///   Node pops (both the pad's pop and the split's): SotcPcPoolNodes::PopNode on the overlay, with the refill (count
///   3 -> 2) planned on the overlay state of that moment; the second pop of a pad-and-split at count 4 refills too,
///   and the header of PredictNodeCall refuses that case in the model (not modelled), so the game runs it.
uint64_t ApplyPrimaryNodeHit(GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
                             const SotcPcPool::Model::Prediction& pred, uint64_t size, uint32_t align, int kind,
                             uint64_t obj, uint64_t overhead_addr = 0);

/// The (address, size) of every Write the last ApplyPrimaryNodeHit call issued, in order (empty after a refusal).
std::vector<std::pair<uint64_t, size_t>> PrimaryLastWriteSet();

} // namespace SotcPcPoolNative
