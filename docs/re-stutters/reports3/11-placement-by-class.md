# 11 - Placement by memory class (design)

Design only, plus the standalone decision core `gpu/sotc/pc_pool_place.{h,cpp}` and its unit tests
`gpu/sotc/tests/pc_pool_place_test.cpp` (250 checks, PASS, clang++ -std=c++20 -Wall -Wextra, no warnings). Nothing is wired:
no hook, no buffer-cache change, no switch. Context: `docs/re-stutters/PC-MEMORY-REWRITE.md` (sections 2, 4, 6),
`reports3/04`, `05`, `06`. Labels: **observed** (read in the code or in a report), **reported** (agent report,
not re-checked here), **inferred**, **open** (needs a run).

## 0. Summary

1. **The class of a request is known at the pool wrapper only partly.** The wrapper 0x19ae0 sees the owner
   object (rdi). The owner decides the set of a block, but a block is requested by an arena, and one arena
   serves several heaps of different classes. The heap is known only inside the 0x12820 call that refills it.
   Placement therefore uses the heap when there is one, and the owner otherwise. A block is only published
   as CPU_ONLY when every source agrees.
2. **Per-block placement is not enough by itself.** A block can carry several heaps' data, so a block gets a
   sticky class: a second, different heap class makes it Unknown (`BlockLedger`).
3. **VA windows are carved per class inside the three pool sets.** The sets are contiguous and adjacent, so the
   windows are a fixed table. Placement is realised by the native path (`SOTC_PC_POOL_NATIVE`); the game's own
   fallback paths are class-blind, and their blocks are classified after the fact by VA. Purity is therefore a
   performance property. Safety comes from `Effective()`: a block publishes only when its owner class, its
   window class and the bind evidence all agree.
4. **Freed VA can be recycled** through a quarantine with a GPU fence and a cache-invalidation gate, per class,
   with a verify mode that never reuses (`VaRecycler`).
5. **Two corrections to the brief** (section 3): the GPU_WRITTEN rule cannot be "no CPU protection" (CPU reads
   must still fault to a readback), and `NoteGpuBind` is not called from the port today.

## 1. Question 1: where the class of a request is known

### 1.1 What the pool wrapper receives

**observed** (`gpu/sotc/probe_alloc.cpp` header, `reports3/06` section 1.4, `pc_pool_native.h`): the owner
object `rdi` is the family object. `+0x10` is the name (char*), `+0x18` the kind (1 CPU, 2 GPU), `+0x1c` the owner-tag
byte. The probe keys its owner rows by this address, because the same names exist for kind 1 and kind 2 objects.
`mem_allocator_alloc` (0x479a0) forwards `rdi` to 0x19ae0 unchanged.

**observed** (`PC-MEMORY-REWRITE.md` section 6, first measurement): only **9 block-level owners** hold the
3.6 GiB. Named in the probe output:

| Owner (name, kind) | Live (wave B) | Write faults/s | GPU pages | Note |
|---|---|---|---|---|
| Default TaggedArena, 2 | 96 MiB | 3,548 | 6,683 | per-frame GPU heaps |
| PageAllocator, 1 | 324 MiB | 1,685 | 0 | **GPU reads it** (write faults on a protected page) |
| BucketAllocator, 1 | 134 MiB | 1,238 | 41 | |
| Default TaggedArena, 1 | 16 MiB | 437 | 8 | |
| HeapAllocator, 1 | 303 MiB | 284 | 180 | |
| BucketAllocator, 2 | 10 MiB | 175 | 87 | |
| HeapAllocator, 2 | 1.2 GiB | 5 | 28,449 | static data |
| PageAllocator, 2 | 1.4 GiB | 0 | 57,618 | static data |
| RTAllocator | 322 MiB | 0 | 19,928 | render targets |

**observed, the warning sign:** `mem_classes.cpp` (`OwnerClass`) records that a first live run classified the
kind-1 PageAllocator as CPU_ONLY from its "no GPU pages" count, and that this was wrong: it took 1,685 write
faults per second. Name and kind are a prior. Only the bind evidence may publish.

### 1.2 Heaps inside the owners

