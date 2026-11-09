# Parallel recording for 60 and 80 FPS in PS4 Pro mode (9 Nov 2026)

Read-only design. Nothing was edited, built or run. Line numbers are for branch `sync/upstream-0.72.2` as of
9 Nov. Millisecond figures marked "est." are estimates from profile shares; only the "measured" rows come from
logs.

**Target (changed during this pass):** first a locked 60 FPS (16.7 ms per thread), then the user's new goal of
**at least 80 FPS uncapped in PS4 Pro mode: 12.5 ms per frame for every thread (draw, recorder, CP), the GPU, and
the game's own threads.** This document covers both. Section 6 says plainly what 80+ needs.

## 0. Summary

- **The draw thread does not record Vulkan commands.** It only copies closures into the `CommandRecorder` stream.
  The recorder thread replays them into real command buffers (`vk_recorder.h:66-80`; the replay loop is
  `vk_recorder.cpp:2560-2611`). So "N workers that record Vulkan commands" (candidate a) and the pass secondaries
  (candidate b) both speed up the **recorder**, not the draw thread.
- **About 80% of the draw thread's 25 ms is serial cache and tracking work** (resolve), and about 20% is pure work
  (hashes, keys, predicates, push and dynamic values, descriptor writes, closure emission). Candidate (a) as stated
  leaves about 20 ms on the serial resolve stage. **(a) on its own is a no-go for 60 and for 80.**
- **The recorder is not at its limit.** Its 16 ms of CPU is mostly the idle spin: the blocks it actually runs take
  **5.3 ms per frame** (`SOTC recorder/frame: busy 5.27`, `t_pro60.out:835`). It has about 7 ms of headroom at
  80 FPS.
- **Chosen design (d), in this order:**
  1. Prerequisite cuts that shrink the serial work itself.
  2. The existing prepare thread (`PrepLoop`), finished, for the pure part.
  3. Descriptor writes moved into recorder closures, which uses the recorder's headroom (candidate c).
  4. Last, a **2-stage split of the serial resolve by cache ownership**: a buffer-side stage, and a texture /
     render-target / bind-commit stage. They form a pipeline in stream order, with per-op command fragments
     spliced into the recorder stream in op order.
- **Expected draw thread:** 25 ms, then about 13-18 ms after the prerequisites (est.), then about 9-12 ms after the
  split (est.). 60 FPS is realistic. 80 FPS is reachable only on paper, at the high-risk end of the plan.
- **80+ real game frames also need a game-side change.** With `fps60game` the simulation takes a fixed step of
  1/59.94 s per frame in some systems. Uncapped at 80 FPS those systems run 1.33x too fast: the same defect class
  as #100/#120/#49. The launcher's current "Unlimited" mode already has this defect above 60.
- **The realistic route to 80+ on screen** is a locked 60 plus the existing frame generation (`SOTC_FRAMEGEN`).

## 1. Measured baseline (Pro 60, warm test route, `user/prof/m0/t_pro60.out`)

| Item | Value | Line |
|---|---|---|
| FPS in play | 33.1-35.0 | `:839`, 20 s window |
| `cpu/frame` draw / rec / CP | 25.00 / 16.02 / 13.55 ms | `:1006` |
| draw thread cores | 0.88 (no waits: gpu, recorder, upload, tc lock all 0) | `:1007-1008` |
| recorder busy (blocks run) | 5.27 ms/frame, the rest is spin | `:835` |
| recorder idle waits | 128,835 per 5 s (~780/frame), 78,138 ended in the spin | `SOTC FUTEX rec` |
| CP spin / sleep | spin 0, label sleep 400 ms/s asleep (sleep is not CPU) | `SOTC CP label sleep` |
| ops per frame | ~3,700 (610,540 per 5 s, 165 frames; `BIND ELIGIBLE`) | |
| bind cache | eligible 57% of ops; refused: compute 173,148, storage/fmask 90,704; fast-bind hit 33% of eligible; frame cache hits 85.7% of consulted | `BIND ELIGIBLE`, `BIND CACHE` |
| descriptor sets | reused 207,086 per 5 s (PLAN-B reuse on) | `PLAN-B` |
| NativeDrain on the draw thread | 0.92 ms/frame, 18.9 drains/frame, 2.0 M texture invalidations per 5 s | `NATIVE TIMING` |
| HLE fills | 42,757 replicate dispatches per 5 s, 14,678 refused (run on the GPU) | `NATIVE HLE fill` |
| CPU of the process | 5.98 of 16 hardware threads | `:1007` |
| GPU | RTX 3060 Ti, driver 616.56 | header |

At 33 FPS: 25 ms / 3,700 ops = **6.8 us per op** on the draw thread.

