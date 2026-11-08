# The remaining draw stutter and the plan to remove it (8 Nov 2026)

## What the stutter is

On a cold cache, after the start loading screen, a few frames last 600–900 ms. In those frames the renderer
threads are almost idle: the per-frame diagnostic shows the draw thread busy for 24–60 ms and the command
processor (CP) for 40–70 ms. Nothing in `SOTCSTALL` accounts for the rest.

The cause is a blocked wait, which no counter in `SOTCSTALL` covers:

1. The CP looks up a draw's pipeline before it hands the draw to the draw thread
   (`liverpool.cpp:2190-2195`, `SubmitDraw`).
2. The optimized pipeline is missing, so `NoDropLink` tries to fast-link stage libraries
   (`vk_pipeline_cache.cpp:2568-2600`).
3. The VS or PS library is missing too (a shader the manifest never saw), so `BuildLibrariesParallel`
   (`vk_pipeline_cache.cpp:1837`) queues both and the CP **blocks on a condition variable** (`lib_wait_cv`,
   up to 10 s, `:1881`).
4. The driver compile of one library is about 15 ms when the machine is idle, but at that moment the 12
   workers are saturated with the pack translations (about 60–100 s of worker time) and the compute pipelines
   (about 90–100 s), so the compile takes 100–200 ms each.
5. While the CP is blocked, the draw thread has nothing to record and waits for the frame
   (`NativeFrameWait`, `buffer_cache.cpp:9260`, the "frame N not recorded within 200 ms" lines).

Evidence:

| Route (cold) | CP library waits | Longest frames |
|---|---|---|
| `dynnp_cold.out` | 3 waits, **585 ms** | 716 ms, 642 ms |
| `nopred_cold.out` | 48 waits, 2.7 s | 683 ms, 742 ms |
| `trace_cold.out` (15,209 pack libraries compiled into the driver cache before play) | 3 waits, **2.7 ms** | 109 ms |

With the libraries already in the driver cache, the same route has no frame above 110 ms. The stutter is
the CP waiting for the driver to compile a library it has never seen.

A second, smaller cause is still open: in `dyn_cold.out` frame 75 took 909 ms with only 59 ms of library
waits in the whole session. The counters do not explain it. Suspects, all in code paths that touch the
driver cache or hold the loading screen during play:

- `MaybeHoldForManifest` can start a hold **during play** when 32 or more manifest stages are pending
  (`vk_pipeline_cache.cpp:7790-7800`), and the hold ends with a synchronous `SaveDriverCache()` on the CP
  (`:7898-7903`) unless `SOTC_NB_IO=1`. The file is 1–2 GB.
- The background saver (`:772-786`) calls `vkGetPipelineCacheData` on the whole cache every 60 s. If the
  driver serializes that against pipeline creation, the CP's fast link blocks inside the driver.
- `SOTC_FPS_LEAN=1` zeroes the `cp parse` / `cp blocked` timers, so the lean runs cannot show these. A run
  with `SOTC_FPS_LEAN=0 SOTC_BLOCK_LEDGER=1 SOTC_TRACE=1` is the next measurement.

## Why the current build is still not acceptable

The run that removed the stutter paid for it:

| First launch (cold) | `trace_cold.out` |
|---|---|
| Pretranslate of every loaded program | 14,738 translations |
| Manifest hold | 34.6 s |
| Pack libraries into the driver cache | 15,209 libraries, 85.0 s |
| Loading screen | **134 s** (with trace overhead) |
| Process private memory | **11.6 GB** |

Only about 1,800 of those libraries were used by the route. The pack compile does what the user asked
(compile everything once, keep only what is used), but it keeps all 14.7k translations and their modules
resident while it runs, and the whole hold runs on 12 workers at normal priority next to the compute builds.

## Plan

Principle: **the CP never waits for the driver.** Every compile happens on workers ahead of the draw, from
data the game gives before it draws (the manifest, then the materials it creates). If something is still
missing at draw time, the wait is a cache hit of a few ms, never a compile.

### 1. First launch: compile everything, but cheaply

Keep the pack compile, change how it runs:

- Translate → build the two libraries → **destroy the module and the translation** right away. Keep only
  the program's hash, permutation and specialization in the shader index. Memory stays flat instead of
  holding 14.7k modules.
- Run it **after** the manifest pipelines and the compute pipelines, on all 16 hardware threads, with the
  draw/CP threads idle at the loading screen. Target: about 15,209 × 15 ms ÷ 16 ≈ 15 s, not 85 s.
- Write the compiled-library list and the driver cache **once**, at the end, on the saver thread.
- Verify on the second launch that the list is accepted and the compile is skipped (the first run printed
  "no valid list: driver cache missing"; the next warm run must print "already compiled (listed) 15209").

### 2. Material-driven libraries (the "load only what is used" half)

- When the game creates a material (`0xf9390` hook, `SOTC_GPL_PREDICT`), queue the VS/PS library jobs of
  that material's slots on the Predict lane. With shader-only keys these are the exact libraries the draw
  will use, and after step 1 they are driver-cache hits (about 1 ms each).
- Keep in memory only libraries that a created material references, plus the manifest's. Evict a library
  when every material referencing it is destroyed (refcount by material), never while a link is in flight.
- Measure the lead time between material creation and first draw with a counter (`predicted: built before
  the draw N, built after the draw M`). If M is not 0, step 3 covers it.

### 3. Dedicated workers for anything a draw waits on

- Reserve 2 of the 12 workers for the Urgent and Predict lanes only. Translation, compute prebuild and
  optimized pipeline builds never run on them. A library compile then costs its real 15 ms (or a 1 ms cache
  hit), not 100–200 ms.
- The CP's parallel wait becomes: both libraries requested → at most max(VS, PS) on an idle worker.
- Count the wait in `cp blocked` even in LEAN mode, and add it to `SOTCSTALL` so it is never invisible again.

### 4. Nothing heavy during play

- No hold during play: `MaybeHoldForManifest` and the pretranslate re-hold run only while the loading
  screen is up. During play, late stages go to the Predict lane.
- No synchronous driver cache save on the CP: all saves on the saver thread (`SOTC_NB_IO` behaviour by
  default), only at loading screens, on exit, or when the cache has been idle for several seconds.
- Split the driver cache into two `VkPipelineCache` objects: a large read-mostly one for the pack
  libraries (saved once after step 1, merged at startup) and a small live one that the 60 s saver touches.
  Then `vkGetPipelineCacheData` on the live cache is cheap and cannot stall a link in the driver.

### 5. Measure and close

- Cold route with `SOTC_FPS_LEAN=0 SOTC_BLOCK_LEDGER=1 SOTC_TRACE=1 SOTC_DROP_TRACE=1` and a new
  `SOTC_SPV_SALT`. Gates: 0 drops, CP library waits 0 ms, no frame above 100 ms after the loading screen,
  loading screen under 60 s on the first launch and under 10 s warm, private memory under 6 GB.
- Warm route: same gates, and "already compiled (listed)" equal to the pack size.
- Then the user's visual check on a cold launch.

## Out of scope here

Table mode (S2) stays off. The pretranslate pack translation stays until step 1 replaces its memory
behaviour; if step 2 covers every draw, the pack translation can later be limited to material-referenced
programs.