**observed** (`reports3/04` section 2, `reports3/05` section 2): the named heaps are created by 0x8c880 with a
parent arena of a given kind, and get a memtype slot (the heap-id globals 0x2ed1f48..1f60, plus 9f80/9f84 for
the families). Every heap is carved inside one of the nine owners. Several classes share one arena:

- The CPU arena (kind 1) holds GameToRenderThread (slot 3), CPU_RenderToGPUToGame (6), GameSingleFrame (1),
  GameDoubleFrame (2), and **RenderSingleFrameCPU (17 / 2)**. The XPR masks come from RenderSingleFrameCPU and
  the GPU writes them (**observed**, `PC-MEMORY-REWRITE.md` section 6, second run: "the GPU writes them into a
  CPU-kind heap").
- The GPU arena (kind 2) holds GameToGPU (5), RenderSingleFrameGPU (2, GPU_DYNAMIC), MaterialParamHeap and
  VirtualMemoryGPU.

### 1.3 Decision

`Decide(Request{owner, heap_slot}, ClassTable)` (`pc_pool_place.h`):

- **Placement** = the heap rule when the request has a heap context and a rule matches; else the owner rule
  (exact address first, then name and kind); else Unknown. The heap is more specific, because it is the thing
  the game allocates for.
- **Publication** = the placement only when every matching source agrees. A GPU heap in a CPU owner places
  GPU_WRITTEN and publishes Unknown. CPU_ONLY additionally needs the owner to be matched, because a heap alone
  does not say what else the arena holds.

Where the heap context comes from: a thread-local "current heap slot" set by the 0x12820 wrap for the duration of
the call. The refill of a heap's arena block runs inside that call, **inferred** from the call chain
(0x12820 → arena → 0x19ae0) and **open**: the verify stage counts refills that happen with no heap context.
The 0x12820 wrap already exists (`PC_RING_ALLOC`, `scripts/hooks.d/pcring.py`, ~80k calls/s), so the heap slot
should be recorded in the same wrap, not in a second one (`MEMTYPE_ALLOC`, `memtype.py`, conflicts with it).

### 1.4 Is placement per block enough

No, not on its own. A block is requested by an arena, and the arena can hand the same block to a later heap
of another class. The block class is therefore **sticky** (`BlockLedger::NoteCarve`): the first carved heap
class is kept, and a second, different class (or an Unknown one) makes the block Unknown for good. This is
the same rule the granule table already applies to a granule touched by a second heap (`OnGranuleShared`,
`mem_classes.h`).

The structural alternative, **class-pure arenas** (one family object per class and kind, created at 0x8c880),
would make every block class-pure by construction and remove the ledger. It changes game init and the arena
counts, so it is the second step, after the TLS route has shown how many blocks are mixed in practice
(**open**: a counter per block of carves with different classes).

## 2. Question 2: VA ranges per class

### 2.1 The existing layout

**observed** (`reports/04-memory-model.md` section 2, `reports3/06` section 1.1, arithmetic checked here):

| Set | Pool id | VA (observed) | Size | Phys |
|---|---|---|---|---|
| S0 flexible | 0 | 0x123c000000 | 425,984,000 B (6,500 pages, ~406 MiB) | flexible, VA only |
| S1 Onion | 1 | 0x1000300000 .. 0x1040300000 | 1 GiB | from 0x100000, type 0 |
| S2 Garlic | 2 | 0x1040300000 .. 0x123bfe0000 | 8,519,548,928 B (~7.93 GiB) | from 0x40100000, type 3 |

S1 and S2 are adjacent, and S2 ends 128 KiB before S0 starts. The port's pool VA window is
`USER_MIN` 0x1000000000 .. `USER_MAX` 0xfc00000000 (`src/runtime_memory.c`, lines 48-49). **Observed:** the
PS4 code packs pointers into 40-bit fields (`runtime_memory.c` comment at `LOW_MIN`), so every class window must
end below 2^40. The 44-bit readback address (`reports2/07`, the `0x8c1b0` descriptor: low 32 bits plus 12 bits)
is satisfied by the same bound. The 31-bit constraint in `LAB-FAILURE-REPORT.md` concerns the engine command
block below 2 GiB (the image), not pool VA, which is already above 4 GiB.

**observed** (`reports3/06` section 2.2): kind 1 searches S0 first, then its primary set (S1), then the spill set
(S2). Kinds 0 and 2 search S2, then S1. **Inferred:** a CPU block is found in S0 first, so a CPU window that
does not cover S0 would get few CPU blocks.

### 2.2 Why not split the sets or add pool ids

Splitting each set into per-class pools would change the game's pool contract: the early reject sums
(S1 + S2 free VA + free phys), the S0-first search of kind 1, POOL_FIX (0x1a321), the node pool and the owner
tags. That breaks the contract in `reports3/06` section 5 and is rejected.

### 2.3 Proposal: windows inside the existing sets, enforced by the native path

1. **Carve** each set's VA into class windows once, while the set is pristine (no region, no alloc, no backed
   bytes, the same test as `SeedMap`). `CarveWindows` (`pc_pool_place.h`) produces the list and `WindowMap::Set`
   validates it (1 MiB alignment, inside [USER_MIN, USER_MAX), no overlap, placement classes only).
   Indicative layout, to be sized by measurement (**open**):

   | Set | Windows (order) |
   |---|---|
   | S0 (kind 1 first) | CPU_ONLY for the whole range |
   | S1 Onion | CPU_ONLY (most), GPU_WRITTEN (the XPR mask heap, kind 1) |
   | S2 Garlic | GPU_STATIC, GPU_DYNAMIC, GPU_WRITTEN (RTAllocator), CPU_ONLY (spill target, small) |