**GPU busy.** No 5 s line reports it. The only GPU timing is the trace layer (`SOTC_TRACE=2 SOTC_TRACE_GPU=1`,
`gpu/shim/sotc_gpu_trace.cpp`, read by the trace tool into `user/prof/m0/gputrace/hitch.md`). That run
(`m_gputrace.out`) was **PS4 mode, not Pro** (`sceKernelIsNeoMode -> 0`). Its typical frames show:
- GPU work of **8.4-14.2 ms per frame** ("typical ms" of the `GPU: draw scheduler queue` row);
- busy 22-41% of the frame, verdict CPU-bound at 30 FPS.

Pro mode renders more pixels. Its GPU time is **unmeasured** and likely at or above the 12.5 ms budget on this
card. Step S0 measures it.

## 2. Where the draw thread's 25 ms goes

### 2.1 Profile shares

Source: the newest draw-thread profile, `user/prof/m30/draw2.txt` (14,374 samples, 8 Nov, 30 FPS cap, same op
path). FutexSleep was 9.1% there and is 0 in Pro 60. Shares below are inclusive and overlap. ms = share of the
non-wait samples (90.9%) scaled to the 25 ms of `t_pro60`, so 1% ≈ 0.275 ms (est.). `draw_warm.txt` (gcn) agrees
within about 2 points per row.

| Item | % | ~ms | Class |
|---|---|---|---|
| `Rasterizer::BindResources` (`vk_rasterizer.cpp:4667`) | 45.7 | 12.6 | mixed (rows below) |
| `BindBuffers` (`:8949`) | 13.5 | 3.7 | serial (contains `ObtainBuffer`) |
| `BufferCache::ObtainBuffer` (`buffer_cache.cpp:6096`) | 10.6 | 2.9 | serial: creates entries, LRU, uploads |
| `SynchronizeMemory` / `SynchronizeMemoryFull` (`:9017` / `:9102`) | 4.1 / 3.2 | 1.1 / 0.9 | serial: page tracking, uploads |
| `ReplaceChangedPages`, `UpdatePageWatchersForRegion`, `Protect` | 4.2, 3.1, 1.5 | 1.2 | serial: page protection |
| `BindTextures` / `BindTexturesFull` (`:10481` / `:10672`) | 10.3 / 8.3 | 2.8 | serial (contains `FindImage`) |
| `TextureCache::FindImage` (`texture_cache.cpp:1509`), `FindTexture` (`:2299`), `UpdateImage` | 3.9, 1.8, 1.5 | 1.1 + | serial: creates images and views, uploads |
| `TryBindCacheFrame` (`vk_rasterizer.cpp:7095`) | 7.8 | 2.1 | lookup pure, drop and commit serial |
| `BindPipelineResources` (`:6883`): `Pipeline::BindResources` / `BindStageSets` (`vk_pipeline_common.cpp:567` / `:719`) | 4.4 | 1.2 | parallel-safe once handles are known (descriptor write `:765`, push, bind) |
| `PrepareRenderState` (`:453`) / `BeginRendering` (`:11394`) | 2.8 / 3.3 | 1.7 | serial: RT `FindImage`, layouts, pass begin/end |
| `UpdateDynamicState` (`:12400`) | 2.6 | 0.7 | parallel-safe (values prepared, setters are closures) |
| `ExecuteShaderHLE` (`vk_shader_hle.cpp`) | 4.8 | 1.3 | serial: CPU fill writes guest memory |
| `PcRingCopy::OnSubmitStart` + `NativeDrain` (`buffer_cache.cpp:8405`) | 2.5 (incl.) | 0.7-0.9 | serial, submit boundary |
| `TryWriteBacking` (guest writes of results) | 2.3 | 0.6 | serial |
| `SynchronizeDmaBuffers` (`:7272`) | 1.8 | 0.5 | serial: protection sweep |
| `CopyGpuResults` (`vk_rasterizer.cpp:11910`) | 2.0 | 0.55 | serial (bound-buffer walk) |
| written-range bookkeeping (`RecordCurrentWrite`, `AddGpuModified`, `NoteWrittenBind`) | 1.7, 1.5, 1.9 | ~1.0 | serial |
| `FastBindHash`, `ResetBindings`, `ImageDesc`/`ImageViewInfo` ctors, `TexBlockKey` | 1.9, 2.1, 2.2, ~1 | ~2.0 | parallel-safe (pure) |
| closure emission (`CommandRecorder::Allocate`, `Publish`, `GenericThunk::Call`, `RunState`) | 0.5, 1.1, 1.4, 1.3 | 1.2 | parallel-safe per op, but publication is in stream order |
| register replay and `flattened_ud_buf.assign` (`liverpool.cpp:1824`, `:1836`) | (in `vector::__assign_with_size` 2.9) | 0.8 | serial input (cheap to remove) |
| `ntdll.dll` (locks: `unique_lock` 8.3%, `mutex::lock/unlock` 1.4+1.3) | 17.0 | ~4.7 | overhead of the serial parts |
| `ucrtbase.dll` (heap, memcpy; `std::function` ctor 2.4%) | 10.7 | ~2.9 | overhead, mostly avoidable |
| diagnostics (`SotcOpPhases`, `ScopeMin`, `SplitTimer`) | 4.5 each | 0-1.2 | should be 0 under LEAN; inclusive credit from inlining (DESIGN-NEXT-CUTS) |

