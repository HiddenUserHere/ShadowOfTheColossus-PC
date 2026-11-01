# Draw thread: cost map, stutter sources and the end-state design

Analysis and design, 1 Nov 2026, branch `re/stutter-architecture`. Read only: nothing was built or
run, and no file under `gpu/` or `src/` was edited. Sources: the 20 s draw-thread profile
`drawprof-base*.txt` (this folder), the Lab matrix logs in `user/pcmem/*/logs/` (139 logs, 9 `base`
runs of 1 Nov after 06:00), `docs/DRAW-THREAD-7C-DESIGN.md` (phase split and CP profile of 29 Oct),
`docs/BIND-PATH-DESIGN.md`, `docs/re-stutters/reports/10-port-redesign.md`, `switches-review.md`,
`memmove.md`, `PC-MEMORY-REWRITE.md`, `HANDOFF.md` and the code.

Labels: **verified** (checked in code, or computed from log or profile numbers in this session),
**measured earlier** (a number from an earlier report, not re-measured), **hypothesis** (needs a
run). File:line references are to the working tree of 1 Nov.

Thread names: the draw thread is `shadPS4:GpuDraw` (`Liverpool::BackLoop`, `liverpool.cpp:560`);
`shadPS4:GpuCommandProcessor` is the PM4 parser (`liverpool.cpp:2926`); `shadPS4:GpuRecorder`
replays the Vulkan calls (`vk_recorder.cpp:1760`). CLAUDE.md calls the bottleneck
"GpuCommandProcessor"; the profile and the logs show the bottleneck is `GpuDraw`.

## 0. Three findings that change how the earlier results read

1. **"draw busy" is the frame period, not the draw thread's work (verified).** The printer computes
   `(window_ms - draw_idle_ms) / flips` (`core/libraries/videoout/driver.cpp:2126-2129`).
   `draw_idle_ns` only accumulates when `kFpsTimers` is true (`liverpool.cpp:641-658`). In every
   current log it is zero, so the value is `5000 / flips`: 5000/138 = 36.23 (printed 36.23),
   5000/145 = 34.48 (34.49), 5000/131 = 38.17 (38.23) in `20261101-072816/logs/base-r1-a1.log`;
   the matrix summary gives 28.2 FPS / 35.57 ms and 26.4 FPS / 38.14 ms, which is 1000/FPS. The
   statement "the draw thread is at 34.5-36 ms against 33.3 ms" (PC-MEMORY-REWRITE §8, HANDOFF) is
   therefore "the frame is 34.5-36 ms". No draw-thread change could have shown up in that number
   except through FPS.
2. **Every `kFpsTimers`-gated timer reads zero in the current logs (verified), while ungated
   timers work.** Zero: draw waits (`gpu 0.00 (1.5)`: a count with no time), `tc lock`, `cp parse`,
   `CP spin 0.00 ms in 39267 rounds`, `FAULTS avg 0.0 us`, the `FAULT SPLIT`, the `SOTC FRAMES`
   cause buckets, `rb wait`. Non-zero: `presentKHR 0.50 ms`, `no free frame 2.36 ms`, `DMA sweeps
   41.6 ms/s`, `SOTC COMPILE translate 1.9`, `memory lock ... 86.1 ms`. `SOTC_FPS_LEAN` is not set
   (`sotc.ini` has no active lines; `pcmem_matrix.py` COMMON_ENV has only `SOTC_FPS=1`). So
   `kFpsTimers = kFpsLog && !kFpsLean` (`sotc_stats.h:45`) behaves as false although both inputs
   say true. **Hypothesis:** a dynamic-initialization order problem of the `inline const bool`
   flags under the COFF/COMDAT + ThinLTO build (one TU's initializer reads `kFpsLog` before it is
   set). 7C-DESIGN §0 already saw the CP-spin symptom on 29 Oct. Until fixed, the SOTCSTALL "draw
   waits", "rb wait", "faults (ms)" and the `SOTC_DRAW_SPLIT` lines cannot measure anything.
