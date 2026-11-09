# Design: next cuts after the locked-30 plan (8 Nov 2026)

Read-only design research. Nothing was edited, built or run. Evidence is file:line in the tree as of branch
`arch/nonblocking-frame`. Millisecond figures are estimates: the profile (`user/prof/m30/draw2.txt`) gives
percent of samples, not milliseconds. This file converts with 1% of samples = 0.26 ms per frame, taking the
draw thread at about 26 ms per frame (the middle of the 24-27 ms range in the plan). Each estimate has to be
replaced by a measured number before it is judged.

Companion: `docs/re-stutters/LOCKED-30-PLAN-2026-11-08.md`.

## 0. Corrections to the premises

1. **`SOTC_PARALLEL_RECORD` was never measured as a gain.** The "29 Oct: no gain" result in
   `CHANGELOG.md` (around line 300) is for `SOTC_BACK_HEAD_CACHE`, `SOTC_BACK_PF` and `SOTC_REG_SAME`, not for
   the prep threads. `docs/DRAW-THREAD-7C-DESIGN.md:113` says "built, off, never timed". The only later figure
   is in `docs/re-stutters/HANDOFF.md` (1 Nov): "dp_pr (SOTC_PARALLEL_RECORD + SOTC_PR_GEN) still costs ~3 ms".
   The cause of that cost is not analysed in the docs.
2. **`SOTC_PR_RESOLVE` divergence has a recorded root cause.** `docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md:236`
   says it "diverged 12 times because it re-derived mapping checks, not snapshots". The 1 Nov verify run
   (HANDOFF.md, around lines 171-176) reports 0 mismatches for `SOTC_PR_GEN=2` with `SOTC_PARALLEL_RECORD=1`.
   The generation-sum path (`SOTC_PR_GEN`) is therefore the candidate to use, not `SOTC_PR_RESOLVE`.
3. **Memory: arenas and staging do not explain the route growth.** The logs show the arenas reach their
   plateau within about 15 s of session start and the staging pools are fixed by the loading screen (section 4).
   `process private` rises from about 5.5 GB (loading end) to 8.3-8.8 GB (route end). That is roughly 3 GB that the
   logs do not attribute.

## 1. Draw-thread profile (`user/prof/m30/draw2.txt`, inclusive %)

| Item | % of samples | ~ms/frame | Class |
|---|---|---|---|
| `Rasterizer::BindResources` (`vk_rasterizer.cpp:4675`) | 45.7 | 11.9 | mixed (section 2) |
| `BindBuffers` (`:8813`) | 13.5 | 3.5 | mixed |
| `BufferCache::ObtainBuffer` (`buffer_cache.cpp:6228`) | 10.6 | 2.8 | mutating |
| `BindTextures` (`:10099`) / `BindTexturesFull` (`:10290`) | 10.3 / 8.3 | 2.7 / 2.2 | mixed |
| `TryBindCacheFrame` (`:7040`) | 7.8 | 2.0 | mixed (lookup is pure, drop/commit are not) |
| `BindPipelineResources` | 4.4 | 1.1 | draw thread |
| `ExecuteShaderHLE` (`vk_shader_hle.cpp:914`) | 4.8 | 1.2 | CPU HLE fills (lock-30 item B3) |
| `SynchronizeMemory` / `SynchronizeMemoryFull` (`buffer_cache.cpp:9541` / `:9626`) | 4.1 / 3.2 | 1.1 / 0.8 | mutating |
| `ReplaceChangedPages` | 4.2 | 1.1 | mutating (page protection) |
| `PcRingCopy::OnSubmitStart` (`pc_ring_copy.cpp:644`) | 2.5 | 0.65 | submit start (section 3) |
| `BufferCache::NativeDrain` (`buffer_cache.cpp:8895`) | 2.4 | 0.6 | submit start |
| `Rasterizer::CopyGpuResults` (`vk_rasterizer.cpp:11698`) | 2.0 | 0.5 | per draw |
| `Core::MemoryManager::TryWriteBacking` | 2.3 | 0.6 | guest write |
| `BeginRendering` (`:11091`) / `PrepareRenderState` (`:468`) | 3.3 / 2.8 | 0.9 / 0.7 | mixed |
| `UpdateDynamicState` (`:12112`) | 2.6 | 0.7 | values already prepared |
| `FutexSleep` (waiting for CP) | 9.1 | (wait) | not work |
| outside loader: `ntdll.dll` / `ucrtbase.dll` | 17.0 / 10.7 | 4.4 / 2.8 | locks and allocation |

