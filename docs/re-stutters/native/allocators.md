# N0 allocators: block allocators, owners and placement for PC-native memory

Specialist report, area "allocators" (revision 2, supersedes revision 1 in place). Static analysis of the decompilation
(`out/re/decomp`, main tree, branch re/pc-native), the runtime logs, the existing reports and the native pool sources.
Not committed (coordinator rule). Labels: **O** observed (decomp, vtable bytes, log line), **I** inferred, **U** unknown.
Eboot VAs; decomp files are named by offset.

## 0. Corrections to revision 1

- The native pool claim is verified in the logs, not only in docs: `user/pcmem/20261101-065841/logs/ring8pool-r1-a1.log`
  "calls 6447, native 6447, verified 6447, kills 0" (paths flex-exact 32, flex-node 824, primary-node 2594, fresh-plain
  3003), and `user/pcmem/20261101-071620/logs/poolrefill-r1-a1.log` "6453/6453". **O**
- The child heap's vtable +0x20 is **0x1d230**, the flip (verified from the eboot bytes, see 2.2). Revision 1 said
  "unconfirmed"; it is confirmed.
- Arena blocks ARE freed during play. The logs show the kind-1 arena at about 14-21 frees per 5 s and the kind-2 arena
  with one 32 MiB free per 5 s ("+18 allocs, -14 frees", "-1 frees (32 MiB)"). Revision 1 said "freed only at family
  destruction": wrong. The release path is 0x15340/0x14060 -> 0x15710/0x14430 -> 0x476f0 -> 0x1b0e0. **O**

## 1. Summary

- Families: CPUMemory (kind 1, 0x13380) and GPUMemory (kind 2, 0x110a0), each with Bucket, Heap and Page allocators. The
  per-frame GPU source is the kind-2 "Default TaggedArena" (32 MiB data blocks, 0x15ee0); its CPU-kind twin (8 MiB
  blocks, 0x14e80) hosts the XPR masks of RenderSingleFrameCPU. **O**
- The arena is keyed: its parent allocators 0x15520 (kind 2) and 0x14240 (kind 1) take the child's memtype id as the key
  (`CONCAT44(child+0x1c, child+0x88)`), and keep a per-key chunk table (32 keys for kind 2, 16 for kind 1). So placement
  can be decided per child heap at the arena level, not only per block owner. **O**
- Placement is feasible in the existing native writer without new pool logic: the fresh writers take their VA from
  `FreshHost::ReserveVirtual` (`pc_pool_native_fresh.h`), so a window is a different host that returns window VA. The
  window's blocks never enter the game's free-VA lists (its free path is intercepted at 0x476f0), so the game's VA leak
  does not reach windows and the VaRecycler quarantine is not needed for them. **I (design, untested)**
- The import is not per owner. The backing view is imported in 64 MiB chunks on demand (`SotcGuestMemory::Get`), and the
  ring path imports runs with `ImportGuestRange` (fast path: two lookups when already imported). So "import once" needs
  windows only for the locality of pinned RAM, not for correctness. **O (code), I (cost)**

## 2. Owners and the arena

### 2.1 Creation and ownership (O)