3. **The Lab route does not run the game configuration users run (verified).**
   `tools/pcmem_matrix.py:58` sets `SOTC_CVARS='skipSplash=1 nointro'`. `sotc.py:295-298` applies
   its default `doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0` only when `SOTC_CVARS` is
   unset, and the launcher adds the same pair by default (`perf_culling`, `sotc_launcher_win.py:
   642-645`). Every matrix run measures the game with GPU index-buffer culling **on**. The route
   frames carry ~2,050-2,180 direct dispatches, ~320 indirect dispatches, ~2,200-2,560 indirect
   draws with GPU-written arguments, ~1,100 with CPU arguments and ~940-1,120 direct draws
   (`SOTC cmds/frame`, `20261101-042755/logs/drawverify-r1-a1.log:395, 470`). **Hypothesis:** a large
   part of the direct dispatches and of the GPU-args indirect draws is that culling; report
   reports2/14 says the cvars "cut index work and GPU dispatches". To be measured first (§5, Q1).

## 1. Per-frame cost of the draw thread

### 1.1 Method

The profile (`drawprof-base.txt`, 11,446 samples in 20 s, `SOTC_PROFILE=GpuDraw`-style wall-clock
sampling: SuspendThread + GetThreadContext every ~1.75 ms, `src/probe.c:499-540`) counts the thread
wherever it is, running, blocked or preempted. Shares are therefore shares of wall time. The run is
the Lab route, base config, ~29 FPS, so one frame is ~34.5 ms of wall time and ~19.7 samples. The
binary of that profile (built 03:56) no longer exists: `out/gpu/bb-probe.exe` was rebuilt at 07:40,
so the dump cannot be re-symbolized and only the 133 lines of `drawprof-base-sym.txt` are usable.
Inclusive shares overlap; the table below is arranged so that indented rows are parts of the row
above. The LTO build folds identical small functions, so small-function names are approximate.

### 1.2 Breakdown (route, ~6,600-7,100 ops per frame, ms of the 34.5 ms frame)

| Part | Share | ms/frame | Hot code |
|---|---|---|---|
| **Not working**: BackLoop idle spin (self) | 5.2% | 1.8 | `liverpool.cpp:688-697` (4096 `CpuRelax`) |
| **Blocked in the kernel** (alert-by-thread-id 3.3, SRW condvar 3.0, WaitForMultipleObjects 2.5, delay 0.8) | 9.6% | 3.3 | flip `GetRenderFrame` (`vk_presenter.cpp:1999`; "no free frame" 1.4-2.4 ms/frame in heavy windows), `DrawWait` 3.8%, copy-pool waits |
| **Guest commands between ops** (`ProcessCommands`) | 7.4% | 2.6 | `liverpool.cpp:2876-2898`: readbacks for guest threads |
| ↳ `ReadMemory` / `FinishAsyncReadback` / `ApplyDownloads` | 5.7 / 5.7 / 3.3 | | `buffer_cache.cpp:1168, 4980, 5449` |
| **Op execution** (`ExecuteBackOp`) | 82.0% | 28.3 | `liverpool.cpp:1168` |
| ↳ by kind: direct dispatch / indirect draw / direct draw / indirect dispatch | 32.1 / 25.5 / 13.0 / 5.8 | 11.1 / 8.8 / 4.5 / 2.0 | `vk_rasterizer.cpp:2306, 1999, 1850, 2481` |
| ↳ per op (counts from the drawverify run, same route) | | ~5.3 / ~2.5 / ~4.5 / ~6.2 µs | |
| ↳ **`BindResources`** | 46.7% | 16.1 | `vk_rasterizer.cpp:2953` |
| &nbsp;&nbsp;↳ **buffers** `BindBuffers` | 19.2% | 6.6 | `vk_rasterizer.cpp:5199` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ `ObtainBuffer` → `SynchronizeMemory` | 15.5 → 13.4 | 5.3 → 4.6 | `buffer_cache.cpp:5886, 7771, 7865` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ page bitmap scans (`MemoryTracker::IteratePages`, `RegionManager::ForEachModifiedRange`) | 8.4 / 7.0 | 2.9 | `memory_tracker.h:129-237`, `region_manager.h:788` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ re-protect (`SignalImpl::Protect` → `Core::AddressSpace::Protect`) and page watchers | 6.8 / 5.8 | 2.3 / 2.0 | `page_manager.cpp:1809`, `runtime_memory_gpu_protect` |
| &nbsp;&nbsp;↳ **textures** `BindTextures` / `BindTexturesFull` | 17.8% | 6.1 | `vk_rasterizer.cpp:5867, 6035` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ `FindImage` (memo, robin hash, region walk) | 9.5% | 3.3 | `texture_cache.cpp:1181, 2972` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ `UpdateImage` / `RefreshImage`, `ResolveOverlap`, `FindTexture` | 4.3 / 3.9 / 3.6 | 1.5 / 1.3 / 1.2 | `texture_cache.cpp:1689, 740, 1535` |
| &nbsp;&nbsp;&nbsp;&nbsp;↳ guest texture copy into staging (`ObtainBufferForImage`) | 3.6% | 1.2 | `buffer_cache.cpp:6540` |
| &nbsp;&nbsp;↳ DMA sweeps (`SynchronizeDmaBuffers`) | 4.5% | 1.6 | `buffer_cache.cpp:6808` ("DMA sweeps 145/s, 41.6 ms/s" in the log agrees) |
| &nbsp;&nbsp;↳ CPU copy shaders (`ExecuteShaderHLE`) | 3.3% | 1.1 | `vk_shader_hle.cpp` |
| ↳ render targets and passes: `BeginRendering` / `PrepareRenderState` | 4.7 / 4.2 | 1.6 / 1.4 | `vk_rasterizer.cpp:6605, 289` |
| ↳ rest of the op (emission into the recorder stream, register replay, user data copy, func ops, waits inside ops) | ~26% | ~9 | |

