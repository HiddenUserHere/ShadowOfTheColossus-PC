# Frame chain in PS4 Pro mode at 60 FPS (9 Nov 2026)

This pass only analyses existing data. Nothing was edited, built or run. Line numbers are for branch
`sync/upstream-0.72.2` as of 9 Nov.

Labels: **measured** means it comes from a trace or a run log. **Code** means it was read from the source or the
eboot disassembly. **Inferred** means it is reasoned from the two, and **open** means it is not settled.

Run: `SOTC_NEO=1 SOTC_MODES=af16,fps60game,grassbuf SOTC_FPS_CAP=off SOTC_FPS_LIMIT=60 BB_VBLANK_HZ=120
SOTC_REC_SPIN_US=0`, plus `sotc.ini`. The ini sets `SOTC_NATIVE=1`, `SOTC_PC_RING=8`, `SOTC_CP_LABEL_SLEEP=1`,
`SOTC_FLIP_THREAD=1` and `SOTC_FLIP_COALESCE=1`, and leaves `SOTC_CP_AHEAD` unset, so it is 0.

## 0. Answer

1. **The game waits on the render-frame gate, not on a vblank or the flip queue.** Every `bpe::threading::Event`
   is created with the name `PS4SyncEvent` (`sub_3b3d0`), so "it waits on PS4SyncEvent" is true but says little.
   The specific waits, all observed in code and in the samples:
   - The main thread (`GameCoreLoop::GameLoop` -> `GameFrame_Update` -> `GameUpdate_FrameGate` ->
     `RenderThread_WaitGateTimed_C` 0xbd0a0) waits on the event at `R+0x10`.
   - "Game Render" waits on the event at `R+0x20`.
   - Both events are set at the very end of `RenderThread::RenderFrame` (0xcfd76, 0xcfd82). That is right after
     `RenderBackend_Frame` (0x90ea0) returns.
   - `RenderBackend_Frame` contains the frame-slot spin (0x90f47). With `SOTC_NATIVE` that spin is the hook
     `NATIVE_FRAME_SLOT_SPIN`, which calls `SotcNative::FrameWait(closed - 1)` (`native_sync.cpp:55-68`).

   So the game starts frame N+1 only after the port reports frame N-1 complete on the GPU. The flip event queue is
   waited on only by the `RenderFlip` thread, and nothing on the frame path waits for that thread.

2. **The frames come in pairs, and that is the serial link.** The port records a frame end only when the
   command processor (CP) goes idle (`liverpool.cpp:4400-4415`, the `else if (submit_done)` branch; frame
   tickets are off).
   - The CP is held back by the draw thread: it waits on labels (48% of CP samples spin in an atomic load) and
     on back-pressure (`cp blocked on draw`). So it is still inside frame N when frame N+1's first submits
     arrive, and it never goes idle between the two.
   - Frame N's end is then recorded together with N+1's. `FrameWait(N)` therefore waits for N+1 to finish
     completely.
   - Result: every other game frame is short (about 8 ms, its wait is already satisfied) and the next one is long
     (53-58 ms traced). The long frame covers the draw work of both frames.
   - Measured in both traces: the tiny frame-end command buffer (0.04 ms) appears once per pair, never once per
     frame.

3. **Cost of the pairing (measured, traced):** the draw thread is idle about **6.6 ms per pair, so 3.3 ms per
   frame (11%)**. It sits idle from the moment it drains both frames until the first op of the next frame
   reaches it:

   | Step | Time (median) |
   |---|---|
   | Frame-end flush submitted | 0.35 ms |
   | GPU runs the work still queued behind the draw thread | 2.2 ms |
   | FrameWait returns, event set, main thread wakes | 0.3 ms |
   | Kick, then the render job's first submit reaches the draw thread | 3.2 ms |

   The untraced run agrees: the draw thread is on the CPU 25.0-26.5 ms per frame against a 28.4-30.1 ms mean
   frame, so about 88% busy.

