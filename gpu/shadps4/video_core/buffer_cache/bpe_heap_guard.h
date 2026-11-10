// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "common/types.h"

namespace Core {
class MemoryManager;
}

namespace VideoCore {

/**
 * Per-game guard for Shadow of the Colossus (GPU.bpe_heap_guard_address, 0 = off)
 *
 * The BPE heap stores a 16-byte header before each block and tree links inside free blocks
 * Stale compute descriptors can write to freed blocks, and readbacks can then overwrite that metadata
 * This caused guest access violations at eboot+0x48ac5/0x48f3d/0x490a0/0x49476
 *
 * Readbacks preserve the CPU's allocator metadata, which is never valid GPU output
 * The guard reads it through physical backing to bypass tracking protection
 */
class BpeHeapGuard {
public:
    BpeHeapGuard(Core::MemoryManager* memory, VAddr heap);

    bool IsValid() const {
        return valid;
    }

    /// Writes `size` bytes of `data` to guest `addr`, skipping allocator metadata. Returns the number of
    /// skipped bytes whose GPU value differed from the CPU value (0 when harmless)
    u64 Write(VAddr addr, const u8* data, u64 size);

    /// sotc: from the lowest to the highest free block's metadata found by the walk ({0, 0}: none).
    /// The heaps a world creates grow in 1 MiB pieces spread over direct memory, so this, not
    /// [heap+0x28] (a pointer all heaps share), tells where their metadata is.
    std::pair<VAddr, VAddr> FreeExtent() const {
        if (free_spans.empty()) {
            return {0, 0};
        }
        VAddr hi = 0;
        for (const auto& span : free_spans) {
            hi = std::max(hi, span.second);
        }
        return {free_spans.front().first, hi};
    }

    /// True when [addr, end) overlaps allocator metadata
    bool HasMetadata(VAddr addr, VAddr end) {
        return !CollectSpans(addr, end).empty();
    }

    /// sotc (B2): one piece of allocator metadata overlapping a write: [lo, hi) is the header of `block`,
    /// and for a free block also its tree links. `freed` says whether the block is free now (live header).
    struct MetaHit {
        VAddr lo;
        VAddr hi;
        VAddr block;
        bool freed;
    };

    /// sotc (B2): the metadata overlapping [addr, end), sorted by lo. Tree links of a block in use are user
    /// data, so only its header is reported for it. Headers and the free list are read live.
    void Hits(VAddr addr, VAddr end, std::vector<MetaHit>& hits);

    /// The in-use block whose user area holds `addr` (its header address and the next block's), found by
    /// following the physical chain from the closest free block below it
    bool FindUsedBlock(VAddr addr, VAddr& block, VAddr& next, int* why = nullptr) const;

private:
    using Span = std::pair<VAddr, VAddr>;

    /// The spans of allocator metadata overlapping [addr, end), sorted
    std::vector<Span> CollectSpans(VAddr addr, VAddr end);

    bool Read(VAddr addr, void* out, u64 size) const;
    bool IsHeapPointer(VAddr addr) const;
    void AddFreeBlock(VAddr block);
    size_t CollectFreeTree(); // returns the blocks added before duplicates were removed
    void CollectHeaders(VAddr lo, VAddr hi, std::vector<Span>& spans) const;
    bool NextBlock(VAddr block, VAddr& next) const;

    Core::MemoryManager* memory;
    VAddr heap;
    bool valid = false;
    std::vector<VAddr> free_blocks; // sorted block starts
    std::vector<Span> free_spans;   // sorted, header + tree links of every free block
    // Every backing run resolved during this guard's walk, sorted by start. The free-tree walk reads
    // thousands of nodes per readback; resolving each one through the VMA map was ~25 ms per SotC frame, and
    // the tree spreads over many physical areas
    struct Run {
        VAddr start;
        u64 size;
        const u8* ptr;
    };
    mutable std::vector<Run> runs;
    /// sotc: index of the run the last Read hit (the walk reads node after node in one run)
    mutable size_t last_run = 0;
    /// sotc: starts loading the cache line of addr when a known run holds it (CollectFreeTree)
    void Prefetch(VAddr addr) const;
};

/**
 * sotc: in-use heap blocks that readbacks write into. A write inside the user area of a block that is
 * still in use holds no allocator metadata (headers sit between blocks, tree links only in free
 * ones), so it needs no guard, whose build walks the whole free tree. The heap changes every frame,
 * which rebuilt the guard about once per frame (~6% of the draw thread in the Shrine). Each hit
 * re-reads the block's header and the next block's back link, so a block freed since is not trusted.
 */
class BpeUsedBlocks {
public:
    explicit BpeUsedBlocks(Core::MemoryManager* memory_) : memory{memory_} {}

    /// True when [addr, end) lies in the user area of a remembered block that is still in use
    /// sotc: pieces (a checked piece list, or null): the fence's tail is then trusted only when
    /// it is not a piece (contiguous pieces follow each other right after a piece's fence)
    bool Covers(VAddr heap, VAddr addr, VAddr end, int* why = nullptr,
                const class BpeHeapPieces* pieces = nullptr);

    /// sotc: true (and remembered) when addr is the user pointer of an in-use block that holds
    /// [addr, end): its header right before addr, checked as Covers checks a remembered one
    bool TryUserPointer(VAddr heap, VAddr addr, VAddr end);

