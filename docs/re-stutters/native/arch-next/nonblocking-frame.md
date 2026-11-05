# A frame that never blocks on secondary work (branch `arch/nonblocking-frame`)

Plan approved on 4-5 Nov 2026. Scope: the command processor (CP), the draw thread and the recorder do no secondary work
inline (file I/O, compiles, translations, program loads, cache saves). Not in scope: the PS4 ordering waits
(WAIT_REG_MEM label spins, `WaitBackWrites`, EOP labels, readbacks, flip and frame-slot waits). A missing pipeline is
drawn with a fast-linked GPL pipeline where the device has GPL with fast linking, and skipped elsewhere. The floor is
Vulkan 1.3; everything above it is feature-detected.

Phases: P0 block ledger, P1 secondary work off the frame threads, P2 job scheduler with classes, P3 never-wait
pipelines with GPL (per-shader libraries), P4 libraries ahead of the draw, P5 persistent cache without the full-blob
rewrite. Each phase is behind a switch and has a measurement gate.

## P0. Block ledger (`SOTC_BLOCK_LEDGER=1`)

`gpu/sotc/block_ledger.{h,cpp}`. Scoped timers at the inline sites, attributed to the calling thread's role
(`SotcStats::tl_role`): inline graphics and compute pipeline creation (`GetGraphicsPipeline`, `GetComputePipeline`),
inline translation (`CompileModule` without worker pools), `LoadLazyProgram`, `RegisterShaderBinary`,
`Storage::DataBase::Save` and `Load`, `SaveShaderIndex`, `SaveDriverCache`, `GplLibraryCache` build and lock wait,
and the recorder's compute pipeline wait (`ComputePipeline::BindWhenReady`). Output: a `SOTC BLOCK LEDGER` line every
5 s (`driver.cpp`) and the frame's share appended to each `SOTCSTALL` line (`vk_presenter.cpp`).

### Baseline (Lab test route, one round each; `user/nb0/nb0_cold.log` salt 71, then `nb0_warm.log` same salt)

Switches: `SOTC_PIPE_FEEDBACK=1 SOTC_PRETRANSLATE_COMPUTE=1 SOTC_VS_DRAW_KIND=1 SOTC_PIPE_PREDICT=0`, plus the user's
sotc.ini (`SOTC_PRETRANSLATE=1`).

| | cold | warm |
|---|---|---|
| boot preparation ("shaders prepared") | 78.8 s | - |
| route p50 / p99 / max / frames over 100 ms | 33.4 / 226.0 / 458.5 ms / 6 | 33.4 / 67.5 / 101.0 ms / 1 |

Inline secondary work on the frame threads, summed over the 5 s windows of the cold run:

| site (thread) | ms | calls | note |
|---|---|---|---|
| driver cache save (CP) | 17,596 | 1 | at the end of the boot hold (`MaybeHoldForPretranslate`); one call |
| graphics pipeline inline (CP) | 2,327 | 30 | tiny draws and other non-deferred builds, ~78 ms each |
| compute wait (recorder) | 1,260 | 10 | compute pipelines a worker had not finished |
| pack load (CP) | 549 | 1,726 | `LoadLazyProgram`, first draw of each program from the pretranslate pack |
| translate inline (CP) | 260 | 24 | |
| storage save (CP) | 230 | 8,954 | per-call copy and queueing |
| compute pipeline inline (CP) | 105 | 15 | |
| driver cache save (other threads) | 1,366 | 2 | the 60 s saver |

Warm: at most 2 ms per frame on the CP (pack loads 0.2-1.3 ms per frame, compute pipeline objects 0.1-0.3 ms), the
recorder at most 0.2 ms per frame, and the 60 s saver 906 ms on its own thread.

Reading: the largest single block of the first session is the driver cache save on the CP right after the boot hold
(17.6 s: the hold screen stays up that much longer, and the first frames after it are slow). Then the inline pipeline
builds (2.3 s in 30 calls), the recorder waiting for compute pipelines (1.3 s), and the pack loads (0.5 s in 1,726
calls). P1 moves the save, the pack loads and the storage copies; P2 and P3 the pipeline and compute waits.

## P1. Secondary work off the frame threads (`SOTC_NB_IO=1`)

Changes:
1. The driver cache save at the end of the boot hold wakes the saver thread (`RequestDriverCacheSave`,
   `driver_cache_cv`) instead of running on the CP. The ledger splits `vkGetPipelineCacheData` ("driver cache get")
   from the total: the get is nearly all of it (2,353 of 2,353 ms in `user/nb1/nb1_cold.log`).
