# 12. poolflex Lab run: hang in loading and exit at 20 s

Scope: analysis only. No source, script or configuration was edited and no game run was started. The logs are in
`user/pcmem/20261101-042755/logs/` (`poolflex-r1-a1.log`, `rb2-r1-a1.log`, `base-r1-a1.log`, `ring7-r1-a1.log`).
The diffs below are proposals; none is applied.

## 0. Verdict

1. **The run did not test native execution.** The log says `SOTC_PC_POOL=2` and `SOTC_PC_POOL_NATIVE=1` were both
   set, so the pool module refused the session. The native entry hook was never installed. The run was the verify
   wrap plus the allocator probe, with the wraps inert.
2. **The configuration was wrong.** The current `tools/pcmem_matrix.py` `poolflex` profile does not set
   `SOTC_PC_POOL=2` (dry run confirmed). The file is untracked and was last written at 04:33:33; the run's
   `summary.md` was written at 04:30:57. The profile used at run time almost certainly had `SOTC_PC_POOL=2`, and was
   corrected after the run (its comment now says the variable "must stay unset").
3. **The inert pool hooks do not cause the stall, as far as the code shows.** They take no game pool lock, and each
   `g_mutex` or `g_map_mutex` they take is released by RAII before return. Details in section 4.
4. **The stall is not pool-specific.** `ring7` (no pool switch at all) stalled in the same batch, with the same guest
   symptom and before the first FPS line. Earlier batches also have stalled `base` runs (6 of 14 logged runs, see
   section 6).
5. **The exit at ~20 s with code 0 is the window-close path.** `gpu/shim/bbgpu.cpp` calls `std::_Exit(0)` after the
   window receives a close request. It prints nothing. No timer, watchdog or Lab route exit can produce this (section 3).
6. **flex-node alone:** not shown safe, but this run gives no evidence against it, because native execution never ran
   in poolflex. Evidence from other runs favours the exact-hit native path (section 7). Apply fix B before testing,
   and do not count one completed run as proof.

## 1. Log comparison

| run | hooks installed | pool hooks | route | last FPS line | game milestone |
|---|---|---|---|---|---|
| base | 2 (ENGINE_CMD) | none | done (181 frames) | 25 s | `starting game` at 7.5 s |
| rb2 | 8 (+PARTICLE, 3x XPR) | none | done (210 frames) | 25 s | `starting game` at 7.3 s |
| poolflex | 6 | PC_POOL_ALLOC, PC_POOL_MAP installed; PC_POOL_NATIVE off | not done | 5 s | no `starting game`, no ENGINECMD, no FREECAM |
| ring7 | 6 (MEMTYPE_FLIP_SITE, PC_RING_HEAP) | none | not done | none | no `starting game` |

Installed hooks in poolflex, in install order (log lines 55-62):
`ALLOC_BLOCK`, `FREE_BLOCK`, `ENGINE_CMD_TICK`, `ENGINE_CMD_PRINT`, `PC_POOL_ALLOC` (0x19ae0, installed),
`PC_POOL_MAP` (0x1af80, installed), `PC_POOL_NATIVE` (0x19ae0, `off(detour: pattern does not match)`).

Line-set diff (timestamps and numbers normalised, `sort -u`):
- Only in poolflex: the two pool hook lines and their counters, the refusal line, the `flip event queue` wait line, and
  one memory-lock line variant. The `PC_POOL_ALLOC` and `PC_POOL_MAP` counters read 1120/s and 1598/s, pre and post
  equal, so the wraps run on every allocation and flush.
- Only in base and rb2: the game milestones (`Loading a save or a new area`, `st_gameflow() starting game`), the
  ENGINECMD and FREECAM lines, the `SOTC LAB ROUTE start/done/exit` lines, and the XPR and PARTICLE hooks.
- Identical within noise in the first 5 s window: memory lock (poolflex held 538.7 ms, base 577.7 ms), FLIP WAIT
  (13.41 ms against 12.86 ms), PRESENT, USLEEP, MUTEX SPIN. The divergence happens after 5 s.