| Owner (name, kind) | Creation | Block size / refill | Pool slot | Notes |
|---|---|---|---|---|
| CPUMemory family, kind 1 | 0x13380 | container | none | children below |
| BucketAllocator, kind 1 | 0x13990 | 64 KiB pages, 16..1024 B | Onion (flex, Onion, Garlic) | CPU small data |
| HeapAllocator, kind 1 | 0x488f0; first refill 0x9600000 via 0x48910 (0x135a5) | 1 MiB heap blocks | Onion | BPE headers share 64 KiB blocks with data |
| PageAllocator, kind 1 | 0x13d20 | takes all above 1 MiB | Onion | CPU-only |
| GPUMemory family, kind 2 | 0x110a0 | container | none | children below |
| BucketAllocator, kind 2 | 0x11261 | 64 KiB pages | Garlic (spill Onion) | small GPU-side data |
| HeapAllocator, kind 2 | 0x11314, block 0x100000 | 1 MiB | Garlic | static GPU data (resource builds) |
| PageAllocator, kind 2 | 0x113ed | the rest | Garlic | static GPU data |
| Default TaggedArena, kind 1 (obj 0x5d0, ptr 0x2dfb2c0) | 0x110a0 at 0x114bb | `+0x80 = 0x4000`; data blocks 8 MiB (0x14e80, `<<9`); headers 0x498 forced to Onion (rcx=1) | Onion | hosts RenderSingleFrameCPU (memtype 17) |
| Default TaggedArena, kind 2 (obj 0x320, ptr 0x2dfb2b8) | 0x110a0 at 0x115e0 | `+0x80 = 0x100000`; data blocks 32 MiB (0x15ee0, `<<5`); headers 0x160 forced to Onion | Garlic | hosts RenderSingleFrameGPU (memtype 16) and its siblings |
| RTAllocator | render_targets_allocate 0x9aef0 / 0x9b778 | per area load | Garlic (spill Onion) | GPU-written |
| GameSingleFrame, Default, TFrameAllocators, STL | 0x110a0 | plain, kind 0 | Garlic | kind 0 also covers GPU-written objects (RenderContextFrameGPU) |

Pool slot rule (spec 2.2): kind 1 tries flex, Onion, Garlic; kinds 0 and 2 try Garlic, then Onion.

### 2.2 The child heap and the arena, verified from the eboot (O)

Vtables read from the read-only segment of `out/eboot.elf` (the decoded slots, not guesses):

- Child heap class (`PTR_FUN_01594ca0`, used by RenderSingleFrameGPU 0x2ed1f54 at 0x8c880 line 147, RenderSingleFrameCPU
  0x2ed1f58, GameSingleFrame, ...): +0x20 = **0x1d230 (the frame flip, which resets the lanes)**; +0xa0 = **0x1d750
  (lane allocator, called by the memtype dispatcher 0x12820)**; +0x98 = 0x13de0; +0x80 = 0x13ea0; +0x88 = 0x13dd0;
  +0x90 = 0x13eb0; +0xe0 and +0xe8 = 0 (the child has no parent-alloc slot; the parent is its +0x80 object).
- Arena kind 2 (`0x15948a0`): +0x20 = 0x13d30 (not the flip); +0xa0 = 0x13ec0; **+0xe8 = 0x15520** (parent alloc with key).
- Arena kind 1 (`0x1594790`): +0x20 = 0x13d30; +0xa0 = 0x13ec0; **+0xe8 = 0x14240**.
- The flip 0x8c860 calls 0x136f0 for both per-frame slots 0x2ed1f54 and 0x2ed1f58; 0x136f0 calls the registry entry's
  vtable +0x20, i.e. 0x1d230. **O**

Lane allocation (0x1d750): 11 per-thread lanes at child+0xa0 + lane*0x30 (thread index from 0x332b0), and one shared
lane at +0x2b0 behind a spin lock. A lane refill calls the parent's +0xe8 with `CONCAT44(child+0x1c, child+0x88)`
and the request size, and aligns the returned VA as `(addr + a-1) & ~(a-1)` on the absolute address. **O**