The `ucrtbase.dll` share (10.7%, about 2.8 ms) and `std::vector::__assign_with_size` (2.9%) point to heap
traffic inside the draw path. That is a cheap cut independent of the designs below and should be checked first.

## 2. Design E: prepare thread one op ahead

### 2.1 What exists and why it did not pay

- `Liverpool::PrepLoop` (`liverpool.cpp:2166`) runs `N` GpuPrep threads. Each one copies the register chunks of
  **every** op into its own `prep_regs` (loop at `:2201-2207`) and runs `setup_stages` (`:2209`, a
  `flattened_ud_buf.assign` per stage) for every op, then computes only its share of draws (`seq % prep_workers`).
  The register replay and the stage setup are therefore paid once per worker, not once per op. With
  `SOTC_PARALLEL_RECORD=1` and 2 workers this is duplicated work on the CP side and on the cores the game and
  recorder need. This is the first suspect for the ~3 ms the 1 Nov run measured.
- `Liverpool::TakePrepared` (`:2273`) uses a slot only when `ready_seq == seq`, `valid`, and
  `map_epoch == rasterizer->MapEpoch()` (`:2305`). `Rasterizer::MapMemory` and `UnmapMemory` bump `map_epoch`
  (`vk_rasterizer.cpp:11711`, `:11725`), and every guest map or unmap invalidates all prepared slots. Game code
  maps and unmaps during streaming, so `pr_epoch` misses concentrate exactly in the heavy frames.
- `Rasterizer::PrepareDraw` (`:11696`) computes values that depend only on registers (dynamic values, push data,
  fast hash, cache eligibility, res hash). It does not compute the bind-cache lookup, the resource predicates, or
  any `ObtainBuffer`/`FindImage`.
- `PrResolveBuffer` (`:4428`) and `PrResolveImage` (`:4486`) compute V#/T# values and the mapping verdict. They
  validate the mapping with a generation-free check. That is the divergence source (section 0.2). The `SOTC_PR_GEN`
  variant (per-range generation sums, `draw_prep.h:` header comment, `gen_sum` fields at `:82` and `:92`) is the one
  that verified clean.
- `BindResources` (`:4675`) reads the prepared slot only for the register-only inputs. It still does the predicates
  (`shader_addressed` lambda in `BindBuffers`), `TableMemo`, `TryBindCacheFrame`, and all cache lookups inline.

### 2.2 What a version that moves the expensive parts needs

Work classes (draw-thread time, from section 1):

| Piece | Read or write of shared state | Can a worker do it? | Estimated saving |
|---|---|---|---|
| Resource predicates (`shader_addressed`, special, written) per descriptor | reads pipeline profile only | yes, pure | 0.5-1 ms |
| Bind-cache key (`BindCacheFrameKey`, `:6645`) and table lookup (`BindCacheFindFrame`, `:6875`) | reads `frame_cache` (4096 entries, `vk_rasterizer.h:821`) which the draw thread mutates | only as a *probe*: entry index plus the entry's generation, validated on the draw thread | 0.5-1 ms |
| Bind-cache stamp check (`BindCacheStampsOk`, `:6659`) | reads `RangeStamps` and `image_stamps` (atomics) | can pre-read, but a write after the probe changes the answer, so the draw thread must repeat it (0.8% of samples) | small |
| V#/T# decode and mapping verdict (`PrResolve*`) | reads memory manager under lock | yes with `SOTC_PR_GEN` snapshots | 1-2 ms (inside BindBuffers/BindTextures self time) |
| `ObtainBuffer` / `FindImage` / `MemoLookup` | create entries, LRU touch, uploads | no. Only a read-only probe with a generation (see below) | 0.5-1 ms |
| `SynchronizeMemory*`, `ReplaceChangedPages` | page protection, uploads | no (ordering with GPU reads) | not in E |
| Descriptor write list, barriers, `Transit`, bind-cache commit | draw-thread state | no | not in E |