### 2.2 Parallel-safe work: about 20% (~5 ms)

These rows read only immutable or op-local data, or produce values that are applied in stream order:
- pipeline-key and fast-bind hashes;
- resource predicates (`shader_addressed`, special, written; see the `BindBuffers` lambdas);
- bind-cache key and probe;
- `ImageDesc`, `TexBlockKey` and view-info construction;
- dynamic-state values and push constants;
- descriptor set writes once every handle is resolved (`updateDescriptorSets` at `vk_pipeline_common.cpp:765`);
- closure argument copies.

The prepare thread (`PrepLoop`, `liverpool.cpp:1904`) already computes part of this (`PrepareDraw`,
`vk_rasterizer.cpp:11985`). `DESIGN-NEXT-CUTS-2026-11-08.md` section 2 has the table and the stamp-validation rules.

### 2.3 Serial work: about 80% (~20 ms), with the reason for each

| Must stay in stream order | Why |
|---|---|
| `ObtainBuffer` / `SynchronizeMemory*` / `ReplaceChangedPages` / `SynchronizeDmaBuffers` | create buffer-cache entries, upload dirty pages, change page protection; the order of protect and copy is the coherence proof (`pc_ring_copy.cpp` header) |
| `NativeDrain` / `OnSubmitStart` (queued by the CP at `liverpool.cpp:4498`, `:5753`) | uploads announced CPU writes and invalidates textures (`TextureCache::InvalidateMemory`) before the submit's first bind |
| `FindImage` / `FindTexture` / `RefreshImage` (`texture_cache.cpp:2501`) / `GetSampler` (`:3067`) | create images, views and samplers; upload; LRU; `TextureCache::mutex` (`ReentrantMutex`) |
| `Runtime::Transit` (`vk_runtime.cpp:199`), `FlushBarriers` (`:926`), `RefreshImageLayouts`, `DecideOpBarrier` (`vk_rasterizer.cpp:5838`) | the layout and barrier state is a running state of the stream |
| `Scheduler::BeginRendering` / `EndRendering` (`vk_scheduler.cpp:190` / `:276`) | the pass is open or closed depending on the previous op (`render_state == new_state`) |
| bind-cache drop and commit (`BindCacheDropFrame`, `CommitBindCacheFrame` `:7619`), written-range marks, `CopyGpuResults` | the next op's hit or miss depends on them |
| HLE fills, `TryWriteBacking` | write guest memory the game or the next op reads |
| submits (`Flush`, arena allocation, readbacks inside binds) | change `CurrentTick` and the command buffer every later op records into |

## 3. Candidates compared

| | What it parallelizes | Effect on the draw thread | Effect on the recorder | Risk | Verdict |
|---|---|---|---|---|---|
| (a) serial resolve + N recording workers into secondaries | Vulkan command recording | ~-1 to -2 ms (est.: closure emission only; the resolve keeps ~20 ms) | splits 5.3 ms of real work that has headroom | high (secondaries, inheritance, barriers) | **no-go**: the bound is the resolve, which (a) keeps serial |
| (b) `SOTC_PASS_SEC` extended (`PassSecondary`, `vk_recorder.cpp:1092`; pool and look-ahead with `SOTC_PASS_WORKERS`, verify `SOTC_PASS_VERIFY`) | the recorder's replay of large passes | none | parallel replay of passes | medium | **not needed**: the recorder is 5.3 ms busy. Keep it as a reserve if C1 pushes recorder busy above ~10 ms |
| (c) move draw-thread work to the recorder | anything that needs only resolved handles: descriptor writes, push dedupe | ~-1 to -1.5 ms (est.) | +1 to +1.5 ms busy | low | **yes**, as step C1 |
| (d) prerequisite cuts, prepare thread, then a 2-stage split of the resolve by cache ownership | the serial resolve itself | -13 to -16 ms in total (est.) | small | split: high | **chosen**, staged |

Why (a) does not help, from the code: `Rasterizer::Draw` (`vk_rasterizer.cpp:3079-3283`) calls
`cmdbuf.bindPipeline`, `drawIndexed` and so on on a fake handle. Each call is a thunk that copies its arguments
into the stream (`CommandRecorder::Run`, `vk_recorder.h:130`). The driver call happens on the recorder
(`header->invoke`, `vk_recorder.cpp:2597`). In the profile, `nvoglv64.dll` is 2.0% of the draw thread: that is
the descriptor updates, not recording.

## 4. The chosen design

### 4.1 Steps that shrink the serial work (prerequisites)

