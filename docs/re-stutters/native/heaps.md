# N0 inventory: memtype heaps (specialist `heaps`)

Scope: the generic allocator `MEMTYPE_ALLOC` = FUN_00012820 (eboot 0x12820) and the heaps it serves. Code read from the
Ghidra decompilation in `out/re/decomp/<bucket>/<offset>.c` (bucket = top 16 bits of the offset). Runtime evidence from
`user/pcmem/20261101-075018/logs/ring8probe-r1-a1.log` (ring8probe, memtype slot lines, granule claims, class pools) and
`user/pcmem/20261101-085630/summary.md`. Not verified: anything marked "open". No game was run. Nothing committed.

## 1. The allocator and the heap table

- `FUN_00012820(size, slot, align, flags)` (decomp 0001/00012820.c, 292 bytes). The slot indexes a table of heap
  objects at eboot `0x2dfaab0` (`DAT_02dfaab0[slot]`). If the thread has a local heap stack (TLS word at `*FS:-0x18`,
  field `+0x400`) and slot == 1, it uses that instead. It calls the object's vtable `+0xa0` (alloc). On failure it calls
  vtable `+0x98` and `FUN_000124f0` (the out-of-memory path).
- Slot ids are assigned at creation: `FUN_000120d0` and `FUN_00011fb0`/`FUN_00013380` put each new heap object in the
  first free entry (starting at 1) and the id is read back from the object at `+0x1c`. The heap-id globals below hold
  those ids.
- Heap-id globals (set once at init, read by every allocation):
  - `0x2dd9f7c/0x2dd9f80` CPUMemory, `0x2dd9f84` GPUMemory, `0x2dd9f88` GameSingleFrame, `0x2dd9f8c` GameDoubleFrame
    (written in `FUN_00013380`, `FUN_000110a0`; cleared to 1 by `FUN_00016d80`).
  - `0x2ed1f48` GameToRenderThread, `0x2ed1f4c` GameToGPU, `0x2ed1f50` CPU_RenderToGPUToGame,
    `0x2ed1f54` RenderSingleFrameGPU, `0x2ed1f58` RenderSingleFrameCPU, `0x2ed1f5c` MaterialParamHeap,
    `0x2ed1f60` VirtualMemoryGPU (written in `FUN_0008c370`, defaulted to 1 in `FUN_0008c880`).
- Runtime slot numbers (ring8probe log): 1 CPUMemory, 5 GPUMemory, 11 GameSingleFrame, 12 GameDoubleFrame,
  13 GameToRenderThread, 15 CPU_RenderToGPUToGame, 16 RenderSingleFrameGPU, 17 RenderSingleFrameCPU,
  18 MaterialParamHeap, 20/21/22 unnamed (no global names them).

### Creation at boot
- `FUN_00013380` (lazy, first use, 842 bytes): CPUMemory = slot 1 (object at `0x2dfb500`, `"CPUMemory"`). Under it,
  BucketAllocator (slot 2, 16 bins of 64 KiB granules, `FUN_00017990(...,0x10000,0x10,0x400)`), HeapAllocator (slot 3,
  its `FUN_00048910(obj, 0x9600000)` sets a 150 MiB pool), PageAllocator (slot 4). The three sub-allocators are chained
  under CPUMemory with `FUN_00047dd0` (bucket 0x400, heap 0x100000, page unlimited). CPUMemory's own backing is a 0x65690
  byte buffer from `FUN_00017070` (tag table).
- `FUN_0008c370` (called from the render init `FUN_0008f6f0`, 1254 bytes): creates the render and game rings and the two
  4 MiB blocks. Its helpers select the base heap: `FUN_000130d0` returns `DAT_02dfb2b8` (GPU-side base, used for
  GameToGPU and RenderSingleFrameGPU), `FUN_000130f0` returns `DAT_02dfb2c0` (CPU-side base, used for GameToRenderThread,
  CPU_RenderToGPUToGame, RenderSingleFrameCPU). The base assignment is inferred from the call pattern, not verified in
  the helpers' creators (`FUN_00101ee0`, `FUN_00a3da30`, `FUN_0019aad0`, not read).
- The ring objects come from `FUN_0001cfb0(obj, base, depth)` (388 bytes, arg 3 = depth):
  GameToRenderThread depth 3, GameToGPU 5, CPU_RenderToGPUToGame 6, RenderSingleFrameGPU 2, RenderSingleFrameCPU 2.
  Depth 2 for the two render frame heaps means double buffering: the GPU may still read frame N-1 while the CPU writes N.