Estimated total for E: **3-5 ms of the draw thread** with PR_GEN-validated snapshots, up to about 7 ms if the
read-only probes of `ObtainBuffer` and `FindImage` also prove stable. These are estimates; section 2.5 has the gate.

### 2.3 Concurrency rules a version needs

Safe to read concurrently (no lock or already atomic):
- `VideoCore::RangeStamps`, `image_stamps` (`vk_rasterizer.cpp:6659` parameters), `GpuFlagStamps()`, `SotcGen::Sum`.
- `MemoryManager::ClampRangeSize` and `IsValidGpuMapping`. Check that both take the memory lock; the design note
  (`ARCH-PARALLEL-DRAW.md:105`) asks for the same check.
- The pipeline profile and `Shader::Info` (immutable after creation). Prep slots write only `runtime[2+index]`.

Need a snapshot or a probe with validation:
- `frame_cache` (`vk_rasterizer.h`, 4096 `BindCacheEntry`, struct at `:715`): a worker reads `entry.hash` and the
  entry index; the draw thread checks equality, and the validity fields of the entry (`made_tick`, `stamp_tick`,
  `bindless_gen`, and the flush generation and `BindVersion()` noted at `:752-760`) before use. The struct has no
  single generation field, so a probe needs one added (a seqlock or a per-entry sequence number). Entries are dropped by `BindCacheDropFrame`
  (`:6929`) and committed by `CommitBindCacheFrame` (`:7548`). Both bump the generation.
- `TextureCache::mutex` (`ReentrantMutex`, `texture_cache.h:123`, `BatchLock` at `:622`) and
  `samplers_mutex` (`:1242`). A probe holding these blocks the draw thread's own lookups. Prefer a
  seqlock-protected image index.
- `BufferCache::address_space` (`buffer_cache.h:972`) and `BindVersion()` (`:171`): a probe returns the
  version it read; the draw thread commits only when it matches.
- LRU touch (`Common::LeastRecentlyUsedCache::Touch`) and `bind_seen` / `bind_key_seen` (`TryBindCacheFrame`,
  stats only): recorded by the probe, applied at commit. Keep these off in measured runs (`SOTC_FPS` off).

Must stay on the draw thread: `g_bind_diff`, `TableMemo` memo, `bind_cache_kept_write`/`bind_cache_hold`
(set inside `TryBindCacheFrame`), and everything that creates a `BufferId`, `ImageId`, view or sampler handle.

### 2.4 Ordering and invalidation

A prepared value is valid only while all of these are unchanged at bind time (the draw thread checks):
- guest mappings: `map_epoch`, or the per-range `runtime_memory_generation` sums of `SOTC_PR_GEN` (keeps an
  unrelated map from voiding the slot);
- the bind-cache generation of the probed entry and `BindVersion()`;
- write stamps (`RangeStamps::Seq`, `GpuFlagStamps().Seq()`, `ImageChangeSeq()`), which the existing
  `BindCacheStampsOk` already checks on the draw thread;
- the render-target key (`bind_cache_state.targets_key`), which is register-only and can be prepared;
- the pipeline pointer the CP resolved (prep must not read the pipeline cache, `ARCH-PARALLEL-DRAW.md`).

