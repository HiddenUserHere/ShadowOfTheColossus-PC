# 15 - Frame-time bursts: 64 MiB binds, camera turns and area loads

Read-only analysis. Nothing was built, run or committed. The only file written is this one.

Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c` (the directory is the high 16 bits of
the address, so `0x112b80` is `decomp/0011/00112b80.c`), `tools/re/eboot_re.py` (function ranges),
the port's code under `gpu/`, and the reports in `docs/re-stutters/`. Addresses are eboot VAs.

Labels: **observed** (code or decompile read here, with location), **reported** (from a report or
trace that was not re-run here), **inferred** (from the observed facts, not tested), **unknown**.

Note on the symptom: the 64 MiB request size and the "5-13 ranges" were reported by the user from
`SOTC_BIG_SYNC_LOG`. The log lines were not in the repository, so the figures are **reported**.

---

## 1. Summary

- The 64 MiB request is **not an engine buffer size**. It is the port's cap on an unbounded
  read-only binding (`SHADPS4_UNBOUNDED_BUFFER_MB`, default 64). The engine's own V# is larger, so
  the port clips it. **observed** (code), which buffer it is: **unknown**.
- The copy is **not the window**. The copy is the sum of CPU-dirty page runs inside the window.
  It reaches 64 MiB because every **fresh 16 MiB tracking region starts entirely CPU-dirty**, and a
  64 MiB window covers 4-5 regions. **observed** (constructor fill) / **inferred** (that this is the
  spike's content).
- The bind copies all dirty runs on the draw thread, in one staging buffer, with no per-frame cap
  and no budget. **observed.**
- Camera turns add draws (one per newly visible object) and first binds of their data. Each first
  bind of a 16 MiB region copies that region, so turn spikes are quantised in 16 MiB units.
  **inferred.** The split between buffers, textures and pipelines in a turn is **unknown**
  (no turn trace is analysed here).
- Area loads add three bursts that are already mapped: the completion drain
  (`SyncPointProcessPendingResources`, no budget), mip copies (`0x98ae0` from job `0x112d30`, no
  budget), and the first bind of every mesh region. **observed** (code) / **reported** (timings).

---

## 2. Burst 1: the 64 MiB bind syncs

### 2.1 Where the 64 MiB comes from (observed)

The clamp is in two places, with the same rule:

| Site | What it does |
|---|---|
| `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4173-4181` (`PrResolveBuffer`) | Prepared path: for a read-only binding with `vsize >= 0xFFFF0000` or `size > 256 MiB`, and `size > window`, sets `size = window`. |
| `vk_rasterizer.cpp:8530-8553` (`BindBuffers`) | Inline path, same rule. Logs `Limiting ... MB binding` only when `vsize < 0xFFFF0000`, so the common unbounded case (`num_records` near 4 GB) is silent. |

Rule (observed): `window = SHADPS4_UNBOUNDED_BUFFER_MB << 20`, default 64 MiB, so `0x4000000`.
Written bindings (`desc.is_written`) are never clamped. The comment states the reason: guest code
marks buffers unbounded with `num_records` near 4 GB and touches only part of them.

So a 64 MiB request means: the shader's V# had `num_records` of at least `0xFFFF0000` (or more
than 256 MiB), it is read-only, and the bind base is the V# base. The base is `0x108c20fbe0`. That
is not 64 KiB aligned, so it is an offset inside a heap, not a buffer start. It lies inside the
8 GiB direct-memory mapping that starts at `0x1040300000` (**reported**, FIRST-FRAME-PEAK phase 0b).

**Which engine buffer owns it: unknown.** Candidates, all **inferred**:

- A shader-addressed read (`PrResolveBuffer` computes `out.shader_addressed` for such buffers): the
  game passes a pointer and the shader reads with an unbounded V#. This matches the lack of a fixed
  size.
- A raw descriptor the engine copies into its bump memory (docs 06 and 09): the SH user-data bump
  and the frame rings (130 MiB CPU ring, 20 MiB GPU ring, docs 09 section 2.1). A 64 MiB window
  lies inside a 130 MiB ring, but the ring base is not known from the static analysis.

To identify it at run time (no build needed by this document): log `pgm_hash`, stage, `num_records`
and the descriptor index at the clamp. The current `LOG_WARNING` skips exactly this case (see above),
so the log needs a new line, not a change of the existing one.

### 2.2 Why the bind copies so much (observed, with one inference)

`BufferCache::ObtainBuffer` (`gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5870`) runs
`EnsureResident`, then `SynchronizeMemory` (line 5998, or the early-sync path at 5997). For a
read-only bind the sync is `SynchronizeMemoryFull` (line 8496):

1. `memory_tracker->ForEachUploadRange(device_addr, size, is_written, ...)` (line 8512) collects
   the CPU-dirty page runs inside `[device_addr, device_addr + size)`. The CPU bit is cleared and
   the page re-protected for each run (`SOTC_PROTECT_BATCH` applies the protects together).
2. The total is `total_size_bytes`. `SOTC_BIG_SYNC_LOG` prints it (line 8574 block, first 40
   syncs of 8 MiB or more, gameplay only), so the user's log already contains the copied bytes.
3. The copy path (default switches, from the headers):
   - host import (`host_import.h`, off by default, line 8609);
   - async upload (`async_upload.h`, off by default, line 8640; the minimum is `SOTC_ASYNC_UPLOAD_MIN_KB`,
     256 KiB);
   - otherwise one staging request for the whole total (line 8650), then the guest-to-staging copy
     on the draw thread. Runs of 2 MiB or more are split into 512 KiB pieces run with
     `BbCopy::ParallelFor` (lines 8736-8755), and the draw thread waits for all of them.
   - then `runtime.CopyBuffer(staging, arena, ...)` (line 8807).

**Why the whole dirty range goes at once (observed):** nothing caps it. There is no piece budget, no
per-frame byte cap on this path, and the bind waits for the copy. `SOTC_LOAD_STREAM` has a per-frame
cap, but only for ranges the session already bound (see 2.4).

**Why the dirty set is large (observed in the constructor, inferred for the spike):**

- `RegionManager`'s constructor fills the CPU bits with ones: `cpu.Fill(~0ULL)`
  (`gpu/shadps4/video_core/buffer_cache/region_manager.h:416-418`).
- `CreateRegion` (`memory_tracker.h:593-615`) takes a fresh slot from the manager pool
  (`MANAGER_POOL_SIZE = 32`, line 23). A fresh slot is all-dirty, and the trace event
  `redirty first touch` is recorded there. A region is `HIGHER_PAGE_SIZE = 2^24 = 16 MiB`
  (`region_definitions.h:17-18`).
- A recycled slot is not refilled: `SetCpuAddress` (`region_manager.h:440-450`) keeps the old bits.
  The release path was not read, so how often a recycled slot appears is **unknown**.

So the first bind of a window that covers regions no bind has touched copies those regions in full.
A 64 MiB window covers exactly 4 regions when aligned to 16 MiB, and 5 when it is not (a partial
region at each end). The reported "5-13 ranges" fits that: each region contributes runs that are split
by the per-region iteration (**inferred**). Up to about 64 MiB per bind, and at 3.3 GB/s for first-touch memory (FIRST-FRAME-PEAK,
**reported**), that is about 20 ms of draw-thread copy, plus the page protects after it.

The arena is also a 64 MiB chunk unit (`ARENA_CHUNK_SIZE`, `buffer_cache.h:1108`), so the first bind
also pays arena residency (`EnsureResident`), which is outside this estimate.

### 2.3 Fixes, ranked

**Fix 1A. Clean fresh regions in idle time, at region creation (port, no game hook).**
Put every region created by `CreateRegion` on a queue. The draw thread drains it in idle gaps, or
the copy threads take it, using the same primitives as `DrainRange` (`load_warm.h`:
`IsSyncClean`, `SynchronizeMemory`, the write fence). A drained region's pages are re-protected by
`ForEachUploadRange`, so later writes are tracked. The first bind then finds the region clean, and
the spike is gone for regions drained before use.

- Correctness: the same as an early bind. A region drained while the CPU still writes is re-dirtied
  by the next write, and the next bind copies it again.
- Cost: the upload includes CPU-only data (the BPE heap lives in the GPU pool, reports 04). That costs
  copy bandwidth and VRAM, not correctness. A class filter (`SOTC_CLASS_TRACK`, `mem_classes.h`)
  can drop CPU-only regions from the queue.
- Expected effect: removes the 64 MiB spike for regions drained earlier. The load measured about
  640-750 MiB of first-touch regions (FIRST-FRAME-PEAK), about 200-230 ms of copy at 3.3 GB/s,
  spread over the load's idle time.
- Risk: medium. VRAM use on a nearly full card (`#137` in `buffer_cache.cpp` shows this was an issue).
- Verify: `SOTC_BIG_SYNC_LOG` count and bytes at reason 1, same route before and after.