Kernel and CRT leaves, overlapping the rows above (`drawprof-base-system.txt`):

| Leaf | Share | ms/frame | Owner |
|---|---|---|---|
| `ZwProtectVirtualMemory` | 6.4% | 2.2 | re-protects after uploads and GPU-write watchers |
| `NtGdiDdDDICreateAllocation` + `DestroyAllocation` | 4.6% | 1.6 | driver allocations on the draw thread. **Hypothesis:** image churn ("IMG FREE ... freed 610, registered 1243" per 5 s: overlap-block, overlap-chance and expand recreate 120-340 images/s), dedicated image allocations, staging growth |
| `memmove`/`memcpy`/`memcmp` (ucrtbase) | 13.8% | 4.8 | guest → write-combined staging 5.5% (`memmove.md` item 1), HLE table compare/build 1.9%, guest write-back 1.4% |

What the profile says as a whole (verified): it is **flat**. The top self-time function after the
idle spin is 1.5%; the top 40 self entries are each 0.3-1.5%. About 55-60% of the working time is
cache validation and mutation inside `BindResources`; the pure emission (descriptor push, dynamic
state, the draw call) is ~5% (7C-DESIGN §0, measured earlier: 1.1 ms of 20.3 ms).

### 1.3 The other GPU threads (for the whole picture)

| Thread | CPU per frame (route) | Real work | Source |
|---|---|---|---|
| draw (`GpuDraw`) | 29.2-35.4 ms (`SOTC cpu/frame`) | ~28-30 ms (profile: 85% of wall minus waits) | verified |
| command processor | 27.3-34.5 ms | ~15 ms; ~10 ms spinning on labels the draw thread writes; pipeline lookup ~22% of samples, PM4 parse ~12% | 7C-DESIGN CP profile, measured earlier |
| recorder | 33-38 ms | busy 8-10 ms; the rest is spin (`vk_recorder.cpp:1781-1795`, 250 µs spin) | verified (`SOTC recorder/frame: busy 8.28`) |
| upload threads | 4.6-6.9 ms | | verified |

Coupling (verified in code): the game's command stream waits on labels (`WAIT_REG_MEM`), and the
port writes EOP labels when the draw thread executes the op (`liverpool.cpp:3994-4079`), not when
the GPU finishes. So the CP, and through the submit gate the guest's render thread, run in
lockstep with the draw thread's progress (STUTTER-ARCHITECTURE §1). "CP waits for the draw thread"
54-73/s, label spin rounds ~39-43k per 5 s.

## 2. The spikes, separately from the steady cost

`SOTCSTALL` (`vk_presenter.cpp`, frames over ~70 ms) of the 9 `base` runs of 1 Nov after 06:00,
frames after 150 (boot dropped): 97 stall frames. Parser:
`scratchpad/stall.py` (session scratchpad, not in the repo). "cpu draw/cp/rec" are thread CPU times
in that frame; "rec busy" is the recorder's real work; "upload" is the upload threads' CPU. The
route (`user/routes/tests.json`) is a free-camera flight at 81.8 units/s, so it streams much faster
than play.

