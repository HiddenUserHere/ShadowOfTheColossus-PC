# 09 - Draw-thread freeze in ring-r2 (SOTC_HOOK_ALLOC_PROBE=1 SOTC_PC_RING=1)

Read-only investigation of `user/pcmem/20261101-032958/logs/ring-r2-a1.log`, compared with the other 15 logs in the same folder. No repository file was edited and nothing was built or run.

## Summary

- The 10.3 s wall-clock stall (`SOTCSTALL frame 316 10277 ms`) matches a present-path lock hold. The present thread held `Scheduler::submit_mutex` for 331 ms per present, 31 presents, about 10.3 s in total. Every non-draw thread that needed the mutex waited for it: `other threads 330.61 ms (1690)` per frame over the window.
- The draw thread's 6.7 s of thread CPU in that frame is **not explained** by any counter. Every instrumented draw wait reads zero (gpu, recorder, upload, tc). The draw thread's own submit-lock wait is 0.03 ms.
- The counters named in the brief (emergency collection, redone-sync readbacks, unmap GPU waits, view-split freeze, DMA sweeps, heap guard, DeclareCpuWrite volume) all fall inside the range of the other runs. None of them is the cause. The user's "emergency collections 1" and "view split freeze 29.39 ms" appear in every run.
- Most likely fix target: the `swapchain.Present()` call made while holding `submit_mutex` (`vk_presenter.cpp` around line 1565). A proposal is at the end; this report only describes it.

## How the log is laid out

- In these logs each window's lines are printed before that window's `SOTC FPS` header. The stall is in the batch that ends at `ring-r2-a1.log:462` ("SOTC FPS 2.7 at 21s").
- The 15 s and 20 s reports were skipped. The batch therefore covers about 10.0 s to 21.3 s, not one 5 s window. The line `SOTC FLIP WAIT per frame (31 frames in 11.3 s)` confirms this.
- The stall starts at the area load. `[00:00:09.958] Loading a save or a new area` (line 337) is followed by `Texture cleanup ... video memory in use 3013 MB`. ring-r1 has the same event at 10.477 s and no stall.
- `SOTCSTALL` is printed in `gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1712`. Its `cpu draw/cp/rec` fields are per-role thread CPU time (`cpu_ns`, the `BbThreadCpu` roles, read through `QueryThreadCycleTime` in `gpu/shim/bbport_thread_cpu.cpp`). `rec busy` is wall time inside recorder blocks (`rec_busy_ns`, `sotc_stats.h`). `draw waits` are wall times of `DrawWait` scopes.
- Env in the run: `SOTC_ENGINE_CMD=1`, `SOTC_HOOK_ALLOC_PROBE=1`, `SOTC_HOOK_FRAME_FLIP=1`, `SOTC_PC_RING=1`. `SOTC_WAIT_SPIN_US` and `SOTC_PC_RING_SLOW` are not set, so the fence spin in `vk_scheduler.cpp:394` and the posted declare path are both off.

## Numbers from the stall batch (ring-r2, lines 364-461)

| Counter | Value | Source |
|---|---|---|
| SOTCSTALL frame 316 | 10277 ms, `cpu draw 6714.4`, `cp 33.9`, `rec 33.2`, `rec busy 10.1` | line 446 |
| SOTCSTALL frame 317 | 239 ms, `cpu draw 167.9`, `rec busy 10264.7` | line 506 |
| draw waits (all kinds) | gpu 0.0, recorder 0.0, upload 0.0, tc 0.0 ms | lines 446, 453 |
| threads per frame | draw busy 364.89 ms (normal 33-40) | line 451 |
| cpu/frame | draw 247.66 ms (normal 32-36 in all other logs) | line 452 |
| recorder/frame | busy 340.65 ms (normal 7-11) | line 456 |
| FLIP WAIT | other threads 330.61 ms/frame (1690 waits); draw 0.03 ms | line 410 region |
| PRESENT | present thread under submit_mutex 331.00 ms per present (normal 0.3-0.7) | line 411 region |
| MUTEX SPIN (guest pthread mutex, `runtime_mutex.c`) | contended 10,208,346, fell 2480 | line 483 region |
| FAULTS | 10855 writes (normal 8000-24000 per 5 s); `avg 0.0 us` (fault timing not measured in this log) | line 427 |
| CP waits for draw | 6/s (normal 46-63/s) | line 436 region |

The per-frame "other threads" figure is per frame over 31 frames. 330.61 ms x 31 is about 10.25 s, which equals the stall. The "under it" figure is 331.00 ms x 31 is about 10.26 s, also the stall.

## Cross-run comparison (all 16 logs)

Worst values per log (windows of 5 s, except the stall batch):

