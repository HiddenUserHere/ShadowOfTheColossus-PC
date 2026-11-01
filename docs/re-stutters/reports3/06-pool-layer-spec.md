# 06 Pool layer spec: eboot 0x18aa0-0x1b0e0

Read-only static analysis of `out/eboot.elf`. Addresses are eboot VAs. [O addr] observed, [I] inferred,
[U] unknown. `root` is the global pointer at 0x2dfca78.

## 0. Corrections to reports 04 and 06

- The 128 KiB phys-0 block is the GNM tessellation factor ring, not a pool [O 0x18aa0].
- Pool id 1 (Onion) and 2 (Garlic) are pool ids, not PS4 memory types. The PS4 types (0 and 3) are the
  5th argument of sceKernelAllocateDirectMemory [O 0x18aa0].
- Kind 1 is not Onion-only. It tries the flexible set first, then Onion, then spills to Garlic. Kind 0
  and 2 go to Garlic, then spill to Onion [O 0x19e00 top, 0x1a181-0x1a27c].
- mem_allocator_alloc (0x479a0) forwards rdx (alignment) and rcx (kind) to the pool. The probe comment
  "rdx and rcx are not read" is wrong. The direct call sites I inspected set edx=0x10000 and
  ecx=[obj+0x18], except two that pass ecx=1 (0x14fa9, 0x16017) [O]. The site at 0x9b778 is not checked.

## 1. Data structures

### 1.1 Root and pool sets

`root` is a bookkeeping block at the top of direct memory (auto VA). Three pool sets share one layout,
at S = root, root+0xa0, root+0x140 [O 0x18aa0, 0x197d0]:

| Set | Pool id (S+0x78) | Backing | Size |
|---|---|---|---|
| S0 = root | 0 | flexible, VA-only; base root+0x20d908 | min(avail_flex - 25 MiB, 0x1964 pages) |
| S1 = root+0xa0 | 1 | Onion, phys from 0x100000, type 0 | 1 GiB (S1+0x70 = 0x4000 pages) |
| S2 = root+0x140 | 2 | Garlic, phys from 0x40100000, type 3 | avail - root - 1 GiB |

Set fields (observed uses):
- +0x08 region-list head (element pointer); +0x20 i32 region count.
- +0x28 free-VA list sentinel; +0x38 head pointer; +0x48 i32 free-VA node count.
- +0x50 bitmap pointer (S0 has none); +0x58 i32 bitmap words; +0x60 u32 page cap.
- +0x70 i32 free VA pages (sum of free-VA runs) [I].
- +0x74 i32 cached free phys pages [I]; decremented by each allocated run.
- +0x78 i32 pool id.
- +0x80/+0x88 allocated bytes, current and peak. +0x90/+0x98 backed bytes, current and peak.

### 1.2 Bitmaps and side tables

- Bitmaps: S1 at root+0xf0 (words at root+0xf8, cap root+0x100); S2 at root+0x190 (words at root+0x198,
  cap root+0x1a0) [O 0x18aa0]. Bit i = phys 0x100000 + i*0x10000. Bit 1 = free phys page. Allocation
  clears bits [O 0x19e00 `&= ~`]. Free and reserve-map set bits [O 0x1abf0 `|=`]. Both bitmaps cover all
  available pages, but each set only sets bits for its own pages [I].
- Batch queue: count i32 at root+0x2102f0; 64 entries of 32 bytes from root+0x2102f8. The queue ends at
  root+0x210af8, where bitmap 1 starts [O 0x18aa0, 0x19e00].
- Owner-tag tables: each allocation writes family byte +0x1c per covered unit, at root+0x20d918 (flexible,
  64 KiB units) or root+0x111908 (1 MiB units) [O 0x19e00]. No reader found [U].

### 1.3 Node (0x70 bytes)

Nodes come from 64 KiB blocks carved by pool_node_alloc (0x19660). Free nodes: sentinel root+0x1e0, head
root+0x1f0, count root+0x200. When the count reaches 2 the pool takes another 64 KiB block (kind 1,
align 8) and the global DAT_02dfca60 (bookkeeping overhead) grows by 64 KiB [O 0x19660].

- +0x00..+0x18: family live-list links (self, family+0x38, next, back) [O 0x479a0].
- +0x20..+0x38: set-list element. +0x20 points back to the node; +0x28 prev; +0x30 next; +0x38 owner list.
- +0x40 owner family (0 internal); +0x48 i32 = -1.
- +0x50 u64 backed bytes. +0x58 u64 VA size. +0x60 u64 VA base.
- +0x68 u8 pool id (copy of S+0x78). +0x69 u8 in-use flag (1 allocated, 0 free VA run).

### 1.4 Family object (obj)

+0x9 enabled byte; +0x10 name; +0x18 i32 kind; +0x1c owner-tag byte; +0x20 family; +0x30 u64 bytes in
use; +0x38 live-list sentinel; +0x48 live-list head; +0x58 i32 live count [O 0x479a0, 0x47620, 0x47a10].

### 1.5 Lock

