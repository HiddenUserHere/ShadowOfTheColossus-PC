# Draw-thread switches: engagement review

Scope: the seven draw-thread switches tested in one run each (draw busy 35.4 vs 35.7 ms, no
measurable gain): SOTC_TEX_BIND_FAST, SOTC_BUF_BIND_FAST, SOTC_VK_POOL, SOTC_PROTECT_BATCH,
SOTC_PRESENT_UNLOCKED, SOTC_UD_SWAP, SOTC_HLE_TABLE_GEN. Baseline profile:
`drawprof-base-sym.txt` (20 s, 11,446 samples; self-time percentages below are from it).
Code references are to the working tree as of this review.

## Summary

| Switch | Default | Engages when | Proof in the log | Ceiling in the baseline profile |
|---|---|---|---|---|
| SOTC_TEX_BIND_FAST | off | always (table size and texture block mode 1) | `SOTC TEX BLOCK` line and new `SOTC FIND MEMO` line (SOTC_FPS) | find-memo search costs at most about 2% self |
| SOTC_BUF_BIND_FAST | off | always, but it is the same code path as SOTC_SYNC_FAST | none printed without SOTC_SYNC_FAST (see below) | about 1% self (SynchronizeMemoryFull, ForEachUploadRange) |
| SOTC_VK_POOL | off | always; deferred destroys go to a helper thread | none (no counter on the deferral) | below 1%, not separable in the profile |
| SOTC_PROTECT_BATCH | off | only on the buffer upload path, for read-only uploads | `SOTC PROTECT BATCH calls/pages/merged` (SOTC_FPS) | not visible by name; kernel time is in ntdll (19.7%) |
| SOTC_PRESENT_UNLOCKED | off | depends on the present path chosen at device creation | log lines at device creation (vk_instance.cpp) | none on the draw-thread CPU; it moves the flip wait |
| SOTC_UD_SWAP | off | only when no prep workers run (default: none) | none (no counter) | about 0.9% of draw-thread samples (memmove site 4) |
| SOTC_HLE_TABLE_GEN | off | only with SYNC_FAST or BUF_BIND_FAST and several gates off | `SOTC HLE SKIP` line (tbl_skipped, tbl_compared, tbl_rebuilt) | about 1.9% self (ReplaceChangedPages memcmp, BuildTable) |

Consequence for the first test: every ceiling is at or below about 2% of draw-thread samples,
about 0.7 ms of a 35 ms frame at most. One run pair (35.4 vs 35.7 ms) cannot resolve that: the
run-to-run spread is of the same order. None of these switches is shown to be dead, but none has
a measured gain either.

## Default configuration

All seven switches are off unless the variable is set to `1` (SOTC_PRESENT_UNLOCKED also accepts `2`,
which selects its presentKHR timer mode). The launcher exposes only SOTC_PROTECT_BATCH and SOTC_SYNC_FAST
(`launcher/sotc_tuning.py` lines 425 and 457). The other five can only be set by hand or by the
matrix tool (`tools/pcmem_matrix.py` lines 111-137: presets `drawfast`, `drawverify`, `drawall`).
`drawall` turns all of them on together, so a combined run cannot attribute a gain to one switch.

## Per switch

### SOTC_TEX_BIND_FAST (texture_cache.cpp `TexBindFast`, `MemoSlot`; vk_rasterizer.cpp `TexBlockMode`)

Two effects, both behind one switch:

1. The FindImage perfect-match memo (`find_memos`) uses 4096 slots instead of 1024 (`MemoSlot`).
2. `Rasterizer::TexBlockMode()` returns 1 when SOTC_TEX_BLOCK is unset, so the texture block memo
   replays a stage's binds (`TryTextureBlock`).

Engagement:
- Effect 2 is proven by the `SOTC TEX BLOCK: lookups N, hits M, stores S, verify mismatches V`
  line (vk_rasterizer.cpp, every 5 s from `TexBlockReport`). The line appears only when the block
  memo runs. Read the hit rate as hits / lookups. Zero hits means the block memo never engages in
  this scene.
- Effect 1 had no counter. This review adds `SOTC FIND MEMO 5 s: lookups L (per frame), perfect
  hits H (rate), table N slots (SOTC_TEX_BIND_FAST on|off)` (texture_cache.cpp, printed from
  `ReportTexProtect`, SOTC_FPS only). A hit rate near 100% means the table size cannot help: the
  memo already answers everything it is asked, and the profile share of `MemoLookup` (1.3% self)
  is the whole cost.