A stale probe must fall back to the inline path, never to a wrong binding. The verify mode (`=2`) compares the
probe result with the inline lookup and counts mismatches. That is the gate.

### 2.5 Step plan (switches off by default)

1. **E0, cheap fix first:** make `PrepLoop` replay registers and `setup_stages` only for the ops its worker
   computes, or copy from the draw-side ring once. Measure `SOTC_PARALLEL_RECORD=1` against off on the route. If it
   is still +3 ms, the prep threads are a loss as built and E1 does not start.
2. **E1, predicates and keys (`SOTC_PR_PRED`):** `PrBuffer.shader_addressed` is already a field (`draw_prep.h:81`).
   Fill it for every descriptor and read it in `BindBuffers`. Verify `=2`: 0 differences.
3. **E2, bind-cache probe (`SOTC_PR_BCPROBE`):** the worker stores the entry index and generation; the draw
   thread checks them and then runs `BindCacheStampsOk` and the commit as today. Verify `=2`: 0 mismatches over
   the route.
4. **E3, PR_GEN for V#/T# (`SOTC_PR_GEN=1`, `=2` verify):** already implemented. Re-measure with E1/E2 on.
5. **E4, read-only `ObtainBuffer`/`FindImage` probes:** only after E1-E3 are measured. This is the largest saving and
   the largest risk. It needs the generation scheme of section 2.4.

Gate for each step: verify `=2` with 0 mismatches over a full route, then one timed round against the base
(one round, no warm-up, per the matrix rule in memory). Keep a step only if the draw thread drops by at least 1 ms.

## 3. Design F: submit-start and draw-side work on another thread

### 3.1 Where it runs and what the draw thread waits for

- `OnSubmitStart` is queued by the CP with `BackOrRun` at `liverpool.cpp:4617` and `:5855`, and runs as
  `VideoCore::PcRingCopy::OnSubmitStart` (`pc_ring_copy.cpp:644`). It calls `EpochEnd`, `NativeDrain` (budgeted
  upload of announced ranges, `buffer_cache.cpp:8895`), the N1 verify pass, and mode-8 `PcHostSubmitStart`. Each
  of these reads guest memory and mutates buffer-cache state. The `SOTC_BACK_WRITE_EXACT` flag makes this work
  not advance `back_write_seq`.
- The ordering proof in `OnSubmitStart` (protect, then copy, `pc_ring_copy.cpp` header) requires every submit's
  protections and uploads to finish before that submit's first bind. A helper that runs the copy for submit N+1
  while the draw thread still runs submit N is only correct if the draw thread waits, before the first bind of
  N+1, on a fence for the copy of N+1. The wait point is the first `ObtainBuffer`/`FindImage` of the submit.

### 3.2 Items

| Item | Where | Share | Can run elsewhere? | What the draw thread must wait for |
|---|---|---|---|---|
| `PcRingCopy::OnSubmitStart` (mode 6/7 copy path) | `pc_ring_copy.cpp:644` | 2.5% (~0.65 ms) | yes, as a helper per submit, if the ordering proof holds | fence on the copy before the submit's first bind |
| `NativeDrain` | `buffer_cache.cpp:8895` | 2.4% (~0.6 ms) | yes: pops the queue and uploads on a helper; marks must reach every bind of the submit, so the marks stay on the draw thread | a per-range "uploaded" sequence checked in `SynchronizeMemory` |
| `CopyGpuResults` | `vk_rasterizer.cpp:11698` (called at `:3973`, `:4088`) | 2.0% | the recording call is cheap; the loop over `bound_buffers` must stay with the draw thread | none (records into the command stream) |
| `TryWriteBacking` (guest write of readbacks) | `buffer_cache.cpp:7164`, `:7210`, `:7274`; `texture_cache.cpp:253` | 2.3% | yes, but the write must be ordered before the game reads it. The CP already waits on labels, so the wait is the label | the EOP label or readback fence |
| `BeginRendering` / `PrepareRenderState` | `vk_rasterizer.cpp:11091`, `:468` | 3.3% / 2.8% | the RT descriptors (format, extent) can be prepared (E domain); `FindImage` for the RT stays with the draw thread | the RT image handle before the render pass starts |
| `UpdateDynamicState` / `ApplyDynamic` | `vk_rasterizer.cpp:12112`, `:12140` | 2.6% | values are already prepared (PrepLoop); the remaining cost is the setter calls. Check whether they are recorder closures | none if closures |
| `SynchronizeDmaBuffers` | `buffer_cache.cpp:7398` | not in top list (inside the sync path) | a sweep, can run on a helper with `SOTC_UNTRACK_AUTO`'s apply list, but it changes page protection, so it stays ordered | the sweep's protection changes before the next bind |