`PS4SyncEvent` and `sceSndxLoaderNotify` "waited Ns" lines are background sound-thread waits. base has 14 of them, so
they are not the signature. The distinguishing line is `SOTC equeue 'flip event queue' waited 5s ... triggered 0`
(poolflex and ring7 only).

Timeline of poolflex:
- 1.7 s: loading screen shown. 2.8 s: `LAB ROUTE ... waiting for gameplay`.
- 5 s: `SOTC FPS 30.2 at 5s`, the only FPS line the run printed.
- About 5.5 s: frame 167 completes at 159 ms (`SOTCSTALL frame 167`). base also has 150-200 ms frames, but it
  recovers. Nothing follows.
- 9.7 s: `Loading screen (the game stopped showing frames)`.
- About 10 s: `flip event queue waited 5s`. The FPS line due at 10 s is missing.
- 15 s: last log line (a background event wait). The 20 s wait line is missing, so the process ended between 15 and 20 s.

## 2. Which thread stopped

- **The FPS report is not on a report thread.** It is written inside `VideoOutDriver::Flip`
  (`gpu/shadps4/core/libraries/videoout/driver.cpp`, function starts at line 608). The 5 s window check is at line 748
  and the report block runs to line 2428. Only the final stderr write is detached (line 2423). Flip runs on the guest
  thread that submits the flip. So a missing FPS line means that Flip has not completed its report point since about
  5.5 s.
- **The flip event is triggered by that same Flip call** (driver.cpp lines 2446-2453, `equeue->TriggerEvent(Flip)`).
  The presenter job queued at lines 610-613 is asynchronous and does not gate the event. The game is therefore waiting
  for a Flip that is not arriving.
- **Which code is stuck cannot be decided from these logs.** Either the guest thread is blocked in its own frame code
  before it calls Flip, or it is blocked inside Flip between line 617 (after `RunPresentJob`) and line 2446. The
  second range includes `UntrackAutoTick`, `SotcLab::OnFrame` (takes a stats mutex), GC and readback code, and
  several `Report*` calls. The 5 s statistics cover only the first window, so they do not show the blocked lock.
- **No lock in the report code itself.** The report only formats counters. No thread named "report" exists.
- **How to decide it:** when no `SOTC FPS` line appears for 12 s, dump the process (`procdump -ma <pid>`, Sysinternals,
  not in the repo) and read all thread stacks. The thread holding the stuck lock or waiting on it will be visible.
  Without a dump, no lock can be named. This is a gap in the logs, not a finding.

## 3. Exit with code 0 at about 20 s

Every code-0 exit I found, checked against the log:

| path | code | prints first | in log? |
|---|---|---|---|
| window close: `gpu/shim/bbgpu.cpp` ~line 430-461, `PollEvents()` returns false on `SDL_EVENT_QUIT` / `SDL_EVENT_WINDOW_CLOSE_REQUESTED` (`gpu/shim/window.cpp` 337-338), then `std::_Exit(0)` (line 460) | 0 | nothing on stdout (`LOG_INFO` only) | consistent |
| Lab route exit: `gpu/sotc/lab_commands.cpp` 765-774 pushes `SDL_EVENT_QUIT` | 0 | `SOTC LAB ROUTE exit` | no; the route never reached `Done` |
| `runtime_restart` on Windows: `src/probe.c` 1569 `TerminateProcess(..., 0)` | 0 | `Runtime: restarting through run.py` | no |
| `runtime_sotc.c` 147 `exit(0)` (guest asked to run another program) | 0 | `STOP: the game asked to execute` | no |
| `src/probe.c` 215 `_exit(20)` (unresolved import) | 20 | `STOP: first unsupported PS4 import` | no |
| `BB_LIST_IMPORTS` `probe.c` 1878 | 0 | import listing | no |

