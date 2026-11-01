# 14 - Loading hangs in the Lab base runs (1 Nov 2026)

Scope: analysis of the matrix logs under `user/pcmem/` (15 base logs in 18 batch folders), plus the
Lab route start code. No game was run and no build was made. Changes made in this pass: the give-up
line in `gpu/sotc/engine_cmd.cpp` and `engine_cmd.h` (section 5). Labels: **observed** (log, source or
summary), **inferred**, **unknown**.

## 1. Answer

- **observed:** only some base runs stalled. 4 base runs (batch `035053` and `035215`, attempts 1 and 2)
  have one thing in common: they ran in matrix batches with `--env SOTC_STALE_CHECK=16`, and in those
  batches every configuration failed to finish (summary: base 0/1, static 0/1). No base run without that
  env stalled in the logs, except the two runs in section 2.3, which were not base runs at all.
- **observed:** the 4 runs load slowly and then end early. The save load ("starting game") came at
  13.6-16.3 s instead of 7.0-8.0 s, the FPS line at 10 s read 1.7-3.0 instead of 24-28, the frames
  were 500-800 ms, and each process ended 14-31 s after its start with no "SOTC LAB ROUTE exit" line
  and no crash file in `user/crashes/`.
- **inferred (not proven):** the cause is the `SOTC_STALE_CHECK=16` diagnostic (it compares guest pages
  of every bind, about 1.1-1.7 million pages per 5 s window in these runs, against 20k-190k in the
  finished run `035657`). The Lab route did not hang in its own code. It waited for "starting game",
  which arrived late.
- **observed, not a route bug:** the route start (`lab_commands.cpp` `AutoRouteTick`) has no time limit
  in `WaitGame` and `WaitCam`. Both wait silently. That is why a slow run looked like a hang. This is
  fixed for `WaitGame` as far as the print goes (section 5); the quit part needs the diff in section 6.
- **observed, separate:** the two base runs `032557` and `035619` (no summary, batches aborted after
  about 8 s) were not base runs. Their logs show switches that the base config does not set:
  `SOTC_PC_RING` and `SOTC_PC_POOL_NATIVE` (032557), and `SOTC_HOOK_ALLOC_PROBE` (035619). They are not
  evidence of a switch-free hang.

## 2. Evidence

### 2.1 Batch environment (observed)

`summary.md` lists `--env` for every run. The matrix strips `SOTC_*` from the inherited environment
(`tools/pcmem_matrix.py` `launch()`), so a switch in a log can only come from the matrix configuration
or from a run started outside the matrix.

| batch | `--env` (summary) | base runs | outcome |
|---|---|---|---|
| 014856, 021424, 032958, 041450, 042755, 044642, 050726 | none | 9 logs (one per attempt) | route done |
| 035657 | `SOTC_STALE_CHECK=16 SOTC_HOOK_ALLOC_PROBE=1 SOTC_CLASS_TRACK=2` | 1 | route done, slow (see 2.2) |
| 035053 | `SOTC_STALE_CHECK=16` | 2 (a1, a2) | route did not run |
| 035215 | `SOTC_STALE_CHECK=16` | 2 (a1, a2) | route did not run; chunks2, static, ring also failed |
| 035931 | `SOTC_STALE_CHECK=16` | none (other configs) | not compared |
| 032557 | no summary (aborted) | 1 | base hook lines show `SOTC_PC_RING=1` and `SOTC_PC_POOL_NATIVE=1` live |
| 035619 | no summary (aborted) | 1 | base hook lines show `SOTC_HOOK_ALLOC_PROBE=1` live |

### 2.2 Timeline per base run (observed; times are game-process seconds from the log stamps)