2. **Realise the split in the free-VA list.** The game keeps one free-VA node per set at init. The pristine
   split writes one node per window into the set's address-ordered list (the nodes come from the node pool at
   root+0x1e0 / +0x1f0 and must be built by the native glue). This is a one-time write under the pool lock.
3. **Place by window in the native path.** `SotcPcPoolNative` today runs exact hits only (no pad, no split,
   no node taken; `FlexHitNode` and `PrimaryHitNode` go to the original). A window fit is a split almost by
   definition, so the native path must learn the split writes (the stores of 0x1aaa0-0x1aaf3 and the split
   branch) before window placement works. This is the critical dependency. Until then placement only changes
   the choice among exact hits, which is measurable but weak.
4. **Fallback paths are class-blind.** The game's own paths (POOL_FIX, the reserve-map path, node refills,
   non-native calls) take whatever node is first. Their blocks are classified after the fact by VA, and
   `Effective()` publishes them only when the window class matches the owner class. That is correct and
   conservative; it costs untracked bytes, never safety.

What a window does **not** give: a guarantee that a window holds only its class. It gives a fast answer to
"which class is this VA" for the fault path and the publish list, and an audit (the mismatch count).

## 3. Question 3: what the port does per class, and what must be true

Correction to the brief (**observed**, `PC-MEMORY-REWRITE.md` section 6, safe run): XPR masks are read two frames
after their write, and 17-32% of mask reads happen before the GPU signals. GPU_WRITTEN therefore cannot be "no
CPU protection" for reads: a CPU read of a GPU_WRITTEN block must fault to a readback. What GPU_WRITTEN drops is
the **write** protection, because a CPU write there is a bug.