**Fix 1B. Measure the window before changing it (no behaviour change).**
Add a log at the clamp (`pgm_hash`, stage, descriptor index, `num_records`) and count, per bind,
how many 16 MiB regions were fresh. This decides whether Fix 1A, Fix 1C or a smaller window is the
right fix for these binds.

**Fix 1C. Shrink the unbounded window (experiment).**
`SHADPS4_UNBOUNDED_BUFFER_MB` is already a switch. Try 8 and 16 on the Lab route. The copy shrinks
only where dirty runs fall outside the cut. Risk: a shader that reads beyond the window gets stale
or missing data. The window is already a correctness compromise, and the same risk is in the
rasterizer comment. Not a default change without a visual check.

**Fix 1D. Async upload for large binds (exists, off by default).**
`SOTC_ASYNC_UPLOAD=1` sends runs of at least 256 KiB to the copy threads (`async_upload.h`). The
draw thread stops copying; the next submit waits for the copy fence (`WaitUpTo`). That moves the
CPU cost off the draw thread, but the GPU queue waits instead, so the frame may not get shorter.
Measure, do not assume. **reported** (loadburst): this switch was never tested on the big binds.

**Fix 1E. Bound the vertex and index part (only if the bind is a vertex fetch).** For a vertex or
index V#, the draw's index range bounds the bytes read (`max index + 1` times the stride). That gives
an exact range. For raw or shader-addressed reads this cannot be computed. Only usable after Fix 1B
shows the binds are vertex fetches (**unknown**).