Parent alloc (0x15520 / 0x14240): take the spin lock at +0x70; find the key's bit in the chunk bitmap (+0x310 / +0x5c8)
and the key table (+0x210 / +0x548); walk the chunk list (+0x200 / +0x538) and fit the request with 0x15c20 / its kind-1
twin; if nothing fits, 0x15ee0 / 0x14e80 allocates a new arena block (`0x479a0` with owner = the arena, kind = the
arena's +0x18). Count and peak bytes are kept at +0x88 / +0x98 (key) and the arena totals. **O (decomp, first 60 lines)**
The full body of 0x15c20 and the block list layout are **U** (not read).

Release: 0x15340 (kind 2) and 0x14060 (kind 1) find the key's bit and call 0x15710 / 0x14430, which free empty blocks
through 0x476f0 (unlink, count -1, bytes -size) and 0x1b0e0 (pool free). Their callers beyond 0x15340 / 0x14060 are **U**.
The logs show these run during play (section 2.4).

### 2.3 Other allocator entries (O)

- 0x479a0 `mem_allocator_alloc`: owner enabled byte +0x9; size rounded to 64 KiB; rdx and rcx forwarded to 0x19ae0.
  Callers: 0x14e80 (2 sites), 0x15ee0 (2 sites), 0x17d20, 0x187e0, 0x48910, 0x48db9, 0x49200, 0x9aa10, render targets.
- 0x476f0 `node release`: the generic unlink-and-free, called from 0x14430, 0x15710, 0x17e90, 0x180b0, 0x18920, 0x48790,
  0x493d0, 0x49730, 0x8d930, and 0x1b0e0's family path. This is the single interception point for window release.
- 0x1b0e0 `pool_free_block`: called from 0x47620, 0x476f0, 0x47740 (family destroy and the helper above).
- 0x47a10 `mem_allocator_alloc_reuse`: reached only through table dispatch (0x8d770); not used by the arena. **U**
- Dispatcher 0x12820(size, memtype, align, flags): picks `DAT_02dfaab0[memtype]` and calls its vtable +0xa0. This is the
  per-heap layer. Its callers (0x8c0f7 descriptors, 0x117876, 0x16ea5f, ...) are listed in section 4.

### 2.4 Runtime evidence (logs, O)

From `user/pcmem/20261101-065841/logs/ring8pool-r1-a1.log` and `user/pcmem/20261101-085630/logs/drawbest-r1-a1.log`:

| Owner | Live MiB | Peak MiB | Notes |
|---|---|---|---|
| Default TaggedArena kind 2 | 64 to 96 | 96.06 | 3 to 4 blocks of 32 MiB; `RING8 host maintain: 1..4 blocks imported` |
| Default TaggedArena kind 1 | 8 to 32 | 40.06 | "+18 allocs, -14 frees in 5 s", churn 27 MiB/s |
| RTAllocator | 322.5 | 322.5 | one area |
| PageAllocator kind 2 | 990 to 1,447 | 1,466 | static GPU data |
| PageAllocator kind 1 | 321 to 348 | 398 | CPU only |
| HeapAllocator kind 1 | 208 to 307 | 311 | CPU only, BPE metadata |
| BucketAllocator kind 1 | 110 to 134 | 134 | CPU only |

Per-heap (MEMTYPE probe): RenderSingleFrameGPU (slot 16) bytes per frame avg 12.6 to 13.7 MiB, max 13.0 to 15.3 MiB,
mostly from caller 0x8c0f7 (descriptors, 1.1 GiB per 5 s). RenderSingleFrameCPU (slot 17) 0.087 MiB per frame, max
0.087 MiB. RINGFLIP: both per-frame heaps flip once per frame (138 to 150 flips per 5 s). **O**

Draw-time per-owner write faults and readbacks: section 3 of the earlier revision and PC-MEMORY-REWRITE section 6.

## 3. Sites by class

| Owner | Live | Faults/s (earlier run) | Class | Written by | Read by GPU | Reuse |
|---|---|---|---|---|---|---|
| Arena kind 2 children (RenderSingleFrameGPU) | 64-96 MiB | 3,548 (48%) | GPU_DYNAMIC | CPU threads, about 33 submits per frame | yes, after submit | per frame |
| RenderSingleFrameCPU (XPR masks) in arena kind 1 | 0.1 MiB per frame | 437 | GPU_WRITTEN | CPU (BuildSetDraws 0x19b760) and GPU | GPU writes; CPU reads (0x1a6d20) | per frame |
| RTAllocator | 322.5 | 0 | GPU_WRITTEN | GPU | GPU | per area |
| PageAllocator kind 2 / HeapAllocator kind 2 / Bucket kind 2 | 1.4 GiB / 1.2 GiB / 10 MiB | 0 / 5 / 175 | GPU_STATIC | CPU at build (0x8be60, 0x96680) | GPU | static |
| CPU kind 1 families | about 0.8 GiB | about 3,200 | CPU_ONLY | CPU | none | block reuse |
| Kind 0 GPU objects (RenderContextFrameGPU, particle double frame) | 20 MiB and others | n/a | GPU_DYNAMIC | CPU until submit | GPU | per frame |

## 4. Address sensitivity sweep (decompilation)

Scope: 79 decomp functions contain a 64 KiB address mask (`0xffffffffffff0000`); 49 touch an allocator global (registry
`DAT_02dfaab0`, arena pointers `DAT_02dfb2b8/c0`, root `DAT_02dfca78`, `DAT_02dfb4d0`). **O (grep, not a full read)**

Findings:

1. Arena pointer identity is compared only at creation and teardown (0x110a0 lines 140-307; 0x13220 destroy lines 9-13).
   No other function compares an arena pointer. **O**
2. Masks to 64 KiB base are page arithmetic on VA: the streaming window (0x98ae0, 0x98e20 `base & ~0xffff + offset`) and
   the non-streamed copy (0x96680 lines 161-162). Placement keeps 64 KiB alignment, so they are safe. **O**
3. Descriptor packing: 0x58d4e0 builds a resource descriptor from a VA (`(VA & ~0xffff) << 0x20`, plus low bits). The base
   is a 40-bit field of the PS4 descriptor. Windows must lie below 2^40 (kVaFieldLimit). The direct pool satisfies this.
   **O (decomp shape), I (field width)**
4. 0x5f42c0 reads `*param_3 & ~0xffff` as a VA base for a shape/UV computation; safe under the same alignment rule. **O**
5. Intra-block walks (BPE heap 0x48910/0x48a70, arena chunk lists, the free-list coalescing in 0x1b0e0) use relative
   offsets and absolute pointers that stay valid while the block lives. No cross-owner compare was found. **O (partial)**

Not covered: the other 30 functions that touch allocator globals were not read; decomp functions that compare heap
pointers through locals (not globals) are **U**. The sweep is therefore partial; the runtime check (section 7, item 3)
is the complete answer.

## 5. Native pool: where a window goes (dispatch and writers, O)

`pc_pool_native_dispatch.cpp` `Apply()` routes the predicted path to a writer. The writers and their VA source:

| Path | Writer | VA source | Host calls |
|---|---|---|---|
| FlexHitExact, PrimaryHitExact | ApplyExactHit (`pc_pool_native.cpp`) | free-VA node of the set | none |
| FlexHitNode, PrimaryHitNode | ApplyNodeHit / ApplyPrimaryNodeHit | free-VA node, split or pad | none (refill through 0x19e00 exact hit) |
| FreshPlain, FreshSpill, FreshReserve, FreshPoolFix | ApplyFresh* (`pc_pool_native_fresh.cpp`) | **`host.ReserveVirtual(size, align)`** | `ReserveVirtual`, `BatchMap` |

`PoolFreshHost::ReserveVirtual` (`pc_pool.cpp` line 1826) calls `runtime_memory_reserve_range_export` (the port's
sceKernelReserveVirtualRange). `BatchMap` (line 1834) calls the port's batch map and applies the entries to the shadow VA
map. **O**

Window implementation, concretely:

1. **Window host.** A `FreshHost` wrapper (`WindowFreshHost`) that serves `ReserveVirtual` from a window's free list when
   the call's owner is a windowed arena (see 6), and falls back to `PoolFreshHost::ReserveVirtual` otherwise. The fresh
   writers then need no change: they already accept any host VA, check its alignment (`HostVa`), and map it with the
   batch flush. **I (design)**
2. **Model.** `PredictCall` does not need the VA (the fresh model says "the VA is not predictable"), so predictions stay
   valid. The shadow map (`PhysMap`) gets the window's VA through the same batch-op update. **O (pc_pool_native_fresh.h
   header: "the plan does not depend on the VA")**