### 3.3 Step plan (switches off by default)

1. **F1 (lowest risk, ~1 ms):** move `NativeDrain` uploads (not marks) to a helper with a per-range
   "uploaded" sequence. `SOTC_NATIVE_DRAIN_ASYNC=1`. Verify: the N1 verify pass and `SOTC_VERIFY=n1` report no stale
   reads. Note: `SOTC_LEAN_SUBMIT` already has a snapshot verify (`take_mode`, `buffer_cache.cpp` near `:8925`).
2. **F2, PC ring copy (`SOTC_PC_RING=6/7`, submit-start helper):** only after F1, and only with a fence on the first
   bind of each submit. The ordering proof must be re-read for the new thread. Verify with mode 7 (expected divergence 0).
3. **F3, RT descriptors (`SOTC_PR_RT`):** prepare format and extent in PrepLoop (already listed as S3 in
   `ARCH-PARALLEL-DRAW.md`). Keep `FindImage` inline.

Expected: F1 about 0.5-1 ms; F1+F2 about 1.3-2 ms; F3 0.3-0.5 ms. Not counted: `TryWriteBacking`, which
needs the label path (the CP already waits there, so the gain depends on what that wait costs).

## 4. Design G: host memory during the route

### 4.1 What the logs show (not a guess)

Source lines: `SOTC VRAM` (`driver.cpp:2201`), `SOTC ALLOC` (`driver.cpp:877-890`, per 5 s window, the counters
are reset each window), `process private` (`vk_pipeline_cache.cpp:8640`, `pipe_manifest.cpp:473` uses
`PrivateUsage`).

| Session time | arena (VRAM) | staging ring (host) | staging large (host) | process private |
|---|---|---|---|---|
| 5 s (loading, `combo.out`) | 1,488 MiB in window (4,420 allocs) | 22 blocks = 352 MiB | 7 = 512 MiB | 5,504 MB at pipeline boot |
| 10-20 s (`combo.out`, `freshcache_warm.out`) | +158 to +170 MiB per 5 s window | 22-28 blocks | 7-8 | 8,268-8,640 MB |
| 25-30 s | +0.4 MiB (plateau) | 448 MiB | 544 MiB | 8,268 MB |
| end, `base.out` | 1,664 MiB buffer arenas (VRAM budget line) | | | 7,875 to 8,812 MB |

Findings:
- **The arenas are VRAM, not process private.** `buffer_cache.cpp:7714` creates them with `MemoryType::Sparse`
  (device-local). They sit in the `VMA blocks` line of `SOTC VRAM`, which is not `PrivateUsage`.
  Their plateau (1.5-1.66 GB) is reached by 25 s; they do not grow over the route.
- **Staging is about 0.9-1.0 GB of host memory and is flat after loading.** `RING_BLOCK_SIZE` is 16 MB
  (`vk_staging_buffer_pool.cpp`, top of file); the ring grows by `blocks.emplace_back` (line 166) with no
  cap, and `TrimRing` releases a block only after `RING_IDLE_FRAMES = 300` idle frames (line 25). The large pool
  (`RequestLarge`, about line 196) keeps idle buffers up to `SOTC_STAGING_LARGE_KEEP_MB` = 256 MB (line 45-50),
  and `held` deferred readbacks are never trimmed while held.
