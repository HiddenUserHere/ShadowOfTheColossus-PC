# Parallel draw preparation: splitting the GpuDraw thread (design, base 904bf26)

Companion to ARCH-DRAW.md. ARCH-DRAW removes the per-binding CPU step by changing what the shaders read
(GPU addresses, heap, one layout). This document covers the other route: keep today's binding model and move
its pure part to worker threads, with the draw thread keeping only the ordered, cache-mutating commit. It
builds on the existing prep mechanism (SOTC_PARALLEL_RECORD, SOTC_PR_RESOLVE) and does not add a second one.

## Goal
- One frame of ~7,000 ops (brief figure; see Measurements) costs the GpuDraw thread well under 16.7 ms wall.
- Rendering results unchanged on NVIDIA, AMD and Intel Arc. Extra memory: small per-worker buffers only.
- Every stage behind a switch with a verify mode (=2) that compares against the inline path.

## Measurements (what the repo records, and what does not match the brief)
| Source | Figure | Note |
|---|---|---|
| ARCH-DRAW.md "Why" | draw thread ~25 ms/frame, ~8 us/op, ~3,150 draws + dispatches per frame | repo figure |
| PERF4-BRIEF.md (line 16) | BindResources 35.8% of the thread; BindBuffers 16.5% (ObtainBuffer 9.0% inside it); BindTextures 12.9% (FindImage 4.0%) | repo figure |
| PERF2-BRIEF.md (line 17) | BindBuffers 16%, BindTextures 15.5%, ObtainBuffer 10.8%, TexBlockKey::TexBlockKey 3.7% self | repo figure |
| Brief for this task | BindResources ~45%, BindBuffers 16%, TableMemo::Resolve 8%, BindTextures 8%, predicates ~15%, BeginRendering 3%, PrepareRenderState 3%; 4-6 us/op; ~7,000 ops/frame; ~30 ms/frame | NOT found in the repo docs. Treat as unverified until phase S0 measures it |
| docs (all) | no recorded speed result for SOTC_PARALLEL_RECORD or SOTC_PR_RESOLVE | the prep gain has never been measured |

Consequence: the percentages in the savings table below are taken from PERF4 where possible, and the brief's
figures are marked. Ops per frame and us/op differ between the two sources (3,150 vs ~7,000 ops; 8 vs 4-6 us).
Stage S0 settles both before any code change is judged.

## What already exists (do not duplicate)
- `Liverpool::PrepLoop` (liverpool.cpp:1716): N GpuPrep threads (`SOTC_PARALLEL_RECORD=1`, default off; workers =
  hardware threads / 4, clamped; refused below 7 hardware threads). Each replays the op's register chunks into its
  own `Regs`, and computes the draws with `seq % prep_workers == index`. Results go into `prep_ring` (PreparedDraw,
  draw_prep.h). Publication: `ready_seq` (release store).
- `Liverpool::TakePrepared` (liverpool.cpp:1804): the draw thread uses a slot only if `ready_seq == seq`,
  `valid`, and `map_epoch == rasterizer->MapEpoch()`. Counters: `pr_prepared`, `pr_not_ready`, `pr_epoch`,
  `pr_ineligible` (sotc_stats.h:240).
- `Rasterizer::PrepareDraw` (vk_rasterizer.cpp:8454): register-only values: DynamicValues (ComputeDynamic), push
  data (MakeUserData), FastBindStatic/Hash, BindCacheEligible, BindResourceHash, draw offsets, footprints.