---

## 3. Burst 2: camera turns and newly visible objects

### 3.1 What grows per frame when turning (observed mechanism)

The XPR visibility consumer `BPXPR_ConsumeVisibilityMask` (`0x1a6d20`, 1386 bytes, called from
`0x19af20`) reads the per-set bitset the GPU wrote (max 2,048 bits, 256 bytes per set, see
`docs/re-stutters/STUTTER-ARCHITECTURE.md` 3.1). For each set bit it:

- emits state for the object's material and mesh when they differ from the previous object
  (`0x16b850`, `0x16b890`, `0xf8990`, `0xae150` for user-data pointers);
- writes the 12-V# descriptor pointer and the index V# when they change (`0x86bf50`, `0x86bfb0`,
  `0x86bed0`), and a `DrawIndexIndirect` (wrapper `0x86b3d0`; docs 07 section 4 lists `0x1a6d20` among
  its callers).

So each newly visible object adds one draw and, when its material or mesh differs, a state change.
The visible count drives the draw count directly. **observed.**

When a turn makes an object visible for the first time, the costs in order of likely size are
**inferred** (none measured here):

1. **Its data is a first bind.** Its vertex and index V#s (`0x8be60`, section 4) point into
   regions no bind has touched (Section 2.2). The first bind copies the whole 16 MiB region.
2. **Its textures are first binds.** A new T# goes through `FindImage`, image creation, and the
   upload (`docs/re-backend/08-texture-lifecycle.md`). Textures are up to 16 MiB each (reports,
   `WarmTexture`).