3. **Release.** The writers never free. Freeing is in the game: 0x476f0 -> 0x1b0e0. A window-aware hook at 0x476f0
   entry tests the node's VA against the windows. For a windowed node it performs the first half of 0x476f0 (unlink from
   the family list, count -1, bytes -size: exactly the instructions of the decomp at lines 0x476f0 1-11) and returns
   without calling 0x1b0e0. The node is parked on the window's free list; its VA and phys stay mapped and allocated in
   the pool's own accounting (the set counters keep the block as in use). This is the only change to the pool's
   bookkeeping, and it is what keeps the VA out of the game's free lists. **I (design)**
4. **Reuse.** The next arena block request (0x479a0 with the arena as owner) is served by the window: insert the parked
   node into the family live list, count +1, bytes +size (the same stores as 0x479a0 lines 9-17 of the decomp), and
   return it. No pool call, no batch op, no phys change. **I (design); the stores are O**
5. **Import.** After the window node exists, `SotcGuestMemory::ImportGuestRange(window_va, window_size)` once at boot
   (ring path, `RingDecide` must hold). Subsequent per-submit calls take the fast path (`FindRun` and `RunMatches`).
   Imported runs are split where the window's phys is not contiguous; see section 6.

## 6. Window layout (concrete)

Sizes are from section 2.4 and the MEMTYPE lines. Each window is one pool node created at boot through the native fresh
path (owner = the arena or the RT object), so its phys comes from the bitmap lowest-first and is usually one run; the
window's VA is the node VA.

