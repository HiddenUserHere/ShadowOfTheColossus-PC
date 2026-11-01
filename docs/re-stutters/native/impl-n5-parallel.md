# N5 part (b): parallel recording of graphics passes

Status: state map done, pass recording already exists (SOTC_PASS_SEC, one worker), counters added
behind SOTC_N5_PARALLEL=1. The multi-worker pool and pass look-ahead are NOT implemented (see
"Not done"). Nothing is committed. Nothing was built or run (no game, no ninja).

## Summary

The part (b) design in N5-BRIEF is already the recorder's design. The draw thread (producer) never
hands raw rasterizer state to a worker: every Vulkan call of a draw becomes a closure that copies its
arguments into the command stream (CommandRecorder::Run / RunState / AllocateCommand, vk_recorder.h).
The recorder's consumer thread replays that stream. For SOTC_PASS_SEC the consumer hands a dynamic
rendering pass to a worker, which records its closures into a secondary command buffer, while the
consumer keeps every synchronization decision on the primary. That is the "simplest correct subset"
of the brief, in place, with one worker.

What is missing for a real pool is structural, not a matter of more threads: the consumer keeps one
active pass (`PassSecondary::Handle` returns true for every block while `active` is set, and
`TryStart` is reached only when `active` is null), so a second worker would never receive a second
job. Passes can overlap only if the consumer scans ahead to the next BeginRendering before it
finishes the current one. That look-ahead is the real work, and it cannot be validated here
(verify = command hash needs a run of the game).

## 1. State map (draw thread -> worker)

Per-op state the draw thread mutates, and where it goes. "Worker" = the PassSecondary worker thread.

| State | Written by | Lives in | Worker reads it? | Decision |
|---|---|---|---|---|
| Bind caches (vertex input, push constants, bind-cache frame, texture block memo, RsMemo, CsStats) | draw thread (rasterizer, vk_rasterizer.cpp) | Rasterizer / DynamicState | no: the worker sees only recorded closures | owner only, resolved before recording |
| DynamicState / render_state (Scheduler) | draw thread | Scheduler | no; state effects are recorded as DynamicState / BindPipeline closures | owner only |
| Descriptor sets, push descriptors, bindless tables | draw thread | Pipeline / descriptor pool | by value only: the closure copies the handles and arrays (CommandRecorder::Copy, pinned chunks keep referenced memory) | owner allocates and writes, worker replays |
| Runtime barrier and layout state (FlushBarriers, transitions, image layouts) | draw thread | Runtime / TextureCache | never inside a pass: the ScanPass switch rejects Barrier, Copy, Dispatch, BeginRendering nested, submits (scan.bad) | owner only (barriers stay on the primary before BeginRendering) |
| Stream buffer / staging uploads | draw thread | StreamBuffer, staging pool | never inside a pass (Copy rejected) | owner only |
| Buffer / texture cache lookups and uploads | draw thread | BufferCache / TextureCache | never inside a pass; a miss records a Copy or barrier and so ends the pass | owner only |
| Replay table (latest state block per command buffer and key) | consumer | PassSecondary::table | consumer only; the worker gets a per-job copy (job->state), chunks pinned | owner only |
| Chunk lifetime (table_pins, job_pins) | consumer, worker (job_pins release, atomic) | CommandRecorder::Chunk | atomic release by worker | already synchronized |
| Real handle of the pass (tls_pass_fake / tls_pass_real) | consumer (BeginRendering thunk), worker | thread_local | thread_local per worker, so several workers do not clash | safe for N workers |
| real_handles[] (fake -> real command buffer) | consumer (Bind) | CommandRecorder | read by worker only for its own pass fake, which is served from TLS (Real(), vk_recorder.cpp) | safe as long as a pass touches its own fake only |
| Secondary command buffer pool | worker (Commit, Hold), consumer (Release at submit tick) | CommandPool | under pool_mutex | safe for N workers |
| Single active job (`active`, job_seq, queue) | consumer | PassSecondary | the worker reads only its Job | the blocker for N workers: one job at a time |

Conclusion: nothing per-op needs per-worker copies in the current design. Per-worker resources are
only needed when several passes are in flight, which the consumer does not allow yet.

## 2. Qualification (what the scan accepts)

`PassSecondary::ScanPass` (vk_recorder.cpp) accepts a pass when, from its BeginRendering to its
EndRendering, every block is one of: Draw, BindPipeline, BindDescriptors, PushConstants, DynamicState,
VertexIndex, Label (or Data / Meta / End). Anything else (barrier, copy, dispatch, nested pass,
submit) makes the pass a fallback. Also required: formats valid (SetPassFormats), the command buffer
not pruned from the replay table, more than SOTC_PASS_SEC_MIN draws, at most kMaxScanChunks chunks,
and the producer publishes the end within kMaxScanWait (5 ms, spin/yield).

This is the brief's predicate: "all ops resolved on the prep path, no uploads, no readbacks, no layout
transition inside, targets already in the layout" maps onto "no Copy / Barrier / Dispatch block inside
the pass". A cache miss or image creation records a Copy or barrier, so it falls back too. Barriers
before BeginRendering stay on the owner, as the brief requires.

## 3. Changes (worktree, uncommitted)

