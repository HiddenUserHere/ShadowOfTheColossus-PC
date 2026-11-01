# P1b: GPL fast-link for cold pipeline misses, wave 1 report

Component P1 part 3 of DRAW-THREAD-DESIGN.md (section 4.2). Switch: `SOTC_PIPE_GPL=1` (off by default).
Owned files: `vk_instance.{h,cpp}`, `vk_graphics_pipeline.{h,cpp}`, `vk_pipeline_cache.{h,cpp}`. `vk_rasterizer.cpp`
not touched. Nothing was built, run or validated.

Verified: a syntax-only compile (exact `compile_commands.json` command, `-fsyntax-only`) of
`vk_instance.cpp`, `vk_graphics_pipeline.cpp` and `vk_pipeline_cache.cpp`, all exit 0. The Vulkan enum and struct
names were checked against the `vulkan.hpp` of the CLANG64 toolchain (header 363). Everything else is verified by
reading the code only: no validation layer, no driver, no frame timing.

## 1. Summary

| Item | State |
|---|---|
| 1. Extension and feature, gated by the switch | Done. `VK_KHR_pipeline_library` then `VK_EXT_graphics_pipeline_library`, feature `graphicsPipelineLibrary` queried first. Nothing enabled when the switch is off or the feature is missing. |
| 2. Library cache, keyed by the state of each part | Done. Four parts, exact-bytes keys (no hash, so no collisions), one mutex. |
| 3. Counters per 5 s, and the report snippet | Done. `ReportPipeGpl` in `vk_pipeline_cache.cpp`; call site given in section 6. |
| 4. Working subset, fallback elsewhere | Done. Rect/quad lists, barycentric geometry shader, discard fragment shader and preloads keep today's path. |

Behaviour with the switch on:
- A cold draw whose key is new, and whose state is linkable, gets the optimized build queued (as today) and is
  drawn at once with a fast-linked pipeline. The draw is no longer skipped.
- A draw whose key is already queued (entry null) and linkable is drawn with a fast-linked pipeline too.
- When the optimized build is collected (`CollectAsyncPipelines`), it replaces the linked entry. The linked pipeline
  is destroyed through `scheduler.DeferOperation`, so it is freed once the GPU is past the current tick.
- Pipelines are registered (`RegisterPipelineData`, `num_new_pipelines`) when the optimized build replaces them, not
  when linked, so the pipeline-cache blob holds the same keys as before.

## 2. Changes (file:function)

`gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`
- `Instance::CreateDevice`: `feature_chain` gains `vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT`. After the
  `VK_EXT_vertex_input_dynamic_state` line: reads `SOTC_PIPE_GPL` (`'1'`), the feature, then adds the two extensions
  (short-circuit: the KHR one only when the feature is there). Warning when the switch is on but not available.
- `device_chain`: the feature struct with `graphicsPipelineLibrary = true`, unlinked when not enabled.

`vk_instance.h`: `IsGraphicsPipelineLibrary()`, member `graphics_pipeline_library`.

`vk_graphics_pipeline.h`
- `class GplLibraryCache` (`enum Part { VertexInput, PreRaster, Fragment, FragmentOutput }`, `GetOrBuild`, one mutex,
  one `tsl::robin_map<std::string, vk::UniquePipeline>` per part).
- `struct GplCounters` and `inline GplCounters g_gpl_counters` (atomics).
- `GraphicsPipeline`: new trailing ctor parameter `GplLibraryCache* gpl = nullptr` (existing callers unchanged),
  `IsLinked()`, `static FastLinkable(key, infos, runtime_infos, profile)`, member `linked`.

`vk_graphics_pipeline.cpp`
- `GplKey` (anonymous namespace): appends the bytes of the key fields.
- `GplLibraryCache::GetOrBuild`: lookup or build under the mutex; a failed build is not stored.
- `GraphicsPipeline::FastLinkable`: false for RectList/QuadList (aux tess shaders from FS inputs), for a missing
  fragment shader that needs the discard shader (`clip_distance_emulation`), and for the barycentric geometry shader
  case (same test as the constructor, using `NeedsBarycentricGs`).