The parallel part only pays once the per-op serial work is smaller; an even split of 25 ms gives 12.5 ms plus
overhead, which misses the budget. These come first, each behind its own switch.

- **P1, partial bind-cache hits for compute and storage ops (`SOTC_BIND_OFFSET_HIT`).**
  - 43% of ops are refused by the cache gate (compute 173k, storage/fmask 91k per 5 s).
  - `BIND-PATH-DESIGN.md` section 2 measured a dispatch at about 5.8 us when its resources change and about
    1.4 us when only offsets change.
  - The step is an entry keyed by the resource identities: a hit replays the buffer and image handles, rewrites
    the offsets, and still applies the written-range marks.
  - Est. -2 to -3 ms.
- **P2, one texture-cache lock per op (`SOTC_TC_OP_LOCK`).**
  - `ntdll` is 17% of the draw thread and `unique_lock` 8.3%.
  - Take `TextureCache::mutex` once in `ExecuteBackOp` (an RAII scope in `texture_cache.h`, like the existing
    `BatchLock`) instead of per lookup (`LockCounted`).
  - Est. -1 to -2 ms.
- **P3, per-op heap traffic (`SOTC_OP_NOALLOC`).**
  - `rt.flattened_ud_buf.assign` (`liverpool.cpp:1836`) becomes a span over the op's own storage.
  - `std::function` closures in the draw path become `InlineFunction`.
  - Est. -1 to -1.5 ms.
- **P4, more fills on the GPU.**
  - 28k of 43k replicate fills per 5 s still run on the CPU.
  - Widen the GPU path. HLE must stay on: `SOTC_FILL_HLE=0` gives a black scene.
  - Est. -0.5 to -1 ms.
- **P5, LEAN check.** One profile with the diagnostics compiled out. If `SotcOpPhases`/`ScopeMin` really cost
  something in the play build, remove them from the per-op path.
  - Est. 0 to -1 ms.

**Not a prerequisite:** the S2 buffer table. Measured on the route (GCN-NATIVE-PLAN):
- `g_buf1_skip`: draw 23.8 ms;
- descriptor mode base: 22.3 ms;
- the heap/texture-table modes: 26.5-30.5 ms.

The per-op serial work it was meant to make skippable was not skipped: no code path skips `BindBuffers` (S6 is
eligibility only). It stays off here.

### 4.2 Prepare thread for the pure part (E0-E2, from DESIGN-NEXT-CUTS section 2.5)

- **E0.** `PrepLoop` replays registers and `setup_stages` only for the ops its worker computes. Today every worker
  replays every op (`liverpool.cpp:1984`). This is the likely cause of the +3 ms that `SOTC_PARALLEL_RECORD`
  measured on 1 Nov.
- **E1.** Predicates per descriptor (`SOTC_PR_PRED`).
- **E2.** Bind-cache probe (`SOTC_PR_BCPROBE`): the entry index plus a per-entry sequence number, validated on the
  draw thread.
- V#/T# decoding stays under `SOTC_PR_GEN`, which verified clean (0 mismatches), not `SOTC_PR_RESOLVE`, which
  diverged.
- `TakePrepared` (`liverpool.cpp:2047`) invalidates every slot on any `map_epoch` bump. Use the per-range
  generation sums instead, so streaming frames keep their prepared slots.
- Est. -2 to -3 ms on the draw thread. Each prep thread costs about 4-6 ms of CPU; there are about 10 idle
  hardware threads.

### 4.3 C1: descriptor writes on the recorder (`SOTC_DESC_ON_REC`)

- The draw thread still allocates the set (`DescriptorHeap::Commit`, `vk_resource_pool.cpp:163`, or
  `AcquirePersistent`), because the bind cache stores the handle.
- The `updateDescriptorSets` call (`vk_pipeline_common.cpp:706`, `:765`) becomes a stream closure that runs
  before the `bindDescriptorSets` closure. The write array and its buffer/image infos are copied into the stream
  with `CommandRecorder::Copy`.
- **Ordering.**
  - A persistent set that a later bind-cache hit rebinds is written by a closure earlier in the same stream, so
    the replay order guarantees write-before-bind.
  - Pool recycling is already by fence tick, which is after the recorder ran.
- **Precondition.** No set may be written twice while a submitted command buffer still uses it. That is already
  the rule today, since the reuse path never rewrites.
- Est. draw -1 to -1.5 ms; recorder busy +1 to +1.5 ms.

### 4.4 D: two-stage split of the serial resolve (`SOTC_RESOLVE_SPLIT`, last)

**Stages and ownership.**
- **Stage B (new thread `GpuResolveB`)** owns the buffer cache and the page tracking: `ObtainBuffer`,
  `SynchronizeMemory*`, `ReplaceChangedPages`, `SynchronizeDmaBuffers`, HLE fills, vertex and index buffer
  resolution, the written-range marks of the op's buffers, and `TryWriteBacking`.
