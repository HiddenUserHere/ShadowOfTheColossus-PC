# N0 allocators: block allocators, owners and placement for PC-native memory

Specialist report, area "allocators". Read-only static analysis: Ghidra C in `out/re/decomp` (main tree), the
existing reports (`reports2/06`, `reports3/06`, `PC-MEMORY-REWRITE.md`), `scripts/hooks.d/alloc.py`, `pcpool.py`,
and `gpu/sotc/pc_pool_place*.h`. Not committed (coordinator rule). Labels: **O** observed in decomp or docs,
**I** inferred, **U** unknown. Eboot VAs; decomp file names are the offsets.

## 1. Summary

- Two families: CPUMemory (kind 1, 0x13380) and GPUMemory (kind 2, 0x110a0). Each owns a BucketAllocator, a
  HeapAllocator and a PageAllocator (O). The two "Default TaggedArena" objects are the per-frame GPU source.
- Per-frame GPU data lives in child heaps of TaggedArena kind 2 (32 MiB blocks). The CPU-kind TaggedArena (kind 1,
  8 MiB blocks) also hosts GPU-written XPR masks (RenderSingleFrameCPU, memtype 0x2ed1f58). Placement by owner kind
  therefore cannot separate CPU and GPU data. It must be decided per heap (memtype slot, 0x12820) and per frame
  window.
- The allocators are address-agnostic: they align by arithmetic on the VA and keep address-ordered free lists. I
  found no engine code that compares heap addresses across owners (not yet swept, see 7.3). Stored absolute
  pointers (BPE headers, arena lists) stay valid because blocks never move. The real constraints are the game's own
  VA leak (freed Onion/Garlic VA is never returned, PC-MEMORY section 6) and the 2^40 field limit.
- Proposal: windowed placement at the block refill sites (0x14e80, 0x15ee0, 0x9b778 render targets), with each window
  host-imported once. Per-frame GPU arena: fixed ring window. Static GPU data: a window imported on allocation.
  CPU-only pools stay in Onion and are never imported.

## 2. Owners

### 2.1 Creation and ownership (O)

| Owner (name, kind) | Creation | Block size / refill | Pool slot of its blocks | Notes |
|---|---|---|---|---|
| CPUMemory family, kind 1 | `cpu_memory_family_init` 0x13380 | family (container) | none | children below |
| BucketAllocator, kind 1 | 0x13990 (size loop 0x13428) | 64 KiB pages, sizes 16..1024 | Onion (kind 1: flex, Onion, Garlic) | CPU small data |
| HeapAllocator, kind 1 | 0x488f0 block 0x100000; first refill 0x9600000 via 0x48910 (0x135a5) | 1 MiB heap blocks | Onion | BPE heap: headers share 64 KiB blocks with data |
| PageAllocator, kind 1 | after 0x135b1 (0x13d20) | takes all above 1 MiB | Onion | runtime: 324 MiB live, 1,685 faults/s, 0 GPU pages |
| GPUMemory family, kind 2 | 0x110a0 (`sub_47b20`) | family | none | children below |
| BucketAllocator, kind 2 | 0x11261 | 64 KiB pages, 16..1024 | Garlic (kind 2: Garlic, spill Onion) | 10 MiB live |
| HeapAllocator, kind 2 | 0x11314, block 0x100000 | 1 MiB | Garlic | 1.2 GiB live, 28,449 GPU pages, 5 faults/s |
| PageAllocator, kind 2 | 0x113ed | the rest | Garlic | 1.4 GiB live, 57,618 GPU pages, 0 faults |
| Default TaggedArena, kind 1 (obj 0x5d0, pointer 0x2dfb2c0, accessor 0x130f0) | 0x110a0 at 0x114bb | `+0x80 = 0x4000`: data blocks 8 MiB (0x14e80, `<< 9`); node headers 0x498 forced kind 1 (Onion) | Onion | hosts RenderSingleFrameCPU (memtype 0x2ed1f58): XPR masks, GPU-written |
| Default TaggedArena, kind 2 (obj 0x320, pointer 0x2dfb2b8, accessor 0x130d0) | 0x110a0 at 0x115e0 | `+0x80 = 0x100000`: data blocks 32 MiB (0x15ee0, `<< 5`); node headers 0x160 forced kind 1 (Onion) | Garlic | hosts RenderSingleFrameGPU (0x2ed1f54) and the descriptor/particle children: the per-frame GPU arena |
| RTAllocator | render_targets_allocate 0x9aef0 / 0x9b778, kind 2 | per area load | Garlic (spill Onion) | GPU_WRITTEN, 322 MiB, 0 faults |
| GameSingleFrame, Default, TFrameAllocators, STL | 0x110a0, kind 0 | plain | Garlic (kind 0) | CPU-only per docs; kind 0 also covers GPU-written objects (5.6) |

