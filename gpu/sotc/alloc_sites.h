// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ALLOC_SITES=1 (needs SOTC_HOOK_MEMTYPE_PROBE=1). Exact CPU/GPU classification per allocation site.
//
// The game has no flag that says "the GPU writes this buffer": the link between an allocation and the shader that
// writes it is made at run time (descriptor tables built by the engine, shaders in the data files). The port sees
// both ends exactly: every engine allocation goes through sub_12820 (the MEMTYPE_ALLOC wrap gives address, size,
// heap and the caller chain), and every GPU use goes through the port (buffer binds carry the recompiled shader's
// own is_written, image lookups say whether the image is a render target or storage image).
//
// This unit keeps a map of the allocations (an allocation ends at its free, MEMTYPE_FREE at 0x12af0; one replaced by
// a later allocation that overlaps it without a free seen is also finished, and counted as such),
// charges each GPU use to the allocation that contains its base address (and, for written buffer binds, marks the
// allocations the rest of the bound range overlaps, up to 1 MiB, as "overlap"), and charges each TRACK=3 write fault
// to its allocation. Per allocation site (three return addresses and the heap slot) it counts allocations and how
// many of them the GPU wrote, read, or never touched, so a site is GPU-written, GPU-read, CPU-only or mixed.
// A written bind is "own" when it starts at the allocation and fits in it, "foreign" when it starts inside at another
// offset or reaches past the end (the table keeps one foreign offset/size sample per site).
// Every 5 s one log line and the full table in SOTC_ALLOC_SITES_FILE (default user/alloc_sites.tsv). Observe only.
#pragma once

#include <cstdint>
#include <string>

namespace SotcAllocSites {

bool On();

// Size core: a small always-on map start -> size of the live allocations, so an engine free can learn the size of
// the block it frees. Independent of the diagnostic registry above (no NoteAlloc/NoteFree needed for it).
/// True when SOTC_ALLOC_SITES=1 or SOTC_NATIVE_OWNER_DISCARD=1 (read once). The core functions do not check it;
/// callers gate on it so the disabled path costs nothing.
bool CoreOn();
/// Records the allocation starting at addr with the given size. An existing entry at addr is overwritten (a later
/// allocation reused the address, e.g. the ring heap).
void CoreNoteAlloc(uint64_t addr, uint64_t size);
/// Removes the entry starting at addr and returns its size, 0 when there is none (or the entry was dropped).
uint64_t CoreTakeFree(uint64_t addr);
/// Appends " core: entries N, hits H, misses M, dropped D" to out.
void CoreReport(std::string& out);

/// Post phase of sub_12820: the returned block, its heap slot and the caller chain (image offsets, ~0 = none).
/// trace: the index Capture returned for this call (~0u: not traced).
void NoteAlloc(uint64_t addr, uint64_t size, uint32_t slot, uint64_t c1, uint64_t c2, uint64_t c3,
               uint32_t trace = ~0u);

/// SOTC_ALLOC_TRACE (sites "c1:c2,..." in hex, c2 0 = any caller; a built-in list of the mixed sites when unset or
/// "mixed"): pre phase of sub_12820 for a traced site. Keeps the 16 general registers at the call (regs: rax, rbx,
/// rcx, rdx, rsi, rdi, rbp, r8-r15, rsp, the GuestRegs order), 512 bytes of the stack from rsp, and the first 128
/// bytes behind rbx, rsi, rdi, rbp, r12-r15, so the value that tells GPU-written allocations apart can be found
/// offline (tools: the alloc_trace.tsv columns). Returns the record index, ~0u when the site is not traced or the
/// buffer is full (5000 records).
uint32_t Capture(uint64_t c1, uint64_t c2, uint64_t c3, uint64_t size, const uint64_t* regs);
/// Where a free was seen. A block freed through 0x12af0 reaches its heap's free too (the second report finds no live
/// allocation and is counted apart).
enum FreeSource : uint32_t {
    kFreeGeneric = 0, ///< the generic free 0x12af0 (MEMTYPE_FREE)
    kFreeSmall = 1,   ///< the small-object allocator's free 0x17e90 (vtbl+0xa8, MEMTYPE_FREE_SMALL)
    kFreeBpe = 2,     ///< the BPE heap's free 0x493d0 (MEMTYPE_FREE_BPE)
    kFreeSources = 3,
};
/// A free: ends the allocation that starts at addr.
void NoteFree(uint64_t addr, FreeSource source);
/// Pre phase of the realloc 0x12c50 (MEMTYPE_REALLOC): the site and trace record of the live allocation at old
/// (~0u when none), kept by the caller for ReallocPost.
void ReallocPre(uint64_t old_addr, uint32_t* site, uint32_t* trace);
/// Post phase of the realloc: the block now at new_addr (size bytes) belongs to the old allocation's site. The old
/// allocation ends when the block moved (its heap free usually ended it already).
void ReallocPost(uint64_t old_addr, uint64_t new_addr, uint64_t size, uint32_t site, uint32_t trace);
/// A buffer bind (BufferCache::ObtainBuffer). written: the shader writes the binding.
void NoteGpuBuffer(uint64_t addr, uint64_t size, bool written);
/// An image lookup (TextureCache::FindImage). written: a render target, depth or storage image.
void NoteGpuImage(uint64_t addr, uint64_t size, bool written);
/// A write fault on a GPU-written page (SOTC_NATIVE_TRACK=3).
void NoteCpuFault(uint64_t addr);

} // namespace SotcAllocSites