| Type | Where (frame) | Frame ms | Signature | Share | Cause |
|---|---|---|---|---|---|
| A. Area-load image burst | once per run, 163-223 | 142-215 | 707-721 images in one frame, upload threads 39-136 ms, draw 108-164 ms, faults low (25-41) | 9 of 97 | first-use texture creation + upload after "Loading a save or a new area" (verified signature; the cause is the 700-image frame) |
| B. Heavy burst frames | ~3 per run, at fixed route points (~285, ~430/568-589, ~706-727) | 127-175 | readbacks 100-285 per frame (normal ~40), write faults 490-1,077 (normal ~200), **recorder busy 25-49 ms (normal 8-10)**, draw CPU 60-100% of the frame | ~30 of 97 | 3-5x the normal command volume in one frame, each op at the normal cost, plus the readback and fault chains that volume brings. Draw-bound. (Volume from recorder busy: inference; the streaming cause: hypothesis) |
| C. Guest-side, draw idle | ~153 (route start) and some at 430-442, 573, 697 | 97-138 | draw CPU 5-35 ms, cp similar, recorder CPU = frame (spinning, busy 1-11 ms) | ~20 of 97 | the game did not submit: game CPU, IO, guest waits (readback faults, memory lock). Not a draw-thread cost; unmeasured because the timers read zero (§0.2) |
| D. 70-90 ms frames | scattered | 70-90 | draw-bound, upload 25-60 ms, `tc` lock wait counts 13-34 | rest | uploads + fault contention on the texture cache lock (count only, no time) |

What is **not** a spike source in these warm runs (verified from the same lines): pipelines (0 in
every spike frame), shaders (0, or 1 in type A), emergency collections (0, `SOTC VRAM` line),
unmap GPU waits (38 per 5 s, 18 ms total). Compiles matter only on a cold shader cache.

Reading: in types A, B and D the draw thread does more work in one frame; nothing in it waits
long. A spike is **per-op cost x burst volume**, with no slack to absorb it, because (a) the steady
load is ~85-90% of the 33.3 ms budget, and (b) the guest cannot run ahead of the draw thread (labels
and the submit gate). ring8 cut the max frame from ~150 to 50-67 ms (HANDOFF, run 065448/065841)
because it removed a class of fault and readback chains from the bursts, while the steady per-op
cost stayed.

## 3. Root causes, ranked, and why the switches did not help

