# SotC Lab: intermittent failure of the free camera and engine commands

Status: investigation only. No repository file was edited and the game was not launched.
Labels: **Observed** = seen in the session logs or in the source text. **Inferred** = a conclusion from
the code or logs that was not reproduced. **Unknown** = not determined.

## Summary

- The route waits for gameplay, then starts the free camera, and the free camera needs the engine
  console tick (`ENGINE_CMD_TICK`) to run the engine lines it asks for. The route only prints
  `SOTC LAB ROUTE start` after the free camera reports `active` with at least one flown frame.
- In every failing run that reached the free camera, the engine executor could not get a 64 KiB block
  below 2 GiB. The message `SOTC ENGINECMD: no free block below 2 GiB, live commands off` was printed
  once, the executor stayed off for the rest of the process, the engine lines were silently dropped,
  the free camera never got its start pose, it gave up with `SOTC FREECAM: no start pose from the
  engine, staying off`, and the route kept retrying without a log line. The log still said
  `waiting for gameplay`.
- The failure is not tied to any `SOTC_*` switch. A base-config failing run and a base-config working run
  have the same switches. A failing run has only the two engine hooks installed.
- The exact occupant of the sub-2 GiB address range was not identified from the logs or the code.
- Also, the engine pipe command and the free camera toggle report `ok` even when the executor is off.
  That makes the failure look like a silent hang.

## Evidence set

