# N4 safe deletions, specialist n4present (present path, Vulkan object pools)

Uncommitted, worktree `agent-ab859a48351e36b77`. Each owned file was copied from the main tree first, then
edited. Line numbers below are the main tree's (before this edit). Line endings were kept per file: CRLF in
vk_scheduler.*, vk_instance.cpp, vk_presenter.*, vk_swapchain.*, image.cpp, vk_rasterizer.cpp; LF in
vk_instance.h and buffer.cpp (as in the main tree). buffer.h and image.h were copied but not edited (identical to
the main tree).

## Per switch

| Switch | Confirmed default (reader) | Action | Where (main-tree lines) |
|---|---|---|---|
| SOTC_PRESENT_UNLOCKED | 0. `PresentUnlockedMode()` returns 0 when unset, 1 for `1`, 2 for `2`, 0 otherwise. Nothing in launcher/, sotc.py, sotc.ini, packaging/, scripts/ or src/ sets it. Only tools/pcmem_matrix.py presets set it (drawfast, drawverify, drawall), which are non-default and are listed below for the coordinator. | Modes 1 and 2 removed. Mode 0 kept: the present runs under `Scheduler::submit_mutex`, one VkQueue, no depth wait, no timer. | See below |
| SOTC_VK_POOL | off. `VkPoolEnabled()` is true only for `1`. Nothing in the port sets it except the tools/pcmem_matrix.py presets drawfast (line 132) and drawall (line 182). | Removed. The default path is kept everywhere: deferred destroy is the inline destroy, BDA buffers keep the dedicated allocation, and the image pool is gated only by SOTC_IMG_POOLED / SOTC_IMAGE_POOL_MB. | See below |
| SOTC_LOD_FB_EVERY | unset = 4 (`static const u32 every`, with `n ? n : 1`). Nothing outside vk_rasterizer.cpp mentions it. | Env read removed. Default kept as `constexpr u32 kLodFeedbackEvery = 4u;`. | vk_rasterizer.cpp:220-228 |

### SOTC_PRESENT_UNLOCKED: removed code (main-tree lines)

- vk_scheduler.h:513-529: `PresentUnlockedMode()` (the only reader), with its doc comment and blank line.
- vk_instance.cpp:756-829: the whole path selection. Kept: queue count 1, one graphics queue, `queue_priorities`
  now `{1.0f}`. Removed: `kNoPresentFamily`, `separate_present_queue`, `find_present_family` (probe surface and
  family scan), `other_present_family`, the PresentId/PresentWait feature probe that set `present_depth_wait`,
  the "no second queue" warning, the `present_queue_info` entry (lines 840-846) and its push (850-852).
  `queue_infos` capacity 3 -> 2 (graphics and transfer families only).
- vk_instance.cpp:1179: device-creation retry now tests `HasXferQueue()` only; the `other_family_present` block
  (1186-1191) removed.
- vk_instance.cpp:1206-1240: the present queue is `device->getQueue(queue_family_index, 0u)` (the mode-0 branch),
  the log line for the path is kept. Its text lost the `SOTC_PRESENT_UNLOCKED=%d` clause, and the switch over
  PresentPath became one fprintf (see Notes).
- vk_instance.h:69-85: `GetPresentQueueFamilyIndex()` and the `PresentPath` enum with `GetPresentPath()`
  removed. Their only callers were vk_swapchain.cpp:49 (changed, see below) and vk_instance.cpp.
- vk_instance.h:437-442: `IsPresentDepthWaitEnabled()` removed (only caller: vk_presenter.cpp:576).
- vk_instance.h:779-780, 830: members `present_queue_family_index`, `present_path`, `present_depth_wait` removed.
- vk_instance.h:424-426 and 406-408 and 414: comments naming the removed mode reworded. The
  `present_wait_enabled` flag is now set only by SOTC_PRESENT_WAIT=1 (vk_instance.cpp:600-615), as before this
  change for the default.
- vk_presenter.cpp:559-577: the PRESENT_UNLOCKED init block and `present_depth_wait` assignment removed.
- vk_presenter.cpp:1271-1294: `Presenter::WaitPresentDepth()` removed (callers: Present and the second present
  path, both removed). `WaitPresented()` stays (used by driver.cpp:2596, 2655 and by SOTC_PRESENT_WAIT).
- vk_presenter.cpp:1621-1624, 1627-1628, 1630-1636, 1650-1652, 1660-1663: in `Presenter::Present`, the depth
  wait and the `present_queue_mutex` branch removed. `submit_mutex` is locked directly, as the mode-0 `else` branch
  did. The recreate after a failed present is still under `submit_mutex`.
