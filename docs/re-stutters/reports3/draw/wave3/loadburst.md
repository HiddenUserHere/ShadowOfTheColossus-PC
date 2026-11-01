# loadburst: SOTC_LOAD_STREAM (wave 3, streaming and upload)

Status: built behind a switch, syntax-checked only. No game run (brief rule 2). Off by default.
Edits are uncommitted in the worktree (branch worktree-agent-a9fa3629902eda37b; the merge commit
0c45ce7 with re/stutter-architecture predates the no-commit rule and stays as it is).

## Evidence: what the 139 MiB is

`tools/trace/sotc_trace.py copysrc` on `user/trace/20261101-085933/frame000167_152ms.sotctrace`,
frame-only window (frames 167-169):

| reason | copies | MiB | notes |
|---|---|---|---|
| bind | 1083 | 140.6 | every copy has an address |
| dma | 620 | 35.6 | |

- Bind size histogram: 768 copies under 64 KiB (12.7 MiB), 292 at 64 KiB to 1 MiB (50.3 MiB), 22 at
  1 to 8 MiB (66.3 MiB), 1 at 8 to 32 MiB (11.2 MiB). The bytes are mid-size buffers.
- Dirty origin of the bind bytes: 23% in regions first touched in this window (43 markers, 688 MiB),
  14% "other" re-dirty, 1% write faults. About 63% had no re-dirty marker in the window: dirtied
  before it, bound for the first time now (backlog).
- Top regions (64 MiB, direct memory): 0x10e8 32.4 MiB, 0x10ec 20.4, 0x105c 19.3, 0x10a0 12.3.

Code facts behind it:
- Sync copies of 2 MiB and more are already split into 512 KiB pieces (`BbCopy::ParallelFor`,
  `SOTC_COPY_BATCH`), but the draw thread waits for them.
- `SOTC_ASYNC_UPLOAD` (copy-thread pool, `async_upload.*`) moves the copy off the draw thread. It was
  off by default and only ran when its own switch was on.
- `SOTC_LOAD_WARM` warms only pages learned for the same event key. A first visit to an area has nothing
  to warm. FIRST-FRAME-PEAK.md found no signal for first-touch data (file reads 1%; map-time tracking
  rejected).

## What was built

Switch `SOTC_LOAD_STREAM=1|2` (2 = verify). Read once in each file that uses it. `SOTC_LOAD_STREAM_MB`
(1-256, default 16) caps the idle drain per presented frame.

Three parts, all on the draw thread's idle gaps or behind an existing pool:

1. **Session live set** (`gpu/sotc/load_warm.cpp`, `NoteBind` to `NoteStreamRange`). The draw thread
   records every bound range, split into 1 MiB pieces, for the whole session (not only the 20 s window).
   Thread-local, direct-mapped repeat cache, at most 32768 pieces per thread.
2. **Idle drain** (`StreamIdle` in load_warm.cpp, called from `Liverpool::BackLoop`'s idle branch in
   `gpu/shadps4/video_core/amdgpu/liverpool.cpp`). Round robin over the live pieces, at most 64 calls per
   gap, 1 ms budget, stops when work arrives, per-frame byte cap. Each call is `BufferCache::DrainRange`.
3. **Copy threads for early syncs** (`buffer_cache.cpp`, `SynchronizeMemoryFull`; `async_upload.*`). An early
   sync (`WarmRange`, `DrainRange`) may hand its copies to the pool when `SOTC_LOAD_STREAM` is on, even
   with `SOTC_ASYNC_UPLOAD` off. `AsyncUpload::Init` creates the pool for the stream switch alone;
   `buffers_enabled` still follows `SOTC_ASYNC_UPLOAD`, so the other upload paths keep their behaviour.

Clean-range check: `DrainRange` calls `memory_tracker->IsSyncClean(addr, size, false)` first. A clean range
costs one read-only tracker look and no copy.

## The reusable API (for the PC-native memory model)