- No route timeout exists. `AutoPhase::WaitGame` waits for `GameStarts() > 0`, which needs the game's
  `st_gameflow() starting game` line, and that never came. The route cannot time out before gameplay.
- `sotc.py` has no watchdog. It passes the child's return code through (lines ~382-410).
- The matrix kills only after 600 s (`taskkill /F`, which gives a non-zero code), and `launch()` ignores the return code.
- `engine_cmd.cpp` has no quit path. The `quit` control-pipe command (`lab_commands.cpp:650`) only returns `ok`.

So the process ended by a window close request, which the log cannot attribute. Nothing in the repo or the matrix
sends one. The user who watched the infinite loading screen is the likely source, which fits the 20 s timing: the last
wait line is at 15 s and the next is due at 20 s.

## 4. Pool hooks in the refusal path

### 4.1 Install order and the native refusal

- The loader installs hooks in declaration order (`scripts/hooks.d/*.py`). `PC_POOL_ALLOC` (wrap, 0x19ae0) is installed
  before `PC_POOL_NATIVE` (entry, 0x19ae0).
- The pattern check runs on the bytes as they are at install time, after earlier detours
  (`gpu/sotc/cpu_patches.cpp` 3036-3056, `pattern does not match`). `PC_POOL_ALLOC` has already rewritten the 0x19ae0
  prologue, so `PC_POOL_NATIVE` fails its pattern. The live loader and `tools/re/hooks_install_check.py` agree on the
  outcome (the second one is refused), but disagree on the reason: the checker reports "displace overlapping bytes"
  from the original image.
- The comment in `gpu/sotc/pc_pool.h` (about line 21) and `scripts/hooks.d/pcpool.py` says the loader refuses "by
  overlap". The reason is the pattern. The comment should be corrected (not a functional issue).

### 4.2 The refusal does not remove the wraps

The wrap hooks are declared with `switch='SOTC_PC_POOL'` only. `Refused()` is checked inside the handlers, so the
refused wraps are installed and run as no-ops. This is why poolflex shows `PC_POOL_ALLOC` and `PC_POOL_MAP` at
their full call rates.

Handler behaviour with both switches set (all line numbers in `gpu/sotc/pc_pool.cpp`):

| handler | what runs before any return | lock taken | released |
|---|---|---|---|
| `AllocPre` (1521) | `lock_guard g_mutex`; resets `g_call`; `if (!Enabled()) return Continue` | `g_mutex` | yes (RAII) |
| `AllocPost` (1568) | `lock_guard g_mutex`; `if (!g_call.active) return` | `g_mutex` | yes |
| `BatchPre` (1402) | `if (!Enabled()) return Continue` | none | n/a |
| `BatchPost` (1448) | `lock_guard g_map_mutex` **unconditionally**; `if (!g_batch.active) return` | `g_map_mutex`, under the game's pool lock (`vmem_batch_map` runs under it) | yes |
| `NativeEntry` (1967) | not installed here; would return `Continue` at `!NativeActive()` before any lock | none | n/a |
| `OnFreeNode` (2056) | `NativeActive()` and `Enabled()` both false, returns before any lock | none | n/a |
| `Report` (2081) | returns before taking `g_mutex` | none | n/a |

- `TakeGameLock` (1334) is called only from `ReadPoolsLocked` (1356, called from `AllocPre` at 1540 and `AllocPost`
  at 1598, both behind `Enabled()`) and from `NativeHost::TakeLock` (1758, native only). In poolflex neither runs.
- **Conclusion:** the inert handlers do not take the game pool lock, do not leak `g_mutex` or `g_map_mutex`, and do not
  create a lock-order inversion. A hang caused by them would need a bug outside these paths.
- **Cost, not a hang:** `AllocPre` and `AllocPost` take `g_mutex` on every allocation (about 1120 calls/s each in the
  log), and `BatchPost` takes `g_map_mutex` on every flush under the game's pool lock (about 1598/s). These serialise
  allocations across threads for no benefit. This is the reason for fix A.

### 4.3 Lock order in native mode (not reached in poolflex)

