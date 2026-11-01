# Wave 3, gameside: guest-runtime causes of the "game-cpu" frames

Scope: the 13 slow frames of trace session 20261101-085933 that hitch.md labelled "game-cpu" (1002, 1107,
1111, 1177, 1185, 1191, 1194, 1217, 1221, 1225, 1234, 1235, 1237). Analysis was read-only on the traces.
No game run and no ninja build. Uncommitted edits on branch `wave3/gameside` (worktree
`.claude/worktrees/wave3-gameside`), based on `re/stutter-architecture` 9f18413.

## 1. Findings, ranked by evidence

Method: `sotc_trace.py` plus two scratch scripts (a per-frame span timeline and clipped span totals per
thread). A frame window is the flagged flip-to-flip frame event. Blocking spans are the recorder's wait
spans (evf, cond, equeue, read/write fault, readback, file read, mutex, sleep).

| frame | ms | BPE job work in the window | game-thread blocking | draw thread in the window | reading |
|---|---|---|---|---|---|
| 1217, 1221, 1225, 1234, 1235, 1237 | 50 to 58 | none traced (silent) | evf wait 56 to 72 ms, same return site on all 4 game threads | ops 35 to 44 ms of 50 | game waits on GPU-side progress; "game-cpu" label is wrong |
| 1002 | 91 | none in window; first job at +103 ms, after the window | evf 23 to 102 ms | ops 42, bind resources 28 | game waits, workers idle or untraced until +103 |
| 1107 | 108 | RenderShadowCascade 0/3 last 165 ms; only 5.5 ms of traced child work; 7612 contended scheduler-lock waits (5.3 ms) | evf 107 ms; FIOS cond 214 ms (idle, file reads about 1 ms) | bind 30, ops 33 | untraced job time plus draw-side bind |
| 1111 | 58 | RenderGBufferBakedPerVertex 26 to 31 ms, little traced inside | evf 77 ms | ops 34, readback 9 | game-inherent job compute |
| 1177, 1185, 1191 | 59 to 84 | none | evf 72 to 98 ms; read fault 4.5 ms on thread 16100 (1177) | readback 8 to 16, command 5 to 20 | draw-thread readback chain (port) |
| 1194 | 50 | first job at +11 ms (RenderXPRDecals) | evf 66 ms | ops 34 | scheduler-lock contention: 16,556 contended waits = 10.3 ms |

Conclusions:

1. The "game-cpu" label is unreliable for most of the 13 frames. In the six 50-to-58 ms frames the draw thread
   is busy 35 to 44 ms and every game thread waits in one `EventFlag_WaitInfinite` return site (0x17edb4c7).
   The signaller is not in the trace (`evf_set` was untraced). Those frames look like GPU-chained waits.
2. Frames 1177, 1185 and 1191 are port-caused readback chains on the draw thread. They belong to the draw-side
   wave (readbacks are Precise by design, see reports2/16-job-system.md section 1).
3. Frame 1194 and frame 1002 point at the job scheduler. Four workers contend on the scheduler lock with the
   default 20 us spin (`SOTC_MUTEX_SPIN_US`, src/runtime_mutex.c). In 1002 the workers do nothing traced
   for 80 ms, which fits an idle loop on `scePthreadYield`, and that loop is untraced.
4. Frame 1107 has about 100 ms of job time with no traced child work. It is either guest compute or a worker
   that is not scheduled. The trace cannot separate the two; the new counters below can.
5. FIOS cond waits (up to 214 ms in 1107) are idle waits. File reads total about 1 ms in the window, so the IO
   path is not a cause. Sound threads follow the render threads.
