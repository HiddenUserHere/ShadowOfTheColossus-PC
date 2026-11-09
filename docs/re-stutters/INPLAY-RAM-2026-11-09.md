# In-play RAM: where the ~9.7 GB of private memory goes (9 Nov 2026)

Measurement and analysis only: no code was changed or committed. Branch `sync/upstream-0.72.2`, HEAD `fbd70c4`,
`out/bb-probe.exe` identical to `out/gpu-codex/bb-probe.exe` (checked with `cmp` before the run). The machine is the
user's: Ryzen 7 5700X, RTX 3060 Ti, warm cache, the user's `sotc.ini`.

## 1. Method

1. **Run 1 (done).** One pass of the automated test route with the existing RAM report:
   `env SOTC_RAM_REPORT=1 SOTC_LAB_ROUTE=user/routes/tests.json SOTC_LAB_ROUTE_EXIT=40 SOTC_LAB_ROUTE_WAIT=0 python sotc.py`
   → `user/prof/m0/s_ram_report.out`. The run was warm, no salt, and `msys2-env.sh` was not sourced.
   `SOTC_RAM_REPORT` (`gpu/sotc/ram_report.cpp`) prints these lines every 5 s:
   - `PrivateUsage` and the working set;
   - a VirtualQuery sweep (MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE);
   - the private split: loader image, guest thread stacks, `HeapSummary` of all process heaps, mimalloc, and the
     remainder;
   - the 8 largest private allocations;
   - staging held bytes, VMA blocks by memory class, and the `VK_EXT_memory_budget` heaps.
   The report printed 3 times, at lines 603, 816 and 1017 of the log.
2. **Static attribution.** I read the owners of the big items in the code:
   - the cache storage prefetch, the pretranslate pack and the staging pool;
   - the guest memory section in `src/runtime_memory.c`;
   - every `VirtualAlloc` call site of our own code.
   I also parsed the shader cache pack file on disk.
3. **Run 2 (instrumented): prepared, NOT run.** The coordinator put game runs on hold (the user is playing). The
   instrumentation is written and is not applied to the tree:
   - `scratchpad/hooks.inc`: an IAT-hook block for `ram_report.cpp`. It hooks VirtualAlloc/VirtualFree in every
     non-system module, and keeps a HeapAlloc/HeapReAlloc/HeapFree pointer map for the `nv*` modules.
     `nvoglv64.dll` and `nvgpucomp64.dll` link a static CRT and import these functions from KERNEL32, checked with
     `llvm-objdump -p`. It also prints private bytes by page protection (write-combine shows host-visible Vulkan
     mappings), by allocation size and by owner module.
   - `scratchpad/ram_count_new.cpp`: a counting global `operator new`/`delete` (live bytes of our own C++ heap, by
     block size).
   Run 2 is needed to split the ~6.2 GB non-heap remainder (section 2, row R).

## 2. Measured breakdown (run 1, last report in play, `s_ram_report.out:1017-1036`)

`PrivateUsage` = 9,753 MiB; the sweep finds 9,473 MiB of committed MEM_PRIVATE. Working set 9,376 MiB.