Pool slot rule (O, spec 2.2): kind 1 tries flexible, Onion, Garlic; kinds 0 and 2 try Garlic, then Onion. Pool sets
are root, root+0xa0, root+0x140 (pool ids 0, 1, 2; spec 1.1).

### 2.2 The TaggedArena: how blocks become heaps (O, decomp)

- `mem_allocator_alloc` (0x479a0) is called with the arena object as owner (`+0x18` = 1 or 2). The arena's own refill
  functions are `0x14e80` (kind 1: `FUN_000479a0(param_1, (*(obj+0x80)) << 9, 0x10000, *(obj+0x18))`, so 8 MiB blocks,
  node stride 0x460) and `0x15ee0` (kind 2: `<< 5`, so 32 MiB blocks, stride 0x128). Both first take a 0x498 or
  0x160 byte header block with the forced rcx = 1 (Onion). The node headers therefore live in Onion even for the GPU
  arena.
- Child heaps (per-frame heaps such as RenderSingleFrameGPU) are made by `FUN_0001cfb0(child, arena, memtype)`. Their
  allocator is the lane allocator `0x1d750` (vtable slot 0xa0, called from the memtype dispatcher 0x12820): 11
  per-thread lanes at child+0xa0 + lane*0x30 (thread index from 0x332b0), plus one shared lane at +0x2b0 behind a spin
  lock. A lane refills by calling the parent's vtable slot 0xe8 (the arena), which carves a run from its blocks.
  Alignment is applied to the absolute VA (`(addr + a-1) & ~(a-1)`).
- Frame boundary: `0x8c860` flips memtype slots 0x2ed1f54 and 0x2ed1f58 (the GPU and CPU per-frame children) via
  `0x136f0`, which calls the heap's vtable slot +0x20. Called from `RenderBackend_Frame` (0x90ea0) after the writers
  (PC-MEMORY 3, reported). `GameFrame_Update` (0x25ae80) flips five more game heaps (0x25b00b-0x25b03b, reported). The
  vtable +0x20 target for the arena children is **U**: 0x1d230 is called only from 0x107540 (ParticleRenderer::BeginFrame),
  so it is not confirmed as the flip.
- Reuse: the flip resets the child's lanes, so a per-frame block is reused every frame. Runtime: 54-60% of block
  allocations reuse a freed base (PC-MEMORY 6, second measurement). Arena blocks are freed only at family destruction
  (0x47620, 0x47740), so per-frame reuse happens inside the arena, not through pool_free_block.

### 2.3 Other allocator entries (O)

- `mem_allocator_alloc` 0x479a0: owner `[obj+0x9]` enabled, size rounded to 64 KiB, forwards rdx and rcx to 0x19ae0.
  Callers: sub_14e80 (2), sub_15ee0 (2), sub_17d20, sub_187e0, sub_48910 (heap refill), BpeHeap_Free (0x48db9),
  sub_49200 (BPE free-run refill), sub_9aa10, render_targets_allocate.
