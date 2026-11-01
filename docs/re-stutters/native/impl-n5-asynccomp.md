# N5 async compute: rule, queue plumbing and classifier (SOTC_N5_ASYNC_COMPUTE=1 plumbing, =2 verify)

Status: queue probe and pick, fallbacks, and the mode-2 classifier implemented; syntax-checked only.
Nothing is committed. No game was run, nothing was built. **No dispatch is moved to the async queue
in any mode.** The move is the open part (section 7). Mode 0 and SOTC_NATIVE without the switch behave
as before.

## 1. Independence: the rule

Model (same ops and footprints as the N5 graph, `impl-n5-graph.md`):
- A **compute pass** is a maximal run of consecutive `Class::Compute` ops of one frame (dispatches from
  the draw thread's back ring, including ASC dispatches, which are queued there in op order).
- Footprint: `AmdGpu::OpFootprint` (reads, writes, `known`), 4 KiB-rounded guest ranges. An unknown
  footprint (no prep threads, or the shaders touch memory no range names) blocks everything.
- Conflict: `AmdGpu::Conflicts(a, b)`: some write of one overlaps a read or write of the other, or either
  footprint is unknown (RAW, WAR, WAW).

A compute pass P is **independent** (would move to the async queue with no wait before it) iff:
- (I1) P's footprint is known; and
- (I2) no earlier op of the same frame that stayed on the graphics queue conflicts with P. Concretely:
  P's reads overlap no earlier non-moved write, P's writes overlap no earlier non-moved read or write,
  and no earlier unknown op of the frame stayed on graphics.

Earlier moved passes do not count against (I2): they are on the same in-order async queue and run before P.

A pass that fails (I1) is "unknown". A pass that fails (I2) is "needs a submit split": it would move if
the graphics tick is split before it (so its inputs are on the GPU timeline), but the rule does not move it.

**Joins.** An independent pass P joins at the first later op that either conflicts with it, or is
`Class::Other` or unknown. The join is a graphics-side timeline wait. A compute pass that stays on graphics
and conflicts with P is also a join. A pass with no join runs to the frame end, where the frame boundary
is the sync. The window of P is the ops between its end and its join; the graphics ops in it that do not
conflict with P are the overlap candidates.

**How the graph computes it** (`vk_frame_graph.cpp`, `AsyncObserve`, called from `Record` when
`AsyncVerify()`):
1. A frame change (`SotcStats::flip_count`) closes any open pass and joins every pending pass at the
   frame end.
2. A compute op extends the open pass (footprint union). An unknown compute op makes the pass unknown.
3. A non-compute op closes the open pass: unknown -> `unknown`, joins all pending passes, and blocks later
   passes (`barrier_seen`); known and conflicting with an earlier non-moved access -> `needs split`, joins
   the pending passes it conflicts with, and its ranges go into the non-moved access list; otherwise it is
   **independent** and joins the pending list.
4. Every later non-compute op joins the pending passes it conflicts with (or all of them when it is
   `Other` or unknown), and adds one window op (and one window graphics op when it does not conflict) to
   the others. Compute ops add a window op only.
5. The non-moved access list is the set of ranges of graphics and other ops and of passes that stayed on
   graphics, for the frame. Sampled frames only (`SOTC_N5_ASYNC_EVERY`, default 8).

Cost: the non-moved access scan is O(passes x accesses) per sampled frame and the pending join check is
O(ops x pending). The owner time in the graph line includes it. Verify only.

## 2. Queue family ownership

- Buffers: CONCURRENT over {graphics, the async family}, the same choice the upload path makes for
  {graphics, transfer} in `buffer.cpp` (`vk_queues.h`). No release/acquire pair is needed, and the
  ordering comes from the timeline semaphores plus the consumer's barrier. The sharing list must include
  the async family index when the queue exists (snippet in section 8, `buffer.cpp` not edited here).
- Images: EXCLUSIVE would need a release on the producing queue and an acquire on the consuming one, plus
  the layout transition, at each join. The graph does not know image layouts; the runtime does. Images
  stay on graphics until the footprint says which ranges are images (today `OpRange` carries no kind; a
  `bool image` in `OpRange` would be the minimal change, owned by the footprint specialist).
- Cost, not measured here: CONCURRENT buffers avoid ownership transfers on all three targets; some drivers
  may take a slower path for CONCURRENT images (compression metadata), which is one more reason images
  stay out. NVIDIA, AMD and Intel Arc behaviour must be measured with the verify run before the move.

## 3. The ASC rings

- Yes, SotC submits compute through the ASC rings: `Liverpool::ProcessCompute` runs on the ASC coroutines
  (`asc_queues[{vqid}]`), and `DispatchDirect`/`DispatchIndirect` there call `SubmitDispatch`, which queues
  `BackOp::Kind::Dispatch` / `DispatchIndirect` on the same back ring the graphics ops use
  (`liverpool.cpp` ~4906-4954 and ~1594). The draw thread runs them in ring order, so `N5Begin` sees ASC
  dispatches in op order and the classifier needs no special case.
- The natural candidates are the ASC dispatches: the game already runs them as independent compute work
  (`docs/re-stutters/reports/02-gpu-cpu-sync.md` item 3 notes the compute ring full retry). Being on an ASC
  ring does not make a dispatch independent; the footprint does. The graph does not tag the source of a
  dispatch (`Sample` has no ASC flag), so the report cannot yet split ASC from gfx-ring compute. Adding that
  needs a flag on `BackOp` set in `SubmitDispatch` (liverpool.cpp, not edited here).
- Name collision: the driver log "SOTC ASYNC COMPUTE built N" (driver.cpp ~2331) counts shader pipeline
  builds on worker threads. It is not a GPU queue.

## 4. Probe and fallbacks (vk_queues.cpp, at device creation)

- `PickAsyncQueue(families, xfer_family)`: the first family with COMPUTE, no GRAPHICS, and >= 1 queue,
  other than the upload family. If none, the upload family's queue 1, when it has >= 2 queues
  (`shares_xfer`, the case of a single C+T family that the upload path also uses).
- `LogAsyncPick`: once, `SOTC N5 ASYNC: mode M, async compute queue family F index I ...`, or
  `no compute-only queue family (of N)`.
- Fallbacks, each logged once: `NoAsyncFamily` (compute stays on graphics, the default when no family),
  `AsyncQueueRefused` (device creation refused the extra queue; the retry is graphics-only).
- `AsyncMode()`: SOTC_N5_ASYNC_COMPUTE 0/1/2, read once, 0 unless `SotcNative::On()`.

## 5. Sync design (for the move; not active)

- Graphics -> async (flush before P): an async submit waits for the graphics timeline value of the newest
  tick that holds a producer or a conflicting access. Needs the producer's tick submitted first.
- Async -> graphics (join): the graphics submit that holds the join waits for the async timeline value of
  the newest independent pass that joins there (one value per submit: the async queue is in order).
- Ordering argument: the CPU submits in op order, so every signal value is queued before a submit that
  waits on it. Async work waits only on earlier graphics ticks and graphics only on earlier async values,
  so the waits form no cycle. Frame end: no wait, the frame boundary already submits.
- Wait slots: `Scheduler::SubmitInfo` has 4 wait slots (`vk_scheduler.h` `wait_semas`); the upload queue
  uses one (`SubmitExecution`, `TakePendingTick`). An async wait takes one more; on overflow the existing
  upload path waits on the CPU. The move must do the same and count it.
- Submit splits: a wait gates the consumer stage of the submit that holds the join, so ops before the join
  in that submit would be held too. The tick must be split at each join to keep them free. The classifier
  counts these as `submit splits/f` (= timeline waits/f in the line below).

## 6. Counters (`N5 async (SOTC_N5_ASYNC_COMPUTE=2, ...)` line, 5 s window)

Fields: sampled frames, compute passes/f, independent /f, would move after one split /f, unknown fp /f,
joins/f by kind (graphics, unmoved compute, other/unknown, frame end), window ops per joined pass, zero-
window share (joins where the join was the next op), graphics ops in windows/f (overlap candidates),
timeline waits/f, submit splits/f.

"moved 0" is printed on every line. Overlap is counted as candidates, not as GPU time. A measured GPU
overlap needs timestamps on the async queue (`SOTC_TRACE_GPU` exists, but its pairs cost queue time), so
that is a separate step. Mode 1 prints the pick and creates the queue; it has no per-window counters.

## 7. The move: what is missing (not implemented, not safe without a verify run)

Moving a dispatch means recording it into a second command buffer with its own bind state. Today's
compute path shares the scheduler's command buffer:
- `Rasterizer::DispatchDirect` / `DispatchIndirect` (`vk_rasterizer.cpp` ~2287, ~2461) call
  `scheduler.EndRendering()` and record on the scheduler's command buffer;
- the inbound barriers come from `Runtime::FlushBarriers` and the buffer cache's barrier sites, all into
  the scheduler command buffer (the graph's `NoteBarrier` sites, `impl-n5-graph.md`);
- descriptor and bind state (`BindBuffers`, `BindTextures`) is per rasterizer, not per queue.
Remaining work, in order: (1) a `vk_async_compute` class like `XferQueue` with its own pool, timeline and
slots; (2) a second recording path for independent passes, with barriers recorded on that command buffer;
(3) the join and flush waits in `SubmitExecution`; (4) CONCURRENT buffer sharing with the async family;
(5) verify: the mode-2 counters must show `unknown` share near zero and a verify run with no hangs and
no image mismatches. Only then =1 moves dispatches. Until then `SOTC_N5_ASYNC_COMPUTE=1` only creates the
queue.

## 8. Snippets for files this task does not own

1. `renderer_vulkan/vk_instance.h`, next to `SotcQueues::Probe n5_probe{};`:
   ```cpp
   SotcQueues::AsyncPick async_pick{};  // sotc: SOTC_N5_ASYNC_COMPUTE (vk_queues.h)
   vk::Queue async_queue{};             // sotc: valid when async_pick.family != SotcQueues::NoFamily
   ```
2. `renderer_vulkan/vk_instance.cpp`, probe block (~line 737): change the guard to
   `if (SotcQueues::On() || SotcQueues::AsyncOn()) {` and after `LogProbe(...)`:
   ```cpp
   if (SotcQueues::AsyncOn()) { // sotc: SOTC_N5_ASYNC_COMPUTE, one compute queue
       async_pick = SotcQueues::PickAsyncQueue(infos, xfer_queue_family_index);
       SotcQueues::LogAsyncPick(async_pick, static_cast<uint32_t>(infos.size()));
   }
   ```
3. `vk_instance.cpp`, queue create infos (~756-772): the upload family asks for 2 queues when it shares,
   and the async family gets its own info otherwise:
   ```cpp
   static constexpr std::array queue_priorities = {1.0f};
   static constexpr std::array two_priorities = {1.0f, 1.0f};
   const bool async_shares_xfer = async_pick.family != SotcQueues::NoFamily &&
                                  async_pick.shares_xfer && HasXferQueue();
   const vk::DeviceQueueCreateInfo xfer_queue_info = {
       .queueFamilyIndex = xfer_queue_family_index,
       .queueCount = async_shares_xfer ? 2u : 1u,
       .pQueuePriorities = two_priorities.data(),
   };
   const vk::DeviceQueueCreateInfo async_queue_info = {
       .queueFamilyIndex = async_pick.family,
       .queueCount = 1u,
       .pQueuePriorities = queue_priorities.data(),
   };
   boost::container::static_vector<vk::DeviceQueueCreateInfo, 3> queue_infos{queue_info};
   if (HasXferQueue()) {
       queue_infos.push_back(xfer_queue_info);
   }
   if (async_pick.family != SotcQueues::NoFamily && !async_shares_xfer) {
       queue_infos.push_back(async_queue_info);
   }
   ```
4. `vk_instance.cpp`, the retry (~1098): extend the condition to
   `if (device_result != vk::Result::eSuccess && (HasXferQueue() || async_pick.family != SotcQueues::NoFamily)) {`
   and before the retry: `if (async_pick.family != SotcQueues::NoFamily) { SotcQueues::NoteFallback(SotcQueues::Fallback::AsyncQueueRefused); async_pick = SotcQueues::AsyncPick{}; }`
   (the retry already drops all extra queues: `queueCreateInfoCount = 1u`).
5. `vk_instance.cpp`, after `xfer_queue = device->getQueue(...)` (~1133):
   ```cpp
   if (async_pick.family != SotcQueues::NoFamily) { // sotc: SOTC_N5_ASYNC_COMPUTE
       async_queue = device->getQueue(async_pick.family, async_pick.queue_index);
   }
   ```
6. `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, line 344 (the graph's enable, so the classifier's Record
   hook runs):
   ```cpp
   n5_on = (Vulkan::FrameGraph::Enabled() || Vulkan::FrameGraph::AsyncVerify()) && back_threaded;
   ```
7. `gpu/shadps4/core/libraries/videoout/driver.cpp`, after line 2030
   (`Vulkan::FrameGraph::Report(report, seconds); // sotc: SOTC_N5_GRAPH`):
   ```cpp
   Vulkan::FrameGraph::ReportAsync(report, seconds); // sotc: SOTC_N5_ASYNC_COMPUTE=2
   ```
   (`vk_frame_graph.h` is already included there for `Report`.)

## 9. Verify runs (not executed)

- `SOTC_NATIVE=1 SOTC_PARALLEL_RECORD=1 SOTC_N5_ASYNC_COMPUTE=2` on the attract and Shrine scenes. Read the
  `N5 async` line with the `N5 graph` line: the classifier is only meaningful where `unknown fp` (graph)
  is near zero. Without `SOTC_PARALLEL_RECORD=1` every op is unknown and the counts say nothing.
- `SOTC_N5_ASYNC_EVERY=1` for a full-frame count on one scene; the default 8 for steady-state numbers.
- Expected by design: `moved 0`. A non-zero `independent` with `zero-window` near 100% means no overlap
  is available and the move would gain nothing.

## 10. Not done

- The move itself (section 7).
- Image ownership (section 2) and the `OpRange` image kind.
- Host and upload writes are not in the op footprints, so the rule cannot see them. An upload copied before
  a compute pass is an input the classifier does not know about. The buffer cache's upload sites would need
  an `AsyncNoteExternal(range)` call before the move is enabled. This is the largest gap in (I2).
- ASC source tag on `Sample` (section 3).
