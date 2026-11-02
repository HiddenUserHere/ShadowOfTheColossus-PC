# Draw architecture: the GPU sees the PS4 address space (design, 2 Nov)

Refines PLAN.md stage N5 into a concrete design, from what PERF2-PERF4 measured.

## Why the current model cannot get fast
- A frame is ~3,150 draws plus dispatches (Lab route field, "SOTC BDA draws" 523k per 5 s) and 4,500-6,000 CP
  commands. The draw thread spends ~25 ms of real work per frame: ~8 us per op.
- That time is resolution, not recording. For every op it reads every V#/T#/S# again and looks each one up in
  general caches: buffer cache intervals (ObtainBuffer), texture cache images and views (FindImage, FindView),
  layout tracking (Transit), descriptor writes (push descriptors or the BDA block table), barrier bookkeeping.
  BindResources alone is 31-41% of the thread.
- Four waves of memos (PERF2-PERF4) made individual lookups cheaper and the verify runs were clean. The thread did
  not get faster, because each memo still costs a key build, a validation and a fallback per binding. The BDA block
  table made it slower: it rebuilds a per-draw table (~6 MB/s) and still calls ObtainBuffer for 3 of 4 buffers
  (those in arenas, not in ring-8 served memory).
- Conclusion: the per-binding CPU step has to go, not get cheaper. The PS4 GPU itself never resolves anything on a
  CPU: a V# is a GPU virtual address plus a size, a T# is a descriptor the texture unit reads directly. The port
  should give the shaders the same thing.

## Target
- Draw thread at most 1.5 us per op (about 5-9 ms per frame, single thread), with parallel recording below 3 ms.
- Command processor below 8 ms per frame (it also limits 60 FPS).
- No extra RAM or VRAM over today (arenas and the mirror replace each other); NVIDIA, AMD, Intel Arc; shader cache
  rebuilt once.

## The design: five parts

### 1. GPU memory mirror: one buffer whose offset is the guest address
- One VkBuffer per GPU-visible guest window, created sparse (sparseBinding + sparseResidencyBuffer, both true on the
  test GPU). Its offset `o` holds guest address `window_base + o`. Its device address is taken once.
- Residency follows ALLOCATION, not binds: when the game maps GPU memory (the memtype / map_direct hooks of
  runtime_memory.c and probe_memtype.cpp already see every allocation), the 64 KiB pages of that range get device
  memory bound with vkQueueBindSparse on a side queue. Allocation events are hundreds per second; binds are a
  million. Unmap releases the pages after the frame fence.
- Contents: CPU writes reach the mirror through the existing native announce path (SotcNative announces, the drain
  uploads dirty ranges at submit start: today it uploads into arenas; it uploads into the mirror instead). GPU
  writes land in the mirror directly. Native mode has no readbacks, so nothing has to come back.
- Per-frame dynamic data (the ring-8 arena, constant buffers the engine writes every frame) stays zero-copy: the
  imported host runs keep their own device addresses. The shader picks the window with one compare
  (address inside the per-frame arena window -> host base, else -> mirror base), from two push-constant words.
- This replaces the buffer cache's arenas and placements for native mode, and with them ObtainBuffer,
  SynchronizeMemory, residency touches, GPU-modified sets and the BDA block table.

### 2. Buffers: the shader computes its own address
- The recompiler already has a BDA backend (EmitContext::BdaLoad/BdaStore/BdaAddress, the guarded atomics,
  spirv_emit_context.cpp ~920-990): today BdaAddress loads (block address, block size) from the per-draw block table.
- Change BdaAddress's source: base and size come from the V# words the shader already has in its flat buffer
  (Info::RefreshFlatBuf copies user data and the sharp tables there for every draw, CP side), and the device address
  is `window_device_base + (vsharp.base - window_base)`. Bounds: the V# num_records/stride, as the hardware does.
  No table, no CPU step per buffer.
- Texel (formatted) buffers: typed loads are done in the shader from the format bits (the recompiler converts
  formats already for its buffer paths); the few that cannot keep a descriptor in the heap (part 3).
- Vertex fetch: the fetch shader is already turned into buffer loads; it uses the same addresses.

### 3. Textures and samplers: one descriptor heap, resolved per table, not per draw
- One global heap (VK_EXT_descriptor_heap, present on the test GPU; fallback: descriptor indexing, universal):
  every image view and sampler gets a permanent index when it is created; descriptors are written once.
- VK_KHR_unified_image_layouts (present on the test GPU): every image stays in GENERAL with no penalty, so there is
  no per-bind layout tracking (Transit) and no descriptor per layout. Barriers become plain memory dependencies
  between passes (part 5). Fallback: GENERAL everywhere (correct, small cost on some hardware).
