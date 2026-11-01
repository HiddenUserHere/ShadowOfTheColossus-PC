# Plan: make the game PC-native (no UMA emulation): no page tracking, no fault readbacks, nothing waits on the GPU

## Context

The port emulates the PS4's unified memory (UMA): it write-protects guest pages to learn what the CPU wrote,
faults and reads back GPU data when the CPU touches it, and compares memory regions to decide what to upload. That
machinery is the stutter: the draw thread spends ~30-34 ms per frame (GPU only 37% busy), most of an op's cost is
binding with page scans/uploads/re-protects, readbacks are serviced on the draw thread, an area load re-uploads
139 MiB in one frame, and guest threads wait on GPU results. Every fix so far (ring8, generation memos, classes,
readback tricks) is another layer of that emulation.

The user's direction (1 Nov): stop emulating UMA. Patch the game (eboot) and rewrite the parts of the engine's GPU
interface so the game behaves like a native PC engine: it TELLS the port what it wrote (explicit uploads), it never
reads GPU memory directly, and NO CPU thread ever has to wait for the GPU. Then delete the page tracking, the fault
readbacks and the region comparisons for good. Constraints already decided: 30 FPS locked and built for 60; NVIDIA,
AMD, Intel Arc; shader-cache rebuild, newer drivers and divergence from shadPS4 accepted; no extra RAM/VRAM; no added
input latency; clean code, delete every switch that is not default or proved useless (vendored shadPS4 options
included); SOTC_DROP_FAST off by default; diagnostics consolidated into SOTC_FPS, SOTC_TRACE, SOTC_VERIFY. No game
runs until the user says so.

## Execution rules (from the user)
- Execute the whole plan, N0 through N5, end to end without stopping between stages; stages are an internal order of
  work and merge points, not pauses for approval.
- Do NOT open the game (no sotc.py, no matrix runs, no traces) until the user authorizes it; everything is verified by
  build, syntax checks, unit tests and code review until then. The verify modes are built so the first authorized
  sessions can prove each stage.
- Every agent is a specialist of one area, briefed as such, and told to go deep: when searching, never stop at the
  first finding; map the whole area, cross-check against the decompilation and the runtime evidence, and report what
  remains unknown.
- Up to 12 Haiku agents at a time, in their own worktrees; Ghidra decompilation (`out/re/decomp`) first, raw assembly
  only to confirm patch bytes. I merge, build, fix integration and commit after each merge point.

## The model (what "PC-native" means here)

| PS4/UMA today | PC-native target | Mechanism |
|---|---|---|
| CPU writes GPU-visible memory anywhere; port detects by write faults | The engine announces writes: per-frame data goes to host-shared memory the GPU reads in place; persistent data is uploaded once when the engine finishes writing it | Hooks/patches at the engine's own write points (allocators, resource loader copies, constant/instance updates) call `sotc_native` APIs |
| GPU writes memory the CPU may read; port reads back on fault | The CPU never reads GPU memory. Each GPU->CPU consumer is removed, or fed without waiting (below) | Patches at each consumer |
| CPU threads wait on EOP labels for the GPU | Waits only where a native engine waits: the frame-pacing fence for the swapchain. Labels the game polls for results disappear with their consumers | Patches + label table |
| Page bitmaps, protects, region compares, readback-on-fault, ring modes, classes | Deleted | Stage N4 |

### Readbacks: removed, not hidden
A readback exists only because CPU game logic consumes a GPU result. For each consumer, in this order of preference:
1. Eliminate the consumer: do the work on the GPU or drop the feature. XPR visibility/occlusion masks (eboot
   0x1a6d20/0x1a6d60): the GPU has ~60% headroom, so disable the occlusion-mask round trip (treat all as visible, or
   keep the culling on the GPU with indirect draws) and pay the extra GPU work instead of a CPU wait. Measure the GPU
   cost first.