2. `Storage` saves move the blob into the IO request instead of copying it on the caller (always on).
3. Pack prefetch: a pretranslate request of a program that is still in the pack (`lazy_programs`) queues a pack load
   job (`QueueLazyPrefetch`): a worker reads the meta and SPIR-V from the pack (`g_pack_mutex` guards the file) and
   creates the modules (`RunLazyLoad`); the CP installs them (`InstallLazyLoad`, `InstallLazyPerm`, shared with
   `LoadLazyProgram`). A draw that comes first still loads inline, and the worker's permutations already there are
   destroyed. At most 2 pack loads run at a time (`FeedLazyPrefetch`).
4. No loose copies of pack stages any more (neither the prefetch nor `LoadLazyProgram`): the warm-up reads a stage
   from the pack when its loose meta or SPIR-V is missing (`PackStageMeta`, `PackStageSpv`, `g_pack_by_stage`; the
   pack is opened before the warm-up), and `SlotHasCurrentMeta` counts a pack slot as taken.

First attempt (`user/nb1/nb1_cold.log`, `nb1_warm.log`, salt 72): the prefetch had no limit and wrote loose copies.
The warm run loaded 11,465 programs and wrote 27,960 loose files; the workers were taken and the recorder waited for
compute pipelines (236 ms per window instead of 20). Warm route p50 49.1 ms, p99 117.8 ms: gate failed. Fixed by
items 3 (limit 2) and 4 (no loose copies).

Gate run (`user/nb1/nb1b_cold.log`, `nb1b_warm.log`, salt 73, one round each):

| | P0 cold | P1 cold | P0 warm | P1 warm |
|---|---|---|---|---|
| boot preparation | 78.8 s | 59.5 s | - | - |
| driver cache save on the CP | 17,596 ms | 0 | 0 | 0 |
| route p50 / p99 / max | 33.4 / 226.0 / 458.5 ms | 33.3 / 107.1 / 276.5 ms | 33.4 / 67.5 / 101.0 ms | 33.3 / 50.3 / 66.4 ms |
| frames over 50 / 100 ms | 25 / 6 | 11 / 3 | 12 / 1 | 4 / 0 |
| CP inline work per frame (warm) | - | - | up to 1.8 ms | up to 0.2 ms |

Warm prefetch: 16,661 programs queued over the session (the game announces every program it loads), 3,352
permutations installed before their first draw, 2,257 loaded by a draw first; the backlog drains 2 at a time.
Still on the CP in the cold run: inline graphics pipelines (1,908 ms in 34 calls), pack loads of programs the boot
hold translated (223 ms), and the recorder's compute waits (885 ms): P2 and P3.

## P2. Job classes: measured first, rewrite not done

`SOTC JOB QUEUE` line (with `SOTC_BLOCK_LEDGER=1`, `vk_pipeline_cache.cpp` `NoteJob` / `ReportJobs`): per job class
the wait between the job being made and a worker taking it, and its run time. Cold run with P1 on
(`user/nb2/nb2q_cold.log`, salt 74), the windows after the boot hold:

| class | wait avg / max | run avg |
|---|---|---|
| compute | 0-14 / 31 ms | 19-217 ms |
| translate | 24 / 37 ms | 8 ms |
| shadow (depth-only graphics) | 6-51 / 208 ms | 31 ms |
| graphics | 531-884 / 1,871 ms | 148-153 ms |

The recorder's compute waits are build time, not queue time (compute jobs are already first and wait about 10 ms).
Graphics jobs wait 0.5-0.9 s because each build takes ~150 ms and they come in bursts; reordering classes does not add
throughput. The class rewrite of the plan is therefore not done (the measurement stays); the graphics wait is P3's
target (a link of ~0.006 ms on libraries built once per shader instead of a ~150 ms build per pipeline).

## P3.0 step 1: per-stage descriptor sets still work and cost the same

Per-shader GPL libraries need a descriptor layout that depends on one stage only, i.e. one set per stage
(`SOTC_STAGE_BINDINGS=1`, `StageDescriptorSet`, bindings numbered from 0 in each set; active when
`maxBoundDescriptorSets` >= 5, 32 here). That mode gives up push descriptors (`uses_push_descriptors` is false with
`stage_sets`), so its draw-thread cost had to be measured before building on it. Runs with P1 on
(`user/nb3/sb_cold.log` salt 75, `sb_warm.log` same salt) against `user/nb1/nb1b_warm.log`:

| warm route | push descriptors (P1) | per-stage sets |
|---|---|---|
| p50 / p99 / max | 33.3 / 50.3 / 66.4 ms | 33.3 / 50.4 / 84.3 ms |
| bind time on the draw thread (SOTC DRAW SPLIT) | 15.7-15.9 ms per frame | 15.3-16.7 ms per frame |
| draw thread CPU per frame | 30.1-31.6 ms | 30.2-33.7 ms |
| error lines | 40 (the known pretranslate export failures) | 40 (the same) |