4. **Why exactly 25.0 ms.** The draw thread needs about 22-25 ms of CPU per game frame (untraced). The pairing
   adds about 3 ms. Flips are then quantized to the 120 Hz present ticks (8.33 ms) with a 2-tick minimum, so the
   frames land on 3 ticks (25.0 ms) and 4 ticks (33.3 ms). The median is the 3-tick bucket.

   **Removing the serial chain cannot reach 16.7 ms.** It recovers about 3 ms per frame, roughly 28.4 to 25 ms
   mean, mostly by turning 33.3 ms flips into 25.0 ms flips. 60 FPS needs the draw thread's own work to drop under
   about 15 ms; `PARALLEL-RECORD-60FPS-2026-11-09.md` covers that.

5. **The other links in the hypothesis are not on the critical path:**
   - **GPU:** busy 21.4 ms per pair, 35%. Its only serial part is the 2.2 ms tail at the end of the pair.
   - **Recorder:** asleep 82% of the route.
   - **Labels:** written by the draw thread when it runs the op ("deferred to the GPU 0"). There is no write-back
     delay.
   - **120 Hz quantization:** it shapes only the displayed cadence. The game does not wait for vblanks.

6. **Best fix (existing switch): per-frame frame-end records.** With `SOTC_CP_AHEAD=2` under `SOTC_NATIVE`,
   `ServiceFrameMarks` records each frame's end as soon as that frame's submits are parsed. It no longer waits for
   CP idle.
   - `GateWait` stays the native `WaitReuse` (`gnmdriver.cpp:256`), so nothing else in the submit gate changes.
   - Expected gain: up to about 3 ms per frame. The p50 likely stays 25.0 ms, the p90 and the 1% lows improve,
     and the mean goes to roughly 25 ms.
   - It must be measured; section 6 has the runs.

## 1. Data and caveats

| Trace | Content | Route window |
|---|---|---|
| A `user/trace/20261109-080203/session.sotctrace` (log `user/prof/m0/z_pro60_gpu.out`) | spans, GPU `cmdbuf` spans, no samples | flip 454 (`LAB ROUTE start at flip 454`) + 222 game frames, 7.1 s |
| B `user/trace/20261109-075626/session.sotctrace` (log `user/prof/m0/y_sample.out`) | spans + stack samples, no GPU spans | flip 464 + 210 game frames, 7.2 s |
| untraced reference `user/prof/m0/t_pro60.out` | log only | `LAB ROUTE done: 265 frames, p50 24.9 ms` |

- **Tracing costs about 15%.** The route p50 is 32.8 ms in trace A, 33.3 ms in B, and 24.9 ms untraced. The draw
  thread's CPU per frame is 28.5 ms traced against 25.0 ms untraced. The traced draw thread also spends 9.4% in
  `NowNs` and about 11% in `ScopeMin`, `SplitTimer` and `Charge`. All millisecond figures below are traced unless
  marked untraced; ratios carry over.
- **Events were dropped.** Trace A dropped 28.2 M events, mostly the four guest job workers' `mutex wait` and
  `mutex holder` spans (11.7 M each in about 30 s). Job-worker events are therefore missing in bursts, and
  `gnm submit done` was found for only 107 of 222 frames. Every other ring (main thread, Game Render, update, CP,
  draw, recorder, GPU, frames) is complete enough for per-frame work.
- **Frame anchor.** A game frame starts when the main thread wakes from the render gate (the end of its
  `evf wait`) and is followed by the `evf set` of `GameFrame_Update` (0x25b054). The release of frame k is the
  wake of frame k+1.
- Scripts are in the session scratchpad (`chain/frames.py`, `chain/final.py`, `chain/phase*.py`). They load the
  `.sotctrace` with the same record format as `tools/trace/sotc_trace.py`.

## 2. The frame loop (who waits on what)