2. Compute it on the CPU from data the CPU already has (if a particle count is derivable from the emitters).
3. Only if neither works: the consumer reads the last completed value without waiting (a native-engine pattern;
   visual data one frame old, no input latency). Each such case is listed and approved individually.
Particle stats (0x101a98, the 0x8c1b0 stats ring) and any other consumer found in N0 go through the same decision.

## Stages

### N0. Reverse-engineer the engine's GPU interface (inventory)
- Map every CPU->GPU write path and every GPU->CPU read path, with REA (reverse-engineer-anything skill) on the
  eboot plus the runtime evidence the port already produces: write-fault RIPs (SOTC_ARENA_ATTRIB, fault attribution),
  readback RIPs ("SOTC READBACK RIPS"), allocator/memtype probes (heap names, owners), the resource loader
  (0x8be60/0x96680, SyncPointProcessPendingResources 0x1480f0), XPR/particle hooks, label waits (WAIT_REG_MEM keys).
- Output docs/re-stutters/native/INVENTORY.md: each site, its class (per-frame, persistent, GPU-result, sync), the
  patch point, and evidence. Completion metric: with page tracking in verify mode, every write fault and every
  readback in a long session is attributed to an inventoried site.
- Source of truth for code reading: the Ghidra decompilation we already have, NOT raw assembly. Per-function C files
  in `out/re/decomp/<bucket>/<offset>.c` (249 buckets, exported by `tools/re/ghidra/ExportDecomp.java`), names in
  `out/re/ghidra/names*.txt` and `tools/re/names.txt`, the index `out/re/index.pkl.gz`, and `tools/re/eboot_re.py`
  (signatures, offsets). Disassembly (REA, objdump) only to confirm a patch site's exact bytes.
- Agents: up to 12 Haiku specialists in parallel (worktrees), each an expert of one area, told not to stop at the
  first finding: allocators/heaps (2: CPU/GPU memory heaps; TaggedArena/Bucket/Page/HeapAllocator), resource loader
  and streaming, render-frame data (constants, instances, skinning), command-buffer building, XPR visibility masks,
  particle stats, other GPU result consumers (queries, labels the game polls), sync and frame pacing (event flags,
  EOP waits), textures/render targets, the fault/readback attribution of the current runtime (to close the
  inventory), and the switch audit second pass.

Stages N1-N5 also run as parallel Haiku specialists (up to 12 at a time, worktrees, Ghidra decomp first), merged and
built by me.

### N1. The native API (`gpu/sotc/native/`)
- `NativeWrite(addr, size)` (persistent data finished: upload once, on upload workers, budgeted), host-shared
  regions for per-frame data (ring8 generalised by placement: the native pool allocator gives each per-frame heap
  its own VA window, imported once; no granule classification), `NativeFree`, a frame-pacing fence API.
- Bind path becomes a lookup: guest address -> (buffer, offset) by region, no sync scan; textures validated by the
  loader's writes, not by page state.

### N2. Patch the engine's write paths
- Hooks/patches (gpu/sotc/hooks + scripts/hooks.d, sotc_patches.cpp) at each inventoried write site call the API.
- Verify mode (SOTC_VERIFY): page tracking still runs and counts writes the API did not announce; must be 0 in long
  sessions before N4.

### N3. Remove GPU->CPU consumers and GPU waits
- Per the readback rule above; each consumer patched, measured (GPU cost, visual check by the user).
- Labels: the game no longer polls result labels; the only remaining waits are frame pacing. Verify mode counts any
  CPU read of GPU-written memory and any label wait outside the frame fence (must be 0).

### N4. Delete the UMA emulation
- Delete page protection for GPU tracking, GPU-modified marks, readback-on-fault, region compares, generation memos
  that only existed to avoid scans, ring modes, memtype/class probes, placement verify scaffolding, and every switch
  of the audit classed DELETE (including unused vendored shadPS4 options). Diagnostics consolidated into SOTC_FPS,
  SOTC_TRACE, SOTC_VERIFY. SOTC_DROP_FAST off by default.