Same cost within one round. Not checked yet: the image (a Lab session with the switch on, by the user).

Step 2 (to build): per-shader libraries. The pre-rasterization library is keyed by the VS module, a layout holding
only the VS set (created with `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`), the topology class and the static
raster state; the fragment library by the PS module, a layout holding only the PS set, and the multisample and
depth/stencil state, without the attachment formats (the spec has fragment shader state ignore them). Vertex input
and fragment output libraries carry no layout. The linked pipeline and the optimized one use the full per-stage layout
with the same flag, so descriptor sets bind as today.

### P2 rewrite (`SOTC_NB_SCHED=1`, asked for by the user after the measurement)

The single deque with front-insert counters is replaced by job lanes (`JobQueues`, `PushJobLocked`, `PopJobLocked`,
`HasRunnableJobLocked`): Compute, Translate, Tiny, Shadow, Graphics, Background, highest first. Background holds the
pretranslate jobs, pack loads, compute prebuilds outside the boot hold and predicted pipelines. With the switch:
graphics jobs are taken by demand (each draw skipped for a queued key adds 1, worth 10 ms of age) plus age; a job
whose pipeline the command processor built itself is cancelled (`queued_graphics`, `cancelled`); background jobs run
on at most half the workers at below-normal thread priority (all workers during the boot hold). Without the switch the
lanes keep today's order, FIFO, except that background jobs no longer sit ahead of graphics jobs queued after them.

A/B, one round each (`user/nb2/nb2s_*.log` salt 76 against `user/nb1/nb1b_*.log` salt 73, both with P1):

| | P1 cold | P1+P2 cold | P1 warm | P1+P2 warm |
|---|---|---|---|---|
| route p50 / p99 / max | 33.3 / 107.1 / 276.5 ms | 33.4 / 184.8 / 350.2 ms | 33.3 / 50.3 / 66.4 ms | 33.3 / 50.1 / 59.0 ms |
| frames over 50 / 100 ms | 11 / 3 | 10 / 4 | 4 / 0 | 3 / 0 |
| graphics job wait, first two gameplay windows | 531 / 884 ms avg | 281 / 627 ms avg | - | - |

Graphics jobs wait 30-47% less on average (demand order: 1,315 and 3,640 skipped-draw points in those windows), the
warm run is the same or better, the cold route p99 and max are higher in this one round. Not decided by one round:
it stays off by default.

## P3 step 2: per-shader GPL libraries (`SOTC_NB_GPL=1` with `SOTC_PIPE_GPL=1 SOTC_STAGE_BINDINGS=1`)

Built: graphics layouts with independent sets and the two per-stage library layouts (`LayoutCache` `layout_flags`,
`null_set`; `GraphicsPipeline::gpl_vs_layout`, `gpl_fs_layout`); library keys per shader, the fragment part without
attachment formats; on the command processor `TryFastLink` only looks libraries up
(`GplLibraryCache::SetLookupOnly`), a miss queues a worker link job (`QueueLinkJob`, translate lane) that builds the
missing libraries and links; the optimized build still replaces the linked pipeline (`CollectAsyncPipelines`).

First run (`user/nb3/gpl_cold.log`, salt 77): `GplLibraryCache::GetOrBuild` held its mutex while a worker built a
library, so the command processor's lookups waited for whole builds ("gpl lock" 14.7 s in one 5 s window; cold route
p99 925.6 ms, max 2,565 ms). Fixed: the lock covers the map only, the build runs outside it, a duplicate is dropped.

After the fix, one round each (`gpl2_cold.log` salt 78, `gpl2_warm.log`), against per-stage sets without GPL
(`sb_*.log`):

| | sets, cold | sets + GPL, cold | sets, warm | sets + GPL, warm |
|---|---|---|---|---|
| route p50 / p99 / max | 33.4 / 266.2 / 459.6 ms | 33.4 / 237.1 / 441.5 ms | 33.3 / 50.4 / 84.3 ms | 33.3 / 50.9 / 58.9 ms |
| frames over 50 / 100 ms | 12 / 5 | 25 / 8 | 3 / 0 | 3 / 0 |
| GPL: draws served by a link, links, failures | - | 334, 119, 0 | - | 2, 22, 0 |
| libraries built / reused (pre-raster, fragment) | - | 71 / 446, 108 / 58 | - | 19 / 16, 20 / 2 |
| gpl lock wait on the CP | - | 0.0-0.7 ms per 5 s | - | - |

No device lost in either run. The command processor still builds 30 pipelines inline in the cold run (2.1 s, the tiny
full-screen draws, which are never skipped), the largest item left in the ledger. Not checked: validation layers (not
installed on this machine) and the image.