| Class | What the port does once placed | What has to be true | How it is checked |
|---|---|---|---|
| CPU_ONLY | Published untracked through `SotcUntrack` (`ApplyRanges`, `SOTC_CLASS_TRACK=1`), kept unprotected | The GPU never binds, samples or writes the block; no texture image overlaps it; the block is class-pure (ledger) and its window is CPU_ONLY | `NoteGpuBind` (**not wired**, section 3.1), `NoteGranuleGpuPages`, `NoteGranuleImage`, the self-check of `SotcUntrack` (trips on a fault) |
| GPU_STATIC | Stays write-protected (cheap: no per-frame faults once built); the build copy is the only CPU write | No CPU write after the build copy | Write faults on a GPU_STATIC block after its build window must stay 0; any hit is a classification error |
| GPU_DYNAMIC | The ring (`SOTC_PC_RING`): arena bytes are declared per submit, not per frame | The GPU reads only bytes written before the submit it runs in; the epoch is the submit | Ring coverage check (`SOTC_PC_RING=4`, byte-range verify): 43% of bound bytes are rewritten in place, so the safe variant is protect-then-copy, not the current `=5` |
| GPU_WRITTEN | No write protection; CPU reads fault to a readback (existing read-fault path); VRAM is authoritative | The CPU never writes the block (any CPU write fault is a trip); each CPU read goes through the readback path; the readback waits for the GPU fence (4-frame bound, not 2: `PC-MEMORY-REWRITE.md` section 6) | Write faults on the block must be 0; `SOTC_RB_CLASS` refusals ("too old") must be 0 with the 4-frame bound |
| Unknown | Tracked as today | none | none |

### 3.1 The bind gate does not exist yet

**observed:** `SotcMemClasses::NoteGpuBind` is defined (`mem_classes.cpp` line 1004) and declared
(`mem_classes.h`), and its comment says it is called from `BufferCache::ObtainBuffer` and from the texture
cache. A search of the port finds **no call site**. The only granule feed that runs is the GPU-page and
image sampling in `buffer_cache.cpp` (around line 6578), which samples about every 250 ms. A sampled
granule can therefore be CPU_ONLY while the GPU is binding it. The sampling catches GPU pages and images, not
a bind that happens between two samples. **Required before any CPU_ONLY publish from placement:** call
`NoteGpuBind(addr, size, cp_frame)` from every buffer and image bind on the draw thread, and verify the bind
counter on the Lab route (**open**).

### 3.2 Why writes on a CPU_ONLY block mean a classification error

**observed** (`mem_classes.cpp`, `OwnerClass`): a write fault only happens on a page the port protects, and the
port protects only pages the GPU reads (bound buffers and images). So a write fault on a CPU_ONLY block is
evidence that the GPU reads it, and the block must be demoted.

## 4. Question 4: recycling leaked VA

**observed** (`PC-MEMORY-REWRITE.md` section 6, combined run): freed Onion/Garlic VA is consumed and unmapped,
never returned: 710 and 50 frees per window in the two measured runs (`PC-MEMORY-REWRITE.md` section 6), as predicted. The game's
free path (`reports3/06` section 2.3) marks the phys bits free, queues the UNMAP and consumes the node's VA.

Recycling takes the VA back from the port's side, never by editing the game's accounting:

1. **Retire** a freed run, after the flush, only when the shadow map (`PhysMap`, pc_pool.cpp) says the whole range is
   unmapped, the window class is a placement class, the run does not overlap a retired run (double free) and
   the bounds are aligned to 64 KiB (`VaRecycler::Retire`).
2. **Quarantine**: not reused before `quarantine_frames` frames (a stale pointer keeps faulting on unmapped
   memory meanwhile, as it does today). The default is a parameter to measure (**open**).
3. **GPU fence**: reused only after the GPU finished every submit up to the retire frame (`gpu_done_frame`).
4. **Cache invalidation**: `MarkInvalidated` only after the buffer and texture caches dropped every entry in the
   run. A run that is only partly covered is not marked.
5. **Class purity**: a run is taken only by a block of the same class, and only inside its window.
6. **Where the VA goes back**: the game's fresh path (`reports3/06` section 2.1, mode 1) asks
   `sceKernelReserveVirtualRange`. The port's `reserve_range` (`src/runtime_memory.c`) can return a recycled run
   instead, aligned as the game asks (1 MiB for S1/S2). The game then creates its node as for any fresh block, and
   its own accounting does not change.

The design choice that makes this safe is the quarantine plus the fence plus the cache gate. Recycling does not
hide a use-after-free from the verify stage: in Quarantine mode the run is never reused and the stale access
still faults.

## 5. Staged rollout (no stage changes the defaults)

Switches (proposed): `SOTC_PC_PLACE` (0 off, 1 observe, 2 verify, 3 apply), `SOTC_PC_PLACE_WINDOWS` (the carve
spec), `SOTC_PC_RECYCLE` (0 off, 1 quarantine, 2 recycle), `SOTC_PC_RECYCLE_FRAMES`.