6. A 26 to 31 ms `RenderGBufferBakedPerVertex` job (1111, and 1002's successor) is game-inherent compute with
   little traced inside. Thread placement cannot shorten it.

## 2. What changed (uncommitted)

All new behaviour is off by default or is diagnostic, and it sits under the existing trace and report switches.

- `src/runtime_sotc.c`, `evf_set`: when SOTC_TRACE is on, an instant `evf set` is pushed with a = flag address,
  b = setter's guest caller, aux = waiters woken. `trace_now()` returns 0 when the recorder is off, so the
  cost with SOTC_TRACE off is one weak call. Waiters are not paired by flag address (the "evf wait" span
  carries bits and caller only); pair them by time.
- `src/runtime_thread.c`, `thread_yield` (scePthreadYield): new switch `SOTC_YIELD_BACKOFF=<n>,<us>`, off by
  default. With it set, every n-th yield of a run sleeps `us` microseconds (`compat_sleep_ns` on Windows,
  nanosleep elsewhere). A run is a series of yields less than 1 ms apart. Each backoff sleep is a trace span
  `yield backoff` on the calling thread when SOTC_TRACE is on. With the switch unset, the path is
  `sched_yield` as before.
- Counters for yields, backoff sleeps and time slept are kept while SOTC_FPS, SOTC_TRACE or the switch is set.
  They print as one line in the 5 s report: `SOTC YIELD: calls=... backoff every N yields for U us: sleeps=...
  slept=...`. Function: `runtime_yield_stats`.
- `src/runtime_thread.c`, `runtime_cpu_stats` (Windows): process CPU share over the report window, as a share
  of one logical CPU and of the machine. It prints `SOTC CPU: process ...`. Printed under SOTC_FPS only.
  Near 100% of the machine means CPU contention.
- `src/runtime.h`: declarations for the two new report functions.
- `gpu/shadps4/core/libraries/videoout/driver.cpp`: extern declarations and the two report lines, next to the
  SOTC USLEEP line in the 5 s report.
- `tools/sotc_trace_classify.py`, `frame_stats`: new cause `gpu-chain`. It applies when the draw thread is at
  least 60% busy and one main game thread (Shadow Game Upd, Game Render, RenderFlip, BPE JobWorker) spends at
  least half the frame in a sync wait (evf, equeue, cond, sema). It is checked before `draw` (85%), so it
  takes precedence over that label; the readback, fault and io checks are unchanged and still come after
  `draw`. The returned dict now has `sync_peak`. Docstring updated.
- `tools/trace/sotc_trace.py`: docstring cause list updated (the hitch verdict comes from `frame_stats`).

Per-thread CPU time (the originally planned SOTC_THREAD_CPU per-thread line) is NOT done: it needs a registry of
host thread handles. The process-wide line answers the contention question first.

## 3. Effect of the new classifier on the 1 Nov session

Run: `python tools/sotc_trace_classify.py user/trace/20261101-085933` (the classifier reads the .sotctrace
files directly). Before the reorder, 39 frames were `draw` and one was `gpu-chain`. After it, 40 frames are
`gpu-chain` and the remainder keep their old labels. Frames 1217, 1235 and the other 50-to-58 ms frames above
now read `gpu-chain`, matching conclusion 1. Frames 1002, 1107, 1111, 1191 read `gpu-chain` too (draw
thread 50 to 95% busy, a main game thread in a sync wait for half the frame or more). The `draw` label is now
used only for frames where the game threads were not sync-waiting. Labels per file are in the classifier's
table; the file name of a dump can differ from its frame number (for example 1177 is in frame001176 and 1185
is in frame001184), so match on the frame number inside the file.

Note that frames with readback waits of 30% or more of the frame still take `gpu-chain` when the draw thread is
busy and a sync wait exists, because `gpu-chain` precedes the readback check. If that is not wanted, move the
readback check above `gpu-chain`.

## 4. Safety and ordering argument

- Yield backoff changes only timing of idle yield loops. Guest code cannot observe a yield's result (it returns
  0 in both paths). The worker's 10 s deadlock check is guest-time based and runs in the same loop, so it still
  happens. Cost: up to `us` extra latency per job pickup, bounded by the switch.
- The `evf set` instant is written after the flag lock is released, so it does not extend the lock hold.
- The `yield_config` word is read once and is racy-by-design like the usleep config; all counters are
  atomics.
- `runtime_cpu_stats` keeps one static previous value and is called only from the report thread.

## 5. How to test in game (one matrix round, no ninja)

- Diagnostic round: `SOTC_TRACE=1 SOTC_FPS=1` (plus the existing lab route). Read the `SOTC CPU:` and
  `SOTC YIELD:` lines and the `evf set` instants in the .sotctrace files.
- Yield round: the same plus `SOTC_YIELD_BACKOFF=8,50` and then `=16,100`. Compare `game-cpu` and `gpu-chain`
  counts in `sotc_trace_classify.py`, `SOTC CPU` share, and frame histogram. The default must stay unset.
- Pair `evf set` instants with `evf wait` spans by time to name the signaller of the 50-to-58 ms frames.

## 6. Risks

- The classifier change re-labels most `draw` frames of old sessions as `gpu-chain`. Old reports' verdict
  tables are not comparable with new ones.
- Yield backoff adds latency to job pickup. Too large a `us` can hurt render waits; start with 50.
- `SOTC CPU` is process-wide. It cannot say which thread burns the CPU.

## 7. What remains

- Per-thread CPU time (SOTC_THREAD_CPU) and a CPU-time column in the hitch tables.
- Identify the signaller of the 50-to-58 ms evf waits from the new `evf set` instants.
- Frame 1107's untraced job time: needs a sampler run (SOTC_PROFILE on BPE JobWorker) to see whether it is
  compute or an unscheduled worker.
- Draw-side readback chain (1177, 1185, 1191): owned by the draw specialist.
- Thread placement or priority for game threads: not justified until the CPU share shows saturation.

## 8. Main tree check

Compared the touched files with branch `re/pc-native` (63f2b8c): `src/runtime_thread.c`, `src/runtime_sotc.c`,
`src/runtime.h`, `tools/sotc_trace_classify.py` and `tools/trace/sotc_trace.py` are unchanged from my base.
`gpu/shadps4/core/libraries/videoout/driver.cpp` gained a 14-line block in `VideoOutDriver::Flip` (around line
1374, the SOTC OP COMPUTE BIND report). My edit is around line 2090 (the report next to the usleep line) plus
two extern declarations near line 100. No overlapping hunk, so the coordinator's merge should be clean.

## 9. Syntax checks

- `src/runtime_thread.c`, `src/runtime_sotc.c`: `clang -fsyntax-only -std=c11 -Wall -Wextra -Werror` with the
  project defines, the worktree `src` and `gpu` directories first, the main tree's third-party includes after.
  Clean.
- `gpu/shadps4/core/libraries/videoout/driver.cpp`: `clang++ -fsyntax-only` with the ninja flags from
  `out/gpu/build.ninja` (the `BB_FONT_PATH` define dropped; it only holds a string), the worktree `gpu/shim` and
  `gpu/shadps4` first. Clean.
- `tools/sotc_trace_classify.py`, `tools/trace/sotc_trace.py`: `py_compile` clean; classifier run on the
  1 Nov session as in section 3.
