# Engine heap map (static analysis, 5 Nov 2026)

Purpose: the input for placing each engine heap in its own address region, so that pages the GPU writes
never hold CPU-written data (the `SOTC_NATIVE_TRACK=3` write faults come from CPU writes into such pages).
Everything below comes from the eboot alone (`tools/re/eboot_re.py` plus a backward register tracer);
the game was not run. Rows are verified in code unless marked **guess**.

## How an allocation picks its heap

- Every engine allocation goes through `sub_12820(size, memtype, align, flags)`.
- `memtype` is an index into the heap registry at data `0x2dfaab0` (`registry[id]` = heap object).
- Ids are assigned at run time by `sub_120d0`, which gives the first free index >= 1. They are not
  constants; callers read them from "slot" globals that the heap constructors fill.
- `memtype == 1` first takes the thread-local scope path inside `sub_12820` (CPUMemory family scopes).
- A heap's kind byte (`obj+0x18`: 1 CPU family, 2 GPU family) is copied from its parent by `sub_1cfb0`.
  The parents are two `Default TaggedArena` objects: `0x2dfb2c0` (kind 1, returned by `sub_130f0`) and
  `0x2dfb2b8` (kind 2, returned by `sub_130d0`).
- Ring heaps flip with `sub_136f0(slot)`, which calls `registry[*slot]->vtbl+0x20` (no-op for slot 1).

## Heaps