| Thread | Waits on | Set by | Source |
|---|---|---|---|
| main (`GameLoop`) | `R+0x10` via `RenderThread_WaitGateTimed_C` 0xbd0ce | `RenderFrame` end 0xcfd76 | code (disasm), samples B: 86% of main samples there |
| Game Render (`RenderThread::ThreadFunction`) | `[R+0]` (frame ready, from main) and `R+0x20` (previous frame job done) | main (`GameFrame_Update` 0x25b054) and `RenderFrame` end 0xcfd82 | code (report 06), trace pairing |
| Shadow Game Update | event set by main at 0x534965 | main, once per frame | trace pairing (1,014 of 1,037 waits) |
| RenderFlip | flip equeue (`FlipEvent_WaitEqueue` 0x960a0) | `VideoOutDriver::Flip` on the present thread | code; sets nothing the frame path waits on |

End of `RenderThread::RenderFrame` (0xcfd45-0xcfd82), from the disassembly:

1. `Frame::InsertWaitFlipDone` (a GPU-side wait, written into the DCB);
2. `SubmitJob(mode 1)`, which leads to `KickoffSubmitJob` and then `sceGnmSubmitDone`;
3. `sub_ccfd0`;
4. `RenderBackend_Frame`, which toggles the 2-slot frame ring (`[r13+0x7b0]`) and then runs the slot spin at
   0x90f47 on the slot's label;
5. sets the events at `R+0x10` and `R+0x20`.

On the PS4 the slot spin waits for the GPU's EOP of frame N-1 (the two slots alternate). The port hook does the
same: `ReuseTarget() = closed - 1`, then `FrameWait`, then `BufferCache::NativeFrameWait`
(`buffer_cache.cpp:8817-8844`). `NativeFrameWait` waits in two stages:
- first for a frame-end record in `pc_host_frames` with `seq >= target` (a condition variable, 200 ms cap);
- then for that record's scheduler tick (`scheduler.Wait`).

The record is written by `PcHostNoteFrameRecorded` (`buffer_cache.cpp:8224`). Its callers:

- **Default (frame tickets off):** the CP idle path (`liverpool.cpp:4400-4415`). It runs only when every
  queue has nothing left to parse, and it records `submit_done_seq` at that moment. If the next frame's submits
  are already queued, the record covers both frames.
- **Frame tickets on** (`SOTC_CP_AHEAD=2` or `SOTC_FRAME_AHEAD=1`, `liverpool.cpp:532`): `ServiceFrameMarks`
  (`liverpool.cpp:6609-6655`) records each frame as soon as every queue has finished the tasks it had when that
  frame's `SubmitDone` ran.

Samples B confirm the wait: in the long frames, after `SubmitDone`, the job worker that runs `RenderFrame` sits
in `FrameWait`, inside `wait_for` on the condition variable. That is the record wait, not the GPU tick.

## 3. Per-frame timeline (route window)

Times are in ms from the frame's start (main thread wake). Columns: median and p90, trace A, with trace B's median
in brackets.

### 3.1 Short frame (111 of 222; the previous long frame's merged record already covers its wait)

| Event | med | p90 | B med |
|---|---|---|---|
| main sets "frame ready" (kick) | 0.35 | 0.42 | 0.36 |
| update thread done (`evf set` 0x5b9967) | 1.76 | 2.14 | 1.89 |
| draw thread gets the first op of this frame (end of its idle) | 3.24 | 3.61 | 3.34 |
| flip of the previous long frame | 3.11 | 6.09 | 3.40 |
| `sceGnmSubmitDone` (29 frames with the event) | 7.49 | 8.03 | 8.05 |
| slot wait (`SubmitDone` to gate set) | 0.21 | 0.27 | 0.21 |
| release = next frame starts | **7.80** | 8.64 | 8.17 |
| per frame: GPU busy / CP parse / draw idle / queue submits | 0.72 / 1.06 / 2.68 / 3 | | |