| Stage | Switch | Behaviour | Pass criterion (verify) |
|---|---|---|---|
| 0 | none | this design and the unit tests only (done) | `pc_pool_place_test` PASS |
| 1 | `PLACE=1` | Decide per block request, BlockLedger, ClassOf per live block; logs and counts only | Counts per class and per owner; refills with no heap context counted (open question); bind counter (after 3.1 is wired) |
| 2 | `PLACE=2` | Native window fit where the native path already runs; the node written is checked against the window; the original result is kept on mismatch | Window mismatches counted (expected near 0 for native, reported for fallback); no kill |
| 3 | `PLACE=3` | Publish CPU_ONLY through `Effective()` (replaces the granule-only rule in `ApplyRanges`) | `SOTC CLASS TRIP` never; GPU bind on a published block = 0; write faults on published blocks = 0 |
| 4 | `RECYCLE=1` | Retire freed runs; count the reuses Recycle would make; never reuse | Retired bytes and would-reuse counts; a stale access on a retired run faults and is counted |
| 5 | `RECYCLE=2` | Reuse retired VA through `reserve_range` | No fault on a recycled run, no cache mismatch, FPS and p99 not worse than base |

Each stage is off by default, has its verify mode, and is gated on benchmarks and the user's ending test, as in
`PC-MEMORY-REWRITE.md` section 7. Stage 2 needs the window split of 2.3 (steps 1-2), and stage 3 needs 3.1.

## 6. What remains to wire

1. **Heap context**: record the 0x12820 memtype slot in a thread-local around the call (same wrap as
   `PC_RING_ALLOC`), and count refills with no context (open question, 1.3).
2. **Owner rules**: the rules are keyed by family object address (`Rule::Match::Addr`), taken from the probe's
   owner rows at run time, with the name and kind rules as the fallback. The table itself is static data, not
   a guess: it must be built from a run that prints the owner addresses (**open**).
3. **Block ledger feed**: `BlockLedger::NoteCarve` from the heap refill (probe_memtype), `Forget` from the
   free path (`FREE_BLOCK`, probe_alloc).
4. **Window split and window fit in the native pool**: the pristine split (section 2.3, step 2) and the split and node
   writes of the window fit (section 2.3, step 3). `pc_pool_native.cpp` is exact-hit only today.
5. **NoteGpuBind call sites** in `BufferCache::ObtainBuffer` and the texture cache bind path (section 3.1). Without
   them CPU_ONLY publish from placement must stay off.
6. **Publish path**: `SotcMemClasses::ApplyRanges` takes `Effective()` instead of the current rule.
7. **Recycler wiring**: `Retire` after the batch flush in `pool_free_block` (FREE_BLOCK at 0x1b0e0), `MarkInvalidated`
   from the cache invalidation, `gpu_done_frame` from the submit fence (gnmdriver PerformSubmit), and the
   `reserve_range` hook for Take.

## 7. Files

- `gpu/sotc/pc_pool_place.h`, `gpu/sotc/pc_pool_place.cpp`: the decision core (pure, no guest access, no lock).
- `gpu/sotc/tests/pc_pool_place_test.cpp`: 22 test functions, 250 checks, PASS. Build:
  `C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -I gpu/sotc gpu/sotc/tests/pc_pool_place_test.cpp gpu/sotc/pc_pool_place.cpp -o <scratch>/pc_pool_place_test.exe`
  (run with `C:/msys64/clang64/bin` on PATH for libc++.dll).

## 8. Open items

1. Refills with no heap context (section 1.3) and the number of mixed-class blocks (section 1.4).
2. Owner addresses of the nine owners on the current eboot (section 6, item 2).
3. Window sizes (section 2.3 table) from the measured live bytes per class.
4. NoteGpuBind wiring and its counter (section 3.1).
5. Quarantine length and the GPU fence source (section 4, items 2 and 3).
6. Whether the native path must split nodes before any window placement shows a gain (section 2.3, step 3).