| # | Item | MiB | Source | Cut? |
|---|---|---|---|---|
| G | **Guest (PS4) memory** | **0 private** (~1,972 resident, in the working set only) | The 10 GiB section is mapped twice (guest view and backing view: `mapped 19,753 MiB`). It is a pagefile-backed `SEC_COMMIT` section, charged to system commit and not to `PrivateUsage`. The `guest pool committed 0.0` line is the on-demand counter, which is unused here (`SOTC_PC_MEMORY` imports the whole view). | Cannot be cut, and it is not part of the 9.7 GB |
| L | Loader image (eboot, PRX, trampolines) | 76.6 | sweep | no |
| S | Guest host thread stacks | 11.9 | sweep | no |
| H1 | **The whole `sotc_pretranslate.pack` held in RAM by the shader cache prefetch** | **1,099.5** | top allocation `0x1cd210000 1099.5 MiB` = 1,152,952,933 bytes, exactly the size of `CUSA08809_v6210f386/sotc_pretranslate.pack`; it is entry 1 of `cache.sotcpack` (parsed: 13,476 entries, one is `sotc_pretranslate.pack` 1,099.5 MB) | **yes, without risk** (section 3.1) |
| H2 | Rest of the process heaps (one NT heap segment `0x35e40000` 1,022.7 MiB, `0x48dda0000` growing 246 → 291 MiB, smaller segments) | ~1,287 | `HeapSummary` 2,386.9 committed / 2,322.7 allocated, minus H1. It holds our C++ objects (programs + `Shader::Info` for 6,187 programs, maps, bind caches), the loose shader-cache prefetch (~132 MB: 2,639 .spv 88 MB, 2,638 .meta 38 MB, 8,194 .key 5 MB) and the NVIDIA driver's malloc (static CRT, same process heap). | partly; the split needs run 2 |
| V | Host-visible Vulkan memory (VMA system blocks) | ~800 (to confirm) | `host-visible system 800.1 MiB`; staging held: HostUncached 656 + HostCached 128 = 784. Likely the `0x7580000 256 MiB` and 3 × `128 MiB` private allocations of the top list. Not yet proven to be counted as private: run 2 shows it by the write-combine protection. | yes (section 3.2) |
| R | **Unattributed private remainder** (not heap, not loader, not stacks, minus V) | **~6,200** | `private remainder 6,997.4 MiB` − V. None of our own `VirtualAlloc` sites is large (probe.c `allocate()`, alt stack, prefetch 4 MiB buffer, `runtime_low_map`, page_manager single pages). So this is the NVIDIA driver (`nvoglv64.dll` / `nvgpucomp64.dll` import VirtualAlloc), or Vulkan memory mapped by the kernel. | see 3.3-3.5; needs run 2 |
| — | Not in the sweep (`PrivateUsage` − sweep) | 280 | page-table / kernel-side charges (est.) | no |
| — | Buffer cache region managers / texture cache slots + page table | 4.9 / 6.5 | owner counters | negligible |
| — | Descriptor pools | ~13 | `DESC REUSE` line | negligible |

VRAM is not in these figures. Device-local memory does not count toward private bytes:
- buffer arenas 1,600-1,664 MiB: raw allocations, in "outside VMA, heap 0" 1,936 MiB;
- images ~990 MiB;
- the DeviceLocal staging ring 256 MiB.

The question "are the arenas host-visible?" therefore has the answer no: the arenas are device-local. Only the
staging rings and the HostCached blocks are host-visible (row V).

### Pipeline objects in this configuration (warm)
- Programs 6,187; graphics pipelines 3,300; GPL libraries held 4,961.
- Compute pipelines **1,333**: this config builds only what is used, with SPECFREE and no prebuilt variants. The
  ~4,986 of the cold analysis do not apply to a warm session. `COMPUTE RELEASE` already destroyed 817 compute
  modules.
- The manifest hold measured **+232 MB for 3,268 pipelines and their libraries** (103 MB pipelines + translations,
  129 MB libraries; log line 386), about 70 KB per pipeline.