3. **Its material may be new.** The first draw with a new shader or pipeline takes the translation
   and pipeline build path (Section 3.3).

The XPR mask itself is read on the CPU in the same frame (reported: about 440 readbacks per heavy
frame, 78-89% of them already complete, report 01). That round trip is a separate source and is
not part of the turn spike here.

### 3.2 Existing pre-warm and why it does not cover a turn (observed)

- `SOTC_LOAD_WARM` (`gpu/sotc/load_warm.h`) records the 64 KiB pages bound in the first 20 s after an
  event, per area key, and warms them in idle gaps on the next visit. **observed**, window from
  FIRST-FRAME-PEAK phase 1. A turn 30 s into an area, or an object first seen after the window, is
  never recorded, so it is never warmed.
- `SOTC_LOAD_STREAM` (same header) drains the session's bound ranges in idle time, 16 MiB per frame
  cap by default. It covers the second and later visits of a range, not the first.
- `SOTC_AREA_PREWARM=3` (`gpu/sotc/area_prewarm.h`) creates images ahead of their first draw, with
  no data. Uploads still happen on first use.
- Texture warm (`SOTC_LOAD_WARM_TEX`, phase 2): **reported** no gain (peak 125 ms against 121 ms
  without it), probably because the textures were warmed before their data was loaded
  (FIRST-FRAME-PEAK, phase 2). Its suspected cause is not verified.

### 3.3 Pipelines and programs on first use (observed, with corrections)

- `PipelineCache::GetProgram` (`vk_pipeline_cache.cpp`, around line 3151) queues a translation for a new program
  when `async_ok` is set. `async_translate_ok` (around line 1655) excludes tiny draws (at most 6
  indices) and draws without dynamic vertex input.
- Tiny draws are built inline (`SOTC_TINY_ASYNC`, default 0; the rule is written at lines 698-716).
  **Correction:** reports 07 and STUTTER-ARCHITECTURE cite `vk_pipeline_cache.cpp:1084` as the
  synchronous tiny-draw build. Line 1084 is now inside the boot-time wait
  (`SOTC_N5_PSO_WAIT_S`). The inline decision is at lines 698-716 and 2694. Line numbers have
  drifted; use the function names.
- Stale cached permutations: `vk_pipeline_serialization.cpp:481-484` (reported, 419 pipelines
  rebuilt on every boot, report 07). Fixing it removes a boot-time rebuild, not the turn spike.
- A program prefetch at the file read `ResourceFactoryResourceLoader::LoadFileInternal` (`0x14b980`)
  is a design (report reports2/08), not built. It needs the program binaries to be in the file.

### 3.4 Fixes for turns, ranked

**Fix 2A. Record the whole visit, not 20 s, and persist it per area.** Extend the learned set of
`load_warm` from the 20 s window to the whole area visit, and keep it across runs. A revisit then
warms the objects that were visible on the first visit. Cost: a larger saved set and idle copies. The
first visit of a session still spikes, which Fix 1A covers.

**Fix 2B. Region queue at creation (Fix 1A) for data.** Same as Section 2.3. It also covers a turn on
the first visit, because the area load created the regions. Regions created by the area load are
drained during the load.

**Fix 2C. Textures warmed after the data loads.** Warm the T# after `LoadFileInternal` (or at the
completion callback `0x148d14`, see 4.2), not at the area event. That is the point the phase 2
attempt did not reach (FIRST-FRAME-PEAK). **inferred.**

**Fix 2D. Pipelines and programs.** Fix the stale permutation index first (reported, cheap, no hook).
Then translate the base program of each area's shaders when the file is read. This follows
reports2/08. Its size: translations are about 2 ms each in routine area frames, and the outlier
frame had 544 translations (reported, 1,151 ms). The outlier is the real gain, and only if its
translations are base permutations (**unknown**).