| # | Cause | Steady cost | Spike role | Evidence |
|---|---|---|---|---|
| 1 | **Coherence validation on the bind path.** Every buffer bind re-asks the page tracker whether its pages changed (`SynchronizeMemory` → bitmap scans), uploads dirty pages through write-combined staging, re-protects them (VirtualProtect), and every written binding re-arms GPU-modified watchers (`AddGpuModified` takes `gpu_modified_mutex` and grows a RangeSet, `buffer_cache.cpp:5990-6060`). Textures do the same through `RefreshImage`/`ObtainBufferForImage`, DMA sweeps through `SynchronizeDmaBuffers`. The work is proportional to coherence events (CPU writes, GPU writes, readbacks), and the check is per page per bind | ~10-11 ms (buffers 6.6 + DMA 1.6 + texture refresh/copy ~2.7) | main: bursts multiply faults and readbacks 3-5x | profile §1.2, code verified |
| 2 | **Texture identity per bind.** `FindImage` memo + hash + region walk, overlap resolution, view lookup for each T# on every draw | ~5-6 ms | moderate | profile; FIND MEMO counter exists (switches-review) |
| 3 | **Compute dispatch volume on the Lab config.** ~2,100 direct dispatches per frame at ~5.3 µs = ~11 ms; dispatches carry written bindings (the expensive path in #1) | up to ~11 ms (the share due to GPU culling is unknown) | yes | §0.3; hypothesis on the culling share |
| 4 | **GPU→CPU results through faults and readbacks**, serviced on the draw thread between ops, plus early labels that force the guest into lockstep | ~2.6 ms + CP spin ~10 ms (on the CP) | large (type B, readbacks 100-285 per frame) | profile, logs; `liverpool.cpp:3994-4079` |
| 5 | **Image churn**: 120-340 images/s created and freed in steady play (overlap-chance, overlap-block, expand), with driver allocations on the draw thread | ~1.6 ms (D3DKMT) + uploads | type A (700 images) | `SOTC IMG FREE` line, system leaves (attribution: hypothesis) |
| 6 | **Render-pass and render-target state per draw**: `BeginRendering`, `PrepareRenderState`; passes are cut by `FlushPeriodic` every 256 commands (`vk_rasterizer.cpp:4655`) | ~3 ms | small | profile |
| 7 | **Flip pacing inside the draw thread**: `GetRenderFrame` blocks for a free frame | 1.4-2.4 ms heavy, 13.7 ms light | delays the CP behind it | `SOTC FLIP WAIT` line |
| 8 | **CPU contention**: recorder spin (~25 ms/frame) and CP label spin (~10 ms/frame) on a 8-core/16-thread CPU running ~7 heavy port threads; ETW of 29 Oct: draw thread on-CPU ~60% in that session | unknown | unknown | hypothesis (CHANGELOG ETW entry; `vk_recorder.cpp:1781-1795`) |

**Why the micro-switches showed nothing** (TEX/BUF_BIND_FAST, VK_POOL, PROTECT_BATCH,
PRESENT_UNLOCKED, UD_SWAP, HLE_TABLE_GEN, TEX_PROTECT_BATCH, bind memo, C1-C5, PARALLEL_RECORD):

1. **Each one targets one slice of a flat profile.** Their ceilings are ≤ 2% of samples
   (≤ 0.7 ms per frame; switches-review.md, verified). The fixed per-op ring cost is 0.12-0.2 µs of
   ~5 µs (SOTC_OPSTAT, measured earlier). No single cut can move a 28 ms total made of six ~2-6 ms
   subsystems.
2. **The measurement could not see them.** "draw busy" is the frame period (§0.1); the gated
   timers read zero (§0.2); one-round matrices and 1+1 A/Bs have a spread of 1-3 ms (CHANGELOG,
   PC-MEMORY-REWRITE §8: base 26.2-28.1 FPS across rounds). A real 0.5-1 ms gain is invisible.
3. **Memos skip the cheap binds.** `SOTC_BUFFER_MEMO` hit 96-99% of buffers with no gain
   (CHANGELOG, measured earlier): the binds it answered were already clean, so the full path for
   them was already the early "nothing dirty" return (`region_manager.h:796-810`). The cost sits in
   the dirty binds and in the per-bind questions that a memo still has to ask ("did the memory
   under it change?"), which are the same page-bit reads. FASTBIND hits only 43%.
4. **Parallel record moved the wrong part.** It moved emission (~5%) off the draw thread; the
   cache mutation, which is ~55-60% of the work, stayed (7C-DESIGN §0-§1).
5. **ring8 moved real work but only for one memory class**, and replaced it with `PcHostBind`
   lookups; it cut the coherence chains in the bursts (max frame down) but not the texture,
   render-state and dispatch costs (mean unchanged).

## 4. End-state architecture

Principle: **move the cost from "every bind asks" to "every change tells"**, and stop emulating
unified memory with page protection on the hot path. The draw thread should only do work when
something changed (a new resource, a dirty range, a new render target), and the guest should wait
on the GPU, not on the draw thread.

### 4.1 Target picture

```
guest submit ─► CP thread: PM4 parse, dirty-register masks ─► op ring
                                         │
                       ┌─────────────────┴──────────────┐
                       ▼                                ▼
            resolve workers (2): pipeline key/lookup,   (read-only against
            sharp decode, resource memo by sharp +       generation snapshots)
            generation, descriptor indices, RS key
                       │ PreparedOp (hit | needs-owner)
                       ▼
            owner thread (today's GpuDraw): misses only: create images/buffers,
            upload dirty granules, transitions, render-pass decisions, events
                       │ compact op stream
                       ▼
            recorder (as today; per-pass secondaries only if busy > 50%)
                       ▼
            GPU: reads host-shared memory in place, writes labels and
            GPU→CPU results into host memory itself
present thread: acquire/present/pacing (the owner never blocks on a free frame)
```

Memory classes (extends PC-MEMORY-REWRITE §4 and the existing switches):

| Class | What | Where it lives | Tracking | Existing piece |
|---|---|---|---|---|
| HOST_SHARED | CPU-written, GPU-read dynamic data: per-frame arenas, CPUMemory granules the GPU binds, constants, instance/skinning data | guest pages imported as host memory (VK_EXT_external_memory_host); the GPU reads in place | none on the bind path; ordering by labels (4.3) | ring8 (`SOTC_PC_RING=8`), `SOTC_PC_RING8_HEAPS=CPUMemory`, `pc_ring.cpp` |
| GPU_STATIC | meshes, textures from the resource loader | device-local | per allocation / 64 KiB granule generation, bumped by the loader and allocator hooks; page protection only as a coarse safety net | `SOTC_STATIC_CLASS`, alloc probes |
| GPU_PRIVATE | render targets, GPU-only scratch | device-local | none; never downloaded | `SOTC_RT_CLASS` |
| GPU_TO_CPU | XPR masks (eboot 0x1a6d20/0x1a6d60), particle stats (0x101a98, 0x8c1b0 ring), labels | written by the GPU into imported host pages (or copied there by the GPU at the producer's end) | none; the CPU reads plain memory | `SOTC_RB_CLASS=1`, XPR and particle probes, host_import |
| UNKNOWN | the rest | as today | today's page tracker (shrinks over time) | `memory_tracker.h`, `page_manager.cpp` |

### 4.2 Components, with gain, risk, effort and dependencies

Gains are estimates for the heavy route (~6.6-7.1k ops/frame), relative to ~28-30 ms of draw work.

| # | Component | Concrete change in this codebase | Expected gain | Risk | Effort | Depends on |
|---|---|---|---|---|---|---|
| M0 | Measurement repair | fix `kFpsTimers` (§0.2); a "draw work" metric = Σ rdtsc of `ExecuteBackOp` + `ProcessCommands` excluding flip and idle (the `SplitTimer` parts already exist, `sotc_stats.h:984-1020`); op counts per SOTCSTALL frame; archive the exe with each profile; Lab cvar parity | enables every gate below | none | 1-2 days | – |
| M1 | HOST_SHARED generalised | ring8 to default; heap serving for every CPUMemory granule the GPU binds; constants and instance data; clip rule kept (`PcHostClipLimited`) | 3-6 ms steady; type B bursts smaller (faults -35%/-73% measured for the arena alone) | medium (seen: vertex corruption until the straddle clip; loading-screen lines until the leave fix) | 1-2 weeks | M0, safe labels (4.3) for GPU writes into it |
| M2 | GPU_STATIC + GPU_PRIVATE by class | `SOTC_STATIC_CLASS=1`, `SOTC_RT_CLASS=1` default; static mesh ranges skip the page scan (static class mode 3 is written, not wired: HANDOFF) | 1-3 ms | low-medium (stale data if an unhooked writer exists; `SOTC_STALE_CHECK` is the oracle) | 1 week | M0 |
| L1 | Honest labels by deferral, generalised | today an EOP label is early unless ring8 served writes since the previous one (`liverpool.cpp` EventWriteEop, `pc_ring.h`). End state: a label is deferred whenever GPU work since the previous label touched HOST_SHARED or GPU_TO_CPU memory; the GPU writes it (fill/update into the imported label page after a barrier), and the CP's wait flushes the open command buffer once (the ring8 CP-wait flush + watchdog). Cover ReleaseMem and EventWriteEos too | removes the correctness reliance on read faults; CP spin turns into real GPU waits | medium-high (each deferral is a GPU round trip; must stay rare: ring8 deferred 116-147 of 23-29k) | 2 weeks | M1 |
| R1 | GPU_TO_CPU without faults | XPR masks and particle stats written into host pages by the GPU (or a GPU copy at the producer's end into a host-cached ring), honest labels from L1; drop the fault/readback path for those ranges | readback service on the draw thread -2 ms; waited readbacks 700-960/s → < 100/s; type B bursts lose their readback chains | high (vertex explosions were seen with stale serving, `SOTC_RB_CLASS=2`); correctness rests on L1 | 2-3 weeks | L1 |
| V1 | Generation-validated bind path | a per-granule (64 KiB) generation counter bumped by every invalidation (fault, declare, GPU write mark, unmap); a resource memo keyed by the raw sharp words (V#/T#, sampler) + the generation of its granules; a hit returns arena offset / image view with no page scan, no lock; a miss takes today's path and refreshes the memo. Texture identity uses the same generations. This replaces `SyncGeneration` + `SyncFastSlot` (`buffer_cache.cpp:7810-7860`) with one model shared by buffers, textures and DMA sweeps | 4-8 ms (the hit path of `BindBuffers`, `FindImage`, `RefreshImage`, `SynchronizeDmaBuffers`) | medium (a missed bump = stale data; `SOTC_STALE_CHECK` + a `=2` verify that runs the full path and compares) | 3-4 weeks | M0; most value after M1/M2 shrink the UNKNOWN class |
| D1 | Descriptor strategy | short term: keep push descriptors (emission is ~5%; descriptor buffers exist as `SOTC_DESC_BUFFER`, no measured gain). End state: bindless heap (descriptor indexing) with a persistent slot per image view and sampler (`SOTC_BINDLESS_SAMP` is the sampler half), per-draw push constants carry slot indices. Buffers: BDA through a GPU-side block table (guest block → arena device address), so shaders read V#s and address memory themselves | heap: ~1 ms; BDA buffers: most of `BindBuffers` that V1 leaves (2-4 ms) and the per-draw descriptor emission | high (recompiler change, shader cache rebuilt, NVIDIA/AMD/Intel validation) | 4-6 weeks | V1 |
| P1 | Pipeline strategy | already async (pipeline workers, async compute/translate, tiny-draw async, permutation collision fix). End state: pipeline key from dirty-register masks on the CP (recompute only when the registers it reads changed; `RefreshGraphicsKey`/`RefreshFlatBuf` are ~13%+9.5% of CP samples), pipeline lookup on the resolve workers; GPL fast-link (VK_EXT_graphics_pipeline_library) for cold misses instead of skipped draws | CP -4 to -6 ms (headroom for 60 FPS); no spike gain in warm runs | low-medium | 2 weeks (keys), +2 (GPL) | – |
| T1 | Image churn and first use | render targets created at their maximum extent (no "expand" recreation), overlap-chance aliases as views of one image where formats are compatible, images from the pooled allocator without dedicated allocations, image creation on the `ImageCreate` threads (`image_prefetch.cpp`) within the existing upload budget; pre-stage decoded textures at loader completion (STUTTER-ARCHITECTURE step 5) | ~1.5 ms steady; type A 150-215 → < 60 ms (estimate) | medium | 2-3 weeks | M0 |
| F1 | Pacing off the draw thread | move `GetRenderFrame`'s wait for a free frame to the present thread; the owner only enqueues the flip (reports2/12) | 1.4-2.4 ms in heavy windows; removes a CP stall chain | low-medium | 1 week | – |
| F2 | Frame pipelining | `SOTC_CP_AHEAD=2` with one frame ahead once labels are honest (L1); clamp the game's dt (`gameMaxDeltaTime`) | bursts spread over two frames: a 2x burst no longer hitches | medium (unfenced guest double buffers, `gnmdriver.cpp:98-155`) | 1 week after L1 | L1, R1 |
| W1 | Resolve workers | rebase `SOTC_PARALLEL_RECORD` + `SOTC_PR_RESOLVE` on V1's generation snapshots (read-only lookups; PR_RESOLVE diverged 12 times because it re-derived mapping checks, not snapshots); misses flagged to the owner | owner -30-40% of what remains after V1 | medium (ordering; `OpFootprint` exists, `draw_prep.h`) | 3-5 weeks | V1, D1 helps |
| S1 | Parallel command buffers | per-pass secondaries (`SOTC_PASS_SEC`) only if the recorder's busy time passes ~50% of the frame after W1; N emit workers stay rejected (7C-DESIGN §4.1) | 0 today (recorder busy 8-10 ms) | – | – | W1 |

Not proposed: N identical draw workers with a sequencer (Amdahl ceiling ~1.1x, 7C §4.1); a texture
‖ buffer stage pipeline over today's page tracker (93% page-footprint conflicts, 7C §0); more
fine-grained locks in the caches (T48 lesson).

### 4.3 The ordering rule that makes host serving safe

HOST_SHARED and GPU_TO_CPU memory is read and written by the GPU in place, so the GPU's progress
must be visible to the game through its own labels. Rule: an EOP/ReleaseMem/EOS label is written
by the draw thread at op time (early, today) **only** if no GPU work since the previous label
touched HOST_SHARED or GPU_TO_CPU memory; otherwise it is deferred and written by the GPU (or by
the scheduler when its tick completes), and a CP wait on it flushes the open command buffer once.
This is what ring8 labels already do for the arena (`SOTC_PC_RING8_LABELS`, default on in mode 8).
The deferral rate is the cost meter: it must stay a small share of labels (gate in §5).

### 4.4 Expected end state (estimate)

Draw-thread work on the heavy route from ~28-30 ms to ~12-16 ms per frame (M1+M2 ~5 ms, V1 ~6 ms,
R1+F1 ~3 ms, T1 ~1.5 ms, before W1). With F2, a 3x burst costs ~40 ms of draw work spread over two
frames. Target: p99 ≤ 36 ms and max < 60 ms at 30 FPS on the Lab route; draw work ≤ 16 ms leaves
room for 60 FPS once the CP's pipeline-key work (P1) is done.

## 5. Phased plan with gates

Gate protocol for every step: Lab route with cvar parity (both culling cvars set as users have
them), the switch off and on alternated over at least three one-round matrix runs, one switch per
comparison; the metric is **draw work ms/frame** (M0), not "draw busy" or FPS; frames > 50 ms and
the route max must not rise; `SOTC_STALE_CHECK=16` reports 0 stale pages in a separate run; the
user's visual check for anything that changes data paths.

| Phase | Content | Gate to pass |
|---|---|---|
| 0 | M0. Q1: route with culling cvars on vs off (dispatches/frame, draw work). Q2: `SOTC_REC_SPIN_US=0` vs default (contention hypothesis). Q3: `SOTC_STAGING_CACHED=1` (WC copies, memmove.md) | timers non-zero in SOTCSTALL; a draw-work line per 5 s; baseline spread known (3 runs) |
| 1 | M1 + M2 to defaults (ring8, heaps, static class 3 wired, RT class) | write faults < 1,500/s total; draw work -4 ms; p99 not worse; 0 stale; no visual change on 3 long sessions |
| 2 | L1 + R1 | waited readbacks < 100/s; label deferrals < 2% of labels; `ProcessCommands` < 2% of draw samples; no vertex or particle artefacts |
| 3 | F1 + F2 | flip wait not on the draw thread; frames > 50 ms halved vs phase 2; no double-buffer corruption |
| 4 | V1 (with `=2` verify) | `BindResources` < 25% of draw samples; draw work -5 ms vs phase 3; verify mismatches 0 |
| 5 | T1 | type A spike < 60 ms; D3DKMT leaves < 1% of draw samples; images registered per 5 s down by half |
| 6 | P1 + W1 | owner draw work -30%; CP work < 10 ms/frame; PR verify 0 mismatches |
| 7 | D1 (bindless, BDA) | `BindBuffers` < 3%; recorder busy -30%; shader cache rebuilt once (CHANGELOG note) |

## 6. Quick wins (can land first)

1. **Lab cvar parity** (`tools/pcmem_matrix.py:58`): add `doIndexBufferCulling=0
   doSunShadowIndexBufferCulling=0` to `SOTC_CVARS` (or run both as two configs). Zero risk: it is
   the users' default. It may change the absolute numbers of every earlier conclusion.
2. **Fix the timers and the "draw busy" line** (`sotc_stats.h:27-45`, `driver.cpp:2126-2129`):
   print `kFpsLog/kFpsLean/kFpsTimers` at startup to confirm §0.2; if it is the init order, make
   the flags function-local statics or read the env once in an explicit init; print draw work from
   the split ticks instead of window minus idle.
3. **Archive the binary with every profile** (`out/gpu/bb-probe.exe` next to the dump), so the
   1 Nov profile problem (rebuilt binary, no re-symbolization) does not repeat.
4. **Promote what is already measured and safe**: ring8 with the clip and labels (28.8-29.1 FPS vs
   28.6, max 50-67 vs 136-160 ms), after three alternating runs with parity.
5. **`SOTC_STAGING_CACHED=1`** measured with the draw-work metric (5.5% of samples are memcpy into
   write-combined staging).
6. **Recorder spin off** (`SOTC_REC_SPIN_US=0` or a short sleep) as an A/B for CPU contention: one
   env var, no code.
7. **Wire static class mode 3** (written, not wired; the `buffer_cache.cpp` hunks are in the
   HANDOFF transcript) and measure it with the draw-work metric.

## 7. Verified vs hypothesis (summary)

Verified in this session: the "draw busy" formula and its equality with the frame period; the
gated timers reading zero while ungated ones work; the Lab cvar override; the profile shares and
kernel leaves; the op counts per frame (drawverify log); the SOTCSTALL spike types and their
counters; the bind path's per-bind scan, protect and `AddGpuModified` locking (code); early EOP
labels (code); pipelines absent from warm-run spikes.

Hypotheses: the cause of the zero timers (init order); the share of dispatches due to GPU culling;
type B bursts being streaming at route speed; the D3DKMT allocations being image churn; CPU
contention from spinning threads; every gain estimate in §4.