- Driver caches loaded file-mapped: live 83 MB (it reaches 200 MB on disk by the end of the user's session), pack
  257 MB. The driver copies them into its own memory (about 1.15 × the file, memory note `driver-cache-bloat`), so
  about **0.4-0.5 GB of R** (est.).

### Timeline (private MB)

| Point | Warm run 1 | Cold `n_cold_lowcpu` |
|---|---|---|
| after cache load | 2,532 | 2,110 |
| hold start | 6,009 (manifest) | 2,986 (pretranslate) |
| hold end | 6,241 | 7,377 (manifest) |
| first in-play reports | 9,184 → 9,593 → 9,753 | 10,639 → 10,929 |

Two jumps are not explained by pipelines:
- **+3.48 GB between the cache load and the manifest hold start (warm).** This includes H1 and its boot transients
  (3.1). The warm-up (3,674 ms) and game boot are in the same window.
- **+2.9 GB between the hold end and the first in-play report, warm and cold alike.** In this window: ASYNC COMPUTE
  builds 1,333 pipelines (6.3 s on 12 workers), the staging reserves fill up (`TickFrame` adds one 16 MB block per
  frame), and the world loads.

The holds themselves add only 0.2-0.5 GB. Run 2's owner and protection lines are what will split the post-hold
2.9 GB.

## 3. Cuts

Risk is measured against the rule "never compile or stall during play".

### 3.1 Do not prefetch `sotc_pretranslate.pack` (and stop the per-launch pack rewrite): **-1,100 MB resident, -2.4 to -3.5 GB boot peak. Risk: none**
- **Where:**
  - `DataBase::Prefetch` (`gpu/shadps4/video_core/cache_storage.cpp:318-420`) skips only `.tmp` and `.sotcpack`.
    It therefore reads the 1.1 GB pretranslate pack as an ordinary cache file and keeps it in `prefetched` for the
    whole session. `FinishPreload` (`:752`) never clears the map, on purpose, because of the 22 Oct hard-disk stall.
  - Nobody reads that copy. The pretranslate pack has its own `FILE*` and index (`PackRead` / `PrePackLoad`,
    `vk_pipeline_serialization.cpp:1083-1230`).
- **Boot transients on top:**
  - `ReadCachePack` reads the whole 1.29 GB `cache.sotcpack` into one vector, and copies every entry out of it
    (`:238-282`).
  - Then `WriteCachePack` gets a full copy of `prefetched` on a detached thread (`:404-411`) and writes 1.29 GB back
    to disk. This happens on every launch in which any loose file is outside the pack or newer than it, which is
    every launch here (`3269 newer than the pack`). The newer files are probably the `.key` files that
    `RewritePipelineUseCount` saves again every session (`vk_pipeline_serialization.cpp:280-294`; not verified).
- **Cut:** skip `.pack` (or exactly `sotc_pretranslate.pack`) in the directory loop of `Prefetch`. The next launch
  rewrites `cache.sotcpack` without it, at ~190 MB.
- **Risk:**
  - None for play: no reader uses the copy.
  - Boot gets faster: about 2.4 GB less copying and 1.1 GB less disk write.
  - No change to the shader cache, because recompiler output does not change.

### 3.2 Cap the host staging reserve: **-300 to -550 MB (if V counts as private; run 2 confirms). Risk: low-medium**
- **Held now:**
  - HostUncached 656 MB and HostCached 128 MB.
  - The upstream 0.72 reserve `SOTC_STAGING_KEEP_MB` = 128 MB per host ring, made ahead one block per frame
    (`vk_staging_buffer_pool.cpp:66-86`, `:292-310`).
  - `SOTC_STAGING_LARGE_KEEP_MB` = 256 MB of idle large buffers per type (`:45-51`, `:342-351`).
- **Cut:** start with environment variables only: `SOTC_STAGING_KEEP_MB=32` and `SOTC_STAGING_LARGE_KEEP_MB=64`.
  Optionally trim the rings above the reserve sooner when idle.
- **Risk:** the reserve exists because of area-change hitches, measured at 11 block allocations in 95 ms on 3 Nov
  (Pro + Maximum, with video memory near its budget). Measure on `user/routes/tests.json`: a route pass with the
  caps, comparing the `SOTCSTALL ... alloc` and `FrameAlloc` numbers. This cut does not touch compiling.

### 3.3 Release the pack VkPipelineCache after the manifest hold: **≈ -300 MB (257 MB file × ~1.15). Risk: medium**
- The pack cache (257 MB file, driver copy resident) exists to serve the GPL library builds of the holds. After the
  manifest hold (libraries held 4,961), destroy it.
- Keep the live cache. It is small (83 MB at load), and in-play builds go to it.
- **Risk:** an in-play library or pipeline build that would have hit the pack blob. It still hits the NVIDIA disk
  cache, except on the first launch after a port update. On AMD and Intel there is no such cache, so gate the cut on
  the NVIDIA vendor ID or on the driver identity matching. The live cache grows during play (83 → 200 MB on disk in
  the user's session), and its driver copy grows with it. Cap it by saving it and restarting the object between
  areas, not during a frame.

### 3.4 Driver per-thread compiler memory: worker threads after the holds: **unknown, possibly 1-3 GB of R. Risk: medium**
- R grows by ~2.9 GB right after the holds, while 12 workers build 1,333 compute pipelines and the remaining
  pipelines. That is far more than the measured ~70 KB per pipeline in the hold.
- The earlier cold analysis saw the same: `cpack_cold` grew while it destroyed every pipeline.
- **Hypothesis:** `nvgpucomp64` keeps an arena per compiling thread.
- **Cut (only if run 2 shows `nvgpucomp64` / `nvoglv64` VirtualAlloc growth that follows the worker count):** after
  the manifest hold, let all but 2-4 workers exit. A thread exit frees the driver's TLS arenas; idle threads keep
  them. Alternatively cap concurrent compile jobs in play (the `SOTC_COMPILE_THREADS` quota already exists for the
  holds).
- **Risk:** in-play compile throughput, for the lazy PSO and predicted libraries. With a warm cache, play compiles
  almost nothing: the route showed `sync full builds 0` and `compute cache-hit builds inline 0`, so 2-4 workers are
  enough. Watch the `gpl nodrop` sync counters and `compute waits`. They must stay 0 on the route.

### 3.5 Unused programs: drop module + `Shader::Info` for programs with no pipeline in the current area: **~100-300 MB of H2 (est.). Risk: high**
- 6,187 programs are resident.
- `Shader::Info` is read at every draw for binding, so it cannot go for programs that are in use.
- Only programs that no live pipeline references could go back to `lazy_programs`. `LoadLazyProgram` reloads them
  from the pretranslate pack (`vk_pipeline_serialization.cpp:1232`), but the reload happens on the draw thread
  unless it is queued at `RES_LOADED`.
- **Risk:** a draw that misses a program stalls on a pack read + `CompileSPV`. Do this last, behind a switch, and
  only after run 2 shows how much of H2 is ours (counting `operator new`).

### 3.6 Not worth cutting in a warm session
- GPL libraries (~130 MB for 4,961) and graphics pipelines (~100 MB): a miss stalls a draw (~63 ms per library).
- Compute pipelines (1,333, all of them used by dispatches).
- Region managers and texture cache tables (~11 MB).
- Descriptor pools (~13 MB).

For the cold first session, destroying the ~3,500 undispatched SPECFREE compute variants after the pretranslate hold
still applies (see `COLD-COMPILE-CPU-RAM-2026-11-09.md`, section 4.2).

## 4. Expected result and recommended order

| Order | Cut | MB saved in play | Risk | Status |
|---|---|---|---|---|
| 1 | 3.1 skip `sotc_pretranslate.pack` in the prefetch | **-1,100** (and -2.4 to 3.5 GB boot peak) | none | ready: one-line filter |
| 2 | Run 2 (instrumented) | — | — | prepared in the scratchpad, waiting for the hold to lift |
| 3 | 3.2 staging keep caps (env only first) | -300 to -550 | low-medium (area-change hitch) | needs one route pass |
| 4 | 3.4 fewer workers alive in play | 0 to -3,000 (decided by run 2) | medium | depends on run 2 |
| 5 | 3.3 release the pack VkPipelineCache after the hold | ~-300 | medium (NVIDIA only first) | |
| 6 | 3.5 unused programs back to lazy | -100 to -300 | high | last |

With 1 + 3 + 5, in-play private goes from ~9.75 GB to **~7.6-7.9 GB**. Reaching ~3 GB needs the driver part of R
(3.4), and run 2 decides whether that part is reducible.

The user's "~3 GB normal" is most likely Task Manager's *Memory* column, which is the private working set. The
private working set is about 9.4 GB of working set − 2.0 GB guest (shared section) − ~0.3 GB images ≈ 7.1 GB now.
It is a smaller number than `PrivateUsage`, so the two cannot be compared directly.

## 5. Run 2: what to apply and read
- **Apply:**
  - Insert `scratchpad/hooks.inc` into the anonymous namespace of `gpu/sotc/ram_report.cpp`, with the includes
    `<unordered_map> <mutex> <atomic> <utility>`.
  - Declare `extern "C" __attribute__((weak)) int64_t sotc_new_live_bytes(void), sotc_new_live_count(void),
    sotc_new_class_bytes(int);`.
  - Call `ReportDetail(out)` at the end of `Report`.
  - Call `InstallHooks` from `LogRam` (`vk_pipeline_cache.cpp:115`) through a new public
    `SotcRamReport::Checkpoint()`, so that the hooks go in at "after cache load".
  - Add `sotc/ram_count_new.cpp` to the `bb-probe` sources in `gpu/CMakeLists.txt`.
- **Run:** with `SOTC_RAM_REPORT=2`, same route, one pass.
- **Read:**
  - `RAMX private by protection` (write-combine = V);
  - `RAMX private VirtualAlloc owner` (`nvoglv64` / `nvgpucomp64` vs untagged = R);
  - `RAMX heap live` (driver malloc in H2);
  - `RAMX operator new live` (ours in H2).
- **Revert:** revert the edits, rebuild, and copy `out/gpu-codex/bb-probe.exe` back to `out/bb-probe.exe`.