| Log | worst SOTCSTALL (ms) | worst cpu draw/frame (ms) | emergency max | unmap GPU wait (ms) | view-split freeze (ms) | redone sync | DMA ms/s max | heap guard ms max | later-GPU-write max |
|---|---|---|---|---|---|---|---|---|---|
| ring-r2 (the bad run) | **10277** | **247.7** | 1 | 46.1 | 224.8 | 1033 | 67.7 | 250.8 | 9919 |
| ring-r1 | 167 | 35.0 | 1 | 51.8 | 232.5 | 1050 | 80.6 | 282.5 | 4310 |
| base-r1 / base-r2 | 175 / 146 | 35.6 / 34.5 | 1 / 1 | 37.0 / 45.7 | 216.8 / 206.6 | 1119 / 1370 | 50.8 / 51.5 | 208.6 / 233.2 | 4542 / 8740 |
| chunks2-r1 / r2 | 158 / 156 | 33.2 / 33.4 | 1 / 1 | 44.9 / 33.2 | 198.0 / 217.2 | 1826 / 1747 | 49.7 / 52.6 | 258.3 / 284.1 | 5086 / 9212 |
| poolnative-r1 / r2 | 175 / 175 | 34.9 / 35.4 | 1 / 1 | 35.5 / 38.0 | 201.8 / 207.9 | 1249 / 840 | 59.7 / 57.8 | 262.2 / 284.4 | 14259 / 10419 |
| probe-r1 / r2 | 180 / 182 | 32.8 / 33.5 | 1 / 1 | 44.8 / 39.2 | 220.9 / 231.9 | 1018 / 1116 | 46.7 / 47.9 | 208.1 / 221.7 | 8179 / 5420 |
| rb2-r1 / r2 | 125 / 151 | 34.7 / 34.4 | 1 / 1 | 49.7 / 44.3 | 215.7 / 221.0 | 259 / 241 | 56.4 / 52.8 | 219.6 / 230.5 | 4537 / 3923 |
| rt-r1 / r2 | 158 / 172 | 33.1 / 34.8 | 1 / 1 | 41.8 / 38.9 | 225.6 / 212.2 | 1787 / 1647 | 51.6 / 53.8 | 214.8 / 305.5 | 10001 / 6467 |
| static-r1 / r2 | 167 / 153 | 34.8 / 33.0 | 1 / 1 | 48.0 / 55.3 | 206.0 / 198.1 | 1794 / 1109 | 60.0 / 55.2 | 255.8 / 246.9 | 7412 / 9414 |

Conclusions from the table:

- Only ring-r2 has the 10 s stall and the 247 ms draw CPU per frame. Every other run stays at 33-36 ms.
- The cited counters are normal in ring-r2. Emergency max is 1 in all 16 logs. Unmap waits (46 ms) sit inside 33-55 ms. View-split freeze (29 ms in the stall batch) is far below the 198-232 ms worst values elsewhere. Redone-sync (878 in the batch) sits in 241-1826. DMA sweeps and heap guard are below the other runs' maxima.
- Note the stall batch's own values: DMA sweeps 43.6 ms/s over 11.3 s is about 0.5 s total, and heap guard 98.5 ms in the same batch. Neither can account for 10 s.

## Candidates, ranked by evidence

### 1. Present call under `submit_mutex` (strong, timing match)

- `vk_presenter.cpp:1565` takes `Scheduler::submit_mutex` with `std::scoped_lock` and then calls `swapchain.Present(present_id)` (`vk_presenter.cpp` line 1572 and `vk_swapchain.cpp:153`, `presentKHR`). The comment at `vk_presenter.cpp:1174-1177` says the present id is "taken under `submit_mutex` together with `swapchain.Present`, so they follow the queue order". So the lock is deliberate.
- In the bad run the lock was held for 331 ms per present. Normal runs hold it 0.3-0.7 ms per present. There were 31 presents, which is about 2.7 per second.
- The other submitters block on it. `LockSubmit` (`vk_scheduler.cpp:26-40`) classifies waits as "draw" when `t_draw_thread` is set and as "other" otherwise. "Other" is 330.6 ms per frame and 1690 waits. The recorder's async ahead submit takes the lock at `vk_scheduler.cpp:498-499`, which is consistent with the recorder's `rec busy 10264.7` in frame 317. The "other" bucket is not split by role, so the recorder is inferred from timing, not counted directly.
- Why `presentKHR` blocks for 331 ms is not in the logs. Possible causes are FIFO vblank wait under an occluded or non-vblanking window, the driver's present queue being full, or the driver serializing the present behind GPU work on the shared queue. The log has a "Mouse released" event at 24.3 s (after the stall) but no focus or resize event inside it.
- Gap: this explains the 10 s of wall time. It does not explain the draw thread's 6.7 s of CPU, because the draw thread's own lock wait is 0.03 ms.

### 2. Draw-thread CPU with no counted wait (unexplained, highest uncertainty)