| Heap | Slot global | Kind | Created in | Who writes the data (basis) |
|---|---|---|---|---|
| CPUMemory family | `0x2dd9f7c`, `0x2dd9f80` (and memtype 1) | 1 | `cpu_memory_family_init` 0x13380 | CPU only; render-context chunks and C++ `new` (PC-MEMORY-REWRITE class table) |
| GPUMemory family | `0x2dd9f84` | 2 | `memory_system_init` 0x111dc | CPU once at build, GPU reads (mesh VB/IB copies); **open**: GPU-written usages of `sub_8be60` land here too |
| GameSingleFrame | `0x2dd9f88` | 1 | `memory_system_init` 0x11738 | CPU only |
| GameDoubleFrame | `0x2dd9f8c` | 1 | `memory_system_init` 0x1194d | CPU only |
| GameToRenderThread | `0x2ed1f48` | 1 | `render_context_register_heaps` 0x8ca2f (id stored by `sub_8c370`) | CPU only |
| GameToGPU | `0x2ed1f4c` | 2 | `render_context_register_heaps` 0x8cc1e | CPU writes, GPU reads |
| CPU_RenderToGPUToGame | `0x2ed1f50` | 1 | `render_context_register_heaps` 0x8cdf4 | CPU; name suggests GPU results back to the game: **open** |
| RenderSingleFrameGPU | `0x2ed1f54` | 2 | `render_context_register_heaps` 0x8cfca | CPU writes, GPU reads (view constants, instances, particles, descriptors); 2-slot ring |
| RenderSingleFrameCPU | `0x2ed1f58` | 1 | `render_context_register_heaps` 0x8d1a0 | **GPU writes** the XPR visibility masks here, CPU reads them (readback) |
| MaterialParamHeap | `0x2ed1f5c` | 2 | `render_context_register_heaps` 0x8d53c (0x400000 block) | CPU writes, GPU reads |
| VirtualMemoryGPU | `0x2ed1f60` | 2 | `render_context_register_heaps` 0x8d5c0 (0x400000 block) | streaming texture window: CPU/DMA writes, GPU reads |
| XPRTempData | not found | 0 | `render_context_register_heaps` 0x8d376 (16 MiB per the engine's own OOM message) | XPR visibility temporaries: **likely GPU-written** (guess) |
| PhysX ring | `0x32f5dd0` | 2 (parent is the GPU arena; the old class table said 1) | `sub_a3da30` (0x1000000 block) | **open** |
| HeapAllocator, PageAllocator (CPU children) | none | 1 | `cpu_memory_family_init` 0x134e3, 0x135c0 | CPU only |
| HeapAllocator (second) | none | 2 | `memory_system_init` 0x11323 | **open** (registration not resolved) |
| BucketAllocator | none | ? | `sub_13990` | **guess**; candidate owner of the small-object allocator whose chunk headers fault (`sub_179c0`/`sub_17e90`) |

Not in the registry: render targets (`RTAllocator`, `render_targets_allocate` 0x9aef0, GPU-written) and
the stats/water rings read through `0x8c1b0` (GPU-written, read back by the CPU).

Ring flips (verified): `GameFrame_Update` 0x25b00b-0x25b042 flips `0x2dd9f88`, `0x2dd9f8c`, `0x2ed1f48`,
`0x2ed1f4c`, `0x2ed1f50`; `sub_8c860` flips `0x2ed1f54` and `0x2ed1f58`; `sub_a3f140` (0xa3f1c8) flips
the PhysX slot. GameSingleFrame and GameDoubleFrame use vtable `0x1594c90`, whose +0x20 (`sub_13d10`)
only reads a byte, so their flips advance nothing statically (run time not checked). No flip was found
in `ParticleRenderer::BeginFrame` (0x107540), against the earlier report.

## Call sites

| Entry | Sites | memtype source |
|---|---|---|
| `sub_12820` direct | 299 | 106 memtype 1 (CPU scope path); 166 from slots: `0x2ed1f54` 126, `0x2dd9f84` 17, `0x2dd9f80` 9, `0x2ed1f58` 7, `0x2ed1f50` 3, `0x2dd9f8c` 2, `0x2dd9f88` 1, `0x2ed1f5c` 1; 5 forwarded by wrappers; 22 unresolved (21 a load through a pointer set on several paths, 1 branch join) |
| Wrappers forwarding memtype | `sub_17130`, `sub_17140`, `sub_17170`, `sub_17180`, `sub_95320` | their 57 resolved callers use slots `0x2ed1f58` 27, `0x2ed1f54` 13, `0x2dd9f84` 10, `0x2ed1f48` 6, `0x2dd9f88` 1 |
| `sub_8be60` (GPU buffer wrapper) | 341 | usage flags in r8d (5th argument): bit 0x2 picks RenderSingleFrameGPU (`0x2ed1f54`), otherwise GPUMemory (`0x2dd9f84`), with a third path at 0x8bf06 (`cmov`, data `0x15fe920`). Usage values: 0x2 146, 0x4 84, 0x0 48, 0x804 18, 0x800 10, 0x1 7, 0x24 7, 0x805 7, 0x5 6, 0x20 4, 0x100 2, 0x200 1 |
| `sub_8c0d0` (descriptor alloc) | 234 | always RenderSingleFrameGPU (`0x2ed1f54`) |

Per-site data: `heap_sites.csv` and `heap_names.md` (session scratchpad; regenerate with the tracer
script `heap_sites.py` if needed).

## What this settles and what it does not

- Settled: the engine allocates from about a dozen named heaps, each chosen through one entry point with
  a slot global. A hook at `sub_12820` (and at the block requests of each heap) sees the heap of every
  allocation, including heaps of areas not visited yet, so placement by heap needs no route coverage.
- Not settled statically: which heaps hold pages the GPU writes. Known or likely: RenderSingleFrameCPU
  (XPR masks), XPRTempData, the `sub_8be60` usages that create GPU-writable buffers inside GPUMemory
  (flag meaning not decoded yet), render targets, the stats rings. The per-heap GPU-written page count is
  a run-time number: `SOTC_HOOK_MEMTYPE_PROBE=1` with `SOTC_MEMTYPE_CLASSES=1` reports it per heap on the
  automated Lab route (`user/routes/tests.json`), about two minutes, no manual play.
- Embedded metadata (BPE heap block headers, small-object chunk headers) shares pages with the data of
  whatever heap it serves; placing heaps in regions does not move it.

## Run-time measurement (5 Nov 2026, Lab route `user/routes/tests.json`)

`SOTC_NATIVE_TRACK=3 SOTC_HOOK_MEMTYPE_PROBE=1 SOTC_MEMTYPE_CLASSES=1 SOTC_CLASS_TRACK=2`, route done
(132 frames). The probes are heavy (669k hook calls/s at `0x12820`), so the frame times are not comparable.

| Heap (registry id) | Granules (64 KiB) | GPU-written pages (last sample) | Write faults/s |
|---|---|---|---|
| CPUMemory (1) | 32,322 (2.0 GiB) | 0 | **2,228** |
| GPUMemory (5) | 43,025 (2.6 GiB) | 24,358 | 421 |
| slot 20 (unnamed) | 5,160 (322 MiB) | 19,923 | 0 |
| RenderSingleFrameGPU (16) | 1,365 (85 MiB) | 0 | 0 |
| slot 22 (unnamed, particle code range) | 108 (6.8 MiB) | 0 | 0 |
| RenderSingleFrameCPU (17) | 4 | 0 | 7 |
| slot 21 (PhysX ring), CPU_RenderToGPUToGame (15) | 5, 1 | 0 | 0 |

- 2,232 granules are shared by two heaps (a 64 KiB granule holds the end of one heap's block and the start of
  another's).
- About 84% of the write faults fall in CPUMemory granules. The GPU-page column is a rotating sample (512
  granules per 250 ms, a full pass takes ~16 s), so its 0 for CPUMemory means the sample missed them, not that
  the GPU never writes there: a TRACK=3 write fault only happens on a page the GPU wrote.
- So the GPU writes into CPU-family memory: memtype 1 allocations made through the render-context scope
  (command buffers and other per-context chunks come from the CPU family) and whatever else lives in the
  shared granules. Which CPUMemory allocations those are is the next thing to find (the allocation caller of
  each faulting granule).

## Fault owners (`SOTC_FAULT_OWNERS=1`, same route, 5 Nov 2026)

Each TRACK=3 write fault charged to the last allocation that touched its 64 KiB granule (heap and the
allocation's caller). 9-12k faults per window, all inside claimed granules. Top owners over three windows:

| Owner (heap, allocation caller) | Faults per window | Allocation sizes seen |
|---|---|---|
| CPUMemory, `sub_fffa0` 0x1001ff (particle code range) | 1.9-2.5k (2 windows) | 0x5460-0x5910 |
| CPUMemory, `ResourceFactoryResourceLoader::PrepareForLoad` 0x14b459 | 0.4-2.8k | 0x634-0x9e09 |
| CPUMemory, `ResourceFactory::CreateResource` 0x147241 | 0.2-2.1k | 0x235c-0x3d607 |
| GPUMemory, `sub_8be60` 0x8bf0b (GPU buffer wrapper) | 0.4-1.6k | 0x50-0xe0 |
| CPUMemory, render contexts: `sub_917c0` 0x91906 (0x500000, the DCB), 0x918d5, `sub_906d0` 0x90741, `RequestNewRenderContext` 0x904fd | 0.2-0.6k each | 0x238-0x500000 |
| CPUMemory, `sub_10f760` 0x10fb75, `sub_561c40` 0x5622bb, `sub_16e990` 0x16ea1e | 0.3-0.9k each | 0x418-0x20000 |
| RenderSingleFrameCPU, `RenderThread::RunFunctionAsRenderJob` 0xc14b4 | ~150-200 | 0x100 |

Reading (not verified): the resource loader's blobs are CPU staging the GPU never writes (class table), so
their faults point at memory reused after a free that still carries the GPU-written state of its previous
owner. The render-context chunks are per-frame command memory the CPU rewrites every frame. "Last
allocation in the granule" is a heuristic for granules shared by many small allocations.

## Mark age, and the fix it led to (5 Nov 2026)

The fault-owner report also stamps each GPU write (`BufferCache::AddGpuModified`) per 4 KiB page and gives the
age of the page's mark at each write fault. Of 9-12k faults per window, 7-12k had no GPU write stamp at all:
they were not merges of GPU-written pages but the shadPS4 tracker's ordinary CPU-write detection on clean pages,
which `SOTC_NATIVE_TRACK=3` runs in full. `SOTC_NATIVE_TRACK_GPU_ONLY=1` (page_manager.cpp, `GpuOnlyProtect`)
write-protects only pages with a read watcher (GPU-modified). With the opening cinematic on: write faults
55-314/s (eboot) and 0-9/s (host modules) against 1.4-4.4k and 0.3-0.7k/s, FPS unchanged (31/29/30/30/28/28),
terrain and other visuals correct (user check). The heap placement above is no longer needed for the faults.

## What is left with `SOTC_NATIVE_TRACK_GPU_ONLY=1` (Lab route, same probes)

323 / 748 / 1,735 write faults per window (65-350/s); every one has a GPU write stamp, most under 0.1 s old:
these are real mixed pages. Owners: GPU buffers from `sub_8be60` (0x8bf0b) take 17-50%, many of their faults at
`+0xffd8` of a 64 KiB chunk (the small-object allocator's chunk header, writer `sub_179c0` 0x17cf1, on the last
page of a chunk whose rest holds GPU buffers); the rest is spread over dozens of owners of 20-60 faults each
(RenderSingleFrameCPU via `RenderThread::SubmitJob`, CPU_RenderToGPUToGame, render-job and particle data).
Writers: the allocator header, particle stats (`ParticleSystemPool::ProcessStats` 0x101ae4), BPE heap metadata
(`BpeHeap_Link` 0x4949c, `BpeHeap_Free` 0x48f07, the `sub_12820` path 0x128ff), `RenderThread::SubmitJob`
0xc55c1, `sub_1d750` 0x1d849. At ~81 us per merge this is 5-28 ms of CPU per second spread over the guest
threads; no single site is worth an allocator change.

## Allocation-site registry: exact GPU/CPU classification (2026-11-06)

The owner tables above charge a fault to the last allocation in its granule. The registry replaces that with
one record per allocation site. Inputs: the static true-sharing and allocator reports, and one measured run.

### The registry (`SOTC_ALLOC_SITES=1`)

- Code: `gpu/sotc/alloc_sites.{h,cpp}`. Needs `SOTC_HOOK_MEMTYPE_PROBE=1`.
- Key: each allocation from `sub_12820` is keyed by its three return addresses and its heap slot.
- Charges to an allocation: buffer binds (with the recompiled shader's `is_written`), image uses, and
  TRACK=3 write faults.
- Written binds are "own" when the bind starts at the allocation and fits in it; otherwise "foreign".
- Output: per-site table `user/alloc_sites.tsv`.
- Lifetime rules:
  - An allocation ends at the generic free `0x12af0` (hook MEMTYPE_FREE).
  - The class frees `0x17e90` (MEMTYPE_FREE_SMALL) and `0x493d0` (MEMTYPE_FREE_BPE) never end an
    allocation that the generic free did not end.
  - The realloc `0x12c50` (MEMTYPE_REALLOC) was never called in the measured runs.
  - An allocation with no free is a ring-heap block; the next allocation over it ends it.

### Measured result (2026-11-06)

- 190 GPU-written sites. 185 have only own written binds.
- 5 are foreign-only: 4 render-target pool sites (slot 20), and the BuildXPR decal block `0x1b397a`
  (the written view sits at offset ~0x2350 inside a CPUMemory block).

### Mixed sites: which pass writes them

A site is mixed when some of its allocations are written by the GPU and others are not. "Can the GPU write
it" is a property of the site. Whether the pass ran for a given object varies.

| Site | Writing pass | Rule |
|---|---|---|
| Skin outputs, `sub_192f20` (A[0] / A[1]) | skin compute `0x193810` (variant A) or `0x193aa0` (variant B) | A[0] or A[1] by frame parity; the two are exact complements (measured) |
| CreateSourceIndices, `obj+0x160` (`sub_193f00` / `sub_194a00`) | `sub_194a00` dispatches | once per rebuild of the object |
| StaticModelSection, `sub_1af960` | IndexCullObjects | after the section becomes visible |
| BuildXPR decal, `sub_1b32f0` | decal cull compute `sub_19c7e0` | binds the records as written; only culled sets |
| InstanceData, `sub_122b70` / `sub_122c30` | "Update GPU Data" compute `sub_123410` | ~2% of objects get own GPU writes (2 of 135 for `sub_122b70`, 7 of 453 for `sub_122c30`); the CPU writes them only at init |

### True sharing

- A static scan of all GPU-written sites (the three static reports) found none. Each CPU write into a
  GPU-written buffer is either before that buffer's first GPU use, or to other bytes of the same page.
- Still open in those reports: post-creation writers of the stored fields were not searched across the
  eboot, and the particle `UpdateSimulation` dispatches were not checked.

### Owner-change discard (`SOTC_NATIVE_OWNER_DISCARD`, `BufferCache::NativeDiscardOwner`)

- Immediate discard on the engine thread crashed 3 of 3 runs at `0xf4f567`: a CPU loop reading index data got
  `0xfefe` indices. Cause: an asynchronous readback of the old contents was still in flight and landed after
  the page was unprotected. An in-flight readback can only be marked stale on the draw thread.
- Deferred discard at the command-processor point (the default now; the immediate path needs
  `SOTC_NATIVE_OWNER_DISCARD_NOW=1`): no crash, but the faults do not change (390 / 1240 / 2082 per 5 s against
  326 / 1271 / 2272 baseline). The engine writes a fresh block before the deferred discard runs.
- Conclusion: stale marks must not arise at all. Next step: GPU-written sites get a port-owned region, and
  their frees go through a quarantine that discards on the draw thread before the memory is reused.

### Port constraints from the allocator RE (static)

- Pointer bits: `0x17e90` (bucket free) and `0x18160` (bucket realloc) take the chunk base from the pointer
  with a chunk-end mask and read the descriptor at `[E-0x18]`. A foreign pointer reads and writes at
  `E-0x38..E-0x10`, so both need an entry hook.
- Classifier `sub_19a50`: maps a pointer to its owning heap through the pool class tables (64 KiB granules
  inside the pool window, 1 MiB granules outside). Unknown pointers get 0. Callers: `0x12af0`, `0x12c50`,
  `0x47840`, `0x47c20`, `0x47f30`. `0x47c20` and `0x47f30` dereference the owner with no NULL check, so a
  foreign pointer crashes there.
- Heap-wide release: `0x47620` (free-all) and `0x47740` (deleting dtor) free nodes through `pool_free_block`
  (`0x1b0e0`) without passing `0x12af0`.
- No header or footer is read before a user pointer outside the BPE heap (`0x493d0`). No 40-bit pointer
  packing was found.