### P3 validation and the tiny draws

Validation layers (`mingw-w64-clang-x86_64-vulkan-validation-layers` 1.4.363, `BB_VK_VALIDATION=1`,
`VK_LAYER_PATH=C:/msys64/clang64/bin`):
- warm run (`user/nb3/gpl_val.log`): 7 links; the only message is VUID-StandaloneSpirv-OpEntryPoint-08721 on
  `vkCreateShaderModule` (two inputs on one location component, from the recompiler's SPIR-V, not related to GPL);
- cold run without pretranslation, so every pipeline goes through the library path (`gpl_val2.log`, salt 79):
  79 links, 117 libraries built with independent-set layouts, 0 link failures, no validation message at all.

Tiny full-screen draws (never skipped) now build only their missing libraries on the command processor and link
(`TryFastLink(slot, true)`, the `SOTC_TINY_GPL` path, on with `SOTC_NB_GPL`), the optimized build follows on a worker.
Run `gpl3_cold.log` / `gpl3_warm.log` (salt 80) against `gpl2_*`:

| | gpl2 cold | gpl3 cold | gpl2 warm | gpl3 warm |
|---|---|---|---|---|
| route p50 / p99 / max | 33.4 / 237.1 / 441.5 ms | 33.5 / 242.5 / 349.5 ms | 33.3 / 50.9 / 58.9 ms | 33.3 / 50.4 / 58.1 ms |
| CP inline: pipelines / gpl libraries | 2,079 ms (30) / - | 10 ms (1) / 1,960 ms (60) | - | - |

The inline cost moved from whole pipelines to libraries but hardly shrank: these draws' libraries take 22-53 ms each
(not the 17 ms average), and the full-screen passes use many distinct pixel shaders. What is left for the first
session is building those libraries before the draw: P4.

## P4. Libraries ahead of the draw (`SOTC_NB_WARM=1`): gate failed, off

Built: every new vertex or pixel permutation (`Program::AddPermut` / `InsertPermut` log it once the boot hold's
eviction is past) gets a warm-up job in the background lane: a link job on the last drawn VS+PS pipeline with the
new shader in its stage, so the worker builds just that shader's library; the linked pipeline is dropped
(`FeedWarmLibraries`, `NoteWarmTemplate`).

Cold run with P1+P2+P3 on (`user/nb4/gpl4_cold.log`, salt 81) against P3 without it (`user/nb3/gpl3_cold.log`):

| | P3 | P3 + warm-up |
|---|---|---|
| route p50 / p99 / max | 33.5 / 242.5 / 349.5 ms | 41.8 / 290.1 / 423.1 ms |
| frames over 50 / 100 ms | 26 / 7 | 52 / 14 |
| graphics job wait, third gameplay window | 0.3-0.9 s | 2.96 s avg |

The warm-up built 2,167 libraries (27 s of worker time) on half the workers; the graphics jobs the frame needed
waited longer, and the frame threads got less CPU (the same pattern as the material prediction of 3c). It also came
too late for the first full-screen passes, which appear right after the boot hold: the command processor still
built ~1.6 s of libraries inline. Kept behind its own switch, off; `SOTC_NB_GPL` alone is P3 as measured.

## P5. The driver cache file

P5.0, two warm sessions on the same port cache (salt 73, P1 on): `user/nb5/dc_off.log` (`SOTC_DRIVER_CACHE=0`) and
`dc_on.log` (default):

| warm session | app file off | app file on |
|---|---|---|
| compute / graphics driver time | 14.8 / 2.9 s | 23.5 / 5.0 s |
| graphics driver cache hits | 287 | 286 |
| route p50 / p99 / max | 33.3 / 42.4 / 58.2 ms | 33.3 / 34.2 / 57.9 ms |
| session length | 26 s | 50 s (1.8 GB read at start, 1.9 GB written) |
| save on the saver thread | - | 25.1 s (vkGetPipelineCacheData 0.6 s, the file write 24.5 s) |

On NVIDIA the driver's own disk cache served the pipelines without the app file (it survived a rebuild of
`bb-probe.exe` at the same path: `dc_off` ran right after one). The port keeps the file because, per the comment at
its load, "the driver's own cache is tied to the executable": a release in a new folder was not measured, so the
file stays on.

Done: the periodic saves rewrite the file only when it grew by 5% or 64 MiB (`SaveDriverCache(final_save)`; the save
at shutdown writes any growth; `SOTC_DRIVER_CACHE_ALWAYS=1` keeps the old rule). Warm session `dc_on2.log`: no rewrite
(0 ms), session 23 s. Open: turning the file off by default on the NVIDIA driver needs the new-folder release case
measured first.