- **Stage T (the existing `GpuDraw` thread)** owns:
  - the texture cache (`FindImage`, `FindTexture`, samplers, RT lookups in `PrepareRenderState`);
  - layouts and barriers (`Transit`, `FlushBarriers`);
  - pass begin and end;
  - the bind cache (try, drop, commit);
  - descriptor set allocation, push and dynamic state;
  - closure emission;
  - submits.
- They form a pipeline: B runs op N+1 while T runs op N. Each stage handles ops strictly in stream order.

**Per-op record (B to T).** A fixed-size slot in a ring, in the style of `PreparedDraw` (`draw_prep.h`):
- per buffer binding: `VkBuffer`, offset, range, written flag, the buffer-cache generation B read;
- vertex and index binds;
- `bound_buffers` and the fixed-function reads that `DecideOpBarrier` needs;
- a pointer to B's **command fragment**.

**Command fragments.**
- B's uploads record copies and buffer barriers. B must not write to the shared stream while T is still emitting
  op N.
- So B records op N+1's commands into a fragment: closures in a chunk list that B owns.
- T splices it with one new block kind, `BlockKind::Fragment`, in `vk_recorder.{h,cpp}`, at the start of op N+1.
  The consumer runs the fragment's blocks in place.
- Order in the stream: T's commands for op N, then B's fragment for op N+1, then T's commands for op N+1. That is
  the serial order.
- Chunk lifetime uses the existing pin rules (`Chunk` pins).

**Drain points.** Both stages meet; B does not run ahead. They are also the fallback for anything not
proven safe:
- **Submit start** (`OnSubmitStart` / `NativeDrain`, ~19 per frame): it invalidates textures, which T owns. It
  runs on T with B idle.
- **Any op where B would submit** (staging full, arena allocation submit, readback wait): B stops at the op and T
  runs B's part inline.
- **Buffer/image aliasing.**
  - Case 1: a buffer range of op N+1 overlaps an image registered in the texture cache (texel buffers, formatted
    buffers that alias render targets; `buffer_cache.cpp:6452` texel image sync).
  - Case 2: op N writes an image whose memory a buffer of op N+1 reads (GPU-modified state).
  - B checks a lock-free "image pages" bitmap (the texture cache's fast empty-page check that `NativeDrain` already
    uses, `buffer_cache.cpp:8423`) and drains on a hit.
- **`Func` ops** (write data, EOP, DMA): drain, as today.
- **Map/unmap** (`map_epoch`): drain.

**Synchronization model.**
- **Barriers.** Buffer upload barriers are in B's fragment, before the op. Image layout transitions stay on T.
  None goes inside a render pass: B's fragment comes before T's `BeginRendering` decision for op N+1. If
  op N+1 needs uploads while T has a pass open, T closes the pass before splicing (`EndRendering`), exactly as a
  serial upload does today, where the copy ends the pass.
- **Descriptors.** T alone allocates and writes, or hands the write to the recorder (C1). B never touches
  descriptor pools.
- **Queries and timestamps.** `SOTC_TRACE_GPU` timestamps and `GpuCheckpoint` stay on T. B's fragments carry no
  queries.
- **Dynamic rendering.** Unchanged: secondaries are not used by D. If `SOTC_PASS_SEC` is also on, a fragment
  holds Copy/Barrier blocks, so the pass scan already refuses a pass that contains one (`ScanPass`).

**Est. after P1-P5, E and C1:** draw ~13-18 ms. Of that, B ~6-8 ms and T ~7-9 ms, plus handoff of
~0.2-0.3 us per op per stage (~1-2 ms in total). The critical stage would be ~9-12 ms. A third stage (splitting T
into texture/RT/layout and bind/descriptor/emit) is possible later. It has the same shape and the same verify.

### 4.5 How the recorder gets under budget

- **Today:** 16 ms CPU, 5.3 ms busy. Each idle wait spins for up to 20 us before sleeping (`vk_recorder.cpp:2493-2505`,
  `SOTC_FUTEX_WAIT`); about 780 waits per frame fill the rest.
- **R1, switch only.** `SOTC_REC_SPIN_US=0` (exists, `:2466`) removes the spin. Expected recorder CPU ~6-7 ms.
  - The cost is more producer wakes on the draw thread, about 700 per frame. Each `WakeByAddress` costs about
    1-2 us, which can add up to ~1 ms on the draw thread.
  - Measure both threads. If the draw thread loses more than it gains, the follow-up is to wake only at op end,
    not per block.
- **Budget after C1:** busy ~6.5-7 ms. That is enough for 80 FPS (12.5 ms). If a later step pushes busy above
  ~10 ms, `SOTC_PASS_SEC=1` with `SOTC_PASS_WORKERS=2` and `SOTC_PASS_VERIFY` is the existing lever.

### 4.6 CP: 13.5 ms, needs about 2-3 ms for 80 FPS

