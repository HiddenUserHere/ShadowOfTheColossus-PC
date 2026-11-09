# Cold compile: CPU and RAM at the first loading screen (9 Nov 2026)

Read-only analysis. Nothing was built or run. Line numbers are those of the working tree on branch
`arch/nonblocking-frame` on 9 Nov 2026; `vk_pipeline_cache.cpp` was being edited by another agent at the time, so
its numbers may shift by a few lines.

Complaint: during the first (cold) loading screen the CPU sits at 99% and process RAM goes past 8 GB (the user sees
about 3 GB during normal play). Goal: compile with less CPU, write results to disk, and drop them from RAM. A longer
first loading screen is fine. Stalls during play are not, so the GPL stage libraries of every manifest pipeline and
all ~4,253 compute programs (int/float variants included) must still be compiled at the first loading screen.

## 0. What the log shows (`user/prof/m0/j_heap_tex_reg2.out`, Ryzen 7 5700X 8C/16T, RTX 3060 Ti, driver 616.56)

| Log line | Event | Process private (`PrivateUsage`) |
|---|---|---|
| 46-48 | live driver cache **368 MB** and pack cache **743 MB** loaded; 12 workers (2 urgent-only) | 4,705 MB (sampled while both file buffers are still alive, see 2.1) |
| 370 | pretranslate hold starts (1,822 translations queued, 4,469 by the end) | |
| 1371, 1470 | live cache saved: 495 MB, then 577 MB | |
| 1440 | pack step: 0 jobs queued (`no valid list`), 16 workers (4 extra) | **9,510 MB** (-196 MB over the step, so ~9.7 GB before it) |
| 1443 | live cache **563 MB > SOTC_DRIVER_CACHE_MAX_MB=512**: the next launch moves it aside and starts empty | |
| 1445 | `shaders prepared in 297.6 s (4469 translations)` | |
| 1466 | manifest hold: 4,741 pipelines (5,911 stages) | |
| 2804 | 3,006 pipelines built at 66 s; the next ~40 s build only libraries | |
| 2847 | manifest hold end: 4,757 libraries held | **11,955 MB** (+1,126 MB: pipelines+translations 864, libraries 263) |
| 2865 | `loading screen: hidden after 407.1 s` | |
| 3109 | `COMPUTE SPECFREE ... prebuilt programs 4253 (variants 4362), compile at load 2957.2 s` | |
| 3098 | `PRETRANSLATE compute: ... built 4362 ... first dispatches 77` (409 by line 3314) | |

Notes:
- This launch was not fully cold. The driver caches were loaded, but the SPIR-V was new (SPECFREE variants), so
  every compute pipeline missed.
- 2,957 s of compile time over ~300 s of hold means about 10 workers busy all the time. That is **680 ms per compute
  variant**, against 146 ms per compute pipeline in `user/prof/gcn/cpack_cold.out:4739` (3,800 synthetic perm-0
  programs, 555 s). SPECFREE programs are bigger, and two workers share each physical core (16 compile threads on
  8 cores), which makes each compile slower.
- **The NVIDIA driver's own disk cache already persists these pipelines.** `k_heap_only_warm.out` is the launch
  after one that moved the oversize live cache aside, so it loaded only 83 MB of live cache, built from 1,082
  compute pipelines. It still built all 4,362 variants in **94.4 s** of worker time (line 2903; 22 ms each, against
  680 ms cold), and `ASYNC COMPUTE built 4986 (25652 ms on workers)` (line 2742). About 3,300 of those pipelines
  were not in any blob the port loaded, so the driver's internal cache served them. The live cache then grew back
  from 83 MB to 265 MB (line 1340): the driver copies its own hits into the app's VkPipelineCache, so the same data
  sits in RAM a second time.
- `PrivateUsage` is commit, not what Task Manager shows. It includes committed guest memory and the fully committed
  guest stacks. The user's "~3 GB normal" is most likely the private working set, so the two figures cannot be
  compared directly. `SOTC_RAM_REPORT=1` (`gpu/sotc/ram_report.cpp:1-40`) prints both, plus a "remainder" (driver)
  line.