### 3.2 Long frame (111 of 222; its wait needs the merged record of both frames)

| Event | med | p90 | B med |
|---|---|---|---|
| kick | 0.33 | 0.44 | 0.35 |
| update thread done | 1.84 | 2.29 | 2.01 |
| draw thread finishes the previous (short) frame's ops; the final submit follows within 0.06 ms (see 3.3) | ~27.8 | | |
| `sceGnmSubmitDone` (78 frames) | 27.84 | 37.83 | 31.76 |
| flip of the previous short frame | 35.44 | 59.50 | 37.71 |
| draw thread drained: every op of both frames done; the CP finishes parsing just before this (it is lock-stepped) | 50.64 | 64.40 | 55.72 |
| frame-end flush submitted (CP idle path, `rasterizer->Flush`) | 50.99 | 65.32 | 55.46 |
| GPU ends the last real command buffer (1.24 ms median, p90 2.05) | 53.16 | 66.28 | (no GPU spans) |
| GPU ends the 0.04 ms frame-end command buffer = the tick FrameWait needs | 53.22 | 66.34 | |
| gate set (0xcfd87), main and Game Render wake = release | **53.47** | 66.86 | 58.05 |
| slot wait (`SubmitDone` to release) | 25.70 | 33.37 | 29.05 |
| per frame: GPU busy / CP parse / draw idle / queue submits | 20.71 / 18.48 / 3.94 / 30 | | |

- **Pair period:** median 61.1 ms (p90 74.7) in A, 66.3 ms in B. Mean game frame: 32.0 ms in A, 34.3 ms in B.
- **Flip-to-flip intervals in the route** (A, in 120 Hz ticks): 2 ticks x40, 3 x64, 4 x45, 5 x45, 6 x19, 7+ x8.
  Median 32.8 ms.
- **Flip timing.** The flip of a frame lands about 5-7 ms after the draw thread passes that frame's last ops.
  That time is the flip thread plus the next present tick that is at least 2 ticks after the previous flip, with
  the `SOTC_PRESENT_WAIT` display gate. This adds display latency only; nothing in the game's frame waits for it.

### 3.3 Inside the long frame's build (partly open)

- In long frames `SubmitDone` comes 25-31 ms after the kick. In short frames it comes after 7-8 ms.
- **The game is not computing in that gap** (samples B): in the stall windows (kick + 12 ms to `SubmitDone`),
  94% of job-worker samples are the job scheduler's idle loop (`JobSystemScheduler_DoJobScheduling`, then
  `mutex_spin`), with no render job deeper in the stack. Main, Game Render and the update thread are all in
  `evf_wait`.
- **The stall ends when the draw thread finishes the previous frame.** Where the draw thread's end-of-frame
  label burst (30 or more `eop/release` within 1 ms) could be found, `SubmitDone` followed it within 0.06 ms
  (median, p90 0.18) in A (18/18) and 0.07 ms in B (13/13). No short frame has such a burst between its kick and
  its `SubmitDone`.
- **Inferred:** the frame's final submit job is gated on a label of the previous frame that the draw thread
  writes. The cause is the same one `SOTC_CULL_EXPRESS` describes (`liverpool.cpp:2505-2530`): the XPR / EOP
  label the game spins on sits behind the previous frame's ops in the back ring. The exact wait is **open**:
  guest stack walks stop at the thread entry, and the job-worker rings lost their events in that window.
- This stall caps how far the game can run ahead of the draw thread at one frame. It costs the draw thread
  almost nothing today, because the draw thread is busy with that previous frame anyway.

## 4. Serial waits and their cost

The cost of each wait is in traced ms. "Per frame" is half the per-pair figure.