- Documented order: `g_mutex`, then the game pool lock, then `g_map_mutex`.
- `FreePre` (`gpu/sotc/probe_alloc.cpp` 488-490) calls `OnFreeNode` before its own lock. When `NativeActive()`,
  `OnFreeNode` takes `g_mutex`. If the free call runs while the game holds its pool lock, that order is reversed
  against `NativeEntry` (which takes `g_mutex`, then spins on the pool lock).
- The spin is bounded (`kLockSpins = 1 << 20`, `pc_pool.cpp` 1031). `NativeEntry` gives up as `LockBusy`, releases
  `g_mutex` and runs the original, so this is a bounded stall, not a deadlock. Whether `FREE_BLOCK` runs under the pool
  lock is not established here.
- `Controller::Run` (`pc_pool.cpp` 874-979) releases the game lock on every return path after `TakeLock`. The only
  unreleased path would be an exception escaping `Predict`, `Compare` or `Observe`. The handler has no try/catch.
  The symptom of a leaked game lock would be guest threads spinning on the lock word, which differs from the poolflex
  signature (waits).

## 5. The writer note: PoolGuestMem::Write versus ReadAny

- `ReadAny` (`pc_pool.cpp` 1235) tries the backing view first (`ReadGuest`, which requires a mapped region), then the
  image.
- `PoolGuestMem::Write` (1699) tries the image first, then the region. The orders differ.
- `Tx::Write` probes with `base_.Read`, which is `ReadAny`, and then `CommitTx` calls `Write`. The overlay therefore
  validates an address with one rule and writes it with another. They agree unless an address lies in the image range
  and is also reported as a mapped region. In that case the model reads the backing view and the commit writes the
  image, so the next read disagrees and the verify kills native execution. That is a kill, not a hang. Whether the image
  range is ever a mapped region is not verified here: check with `runtime_memory_region(ImageBase + kRootGlobal)` at
  startup.
- `Tail()` writes the peak counters (`kLowPeakGlobal`, `kHighPeakGlobal`) through `PoolGuestMem::Write`. Those are image
  addresses. The eboot is write-tracked (`SOTC WRITE FAULTS: eboot 957/s` in poolflex), so a host write into the image
  can take the fault path. That path is not analysed here.

## 6. Is the stall pool-specific? No

Route completion across all logs in `user/pcmem/` (tally by `LAB ROUTE done`):

| config | completed / runs |
|---|---|
| base | 8 / 14 |
| static | 5 / 9 |
| ring | 6 / 8 |
| chunks2 | 3 / 5 |
| pool2 (verify wrap only) | 2 / 3 |
| poolnative (native, exact paths, probe) | 3 / 3 |
| rb2, rt | 5 / 5 each |
| probe | 3 / 3 |
| poolflex | 0 / 1 |
| ring7 | 0 / 1 |

- base stalls in 6 runs, none with pool switches. The stalled base runs are in batches `032557`, `035053`, `035215` and
  `035619`. Their cause is not analysed here.
- So a single run of any configuration proves little. The stall rate is high across configurations.

## 7. Is flex-node alone safe to test?

- **Native entry path in exact mode is clean in the logs.** `poolnative` (`SOTC_PC_POOL_NATIVE=1` plus the alloc probe,
  default paths) completed 3 of 3 routes. Each run logs `calls ~5000-6500, native 25-30, verified 25-30, kills 0,
  lock busy 0, snapshot failed 0, refused 0`. Note `poolnative-r1` in `035931` ran at p50 75 ms against 33 ms for the
  others; not investigated.
- **The flex-node writer (`ApplyNodeHit`, `pc_pool_native.cpp` 586) has not been seen in a logged route.** The native
  summary lines do not split by path, and the only run with `flex-node` in `SOTC_PC_POOL_PATHS` is poolflex, where
  native never ran.