### N5. A modern, high-performance render architecture
With the memory model native, the renderer no longer needs per-op resource discovery, so the execution side is
rebuilt around current-generation patterns (refactored in stages on top of the shadPS4 shader recompiler):
- **Resources created at the source.** Buffers and textures are created when the engine allocates/loads them (N1
  hooks), never discovered at bind time. A bind is an index.
- **Bindless everything.** One descriptor heap for all images and samplers (descriptor indexing), buffers through
  buffer device addresses with a GPU-side table (the SOTC_BDA groundwork, made safe for host-shared memory by
  importing with the device-address usage). Shaders read V#/T# and index the heap themselves: no per-draw descriptor
  writes, no push-descriptor churn. Recompiler changes accepted (shader cache rebuilt once).
- **Frame graph.** The CP turns each frame's PM4 stream into passes with declared reads/writes; barriers, layout
  transitions and transient aliasing are computed once per pass from the graph, not per op. Render passes merge by
  target set.
- **Parallel recording.** Passes are recorded on a worker pool (secondary command buffers or one primary per
  worker), stitched in submission order; per-op CPU cost target well under 1 us, so one frame of ~6,500 ops fits in a
  few ms across workers (60 FPS ready).
- **Async queues.** Uploads on the transfer queue (N1 upload workers), independent compute passes on an async
  compute queue, synchronized with timeline semaphores; the graphics queue only does graphics.
- **Pipelines.** All PSOs prebuilt from the cache at boot; cold misses fast-linked with graphics pipeline libraries;
  pipeline keys from dirty registers.
- **Present.** Frame pacing and acquire/present on the present thread; the frame fence is the only CPU wait.
- Feature probe per device (NVIDIA, AMD, Intel Arc); each feature has a fallback until its stage gate passes on all
  three, then the fallback is deleted.

### Wave 3 work in flight (paused by plan mode)
- Keep and finish only what survives the new model: opcost's compute bind cache (N5), loadburst's upload workers
  (N1 upload path), gameside's diagnostics (evf-set trace, thread CPU, yield stats; the hitch classifier's
  "gpu-chain" label), taxonomy (finish the report: it sizes N5), switchaudit (second pass with the user's rules: it
  drives N4). rbthread's stale-redo change is superseded by N3 (readbacks removed): drop it.

## Critical files
Eboot hooks/patches: `gpu/sotc/hooks.cpp`, `gpu/sotc/hook_handlers.cpp`, `scripts/hooks.d/*.py`,
`gpu/sotc/sotc_patches.cpp`, `scripts/sotc_modes.py`. Allocator/placement: `gpu/sotc/pc_pool_native*.cpp`,
`pc_pool_place_glue.*`. Memory model to replace then delete: `gpu/shadps4/video_core/buffer_cache/*`,
`page_manager.cpp`, `texture_cache/texture_cache.cpp`, `gpu/sotc/pc_ring*`, `mem_classes*`, `rb_class*`,
`static_class*`, `probe_*`. Labels: `gpu/shadps4/video_core/amdgpu/liverpool.cpp`. Launcher/tools:
`launcher/sotc_tuning.py`, `sotc.py`, `tools/pcmem_matrix.py`.

## Verification
- Build and syntax checks per change; Python tests and gpu/sotc unit tests.
- When the user allows game runs: SOTC_VERIFY sessions (unannounced writes 0, CPU reads of GPU memory 0, label
  waits outside the frame fence 0), one-round matrix per stage, SOTC_TRACE free-play sessions analysed with
  `tools/trace/sotc_trace.py hitch`, and the user's visual check of every removed GPU->CPU consumer.
- Final gate: no page faults from GPU tracking, no readbacks, no CPU wait on the GPU except frame pacing; p99 <= 36 ms
  and max < 50 ms at 30 FPS over the Lab route and a 20-minute session; owner draw work <= 12 ms for 60.