- `mem_allocator_alloc_reuse` 0x47a10: called only by sub_8d770, which has no static caller (table-dispatched, U). It
  subtracts the old node's `+0x50` from the family bytes first (reuse in place).
- `pool_free_block` 0x1b0e0: called by 0x47620 (family destroy), 0x476f0 and 0x47740. 0x476f0 is a generic "unlink and
  free this node" helper called from 0x14430, 0x48790, 0x15710, 0x180b0, 0x8d930, 0x18920, 0x17e90, 0x493d0, 0x49730.
  Its per-frame frequency is **U** (not measured).
- Dispatcher `0x12820(size, memtype, align, flags)`: picks the heap object from the registry `DAT_02dfaab0[memtype]` and
  calls its vtable slot 0xa0. This is the per-heap layer where class decisions belong (5.1). Callers include 0x8c0d0
  (descriptors), 0x96680, 0x117876, 0x16ea5f (about 80k calls/s, PC-MEMORY 6).

### 2.4 BPE heap (0x48a70-0x4a477) (O for code, I for layout)

- 0x48910 (heap refill) calls 0x479a0 for a block, then writes a header and free-tree links inside that block (for
  example `puVar2[1] = 3` and a relocated pointer field in `puVar2[3]`). The walk is `BpeHeap_Walk` (0x48a70). Headers
  therefore sit in the same 64 KiB pages as data. For a GPU-written heap this puts metadata writes on GPU pages
  (report 06 section 3.4: 18% of write faults).
- Stored pointers are absolute VA (`*(puVar2+0x20+off) = puVar2+2`). A block is never moved after allocation, so
  placement is safe as long as its VA is fixed for the block's life.

## 3. Sites by class

Classes as in `PC-MEMORY-REWRITE.md` section 4, with per-owner evidence from its section 6 measurement (write faults per
second, live size, GPU pages).

| Owner | Live | Faults/s | Class | Written by | Read by GPU | Reuse |
|---|---|---|---|---|---|---|
| Arena kind 2 child: RenderSingleFrameGPU (0x2ed1f54) | 96 MiB (3-4 x 32 MiB) | 3,548 (48% of all) | GPU_DYNAMIC | CPU game threads, about 33 submits per frame | yes, after submit | per frame, blocks reused |
| RenderSingleFrameCPU (0x2ed1f58) in arena kind 1 | 16 MiB | 437 | GPU_WRITTEN (XPR masks) plus CPU writes | CPU (BuildSetDraws 0x19b760); GPU writes masks | GPU writes, CPU reads (0x1a6d20) | per frame |
| Render targets (RTAllocator, 0x9aef0) | 322 MiB | 0 | GPU_WRITTEN | GPU | GPU (CPU never reads) | per area load |
| PageAllocator kind 1 (CPU) | 324 MiB | 1,685 | CPU_ONLY | CPU | none | block reuse |
| BucketAllocator kind 1 | 134 MiB | 1,238 | CPU_ONLY | CPU | none (438 reads/s) | pages |
| HeapAllocator kind 1 | 303 MiB | 284 | CPU_ONLY (BPE headers) | CPU | none | heap |
| HeapAllocator kind 2 | 1.2 GiB | 5 | GPU_STATIC | CPU at build (0x8be60, 0x96680) | GPU | static |
| PageAllocator kind 2 | 1.4 GiB | 0 | GPU_STATIC | CPU at build | GPU | static |
| BucketAllocator kind 2 | 10 MiB | 175 | GPU_STATIC (small) | CPU | GPU | static |
| RenderContextFrameGPU (kind 0 object) | 20 MiB | not measured | GPU_DYNAMIC | CPU until submit | GPU | per frame, DCB/CCB |
| Particle double frame (kind 0, sub_101ee0) | not measured | not measured | GPU_DYNAMIC | CPU | GPU | per frame |

## 4. Placement proposal (PC-native), per owner