- `DAT_02ed2108` (MaterialParamHeap backing): `FUN_00012820(0x400000, DAT_02dd9f80)` = 4 MiB from CPUMemory (slot 1).
  `DAT_02ed2110` (VirtualMemoryGPU backing): `FUN_00012820(0x400000, DAT_02dd9f84)` = 4 MiB from GPUMemory (slot 5).
  Each is wrapped in a HeapAllocator-like object (`FUN_00048b70` at `0x2ed3260`, `0x2ed3350`) that is the id in
  `0x2ed1f5c` / `0x2ed1f60`.

### Frame boundary
- `sub_8c860` (entry hooked by MEMTYPE_FLIP_SITE) calls the ring flip `0x136f0` once for `0x2ed1f54` and once for
  `0x2ed1f58`. It runs once per frame after the writers. The SOTC probe logs 18-31 flips/s per window, matching the frame
  rate. Open: whether the flip waits on a GPU label for frame N-2 (that would be a CPU wait on the GPU, see N3). The flip
  body (`0x136f0`) was not read.

## 2. Per heap

Columns: sizes and backing, creator, callers (allocation sites with the runtime calls/MiB per 5 s window, from
ring8probe), lifetime, CPU writers, GPU access, reuse, and the runtime fault rate. "Callers" lists are the decomp
headers' top callers plus the caller addresses the probe reported; the complete list needs `out/re/index.pkl.gz` (not
parsed here).

### 2.1 CPUMemory (slot 1). Faults 2288/s (pool line, 1 Nov). Allocations 702,786 calls/s, 2112 MiB/s.
- Backing: the general CPU heap, under BucketAllocator / HeapAllocator / PageAllocator (`FUN_00013380`).
- Biggest callers, all small strings: `FUN_0000f1a0` (eboot 0xf1e2 and 0xf209: 451,533 calls, 33.3 MiB per 5 s, average
  about 70 bytes). `FUN_0000f1a0` is a copy-string constructor (strlen, alloc, memcpy, refcount); it has many callers
  (`FUN_002b85e0`, `FUN_00139860`, `FUN_00193e50`, ...). Other CPUMemory users: `FUN_0008be60` with the flags bit 1
  set (GPU-visible CPU-side buffers, see 2.2), `FUN_0008c370` (4 MiB MaterialParamHeap backing), `FUN_00013380` setup.
- Lifetime: general heap, small objects, frees not analysed. Frame bytes avg 68 MiB, max 699 MiB (a level-load frame).
- CPU writers: the string copies (CPU only) and the dynamic buffers of `FUN_0008be60`.
- GPU access: the port reports `never-bound 0/s`: every faulting page was already bound to a GPU resource. The GPU reads
  part of CPUMemory: the buffers that `FUN_0008be60` creates with `flags & 1` (see 2.2).
- Granule evidence (ring8probe "granule claims"): CPUMemory owns 31.5k to 32.3k granules, 478 to 1612 granules are shared
  with other slots. **Hypothesis (to verify):** the write faults on CPUMemory come from string churn landing in 64 KiB
  granules that also hold GPU-bound buffers. Strings are CPU-only; the granule is tracked because a GPU buffer sits in the
  same granule. A dedicated host-shared heap for the GPU-visible buffers would separate them.
- Proposed mechanism: keep CPUMemory for CPU-only data (no tracking needed once the GPU buffers move out). GPU-visible
  CPU-side buffers (`flags & 1` in 0x8be60) go to a host-shared heap.
- Patch point: `FUN_0008be60` (see 2.2), not the string constructor.

### 2.2 GPUMemory (slot 5). Faults 1614/s (pool line). 10,739 calls/s, 492 MiB/s. Granules 40k to 42k.
- Creator: slot id stored at `FUN_000110a0` (decomp lines 66, 267, 326 set `0x2dd9f84/88/8c`); the heap object itself is
  created in the same function family (not read).