- `Rasterizer::PrepareResources` (8507, `SOTC_PR_RESOLVE=1` or `SOTC_PR_GEN`): per stage, per buffer
  (`PrResolveBuffer`, 2981: V# base/size, ClampRangeSize, unbounded-window limit) and per image (`PrResolveImage`,
  3019: T# words, verdict null/rejected/bindable). Used by BindBuffers and BindTextures via `pr_stage`.
- `Rasterizer::PrefetchImagesShared` (8545): prep threads prefetch textures under `prep_prefetch_mutex`.
- `Rasterizer::CheckPrepared` (8554): `SOTC_VERIFY=1` recomputes everything inline and compares.

What this leaves on the draw thread: all cache-mutating work (ObtainBuffer, FindImage, GetSampler, Transit,
barriers, descriptor writes, bind-cache state), the predicates that decide each binding, the table memo and the
texture-block key, and the closures the recorder receives.

Gap: `DispatchDirect` (2413) and `DispatchIndirect` (2615) receive no prepared resources. Only the footprint
(`XpFillResources`) runs on the prep thread for dispatches. Compute binds are therefore not prepared at all.

## Work classification
Class A: pure function of the op's captured registers, flat copy and immutable pipeline data (can run on workers).
Class B: reads or mutates shared cache state (must run on the draw thread, or on workers only as a read-only
lookup validated at commit). Class C: stream order (must stay sequential).

| Piece of work | Where (file:function) | Class | Notes |
|---|---|---|---|
| Register replay, flat/user data copy | liverpool.cpp:PrepLoop, ExecuteBackOp | A (done) | already on prep threads / CP |
| Dynamic state values | vk_rasterizer.cpp:ComputeDynamic (8637) | A (done) | ApplyDynamic stays on draw thread |
| Push data, fast hash, cache eligibility, res hash | PrepareDraw (8454) | A (done) | |
| Draw offsets | PrepareDraw, GetDrawOffsets (747) | A (done) | |
| Buffer V#: base, size, ClampRangeSize | PrResolveBuffer (2981) | A (done under PR_RESOLVE) | `memory->ClampRangeSize` must be safe off-thread (check) |
| Image T#: words, null/invalid, mapping verdict | PrResolveImage (3019) | A (done under PR_RESOLVE) | |
| Buffer predicates: shader-addressed, special, written | BindBuffers lambda `shader_addressed` (5995), uses at 6107, 6161, 6208, 6420, 6482; `Shader::ShaderAddrBuffer` | A, not done | depends on pipeline profile only; per descriptor, per op |
| Bindless / array / storage-image classification | IsBindlessStage (100 / 2772 / 3638), PrStageResolvable (2966) | A, partly | stage-level, constant per pipeline |
| Sampler key from S# words | BindSamplers (7813), texture_cache.GetSampler (7849) | A for the key, B for creation | key on worker; GetSampler under samplers_mutex stays |
| Texture block key (TexBlockKey) | MakeTextureBlockKey (6661), TryTextureBlock (6736) | A for the key build, B for the memo | PERF2: 3.7% self |
| Table memo key (heap index table) | TableMemo::Resolve (3290), sotc_arch | A for the content hash, B for the memo lookup and FindImage | |
| Render target descriptors (format, extent, hint) | PrepareRenderState (353), `construct_at(&desc, ...)` (380, 391) | A | |
| Render target image lookup | PrepareRenderState -> texture_cache.FindImage (381, 393), BatchLock (355) | B | mutates LRU/tick stamps |
| Buffer range to buffer lookup | BindBuffers -> buffer_cache.ObtainBuffer (6380), BufferCache | B | creates entries, uploads, granule state |
| Image lookup for T# | BindTextures (6975) -> FindImage (7243, 7414, 7496), FindView | B | creates images and views |
| Layout tracking, barriers | runtime.Transit, RefreshImageLayouts (2745), BeginRendering (7879), Passes::Decide, runtime.FlushBarriers (2004) | C | stream order |
| Bind cache state (memos, generations) | BindCacheSync(Frame) (3317), TryBindCache(Frame), CommitBindCache(Frame) | B/C | draw thread only (g_bind_diff is draw-thread state) |
| Descriptor write list | BindResources (3206) set_writes, Pipeline::BindResources (2010) | C | per-op, draw thread |
| Vertex / index buffer binds | BindVertexBuffers (5139), BindIndexBuffer (5258) | B | ObtainBuffer |
| Dynamic state commit | UpdateDynamicState (8620) -> DynamicState::Commit | C | |
| Recording | scheduler.CommandBuffer() wrappers (vk_recorder.cpp: g_recorder->Run) | C | the draw thread only builds closures; GpuRecorder does the real work |
| Pass secondaries | SOTC_PASS_WORKERS (vk_recorder.cpp) | not on this thread | irrelevant to draw-thread cost |

## Staged plan
Savings are per frame, as a share of the draw thread (baseline ~25 ms per ARCH-DRAW; PERF4 shares). Brief-only
figures are marked (b). Estimates, not measurements: each stage's gate is its measured number.

| Stage | Moves to workers | Switch | Expected saving (share of draw thread) | Verify |
|---|---|---|---|---|
| S0 measure | nothing: baseline of pr_prepared / pr_not_ready / pr_epoch / pr_ineligible, draws per frame, us/op with SOTC_PARALLEL_RECORD=1 SOTC_PR_RESOLVE=1 on the attract and Shrine scenes | none | none; settles the figures above | n/a |
| S1 predicates (first step) | shader-addressed, special, written and bindless classification per descriptor, stored in PrBuffer / per-stage flags | SOTC_PR_PRED (0 off, 1 use, 2 compute inline and compare) | 5-10% from PERF4 BindBuffers self time; up to 15% (b) | =2: compare with the inline predicates; count differences |
| S2 keys | sampler keys (S#), texture-block keys, table content hashes, all as values in PreparedResources | SOTC_PR_KEYS | 5-8% (TableMemo::Resolve key side and TexBlockKey, PERF2 3.7% self; 8% (b) for TableMemo) | =2: recompute the keys inline, compare |
| S3 render-state descriptors | RT format / extent / clear and hint values (PrepareRenderState, BeginRendering inputs); keep FindImage on the draw thread | SOTC_PR_RT | 3-6% (PrepareRenderState and BeginRendering 3% each (b)) | =2 compare |
| S4 read-mostly lookup on workers | buffer range to buffer id and image lookup done as a read-only probe (shared lock) on the worker; draw thread commits only if a generation counter still matches, else inline | SOTC_PR_LOOKUP | 5-8% (ObtainBuffer 9%, FindImage 4% in PERF4; partial because creation and uploads stay) | =2: run the probe and the real lookup, compare ids |
| S5 compute prep | the same S1/S2 values for DispatchDirect/DispatchIndirect (today none) | SOTC_PR_COMPUTE | unknown: depends on the dispatch share of S0 | =2 |

Cumulative estimate: S1 to S3 remove roughly 15-30% of the draw thread; with S4 roughly 25-40%. From ~25 ms that
gives ~15-19 ms per frame of commit work: at or just under the 16.7 ms budget, not "well under" it. Parallel prep
alone does not reach the target; the remaining per-op commit (ObtainBuffer, FindImage, Transit, descriptor
writes) is what ARCH-DRAW parts 1-4 remove. The two plans are complementary: S1-S4 are useful even when
ARCH-DRAW lands, because the predicates and keys they move are the same ones ARCH-DRAW retires.

Parallel recording of render passes (SOTC_PASS_WORKERS) does not reduce this thread and is not part of this plan.

## Thread-safety
- Prepared values are pure: they may hold V#/T#/S# words, sizes, verdicts, keys and hashes. They must never hold
  an ImageId, BufferId, view handle or sampler handle, nor the answer of ObtainBuffer or FindImage. These are the
  draw thread's to create.
- Inputs read on workers must be safe to read concurrently: `MemoryManager::ClampRangeSize`,
  `IsValidGpuMapping` (already read on prep threads under PR_RESOLVE: confirm they take the memory lock),
  `SotcGen::Sum` (sotc_generation.h), the pipeline's profile and stage info (immutable after creation).
- S4 only: the texture cache's `ReentrantMutex mutex` and `BatchLock` (texture_cache.h:599-643) and the buffer
  cache's mutexes (`prev_mutex`, `granule_mutex`, buffer_cache.h:661, 850) are the locks a read-only probe would
  take. A probe must not touch the LRU or the tick stamps (FindImage stamps `tick_accessed`); those are recorded
  by the probe and applied at commit. Writers (creation, eviction, unmap) bump a generation; a probe result with an
  older generation is discarded. The probe is a read under a shared lock; no new lock order is introduced:
  buffer cache before texture cache, as today.
- Bind cache state, `g_bind_diff` and the TableMemo memo stay draw-thread state.

## Staleness and invalidation
- Guest memory mappings: `runtime_memory_generation()` (odd: change in progress) and `MapEpoch()` gate every
  prepared slot. A map or unmap between prep and draw makes the slot unusable and the draw computes inline. Cost:
  `pr_epoch` counts it. Map-heavy frames can lose the whole benefit. S0 must report the share.
- Per-range generations (SOTC_PR_GEN, `SotcGen::Sum` per range, max span 64 MiB) make the check per range. Use
  them for S1-S3 so an unrelated map does not void a slot.
- Register and flat-buffer snapshot: the prep thread reads the op's captured flat copy and user data, not live
  guest memory. A later guest write to a T# or V# word therefore cannot change a prepared value; it changes the
  next op's capture. Only mappings can change under a prepared value, and the generations above cover them.
- S4 probe results: validated by the cache generation at commit, not by the op's age. A stale probe means a
  full inline lookup, never a wrong binding.
- Pipeline or shader creation between prep and draw: pipelines are resolved on the draw thread
  (`pipeline_cache.GetGraphicsPipeline`) unless `use_resolved`. Prep runs with the pipeline pointer the CP
  resolved; keep that invariant and add nothing that reads the pipeline cache from a worker.

## Verification
- Existing: `SOTC_VERIFY=1` (CheckPrepared, vk_rasterizer.cpp:8554) recomputes dynamic state, push data, fast and
  cache keys, offsets and (with PR_RESOLVE) SamePreparedResources; mismatches are logged (first 16) and counted
  (`pr_verify_mismatches`). Reuse it; do not add a second comparison framework.
- Each new switch follows the same shape: =1 use prepared values only; =2 compute the inline value on the draw
  thread, use it, compare with the prepared one, count and log differences (first 16, as CheckPrepared does).
- Gate for every stage: zero verify differences on the attract demo and the Shrine scene at =2, plus the measured
  saving in the profile (SOTC_OPSTAT / SOTC_OP_PHASES phase counters, SOTC_PROFILE on GpuCommandProcessor and the
  draw thread) and the pr_* ratio (prepared / draws) not lower than before.
- Images: the rendered output must match with the switch off and on (same frame hashes or screenshots at fixed
  frame numbers), since every stage claims identical results.

## Risks
- Prep is off by default and needs 7+ hardware threads. Hardware with fewer threads gets no benefit at all. Hosts
  below that threshold need a different answer (ARCH-DRAW).
- Thread contention: prep workers compete with the CP, the draw thread, GpuRecorder and the game's own threads.
  Measured throughput on 8-core hosts may not hold on 6-core or 16-core ones. Measure on the three vendor
  machines, not only one.
- Map-heavy scenes void slots (pr_epoch). The per-range generation (S1-S3) is the mitigation; its cost (a sum per
  descriptor range) is itself counted in S0.
- Staleness bugs are silent. Verify mode is the only detector; run it on every stage before turning it on by
  default.
- The brief's percentages are not in the repo. If S0 shows BindBuffers and BindTextures far below PERF4, the
  stage savings above shrink in proportion.
- Memory: prepared slots are 8 KiB each (PreparedResources, static_assert at draw_prep.h); the ring holds
  BackRingSize of them. S1-S3 must keep the slot under the current bound or raise it with the assert updated.
- Shader cache: S1-S3 change no shader output, so no cache rebuild.

## First step (small, shippable, behind a switch): S1 predicates on the prep thread
Goal: move the per-descriptor predicates of BindBuffers from the draw thread to PrepareResources, with
`SOTC_PR_PRED` (0 default, 1 use, 2 verify). No change to what is bound.

Functions and types touched:
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`
  - `PrepareResources` (8507): fill the new predicate fields for each buffer (only when PR_RESOLVE is on, as the
    buffers are prepared only then).
  - `PrResolveBuffer` (2981): compute `shader_addressed` from `Shader::ShaderAddrBuffer(profile, stage, desc)`.
    The profile is `pipeline->GetProfile()`; the draw thread gets it from `SotcArch::ShaderAddr::BindProfile()`
    (set at BindResources, 3208). The prep thread must pass the pipeline's profile explicitly, not the
    thread-local one.
  - `SamePreparedResources` (3049): compare the new fields (used by SOTC_PR_RESOLVE verify and PR_GEN=2).
  - `BindResources` (3644): choose the prepared stage (`pr_stage`) as today; no change in logic.
  - `BindBuffers` (5881): the lambda `shader_addressed` (5995) reads the prepared field when `pr_stage` is set and
    the switch is 1; =2 keeps the lambda and compares. Call sites: 6107, 6161, 6208, 6420, 6482 use the lambda,
    so they change by one place.
- `gpu/shadps4/video_core/amdgpu/draw_prep.h`: add `bool shader_addressed{}` to `PrBuffer`, with a comment; keep
  `static_assert(sizeof(PreparedResources) <= 8192)`.
- A new read-once switch next to `SotcPrGenMode` (vk_rasterizer.cpp:2934), same pattern.

Not touched: liverpool.cpp (PrepLoop, TakePrepared), BufferCache, TextureCache, the recorder, the bind cache.

Gate for the first step:
- SOTC_PARALLEL_RECORD=1 SOTC_PR_RESOLVE=1 SOTC_PR_PRED=2: zero "PR predicate mismatch" lines over the attract and
  Shrine scenes; the existing verify counters unchanged.
- SOTC_PR_PRED=1 vs 0: identical frame output on the same scene and frame numbers.
- Profile: BindBuffers self time and the `shader_addressed` share drop (SOTC_PROFILE, tools/profile_functions.py).
  If the drop is under 2% of the draw thread, S1 is not worth its complexity and the plan moves to S2.

Syntax check per the repo rules (clang++ -fsyntax-only on vk_rasterizer.cpp with the out/gpu compile flags). No
code is written by this document.
