# Final design: a stutter-free draw path (30 FPS locked, built for 60)

Status: design, 1 Nov 2026. Supersedes the phase plan of DRAW-THREAD-DESIGN.md section 5 where they differ.
Inputs: DRAW-THREAD-DESIGN.md (cost map), the wave-1 results (HANDOFF.md, "In-game results of the draw-thread
phases"), the trace session user/trace/20261101-085933 (106 slow frames: 86% draw thread, 12% game), and the
wave-3 specialist reports (docs/re-stutters/reports3/draw/wave3/, numbers to be folded in as they land).

## 0. Decisions from the user (1 Nov)

| Question | Answer | Consequence |
|---|---|---|
| Target | 30 FPS locked, prepared for 60 | Worst-case draw work <= 25 ms per frame at 30; the structure must reach <= 12 ms (60) by adding threads, not by redesigning again |
| Scope | Refactor in stages | shadPS4 stays the base; every stage lands behind a switch, is verified, then becomes the only path |
| GPUs | NVIDIA, AMD, Intel Arc | Only features all three ship on current Windows drivers (section 7); everything else has a fallback |
| Accepted costs | Shader cache rebuild, newer driver, divergence from shadPS4 | Recompiler changes (BDA, bindless) are allowed; Vulkan 1.3 baseline |
| Not accepted | More RAM/VRAM; added input latency | No CPU-ahead frame (SOTC_FRAME_AHEAD is dropped); no duplicated big buffers; bursts must be absorbed inside the frame |
| Game patches | Yes; and clean up: delete switches that are not default or proved useless, clean code, maximum performance | Eboot patches may cut work at the source; the switch audit (wave3/switch-audit.md) drives the deletions |

## 1. Where the time goes today (measured)

- Steady state: draw work 30-34 ms per frame at 28-29 FPS; GPU 37% busy. The draw thread IS the frame.
- Spikes: (a) dispatch-heavy frames, +30 ms of op cost; (b) readbacks serviced on the draw thread, +8..17 ms;
  (c) area loads, 139 MiB re-uploaded in one frame, 150-215 ms; (d) 12% game-side (event-flag chains, a 31 ms bake
  job, file IO).
- Per op: ~5 us per direct dispatch, ~2.5 us per indirect draw; ~4,000-6,500 ops per frame. Half of an op's cost is
  resource binding (buffers, textures), then render passes/targets, page-bitmap scans, re-protects, memcpy.
- What already works (0 verify mismatches in game): ring8 host arena (default), generation memo for buffer binds
  (85% hits) and textures (93% hits), pipeline key skips (77%), off-thread frame choice, pooled images.
- What failed: honest labels as written (14-30% of labels deferred, 8 FPS), heap serving by 64 KiB granules,
  CPU_ONLY publishing (cannot cut faults by design), frame-ahead (latency, rejected).

Floor arithmetic for 30 locked: 33.3 ms budget minus present/flip and margin -> <= 25 ms worst-case draw work. With
6,500 ops in a burst frame that is <= 3.8 us per op on one thread, or the same work over several threads. For 60:
<= 12 ms, so either ~1.8 us per op single-threaded (not credible with Vulkan emission alone at ~0.3-0.5 us) or the
work split across recorder threads. Hence the two halves of the design: make an op cheap (stages B-E), then make ops
parallel (stage F).

## 2. Principles

1. Every change tells; no bind asks. Validity is a generation number compared on the hot path, never a page scan.
2. Memory lives by class, decided at allocation, and the class decides the mechanism (host-served, uploaded once,
   GPU-private, GPU-to-CPU). Page protection is a safety net for the UNKNOWN class only, and that class shrinks to
   nearly nothing.
3. The draw thread records; it never waits on readbacks, never copies bulk memory, never creates resources in the
   steady state.
4. One code path per mechanism. A switch exists only while its stage is being verified; after the gate the old path
   is deleted.
5. Correctness is checked by construction (verify modes, SOTC_STALE_CHECK) before speed is measured.

## 3. Memory by class (stage B)

| Class | Content | Mechanism | Built on |
|---|---|---|---|
| HOST_SHARED | per-frame arenas, GPU-read CPU-written heaps (CPUMemory granules the GPU binds, constants, skinning/instance data) | guest pages imported as host memory, GPU reads in place; no tracking | ring8 (default today) |
| STATIC | meshes and textures from the resource loader | uploaded once at load completion, then generation-validated; write protection only as a safety net that bumps the generation | static class, loader hooks, generation API |
| GPU_PRIVATE | render targets, GPU-only scratch | VRAM only, never downloaded, never tracked | RT class |
| GPU_TO_CPU | XPR visibility masks, particle stats, labels the CPU polls | GPU writes into imported host pages; the guarding label is written when the GPU is done | rb_host, label machinery |
| UNKNOWN | everything not classified | today's page tracker | memory_tracker |

How HOST_SHARED reaches the heaps without the per-granule cost that failed: placement, not classification. The
native pool allocator (SOTC_PC_POOL_NATIVE: 6,454/6,454 calls native and verified) already executes the game's
allocator. Stage B gives each GPU-read heap its own VA window (the placement glue's windows), so a heap is one or a
few large ranges, imported once at boot. No memtype probe, no class tracking, no ring re-application, no extra
memory (the ranges are the game's own pages). Granule classification remains only as a verifier.

Labels, redesigned (replaces SOTC_LABELS_HONEST): defer only the labels that guard a CPU read of GPU_TO_CPU
memory. Those labels are known: the XPR "CS done" label (hook XPR_CS_DONE_LABEL), the particle-stats fences, and
any label the game polls before reading a result range (learned once from the CP's wait-register list and the
GPU_TO_CPU registry). Every other label keeps today's early write. Expected deferral rate: well under 1%.

## 4. Readbacks (stage C)

1. Eliminate at the source with eboot patches where the game reads GPU results synchronously but could use the
   previous frame's values (occlusion masks, particle counts). This is not input latency: it is one frame of
   visibility-data lag, which the game already tolerates in its own double-buffered paths. Each patch behind a switch
   with a verify mode (compare this frame's and last frame's values; count frames where they differ visibly).
2. What remains: GPU writes into host memory (GPU_TO_CPU class) with the guarding label deferred (section 3).
3. Residual readbacks (UNKNOWN class): serviced by a readback thread with its own command buffer and a timeline
   wait on the writer's tick (wave3/rbthread), never by the draw thread.

## 5. The bind path (stage D)

- Buffers: the generation memo becomes the only path for read binds (no SyncFastSlot / SyncGeneration / bind memo
  in the rasterizer: one memo, keyed by (address, size), validated by SotcGen::Sum and BindVersion). Written binds
  bump generations once per range per submit, not per bind.
- Textures: the identity memo becomes FindImage's front door; its hit path stops taking the texture-cache mutex
  (read-mostly snapshot of the image table, rebuilt by the owner) and its epoch becomes per address, not global.
- Descriptors: per-pipeline-layout descriptor diffing (push only the slots that changed since the previous op of the
  same layout), then a bindless sampled-image heap with persistent slots (the descriptor-heap half of D1).
- Render passes: one BeginRendering per render-target set per pass, not per draw (wave3/opcost).
- Every hot-path env read, trace/stat branch and per-op heap allocation goes (opcost).

Target: <= 2 us per op on one thread for the common hit case.

## 6. Loads and resources (stage E)

- Uploads: the memcpy into staging runs on upload workers; the draw thread only records the copy (or a transfer
  queue copy the draw submit waits on with a timeline semaphore). STATIC uploads start at loader completion, budgeted
  per frame, so no frame pays a load (wave3/loadburst).
- Images: created ahead on the prefetch workers from the area's recorded list (area prewarm), pooled, render targets
  at their recorded maximum extent. No extra VRAM: the pool replaces dedicated allocations, it does not add to them.

## 7. Execution model and the road to 60 (stage F)

Today: CP thread parses; one draw thread resolves and records; a recorder submits. For 60 the resolve+record work
must spread over threads without changing GPU order:

1. The CP emits ops with a sequence number and a footprint (OpFootprint exists: buffers, images, render targets).
2. Resolver workers (N = cores/4) turn ops into packets: pipeline, descriptor deltas, buffer/image handles, all
   read-only against generation snapshots (the W1 machinery, made cheap: it cost ~3 ms because it rebuilt mapping
   checks; with generations validated per packet it becomes a sum compare).
3. The owner thread only handles misses (creation, uploads, transitions) and pass boundaries.
4. Recording splits by render pass: each pass's packets go to a secondary command buffer recorded on a recorder
   thread; the owner stitches them in sequence order into the primary. Barriers between passes are computed from
   footprints by the owner, so recording threads never decide synchronization.
5. Submit as today (timeline semaphore per submit).

The order guarantee: GPU order = sequence order, because secondaries are executed in sequence order and all
cross-pass hazards are resolved by owner-inserted barriers. Within a pass, ops are recorded by one thread in order.

## 8. Vulkan feature matrix (NVIDIA, AMD, Intel Arc, Windows, current drivers)

| Feature | Use | Fallback |
|---|---|---|
| Vulkan 1.3 core (dynamic rendering, sync2, timeline semaphores) | baseline | none (required) |
| VK_EXT_external_memory_host | HOST_SHARED, GPU_TO_CPU | upload path (today's) per range |
| Buffer device address + int64 | BDA block table (60 FPS stage) | descriptors |
| Descriptor indexing (sampled images) | bindless heap | push descriptors |
| VK_EXT_graphics_pipeline_library | cold-miss fast link | async compile, draw skipped |
| VK_KHR_push_descriptor | today's descriptors | descriptor sets |

Each feature is probed at device creation; a missing one turns its stage's fallback on for the session and logs it.
Intel Arc is validated on every stage that adds a feature before that stage's gate.

## 9. Clean-up (stage A, runs first and with every stage)

- The switch audit (wave3/switch-audit.md) classifies every switch. Deleted: everything measured without gain or
  broken, superseded ring modes, CPU_ONLY publishing, granule heap serving, honest labels v1, frame-ahead, recycle,
  texture protect batch, placement stage tests that never shipped, unused diagnostics. Kept: defaults (made
  unconditional, the switch removed once the default is proven), user options in the launcher, the measurement
  tools (SOTC_FPS, SOTC_TRACE, SOTC_DRAW_WORK, verify modes while a stage is open).
- Hot paths read no environment; switches are read once into a settings struct at startup.
- Each stage ends by deleting its old path.

## 10. Stages, gates and order

| Stage | Content | Gate (Lab route, users' culling cvars, one round per config, plus the user's visual check) |
|---|---|---|
| A | Clean-up from the audit; proven defaults made unconditional | build clean; default config within noise of today; no visual change |
| B | Memory by class: heap placement windows + import; STATIC upload-once; labels v2 | write faults < 500/s; labels deferred < 1%; 0 stale pages (SOTC_STALE_CHECK); no visual change |
| C | Readbacks: eboot patches, GPU_TO_CPU, readback thread | waited readbacks < 50/s; draw-thread readback time 0 |
| D | Bind path as the only path; descriptor diffing; render-pass merging; hot-path hygiene | draw work <= 20 ms/frame steady; per-op cost <= 2 us |
| E | Loads: worker uploads, loader-time STATIC uploads, image pre-creation | no frame over 50 ms on area loads |
| F | Resolver workers + per-pass parallel recording | draw work on the owner <= 12 ms/frame; 60 FPS mode holds on the Lab route |

Gate for 30 locked = stages A-E: p99 <= 36 ms and max < 50 ms over the Lab route and a 20-minute free-play
session, with no added latency (no CPU-ahead frame).

## 11. Open questions for later stages

- Which eboot readback sites can take last-frame values without visible artefacts (stage C needs a per-site check).
- Whether the native pool allocator can carve GPU-read heaps into windows without changing the game's address
  space assumptions (stage B, verify mode first).
- Recorder count and pass granularity for stage F (measure secondaries' overhead on Arc).