- vk_presenter.cpp:1949-1968: the second present function: same simplification (depth wait and queue lock removed,
  `submit_mutex` held over `swapchain.Present()` and the recreate, as in mode 0).
- vk_presenter.cpp:62 (`depth_wait_ns`, `depth_wait_count`, `depth_wait_timeouts`) and 2209-2222 (the
  `SOTC PRESENT DEPTH WAIT` report line) removed. Only WaitPresentDepth used them.
- vk_presenter.h:196-200 (`WaitPresentDepth` declaration and comment), 252-258 (`present_unlocked`,
  `present_depth_wait`, `present_queue_mutex`) removed. vk_presenter.h:192-193 comment reworded.
- vk_scheduler.h:513-529: see above.
- vk_swapchain.cpp:47-58: the two-family `queue_family_indices` array and the concurrent/exclusive choice removed.
  The swapchain is `eExclusive` over `GetGraphicsQueueFamilyIndex()`, which is what mode 0 computed
  (both families were equal, so `exclusive` was true and `queueFamilyIndexCount` was 1). Lines 74-76 use the
  single index.
- vk_swapchain.cpp:158-160: `time_present = SotcStats::kFpsLog` (the mode-2 timer removed; the SOTC_FPS timer
  is unchanged). vk_swapchain.h:39 comment reworded.

### SOTC_VK_POOL: removed code (main-tree lines)

- vk_scheduler.cpp:25-51: the anonymous namespace (`pool_destroy_scheduler`), `VkPoolEnabled()`,
  `GpuDestroyDeferred()`, `DeferGpuDestroy()`. With the switch off, `GpuDestroyDeferred()` was always false, so
  every caller ran its destroy inline; that is now written inline.
- vk_scheduler.cpp:102-106 (constructor registration) and 168-169 (destructor unregistration) removed.
- vk_scheduler.h:866-878: the declarations of `VkPoolEnabled`, `GpuDestroyDeferred`, `DeferGpuDestroy` and their
  doc comment removed.
- buffer.cpp:140-155 (`UniqueBuffer::Destroy`): the deferred branch and its lambda removed. The ReBAR bytes are
  released after `vmaDestroyBuffer`, as the default path did (`const u64 rebar = rebar_bytes; rebar_bytes = 0;`).
- buffer.cpp:170-176 (`UniqueBuffer::Create`): `bda_flag` is `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` for every
  BDA buffer (the default, since `VkPoolEnabled()` was false). Comment reworded.
- image.cpp:111-121 (`UniqueImage::Destroy`): deferred branch and lambda removed, `vmaDestroyImage` inline (default).
- image.cpp:130-131, 150, 156, 185: comments reworded. `ImagePool()` now uses `ImgPooledEnabled() ? 256 : 0`
  (was `VkPoolEnabled() || ImgPooledEnabled()`). `SOTC_IMG_POOLED=1` keeps its behaviour without any VK_POOL.
  `SOTC_IMAGE_POOL_MB` is unchanged.

### SOTC_LOD_FB_EVERY: removed code

- vk_rasterizer.cpp:220-228: the env reader (`static const u32 every`) replaced by
  `constexpr u32 kLodFeedbackEvery = 4u;`. The cycle test is `++sotc_lod_cycles % kLodFeedbackEvery == 0`. The
  default (4, env unset) is unchanged. The env override is gone; no run in the repo sets it.

## Kept, and why

- SOTC_IMG_POOLED and SOTC_IMAGE_POOL_MB: kept, they are live (switch-audit and tools/pcmem_matrix.py dp_img).
- `ImgPooledEnabled()` still gates the image-pool allocation fallback (image.cpp:201, 668): unchanged.
- `PresentPath`-independent code: `present_queue`, `GetPresentQueue()`, `Swapchain::present_khr_*` (used by
  SOTC_FPS), `WaitPresented()`, `present_lock_ns` and the `SOTC PRESENT` report line: all used in mode 0.
- The `SOTC PRESENT: path shared-queue (...)` stderr line is kept (it prints in the default config). Its text lost
  the `SOTC_PRESENT_UNLOCKED=%d` clause. Nothing in the repo greps for that text.

## Behaviour changes to note

- `SOTC_PRESENT_UNLOCKED=1|2` and `SOTC_VK_POOL=1` are now no-ops. A run that sets them gets the default path.
  The tools/pcmem_matrix.py presets that set them (listed below) no longer measure anything different, so the
  coordinator should drop the keys (or the presets).