- **The route growth in `process private` (about 3 GB) is not explained by the logs.** The candidates, none
  measured, are: the Vulkan driver's host memory as pipelines are compiled on the route (the driver cache of
  `sotc_vk_pipelines.bin` costs its own size in RAM, measured in the memory notes: 2.3 GB of RAM for a 2 GB file),
  the GPL libraries (about 100 MB), the host import of guest runs (`SotcGuestMemory::ImportGuestRange`, ring mode 8),
  and the bind and pipeline caches. The RAM report (`SOTC_RAM_REPORT=1`, `gpu/sotc/ram_report.cpp`) already
  splits `private` into "remainder" (driver), heaps, stacks and mimalloc. It has not been run on a route in the logs.

### 4.2 Proposed caps (each behind a switch, after the RAM report has been read)

1. **Measure first:** one route with `SOTC_RAM_REPORT=1` at the loading end and at the route end. The "remainder"
   line decides which of the candidates matters. No cap is worth shipping until that line is known.
2. **Driver cache size (plan item D1):** do not load a `sotc_vk_pipelines.bin` above a size cap
   (`SOTC_PIPE_CACHE_MAX_MB`, suggested 256); start fresh above it. Expected: the cache file is 25 MB when fresh and
   grows with each salted run (memory note). This is the only candidate that could plausibly account for GB.
3. **Staging ring cap:** `SOTC_STAGING_RING_MAX_MB` (suggested 256). Past the cap, `RequestFromRing` waits on the
   oldest block's tick instead of allocating. Saves up to about 100-200 MB host memory. Risk: a stall inside
   a streaming frame. Gate: the frame p99 on an area entry must not rise.
4. **Large staging cap:** `SOTC_STAGING_LARGE_KEEP_MB` from 256 to 128 (one switch, no code). Saves up to 128 MB.
   Cost: the `pow2` reuse may miss in one burst (the note in `vk_staging_buffer_pool.cpp:30-35` records 240 MiB of
   allocations per 5 s when it was off).
5. **VRAM budget, not RAM:** `SOTC_ARENA_EVICT_MB` default 1536 (`buffer_cache.cpp:10748`) is already passed by
   the plateau (1,664 MB), so eviction runs at 64 MB per 2 s and cannot keep up during streaming. Lower it only
   if the VRAM budget line shows pressure on 6 GB cards. This does not change RAM.

Expected: caps 2-4 together about 0.3-0.5 GB of private memory. They do not reach the under-6 GB goal on their own.
Reaching it depends on the RAM report. If the driver remainder dominates, the goal needs a driver-side change
(fewer or smaller pipelines, or lazy creation), not a cap in this tree.

## 5. Gates and order of work

1. Measure before any change (no game run is needed for the RAM report on an existing route): `SOTC_RAM_REPORT=1`
   at loading end and at route end, one round.
2. Draw-thread heap cut (`ucrtbase` 10.7%, `vector::assign` 2.9%) and the existing items of the locked-30 plan B.
3. E0 then E1-E3 with verify `=2`.
4. F1, then F2 with mode 7 verify.
5. G caps 2-4 after the RAM report.

Shared constraints for all three: every switch off by default; verify modes report 0 mismatches over a full route
before a timed round; one timed round per configuration (memory rule "matrix one round"). Estimates above are not
gates.

## 6. What was not checked

- No timed run exists for any of E, F or G. The percentages come from one profile (`draw2.txt`, 14,374 samples).
- `ApplyDynamic`'s setters were not checked for recorder closures (section 3.2, last row).
- The 1 Nov "+3 ms" for PARALLEL_RECORD is taken from the HANDOFF note; its run file was not found in the tree.
- `SOTC_RAM_REPORT=1` output for a route is not in the logs; section 4 is inferred from the VRAM, ALLOC and
  `process private` lines only.