The CP profile on record (`user/prof/gcn/cp_warm.txt`, 8 Nov) predates the manifest-hold fixes. Its play-time
leads are still valid:
- `GetGraphicsPipelineLookupSlow` (~29% inclusive) and `RefreshGraphicsStages` (~15%);
- `RefreshFlatBuf` (6.7%): `pipe dirty` shows 7,900 flat-buffer refreshes per frame of 70 words each;
- `WaitBackWritesSlow`.

| Step | What | Est. |
|---|---|---|
| CP0 | `SOTC_PROFILE=GpuCommandProcessor` on the Pro 60 route | measurement |
| CP1 | flat buffer: copy only on change (the 41% unchanged are already detected; skip the copy and the hash) | -0.5 to -1 ms |
| CP2 | pipeline lookup: a per-pass memo of the last key; the register part is rebuilt 26% of the time | -1 to -2 ms |
| CP3 | wake the draw thread only when it sleeps (`FUTEX cp` 22k waits per 5 s) | -0.3 to -0.5 ms |

**Constraint:** `vk_pipeline_cache.cpp` is being edited by another agent now. CP2 waits until that work lands.

## 5. Expected ms per thread and the step order

One timed round per configuration on the test route (`user/routes/tests.json`), Pro 60 mode
(`SOTC_NEO=1 SOTC_MODES=af16,fps60game,grassbuf SOTC_FPS_CAP=off SOTC_FPS_LIMIT=60`), no warm-up, no repeats.
Read `SOTC cpu/frame`, `SOTC recorder/frame: busy`, and the `DISPLAY` vblank histogram. For the 80 FPS
measurements use `SOTC_FPS_LIMIT=0`. Every switch is off by default. A step stays only if its thread drops by
at least 1 ms and its verify reports 0 mismatches over the route.

| # | Step | Switch | Draw | Rec CPU (busy) | CP | Files (owner) |
|---|---|---|---|---|---|---|
| 0 | base, measured | - | 25.0 | 16.0 (5.3) | 13.5 | - |
| S0 | measurements: draw and CP profiles, `SOTC_OP_PHASES=1`, GPU busy (`SOTC_TRACE=2 SOTC_TRACE_GPU=1`), uncapped round | none | = | = | = | none. Optional 5 s GPU busy line: `gpu/shim/sotc_gpu_trace.{h,cpp}`, `core/libraries/videoout/driver.cpp` |
| R1 | no recorder spin | `SOTC_REC_SPIN_US=0` (exists) | 25-26 | 6-7 (5.3) | = | none (if wake batching is needed: `vk_recorder.{h,cpp}`) |
| P1 | partial bind-cache hits for compute/storage | `SOTC_BIND_OFFSET_HIT` (`=2` verify against the full bind) | 22-23 | = | = | `vk_rasterizer.cpp` (bind-cache functions), `vk_rasterizer.h` |
| P2 | one texture-cache lock per op | `SOTC_TC_OP_LOCK` | 20.5-22 | = | = | `texture_cache.{h,cpp}`; the one-line scope in `ExecuteBackOp` is given to P3's owner |
| P3 | no per-op heap traffic | `SOTC_OP_NOALLOC` | 19.5-21 | = | = | `amdgpu/liverpool.cpp` (`ExecuteBackOp`), `shader_recompiler/info.h` |
| P4 | more fills on the GPU | `SOTC_HLE_GPU_MORE` | 19-20.5 | = | = | `renderer_vulkan/vk_shader_hle.cpp` |
| P5 | LEAN diagnostics check | build flag | 18-20.5 | = | = | `sotc_op_phases.h`, `sotc_trace.h` |
| C1 | descriptor writes on the recorder | `SOTC_DESC_ON_REC` (`=2`: write on both and compare set contents) | 17-19.5 | 7-8.5 (6.5-7) | = | `vk_pipeline_common.cpp`, `vk_resource_pool.{h,cpp}` |
| E | prepare thread (E0, E1, E2) | `SOTC_PARALLEL_RECORD=1` + `SOTC_PR_PRED`, `SOTC_PR_BCPROBE`, `SOTC_PR_GEN=1` (`=2` verify) | 14-17 | = | 13.5 (+ prep threads 2x4-6 ms on other cores) | `amdgpu/liverpool.cpp` (`PrepLoop`, `TakePrepared`), `amdgpu/draw_prep.h`, `vk_rasterizer.cpp` (`Prepare*`, `CheckPrepared`), after P1 and P3 |
| CP1-3 | CP cuts | `SOTC_CP_FLAT_SKIP`, `SOTC_CP_KEY_MEMO`, `SOTC_CP_WAKE_SLEEPING` | = | = | 10.5-12 | `amdgpu/liverpool.cpp` (`ProcessGraphics`), `shader_recompiler/info.h`, `vk_pipeline_cache.cpp` (after the other agent), after E |
| D | 2-stage split | `SOTC_RESOLVE_SPLIT=1` (`=2` lockstep, `=3` audit) | 9-12 (critical stage) | +0.3 | = | new `renderer_vulkan/vk_resolve_split.{h,cpp}`, `buffer_cache/buffer_cache.{h,cpp}`, `vk_recorder.{h,cpp}` (fragment block), `vk_rasterizer.cpp` (read the B record), `amdgpu/liverpool.cpp` (`BackLoop` handoff); one agent, after everything above |