What to look for: the find-memo search (`ForEachImageInRegion`, 0.8% self) runs only on a memo
miss. The 4x table can only remove misses caused by slot collisions. Separate the two effects with
`SOTC_TEX_BLOCK=1` alone (block memo, table 1024) against `SOTC_TEX_BLOCK=1` plus
`SOTC_TEX_BIND_FAST=1` (block memo and table 4096). The difference is the table alone.

### SOTC_BUF_BIND_FAST (region_manager.h `SotcBufBindFast`; buffer_cache.cpp `SynchronizeMemory`)

Finding: in the code it is a duplicate of SOTC_SYNC_FAST. `SotcBufBindFast::kGenStamps` is true when
either variable is `1`, and that flag gates the generation stamps, the clean-range fast path in
`SynchronizeMemory` (buffer_cache.cpp ~7626-7680), the `BumpSyncGen` calls, and
`TableGenUsable` (SOTC_HLE_TABLE_GEN). `SotcBufBindFast::kOn` is never read. The only difference
between the two switches is the printer: `SOTC SYNC FAST skipped S/L (rate)` is printed under
`SotcStats::kSyncFast` (driver.cpp ~1445) only. So with SOTC_BUF_BIND_FAST=1 and no SOTC_SYNC_FAST,
the counters are incremented (under SOTC_FPS) and never printed.

Engagement: the `SOTC SYNC FAST skipped` line, which needs SOTC_FPS=1 and SOTC_SYNC_FAST=1 (or a
one-line change to the printer gate in driver.cpp to `kSyncFast || SotcBufBindFast::kGenStamps`,
not made here: driver.cpp is outside the files this review owns). The rate is skipped / lookups.

What to look for: a rate near 0% in a scene means the bind path never repeats a clean range; the
switch then costs one stamp read per sync and nothing else. A useful control: SOTC_BUF_BIND_FAST=1
against SOTC_SYNC_FAST=1 are the same code path, so their difference is the run-to-run noise of the
measurement, not a switch effect.

### SOTC_VK_POOL (vk_scheduler.cpp `VkPoolEnabled`, `GpuDestroyDeferred`; buffer.cpp:141-152; image.cpp:108-132)

Effects: BDA and image destruction is queued on a helper thread (`DeferGpuDestroy`), and images are
sub-allocated from a 256 MiB VMA pool (`SOTC_IMAGE_POOL_MB` overrides the size).

Engagement: no counter on the deferral and no log line on success. The only log is a warning when
the image pool cannot be created (image.cpp:161). To prove it, count `DeferGpuDestroy` calls in
vk_scheduler.cpp (not a file this review owns; recommended as a one-line `fetch_add` under
SOTC_FPS). Without that, the expected effect is visible only as the draw thread's share of
`vkDestroy*`/`vmaFree*` time, which the profile does not name (nvoglv64.dll is 0.9%).

What to look for: a deferred destroy costs the draw thread one queue push instead of the driver
call. The ceiling is the destroy time on the draw thread, which the baseline does not isolate.

### SOTC_PROTECT_BATCH (region_manager.h:902-913; buffer_cache.cpp:7717; page_manager.cpp `ProtectBatch`)

Engagement: the deferrable path is taken only inside `SynchronizeMemoryFull`'s ProtectBatch scope
(buffer_cache.cpp:7717), only when `ProtectBatchOpen()`, and only for pages whose CPU bits are
cleared with no GPU bit set (`(gpu & write_prot & mask) == 0`, region_manager.h:908). Written
bindings never defer.

Counters: `SOTC PROTECT BATCH calls C (pages P), merged from R, redundant skipped D`
(driver.cpp ~1454, SOTC_FPS). `R` counts the collected requests, `C` the host protect calls the
flushes made. Engagement is `C < R`. `C = R = 0` means no upload took the deferrable path.
The counters increment only under SOTC_FPS (`FlushProtectBatch`), so the line is zeros without it.

What to look for: the host protect calls are the kernel transitions (VirtualProtect). In the
baseline profile they sit under ntdll.dll (19.7% of samples outside the loader), not under a named
game function. A real gain would show as a lower ntdll share together with C/R well below one.
Compare the ntdll share between runs before reading the frame times.

### SOTC_PRESENT_UNLOCKED (vk_instance.cpp:720-790, 1141-1151; vk_presenter.cpp:550-559; vk_scheduler.h:513-530)

Engagement: decided at device creation. The log says which path was taken:
- `SOTC_PRESENT_UNLOCKED=1: presents use queue 1 of family N` (or queue 0): a present queue of its
  own (PresentPath::OtherFamily or a second queue). This is the case where the switch works.
