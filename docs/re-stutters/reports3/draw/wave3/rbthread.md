# Wave 3 rbthread: GPU to CPU readback path analysis

Scope: analysis only, no code change. The partial edits made earlier were reverted
(`git checkout -- gpu/shadps4/video_core/buffer_cache`). The coordinator superseded the stale-redo design with
a plan that removes GPU to CPU readbacks at the source (patching the game's consumers).

## 1. The path, step by step

1. **Request.** A guest thread reads or writes a page the GPU modified since the last apply. The page
   protection fires and `BufferCache::ReadMemory` (`gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`,
   ~line 1024) runs on that guest thread. The entry is `Rasterizer::ReadMemory` (`vk_rasterizer.cpp:7007`),
   called from `page_manager.cpp:1477`.
2. **Begin (draw thread).** The guest thread sends `BeginAsyncReadback` with `SendCommand<true>`. It runs on
   `shadPS4:GpuDraw`, between two ops, in `Liverpool::ProcessCommands` (`liverpool.cpp:2634`). On the draw
   thread: `GatherDownloads` collects the GPU-modified copies, `staging_pool.Request` takes staging, the copy is
   recorded into the tick being built (or into an ahead or transfer-queue command buffer), then
   `scheduler.Flush()` submits without waiting (`vk_scheduler.cpp:283`).
3. **GPU wait (guest thread).** The guest waits for the tick: `WaitSubmitted`, the work semaphore, and the
   `XferWaitValue` spin. This wait is already off the draw thread.
4. **Finish (draw thread).** The guest sends `FinishAsyncReadback` with `SendCommand<true>`.
   - Clean: `Invalidate`, `ApplyDownloads` (BPE heap guard, memory tracker unmark, `MarkInflightReadbacksDirty`),
     `SubtractDownloaded`, staging freed deferred.
   - **Dirty** (a GPU write recorded after the copies, or another readback applied first): `DownloadMemory`
     runs synchronously on the draw thread (`buffer_cache.cpp`, dirty branch of `FinishAsyncReadback`, ~line
     4509). It calls `scheduler.FinishHostRead()` (`vk_scheduler.cpp:296`: submit, `WaitSubmitted`,
     `work_semaphore.WaitHostRead`) or `SubmitAheadAndWait()` (`vk_scheduler.cpp:355`). Both wait for the GPU to
     finish all submitted work, on the draw thread. Then it applies the bytes.

So the draw thread blocks on the GPU for every stale guest readback. That drain is the target of this analysis.
The clean apply is CPU work on the draw thread, not a GPU wait.

Other synchronous callers of `DownloadMemory`: the prefetch-stale branch of `FinishAsyncReadback`, the
non-async path (`SOTC_RB_ASYNC=0`), and the `assume_locks` path of `ReadMemory`.

## 2. Measured counters

Source: `user/pcmem/20261101-085630/logs/default-r1-a1.log`, one run with five consecutive 5 s reports
(the "FPS" line starts each window). The `drawbest-r1-a1.log` in the same folder has only its first window with
stale redos (36 in the first window, line 248); its later windows were not read.

| 5 s window (log line) | readbacks/s (FPS line) | stale redone synchronously | made stale: later GPU write / another readback | RIPS readbacks/s (waited) | guest waits: count, total |
|---|---|---|---|---|---|
| 1 (225, 241, 301) | 85 | 67 | 183 / 0 | 48.5 | 358, 4021 ms |
| 2 (365, 381, 438) | 1217 | 1346 | 2045 / 1246 | 931.1 | 4610, 2312 ms |
| 3 (~480-542) | see note | 1616 | see note | 1077.5 | 5424, 1595 ms |
| 4 (~590-658) | see note | 1245 | see note | 877.6 | 5180, 2002 ms |
| 5 (~700-769) | see note | 1120 | see note | 812.5 | 4640, 2352 ms |

Note: the readbacks/s and the "made stale by" lines for windows 3-5 were not extracted in this pass; the
values above are the ones read. Windows 2-5 are the heavy ones.

Draw-thread part of the guest wait (`SOTC DRAW SPLIT`, per frame, `guest readback waits X ms [queue, submit,
gpu, finish]`):
- Window 2 (line 357, 129 frames): 35.7 ms [2.5, 1.5, 8.8, 5.1].
- Window 3 (line 477, 148 frames): 36.6 ms [0.9, 1.3, 7.0, 1.6].
- Window 1 (line 219, 151 frames): 1.1 ms [6.9, 0.0, 1.2, 3.5] (the "finish" slice is the draw-thread side).

Draw work and readbacks (`SOTC DRAW WORK`, per frame): readbacks 17.74 ms in window 2 (line 351), 10.78 ms in
window 3 (line 471), 13.84 ms in window 4 (line 575), 17.82 ms in window 5 (line 685). Ops are 30-36 ms/frame in
the same windows.

`SOTC RB pattern` (window 2, line 370): 4649 readbacks, 4610 waited. 52% hit the same window as the previous
frame, 16% are adjacent to the same thread's previous window. Average window 64 KiB, average downloaded
14.3 KiB. The top windows repeat 240-350 times in 5 s.

`SOTC GPU writes by shader` (window 2, line 371): shader `0x50784801/b2` marked 6.73 MB in 14029 marks and was
the source of 1051 readbacks at offsets +0..+16 of its marks. Readbacks outside any mark: 3714.

## 3. Which guest code issues the readbacks

From `SOTC READBACK RIPS` (guest instruction RIP at the fault, eboot offsets), window 2 (line 438: 931
readbacks/s waited for the GPU at 41 guest instructions). Top entries:

| eboot offset | readbacks/s | Ghidra function (out/re/decomp) | size | callers (decomp header) |
|---|---|---|---|---|
| 0x1a6d60 | 363.4 | FUN_001a6d20 (+0x40) | 1377 | FUN_0019af20 |
| 0x101a98 | 194.6 | FUN_001018c0 (+0x1d8) | 1350 | FUN_00109970 |
| 0x1a6d3a | 60.5 | FUN_001a6d20 (+0x1a) | 1377 | FUN_0019af20 |
| 0x1a8b38 | 48.4 | FUN_001a8a40 (+0xf8) | 534 | FUN_0019c740 |
| 0x0f9c40 | 47.8 | FUN_000f9c40 (+0) | 76 | FUN_000fb1e0, FUN_000fb2d0 |
| 0x101a10 | 33.6 | FUN_001018c0 (+0x150) | 1350 | FUN_00109970 |
| 0x19d624 | 22.5 | FUN_0019d410 (+0x214) | 9092 | FUN_0019d340, FUN_000cefa0, FUN_000cda40 |
| 0x0937fd | 22.3 | FUN_000937c0 (+0x3d) | 124 | FUN_000b9710, FUN_000a69d0, FUN_005d5ec0, ... |
| 0x101ab2 | 19.3 | FUN_001018c0 (+0x1f2) | 1350 | FUN_00109970 |
| 0x11d940 | 18.1 | FUN_0011d7a0 (+0x1a0) | 1066 | FUN_000c1860, FUN_000ce850, FUN_000cd330, ... |
| 0x1a8c83 | 16.5 | FUN_001a8c60 (+0x23) | 358 | FUN_001a8a40, FUN_001a8dd0 |
| 0x1a7215 | 13.9 | FUN_001a6d20 (+0x4f5) | 1377 | FUN_0019af20 |

The first two rows (FUN_001a6d20 and FUN_001018c0) account for about 60% of the readback RIPs in that window.
Other offsets in the same log (0x17b02, 0x135719, 0x48ac5, 0x49800, 0x49151, 0x490a0, 0x492d9, 0x49843,
0x1432bc, 0xa6ce08) map to FUN_000179c0, FUN_00135690, FUN_00048a70, FUN_000497f0, FUN_00049030, FUN_000492d0,
FUN_001432b0 and FUN_00a6cd80 (a lower-volume tail, from window 1).

Caveats:
- The mapping assumes that the eboot offset in the log equals the Ghidra address. This is not verified: the
  Ghidra image base was not checked against the eboot load base. Confirm before acting on it.
- `out/re/ghidra/names.txt` and `names2.txt` have no labels for these functions (only import names and a few
  addresses). The names here are the `FUN_` placeholders from `out/re/decomp/<hi16>/<addr>.c`; the decomp
  headers give the size and callers. What the functions do is still open.
- The `called by` lists come from the decomp headers, not from a call-graph walk.

## 4. What this means for the plan

- The largest avoidable draw-thread wait is the synchronous re-read of stale guest readbacks (step 4, dirty
  branch). Stale redos run about 1100-1600 per 5 s in the heavy windows, each a GPU drain on the draw thread.
  This is the item the superseded design targeted.
- The clean apply (`ApplyDownloads`, about 3.3 ms per frame in the 1 Nov profile) stays on the draw thread. It
  is not a pure copy: it unmarks the memory tracker and marks inflight readbacks dirty (walking the draw-thread
  inflight list), uses static BPE heap guard slots, and a draw-thread `thread_local` (`tl_rbp_skipped`). Moving
  it needs its own locking design.
- Readbacks that need not happen: the windows repeat (52% same window as the previous frame; the top windows
  repeat 240-350 times in 5 s). Removing them needs the consumers to read a different source (the source-side
  plan) or a GPU-side change compare. Neither is implemented here.
- `SOTC_RB_OWNER=1` (overlapping windows of several guest threads; "made stale by another readback" 1246 in
  window 2) is the first existing off-by-default switch to measure.

## 5. Evidence sources

- `user/pcmem/20261101-085630/logs/default-r1-a1.log` (counters and RIPS above) and `drawbest-r1-a1.log`
  (window 1 only).
- `user/trace/20261101-085933/hitch.md`: rb finish and command spans of 8-17 ms in 17 of 40 files. The link to
  the synchronous re-read is inferred, not measured.
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`: `ReadMemory` (~1024), `BeginAsyncReadbackImpl` (~1287),
  `FinishAsyncReadback` (~4432), `DownloadMemory` (~4591), `ApplyDownloads` (~4902).
- `gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp`: `Flush` (283), `FinishHostRead` (296),
  `SubmitAheadAndWait` (355).
- `out/re/decomp/<hi16>/<addr>.c` and `out/re/ghidra/names*.txt` (main checkout only; `out/` is not in the
  worktree).

No timings were re-measured for this document. The counter values are the 1 Nov logs as recorded.
