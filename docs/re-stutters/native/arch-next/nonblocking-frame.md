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