- `SOTC_PRESENT_UNLOCKED=1 but there is no second queue ...` (vk_instance.cpp:788): off.
- `SOTC_PRESENT_UNLOCKED=1 not used: <reason>` (vk_presenter.cpp:559): off.
- On one shared queue (PresentPath::SharedDepthWait) the presenter only waits for older presents;
  the lock behaviour is the same as 0.

What to look for: the switch changes the submit mutex hold, not draw-thread CPU. The "draw busy"
line (driver.cpp:2128) includes the flip wait (reports2/12-flip-present.md), so any gain appears
in the flip-wait split (`SplitWaitFrame`, printed with SOTC_DRAW_SPLIT/SOTC_FPS), not in the
CPU profile. Mode 2 adds the presentKHR timer, which measures the present call alone. Judge this
switch by the flip-wait split and the presentKHR time, not by draw busy.

### SOTC_UD_SWAP (liverpool.cpp:1151-1158, 1225-1229)

Engagement: `ud_swap && prep_workers == 0`. The default has no prep workers (SOTC_PARALLEL_RECORD is
off, liverpool.cpp:356), so the switch engages in the default config when it is set. There is no
counter.

What to look for: the memmove site "`vector<u32>::assign` of the flattened user data"
(`ExecuteBackOp`, memmove.md item 4) is 103 samples (0.9%) in the baseline, with `ExecuteBackOp`
self at 1.0%. That is the ceiling: about 0.9% of draw-thread samples, roughly 0.3 ms of a 35 ms frame
at most. A swap replaces the copy with a pointer exchange; the expected profile change is that the
memmove site drops out of the `ExecuteBackOp` callees. Measure with the profiler, not draw busy.

### SOTC_HLE_TABLE_GEN (vk_shader_hle.cpp `TableGenUsable`, `HleSkipTick`; region_manager.h BumpSyncGen)

Engagement: `TableGenUsable()` needs all of: SOTC_HLE_TABLE_GEN=1, the generation stamps
(SOTC_SYNC_FAST or SOTC_BUF_BIND_FAST), `!SotcUntrack::AutoOn()` (SOTC_UNTRACK_AUTO=1 with
SOTC_CP_AHEAD unset, or SOTC_CLASS_TRACK=1: both off by default), `!RegionManager::RingOn()`
(SOTC_PC_RING unset: off by default), `SotcMemClasses::Mode() == 0` (SOTC_CLASS_TRACK unset: off by
default), and `!SOTC_UNTRACK_RANGES`. With the defaults, only the first two are needed. Without the
stamps, the switch is silently inert.

Counters: the `SOTC HLE SKIP` line (`HleSkipTick`, every 5 s, prints `tbl_skipped`, `tbl_compared`,
`tbl_rebuilt`). Engagement is `tbl_skipped > 0`. Note that `drawall` turns on SOTC_BUF_BIND_FAST, so
the stamps are on there, and TableGen engages in that preset. The `drawfast` preset does not
contain it.

What to look for: the baseline spends 1.1% self in `ReplaceChangedPages` memcmp (131 samples,
memmove.md item 2) and 0.8% in `BuildTable` memcpy (item 6): about 1.9% of draw-thread samples
at most, of which the table skip can remove part. A high `tbl_skipped` share with low
`tbl_rebuilt` confirms the skip; the profile should show `ReplaceChangedPages` shrinking.

## Added counters (this review)

- `SOTC FIND MEMO 5 s: lookups, perfect hits, table slots` (texture_cache.cpp, SOTC_FPS only):
  `MemoLookup` counts lookups and perfect matches; `ReportTexProtect` prints them every 5 s.
  Only the find memo is covered; the table-size switch can now be judged by its hit rate.

Counters that would close the remaining proof gaps, in files this review does not own:
1. driver.cpp: print the SOTC SYNC FAST line also when `SotcBufBindFast::kGenStamps` is set
   (BUF_BIND_FAST alone is otherwise silent).
2. vk_scheduler.cpp: count `DeferGpuDestroy` calls (SOTC_VK_POOL engagement).
3. liverpool.cpp: count the `flattened_ud_buf.swap` branch (SOTC_UD_SWAP engagement).

## What to run next

1. Measure one switch at a time, against the same base, with SOTC_FPS=1, at least five runs
   alternating base and switch. The per-switch ceiling is below about 2% of draw-thread samples,
   so the difference needs more runs than one pair.
2. Read the engagement line first. A switch whose line shows zero is not measured yet.
3. Use the profiler for the draw-thread change (`SOTC_PROFILE`, `profile_functions.py`) and the
   flip-wait split for PRESENT_UNLOCKED. Do not judge the flip-wait or PRESENT_UNLOCKED on draw busy.
4. Use BUF_BIND_FAST against SYNC_FAST as a noise control: they run the same code.