| Window | Serves | Size | Blocks | Source of the size | Import | Created at |
|---|---|---|---|---|---|---|
| A: per-frame GPU arena | arena kind 2 blocks (32 MiB) and their children | 128 MiB | 4 x 32 MiB | 96 MiB peak live, 3-4 blocks in ring8 | once at boot | 0x110a0 after 0x115e0 |
| B: XPR mask block | arena kind 1 block for child key of RenderSingleFrameCPU | 16 MiB | 2 x 8 MiB | 0.09 MiB per frame, one 8 MiB block is enough; 16 MiB leaves a second block for the flip | once at boot | 0x110a0 after 0x114bb |
| C: render targets | RTAllocator blocks (0x9b778, 0x14280000 = 320 MiB, 0x23d10000 = 595 MiB) | 352 MiB for the 320 MiB case | 1 area block | 322.5 MiB live | once per area load | 0x9b778 |
| D: GPU static | none | n/a | n/a | 1.4 GiB PageAllocator + 1.2 GiB HeapAllocator | per 64 MiB chunk on first use (existing `SotcGuestMemory::Get`) | n/a |

Notes:

- **A** is the one that matters for the flip. The window holds the arena's own blocks, so the per-frame data gets a fixed VA
  for the whole session: the ring8 import (`host maintain: N blocks imported`) becomes a boot-time import with no per-block
  churn. The 0x160 headers stay in Onion (forced by the game), so window A holds only the 32 MiB data blocks.
- **B** is the placement answer for the masks, but only at the child-key level: the arena's chunk table is per key
  (0x14240), and the window's block must be excluded from the global block list walk, so other kind-1 children never carve
  from it. The walk is the loop over `*(param+0x538)` in 0x14240; the patch skips window nodes there. **I**
- **C** needs the window for the render-target size actually requested; the second size (595 MiB) is not in the logs.
  Confirm which allocation is which before sizing. **U**
- **D** does not need a window. Static data is already importable per chunk through the backing view; placing it in windows
  would only move pins around. The open cost is RAM pinning: 2.6 GiB of static GPU data is pinned once imported. **I**

Total extra VA reserved: about 500 MiB. Physical RAM: no extra, because the blocks come from the pool's own phys and are
the same blocks the game allocates today (window A replaces the arena's current blocks; window B and C replace blocks the
game would have allocated anyway).

## 7. VA capacity and the VaRecycler

- Windows never return VA to the set free lists, so the game's leak does not touch them. Window capacity is the window
  size. When a window is full, the arena allocation falls back to the pool (counted as a window miss, never silent).
- The quarantine (`pc_pool_place.h`, section 3) is needed only for the game's own freed Onion and Garlic VA (710 and 50
  VA units per 5 s window, PC-MEMORY section 6). Windows do not need it. **O (leak rates), I (quarantine not needed)**
- The parked-node list is bounded by the window's block count (4 for A, 2 for B, 1 for C), so no growth.

## 8. Patch points (summary)

| Site | Change | Kind |
|---|---|---|
| 0x110a0 after 0x115e0 / 0x114bb | create windows A and B (pool node each) and import | boot |
| 0x476f0 entry | window release (park, no 0x1b0e0) | wrap, pre-phase |
| 0x14240 / 0x15520 key walk | skip window nodes in the global list; window nodes for their key | entry, list walk |
| 0x479a0 with arena owner | return a parked window node (family insert) | wrap, replace |
| 0x9b778 | window C for the render target request | entry |
| 0x8c860 | no change (frame flip already probed) | none |
| `WindowFreshHost` | ReserveVirtual served from a window | host, new class |