| run | env | loading screen over | starting game | FPS at 5 s | FPS at 10 s | FPS at 15 s | SOTCSTALL lines | process end (log mtime, after batch start) | route |
|---|---|---|---|---|---|---|---|---|---|
| 014856 r1 | none | 1.5 | 8.0 | 30.2 | 28.3 | 29.8 | 2 | ~32 s | done |
| 014856 r2 | none | 1.1 | 7.1 | 30.3 | 27.4 | 30.0 | 3 | - | done |
| 021424 r1 | none | 1.1 | 7.7 | 30.2 | 26.2 | 29.2 | 3 | - | done |
| 032958 r1 | none | 1.1 | 7.3 | 30.2 | 24.4 | 26.7 | 6 | - | done |
| 032958 r2 | none | 1.3 | 7.6 | 30.2 | 26.6 | 29.1 | 6 | - | done |
| 041450 r1 | none | 1.1 | 7.2 | 30.2 | 25.6 | 29.0 | 3 | - | done |
| 042755 r1 | none | 1.3 | 7.5 | 30.2 | 24.5 | 28.7 | 12 | - | done |
| 044642 r1 | none | 1.1 | 7.1 | 30.2 | 27.0 | 30.0 | 2 | - | done |
| 050726 r1 | none | 1.1 | 7.3 | 30.3 | 26.8 | 30.0 | 2 | - | done |
| 035657 r1 | STALE_CHECK + ALLOC_PROBE + CLASS_TRACK | 1.2 | 7.3 | 30.2 | 12.7 | 13.4 | 213 | - | done (94 frames, p50 75 ms) |
| 035053 r1 a1 | STALE_CHECK=16 | 1.2 | 16.3 | 30.1 | 3.0 | - | 32 | ~31 s | route start printed at ~26 s, then no further line |
| 035053 r1 a2 | STALE_CHECK=16 | 1.4 | no line | 29.9 | - | - | 7 | ~15 s | no |
| 035215 r1 a1 | STALE_CHECK=16 | 1.5 | 15.0 | 30.0 | 2.6 | 1.7 | 23 | ~23 s | no |
| 035215 r1 a2 | STALE_CHECK=16 | 1.4 | 13.6 | no line | no line | no line | 16 | ~19 s | no |
| 032557 r1 a1 | none (but PC_RING, POOL_NATIVE live) | 1.4 | no line | 30.2 | - | - | 0 | ~8 s | no |
| 035619 r1 a1 | none (but ALLOC_PROBE live) | 1.4 | no line | no line | - | - | 0 | ~7 s | no |

The batch folder of 032557 has no summary, and the next matrix batch started 27 s later (032624). The
folder of 035619 has no summary, and the next batch started 38 s later (035657). So both were stopped by
hand (inferred).

### 2.3 Signatures (observed, grouped)

- **S1, slow load and early end (035053 a1, a2; 035215 a1, a2).** All four have the same
  `SOTC_STALE_CHECK=16` and the same base switch set (hook lines match the good runs: ENGINE_CMD only).
  Frame times 500-800 ms from about 5 s (`SOTCSTALL frame 191 800 ms ... cpu draw 789 ms`), `FPS 1.4-3.0`
  from 10 s, save load at 13.6-16.3 s (good: 7.0-8.0 s). The process ends 14-31 s after start with no
  exit line, no crash file in `user/crashes/` for these times, and the matrix moved on at once.
- **S2, slow but finished (035657).** Same `SOTC_STALE_CHECK=16` plus two more switches. Save load at
  7.3 s, FPS 12.7 at 10 s, 213 stall lines, route done. So the check is not the only cause of slowness
  in this batch, or the extra switches changed its cost (unknown).
- **S3, good.** All other base runs: save load 7.0-8.0 s, FPS 24-28 at 10 s, route done, `LAB ROUTE exit`.
- **S4, not base (032557, 035619).** Logs stop at 7-8 s, before the 10 s report, with no summary. The
  switches are visible in the hook lines (section 2.1). The log is silent after 8 s (the last line is
  a stats line or a warning). The runs were stopped by hand, so the cause is unknown.