| # | Wait | Serial? | Cost | Evidence |
|---|---|---|---|---|
| W1 | **Merged frame-end record.** `FrameWait(N)` waits for N+1, because the record is taken only at CP idle. | yes: this is the root cause | it turns W2-W4 into draw-thread idle: **about 6.0 ms per pair (3.0 ms per frame)** | one 0.04 ms frame-end command buffer per pair, never per frame; slot wait 25.7 ms in long frames, 0.2 ms in short ones |
| W2 | GPU tail at the pair end: the GPU is about 2 ms behind the recorder when the draw thread drains | yes, because of W1 | 0.35 ms (flush submitted) + 2.2 ms (GPU finishes) = **2.5 ms per pair** | 50.64 to 50.99 to 53.16 |
| W3 | Tick to gate set to main wake (`scheduler.Wait`, `evf set`, wake) | yes | **0.3 ms per pair** | 53.16 to 53.47 |
| W4 | New frame start to first op on the draw thread (kick, Game Render, `RenderFrame` job, first submit, CP parse) | yes, because of W1 | **3.2 ms per pair** | short frame: draw idle until 3.24 ms |
| W5 | Long build waits for the draw thread to finish the previous frame (3.3) | yes, but hidden while the draw thread is the bottleneck | about 0 today | `SubmitDone` within 0.06 ms of the end-of-frame burst |
| W6 | CP waiting on labels behind the draw thread | yes, by design (labels are written at op time) | not on the critical path; it is what keeps the CP from going idle (W1) | 48.5% of CP samples in `__cxx_atomic_load`; `cp blocked on draw` spans; `CP label sleep ... 462 ms/s asleep` |
| W7 | GPU idle between submits | no: the GPU is starved by the draw thread, it does not block it | GPU busy 21.4 ms of 61 per pair (35%) | GPU spans |
| W8 | Recorder | no | busy about 5 ms per frame, asleep 82% | samples B; `recorder/frame: busy 5.27` untraced |
| W9 | Present / 120 Hz tick / display gate | not for the game | about 5-7 ms from the draw thread finishing a frame to its flip (display latency) | flip positions in 3.1 and 3.2 |
| W10 | Per-submit `WaitReuse` (`FrameWait(closed-1)` in `GateWait`) | no | about 0: the target is always already recorded | `NATIVE SYNC reuse ...`; the frame-wait total matches the slot spins alone |

- **Draw-thread busy time per pair:** 61.1 - 6.6 = 54.5 ms, so **27.2 ms per frame traced** (about 25 ms
  untraced: `cpu/frame draw 25.00` and `26.51` in `t_pro60.out`).
- **Untraced cross-check of the pairing:** `NATIVE ... frame waits 5734 avg 0.36 ms` in 5 s with
  `slot_spin 165/165` gives 2.06 s of frame waits for 165 frames. That is about 25 ms on every other frame, the
  same shape as traced (`t_pro60.out`, route window).

## 5. Fixes, ranked by ms saved