**Fix 2E. Turn spike test before tuning.** Run one fixed turn route (same save, same route, as the
verification rule in STUTTER-ARCHITECTURE 4) with `SOTC_BIG_SYNC_LOG`, the existing texture-life
report and the first-use counters. Split each spike into bind bytes, textures and pipelines.
Without this split, the order of 2A-2D is a guess.

---

## 4. Burst 3: area loading (streaming path)

### 4.1 The engine path (observed unless marked)

- Loader thread: `ResourceFactory::ThreadFunc` (`0x1464d0`, 658 bytes). Loads run through
  `ResourceFactoryResourceLoader::LoadFileInternal` (`0x14b980`), which calls two virtuals of the
  loader object for the file read (`vtbl+0x80`, `vtbl+0xa8`). Decoding (Oodle, `0x1aa2a0` to
  `0x89d3b0`) and Fios2 reads are **reported** (report 05), not re-read here.
- Completions: `SyncPointProcessPendingResources` (`0x1480f0`, 1291 bytes). It takes a lock, then walks
  the completion lists (`+0x2a8` / `+0x2b8`) until the sentinel, finalising each entry. No item or
  time count was found in the part read. **observed** (list walk). Callers: `0x1469d0`
  (`WaitForAllCmdsToFinish`) and `0x3c5bf0`.
- Barrier: `WaitForAllCmdsToFinish` (`0x1469d0`) loops while `HasPendingWork` (`0x147f40`) is true.
  **Conflict to resolve:** STUTTER-ARCHITECTURE 3.3 says the `GameLoop` call at `0x25cb12` is after the
  main loop; `reports2/03` lists `0x25cb12` as a direct call from `GameLoop` without placing it outside
  the loop, and reaches the barrier through `sub_3c5a00` (`0x3c5a42`). The callers of `0x1480f0` are
  `0x1469d0` and `0x3c5bf0` (decompile header). **unknown** whether a barrier runs in gameplay;
  STUTTER-ARCHITECTURE step 0 proposes the check.
- Texture mips: `TextureManager` runs once per frame from the render job (`0x117930` to `0x13cae0`
  and `0x117160`). `0x13cae0` calls `0x13ccd0` (queue scan), `0x13d0f0`, `0x13d280`. `0x13ccd0`
  (1016 bytes) walks the queue bitset (`+0x204030`), calls `0x13a6f0` per slot, and calls `0x13a3e0`
  (760 bytes, the per-texture state machine that steps the mip records). **observed.** No per-frame count cap was
  found in these loops (**observed** for `0x13ccd0`, **inferred** for the rest). The cvar
  `numMipRequestsPerFrame` (256) has no reader found (`reports2/03`, **reported**).
- Mip copy: `0x13a3e0` calls `0x112b80` (`TextureStreamingChunk::ProcessTextureData`, 419 bytes),
  which kicks job `0x35480` with body `0x112d30` (18 bytes). The body calls `0x98ae0` (693 bytes) to
  grow the resident level: it commits the range with `0x12950`, copies the mip data with a CPU
  `memcpy` from the loaded buffer, and sets `+0xc0` (docs 08, section 3.3). **observed.**
- Meshes: `sub_1264d0` (5007 bytes, the deserializer, docs 07). Each vertex attribute stream goes
  through `0x8be60` (547 bytes): a fresh allocation (`0x12820`, family 1 or 2), an `import_memcpy` of
  the stream into it, and `sceGnmRegisterResource` for named buffers. **observed.** So a mesh is a copy
  into fresh pool pages, which are first-touch regions for the port (Section 2.2).

### 4.2 What the port does per streamed asset (observed, with reports)

- A new region starts all CPU-dirty and unprotected (Section 2.2). The loader's copies into it do not
  fault, because nothing is protected yet.
- At the first bind the port copies the dirty runs, then protects them. The protect cost is
  11-18 ms in the peak (reported, FIRST-FRAME-PEAK `_lw3`, `_lw4`).
- Afterwards, the loader's and the job's `memcpy` into tracked pages raise write faults. About 47% of
  the 97k write faults in slow frames come from host `memcpy` and `memset` (reported, STUTTER-
  ARCHITECTURE 3.2). These are the port's own faults, not the game's, and they follow the protects.