**Parallel agent grouping (disjoint files):**
- Wave 1: P1 (`vk_rasterizer.*`), P2 (`texture_cache.*`), P3 (`liverpool.cpp`, `info.h`), P4
  (`vk_shader_hle.cpp`), C1 (`vk_pipeline_common.cpp`, `vk_resource_pool.*`), S0's GPU line
  (`sotc_gpu_trace.*`, `driver.cpp`).
- Wave 2: E (`liverpool.cpp`, `draw_prep.h`, `vk_rasterizer.cpp`).
- Wave 3: CP1-3.
- Wave 4: D.

Estimates are not gates. If the draw thread after wave 2 is above 17 ms, D cannot reach 12.5 ms (two stages of
an even split plus 1-2 ms of handoff), and only 60 FPS stays a target.

### 5.1 Verify, including "a frame serially vs in parallel"

- **P1, E, C1** use the existing pattern: `=2` runs the new path and the inline path, compares, counts mismatches,
  and uses the inline result. The gate is 0 mismatches over the route.
- **D**, three modes:
  - **`=2` lockstep.** B runs on its thread, but T waits for B to finish op N before starting it (no overlap).
    This is exactly the serial order, and it tests the record, the fragment splice and the ownership move without
    the overlap.
  - **`=3` audit with overlap.** At T time, for each resolved buffer of op N, T runs a read-only probe: the buffer
    lookup without create, plus a "range still clean" check against the page tracker. It compares handle,
    offset and generation with B's record and counts any write that landed between B's and T's execution of the
    op. Every mismatch is a missing drain rule. The gate is 0 over the route.
  - **Stream hash.** Each op's emitted blocks are hashed in order (block kind, pipeline handle, descriptor
    contents by resource identity, barrier masks, image layouts), like the `SOTC_PASS_VERIFY` block hash.
- **Same frame, serial vs split.**
  - Capture one frame of the attract demo, or one fixed spot of the route, twice: `SOTC_RESOLVE_SPLIT=0` and `=1`,
    with RenderDoc via `SOTC_WRAP` + F12 (`tools/rdoc/`).
  - Compare per pass with an `tools/rdoc` script: the per-pass render target images, and the ordered action and
    barrier list.
  - Then the user's visual check on a cold and a warm launch. Flicker and depth defects are the failure signature
    and are not acceptable.

## 6. What 80+ FPS needs, and whether it is realistic

### 6.1 Per thread

| Thread | Now | Needed | Plan reaches (est.) |
|---|---|---|---|
| draw | 25.0 | <= 12.5 | 9-12 only with D; 14-17 without it |
| recorder | 16.0 CPU, 5.3 busy | <= 12.5 busy | 6.5-7 busy after R1 and C1 |
| CP | 13.5 | <= 12.5 | 10.5-12 after CP1-3 |
| GPU (Pro) | unmeasured; PS4 mode 8.4-14.2 ms on the RTX 3060 Ti | <= 12.5 | unknown; likely needs the upscaler (DLSS/FSR render scale) in Pro mode |
| game threads (update, render, job workers) | unmeasured; the trace shows them waiting on the frame | <= 12.5 | measure with `SOTC_TRACE=2` (busy = frame - evf wait); `usleep(1)` costs a 0.52 ms floor (report 05) |

### 6.2 Display cadence

- `BB_VBLANK_HZ=120` puts flips on an 8.33 ms grid.
- A steady 80 FPS cannot sit on it: frames alternate 1 and 2 vblanks (8.3 / 16.7 ms), which is visible judder.
- 80 needs 240 Hz vblanks or a VRR display.
- 120 on the 120 Hz grid needs 8.3 ms per thread, which is not reachable.

### 6.3 Simulation (the blocking issue)

Facts from `docs/re-stutters/reports2/05-frame-pacing.md` and `scripts/sotc_modes.py:59-68`:

- **There is a fixed step F.**
  - The game loop hands some systems a fixed step `F = N / rate` (`DAT_02eee7b0`, set in `GameCoreLoop::GameLoop`
    0x25c080). It is passed to `FUN_00a3f140` (via `sub_25bb30`) and `FUN_003c0df0` (via `sub_25d8b0`).
  - `rate` is the float at display+0x28 (29.97 or 59.94). `N` comes from `TargetFramesPerSeconds` (1 = 60,
    2 = 30).