Principle: a window is a fixed VA range, host-imported once (VK_EXT_external_memory_host, as in SOTC_HOST_IMPORT). The
CPU writes through the game's own pointers, with no faults and no tracking. The GPU needs an ordering rule for writes
after submit (the ring8 label rule in `pc_ring.h`). CPU-only owners need no window.

1. Per-frame GPU arena (arena kind 2 children). Window A: 3 or 4 x 32 MiB, 128 MiB reserved, imported at boot. Patch
   point: the `0x479a0` call inside the `0x15ee0` refill (`FUN_000479a0(param_1, obj+0x80 << 5, ...)`). When the owner
   is arena kind 2, take the next window block; the node bookkeeping stays the same, only the block base changes. The
   0x8c860 flip (already probed by the frame-flip hook) releases the ring slot. The 0x160 headers stay in Onion (forced
   by the game), so only the 32 MiB data blocks move.
2. Per-frame CPU-kind arena (RenderSingleFrameCPU, holds XPR masks). Do not place by owner. Two options: (a) place by
   memtype 17 at 0x12820 (the heap_slot input of the glue) into window A, so the masks are GPU-visible without a CPU
   wait; or (b) remove the GPU-written masks from this heap (N3 readback removal). Option (a) is the placement answer.
3. Render targets. Window B per area: RTAllocator blocks (`render_targets_allocate` 0x9b778, sizes 0x14280000 or
   0x23d10000) come from a window imported once per area load. Patch: the 0x9b778 call through the pool wrapper 0x19ae0
   (owner = the kind 2 object). Released at area unload (0x1b0e0 path).
4. GPU static (kind 2 family: Heap, Page, Bucket). Window C: today's Garlic layout (1.4 GiB PageAllocator, 1.2 GiB
   HeapAllocator). Imported per 64 MiB chunk on first touch (already in SOTC_COMMIT_CHUNKS=2). Resource copies (0x8be60,
   0x96680) become explicit uploads (N1 NativeWrite) or are free in the window.
5. CPU-only (kind 1 family, kind 0 plain objects). No window; stays in Onion or flex. Remaining issue: BPE metadata
   sharing 64 KiB blocks with GPU data. Kind 1 and kind 2 blocks already never mix within one heap, so this is solved
   by the owner rule at 0x19ae0.
6. Kind 0 objects that are GPU-written (RenderContextFrameGPU, particle double frame): keep them out of kind-based
   rules (report 06 warning). Route them by owner address, not by kind.

Patch points: 0x19ae0 (PC_POOL_NATIVE entry, owner in rdi, already hooked), 0x15ee0 and 0x14e80 refill call sites
(block routing), 0x9b778 (render targets), 0x12820 (memtype routing by heap_slot), 0x8c860 (frame fence and slot
release), 0x1b0e0 (window return on free, not per frame).

Capacity: freed Onion and Garlic VA is never returned by the game (710 and 50 per window, PC-MEMORY section 6). A window
that recycles blocks therefore needs the VaRecycler quarantine (`pc_pool_place.h` section 3). Without it, windows
exhaust within minutes. Recycling is the main risk, not the mapping.

## 5. Address dependencies that placement could break

1. Alignment on absolute VA (0x1d750 refill, 0x19e00 round-up, 0x48910 at 0x10000). Window bases must be multiples of
   64 KiB (1 MiB for the S1/S2 search). Safe if windows are 1 MiB aligned.
2. Address-ordered free lists (0x1a800 first fit by address; 0x1b0e0 coalescing when base + size equals the next base).
   Separate windows never merge, which is correct. Only fragmentation order changes.
3. Stored absolute pointers (BPE heap, arena node lists, lane lists at child+0xa0). Fixed for a block's life. Safe, as
   long as window VA is not returned to a non-window list by 0x1b0e0 and a block is not reused while a node still
   points into it. The per-frame flip already guarantees this for the arena.
4. Pool-set spill (kinds 0 and 2 spill to Onion; kind 1 spills to Garlic). A window must not spill silently; the op 4
   type change (0x1a500-0x1a5e6) must still be generated.