- Constructor: when `gpl && !preloading && FastLinkable(...)`, the state already computed is split into four
  `GraphicsPipelineCreateInfo`s, each with `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` (plus the descriptor-buffer flag
  when enabled, as the monolithic path). The keys:
  - VI: layout, dynamic state list, topology, vertex attributes/bindings/divisors (when not dynamic).
  - PreRaster: layout, dynamic states, module handles of VS/GS/TCS/TES, tess control points, prim type, polygon mode,
    provoking vertex, depth clamp/clip, clip space.
  - Fragment: layout, dynamic states, FS module handle, sample count and sample shading, z/stencil format, rendering
    info (color formats and samples, depth/stencil formats).
  - Output: layout, dynamic states, sample count and sample shading, logic op, blend attachments, rendering info.
  Stages split: the fragment module goes to the Fragment part, the rest to PreRaster. Then a link with
  `VkPipelineLibraryCreateInfoKHR` (four handles), no link-time-optimization flag. On a refused library or link the
  pipeline stays unbuilt (`linked` false) and the caller keeps the draw skipped.

`vk_pipeline_cache.h`: member `GplLibraryCache gpl_libs` (after `layout_cache`, so it is destroyed first),
`TryFastLink(std::unique_ptr<GraphicsPipeline>&)`, `ReportPipeGpl(std::string&, double)`.