- Central allocator: `FUN_0008be60` (resource creator, 547 bytes, 108 caller files in the tree). Its arguments:
  `param_1` = resource object (pointer at `+0x10`, size fields), `param_2` = resource type (0x1a/0x1b/0x1c have
  special writers `FUN_00865600`, `FUN_00865500`, `FUN_008655d0`, `FUN_008652d0` that fill the V#/T#-like descriptor),
  `param_3 * param_4` = total size, `param_5` = flags, `param_6` = initial data pointer, `param_7` = ...
  - **Corrected (round 2, from the decomp of 0x8be60):** `lVar2 = param_6` by default; only when `0x800` is clear
    does the function choose storage (`lVar2 = 0` first). So:
    - `0x800` set: **zero-copy.** The caller already owns the block (`param_6`) and it is used as is: no alloc, no
      memcpy. Descriptor only. (Callers allocate it with `FUN_00012820` first, e.g. the particle buffers, see 2.9.)
    - `0x800` clear, bit `0x2` set: **no own storage.** Descriptor only. If `param_6 != 0`, the data is copied into a
      fresh RenderSingleFrameGPU block (`0x2ed1f54`) (transient, per frame).
    - `0x800` clear, bit `0x2` clear, bits `0x18` clear: heap block of `size`, from CPUMemory if bit 0 is set (`0x2dd9f80`),
      else GPUMemory (`0x2dd9f84`). Bit 0 is the only selector here; bits `0x4`, `0x20`, `0x100`, `0x200` do not change
      the storage path (they feed the view-type code after it).
    - `0x800` clear, bit `0x2` clear, bits `0x18` set: size + 4 bytes from GPUMemory, the extra dword is zeroed.
    - The memcpy of initial data (`param_6 != 0`) is inside the `0x800`-clear branch, so it is skipped for zero-copy.
  - Call-site census (decomp, 339 call sites of 0x8be60, script in the scratchpad): flags `0x2` 146 sites (descriptor
    only, 0x1c/0x1b views, the address is set elsewhere), flags `0x4` 85, flags `0` 45 (GPU heap), `0x804` 19 and
    `0x800` 9 (zero-copy: the particle and 0x1b buffers), flags `1` 7 and `5` 6 (CPUMemory-backed, see 2.1), `0x805` 7,
    `0x24` 7, others 4 and fewer.
- Callers that allocate GPUMemory directly with slot 5: `FUN_0008be60` (1851 MiB per 5 s in 17,665 calls, 0xb0d1c7
  in `FUN_00b0ced0`: 28,181 calls, 60 MiB), `FUN_00097a50` (resource creator, 2834 bytes, 21 callers, slot 5 via a heap
  pointer passed by the caller: 0x97ec4/0x98100/0x98320: 213 MiB), `FUN_00096680` (1629 bytes, 3 callers, 0x96b48:
  201 MiB, 218 calls), `FUN_0008f6f0` (2350 bytes, render init, 0x8fe9a and 0x8ff54: 76 MiB in 2 calls), `FUN_001b0440`
  (0x1b0575: 14 MiB).
- Lifetime: level or persistent for resources (large VBO/IBO/textures). Frees and reuse: not analysed (open).
- CPU writers:
  - Initial data at creation: `FUN_0008be60` does `import_memcpy(block, param_6, param_4*param_3)` when `param_6 != 0`
    (decomp lines after the allocation). That single site is an announce point for every creation with initial data.
  - Writes after creation: unknown. The creation returns the object; the writers are the callers of `0x8be60` that
    store into the returned block (open: about 108 callers).
- GPU access: draws read the buffers. Whether the GPU writes some of them (render targets, stream-out) is open.
- Proposed mechanism: host-shared heap for GPUMemory (device-local with host-visible access, or an imported host region)
  so the CPU writes in place. For the creation path use `NativeWrite(block, size)` after the memcpy at `0x8be60`, and
  announce each post-creation write site found by the open caller walk.
- Patch point: the two heap-id selections in `FUN_0008be60` (the `puVar3 = &DAT_02dd9f84 / &DAT_02dd9f80` pair), and the
  memcpy site. Changing the slot id there moves the allocation.

### 2.3 RenderSingleFrameGPU (slot 16). Faults about 1800/s (brief). 16,024 calls/s, 393 MiB/s, 12.6 MiB per frame avg (13.0 max).
- Creator: `FUN_0008c370` (`FUN_0001cfb0` ring depth 2 on the GPU base). Default id `FUN_0008c880`.
- Chunk allocator: `FUN_0008c0d0(obj, size)` (74 bytes). `size = 0 ? obj[8] : size` (default chunk size from the object),
  align 0x10, slot `0x2ed1f54`. It returns the block base in `obj[0..1]`. 156 caller files in the tree. Runtime: 38,118
  calls per 5 s, 1291 MiB (about 34 KB per call, about 254 calls per frame at 30 FPS).
- Material param update, `FUN_00117160` (decomp file 0011/00117160.c, 1982 bytes, "updateDynamicGPUDataJob", line 280):
  `block = FUN_00012820(0x400000, 0x2ed1f54, 0x10)` (4 MiB per frame from RenderSingleFrameGPU), then
  `memcpy(block, DAT_02ed2108 (MaterialParamHeap, CPU), 0x400000)` (line 281), then `FUN_0016eb90(queue, DAT_02ed2110
  (VirtualMemoryGPU), 0x100000 dwords, block, ...)` (line 282): a GPU DMA of the copy into VirtualMemoryGPU. Runtime:
  157 calls per 5 s, 628 MiB, which is one per frame. This is the largest per-frame CPU write in the render path: 4 MiB
  copied by the CPU every frame plus a GPU copy of the same 4 MiB.
- Other callers (runtime, per 5 s): `FUN_0016e990` (0x16ea5f, 54 MiB, 2039 calls; from `FUN_000906d0`), `FUN_00117160`
  (above), `FUN_00103ad0` (0x1043d9), `FUN_000928f0` (0x92a57, 17k calls, 0.13 MiB), `FUN_00095320` (0x95377,
  8.6k calls, 0.33 MiB), `FUN_0017ecf0` (0x17f2f3, 936 calls). The `0x8c0f7` entry is the call inside `FUN_0008c0d0`
  itself, so the real callers are the 156 callers of `0x8c0d0`.
- Lifetime: per frame. Reuse: the flip at `sub_8c860` (`0x136f0`) advances the ring; with depth 2 a block is reused after
  two frames. Whether the flip waits on a GPU label: open (`0x136f0` not read).
- CPU writers: the 0x8c0d0 chunk users (the chunk contents are written by their owners; 156 callers, open), and the
  `memcpy` in `0x117160`. The `0x8be60` transient path (flags `0x2` without `0x800`, round 2 correction) also memcpys
  initial data into this heap.
- GPU access: the GPU reads the chunks in its frame (the port's GPU reads of ring data).
- Proposed mechanism: host-shared ring. Allocate the two frame blocks once from host-shared memory imported once, and
  let the chunk allocator carve from the current block. No per-chunk announce; announce the frame flip (`sub_8c860`) so
  the port knows which block the GPU reads. Keep the DMA only if the GPU copy is still needed (see 2.6).
- Patch point: `FUN_0008c370` (where the ring base is created: host-shared base instead of the GPU base) and
  `FUN_0008c860` entry (frame boundary, already hooked).

### 2.4 RenderSingleFrameCPU (slot 17). Small: 1466 calls/s, 2.7 MiB/s, 0.087 MiB per frame.
- Creator: `FUN_0008c370` (`0x2ed1f58`, ring depth 2, CPU base).
- Callers (per 5 s): `FUN_000ca150` (0xca17b, 6.7 MiB), `FUN_000c7850` (0xc7aa8, 3.5 MiB), `FUN_000c12a0` (0xc14b4, 0.69),
  `FUN_000c5560` (0xc55a5), `FUN_000c95c0` (0xc9603, 0xc9868).
- Lifetime: per frame, ring of 2. GPU access: not seen in the port's data (open: whether any bound GPU resource lives here).
- Proposed mechanism: CPU-only ring, no tracking needed. If the GPU does read it, it must be host-shared (same as 2.3).

### 2.5 MaterialParamHeap (slot 18). CPU, 4 MiB block at `DAT_02ed2108`. 978 calls/s, 0.54 MiB/s.
- Creator: `FUN_0008c370` (heap object `0x2ed3260` over the 4 MiB CPUMemory block).
- Sub-allocations: `FUN_000f5cd0` (452 bytes, callers `FUN_000f8270`, `FUN_000f6c90`; 4,922 calls per 5 s, 2.7 MiB).
- Per frame: the whole 4 MiB is copied to RenderSingleFrameGPU and then DMA'd to VirtualMemoryGPU (2.3 and 2.6).
- CPU writers: the material param writers that call `FUN_000f5cd0`/the heap object (open: the names of those callers).
- Proposed mechanism: make the frame's material block itself the host-shared ring (2 blocks). The CPU writes params into
  the current block, the GPU reads them there (or through a GPU alias of the same block). Removes the 4 MiB memcpy and
  the DMA. Announce only the bytes written (the heap object already knows its allocations).
- Patch point: `FUN_00117160` lines 280-282 (the memcpy and the DMA call), and the heap object at `0x2ed3260` (allocator
  base).

### 2.6 VirtualMemoryGPU (`0x2ed1f60`, backing `0x2ed2110`, GPUMemory). GPU-written by DMA.
- The 4 MiB block is filled each frame by a GPU DMA (`FUN_0016eb90`, PM4 DMA_DATA in the job) from the frame copy in 2.3.
  The CPU does not read it. Counted in GPUMemory's fault total (granules owned by slot 5).
- Proposed mechanism: alias: the GPU reads MaterialParamHeap's host-shared frame block directly, so the DMA and the copy
  disappear. If the DMA must stay, the port performs it as a host copy at submit time (no CPU write tracking needed).

### 2.7 GameSingleFrame (slot 11) and GameDoubleFrame (slot 12). CPU only, tiny.
- Slot 11: 21 calls/s, 0.20 MiB/s, caller `FUN_000a3ee50` (0xa3ee9f, 105 calls, 0.98 MiB). Slot 12: 7 calls/s, no bytes.
- Not GPU-visible in the data. Only the granule claim (shared granules) can make them tracked. No action beyond keeping
  them out of GPU classes.

### 2.8 GameToRenderThread (13), GameToGPU (4, `0x2ed1f4c`), CPU_RenderToGPUToGame (15). Message rings.
- Slot 13: 53 calls/s, 0 MiB/s, callers `FUN_004f6f40` (0x4f6fb8, 0x4f72d8). Slot 15: 63 calls/s, 0 MiB/s, callers
  `FUN_00198bb0` (0x199bb8, 206 calls), `FUN_000afcc0` (0xafdea, 109 calls).
- GameToGPU is a depth-5 ring on the GPU base; no calls observed in the probe (allocation data not seen).
- Command-sized (near zero bytes); not a fault source. Open: whether the GPU reads CPU_RenderToGPUToGame (name suggests
  commands): the calls are tiny, so it is not in the fault top list.

### 2.9 Slot 20, 21, 22 (unnamed in the probe, creators found in round 2 for 20 and 22).
- Slot 20: 21 calls/s, 93 MiB/s, 3.0 MiB per frame avg, 469 MiB max. Callers all in `FUN_00097a50` (resource creator,
  2834 bytes; 0x98100: 293 MiB in 55 calls, 0x97ec4: 93 MiB, 0x98320: 57 MiB). 5,160 granules at boot (about 320 MiB):
  large resources (meshes, textures) at level start. Granule owner: slot 20 only, never shared.
- Slot 22: 152 calls/s, 3.6 MiB/s, callers `FUN_000ffe60` (0xffeb0: 18 MiB in 629 calls). 49 granules.
- Slot 21: 10 granules, no calls seen.
- **Round 2, creators resolved from the decomp:**
  - Slot 20 = **RTAllocator** (render targets). `FUN_0009aef0` (render_targets_allocate, 19 KB) creates it at line 473:
    `FUN_000475c0(obj, "RTAllocator", 2)` (GPU family), when the display mode is 1920x1080 (`param_2[2]==0x780`,
    `param_2[3]==0x438`). Its backing block comes from `FUN_000479a0(family, 0x14280000 or 0x23d10000, 0x10000, ...)`:
    0x14280000 = 322 MiB (base PS4), 0x23d10000 = 600 MiB (Neo, `sceKernelIsNeoMode`). That matches the log line
    "RTAllocator blocks (322 MiB)" (drawbest, 1 Nov) and the 5,160 granules of slot 20 at boot. The render-target
    resources (`kRTT_*` names, 26 calls to `FUN_00097a50` from `render_targets_allocate`) are the 293 MiB of slot 20.
    Lifetime: allocated at mode setup, persistent for the session.
  - Slot 22 = the **particle renderer's buffers** (likely; the heap id is passed in by the caller). `FUN_000ffe60`
    (318 bytes, called by `FUN_001051f0`, `FUN_00103ad0`, `FUN_00107540`; the eboot callers are
    `ParticleRenderer::UpdateSimulation` and `ParticleRenderer::BeginFrame`) allocates three blocks with
    `FUN_00012820(size, *param_2, 0x10)`, where `*param_2` is the caller's heap id, then makes zero-copy descriptors
    with `FUN_0008be60(..., type 0x1b, flags 0x804, block)`. Sizes: stride*count (count = `param_3`/`param_4`), 0x70
    bytes times count, and 4 bytes times 9 times count. The ParticleRenderer writes the blocks each frame (to confirm: the
    writers are the functions that contain the callers' eboot addresses 0x103fc2, 0x105710 (UpdateSimulation) and
    0x109703 (BeginFrame); `FUN_00103ad0` is the function containing 0x1043d9, a slot-16 caller in the same renderer). Slot 22 at
    runtime: 152 calls/s, 3.6 MiB/s, and `FUN_000ffe60` alone is 18 MiB in 629 calls per 5 s.
  - Slot 21: no creator found yet (10 granules, no calls). The next step is the callers of `FUN_000479a0`.
  - Remaining check: which global holds the particle heap id (the value of `*param_2` at the call), to confirm it is 22.

### 2.10 Totals and the write faults of the render path (1 Nov)
- Frame bytes written by the CPU into GPU-visible memory (estimate, from the probe): RenderSingleFrameGPU 12.6 MiB avg,
  of which 4 MiB is the material copy (2.5, 2.3). GPUMemory creations run about 98 MiB/s (492 MiB per 5 s, about 3.3
  MiB per frame at 30 FPS, see 2.2; spikes at level load). CPUMemory buffers unknown.
- Fault counts: CPUMemory 2288/s, GPUMemory 1614/s (pool line, ring8probe), RenderSingleFrameGPU about 1800/s (brief).
  **Open:** whether the port counts one fault per 4 KiB page or per granule. The material copy alone is 1024 pages per
  frame (4 MiB), about 30k pages/s at 30 FPS if counted per page, which does not match 1800/s. The unit must be checked in
  `gpu/sotc/` before the per-heap numbers are compared.

## 3. Sites by class (heaps)

| # | site (eboot) | what | class | proposed patch point |
|---|---|---|---|---|
| H1 | 0x12820 generic allocator | all heap allocations | dispatch (all heaps) | none (the allocator is the choke point; use the heap id) |
| H2 | 0x8be60 resource creator, memcpy of initial data | copy into a new buffer | announce-write (per-creation) | after the memcpy: NativeWrite(block,size) |
| H3 | 0x8be60 heap-id selection (`&DAT_02dd9f84/80`) | GPUMemory / CPUMemory choice | placement | change the slot id per flag: host-shared heap for GPU-visible buffers |
| H4 | 0x8be60 transient path (flags 0x2 without 0x800) | initial data to RenderSingleFrameGPU | per-frame write into ring | host-shared ring (2.3) |
| H5 | 0x8c0d0 chunk allocator (156 callers) | per-frame chunks, 254/frame | per-frame, ring | host-shared ring base in 0x8c370; writers open |
| H6 | 0x8c370 ring creation (depth 2, bases 0x130d0/0x130f0) | base of the frame rings | setup | host-shared base |
| H7 | 0x8c860 frame boundary (sub_8c860, flip 0x136f0) | ring flip | frame fence | announce frame; check whether the flip waits on GPU (open) |
| H8 | 0x117160 updateDynamicGPUDataJob, memcpy 4 MiB | material params to frame block | per-frame CPU copy (4 MiB) | drop the copy: MaterialParamHeap is the frame block |
| H9 | 0x117160 DMA (0x16eb90) into VirtualMemoryGPU | GPU-side copy | GPU write (per frame) | alias the GPU read to the host-shared frame block, or host copy at submit |
| H10 | 0x97a50 resource creator, slot 5/20/22 | big resources (level load) | persistent, load | announce on creation (NativeWrite of the loaded data) |
| H11 | 0x96680 (slot 5, 3 callers) | resource data | persistent | announce as H2 |
| H12 | 0xf1a0 string copy (CPUMemory, 451k calls/5 s) | CPU-only strings | CPU-only | none for the GPU; moves out of granules shared with GPU buffers (2.1) |
| H13 | 0xf5cd0 material param sub-allocations (slot 18) | CPU writes into MaterialParamHeap | per-frame CPU write | the material block (H8) |
| H14 | 0x16e990 (slot 16, 54 MiB per 5 s, caller 0x906d0) | frame data | per-frame | host-shared ring (H5) |
| H15 | 0x928f0, 0x95320, 0x17ecf0, 0xc5560/0xc95c0 (slot 16/17) | small per-frame chunks | per-frame | host-shared ring |

Count: 15 sites of the heap model (H1-H15). Write-announce or placement sites: H2, H3, H4, H5, H6, H8, H10, H11, H13, H14.
GPU-side writes: H9. Sync: H7 (open). CPU-only: H12, 2.7, 2.8.

## 4. Proposed PC-native mechanism per heap

- CPUMemory (1): CPU-only. Move GPU-visible buffers out (H3). Expected: CPUMemory faults fall to near zero.
- GPUMemory (5): host-shared heap for the resource buffers; announce the initial-data write (H2, H11); announce the
  post-creation writes after the open caller walk. Largest persistent GPU data (1851 MiB of creations per 5 s window
  in 0x8be60 alone, mostly level load).
- RenderSingleFrameGPU (16): host-shared two-block ring; the chunk allocator carves from the current block (H5, H6, H4);
  announce the flip (H7). Writers of the chunks: open (156 callers).
- RenderSingleFrameCPU (17): CPU-only ring, no GPU tracking if the GPU does not read it (open).
- MaterialParamHeap (18): becomes the host-shared frame block (H8, H13). Removes the 4 MiB copy per frame.
- VirtualMemoryGPU (`0x2ed1f60`): alias of the material block, DMA removed (H9).
- Slots 20-22: find their creators first; they are level-load resources (H10), announce on creation.
- Strings and small CPU objects: no change.

## 5. Open questions (with what would resolve them)

1. Who writes the GPUMemory buffers after creation (108 callers of `FUN_0008be60`): walk the callers of `0x8be60`,
   find the stores into the returned block (`param_1[4]`). Resolved by a decomp walk over `out/re/index.pkl.gz`.
2. Who writes the RenderSingleFrameGPU chunks (156 callers of `0x8c0d0`): same walk on `0x8c0d0`.
3. Creators of slots 20, 21, 22: grep the call sites of `FUN_00047b20`, `FUN_000475c0`, `FUN_00013990` with a name
   string, and the heap constructors that run after `FUN_00013380`.
4. Does `FUN_00136f0` (ring flip) wait on a GPU label for frame N-2? A CPU wait on the GPU would be a sync site (N3).
5. Frees and reuse of GPUMemory and CPUMemory blocks (the vtable `+0x98`/free path of the heap objects).
6. The meaning of the flags of `0x8be60` (bits 0, 1, 3-4, 8, 11): which resource types set `0x800`, `0x1`, `0x2`.
7. The fault unit (per page or per granule) in `gpu/sotc/` (see 2.10). It changes how the per-heap rates compare.
8. `FUN_0016eb90` (the DMA): confirm it is a DMA_DATA packet and what the port already does with it.
9. The base objects `DAT_02dfb2b8` / `DAT_02dfb2c0`: their creators (`FUN_00101ee0`, `FUN_00a3da30`, `FUN_0019aad0`).
   **Resolved in round 2, see section 6.**

## 6. Round 2: answers to the open questions

Tool: `tools/re/eboot_re.py` (callers / callees / fstr / func), run from the main tree. Scripts in the scratchpad.

### 6.1 Heap families and base objects (creators)
- `FUN_000110a0` (memory_system_init, 2739 bytes) creates the two base heaps used by the render rings:
  - `DAT_02dfb2c0` = "Default TaggedArena", kind 1, chunk 0x4000 (16 KiB), created with `FUN_00017070(0x5d0)`.
    `FUN_000130f0` returns it: the **CPU base** (GameToRenderThread, CPU_RenderToGPUToGame, RenderSingleFrameCPU).
  - `DAT_02dfb2b8` = "Default TaggedArena", kind 2, chunk 0x100000 (1 MiB). `FUN_000130d0` returns it: the **GPU base**
    (GameToGPU, RenderSingleFrameGPU). This resolves the question of 9 and confirms the attribution in section 1.
  - In the same function: CPUMemory's families (`BucketAllocator` `FUN_00013990`, `HeapAllocator`, `PageAllocator`),
    GameSingleFrame (`FUN_000475c0(...,"GameSingleFrame",0)`).
- Family kind (the 3rd argument of `FUN_000475c0`, inferred, not verified in the constructor): 1 = CPU, 2 = GPU, 0 = frame
  ring. Used by: GameToRenderThread 0, GameToGPU 0, CPU_RenderToGPUToGame 0, RenderSingleFrameGPU 0,
  RenderSingleFrameCPU 0, XPRTempData 0, MaterialParamHeap 2, VirtualMemoryGPU 2, RTAllocator 2.
- Family creation sites in `render_context_register_heaps` (0x8c880, 3479 bytes): the 8 `mem_family_ctor` calls
  (0x8ca2f ... 0x8cfca). Its first block (lines 12-51) sets the slot globals to 1, then the families are constructed.
  The ring attach (`FUN_0001cfb0(obj, base, depth)`) is in `FUN_0008c370`, called from `FUN_0008f6f0`.
- Ring objects get their vtable in `render_context_register_heaps` (`DAT_02ed29c0 = &PTR_FUN_01594ca0`, and the same
  pattern for `DAT_02ed2120`, `DAT_02ed2400`, `DAT_02ed2ca0`). The eboot data in `out/eboot.elf` at those addresses reads
  as zero (bss, written at runtime), so the vtable contents were not read.

### 6.2 Slots 20-22 (done in 2.9)
Slot 20 = RTAllocator (render targets, 322 MiB), slot 22 = particle renderer buffers (`FUN_000ffe60`), slot 21 open.

### 6.3 The ring flip `FUN_000136f0`: does it wait on the GPU?
- `FUN_000136f0(param_1)` (146 bytes, callers `FUN_00a3f140`, `FUN_0008c860`, `FUN_0025ae80`): reads the heap id `*param_1`,
  returns at once if it is 1 (CPUMemory), otherwise calls the heap object's **vtable entry +0x20** (a virtual flip,
  `goto` indirect). So the flip's behaviour is whatever the ring family's `+0x20` method does.
- The ring family vtable is `PTR_FUN_01594ca0` (set in `render_context_register_heaps`). Its `+0x20` target was not
  resolved: the vtable is not in the static file data I could read (`out/eboot.elf` reads zero at 0x1594ca0 in my
  scratch reader; the address may need the rodata segment mapping). **Open.** Resolution: read the vtable from
  `out/boot.bin` or the PT_LOAD of the rodata, then `eboot_re.py dis` on the `+0x20` target and check for a call to a
  label wait (the EOP wait helper, or `WAIT_REG_MEM` in the port's labels).

### 6.4 Frees and reuse
- Not resolved in round 2 (the free paths are the heap objects' vtable slots, `+0x98`/`+0x...`). The port already models
  the game's block allocator in `gpu/sotc/pc_pool*.cpp` (the brief says "fully modelled"); the frees are the pool's
  `Free` path. Open: the exact free vtable slot per family kind.

### 6.5 The meaning of the 0x8be60 flags
Corrected in section 2.2 (zero-copy `0x800`; no-storage `0x2`; bit 0 CPU vs GPU in the plain path; `0x18` size+4 GPU).
The flag bits `0x4`, `0x20`, `0x100`, `0x200` do not change storage: they feed the view-type code after the allocation
(the `0x3c` mask decides the descriptor variant in `FUN_008652d0`).

### 6.6 Writers of the 0x8be60 buffers after creation (partial)
- Census of all 339 call sites by the flags argument (section 2.2): 146 descriptor-only (`0x2`), 19+9 zero-copy
  (`0x804`/`0x800`), 45 plain GPU (`0`) and 85 plain GPU with bit 2 (`0x4`), 13 CPU-backed (`1`, `5`).
- Zero-copy writers that are known: `FUN_000ffe60` (particles, slot 22) and `FUN_0019aad0`, `FUN_0019c7e0`,
  `FUN_001af960`, `FUN_000fedc0`, `FUN_000afcc0`/`FUN_000b0100`, `FUN_0011dbe0`, `FUN_000ceae0`, `FUN_000f5cd0`
  (MaterialParam sub-buffers, slot 18). These are the sites whose block the caller writes itself, so the caller is the
  writer. Each needs its per-frame write site named before the NativeWrite patch.
- Most of the 146 descriptor-only sites (`0x2`) have a null block at creation (`param_6 == 0`; a few pass a local
  buffer): the view's address is set later by the owner. Their writers are the owners of the storage they view, not `0x8be60`.
- Full walk of the 108 callers and their stores into the returned object: **not finished**. The callers command gives
  the list (`eboot_re.py callers 8be60`); each caller needs its `param_1+4` store traced. This is the next task.

### 6.7 Writers of the 0x8c0d0 chunks
Not walked in round 2 (156 callers). Same method as 6.6. Open.

### 6.8 Fault unit (reconciled)
- Faults are host page-protection faults: `page_manager.cpp` protects 4 KiB pages (`PM_PAGE_SIZE`, `VirtualProtect` at
  4096), and `mem_classes.cpp::NoteWriteFault` / `pc_pool_place_glue.cpp::Glue::OnWriteFault` count one fault per first
  write to a protected page. A page stays unprotected until it is re-protected, so a fault count is the number of
  protected pages written, not the number of bytes written.
- Reconciliation: the material copy writes 1024 pages per frame (4 MiB), about 30,700 page-writes per second at 30 FPS.
  The measured 1,800 per second for RenderSingleFrameGPU is about 60 faults per frame, so most of those pages are not
  protected at write time. That fits the design: only pages the GPU has read since the last protect fault. The
  fault figures measure "CPU writes to GPU-read pages", not "CPU bytes written". The per-heap rates are therefore not
  comparable to byte totals.

### 6.9 H12 (string churn sharing granules with GPU buffers): verified from the decomp
- `FUN_0000f1a0` allocates its string with `FUN_00012820(size, 1, 8, 0)`: slot 1 = CPUMemory, align 8 (decomp
  0000/0000f1a0.c, the `lVar1 = FUN_00012820(uVar2,1,8,0)` line).
- The CPU-backed GPU buffers of `FUN_0008be60` (flags `1` or `5`, 13 call sites) allocate from slot 1 too (the plain
  path picks `DAT_02dd9f80`), align 0x10. Both use the same CPUMemory object tree (BucketAllocator for <= 0x400 bins,
  HeapAllocator below 1 MiB, PageAllocator above), so a string and a GPU-bound CPU buffer can be placed in the same
  64 KiB granule. **Structurally confirmed.** The granule sharing counts (478 to 1612 shared granules) are consistent
  with that. Whether it causes the CPUMemory fault rate is still a hypothesis: it needs the size distribution of the 13
  CPU-backed buffers and the granule owner counts per window.

### 6.10 Summary of round 2 status
- Done: 6.1 (creators, GPU/CPU base), 6.2 (slots 20, 22), 6.5 (flags), 6.8 (fault unit), 6.9 (H12 structure).
- Partial: 6.6 (classes known, full writer walk not done), 6.4 (frees).
- Open: 6.3 (flip vtable +0x20), 6.7 (0x8c0d0 writers), slot 21, the particle heap id.
