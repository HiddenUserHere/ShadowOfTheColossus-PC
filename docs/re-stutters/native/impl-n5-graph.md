# N5 graph: pass and hazard model (SOTC_N5_GRAPH=1 observe, =2 verify)

Status: part (a) observe and verify implemented, syntax-checked. Part (b) parallel recording not started.
Nothing is committed. No functional change: the old per-op barrier path runs unchanged.

## Model
- Pass: a maximal run of graphics ops with the same render-target identity (`N5TargetKey`: color and depth
  surfaces by address, format, pitch, height, slices), or of compute ops. Command-processor ops (Class::Other)
  end a pass.
- Hazard group: a run of ops whose footprints (AmdGpu::OpFootprint, prep slot) do not conflict
  (AmdGpu::Conflicts) with the union of the group. A barrier is predicted before the first conflicting op,
  before every op after an unknown footprint, and when the group exceeds 64 ranges. Groups cross pass
  boundaries (a target read by the next pass is a normal range conflict).
- Verify: barrier events are counted per thread (thread_local). An op with events and no predicted barrier is
  "missed". Logs the first 24 misses in mode 2.

## Order argument
Nothing reorders or defers work in either mode. The graph reads the op sequence as the draw thread executes it
(N5Begin/N5End bracket every ExecuteBackOp, including express-lane ops, which run on the draw thread in seq
order). GPU order equals op sequence order because the recorder is unchanged.

## Files changed (worktree, uncommitted)
- `gpu/shadps4/video_core/renderer_vulkan/vk_frame_graph.{h,cpp}` (new): Sample, Enabled/Verify, NoteBarrier,
  BarrierEvents, AddOwnerNs, Record (passes, hazard groups, counters), Report.
- `gpu/shadps4/video_core/amdgpu/liverpool.cpp`: `N5TargetKey` (static), `Liverpool::N5Begin`, `Liverpool::N5End`
  (new, after NoteOpOverlap); `n5_on` set at init (`Enabled() && back_threaded`); `fp_fill` includes `n5_on`;
  ExecuteBackOp calls N5Begin before `if (ovl_on)` and N5End after the switch.
- `gpu/shadps4/video_core/amdgpu/liverpool.h`: include vk_frame_graph.h; members `n5_on`, `n5_cur`, N5Begin, N5End.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: include; `NoteBarrier()` before the 7
  `pipelineBarrier2` sites (barrier code only).
- Hooks: no native hook changes (N5 is not a memory hook, so no hooks.d record).
- Note: `gpu/shadps4/video_core/sotc_stats.h` and `gpu/shadps4/core/libraries/videoout/driver.cpp` were copied
  from main as bases (unchanged by me). `gpu/sotc/native/`, the docs, and the rest of the copies were also
  copied from main. Only the files above are my changes: restrict `git diff` to them.

## Snippets for files I do not own (required for verify to be complete)
1. `renderer_vulkan/vk_runtime.cpp`, `Runtime::FlushBarriers()`: this is the main per-op barrier emitter
   (Transit / AccessBuffer / IsBufferAccessed all end here). Add the call before `scheduler.EndRendering();`:
   ```cpp
   Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
   ```
   Add `#include "video_core/renderer_vulkan/vk_frame_graph.h"`. Without this, verify reports almost no
   barrier events and the "missed" count is meaningless.
2. `renderer_vulkan/vk_runtime.cpp` line ~487 (`cmdbuf.pipelineBarrier2`) and `buffer_cache/buffer_cache.cpp`
   ~1722/1742/4640/4653 (`ahead_cmdbuf`/`cmdbuf.pipelineBarrier2`): the same one-line call before each.
3. `driver.cpp`, in the 5 s report next to `SotcNative::Report(report, seconds);`:
   ```cpp
   Vulkan::FrameGraph::Report(report, seconds); // sotc: SOTC_N5_GRAPH
   ```
   (`#include "video_core/renderer_vulkan/vk_frame_graph.h"`). Report needs the frame count from
   `SotcStats::flip_count`, which it reads itself.

## Counters (one line per 5 s window, `N5 graph (SOTC_N5_GRAPH=n): ...`)
frames, ops/f, passes/f (gfx, compute), ops/pass, same-target restarts/f (after compute, after other),
barriers/f old (in ops), graph (predicted + outside-op), predicted, missed, unknown footprints, command-processor
ops/f, owner ms/f, workers 0.

## Required runs (not executed: no game, no build here)
- `SOTC_NATIVE=1 SOTC_PARALLEL_RECORD=1 SOTC_N5_GRAPH=1`: read ops/pass, restarts, unknown footprint share.
  Unknown footprint share must be near zero for the numbers to mean anything; without
  SOTC_PARALLEL_RECORD=1 every op is a barrier point.
- `SOTC_NATIVE=1 SOTC_PARALLEL_RECORD=1 SOTC_N5_GRAPH=2` on the attract and shrine scenes: "missed" must be
  zero per window before any barrier elision is considered. Inspect the logged missed ops first.

## Verify and syntax checks done
- Syntax-only (clang, the main tree's flags, shim before shadps4 in the include path, source tree =
  main's gpu/ with the changed files): `vk_frame_graph.cpp`, `vk_rasterizer.cpp`, `liverpool.cpp` pass.
- Not done: any game run, any build, measured counts.

## Not done / remaining
- Part (b), parallel recording: not started. The per-op rasterizer state (BindBuffers/BindTextures bind
  caches, descriptors, runtime barrier and layout state) is shared with the owner and not separable without a
  rewrite of those paths. Start with passes that have read-only resolution only once verify shows missed = 0.
- No functional barrier elision: the old path also maintains image layout state, so elision must come after
  verify. Same-target render-pass restarts are measured but not merged: merging across compute or
  other ops would reorder, which the brief forbids. Restarts after a barrier flush are a separate target
  for vk_runtime.
- Limit: footprints exist only with SOTC_PARALLEL_RECORD=1 (prep threads). Draw-thread inline footprints are
  not computed.
- WAW counts as a conflict (conservative). Predicted barriers are therefore an upper bound for in-pass color
  writes.