`vk_pipeline_cache.cpp`
- `TryFastLink`: `FastLinkable` (counts `unlinkable` when false), then the linked constructor with `&gpl_libs`.
- `GetGraphicsPipeline`: `gpl_on` read from the instance. Cold-miss counter on `is_new || queued`. The queued path
  (`queued && num_indices > 6`) and the new-key path (before `QueueAsyncPipeline`'s return) call `TryFastLink`, and on
  success return the linked pipeline with the same memo updates as the normal return. The memo hit path counts linked
  draws.
- `CollectAsyncPipelines`: if the entry is a linked pipeline and the job is not preloaded, the job's pipeline replaces
  it (memo cleared, registered, `num_new_pipelines`, counter), and the old one is destroyed via `DeferOperation`.
- `ReportPipeGpl`: the report, inside `namespace Vulkan`, at the end of the file.

## 3. Counters (`SOTC_PIPE_GPL=1`)

One line per 5 s window, printed only when the window had cold misses, linked draws, replacements or failures:

`SOTC pipe gpl: cold misses C (not linkable U), linked draws L, links N (failed F, avg X ms), optimized replacements R; libraries built vi a pr b fs c fo d, reused vi ... pr ... fs ... fo ...`

| Field | Meaning |
|---|---|
| cold misses C | draws with no built pipeline for their key (new key, or a key still queued). Counted with the switch on, whether or not the draw is linked. Includes tiny draws and depth-only draws, which are still built synchronously as before. |
| not linkable U | cold draws for which `FastLinkable` said no (the draw is skipped as before). Counted per draw. |
| linked draws L | draws that returned a fast-linked pipeline (memo hits included). |
| links N, failed F, avg X ms | successful fast links, refused library or link creations, and the average `vkCreateGraphicsPipelines` time of the links (libraries not included). |
| optimized replacements R | optimized builds that replaced a linked entry. |
| libraries built / reused | per part: a library created by the driver, or found in the cache. A high reuse count with low builds is the expected steady state. |

## 4. Call-site snippets (files not owned)

`gpu/shadps4/core/libraries/videoout/driver.cpp`, inside `namespace Vulkan` (the block that declares
`ReportPipeMemo` / `ReportPipeDirty`):
- After line 94 (`void ReportPipeDirty(std::string& out, double seconds); // sotc: SOTC_PIPE_DIRTY`), add:
  ```
  void ReportPipeGpl(std::string& out, double seconds); // sotc: SOTC_PIPE_GPL
  ```
- After line 2033 (`Vulkan::ReportPipeDirty(report, seconds); // SOTC_PIPE_DIRTY`), add:
  ```
  Vulkan::ReportPipeGpl(report, seconds); // SOTC_PIPE_GPL
  ```
(The brief said about line 2025; the lines are 2032-2033 in the current file.)

No other call sites need changes. `vk_rasterizer.cpp` is not affected: `GetGraphicsPipeline` returns a non-null
pipeline for a linked draw, as it does for any built one.

## 5. How to test in game

1. Build as usual (not done here). Run one Lab route with `SOTC_PIPE_GPL=1` and `SOTC_FPS` on, following the standing
   rules (`user/routes/tests.json` for the route, `tools/pcmem_matrix.py --repeat 1` for a round).
2. Read the `SOTC pipe gpl` line. Expect: cold misses ~= the base run's skipped geometry draws, linked draws > 0,
   replacements close to the number of links once the workers catch up, failed = 0.
3. Read `SOTC SYNC PIPELINE` lines (stderr). Compare the number and time of the graphics sync builds with the base run.
   The fast link moves shader compiles of new VS/FS pairs onto the CP thread (section 7); the gain is in the cases where
   the libraries are reused.
4. Check `user/last_run.log` and the validation layers, if available, for VUIDs on `VkGraphicsPipelineCreateInfo`
   (dynamic state and rendering info in the link, descriptor-buffer flag on libraries). This is the first thing to check.
5. Compare the CP thread samples under `RefreshGraphicsKey` (unchanged) and the frame timers in the same route, with the
   switch off and on, three alternating runs each. Keep only if the draw-skip count drops and the CP stall does not grow.

## 6. Risks

- **The CP thread does the library compiles.** Before, a cold miss queued a worker compile and skipped the draw. Now
  the pre-raster and fragment libraries of a new VS/FS pair are compiled on the CP thread, which is the same driver
  work the monolithic sync build did (about 36 ms per build in the notes). The gain is only real when the VS or FS
  module is already known (the libraries are reused, so the cost is the VI/FO libraries plus a fast link). This is
  the main thing to measure. If most cold misses are new VS/FS pairs, the switch will stall the CP more than it saves.
- **Unverified Vulkan rules.** (a) `pDynamicState` is given to every library and to the link, with the same list.
  (b) `VkPipelineRenderingCreateInfo` is given to the fragment and output libraries, not to the link. (c) The
  descriptor-buffer flag is set on the libraries (as on the monolithic pipelines). (d) The VI library has a null
  `pVertexInputState` when vertex input is dynamic, as the monolithic path. (e) All libraries get the same layout, as
  a link requires. Any of these may be refused by a validation layer; (a) and (b) are the ones I am least sure of.
- **Memory.** Libraries are never evicted (the pipeline caches are not either). The PreRaster and Fragment parts
  hold the driver's compiled code, so the RAM report (`SOTC_RAM_REPORT`) may grow with the switch on. Not measured.
- **Replacement timing.** A linked pipeline stays in use until its optimized build is collected. Under load that can
  be a long time; the linked code is correct but may run slower than the optimized one.
- **Destruction.** The linked pipeline is freed via `scheduler.DeferOperation` (GPU tick), not immediately. This is the
  same mechanism as `DeferPriorityOperation` elsewhere in the scheduler, but it is new for graphics pipelines.
- **Shader-collect mode.** The fast-link path does not add to `module_related_pipelines`. The shader collect mode is
  already excluded from the async path (`AsyncIneligibleReason`), so the linked path is not reached in that mode.
  Not tested.
- **Blob contents.** A linked pipeline is not registered until its optimized build is collected. A game session that
  ends first saves nothing for it (correct, nothing built was lost).
- **Counters are per draw** for `unlinkable` and `linked draws`. Read the ratio with the cold misses in mind.

## 7. Not finished

- Step 1 of the p1.md plan (measure `LogSyncPipeline` per graphics build with a `SOTC_FPS` run before any code) was
  not done; this change is written without that number. Measure before keeping it.
- Not covered by fast-link (served by the old path, the draw skipped): RectList/QuadList, the barycentric geometry
  shader, the discard fragment shader, preloaded pipelines (boot preload), and the tiny-draw run of
  `SOTC_TINY_ASYNC=1` (which still uses the queue).
- The library cache lives in memory only. Nothing about libraries is written to the game's pipeline blob; the
  driver's own pipeline cache (`sotc_vk_pipelines.bin`) may still hold their compiles across a restart.
- `FastLinkable` is conservative: the barycentric case uses the same test as the constructor, but the discard-shader
  case is a whole exclusion.
- No eviction of libraries, no per-part counter for build time (only the link time).
- Nothing was built, linked or run. The validation layer result is unknown.