- Thread CPU on the draw thread is 364 ms per frame. Every `DrawWait` bucket is zero, `CP waits for draw` drops from about 50/s to 6/s, and `PC RING apply` shows nothing unusual.
- Candidates that could burn CPU without a `DrawWait` scope:
  - A Vulkan driver call on the draw thread that the port does not wrap in a timer. The NVIDIA driver can spin inside a blocked call (`vkAcquireNextImageKHR`, `vkQueueSubmit`, `vkQueuePresentKHR`). Confirm with a call-stack sampler.
  - The write-fault path. There were 10855 faults in the window, which is in range, but fault timing reads `avg 0.0 us` in this log, so per-fault cost was never measured. The stall would need about 0.6 ms per fault. Confirm with fault timing on (`kFpsTimers`).
  - A busy spin in a page-manager or texture-cache loop with no timer. The only spin with a visible counter is `stack registry spins 2837`, which is in range. The fence spin in `vk_scheduler.cpp:394` is off, because `SOTC_WAIT_SPIN_US` is unset.
- Ruled out as the cause of the CPU itself: the DMA sweep, heap guard, memory lock and view-split counters are all measured and too small.

### 3. DeclareCpuWrite / ring declares on the draw thread (weak)

- Declares are 5337/s (in range: 4762-5772/s in ring-r1). `declared bytes 0.00 MiB/frame`.
- `PageManager::DeclareCpuWrite` (`page_manager.cpp:1749-1760`) posts to the draw thread through `RunOnDrawThread` when `RunsGpuWorkElsewhere()`. But the ring calls `DeclareRingWrite` by default (`pc_ring.cpp:511`) and only uses `DeclareCpuWrite` when `SOTC_PC_RING_SLOW=1`, which was not set. I did not read `DeclareRingWrite`'s body, so whether it can post is unverified. There is no counter for posted runs or their duration.

### 4. Ruled out

- **Emergency VRAM collection** (`texture_cache.cpp:2366`): one per run in all 16 logs, VRAM 3028 of 6532 MB, GC freed 0 in the stall batch. Not a 10 s walk.
- **Synchronous readback redo storm**: 878 redone (stale) in the batch, in range; the wait time for those is counted as GPU waits, which read zero.
- **FaultGrace / hot pages**: 52 marks, 56 KiB hot uploads, no cap hits. Normal.
- **Huge DMA sweep** (`SynchronizeDmaBuffers`): 122/s, 368 ranges each, 43.6 ms/s. Sum over the 11.3 s batch is about 0.5 s and in range with the other runs.
- **Image re-uploads**: 2 images at frame 316 (0 ms). Normal.
- **Unmap GPU waits**: 158 waits, 46.1 ms. In range.
- **View split freeze**: 29.4 ms, below the 198-232 ms worst values in the other runs.

## Counters that would confirm or reject each candidate

1. Present under lock:
   - Add a timer around `swapchain.Present` alone (not the whole lock), and check the existing `SOTC_TRACE` "present" span duration for the bad run.
   - Split the "other" lock wait by role (recorder, CP, present, other) inside `LockSubmit`.
   - Record vblank or display timing (`VK_GOOGLE_display_timing`, if available) for the window, and window occlusion or focus.
   - Confirm: `presentKHR` takes about 331 ms per call and the other-thread waits are in the recorder. Reject: `presentKHR` is fast and the time is spent elsewhere under the lock.
2. Draw-thread CPU:
   - Run the same config with `SOTC_PROFILE=GpuCommandProcessor` and `SOTC_PROFILE_OUT=<file>`, then `tools/profile_functions.py <file> out/gpu/bb-probe.exe 60 --inlines --qualified`. The sampler shows where the draw thread's CPU goes. Confirm: the top stacks are in a driver call or a loop; reject: the top stacks are in normal back-op code.
   - Turn on fault timing so `avg 0.0 us` becomes real. Confirm: per-fault cost of about 0.6 ms.
3. DeclareCpuWrite:
   - Count `RunOnDrawThread` posts from `PageManager::DeclareCpuWrite` and time them. Confirm: any post with a long run time. Reject: no posts during the window.

## Proposed fix for the most likely cause (described, not implemented)

Do not hold `Scheduler::submit_mutex` across `swapchain.Present`. Keep the present-id ordering with its own small mutex around `TagPresent` and `presentKHR`, and let queue submits take only `submit_mutex`. Before changing the lock, add the `presentKHR`-only timer from counter 1. If the present blocks for more than a bound (for example 50 ms), log it and drop back to the existing free-frame path rather than stalling every submitter.

This is the right first fix only if counter 1 confirms that `presentKHR` itself is slow. If the present is fast, the lock hold time is coming from elsewhere under the lock and the fix moves to that call. The draw-thread CPU (candidate 2) should be profiled in parallel, because moving the present does not by itself explain 6.7 s of draw CPU.

## Caveats

- `FLIP WAIT` "other threads" is every non-draw thread, not only the present and recorder. The per-role split is inferred.
- `cpu_ns` as thread CPU time is inferred from `QueryThreadCycleTime` in `bbport_thread_cpu.cpp`. The sampler (counter 2) would confirm it.
- The table comparisons use the worst window value per log. The stall batch is 11.3 s long while most other windows are 5 s, so per-second rates are the fair comparison for the DMA and lock rows.