- File reads are not the signal: 1% of the peak's copied bytes lie in pages a file read wrote
  (reported, FIRST-FRAME-PEAK phase 0). The bytes are written by the game after decoding, so the
  load-time signal is the game's writes, which are not visible while the region is untracked.

### 4.3 Fixes for area loads, ranked

**Fix 3A. Region queue at load time (Fix 1A).** The loader creates regions, so the queue fills during the
load, and the draw thread drains it while the load screen is up. This is the largest single change for
area entry. It replaces the 640-750 MiB first-touch copy of the first frame with the same copy spread over
the load.

**Fix 3B. Bound the completion drain (`0x1480f0`).** Finalise at most N completions, or M ms, per frame
during gameplay. The barrier callers (`0x1469d0`, `0x3c5bf0`) need the full drain at load, so the bound
must be off there. Risk: a resource kept pending longer, and the destructor and `DeleteResource` also
wait on it (`STUTTER-ARCHITECTURE` 3.3). Do not change `HasPendingWork` globally.

**Fix 3C. Cap mip copies per frame (`0x98ae0`).** The hook `MIP_BUDGET_COPY` (`sotc/mip_budget.h`) only
observes today. A cap of N copies per frame would move the rest to later frames. Effect: visible
pop-in, not corruption (the state machine keeps the request). Count the copies per frame first.

**Fix 3D. Pre-stage textures at loader completion (`0x148d14`).** When a texture's file completes, hand
its data to a worker that creates the image and uploads it, paced by the existing 32 MB budget
(STUTTER-ARCHITECTURE step 5). This is the warm that was not built at completion (Fix 2C).

**Fix 3E. Drain delay for unloads.** The port's unmap waits (`drop waits 2859 ms` per 5 s window,
`reports2/03`, **reported**) are a separate burst on area change. `SOTC_DROP_FAST=1` removes most of it
in the dev configuration (STUTTER-ARCHITECTURE table 2). Measure it on an area change before deciding.

---

## 5. Corrections and conflicts found while writing this

| Claim | Where | Correction |
|---|---|---|
| Synchronous tiny-draw build at `vk_pipeline_cache.cpp:1084` | STUTTER-ARCHITECTURE table 2, report 07 | Line 1084 is the boot PSO wait now. The inline rule is at lines 698-716 and 2694. |
| `GameLoop` barrier at `0x25cb12` is a direct call from the loop | reports2/03 | STUTTER-ARCHITECTURE 3.3 says it is after the loop. reports2/03 does not place it either way. Unresolved; see 4.1. |
| The 64 MiB bind is a game V# | user report wording | It is the port's window (Section 2.1). The game V# is larger and unidentified. |
| A bind copies its whole window | implied by the log | It copies the CPU-dirty runs in the window (Section 2.2). The total is in the same log line. |

---

## 6. Unknowns and measurements to run

1. Which engine buffer the 64 MiB bind reads. Log `pgm_hash`, stage, descriptor index and `num_records` at
   the clamp (Fix 1B). This needs a build and a run, which this document does not do.
2. How many bytes of the 64 MiB bind are fresh regions against recycled regions, per bind (Fix 1B).
3. Whether the bind is a vertex fetch or a raw read (decides Fix 1E).
4. Whether any `HasPendingWork` barrier runs in gameplay (STUTTER-ARCHITECTURE step 0, `0x1469d0`).
5. The turn spike split: bind bytes, textures and pipelines per turn on a fixed route (Fix 2E).
6. The release path of recycled manager slots (`free_managers`): how often a region is fresh. Not read.
7. The callers of the frame rings (`0x48440`), listed as unknown in docs 09 section 9. Not affected by
   the fixes above, but they decide whether ring memory is part of the first-touch set.
8. The byte count of the mip copies per frame (Fix 3C), which decides the cap.
9. The GPU-side cost of the arena copy (`CopyBuffer`), which these estimates leave out.