Lines that appear in every run and are not signatures: `Runtime: event flag 'PS4SyncEvent' waited Ns`
(3-8 per run, also in the good runs), `sceSndxLoaderNotify` waits, and `ProcessGraphics: Unimplemented
IT_SET_PREDICATION`. The "6 stalled of 14" count in report 12 mixes S1 with S4; the correct count is 4
S1 runs (plus 2 aborted non-base runs) out of 15 base logs.

### 2.4 Exit paths (observed in source)

Exits that print nothing and can end a run early: window close (`gpu/shim/bbgpu.cpp:460`,
`std::_Exit(0)`, after `SDL_EVENT_QUIT`). The Lab route quit (`lab_commands.cpp` ~770) prints
`SOTC LAB ROUTE exit` first, so it is excluded. The assertion path (`common/assert.cpp:17`) prints
`STOP:` first and is also excluded. No crash files exist for the S1 times. So the S1 processes ended by
a window close or an external kill (inferred: a manual close, since the matrix had no reason to kill
them within 600 s). The matrix does not record the return code, so this cannot be checked from the logs.

### 2.5 The stale check (observed)

`SOTC STALE CHECK` lines per 5 s window: S1 (`035053` a1, `035215` a1) 1.08-1.66 million pages compared
per window, S2 (`035657` a1) 20k-190k. S1 logs also contain two real findings: `STALE CHECK: stale page
0x12dad3e000 ... hash age 3803 upload generations ... owner not named` (035053 a1), meaning a bound page
differed from its last upload. That is a correctness signal and is separate from the stall.

## 3. Lab route start (observed in source)

- `AutoRouteTick` (`gpu/sotc/lab_commands.cpp` ~666-775) is a state machine: `WaitGame` (needs
  `GameStarts() > 0` and `SOTC_LAB_ROUTE_DELAY` s after it), `StartCam`, `WaitCam` (needs
  `SotcFreecam::Get().frames > 0`, which only advances on engine ticks), `Playing`, `Done`, `Closed`.
- `WaitGame` and `WaitCam` have no limit and print nothing while they wait. `StartCam` retries every 3 s.
- `GameStarts()` is set by `TakeStdout` (`gpu/sotc/engine_cmd.cpp`), fed by `bbgpu_engine_output`
  from `src/runtime_file.c` `do_write` for fd 1 and 2. It searches one `write()` for "st_gameflow()
  starting game" and drops writes over 4096 bytes. A split write would be missed (not seen in the logs).
- The free camera's engine line (`LowBuffer`, `bb_low_block` in `src/probe.c` 86-97) is not on the
  `WaitGame` path. The block is committed at the end of the eboot reservation. If it were missing,
  `OnTick` would return early and `WaitCam` would wait forever. No base log shows
  `SOTC ENGINECMD: no free block`.
- `CheckGameWait` (engine_cmd.cpp, new) prints the give-up line from the engine tick, so it works even if
  the route does not call it.

## 4. Cause and fix (proposed)

1. **Cause (inferred):** the stale check as run in the `--env` of batches `035053` and `035215`. It
   costs CPU in proportion to the binds it compares, which slows the load to 1-3 FPS.
   The route then waits for a "starting game" line that comes 6-8 s later than usual, and the run ends
   before or shortly after the route starts.
   - Not proven. Evidence against a pure cost explanation: S2 (`035657`) compared fewer pages and was also
     slow. The two diagnostic switches it adds could be part of it.
2. **Fix for the matrix (not made here, `tools/pcmem_matrix.py`):** do not pass `SOTC_STALE_CHECK` to
   timed runs; run the check in its own batch. Also record the process return code and whether
   `LAB ROUTE exit` was seen in `summary.md`.
3. **Fix for the route (done in part):** the give-up line (section 5). The quit needs the diff in
   section 6, so a stalled run ends by itself at `SOTC_LAB_ROUTE_WAIT` seconds (default 180) instead of
   waiting for an external close.