Logs are in `C:\Users\ptr\AppData\Local\Temp\claude\C--Users-ptr-Documents-sotc-pc\5c619f39-4d55-404e-9cbb-38a52993d236\scratchpad\`.

| Log | Result | `no free block` | `FREECAM on` | `no start pose` | `ROUTE start` | Hooks installed | Other switches seen |
|---|---|---|---|---|---|---|---|
| rbv2_mat.out | working | 0 | 1 | 0 | 1 | 5 | RB prefetch and materialize on |
| ring_apply1.out | working | 0 | 1 | 0 | 1 | 6 | PC RING mode 1 |
| ring_base1.out | working | 0 | 1 | 0 | 1 | 5 | none |
| waveB_lab1.out | working | 0 | 1 | 0 | 1 | 11 | many |
| ring_shadow5.out | **failing** (see note) | 1 | 0 | 0 | 0 | 5 | PC RING mode 2 |
| rbv2_v2.out | failing | 1 | 0 | 2 | 0 | 5 | RB CLASS on |
| ab_base1.out | failing | 1 | 0 | 0 | 0 | 5 | none |
| ab_base2.out | failing | 1 | 0 | 0 | 0 | 5 | none |
| ab_ring1.out | failing | 1 | 0 | 0 | 0 | 6 | PC RING mode 1 |
| ab_ring2.out | failing | 1 | 0 | 0 | 0 | not checked | not checked |
| rb_base.out | failing | 1 | 0 | 0 | 0 | **2** (engine pair only) | none |
| ab_base3.out | inconclusive | 0 | 0 | 0 | 0 | 5 | none |
| ab_ring3.out | inconclusive | 0 | 0 | 0 | 0 | 6 | PC RING |

Notes on the table:

- **Observed:** `ring_shadow5.out` is listed as working in the task brief, but its log has no
  `SOTC LAB ROUTE start`, one `no free block` line, and no `SOTC FREECAM on` line. It belongs with the
  failing runs.
- **Observed:** `ab_base3.out` and `ab_ring3.out` contain `ENGINE_CMD_TICK` hook counters and the route
  wait line, but no engine line and no freecam line. They end about 90 lines after the game start. The
  route needs `SOTC_LAB_ROUTE_DELAY` (8 s) after the start, then the free camera. **Inferred:** these runs
  were closed before the route reached the free camera, so they say nothing about the bug.
- **Observed:** "starting game" timing (6.89 to 7.81 s) and save load timing (9.31 to 10.50 s) overlap
  between working and failing runs. No timing separation was seen at this granularity.
- **Observed:** `PS4SyncEvent` and `sndxLoaderNotify` "waited 5s" lines appear in both groups (9 to 16
  per log). They do not discriminate.

## Mechanism (chain of events in the failing runs)

1. **Observed, route gate** (`gpu/sotc/lab_commands.cpp:717-724`). `WaitGame` waits for
   `SotcEngineCmd::GameStarts() > 0` and 8 s since the last start. `GameStarts()` is incremented by
   `TakeStdout()` when the game prints `st_gameflow() starting game` (`gpu/sotc/engine_cmd.cpp:213-219`).
   That path does not depend on the engine tick or on `LowBuffer`. In the failing runs the gate was
   passed, because the freecam state machine was reached (step 2).

2. **Observed, freecam start** (`lab_commands.cpp:725-730`). `StartCam` calls `SotcFreecam::SetActive(true)`,
   which sets phase `AskCamera` (`gpu/sotc/freecam.cpp:215-226`). The next engine tick,
   `EngineFrameLine`, returns the line `PrintCameraPos` (`freecam.cpp:303-307`).

3. **Observed, the line is dropped.** `OnTick` calls `EngineFrameLine` first (`gpu/sotc/engine_cmd.cpp:135-139`),
   then calls `LowBuffer()` (`engine_cmd.cpp:146-149`). If `LowBuffer()` returns null, the function returns
   `Continue` with no error, and the line is never written to the guest.

4. **Observed, the failure is cached.** `LowBuffer()` is a function-local static
   (`engine_cmd.cpp:77-96`). Its initializer runs once. If it finds no block, it prints
   `SOTC ENGINECMD: no free block below 2 GiB, live commands off` (line 92) and returns null for the rest
   of the process. No later call retries.

5. **Observed, the start pose never arrives.** Without the executed `PrintCameraPos` and `printPlayerPos`,
   `ReadPose` never sees a `Camera Position: SetCameraPos` line. It times out after `kStartTimeoutFrames`
   = 120 engine ticks (`freecam.cpp:46, 313-327`). It retries 4 times (`freecam.cpp:320-326`). On the
   fourth timeout it prints `SOTC FREECAM: no start pose from the engine, staying off` and sets phase `Off`
   (line 321-322). The `rbv2_v2.out` log shows this message twice, about 16 s apart. That matches its
   roughly 60 s of gameplay.

6. **Observed, the route cannot see the failure.** `WaitCam` (`lab_commands.cpp:731-745`) needs
   `st.active && st.frames > 0`. `frames` only increases in phase `Flying` (`freecam.cpp:354`), which is never
   reached. When `active` drops to false, the route silently returns to `StartCam` after 3 s (lines 741-742).
   The route log line stays at `SOTC LAB ROUTE ...: waiting for gameplay` (line 705). That is the symptom
   the user saw.

7. **Observed, the pipe and lab report success.** `SotcEngineCmd::Queue()` checks only that the hook is
   installed and the console has ticked (`engine_cmd.cpp:267-275`). It does not check `LowBuffer()`, so it
   returns true and `engine` replies `ok` (`lab_commands.cpp:401-404`). `SotcFreecam::Enabled()` checks only
   the lab flag and `SotcEngineCmd::Available()`, which is `IsInstalled("ENGINE_CMD_TICK")`
   (`freecam.cpp:205-208`, `engine_cmd.cpp:263-265`). `freecam on` returns `ok` because `SetActive` returns
   true whenever `Enabled()` is true (`freecam.cpp:215-226`).

## Question 1: what the route waits for, and why it can stay false

- The start gate is the game's own `st_gameflow() starting game` stdout line plus 8 s
  (`lab_commands.cpp:717-722`, `SOTC_LAB_ROUTE_DELAY`). It is set by stdout and does not depend on the
  engine executor. In the failing runs it was passed.
- The condition that stays false is `st.active && st.frames > 0` in `WaitCam` (`lab_commands.cpp:731-733`).
  `frames` depends on `EngineFrameLine` returning `Flying` (`freecam.cpp:343-363`). That requires two
  stdout/print lines (`Camera Position: SetCameraPos`, `Player Position: SetPlayerPos`) produced by
  engine commands that can only run through `LowBuffer()`.
- So the route's readiness depends on the engine executor, and the route has no timeout or error path for
  it. **Observed:** the `WaitCam` reset at lines 741-742 is silent, and nothing after `StartCam` prints a
  message, so the failure is invisible in the log.

## Question 2: why engine commands and the free camera become unavailable

- **Observed, what the allocation is.** One 64 KiB block of read-write memory at a fixed address below 2 GiB,
  holding the `bpe::String` view and the text of one console line (`engine_cmd.cpp:67-73`, `LowBlock` with
  `str[4]` and `text[kMaxLine + 16]`). It is needed because the game keeps 31-bit pointers (the comment at
  lines 11-14 and CLAUDE.md).
- **Observed, how it is found.** `LowBuffer()` scans `VirtualAlloc(addr, 0x10000, MEM_RESERVE|MEM_COMMIT)` from
  `0x7fff0000` down to `0x01000000` in 64 KiB steps (`engine_cmd.cpp:80-90`). It accepts the first address
  that succeeds and returns null if none does.
- **Observed, what it disables.** Only the execution of engine console lines, through `OnTick`. Everything that
  uses engine lines fails with it: the free camera, the autopilot's freecam path, the `engine` pipe command,
  and jumps (`RequestBoss`, `lab_commands.cpp:389`, which returns `live-unavailable` only when `Available()` is
  false, not when `LowBuffer()` is null, so jumps also queue silently).
- **Observed, why it can fail.** The scan found no free granule in
  `[0x01000000, 0x7fff0000]` at the moment of the first engine line (the first `PrintCameraPos`, about
  8 s after the game starts). That is the only explanation consistent with the code: every `VirtualAlloc` at
  every probed address failed.
- **Unknown, the occupant.** I could not identify what occupied the whole sub-2 GiB range at that moment in
  the failing runs. What I checked:
  - The eboot reservation: `src/probe.c:82-106` reserves 256 MiB from the first free range at or above
    `0x400000` and commits only the image size from it. It is not the occupant (it is not free for the
    scan, and it is outside the 64 KiB probe only in part).
  - The guest memory pool view: `src/runtime_memory.c:818-832` maps `POOL_SIZE + FLEX_SPAN` (9152 MiB
    by default, per the comment at lines 820-822) with `MapViewOfFile3(..., NULL, ...)`. **Inferred:** a view
    that large cannot fit below 2 GiB, so it is not the occupant, but a NULL-base mapping is placed by Windows
    and its placement is not controlled here.
  - The guest low-memory allocator `runtime_low_map` (`src/runtime_memory.c:1362-1373`) uses
    `LOW_MIN = 0x800000000` and up. It does not allocate below 2 GiB.
  - The new uncommitted files (`gpu/sotc/pc_ring.cpp`, `probe_memtype.cpp`, `rb_class.cpp`,
    `probe_xprmem.cpp`) contain no `VirtualAlloc`. The alloc hooks only call protection changes.
  - Other sub-2 GiB host allocations come from Windows and the GPU driver, which the repository does not
    control. **Inferred:** the occupant is one of those, but this is not confirmed.
- **Observed, what does not explain it.** The failing runs use the same switch set as the working run that
  shares their configuration (`ring_base1.out` working, `ab_base1.out` failing: 5 hooks each). `rb_base.out`
  fails with only the engine hook pair installed and no alloc hooks. So `SOTC_HOOK_ALLOC_PROBE`,
  `SOTC_PC_RING`, `SOTC_RB_CLASS` and the rest are not the cause. Note that `rb_base.out` has 2 hooks
  installed and the other failing runs have 5 or 6, so the fail case covers both.

## Question 3: race at boot, hooks off, or memory below 2 GiB used up

- **Observed, not a switch.** See above.
- **Observed, not the gate.** The gate is passed in every failing run that prints `no free block`.
- **Inferred, a race at boot.** The scan runs on the first engine line, which is about 8 s after the game
  starts. At that moment the process is busy (loading, Vulkan, guest threads). The outcome depends on the
  instant. The logs show no timing separation, so this is a hypothesis.
- **Inferred, memory use.** Something other than the lab takes the whole sub-2 GiB range at that moment.
  Possibly the driver or Windows, but not confirmed. The fix below removes the dependence on the scan either
  way.
- **Observed, consequence of the static.** Once the scan fails, it never retries in that process. A restart
  of the game is required, which matches "sometimes it works".

## Proposed minimal fix (describe only, not applied)

1. **Reserve the block at boot, from memory this process already owns.** In `src/probe.c`, after
   `allocate_image` commits the image, commit the last 64 KiB of `eboot_reservation`
   (`eboot_reservation + EBOOT_RESERVATION - 0x10000`) with `VirtualAlloc(..., MEM_COMMIT, PAGE_READWRITE)`.
   Require `size <= EBOOT_RESERVATION - 0x10000` (today the check is `size > EBOOT_RESERVATION`,
   `probe.c:101`). Export the pointer, and make `LowBuffer()` in `gpu/sotc/engine_cmd.cpp:77-96` use it
   instead of scanning. The address is below 2 GiB by the `EBOOT_LIMIT` check (`probe.c:86-96`) and cannot
   be taken by anything else, because the eboot range is already reserved for this process.
2. **Do not advance the freecam state when no line can run.** In `OnTick` (`engine_cmd.cpp:117-163`), obtain
   `LowBuffer()` first and return `Continue` if it is null, before `EngineFrameLine` is called
   (`engine_cmd.cpp:135-139`). Then phases `AskCamera` and `ReadPose` are not consumed silently.
3. **Report the failure at the entry points.** `SotcEngineCmd::Queue()` (`engine_cmd.cpp:267-296`) should return
   false with an error when `LowBuffer()` is null. Then the `engine` pipe command returns `err` instead of
   `ok` (`lab_commands.cpp:401-404`). `SotcFreecam::Enabled()` should also report false when the executor is
   off (`freecam.cpp:205-208`), so `freecam on` and the route do not start.
4. **Make the route fail visibly.** In `WaitCam` (`lab_commands.cpp:731-745`), after a bounded number of
   retries or seconds, print one line such as `SOTC LAB ROUTE: free camera did not start (engine commands
   unavailable, see SOTC ENGINECMD)` and stop, instead of retrying silently. Change the wait text to name
   the state.
5. **If fix 1 is not taken,** retry the scan on later calls (do not cache null in the static), and log the
   first failed attempt's `GetLastError()`.

## How to confirm the occupant before changing code (diagnostic only)

- On the first `LowBuffer()` failure, log `GetLastError()` for the first probe address, and walk the address
  space with `VirtualQuery` over `[0x01000000, 0x80000000)`. Print the count of free regions, the largest free
  region, and the base and size of the largest allocated regions. This identifies the occupant directly.
- Log the time of the first `LowBuffer()` call relative to `st_gameflow() starting game`, to test the boot-race
  hypothesis.

## Open points

- The occupant of the sub-2 GiB range was not identified. This needs a run with the diagnostic above.
- `ab_base3.out` and `ab_ring3.out` are inconclusive (ended before the freecam start). They should be re-run
  with a longer session before counting them.
- The workaround until the fix is in place: restart the game when the route shows `waiting for gameplay`
  and no `SOTC FREECAM on` line appears after about 20 s. **Inferred:** the failure is per process, so a
  restart can clear it, which matches the intermittent reports.