- **Two requirements for the test:**
  1. Include `SOTC_HOOK_ALLOC_PROBE=1`. Without `FREE_BLOCK`, the native side turns itself off with one line
     (`FREE_BLOCK hook is off (SOTC_HOOK_ALLOC_PROBE): native execution off`). The configuration in the request omitted it.
  2. Do not set `SOTC_PC_POOL`. The matrix profile is now correct.
- **Recommendation:** apply fix B first, because the flex-node writer relies on the write path. Run `flex-node` next to
  `poolnative` and `base` in one batch, at least three rounds each. Keep the stall dump in mind: a stalled run is not
  attributable to the pool unless the dump shows the game pool lock held (a leaked lock would show guest threads
  spinning on the lock word).

## 8. Proposed fixes (diffs, not applied)

### Fix A: do not install the wraps when the session is refused (`scripts/hooks.d/pcpool.py`)

```diff
+import os
+import re
+
 EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'
+
+
+def _atoi(name):
+    """The value of an environment switch as C atoi() reads it (pc_pool.cpp WrapSwitch, NativeSwitch)."""
+    match = re.match(r'\s*([+-]?\d+)', os.environ.get(name, ''))
+    return int(match.group(1)) if match else 0
+
+
+# Both switches set: pc_pool.cpp refuses the session (Refused()). An installed wrap still runs its handler on every
+# allocation and flush (g_mutex, g_map_mutex), so the pool hooks are not declared at all.
+REFUSED = _atoi('SOTC_PC_POOL') == 2 and _atoi('SOTC_PC_POOL_NATIVE') == 1
 
-HOOKS = [
+HOOKS = [] if REFUSED else [
```

Notes: `run_script` in `sotc.py` does not pass an env, so the script inherits the Lab environment. The `_atoi` helper
mirrors the C side (`atoi`, so `"2x"` counts as 2). The hook count in the log drops by 3 when refused.

### Fix B: `PoolGuestMem::Write` uses the same order as `ReadAny` (`gpu/sotc/pc_pool.cpp`, class at line 1699)

```diff
     bool Write(uint64_t address, const void* data, size_t n) override {
-        if (InImage(static_cast<uintptr_t>(address), n)) {
-            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), data, n);
-            return true;
-        }
+        // The same order as ReadAny (backing view first, then the image), so a write lands where its read came from.
         uintptr_t start = 0, end = 0;
         int mapped = 0;
-        if (!runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) || !mapped) {
-            return false;
-        }
-        if (address < start || address + n > end) {
-            return false;
-        }
-        unsigned char* p =
-            const_cast<unsigned char*>(runtime_memory_backing_pointer(static_cast<uintptr_t>(address)));
-        if (!p) {
-            return false;
-        }
-        std::memcpy(p, data, n);
-        return true;
+        if (runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) && mapped &&
+            address >= start && address + n <= end) {
+            unsigned char* p =
+                const_cast<unsigned char*>(runtime_memory_backing_pointer(static_cast<uintptr_t>(address)));
+            if (p != nullptr) {
+                std::memcpy(p, data, n);
+                return true;
+            }
+        }
+        if (InImage(static_cast<uintptr_t>(address), n)) {
+            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), data, n);
+            return true;
+        }
+        return false;
     }
```

This changes behaviour only where the two rules disagree. Those cases currently produce a verify kill.

### Not diffed: documentation

- `gpu/sotc/pc_pool.h` lines 20-22 and `scripts/hooks.d/pcpool.py` lines 23-24 say "refused by overlap". Correct to
  "refused by pattern: PC_POOL_ALLOC rewrote the bytes first".

## 9. Open items

- Get a thread dump at the stall (section 2). Without it, the stuck thread and lock are unknown.
- Confirm whether the image address range can be a mapped guest region (section 5).
- Confirm whether `FREE_BLOCK` runs under the game's pool lock (section 4.3).
- Find the cause of the base stalls in earlier batches (section 6). Until then, every single run is noise.
- Only the `poolflex` profile in `tools/pcmem_matrix.py` was corrected after this run. It is untracked, so the earlier
  version cannot be diffed.