- The T#->index step moves to the command processor and runs per TABLE, not per draw: the CP already reads the
  sharp tables for the pipeline lookup (SharpFetch, spec memo at 100% hits). A table's content (its dwords) maps
  to an "index table" (the heap indices of its T#/S# entries) through a content hash memo; only a changed table
  pays the texture-cache lookup (FindImage + view creation). The draw carries the index table's address.
- The texture cache keeps its job (creating images, uploading, detecting overlaps) but it is called on table
  changes and on image events, not on binds.
- Later step (optional): a GPU-side T# hash table so even changed tables resolve on the GPU, with a feedback
  buffer for misses. Not needed for the target.

### 4. One pipeline layout, one push-constant packet
- All pipelines share one layout: the heap, plus push constants: the flat-buffer address, the index-table address,
  and the two window bases (32 bytes).
- Per op the draw thread does: bind pipeline (when changed), push 32 bytes, set dynamic state that changed
  (viewport/scissor/blend constants), draw or dispatch. No descriptor sets, no push descriptors, no per-binding
  barriers. Expected well under 1 us per op.
- The flat buffer (user data + sharp tables) is written by the CP into a per-frame ring it owns (it builds it today
  for the pipeline key); the draw thread only passes its address.

### 5. Passes, barriers, parallel recording
- With resources addressed by the shaders, the hazards are per pass, not per binding: the CP groups ops into passes
  by render target set and compute runs (the frame-graph idea of N5), with one global memory barrier where a pass
  reads what a previous one wrote (the CP knows the written ranges: storage writes are declared by the shader info).
- Passes are recorded in parallel on the worker pool (secondary command buffers per pass, or one primary per worker
  with the queue order kept), which now scales because recording is pure (no shared cache state).
- Optional afterwards: VK_EXT_device_generated_commands (present on the test GPU) for long runs of draws with the
  same pipeline, if recording still shows in the profile.

## What goes away
BindBuffers / ObtainBuffer / SynchronizeMemory / NativeDrain into arenas / the BDA block table / BindTextures'
second pass and layout tracking / push-descriptor writes / per-bind native notes (write extent, native copies,
gpu-modified set) / most of the memo switches of PERF2-PERF4 (they become dead and are deleted at the end).

## Phases (each behind a switch, =2 verifies against the old path, the gate is a measured number)
| Phase | Work | Verify (=2) | Gate |
|---|---|---|---|
| A0 | Measure: ops/frame, us/op of the draw thread and the CP, at the Lab route and the Shrine | | baseline table |
| A1 | Unified image layouts (probe + fallback), Transit and layout tracking off | image layouts never consulted give the same barriers | BindTextures share drops |
| A2 | Mirror: sparse window(s) bound from allocation hooks; announce drain uploads into the mirror | mirror bytes == guest bytes for sampled ranges after each drain | no missing residency in a run |
| A3 | Recompiler: BdaAddress from V# words + window bases; all storage/uniform buffers through it | shader-side address == ObtainBuffer's address for every binding (CPU-side replica of the formula) | BindBuffers and ObtainBuffer out of the profile |
| A4 | Heap + per-table index memo on the CP; draw passes the index table | index table == today's per-bind resolution | BindTextures out of the profile |
| A5 | One layout, push-constant packet, no descriptor sets | draw output identical (screenshots, N1 verify) | draw thread <= 1.5 us/op |
| A6 | Passes and parallel recording | barrier set == per-op barriers it replaces | draw thread < 3 ms/frame wall |
| A7 | Delete the dead paths and switches | | code size, all three vendors |

## Risks and how each is handled
- Sparse binding cost and limits: binds are batched per allocation on a side queue with a timeline semaphore; the
  page granularity (64 KiB) wastes at most one page per allocation edge. Vendor limits are probed at boot.
- Shader cost of address math and bounds checks: one add and one compare per buffer per invocation (hoisted, as the
  BDA backend already hoists blocks); measured in A3 against descriptor loads on all three vendors.
- Out-of-range V# accesses: bounded by num_records like the hardware; robustness no longer comes from the
  descriptor, so the guard is in the generated code (the BDA backend's `valid` path).
- Formatted buffers that cannot be emulated cheaply keep a heap descriptor (counted in A3).
- Memory: the mirror replaces the arenas; VRAM is checked at every phase against the current report (2.0 GB used of
  6.5 GB on the test GPU).
- Intel Arc and AMD: each extension has a fallback (descriptor indexing for the heap, GENERAL layouts, no DGC); the
  fallback is deleted only after its phase gate passes on all three.