## 1. Threads that run during the loading screen

### 1.1 Pool sizes and priorities (file:line)

| Pool | Size on the 5700X | Priority | Where |
|---|---|---|---|
| Pipeline workers (`shadPS4:PipelineWorker` / `PipelineUrgent`) | `clamp(hw - 4, 2, 12)` = **12**, of which `SOTC_GPL_URGENT_WORKERS` = 2 serve only the Urgent/Predict lanes | Normal. `SOTC_WORKER_LOW_PRIO=1` sets all to Low (off by default, 27 Oct review) | `vk_pipeline_cache.cpp:898-918`, `:2574-2591`; urgent default `nonblocking.h:232-246` |
| Per-job priority change | Capped lanes (Predict, Shadow, Graphics, Background) run BELOW_NORMAL when `SOTC_NB_WORKER_CAP=1` (set in the user's `sotc.ini`). The Background lane does too with `SOTC_NB_SCHED`. **Compute and Translate jobs (the whole pretranslate hold) stay Normal.** | | `vk_pipeline_cache.cpp:2132-2138` (`SetWorkerBelowNormal`), `:2637-2684` (slots), lanes `vk_pipeline_cache.h:589`, `:615-623` |
| Worker caps | Lifted during both holds: `BackgroundCap` (`:2484-2491`) and `CappedCap` (`:2493-2500`) return "unlimited" while `pre_holding` / `manifest_holding` | | |
| Extra pack workers (`shadPS4:PipelinePack`) | `hw - workers` = **4**, so 16 compile threads in total, for `BuildPackLibraries` with `SOTC_GPL_PACK_LEAN` (default on) | Normal | `:6765-6766`, `:7282-7308` |
| Async translate / compute prebuild / GPL boot library jobs | No separate pool: all are jobs on the same workers (lanes Translate, Compute, Graphics/Predict) | as above | prebuild push `:6518-6530`; library jobs `:6806-6823`; manifest libraries `:9097-9101`, `:9017-9031` |
| Driver cache saver | 1 thread; also runs the pack publication | Normal | `:861-895` |
| Shader cache `DataBase` I/O worker | 1 thread (thousands of small files during the hold; the hold waits for `writes < 64`) | Normal | `cache_storage.cpp:185`, queue `:497`; hold waits `vk_pipeline_cache.cpp:7364-7371`, `:9079` |
| Shader cache prefetch | up to 16 threads, only at startup | Normal | `cache_storage.cpp:342-361`; `WriteCachePack` detached copy `:372-379` |
| Copy pool | `clamp(Available/4, 1, 4)` = 4 | Normal | `gpu/shim/bbport_copy.cpp:54` |
| Recorder | 1; spins 250 us, then waits (futex with `SOTC_FUTEX_WAIT=1`) | High | `vk_recorder.cpp:2443`, `:2464-2480` |
| Command processor | 1; in the holds it waits on `translate_done_cv` in 20-50 ms steps (`:7380-7382`, `:9117-9119`) | High only with `SOTC_GPU_THREAD_HIGH=1` | `liverpool.cpp:79-85` |
| Presenter / loading screen | the loading screen is drawn every frame; the warm-up painter (`vk_presenter.cpp:672-677`) only before `WarmUp` | | |
| Game threads | the guest's own threads keep running and load shader files (the holds wait for `LastProgramNs` to be idle for 3 s) | guest | `:9093-9094` |
| `BbThreads::MakeBackground` (IDLE priority) | defined, **never called** | | `gpu/shim/bbport_threads.h:38-40` |

### 1.2 How many run at once

During the pretranslate hold, 12 workers compile at Normal priority (compute prebuilds plus translations). During
the pack step there are 16. Add the I/O worker, the saver, the recorder's spin and the game's loader threads, and
there are about 15-17 runnable threads on 16 logical CPUs. That matches the 99% CPU. The NVIDIA Vulkan driver
compiles inside `vkCreate*Pipelines` on the calling thread, so the 3,046 s `on workers` (line 3179) is the driver
compiler running on the port's workers. I know of no extra driver compile pool for Vulkan; the driver does run its
own threads (for example its disk cache writer). **Not verified:** to check, look at the thread list in Process
Explorer during a hold, filtered by start address in `nvoglv64.dll`.

## 2. Where the RAM goes, ranked

Size estimates marked "(est.)" are not measured. Only the log lines in section 0 are.

1. **Unexplained growth of ~5-6 GB during the pretranslate hold (4.7 GB to 9.7 GB, and ~1.1 GB of file buffers
   were freed in between, see item 3).** This is the biggest item, and no log line splits it. Candidates:
   - 4,986 compute `VkPipeline`s (4,362 prebuilt variants plus manifest/warm-up ones) kept for the whole session.
     With `SOTC_COMPUTE_PACK` off (the default since 8 Nov, `nonblocking.h:198-201`), a prebuild goes to the live
     cache (`vk_pipeline_cache.cpp:6486-6493`), is registered (`:6510`), and nothing ever destroys it. Only ~409
     of the 4,253 programs had a dispatch in this session (line 3314).
   - 4,253 compute programs keep their modules and `Shader::Info`: the end-of-hold eviction skips compute programs
     unless the compute pack touched them (`:7440-7446`; log line 1444: `0 prepared programs left`).
   - Heaps that the NVIDIA compiler keeps per thread, multiplied by 12-16 compiling threads. Not measured. Two
     facts point this way: `cpack_cold.out` still grew 2.7 GB to 6.4 GB while it destroyed all 3,800 compute
     pipelines and 15,200 libraries (lines 42, 4743), and in the manifest hold, pipelines plus translations cost
     only ~290 KB per pipeline (864 MB / 3,006). (est.)
   - **Needed after it is on disk?** Pipelines that no dispatch has bound: no. They are in the driver's disk cache
     (section 0, `k_heap_only_warm`) and in the live blob. Modules: no. The SPIR-V and meta are in
     `sotc_pretranslate.pack` (`vk_pipeline_serialization.cpp:1116-1171`), and `LoadLazyProgram` brings them back.
2. **Driver-held VkPipelineCache objects: ~1.3 GB plus driver overhead.** The live cache (368 MB loaded, 563 MB at
   save time) and the pack cache (743 MB) are created from whole files (`vk_pipeline_cache.cpp:836-857`, loader
   `:771-835`) and stay resident all session. The memory note `driver-cache-bloat` measured 2.3 GB of RAM for a
   2 GB file. In this run the pack list was invalid (line 1440, `no valid list`), so the 743 MB of pack cache was
   held without any list naming its contents. The pack cache has no size cap (`:850`, no `max_bytes`). The live
   cap (`:186-193`, 512 MB) leads to a cycle: a cold run writes 563 MB, the next launch throws it away, and the run
   after that rebuilds it from the NVIDIA cache.
   **Needed after it is on disk?** Only as a source of hits. On NVIDIA the driver's own disk cache already serves
   those hits. The comment at `:765-768` says that cache is tied to the executable, so it misses once after each
   port update. That is the case the blob exists for.
3. **Transient whole-blob buffers (0.4-1.1 GB per event).** At startup `load_cache` reads each file into a
   `std::vector` (`:803-808`), and the driver copies it. The 4,705 MB sample at `:926` is taken while both copies
   are alive. Every save allocates a buffer as big as the cache (`SaveCacheFile`, `:1013`), 563-781 MB per save,
   on top of the driver's own serialization. Pack publication `:7139-7174`, exit `:7239-7277`.
   **Needed?** No. Both can go through a file mapping (section 4.3).
4. **Manifest pipelines and translations: 864 MB** (line 2847). These 3,006 graphics/compute pipelines are what the
   first scene draws. Keep them.
5. **Held GPL libraries: 263 MB for 4,757 libraries** (line 2847; ~55 KB each). Manifest libraries carry
   `FlagManifest` and are never evicted (`vk_graphics_pipeline.cpp:265-266`, `:136`). Only predicted libraries
   can be evicted (`EvictStale`, `:128-160`). Pack libraries are already destroyed right after compile (`t_gpl_discard`,
   `:245-252`). **Needed?** They are needed while their draws run, and a draw that misses one waits for it: 23
   synchronous library builds took 1,448 ms (63 ms each, log line 3321). Low value, high risk. Keep them in v1.
6. **Pretranslate pack: already streamed.** Each variant is written with `fwrite` as it completes (`PrePackAdd`,
   `vk_pipeline_serialization.cpp:1116-1145`). Only a 32-byte index entry per variant stays in RAM. The shader
   cache write queue (`cache_storage.cpp:497`) holds blobs only until the I/O worker writes them, and the hold
   waits for it to drain.
7. **"Until the hold ends" items:** the modules of pack jobs (`drain_modules`, `vk_pipeline_cache.cpp:6841-6862`,
   freed as the jobs finish), the modules that `restore_pack` loads again (`:7384-7405`), and the program eviction
   after the library wait (`:7433-7446`). In this run all of these were empty or small, because the pack step
   queued 0 jobs.

## 3. How results reach disk today

- **Live driver cache (`sotc_vk_pipelines.bin`):** written as a whole blob only: `vkGetPipelineCacheData` into a
  vector, then a `.tmp` file and a rename (`vk_pipeline_cache.cpp:981-1047`). The saver thread does it every 60 s,
  or on request when the file grew by at least 5% or 64 MiB (`:999-1001`). The holds request a save at their end
  (`:7419-7425`, `:9131-9137`), and there is a final save at exit (`:945-949`). Nothing is incremental, and nothing
  is released from memory after a save.
- **Pack cache (`sotc_vk_pack.bin`):** the same whole-blob write, done by `RunPackPublish` on the saver thread
  (`:7139-7174`, requested at `:6917-6938`) and again at exit when it grew (`:7239-7277`). The library list
  (`sotc_pack_libraries.bin`) is written after it and bound to the blob's hash. Libraries are destroyed after
  compile. The VkPipelineCache object that holds their binaries is not.
- **Compute pack (`SOTC_COMPUTE_PACK=1`, off):** this already does compile, then into the pack cache, then destroy
  the pipeline and its module (`PackComputePipelines`, `:7027-7119`). A later dispatch builds the pipeline again
  from the pack cache, inline on the recorder (`nonblocking.h:189-205`). It was turned off on 8 Nov because the
  synthetic perm-0 specs never matched a dispatch. **With `SOTC_COMPUTE_SPECFREE=1` they do match** (27,478 of
  27,708 dispatches hit a prebuilt variant, line 3109), and `PackComputePipelines` already handles the
  int/float variant pairs (`:7040-7047`). The reason it was turned off no longer holds.
- **Doc drift:** the comment at `nonblocking.h:189` and the one at `pretranslate.cpp:89` still say
  `SOTC_COMPUTE_PACK` is on by default. The code is `SwitchOn` (off). Fix the comments when this area is next edited.

Can results be streamed in batches and the objects freed? Yes:
- `vkGetPipelineCacheData` works on any cache object, including a small one made for a single batch.
  `vkMergePipelineCaches` can combine batches when that is wanted.
- After a batch's data is on disk, the batch cache can be destroyed, along with every pipeline and module of that
  batch that no draw or dispatch holds. Building one of them again later hits a cache: the NVIDIA internal cache
  in any case, or a batch file loaded on demand (section 4.2). That is the "recreate from disk" path, and a hit
  costs ~1-25 ms on a worker (22 ms average while 12 workers were busy in `k_heap_only_warm`).

## 4. Design (all switches off or neutral by default until measured)

### 4.1 CPU: `SOTC_COMPILE_THREADS`, `SOTC_COMPILE_PRIO`

- `SOTC_COMPILE_THREADS=<n>`. Default: the number of physical cores (8 on the 5700X; from
  `GetLogicalProcessorInformationEx`, the same source as `BbThreads::GetCpuLayout`, `bbport_threads.h:71-195`),
  at least 2. It caps how many jobs run at once **while `pre_holding || manifest_holding`**: add a hold quota next
  to `CappedCap` in `PopJobLocked` (`vk_pipeline_cache.cpp:2529-2551`), and make `StartExtraWorkers` (`:7282-7291`)
  start no workers beyond `n`. The worker count during play stays 12. Expected: CPU use during the hold drops to
  about 55-65% of 16 logical CPUs. Compile throughput drops less than the thread count, because the second thread
  on a core adds only ~20-30% for compiler workloads. (est.)
- `SOTC_COMPILE_PRIO=below|idle|normal`, default `below` during the holds only. Every worker that takes a job while
  a hold is active calls `SetWorkerBelowNormal(true)` (`:2132`); `idle` uses `BbThreads::MakeBackground`. The
  command processor waits through the whole hold, so nothing the port runs is starved. The game's loader threads
  and the loading screen get the CPU first, which keeps shader files arriving at the same rate. The 27 Oct
  objection (`:2579-2583`) is about play, where the recorder blocks on compute work that a worker has claimed.
  During play keep the existing rules.
- Optional: `SOTC_COMPILE_ECO=1` puts the workers in Windows EcoQoS (`SetThreadInformation`,
  `ThreadPowerThrottling`) during the holds. Lower clocks and less heat; on a hybrid Intel CPU it moves them to
  E-cores. Measure before turning it on by default.

### 4.2 RAM: `SOTC_COMPILE_STREAM=1` (batch writes and release)

1. **Compute prebuilds go to a batch cache, not the live cache.** At `:6486-6493`, while a hold is active, build
   into the current `stream_batch` VkPipelineCache (a new one every 256 jobs, or every 64 MB from
   `vkGetPipelineCacheData` size queries). When a batch's last job completes, the saver thread
   (`:861-895`) writes it as a segment `cache/sotc_stream/<seq>.bin` plus an index record
   (pipeline id → segment, offset), via a file mapping (4.3). Then it destroys the batch cache.
2. **Release per batch, not at the end.** For each finished batch, run the `PackComputePipelines` rule
   (`:7048-7095`: not bound by a dispatch, no translation in flight, the other variant not busy) on the batch's
   pipelines only. Destroy those pipelines and their modules, and mark the programs `pack_touched` so the eviction
   at `:7440-7446` sends them back to `lazy_programs`. Pipelines bound by a dispatch stay, as they do today.
3. **Do not load whole blobs again on the next launch.** The live cache keeps only what play builds, so it stays
   small and the 512 MB cycle stops. Do not load the segments at startup. A destroyed pipeline comes back when
   either:
   - its area's shader file loads. The `RES_LOADED` hook already names the programs (`probe_csdr.cpp:336`), so
     queue rehydrate jobs on the workers (`LoadLazyProgram` + build). The build hits the NVIDIA internal cache, or a
     temporary VkPipelineCache made from just that program's segment, which is destroyed after the build. Or:
   - its first dispatch finds no pipeline. The fallback is the existing inline build on the recorder from a cache
     (`nonblocking.h:194-196`).
4. **GPL libraries:** unchanged in v1. Pack libraries are already discarded. The pack cache can use the same
   segmented scheme (batch cache, write, destroy) instead of one object that is resident all session, with the
   same on-demand rule when a library is needed.
5. **Better long-term option:** if the device exposes `VK_KHR_pipeline_binary` (check the extension list; not
   probed in this tree, `grep` finds no use), write each pipeline's binary to its own record and build again from
   that one record. No VkPipelineCache object of any size then needs to stay resident. The rest of this design
   works without it.

### 4.3 RAM: file-mapped cache I/O (`SOTC_CACHE_MAP=1`)

- Save: `vkGetPipelineCacheData(size)`, then create a `.tmp` file of that size, `CreateFileMapping` +
  `MapViewOfFile`, `vkGetPipelineCacheData(view)`, `FlushViewOfFile`, unmap, rename. The pages are backed by the
  file, so they add no private commit. This replaces the vector at `:1013`.
- Load: pass the mapped view as `pInitialData` (`:815-819`) instead of the vector read at `:803-808`. This removes
  one of the two startup copies (~1.1 GB transient in this run).

### 4.4 Expected effect

| Item | RAM at the end-of-hold peak | Loading time | Confidence |
|---|---|---|---|
| Threads 12/16 → 8, below normal | unknown: depends on per-thread compiler heaps (item 1); 0 if there are none | +10-30% on the compile phases: 407 s → ~450-520 s (est.) | CPU: high; time: est. |
| Stream compute prebuilds + release ~3,950 undispatched variants with their modules | -0.5 to -1.5 GB (est. from ~290 KB per pipeline+translation) | +0.1-0.3 s per batch on the saver thread, overlapped | est. |
| No resident whole blobs (live stays small, pack segmented) | -1.0 to -1.5 GB (blob size × ~1.15, memory note) | 0 at the hold. A later area pays rehydrate jobs (~409 programs × 5-25 ms on workers, at the area's load) | medium |
| File-mapped save/load | 0 at the end-of-hold peak; -0.6 to -1.1 GB at the startup and save-time peaks | 0 | high |
| **Total** | **~12.0 GB → ~8-9.5 GB.** Reaching ~6 GB needs item 1 explained (`SOTC_RAM_REPORT=1` "remainder" line) | ~+10-30% first loading | |

### 4.5 Risks

- **A released pipeline or library needed by a draw before its rehydrate job runs.** It stalls on the recorder:
  1-25 ms per compute pipeline on a cache hit, ~63 ms per GPL library (line 3321). Mitigations: rehydrate at shader
  file load (`RES_LOADED`), before any draw. Never release what a dispatch or draw has bound, and keep libraries in
  v1. Count `compute cache-hit builds inline` (already printed, line 2945) and gate on it being near 0 on the
  automated route (`user/routes/tests.json`).
- **The NVIDIA disk cache is not under the port's control.** Its size is set in the NVIDIA control panel; it can be
  cleared or evicted, and it is tied to the executable (`:765-768`), so the first launch after a port update
  misses. With `SOTC_COMPILE_STREAM`, the port's own segments cover that case. On AMD/Intel the segments are the
  only reliable hit source, so the on-demand segment load is required, not optional.
- **The pack list is bound to the loaded blob's hash** (`PackLibraryListMatches`, `:6640-6658`). A segmented store
  needs a new list format: segment ids, plus the driver identity in each segment header. Any mismatch must
  invalidate the whole list and fall back to compiling.
- **The bounded waits in `PackComputePipelines` (60 s) and `BuildPackLibraries` (30 min)** can leave a pipeline
  unfinished. The per-batch release must skip such a pipeline, as the existing rule does (`:7055-7057`).
- **Below-normal workers** under a busy game thread at the loading screen could stretch the hold more than
  estimated. Measure one round per configuration (no warm-up, no repeats), with the hold time and the
  `SOTC loading screen: hidden after` line.
- **Fewer threads with `SOTC_CPUSETS=1`:** the default is already capped at 4 (`:906`). The hold quota must take
  the smaller value.

## 5. Order of work

1. Measure, with no code change: one cold run with `SOTC_RAM_REPORT=1` (the remainder line at the end of the
   pretranslate hold), and the same run with `SOTC_COMPUTE_PACK=1` (with SPECFREE on). The second run shows how
   much destroying the compute pipelines and modules frees. Move `sotc_vk_pipelines.bin` aside first if it is
   oversize (memory note), and use `SOTC_SPV_SALT` for the cold run.
2. `SOTC_COMPILE_THREADS` and `SOTC_COMPILE_PRIO` (small, contained in `PopJobLocked` / `StartExtraWorkers` /
   `AsyncPipelineWorker`). Measure 8 against 12 threads: CPU %, hold time, private memory.
3. `SOTC_CACHE_MAP` (`SaveCacheFile` and `load_cache` only).
4. `SOTC_COMPILE_STREAM`: batch caches, per-batch release, segments with index, rehydrate at `RES_LOADED`.
5. If the device has it: `VK_KHR_pipeline_binary` per-pipeline records in place of segments.