    /// sotc: true (and remembered) when [addr, end) lies in the user area of an in-use block of the
    /// heap piece at `piece`, found by following the physical chain from the piece's first block
    /// (or from a remembered block of the piece below addr). Blocks are checked as Covers checks
    /// a remembered one. The chain from the closest free block, the guard's start, ends at that
    /// block's piece: writes into a piece with no free block below them built the guard each time.
    bool FindInPiece(VAddr heap, VAddr piece, VAddr piece_end, VAddr addr, VAddr end);

    void Add(VAddr block, VAddr next);

    /// The in-use, size-0 block that ends the chain: the heap's tail after it holds no metadata
    void SetFence(VAddr block) {
        fence = block;
    }

    size_t Size() const {
        return blocks.size();
    }

private:
    bool StillUsed(VAddr heap, VAddr block, VAddr next) const;

    Core::MemoryManager* memory;
    std::vector<std::pair<VAddr, VAddr>> blocks; // sorted [header, next header)
    VAddr fence = 0;
};

/**
 * sotc (B2): GPU writes into guest memory in native mode (SOTC_NATIVE=1, ring 8). Such writes land in
 * the guest's own bytes, with no readback and no tracker, so BpeHeapGuard (which runs on readbacks only)
 * never sees them. This module checks every in-place write against the allocator metadata of the BPE
 * heaps and counts the overlaps (per 5 s line and the last ones, for the fault handler). SOTC_BPE_FIX:
 *   0 (default): nothing; 2: count only; 1: count, and clamp the written bindings (Check with clamp) at
 *   their first metadata byte, so a stale descriptor cannot write the heap's headers and free links.
 */
namespace BpeNative {

/// What wrote the range (the counters and the fault report name it)
enum class Kind : u8 {
    Bind,          // a written V# bound in place (Rasterizer::BindBuffers)
    GpuFill,       // Rasterizer::FillBuffer, the range in the imported arena (vkCmdFillBuffer)
    GpuCopy,       // Rasterizer::CopyBuffer, destination in the imported arena
    CpuFill,       // Rasterizer::FillBuffer, host std::fill into guest memory
    CpuCopy,       // Rasterizer::CopyBuffer, host memcpy into guest memory
    HleCopy,       // ExecuteShaderHLE copy shader: destination pieces
    HleTable,      // ExecuteShaderHLE replicate fill: host writes of the table
    ImageDownload, // TextureCache::WriteBackSkippingStacks: a GPU-written image
    CpWrite,       // Liverpool: fence values and end-of-pipe labels
    Count
};

/// SOTC_BPE_FIX as read once: 0, 1 or 2 (any other value is 0)
int Mode();

/// Checks the in-place write [addr, addr + size) of kind `kind` (detail: e.g. the shader hash) against the
/// allocator metadata of the BPE heaps. Returns the size to use: with clamp and SOTC_BPE_FIX=1, the part
/// before the first metadata byte (0 when it starts in metadata); otherwise size. `in_place` is false for
/// writes that land in the device arena and reach guest memory later through a readback (guarded there).
u64 Check(VAddr addr, u64 size, Kind kind, u64 detail, bool in_place, bool clamp = false);

/// Called by the native stray-fault path: prints the most recent metadata overlap within 1 MiB of the fault
void ReportNearFault(VAddr fault_addr);

} // namespace BpeNative

/**
 * sotc: the pieces a BPE heap is made of. A heap grows piece by piece over direct memory, and its
 * pieces are spread among other GPU buffers: the heap's guarded range (its free blocks' extent)
 * spans hundreds of MiB that are not the heap's, and every readback there built the guard again
 * (a walk of the whole free tree, ~1 ms) only to find no metadata. Each piece starts with a
 * sentinel block (word 0: the previous piece, word 1: size 0, flags 3) and ends with an in-use
 * block of size 0. The newest piece is at heap+0xb8; heap+0x58 counts the pieces' MiB (a piece
 * is 1 MiB or more), heap+0xd0 is the 1 MiB unit and heap+0x30 the heap size: the pieces' MiB,
 * plus, for the heap the game creates at boot, a first contiguous area (heap+0x28, also the last
 * node of the list). Allocator metadata lies only inside those. A piece's end is not stored, so
 * each piece is taken as long as the MiB left over allow (never past the next piece).
 */
class BpeHeapPieces {
public:
    explicit BpeHeapPieces(Core::MemoryManager* memory_) : memory{memory_} {}

    /// Re-reads the piece list when the heap's newest piece, MiB count or size changed. False when
    /// the list does not check out (the caller then guards as before)
    bool Refresh(VAddr heap);

    /// True when [addr, end) overlaps the heap's memory (valid only after Refresh returned true)
    bool Overlaps(VAddr addr, VAddr end) const;

    /// The piece holding addr as [start, end) (start 0: none, or the boot heap's first area)
    std::pair<VAddr, VAddr> PieceOf(VAddr addr) const;

private:
    /// True when the piece at `piece` ends at `end` (its last block found right before end)
    bool IsPieceEnd(VAddr piece, VAddr end) const;

    Core::MemoryManager* memory;
    VAddr head = 0;
    u64 count = 0;
    u64 unit = 0;
    u64 heap_size = 0;
    VAddr initial_lo = 0, initial_hi = 0; // the boot heap's first, contiguous area
    bool valid = false;
    std::vector<std::pair<VAddr, VAddr>> pieces; // sorted, [start, end) upper bounds
};

} // namespace VideoCore