- `VideoCore::AsyncUpload` (async_upload.h): `Get()`, `StreamEnabled()`, `BuffersEnabled()`, `Queue(...)`,
  `WaitUpTo(owner, tick)` (the submit's fence), `WaitVa`, `WaitPhys` (write fence). Unchanged apart from
  the pool creation.
- `BufferCache::DrainRange(VAddr addr, u64 size) -> u64`: the (addr, size) upload entry point. Draw thread
  only. Syncs CPU-dirty bytes of the range into the arena; the copy goes to the copy threads when the
  early-sync marker is on; the staging-to-arena copy is recorded now and the next submit waits for the
  copy threads (`WaitUpTo`). Returns bytes uploaded (0 when clean).
- `BufferCache::WarmRange(addr, size)`: the same, for the learned set (`SOTC_LOAD_WARM`).
- `SotcLoadWarm` (load_warm.h): `StreamEnabled()`, `StreamVerify()`,
  `StreamIdle(budget_ns, frame, drain_fn, stop_fn, ctx)`, `NoteStreamAsync(bytes)`,
  `NoteStreamBind(bytes, ns)`, `NoteStreamVerifyBad()`.

What is not yet reusable: the entry point is draw-thread only, and it still goes through `ObtainBuffer`
(residency, bind memo, reason tags). A game-announced upload would call `DrainRange` for each announced
range; its own fence is the existing submit wait.

## Counters (5 s report, `SOTC LOAD STREAM:` line in the warm's writer)

- MiB drained in uploads (`s_stream_drained`, upload count, calls).
- MiB handed to copy threads by early syncs.
- MiB copied by real binds on the draw thread, and the worst single bind sync in ms (`NoteStreamBind`).
  The per-frame sum is not recorded (only the worst single bind sync).
- `verify mismatches` (=2 only): a range not clean after `DrainRange`'s sync.

## Safety argument

- An early sync is an ordinary `ObtainBuffer` sync: same `SynchronizeMemory`, same tracker
  (`ForEachUploadRange` clears CPU bits, re-protects). A bind after it finds clean bytes, and any later guest
  write faults and is copied at the next bind, as for any early bind (SOTC_LOAD_WARM already relies on
  this).
- Copies on the copy threads use the write fence and submit wait of `SOTC_ASYNC_UPLOAD` unchanged. The
  early marker only changes who copies, not what.
- The verify mode asserts `IsSyncClean` after the sync. A guest write racing the sync can trip it, so it is
  a count to read, not proof of a wrong copy.

## Risks and limits

- The drain may upload ranges never bound again. Capped (16 MiB per frame), only session-bound ranges.
- First-touch data (~23% of the 139 MiB) is not covered: no signal before the first bind. That needs the
  learned set (SOTC_LOAD_WARM) or the loader-completion hook (STUTTER-ARCHITECTURE step 5). Not done.
- The image side (709 images in one frame) is not touched. `SOTC_AREA_PREWARM=3` is the existing route.
- Expected effect: the drain targets the backlog (about 63%). Unmeasured.

## Syntax check

`-fsyntax-only` with the main build's own command (cwd `out/gpu`), with the worktree's
`gpu/shadps4` placed before the main tree's, passing for: `gpu/sotc/load_warm.cpp`,
`gpu/shadps4/video_core/buffer_cache/async_upload.cpp`, `buffer_cache.cpp`, `amdgpu/liverpool.cpp`.
Limit: the worktree predates main's `gpu/shim/common` replacements, so the shim comes from the main tree.
The patch (`git diff` of the worktree) applies cleanly, by dry run, to the main tree's current versions of
all seven files. It has not been compiled against the main tree's headers.

## Test plan (needs the user's authorization, per HANDOFF)

1. One round, verify first: `SOTC_LOAD_STREAM=2` against default. `verify mismatches` should stay 0.
2. Then `SOTC_LOAD_STREAM=1` against default on the Lab route. Read: MiB drained, MiB to copy threads, MiB
   copied by binds on the draw thread, worst bind sync ms, and the frame time at the area load.

## Not done

- No game run, no build (`ninja`), no commit (per the new rule).
- Loader-completion or zone-activation trigger for first-touch data.
- Per-frame upload sum counter.