| Rank | Fix | Saves (est.) | Risk | Switches |
|---|---|---|---|---|
| 1 | **Per-frame frame-end records.** Run the frame tickets with `SOTC_NATIVE`: `SOTC_CP_AHEAD=2` sets `frame_tickets` (`liverpool.cpp:532`), and `ServiceFrameMarks` then queues OnSubmit, Flush and `PcHostNoteFrameRecorded(frame)` for each frame once its submits are parsed. `GateWait` stays the native `WaitReuse` (`gnmdriver.cpp:256`), and `SubmitDone`'s non-native gate is skipped (`gnmdriver.cpp:2643`). | W1 removes W2+W4 from the draw thread: **about 3 ms per frame** (traced 30.5 to about 27.2; untraced mean about 28.4 to about 25). The p50 likely stays 25.0 ms (3 ticks); the 33.3 ms flips and the p90 drop. | Low to medium. This combination has never been measured with `SOTC_NATIVE`. `ServiceFrameMarks` waits for every queue's completed-task count, including the compute queues; a compute task that never finishes delays the record (the 200 ms cap then applies, with log `SOTC NATIVE frame wait: frame N not recorded`). One extra flush per frame (cheap). | `SOTC_CP_AHEAD=2`. Do not use `SOTC_FRAME_AHEAD=1`: it also sets `frame_tickets` but is documented as needing `SOTC_LABELS_HONEST`. |
| 1b | Same fix in code, if `SOTC_CP_AHEAD=2` brings side effects: with `SotcNative::On()`, make `frame_tickets` true (or make the idle path record per `SubmitDone` mark). | same as 1 | same | new code; no new switch is needed if it is folded into native |
| 2 | **Shorter GPU tail at the frame end.** Flush the open command buffer when the CP parses the frame's last EOP or the flip, not only after the draw thread drains. The GPU then starts the last batch earlier. | up to about 1 ms per pair today; about 0 after fix 1 (the tail then overlaps the next frame's drawing) | low | recorder or flush cadence (no existing switch for "flush at frame-end label") |
| 3 | **Remove W5** (the long build waits for the previous frame's label). Run the XPR segment ahead with `SOTC_CULL_EXPRESS=1`, or end the spin at record time for every label kind. | 0 today. It matters only once the draw thread is below the game's build time (about 8 ms traced), i.e. after the draw-thread cuts. | medium (reordering; see the rules at `liverpool.cpp:2505+`) | `SOTC_CULL_EXPRESS`, `SOTC_NATIVE_MASKS` |
| 4 | **Signal the gate when the GPU passes the label, without the record step.** Not recommended: with `SOTC_PC_RING=8` the GPU reads the frame ring straight from imported guest memory, so the slot really needs GPU completion. After fix 1 the record step costs nothing anyway. | about 0 after fix 1 | high (ring reuse) | none |
| 5 | **Let the game run one more frame ahead.** The engine has 2 frame slots, and the spin waits for N-1 by design, so going further needs a third ring slot in the engine. | 0 while the draw thread is the bottleneck | high | none |
| 6 | **Drop the 120 Hz quantization of the game's flips.** No throughput gain, because the game does not wait for vblanks or flips. It only changes which tick a finished frame is shown on (lower latency, no faster game). It also moves the game step: with the fixed step F = N/rate (05-frame-pacing), the game speed follows the frame rate unless the dt path compensates (hooks H1/H2 are still open). | 0 ms of frame time | medium (pacing, game speed) | `BB_VBLANK_HZ`, `SOTC_FPS_LIMIT`, `SOTC_PRESENT_WAIT`, `SOTC_PACER` |
| 7 | **Cut the draw thread's own work** (this is what 60 FPS needs). The route profile (samples B, traced): `BindResources` 31%, `DrawIndirect` 23.5%, `DispatchDirect` 15.7%, `DispatchIndirect` 10.3%, `Draw` 9.3%, `ObtainBuffer` 7.1%, `SynchronizeMemory` 6.8%, `TryBindCacheFrame` 5.8%, shader HLE 4.9%, `BindTexturesFull` 4.9%, `ReplaceChangedPages` 4.3%. | the only route from about 25 to under 15 ms per frame | see `PARALLEL-RECORD-60FPS-2026-11-09.md` | |

**Expected result of fix 1 alone:** about 25 ms per frame untraced, every frame on 3 ticks. That is about 40 FPS
instead of 35, with fewer 33.3 ms frames. 60 FPS still needs fix 7.

## 6. Runs to measure (for the coordinator)

All runs use the Lab route `user/routes/tests.json`, one round each. Run `python sotc.py` without sourcing
`msys2-env.sh` first.

1. **A/B for fix 1, no trace.** Use the base environment of this document (`SOTC_NEO=1 SOTC_MODES=af16,fps60game,grassbuf
   SOTC_FPS_CAP=off SOTC_FPS_LIMIT=60 BB_VBLANK_HZ=120 SOTC_REC_SPIN_US=0`) once as is and once with
   `SOTC_CP_AHEAD=2`. Compare:
   - `SOTC LAB ROUTE done` (p50, p99, hitches > 33 ms) and `SOTC DISPLAY` (1, 2 and 3 vblank counts);
   - `SOTC cpu/frame draw` against the mean frame: draw on-CPU should rise from about 88% toward 100%;
   - `SOTC NATIVE ... frame waits`: count and average; the total should fall from about 2 s per 5 s;
   - the log must have no `SOTC NATIVE frame wait: frame N not recorded within 200 ms` line.
   Add `SOTC_CMD_COUNTERS=1` to both runs: it classifies the frame-slot waits as cp-behind or gpu-behind
   (`native_sync.cpp:27-37`).
2. **Trace run without the job-worker mutex flood**, to settle 3.3 and confirm one record per frame:
   - Environment: the run-1 environment, plus `SOTC_TRACE=2 SOTC_TRACE_GPU=1`.
   - `SOTC_TRACE_ONLY=evf wait,evf set,gnm submit,gnm submit+flip,gnm submit done,draw idle,cp parse,cp blocked on draw,eop/release,func,queue submit,gpu,present,equeue wait,sleep,sample`
     (`gpu/shim/sotc_trace.cpp:110-130`; "gpu" keeps the GPU timeline, "sample" the sampler).
   - Do it once with `SOTC_CP_AHEAD=2` and once without.

## 7. Instrumentation the traces lacked

| Need | Where | Switch |
|---|---|---|
| Span "frame wait" around the record wait and the tick wait. Fields: a = target, aux = 0 record / 1 tick, b = caller (slot spin or `WaitReuse`). | `BufferCache::NativeFrameWait`, `buffer_cache.cpp:8817-8844` (or `SotcNative::FrameWait`, `native.cpp:335`) | `SOTC_TRACE` |
| Instant "frame recorded". Fields: a = seq, b = tick, aux = path (0 idle, 1 mark). It shows directly whether two frames share one record. | `BufferCache::PcHostNoteFrameRecorded`, `buffer_cache.cpp:8224` (callers at `liverpool.cpp:4412` and `6641`) | `SOTC_TRACE` |
| Scheduler tick on the GPU `cmdbuf` span, so a frame-end tick can be matched to its GPU span. b is the command count today. | `SotcTrace::Submitted`, `vk_scheduler.cpp:1471` | `SOTC_TRACE_GPU` |
| `evf wait` with the flag address. Today a = bits and b = the generic wrapper 0x3b4c7. Proposed: a = flag pointer, aux = bits, b = the wrapper's caller. | `src/runtime_sotc.c:454` | `SOTC_TRACE` |
| Instant "flip queued" (flip arg, index) when the request is pushed, and the vblank count on the existing "flip" instant. These give the 120 Hz tick each flip lands on and its queue delay. | `driver.cpp` `SubmitFlipInternal` callback (`requests.push`, about 3135-3160) and `VideoOutDriver::Flip` (`driver.cpp:620-624`) | `SOTC_TRACE` |
| A minimum duration for `mutex wait` / `mutex holder` spans, so the job-worker rings stop dropping 28 M events | `src/runtime_mutex.c:179-181` (as `SOTC_TRACE_IMPORT_MIN_US` does for imports) | new `SOTC_TRACE_MUTEX_MIN_US` (or `SOTC_TRACE_ONLY` as in section 6) |
| Guest-side timing of the long-build stall: time hooks on `RenderThread::KickoffSubmitJob` (0xd07c0), `JobSystem_WaitForCounter` (0x35590, with caller) and `BPXPR_RecordMaskedDraws` (0x11d7a0) | `scripts/hooks.d` via `tools/trace/sotc_trace.py autofuncs`, or by hand | `SOTC_TRACE_FUNCS` |