## 9. Address dependencies that placement could break

1. Alignment on absolute VA: windows are 1 MiB aligned (the S1/S2 search rounds to 1 MiB; 64 KiB for the lanes).
2. Address-ordered lists: window nodes are never in the set lists, so no ordering changes there.
3. Stored absolute pointers (BPE, arena chunk lists, lane lists): valid while the block lives; a parked block is reused
   only after its previous owner is gone (the family unlink in 0x476f0 precedes the park). Risk: a stale pointer into a
   parked block would see the next owner's data. Test: the verify mode of section 7 item 3.
4. Pool spill: a window node is created by the native writer, so the op 4 spill rule still applies at creation.
5. 40-bit descriptor fields (0x58d4e0): windows must be below 2^40. The direct pool region is well below it.
6. POOL_FIX and the empty free-VA guard: unchanged; windows bypass the free-VA lists.
7. The MEMTYPE and descriptor callers (0x8c0f7) write V# base addresses into GPU-read descriptors: those bases are the
   window VA, which is stable for the session, so no descriptor rewrite is needed.

## 10. Open questions (with the test that resolves each)

1. The render target 595 MiB size: which allocation is it (0x9b778 call sites). Test: log the size and owner at 0x9b778.
2. Which callers reach 0x15340 / 0x14060 (the key release) and how often. Test: count calls per second with the probe.
3. Pointer compares through locals in the 30 unread functions. Test: a runtime check that no compare crosses a window
   boundary (verify mode with windows on: count any access to a parked block by its previous owner).
4. The 0x15c20 block-list walk layout: needed to write the window exclusion correctly. Test: read 0x15c20 and 0x14240 in full.
5. Phys contiguity of the window nodes at boot (one run). Test: count the ImportRun calls for window A (one run expected).
6. RAM pinning of the imported chunks (D): measure the working set with SOTC_PC_RING on.

## 11. Files read

Decomp (main tree): `out/re/decomp/0001/` 000130d0, 000130f0, 00013380, 00013990, 00013d20, 00013110, 000136f0, 0001d230,
0001d750, 00012820, 000120d0, 0001cfb0, 00017990, 00018750, 00019e00, 00015ee0, 00014e80, 00015520, 00014240, 00015340,
00014060, 00015710, 00014430, 0001b0e0, 000110a0, 00013220, 0001d230; `out/re/decomp/0004/` 000479a0, 00047a10, 00047620,
00047740, 000476f0, 00047ab0, 00047dd0, 000488f0, 00048910, 00049200, 00049730, 000452f0; `out/re/decomp/0008/` 0008c370,
0008c860, 0008c880, 0008d770; `out/re/decomp/0003/000332b0`; the sweep over `out/re/decomp/*/*.c`.
Eboot vtables: `out/eboot.elf` read-only segment (0x1594ca0, 0x15948a0, 0x1594790).
Logs: `user/pcmem/20261101-065841/logs/ring8pool-r1-a1.log`, `user/pcmem/20261101-071620/logs/poolrefill-r1-a1.log`,
`user/pcmem/20261101-085630/logs/drawbest-r1-a1.log`.
Native sources (headers read in full, `.cpp` read by signature and entry points): `gpu/sotc/pc_pool_native_dispatch.h/.cpp`,
`pc_pool_native_fresh.h/.cpp` (header in full), `pc_pool_native_nodes.h`, `pc_pool_native.h`, `pc_pool.cpp` lines 1800-1900.
Port: `gpu/shadps4/video_core/renderer_vulkan/sotc_guest_memory.h/.cpp` (ImportGuestRange, Get),
`gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp` (callers of ImportGuestRange).

Not read in full (declared): `pc_pool_native_primary.cpp`, `pc_pool_native_reserve.cpp`, `pc_pool_native_nodes.cpp`
bodies beyond their signatures, `pc_pool_native_fresh.cpp` bodies beyond the entry points, `pc_pool_native.cpp`.
These need a full read before the window code is written. Nothing was committed or merged.