- **Those systems run at FPS x F.** This is proven by the history:
  - the old `fps60` patch ran at 60 FPS with F = 1/30 s, and physics and timers ran twice as fast (#100 colossi
    flying, #120 the second colossus' wall, #49);
  - `fps60game` fixed it by making F = 1/59.94 s.
- **The real-time dt consumer** (`sub_25ccc0`, `FixedTimeStep` on) tracks wall time on average, but only in
  quanta of `F * 0.5`.
  - At 80 FPS with F = 16.7 ms it alternates steps of 16.7 and 8.3 ms: animation judder.
  - The update thread's own dt (GameCoreLoop+0x3bc0) has an unknown writer.
- **`sotc_fps_cap.h:5` ("Its frame logic follows the real time") is only half true.** The F-fed systems do not.

Consequences:

1. **Uncapped above 60 with `fps60game`** (the launcher's current "Unlimited") runs the F-fed systems at
   FPS/59.94 speed: 1.33x at 80 FPS, 2x at 120. Same defect class as #100/#120. Not acceptable as an 80 FPS mode.
2. **A locked rate R** is possible in principle:
   - patch `rate` (display+0x28) or N, so that F = 1/R; for example R = 80 with `SOTC_FPS_LIMIT=80`;
   - correct only while the frame rate holds R exactly; every drop is slow motion in those systems, as on the
     PS4 at 30;
   - the physics and animation were tuned for 1/30 and 1/60 steps only, so other steps are untested (climbing
     grip, fur, colossus physics);
   - this needs RE of the writer of display+0x28 and of the consumers of F, then a "locked R" mode in
     `sotc_modes.py` and `sotc_fps_cap.h`, then a play test of the #100/#120/#63 spots.
3. **Frame-rate independence** (writing a measured dt into F every frame): high risk.
   - Fixed-step consumers may assume a constant step.
   - `Renderer_FrameTail`'s `counter = (counter + 1) % N` ties render cadence to N.
   - Not recommended without a full RE of every F consumer.
4. **Render-only extra frames:** frame generation exists (`SOTC_FRAMEGEN`: DLSS-G on RTX 40/50, FSR 3 on any GPU;
   `fg_hudless`). A locked 60 simulation and render with frame generation gives 120 on screen with correct game
   speed. It costs GPU time (FSR 3 roughly 2-4 ms on this card, est.) and about one frame of latency.

### 6.4 Verdict

- **Locked 60 in Pro mode: go.**
  - The draw thread needs 25 to under 16.7 ms. The prerequisites, E and C1 (est. 14-17 ms) are enough, with D as
    the margin.
  - The recorder is already fine after R1, and the CP is already under 16.7 ms.
- **80+ real game frames: not realistic in this plan's horizon.** It needs all of these:
  - the draw thread halved, which only the high-risk split D reaches, and only if the prerequisites land at the
    optimistic end;
  - the CP cut by about 2 ms;
  - the GPU at or under 12.5 ms in Pro mode (unmeasured, likely not without upscaling);
  - 240 Hz vblanks or VRR;
  - a game-side "locked 80" step change that has never been tested against the physics bugs.
- **The serial share is the core number:**
  - 80% of 25 ms = 20 ms that no parallel recording scheme removes;
  - an ideal 2-way split of what remains after the prerequisites gives 9-12 ms;
  - every hazard drain pushes that back up.
- **80+ on screen: realistic** as a locked 60 plus frame generation, once 60 holds.

## 7. Risks

| Risk | Shows as | Mitigation |
|---|---|---|
| B runs ahead across a texture invalidation or an aliasing image write | flicker, stale textures, garbage on formatted buffers (the S2 horse-copy class) | drain at submit start, map/unmap and `Func` ops; image-page bitmap check; `=3` audit with 0 mismatches before any timed run |
| fragment spliced in the wrong place relative to `BeginRendering`/`EndRendering` or a layout transition | depth defects, missing passes | fragments only at op start, T closes the pass before a non-empty fragment; `=2` lockstep and the stream hash |
| B triggers a submit, or `CurrentTick` moves under T | wrong command buffer, use-after-free | B never submits: an op needing it drains and runs inline |
| C1 descriptor write after a bind or into an in-use set | flicker on reused sets | write closure always precedes the bind in the same stream; `=2` compares contents |
| P1 partial hit misses a written-range mark | stale reads next op, readback errors | marks are always applied; `=2` against the full bind |
| prepare thread (E) uses stale snapshots | wrong bind | generation-validated probes only, fall back inline; PR_GEN `=2` |
| R1 adds wakes on the draw thread | draw +0.5-1 ms | measured on both threads; wake only at op end if it costs |
| game speed above 60 | colossi and physics too fast (#100/#120) | no uncapped >60 real frames without the locked-R mode; prefer frame generation |
| frame-rate claims based on estimates | wasted waves | S0 measures first; the go/no-go after wave 2 (above 17 ms means stop D and keep the 60 target) |