5. 40-bit pointer packing (`kVaFieldLimit = 2^40`, pc_pool_place.h). Windows must stay below 2^40. The direct pool
   near 0x1040300000 satisfies this.
6. POOL_FIX and the empty free-VA guard (spec 2.5): unchanged by placement, but an exhausted window must fall through
   to the next set, never read `[rax+0x58]` on an empty list.
7. Not checked: whether any game code compares a heap pointer with a global base, or derives a GPU address by
   subtracting a heap base (for example to build a V# or a descriptor index). This needs a decomp sweep for comparisons
   against `DAT_02dfaab0[*]` results and heap `+0x1c` ids. Unknown until that sweep runs.

## 6. Verification state

- Measured (docs): per-owner faults and live sizes (PC-MEMORY 6, first measurement); per-frame heap bytes (second run);
  ring8 safe run (27 native exact-hit allocations, 27 verified, 0 kills); PcPool model 6,140 matches, 112 mismatches,
  0 unmodelled (corrected model run).
- Claimed, not re-checked: "6454/6454 calls native and verified" (HANDOFF.md line 16). The safe run in PC-MEMORY 6
  performed only exact-hit allocations (27). Flex-node 770/770 was verify-only. The node-hit writer (`ApplyNodeHit`)
  exists in code with a refill path that needs `overhead_addr`. Treat "6454/6454 native" as a claim until a run shows it.
- Not read in full: `pc_pool_native_dispatch/fresh/nodes/primary/reserve.cpp`; the glue's FitInWindow and
  AutoWindowPlan semantics; the arena vtable (slots 0x20 and 0xe8 targets); the BPE walk 0x48a70.

## 7. Open questions (with the test that resolves each)

1. Which function does the arena child's vtable slot +0x20 point to (the flip target)? Test: log the pointer at 0x136f0
   entry, or decode the vtable at the child's first word.
2. Does pool_free_block run per frame? Test: count pool_free_block and 0x476f0 per second in one Lab window (FREE_BLOCK
   probe in alloc.py).
3. Are heap pointers compared outside the allocators? Test: decomp sweep for compares against `DAT_02dfaab0[]` results,
   plus a runtime check that no compare crosses window boundaries.
4. Is RenderSingleFrameCPU the only kind-1 heap with GPU-written data? Test: owner and memtype logging at 0x12820 with
   the GPU bind feed (`SOTC_RB_OWNER`).
5. Does the arena's 32 MiB refill run per frame or only on growth? Test: count 0x479a0 calls with owner = arena kind 2
   per second.
6. Node-hit writer correctness for window blocks (spec 2.5 hazard). Test: verify mode with windows on, pool mismatch count.

## 8. Files read

Main tree (base for reading, branch re/pc-native): `out/re/decomp/0001/` 000130d0, 000130f0, 00013380, 00013990,
00013d20, 00013110, 000136f0, 0001d230, 0001d750, 00012820, 000120d0, 0001cfb0, 00017990, 00018750, 00019e00, 00015ee0,
00014e80, 0001b0e0; `out/re/decomp/0004/` 000479a0, 00047a10, 00047620, 00047740, 000476f0, 00047ab0, 00047dd0,
000488f0, 00048910, 00049200, 00049730, 000452f0; `out/re/decomp/0008/` 0008c370, 0008c860, 0008c880, 0008d770;
`out/re/decomp/0003/000332b0`; `docs/re-stutters/reports2/06-allocator-placement.md`, `reports3/06-pool-layer-spec.md`,
`PC-MEMORY-REWRITE.md`; `scripts/hooks.d/alloc.py`, `scripts/hooks.d/pcpool.py`; `gpu/sotc/pc_pool_place.h`,
`gpu/sotc/pc_pool_place_glue.h`, `gpu/sotc/pc_pool_native.h`.

Note: this worktree is on an older base and has no `docs/re-stutters/native/` folder. The report is written here,
uncommitted, per the coordinator rule. Nothing was committed or merged.