Global spin lock at 0x2dfc8a0: `lock cmpxchg` 0 -> -1, `pause` between tries, sceKernelUsleep(1) after
10000 misses [O 0x19b25-0x19b4f]. Release: store 0. Taken by 0x19ae0, 0x1a660, 0x197d0, 0x1b0e0 [O]. The
helpers assume it is held [I].

## 2. Algorithms

### 2.1 Entry points

pool_alloc_region(obj, va_hint, node, size, mode, align, kind) at 0x19e00. Align is rounded up to 64 KiB.
- mode 0: normal allocation from the free-VA lists.
- mode 1: fresh VA from sceKernelReserveVirtualRange(size, max(1 MiB, align)), new node [O 0x19f8a].
- mode 2: release, returns the VA to reserve via vmem_query_reserve_map flag 0 [O 0x1a16b].
- A given node means reuse (0x47a10 path) [O].

### 2.2 Alloc (mode 0)

1. Primary set P = S1 if kind==1, else S2: `((kind!=1)+1)*0xa0` [O 0x19e8c]. Fallback F = the other set
   [O 0x1a187-0x1a1aa].
2. If kind==1: first-fit in S0's free-VA list (0x1a800). A hit returns at once, with no batch op [O].
3. Early reject when (S1+S2 free VA + free phys pages) < request [O 0x19e00 top; meaning I].
4. Take a free-VA run from P (0x1a800): first node with size >= request; alignment pad stays free [O].
   The list is address-ordered, so first-fit by address [I].
5. Page loop, until the request is covered:
   a. If P.74 == 0: pick a free-VA node (best fit on node+0x50 in the fallback branch [I]) and call
      vmem_query_reserve_map(P, node, base, size, 1). That marks the node's mapped pages free in the
      bitmap, adds to P.74, queues an UNMAP of the VA, and consumes the VA from the node front [O 0x1abf0].
   b. Scan P's bitmap: first nonzero word, lowest set bit. The run extends over set bits, capped by the
      remaining pages and P.60 [O 0x1a2f8-0x1a3ff]. If no bit is set, see 2.5.
   c. Clear the run's bits and do P.74 -= run. Queue op 0 (VA = cursor, phys = 0x100000 + idx*0x10000,
      length = run bytes, prot 0x33). If the block was taken from F, also queue op 4 with type 3 when F
      is S1 and type 0 when F is S2 [O 0x1a500-0x1a5e6, local_4c].
   d. node+0x50 += len; P.90 += len; cursor += len.
6. Flush (vmem_batch_map) before return [O 0x1a62d].
7. New nodes join P's region list (P.20++, P.80 += size) [O].

### 2.3 Free (0x1b0e0)

1. Unlink from the region list; P.20--; P.80 -= size; P.90 -= backed; P.70 += pages [O].
2. Pool id != 0: call vmem_query_reserve_map(P, node, base, size, 1). That marks bits free and queues
   UNMAP. Then it consumes the node's VA, and the node goes back to the node pool if it is exhausted.
   Frees of Onion and Garlic blocks are immediate. Phys coalescing is implicit in the bitmap [O].
3. Pool id 0 (flexible): merge into S0's address-ordered free-VA list with neighbour coalescing [O].
4. [U] Whether the VA of step 2 returns to any free list. Section 6, item 1.

### 2.4 BatchMap op encoding

The 32-byte entry matches the port's BatchEntry (`src/runtime_memory.c:66`):

| Offset | Field |
|---|---|
| +0x00 | u64 VA |
| +0x08 | u64 phys offset (op 0), else 0 |
| +0x10 | u64 length |
| +0x18 | u8 prot (0x33 map, 0 unmap) |
| +0x19 | u8 type (0 for normal ops; 3 or 0 for op 4) |
| +0x1c | u32 operation: 0 map direct, 1 unmap, 4 protect with type |

The game uses only ops 0, 1 and 4 here [O]; the port's batch_map2 (`src/runtime_memory.c:1281`) also
implements 2 and 3.

Flush (0x1af80) calls sceKernelBatchMap on the pending entries. On error it skips the failing entry and
continues, returns the last error, and resets the count [O]. A full-queue flush that fails drops the
pending op [O 0x19e00 at 0x1a500, 0x1abf0 at 0x1ae07].

### 2.5 POOL_FIX (eboot 0x1a321, 15 bytes)

Original `eb 34` + padding is `jmp 0x1a357`, taken when the bit scan finds no nonzero word. r12 stays -1,
so r8 = r12>>6 is huge, and `or rax, [r9+r8*8]` at 0x1a371 reads about 2^61 bytes past the bitmap [O]
(the note at `src/runtime_memory.c:29`).

Patched bytes (`scripts/sotc_modes.py:88`): `41 c7 46 74 00 00 00 00` (mov dword [r14+0x74], 0, so
P.74 := 0); `31 c0` (xor eax, eax); `e9 c1 fe ff ff` (jmp 0x1a1f1) [O bytes match].

Effect [I]: the cached count is zeroed and control re-enters at 0x1a1f1, which takes the free-VA node path
(0x1a205), maps that node through vmem_query_reserve_map, and rescans.