## 5. Change made (engine_cmd.cpp, engine_cmd.h)

- `OnTick` calls `CheckGameWait(frame)` first. With `SOTC_LAB_ROUTE` set, the wait for the first
  "starting game" line is bounded by `SOTC_LAB_ROUTE_WAIT` seconds (default 180, 0 = no limit), counted
  from the first engine tick. On expiry it prints once:
  `SOTC LAB ROUTE gave up waiting for gameplay after N s: no "st_gameflow() starting game" on stdout; engine ticks T, console seen|never seen, command block ok|missing`
  and sets an atomic flag read by the new `SotcEngineCmd::GameWaitExpired()`.
- The ticks count tells the two cases apart: ticks still advancing means the update thread runs and the
  text did not reach stdout; ticks flat means the update thread is stuck.
- Syntax checked with `clang++ -fsyntax-only` (flags from `hook_handlers.cpp`, because
  `out/gpu/compile_commands.json` is from 29 Oct and does not list `engine_cmd.cpp`).

## 6. Diff for `gpu/sotc/lab_commands.cpp` (not applied; not this pass's file)

```diff
     case AutoPhase::WaitGame: {
         const uint64_t starts = SotcEngineCmd::GameStarts();
         const int64_t last = SotcEngineCmd::LastGameStartMs();
         if (starts > 0 && now - last >= static_cast<int64_t>(a.delay_s * 1000.0)) {
             a.phase = AutoPhase::StartCam;
+        } else if (starts == 0 && SotcEngineCmd::GameWaitExpired()) {
+            // sotc: gave up waiting for gameplay (engine_cmd.cpp printed why): close as the exit does.
+            a.phase = AutoPhase::Closed;
+            SDL_Event quit{};
+            quit.type = SDL_EVENT_QUIT;
+            SDL_PushEvent(&quit);
         }
         break;
     }
```

`WaitCam` has the same silent wait (it needs engine ticks). A similar check there (print after 60 s)
would be the next step, but it needs a measured limit first.

## 7. Verification

1. Run base three times with `SOTC_STALE_CHECK` unset and three times with it set, same batch, nothing
   else set. Expected if the check is the cause: the set runs show FPS 1-3 at 10 s and `starting game`
   after 13 s.
2. Run one base with `SOTC_LAB_ROUTE_WAIT=30` and a deliberately slow start (for example the stale check
   set). Expected: one give-up line with the tick count, and (with the section 6 diff) a close with
   `SOTC LAB ROUTE exit` absent and the process ending at about 30 s.
3. Record the return code of each matrix run next to its summary row.

## 8. Corrections to earlier reports

- **Report 12, section 6:** "6 of 14 base runs stalled" mixes two causes. Four runs are S1 (the
  stale-check batches). Two are not base runs at all (section 2.3, S4). The 'window close' explanation
  in report 12 section 3 fits S1 only as an inference.
- **Report 10, H3 and open question 2:** `SOTC_RELEASE_ORDER` defaults to **on**
  (`src/runtime_memory.c` `release_order()` returns true unless the variable is `0`). The release
  hole is therefore only the cases the loop does not cover (section 9 below), not the default.

## 9. Open questions

1. Which switch made S2 slow and S1 stall: the stale check alone, or its combination with the other
   two switches in `035657`?
2. Why were 032557 and 035619 started with `SOTC_PC_RING`, `SOTC_PC_POOL_NATIVE` or `SOTC_HOOK_ALLOC_PROBE`
   set, when the matrix strips `SOTC_*`? (Possibly an earlier matrix version or a manual run.)
3. Did the user close the S1 windows by hand? If not, the process end is still unexplained.
4. `runtime_memory.c` release order: pending unmap hooks queued by other threads run after the zeroing
   when the releasing thread's own unmap count is zero. That is the gap in report 10 section 3 H3; it is
   not the S1 cause (no stall signature involves zeroed memory).