- With `SOTC_VK_POOL=1` the image pool (256 MiB) is no longer on by default. `SOTC_IMG_POOLED=1` still turns it on.
- The swapchain no longer has a concurrent-sharing case. Only the present-family path (mode 1 OtherFamily) used it.

## Syntax check

Compiled with `-fsyntax-only` (clang++ from `C:/msys64/clang64/bin`, the main tree's compile_commands.json flags,
with `-I` for the overlay's gpu/shim then gpu/shadps4 first). The worktree base is stale, so the overlay is the
main tree's gpu/shim and gpu/shadps4 with the 13 owned files from the worktree on top (the overlay is in the
scratchpad, not the repo). Result, all PASS with no diagnostics:

- renderer_vulkan/vk_scheduler.cpp
- renderer_vulkan/vk_instance.cpp
- renderer_vulkan/vk_presenter.cpp
- renderer_vulkan/vk_swapchain.cpp
- buffer_cache/buffer.cpp
- texture_cache/image.cpp
- renderer_vulkan/vk_rasterizer.cpp

A first run with a stale header set (worktree base) showed errors in vk_rasterizer.cpp and vk_presenter.h. Those
were the stale base, not the edits (the overlay run has none). One real error in vk_presenter.h (a range cut that
took too much) was found and fixed before the final run.

Not done: no build with ninja, no game run (per the brief).

## Grep before and after

- Before (main tree, code only): SOTC_PRESENT_UNLOCKED, SOTC_VK_POOL, SOTC_LOD_FB_EVERY, PresentPath and its
  values, PresentUnlockedMode, VkPoolEnabled, GpuDestroyDeferred, DeferGpuDestroy, present_queue_mutex,
  present_depth_wait, WaitPresentDepth, depth_wait_*, OtherFamily, SharedDepthWait, SecondQueue, GetPresentPath,
  IsPresentDepthWaitEnabled, GetPresentQueueFamilyIndex: only in the owned files and tools/pcmem_matrix.py.
- After (worktree, excluding .git, out, dist, third_party): no hits for any of them.

## Snippets and lines for files I do not own (for the coordinator)

No code snippet is needed outside my files: the removed names had no other C++ caller. The rows and lines below are
for the coordinator to apply. I did not edit them.

tools/pcmem_matrix.py (the presets set the removed keys; drop them or the presets measure the default):
- line 132: `'drawfast': {'SOTC_VK_POOL': '1',`
- line 133: `'SOTC_PROTECT_BATCH': '1', 'SOTC_PRESENT_UNLOCKED': '1'},`
- line 135: `'SOTC_PRESENT_UNLOCKED': '2', 'SOTC_CMD_STATS': '1'},` (drawverify)
- line 182: `'drawall': {'SOTC_VK_POOL': '1',`
- line 183: `'SOTC_PROTECT_BATCH': '1', 'SOTC_PRESENT_UNLOCKED': '1',` (drawall)

launcher/: no row names any of the three switches (grep clean). No LIVE entry in launcher/sotc_tuning.py.

Docs naming the removed switches (not edited):
- docs/re-stutters/reports3/draw/switches-review.md:4-5 (summary list), 15 (SOTC_VK_POOL row), 17
  (SOTC_PRESENT_UNLOCKED row), 28 (mode 2 of SOTC_PRESENT_UNLOCKED), 79-80 (VK_POOL section heading and text),
  110-117 (PRESENT_UNLOCKED section), 166 (DeferGpuDestroy counter suggestion).
- docs/re-stutters/reports3/draw/wave1/t1.md:6 (`SOTC_IMG_POOLED=1 ... as with SOTC_VK_POOL=1`). The image-pool
  statement is still true for SOTC_IMG_POOLED=1 but the comparison is no longer valid.
- docs/re-stutters/reports3/draw/wave3/switch-audit.md:77 (row 28, SOTC_PRESENT_UNLOCKED), 505 (SOTC_LOD_FB_EVERY
  row, now with its reader removed), 594 (SOTC_PRESENT_UNLOCKED reader row), 767 (SOTC_VK_POOL reader row). These are
  the audit's own rows: mark them deleted in the N4 audit update.
- CHANGELOG.md, README.md, docs/sotc/*: none of the three switches is named there (grep clean), so no edit.

## Not done / remaining

- Nothing committed. Worktree diff: only the 11 files listed above differ from the main tree; buffer.h and image.h
  are identical to the main tree. (They were copied and not edited.)
- The `SOTC PRESENT` path log text changed (see Kept). No doc in the repo quotes the old text.