Hazard [I]: 0x1a1f1 skips the empty-list check at 0x1a1e4. With an empty free-VA list and rax = 0 on
entry, control reaches 0x1a2c0 with rax = 0 and reads [0+0x58]. A replacement must keep the guard.

Root cause (P.74 > 0 with no free bit) is [U].

## 3. Physical and VA placement

- Root sits at the top of phys, size ((pages+63)>>6)*0x10 + 0x220af7 rounded to 64 KiB [O 0x18aa0].
- Onion: phys 0x100000, 1 GiB, type 0, auto VA, 1 MiB alignment [O 0x18aa0].
- Garlic: phys 0x40100000, type 3, auto VA. Size = avail - root - 1 GiB [O 0x18aa0].
- Flexible: ReserveVirtualRange at root+0x20d908 (1 MiB alignment), then MapFlexibleMemory with prot 0x33
  and flags 0x10. Size is min(avail_flex - 0x1900000, 0x1964*64 KiB) [O 0x197d0]. The port backs it with
  `flex_bitmap` (`src/runtime_memory.c:335`).
- VA and phys are independent: the set gives the VA run, the bitmap gives phys, run by run.

## 4. Thread safety and callers

- Entry points take the global lock. A flush runs port sceKernelBatchMap under the game's lock; the port's
  SOTC_BATCH_SPLIT releases it per entry (`src/runtime_memory.c:1284`).
- mem_allocator_alloc (0x479a0) has 9 callers: sub_14e80, sub_15ee0, sub_17d20, sub_187e0, sub_48910,
  BpeHeap_Free, sub_49200, sub_9aa10 and render_targets_allocate (0x9b778, size 0x14280000 or 0x23d10000).
  Most pass ecx=[obj+0x18]. The sites at 0x14fa9 and 0x16017 pass ecx=1 [O].
- sub_55fd30 calls 0x19ae0 directly: NULL owner, 100 MiB, align 512 MiB, kind 1 [O].
- mem_allocator_alloc_reuse (0x47a10) calls 0x1a660 only from sub_8d770, which has no static caller [U].
- pool_free_block (0x1b0e0) is called from mem_allocator_free_all (0x47620), sub_476f0 and sub_47740.
  free_all unlinks each node from the family list first [O]. Per-frame frequency is [U].

## 5. Contract

A replacement must keep these identical:

1. Pool order: kind 1 tries flexible, Onion, Garlic; kinds 0 and 2 try Garlic, Onion. Op 4 type as in 2.2.
2. Granularity: 64 KiB phys pages, phys base 0x100000, bit 1 = free.
3. Returned block: VA = node+0x60, size = node+0x58, a multiple of 64 KiB, flushed before return.
4. Op order: unmap before a remap of a reused VA; op 4 after op 0 for a spill block.
5. Family live list, count (+0x58) and bytes (+0x30).
6. Serialization by the global lock.
7. POOL_FIX behaviour: never read past the bitmap, zero the cached count, resume through a VA node, and
   keep the empty-list guard.
8. Owner-tag writes (readers unknown): keep them until a reader is found.

Unique signatures (`eboot_re.py sig`, wildcards are rel32/RIP displacements):
- 0x19ae0 pool_alloc_block_wrapper: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ?? 89 55 c0 89`
- 0x19e00 pool_alloc_region: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ?? 44 89 cb 48`
- 0x1a660 pool_alloc_region_aligned: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 38 48 8b 05 ?? ?? ?? ?? 44 89 4d bc 44 89 45 c0`
- 0x1a800 pool_take_free_run: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89 d3 49 89 ff 31`
- 0x1abf0 vmem_query_reserve_map: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec a8 00 00 00 48 8b 05 ?? ?? ?? ?? 44 89 85 74 ff`
- 0x1af80 vmem_batch_map: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 8b 0d ??`
- 0x1b0e0 pool_free_block: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 35 ?? ?? ?? ?? 49 89 fd bb`
- 0x19660 pool_node_alloc: `55 48 89 e5 41 56 53 48 83 ec 10 48 8b 05 ?? ?? ?? ?? 45 31`
- 0x197d0 pool_init_flexible: `55 48 89 e5 41 57 41 56 53 50 bb 01`
- 0x18aa0 bpe_PlatformPageAllocator_Init: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 60 01 00 00 48 8b 05 ?? ?? ?? ?? 49 be 00 00 00`
- 0x479a0 mem_allocator_alloc: `55 48 89 e5 53 50 48 89 fb 31 ff 80`
- 0x47a10 mem_allocator_alloc_reuse: `55 48 89 e5 41 56 53 48 83 ec 10 49 89 fe 48 89 f3 31 f6 41`

## 6. Open questions

1. Does an Onion or Garlic free VA return to a free list (2.3 step 2 consumes it)? Test: log node base and
   size around 0x1b0e0 and check reuse.
2. Why can P.74 > 0 with no free bit? Test: at 0x1a2f8 log P.74, the bitmap popcount, P.50, P.58, P.60.
3. Who reads the owner-tag tables?
4. Meaning of 0x323d0, 0x32560, 0x19c90 [U].
5. Who calls sub_8d770 [U].
6. Does free_all run per frame? Test: count pool_free_block per second.