New files:
- `gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.h` / `.cpp`: `Vulkan::ParallelRecord`.
  `Enabled()` (SOTC_N5_PARALLEL=1 and SOTC_NATIVE, SOTC_N5_GRAPH nonzero, SOTC_PARALLEL_RECORD=1,
  SOTC_PASS_SEC=1; one LOG_WARNING when the switch is on but a prerequisite is missing).
  `NoteFallback(reason, block)`, `NotePassParallel()`, `NoteWorkerJob(ops, ns)`,
  `NoteOwnerWait(ns)`, `Report(out, seconds)`.

Edited `gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp` (all changes marked `sotc:` or
`SOTC_N5_PARALLEL`, no change to what is recorded):
- `PassSecondary::Scan` (struct): `reason`, `block` fields.
- `PassSecondary::ScanPass`: sets `scan.reason` at the timeout, the chunk limit and the block switch
  (`scan.block` = BlockCategory of the blocking command).
- `PassSecondary::TryStart`: counts each fallback reason (formats, dropped, scan stopped, scan bad by
  reason and block category, few draws) and each parallel pass. Counts only when `Enabled()`.
- `PassSecondary::Finish`: owner wait for the worker, timed only when `Enabled()`.
- `PassSecondary::Record`: worker time per job and the number of commands replayed (state entries
  plus pass blocks); timed only when `Enabled()`.
- One include line.

Mode 0 and SOTC_N5_PARALLEL without prerequisites: the only extra work is one cached bool read per
pass and the untaken branches; no clock reads.

Base copies from the main tree (unchanged by me, needed to compile): `gpu/sotc/native/*`,
`gpu/shadps4/video_core/sotc_stats.h`, `gpu/shadps4/video_core/renderer_vulkan/vk_frame_graph.{h,cpp}`.
`vk_recorder.{h,cpp}` and `vk_recorder_cmds.inc` were already identical to the main tree.

## 4. Counters (one line per 5 s window, from the report snippet below)

`N5 parallel (SOTC_N5_PARALLEL=1): passes secondary S, owner O (formats F, dropped D, stopped X,
timeout T, chunks C, block B, fewdraws W), block <category> n ... | jobs J, ops/job A, worker M ms/job
(P% of 1 worker), owner wait W ms/s, workers 1`

- passes secondary = passes_handed_to_worker (the "parallel" count).
- passes owner = every BeginRendering the recorder saw and did not hand over, by reason.
- block categories name the blocking command for `block` (barrier, copy, dispatch, beginrender, ...).
- ops/job = commands replayed per pass on the worker.
- worker ms/job and % of one worker: worker occupancy.
- owner wait ms/s: time the consumer waited at EndRendering. This is the number that says whether a
  second worker could help: near zero means the worker is not the bottleneck.

Not measured: the owner's own recording time (the producer, draw thread) and the Vulkan time of the
secondary on the GPU.

## 5. Snippets for files I do not own

`gpu/shadps4/core/libraries/videoout/driver.cpp`, with the other report lines near
`Vulkan::FrameGraph::Report(report, seconds);` (about line 2031):
```cpp
#include "video_core/renderer_vulkan/vk_parallel_record.h" // sotc: SOTC_N5_PARALLEL counters
...
Vulkan::ParallelRecord::Report(report, seconds); // sotc: SOTC_N5_PARALLEL (passes, waits)
```
Use it together with `SOTC_PASS_SEC=1` (the counters only see passes the recorder reaches).

## 6. Verify

- Syntax-only check (clang++ from `out/gpu/compile_commands.json`, flags of
  `gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp`, worktree `gpu/shim` and `gpu/shadps4`
  first, `-fsyntax-only`): `vk_recorder.cpp` exit 0, `vk_parallel_record.cpp` exit 0.
- Not done: a build, any game run, measured counts, the step 3 command-sequence hash. The hash needs
  a capture of each vkCmd* argument in both the worker and the owner path. It cannot be checked
  without running the game, which the brief rules out for this session.

Run plan for the measurement (not executed):
`SOTC_NATIVE=1 SOTC_PARALLEL_RECORD=1 SOTC_N5_GRAPH=1 SOTC_PASS_SEC=1 SOTC_N5_PARALLEL=1` on the
attract and shrine scenes. Read: passes secondary vs owner, the fallback reasons, owner wait ms/s,
worker % of one worker. Go to a pool only if the owner wait is large and the worker is near 100%
busy, which would show the worker as the bottleneck; if the owner wait is near zero, the worker is
not the limit and a pool gains nothing.

## 7. Not done, and why

1. Multi-worker pool. The consumer keeps one active job, so extra workers would be idle. Needs (2).
2. Pass look-ahead. The consumer must scan the next BeginRendering and snapshot the replay table at
   that point (the table changes with the state blocks of the pass in flight) before it finishes the
   current pass. Then the next pass's Job can be queued while the current one is recorded, with
   per-job state copies (already the design of Job::state). The change touches Handle, TryStart and
   Finish, and needs the verify run to catch replay-table mistakes.
3. Owner-side barrier elision. Not part of this task; the brief keeps barriers on the owner.

I did not implement (1) and (2) blind: they change the order in which the replay table is read and
the chunk pins are released, a risk that only a run with verify can close.
