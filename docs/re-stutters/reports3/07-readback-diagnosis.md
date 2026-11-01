# 07 - Readback diagnosis: why prefetched copies do not stop the guest read faults

Read-only. Nothing was built or run, and no repository file was edited except this report.
Labels: **observed** (log, code or disassembly read in this session), **inferred** (reasoning from
observed code, not measured), **unknown** (needs a run).

## 0. Snapshot and caveats (read first)

- **The tree moved during the analysis.** `buffer_cache.cpp` changed at 01:43 (class freshness
  bypass, `rb_class_cnt` counters, `PrefetchClassAlive`), `particle_prev.cpp` and `rb_class.h` at
  01:46 (stats request moved to the dispatching call, two calls ahead; counters asked, queued,
  deduped, empty, dropped, taken), and `rb_class.cpp` again after that. Line numbers below were
  re-checked against the current file (`buffer_cache.cpp`, 8535 lines). Where the logged build
  differs, the text says so.
- **The logs predate those changes.** `rbv2_v2.out` and `rbv2_mat.out` come from the build of
  about 01:31 (`rb_class.cpp` mtime 01:30). The class-freshness bypass and the stats move are not
  in them. The current code already prints a per-class line (`SOTC RB class (5 s)`), so the next
  Lab run answers most of section 2 directly.
- The guest-wait timers are zero in both logs (section 4, item 5), so the logs cannot show blocked
  time.

## 1. Answers

**Q1, short.** Prefetch and materialize work as designed: in every 5 s window of the RB_CLASS run,
about 1,900-2,400 copies were applied with no fault, and about 2,000-2,600 more were dropped dirty or
unused (stale plus expired). The guest still reached the copy path 540-750 times per second. A fault that waits does so
because the bytes it asks for were GPU-written again after the copy, or because no live copy covers
them. The port returns the newest recorded GPU bytes, so a copy taken earlier cannot satisfy a read
of bytes rewritten after it, unless the port serves the older bytes. RB_CLASS did not reduce the
wait rate: baseline 542-658/s, RB_CLASS 566-749/s, with more copies applied (about 2,000 vs about
1,000 per window).

**Q2, short.** eboot+0x1a72e0 is the first load of the bit scan in `BPXPR_ConsumeVisibilityMaskIndirect`
(0x1a7290-0x1a7863). It reads the XPR visibility-mask bitmap, one of four per-call bitmaps that
`BPXPR_BuildSetDraws` (0x19b760) allocates from the RenderSingleFrameCPU heap (memtype 0x2ed1f58;
kind 1 by the plan's section 6 measurement). The game does not wait for the GPU here (no label is
read). The port does: the fault at that instruction makes the guest thread wait for the GPU tick
that wrote the bitmap, then copy it. Together with the direct variant 0x1a6d60, the XPR mask
consumer accounts for about 55-60% of the waits; the particle stats read at 0x101a98 for about
20%.

## 2. Q1 - why a copy does not stop the fault

### 2.1 What the logs show (observed)

Logs: `rbv2_v2.out` (SOTC_RB_CLASS=1, the 01:31 build) and `rbv2_mat.out` (baseline: prefetch and
materialize, no RB_CLASS). The RIP wait rate is the `SOTC READBACK RIPS` line of the same 5 s window.

| t (s) | readbacks/s | RIP waits/s | issued | consumed at fault | expired | stale | materialized | not fresh | waited at fault |
|---|---|---|---|---|---|---|---|---|---|
| 10 | 852 | 566.0 | 4133 | 74 | 492 | 1656 | 1870 | 566 | 29 |
| 15 | 908 | 671.6 | 4787 | 51 | 561 | 1943 | 2182 | 719 | 47 |
| 20 | 871 | 644.8 | 4906 | 65 | 499 | 2041 | 2264 | 650 | 41 |
| 25 | 859 | 638.9 | 5052 | 23 | 500 | 2122 | 2372 | 614 | 31 |
| 30 | 953 | 691.2 | 4739 | 56 | 583 | 1958 | 2112 | 728 | 32 |
| 35 | 958 | 738.6 | 4658 | 63 | 557 | 1891 | 2133 | 675 | 27 |
| 40 | 1010 | 748.8 | 4466 | 93 | 563 | 1788 | 1977 | 721 | 38 |
| 45 | 850 | 603.3 | 4288 | 162 | 476 | 1516 | 2122 | 530 | 23 |
| 50 | 862 | 643.6 | 4366 | 76 | 524 | 1671 | 2048 | 592 | 31 |
| 55 | 954 | 735.9 | 4809 | 66 | 521 | 2017 | 2161 | 679 | 43 |
| 60 | 908 | 685.3 | 4684 | 167 | 569 | 1797 | 2140 | 630 | 26 |

Baseline (`rbv2_mat.out`), 10-25 s: RIP waits 614, 658, 542, 587 per s; issued 3112-4150;
consumed 156-358; stale 1426-2292 (46-56% of issued); materialized 927-1133.

Per-window accounting closes: in the 10 s window, 74 consumed + 492 expired + 1656 stale + 1870
materialized = 4092 of 4133 issued (41 still pending).

- **Stale is 35-42% of issued (RB_CLASS run)**. Stale counts prefetches dropped dirty before use
  (`rb_pf_stale`, incremented at buffer_cache.cpp:2364, 2449 and 4716). The stale-by-reason
  counter (`readback_stale`) does not include prefetches, so the reason is unknown.
- **Materialization removes waits, but not enough.** Materialized copies clear the GPU bit of their
  bytes, so a later fault on them finds no bytes to copy and does not wait. The waits that remain
  are readbacks that still find GPU-modified bytes in their window.
- **Faults rarely meet a live copy.** Consumed-at-fault is 23-167 per window. Copies waited on at
  the fault (`pf_claimed`) are 23-47 per window. So at-fault waiting on a pending copy is a small
  share.
- **Dirty copies cost twice.** When a guest read claims a prefetch that goes dirty while it waits,
  `FinishAsyncReadback` (4716-4718) gathers the whole window again and waits for it. Those are the
  "readbacks redone synchronously (stale)" the log counts (131-288 per window).
- **Where the waits are** (`SOTC READBACK RIPS`). At 566.0/s: 0x1a6d60 153.0, 0x101a98 110.0,
  0x1a72e0 92.7, 0x1a8210 28.7, 0x1a8b38 19.5. At 671.6/s: 0x101a98 155.2, 0x1a6d60 154.4,
  0x1a72e0 127.6, 0x1a6d3a 41.6, 0x1a8210 37.6. The XPR consumer (0x1a6d20-0x1a8dc6) is about 55-60%
  and the stats read (0x101a98) about 20-23%.

### 2.2 Mechanisms, with evidence (current file line numbers)

| # | Mechanism | Evidence | Label |
|---|---|---|---|
| M1 | **Copies go dirty because a later GPU write hits their page runs.** A prefetch's `gpu_ranges` are page runs, not the bytes it copies. `GatherDownloads` builds `gpu_ranges` from `unowned(address, address + range_size)` over `ForEachDownloadRange` (buffer_cache.cpp:5067-5069, 5080), and the GPU bit is per 4 KiB page. A write to any byte of those pages dirties the copy: `MarkInflightReadbacksDirty` (1909-1917) and `PrefetchMarkDirty` (2279-2281). The copy applies only its byte ranges (`copies`), so a write to another byte of the same page does not change the applied data. | 1909-1917, 2279-2281, 5067-5080 | observed (code); the share of page-only dirtying is **unknown** |
| M2 | **Real rewrites.** The mask arena and stats arrays are rewritten inside the same frame, and a read of bytes written after the copy must wait for that newer write. The port has no way to return the older complete bytes unless `TryServePrevious` (SOTC_RB_PREV) is used. | Stale 35-42% with the same order of misses in the baseline (46-56%) | inferred |
| M3 | **Lead time within one tick.** A prefetch is recorded in the tick being recorded (`p.tick = tick`, 2625). A fault that finds it unsent flushes and waits for its own tick (`unsent |= ...` 2409, `Flush` 2413). That wait is for the whole tick, as for a normal readback, so the copy saves nothing. | 2400-2415, 2625 | observed (code); size of the effect **inferred** |
| M4 | **Stats requests were made one call ahead (v2 build)**. The copy for the array the next call reads was requested at the current call, a few microseconds before the read. With M3, it could not complete before that read. The v3 change requests the array two calls ahead, at dispatch time. The lead is still a few calls, not a tick. | v2: `particle_prev.cpp:122-134` (old text); v3: `particle_prev.cpp:213-224, 262-271` | observed (code); **unknown** lead time |
| M5 | **A request taken before its dispatch is recorded produces an empty copy.** `total == 0` drops the window silently in the v2 build (`continue`, now 2590-2594, counted as `rb_class_cnt.empty` in the current build). The draw thread consumes queued commands in order, and the dispatch that writes the array may not be recorded yet when the request is taken. | 2590-2594; `particle_prev.cpp` request in the dispatching call | inferred; **unknown** share |
| M6 | **The "issued" counter is not a copy count.** `NoteRequestIssued(added)` (rb_class.cpp, `NoteRequestIssued`) counts windows newly armed (2489-2491, 2506). The v2 line `stats copies asked 2108/s, issued 384/s` is therefore not a drop count. The 1,700/s gap is requests for windows already armed, and those with `total == 0` (M5), which the v2 build did not count. | `rb_class.cpp`, `NoteRequestIssued`; 2489-2491, 2506 | observed (code) |
| M7 | **"held" counts calls, not windows.** Held windows stay in `pf_armed` and are re-examined at every `PrefetchIssue` call, so 150,000-225,000 "held per second" is a per-call count (2553-2556, 2660). | 2551-2556, 2660 | observed (code) |
| M8 | **Freshness uses flip counts, not cp frames.** `PrefetchNoteWrite` records fresh spans with `flip_count` (2163, 2202), and `PrefetchFreshCovers` accepts only writes in this or the previous flip (2229, 2233, 2146). Spans are recorded only for windows already recurring at write time (2168-2170), so writes before a window became recurring are never fresh. This explains the "not fresh" 530-728 per window in the v2 build. In the current build class copies skip the test (2638); the test still applies to ordinary copies. | 2146, 2168-2170, 2202, 2229-2233, 2638 | observed (code); cause of most "not fresh" **inferred** |
| M9 | **Expired copies are probably the not-fresh ones.** Materialize skips a copy that is not fresh (3023), so unless a fault reads it, it expires after 8 ticks (`rb_pf_ticks`). Expired 476-583 per window is close to not-fresh 530-728. | 3023, `rb_pf_ticks` default 8 | inferred |
| M10 | **The RIP counter counts the copy path, not blocking.** `NoteReadbackRip` runs for every readback with `waiting` set (1153), and `waiting` is also set for a dependent readback whose window has no GPU bytes but an in-flight owner (1494-1500, 1505-1513). Those may find the owner already complete. "waited for the GPU" overstates the wait. | 1144, 1153, 1494-1513, 1797 | observed (code) |
| M11 | **Guest-wait timers are zero although SOTC_FPS=1 was set.** `rb_guest_count`, the pattern counters and the wait times are under `kFpsTimers` (`kFpsLog && !kFpsLean`, `sotc_stats.h:45`), and every such line reads 0 in both logs. `scripts/` and `sotc.ini` do not set `SOTC_FPS_LEAN`. Either the shell that started the run had it set, or something else is at fault. | 1202-1207 (guest), `driver.cpp:888-895`, `sotc_stats.h:27,45` | observed (log); cause **unknown** |

### 2.3 Specific questions from the task

- **Freshness rule ("not fresh")**: M8. 530-728 copies per window in the v2 build refused
  materialize. The current build exempts class copies (2638), so this now affects only the
  ordinary copies.
- **Granule and window mismatch (64 KiB)**: no mismatch found. Both the copy key
  (`PrefetchNoteRead`, `PrefetchIssue`) and the fault window (`ReadbackWindow`, narrow mode) use
  `AlignDown(addr, 64 KiB)`. A fault wider than one granule, or a stats read that spans two, still
  waits on the granule it does not cover (not counted).
- **Ownership (rb_owned)**: `GatherDownloads` skips bytes of other in-flight readbacks
  (`unowned`, rb_owned). A fault on an owned range waits for that owner's tick (1494-1500). That
  is M3 again: the owner was copied in the tick being recorded.
- **Read before the copy completes**: M3. The copy's tick is the one being recorded at issue time,
  and a fault in the same tick forces a submit.
- **Pages re-protected**: materialize and apply clear GPU bits only for copied ranges, and the
  tracker's page bit is cleared with them. The page is not re-protected unless a later GPU write
  sets it again. No defect found.
- **Dirty and stale marking**: M1 (page runs) and M2 (real rewrites). Both paths mark the same way
  (1909-1917 and 2279-2281).
- **The stats request slot**: **not the wrong slot.** In the v2 build the request covered
  `prev_desc = (read_slot + 1) % 3`, the slot the next call reads (old `particle_prev.cpp:122-134`).
  In the current build `prev_desc` is at `particle_prev.cpp:258`, and the request is made at dispatch
  for slot `(read + 2) % 3`, read two calls later (`particle_prev.cpp:262-271`). Both match the slot arithmetic in the file comment. The
  timing (M4) is the problem, not the slot.

## 3. Q2 - eboot+0x1a72e0

### 3.1 Identity (observed)

- `python tools/re/eboot_re.py func 0x1a72e0` gives `BPXPR_ConsumeVisibilityMaskIndirect`,
  0x1a7290-0x1a7863 (1491 bytes). Pseudocode: `out/re/decomp/001a/001a7290.c`.
- The loop at 0x1a72d1-0x1a72f3 reads one qword per iteration: `mov rdx, [rbx + rcx*8]` at
  0x1a72e0, with `rbx = rdx` (the third argument, `mov rbx, rdx` at 0x1a72a9). It scans up to
  `(u16 [param_1+0x64] + 63) / 64` qwords for the first nonzero word, then the second loop scans
  `param_2` the same way (0x1a7320).
- The **direct** variant `BPXPR_ConsumeVisibilityMask` (0x1a6d20, `out/re/decomp/001a/001a6d20.c`)
  does the same first-word scan at 0x1a6d60, on its second argument. Both variants scan the same
  logical object: the `uVar6` mask source chosen in `BPXPR_SelectMaskSource` (0x19af20,
  `out/re/decomp/0019/0019af20.c`).
- `BPXPR_SelectMaskSource` chooses the variant: the direct one when `[cfg+0x22] == 0`, or the
  mask-kind selector at `[param_3+0x20]` is 0 or 7, and the indirect one otherwise
  (decomp of 0x19af20, the branch on `uVar2 & 0xff00000000`).
- The mask pointer `uVar6` is read from the per-set entry (0xa8 bytes, index `[param_1+0x148]`,
  table at `[ctx+0x38]`) at field +0x0, +0x8, +0x10 or +0x18, by the selector (disassembly
  0x19afa8-0x19b059). Those four fields are the four bitmaps `BPXPR_BuildSetDraws` allocates
  (report 02, section 2.4; `BPXPR_BuildSetDraws` is 0x19b760-0x19c193).

### 3.2 Which buffer and which allocator

- **Buffer:** the XPR visibility-mask bitmap of one draw set. Size `4 * ceil(n / 32)` bytes, with
  `n = u16 [object+0x64]`, 16-byte aligned, at most 256 B per bitmap (report 02, section 2.4).
- **Allocator:** `FUN_00012820(size, DAT_02ed1f58, 16, 0)` (BuildSetDraws, report 02). The memtype
  slot is 0x2ed1f58, which the plan (section 4, section 6) names **RenderSingleFrameCPU, slot 17,
  kind 1**, measured in the second run. **Report 02 said "GPU family, kind 2 (inferred)". The
  plan's measurement supersedes it.** The mask bitmaps therefore sit in a CPU-kind heap that the
  GPU writes (the conflict the plan records).
- **Other XPR-family functions near the RIPs:** BPXPR_BuildSetEntry 0x19b400-0x19b759 (857 bytes);
  unnamed `sub_1a8180` (0x1a8180-0x1a85e3, which contains RIPs 0x1a8210 and 0x1a83f1);
  unnamed `sub_1a8a40` (0x1a8a40-0x1a8c56, contains RIP 0x1a8b38); unnamed `sub_1a8c60`
  (0x1a8c60-0x1a8dc6). Not analysed further in this pass.
- The RIP 0x1a6d3a (direct consumer, `movzx r10d, word [r9+0x64]`) reads the set record, not the
  bitmap, so the record's page is also GPU-modified (inferred: the record sits in the same heap).

### 3.3 Does the CPU wait for the GPU?

- **The game does not** (report 02, section 2.3, observed): no WAIT_REG_MEM on the mask labels
  (+0x40, +0x68), and the consumer reads no label. The game's own frame pacing is the only sync.
- **The port does.** Each fault on a GPU-modified page enters `BeginAsyncReadbackImpl`, which
  copies the GPU bytes and then waits: `scheduler.WaitSubmitted(readback.tick)` (1164),
  `GetWorkSemaphore()->Wait(readback.tick)` (1172), and `FinishAsyncReadback`, sent to the draw
  thread with `SendCommand` (1190-1191). That wait is the RIP's cost. It is port-induced and not in the game.
- The recording that reads the mask runs on a CPU job (`sub_c1860` -> `0x11d7a0` -> `0x19af20`,
  report 02, section 2.1). The guest thread that blocks at the fault is therefore a job worker
  (inferred).

### 3.4 Why it reads GPU-written data at all

Report 02 (inferred) says the consumer records before the GPU runs the frame's cull, so it reads
bytes last written at that address by an earlier execution. The `forceUseLastIndexBufferCullingResults`
cvar name suggests the game deliberately uses the previous cull result. If so, the port could serve
the last completed copy instead of waiting (M2). That is the existing `TryServePrevious` path, with
no recorded measurement (report 02, section 2.6).

## 4. Corrections to earlier notes

1. **Report 02 family claim** (GPU kind 2, inferred) is superseded by plan section 6 (RenderSingleFrameCPU, slot 17, kind 1, measured).
2. **"Readbacks (waited for the GPU)"** in `SOTC READBACK RIPS` counts the copy path, not blocking (M10).
3. **RB_CLASS "issued"** counts newly armed windows, not copies (M6). **"held"** counts per call (M7).
4. **The stats request slot** is correct in both builds (section 2.3). The problem is timing (M4, M5).
5. **Guest-wait timers are zero** in both logs though SOTC_FPS=1 was set without LEAN (M11). Until
   this is explained, the logs say nothing about blocked time.

## 5. Fixes (described, not applied)

Ordered by what they remove and the risk.

**F1. Measure before changing (cheap, decisive).** Use the current counters: `SOTC RB class (5 s)`
(issued, applied, read by a fault, stale, block gone, expired, claimed, held, empty, capped, budget,
no arena) per class, and the stats-read check (`NoteStatsReadCheck`). Add: (a) a stale reason split
into byte overlap and page-only (M1); (b) at each waiting readback, whether a copy covered its
window and why it did not serve (dirty, unsent, not issued, not fresh); (c) explicit
`SOTC_FPS_LEAN=0` and a check of the environment that started the logged run (M11). A run with the
current build, with the same route, answers M1 vs M2 directly.

**F2. Narrow the dirty test to copy bytes (M1).** In `MarkInflightReadbacksDirty` (1909-1917) and
`PrefetchMarkDirty` (2279-2281), test the copies' byte ranges (`srcOffset`/`size` plus the arena base)
instead of `gpu_ranges`. Be careful: `SubtractDownloaded` (4907-4932) subtracts `gpu_ranges` from the
byte-precise GPU-modified set. Changing only the dirty test lets a later write in an uncopied byte of
the same page survive the dirty check and then be cleared by the subtract. The dirty test and the
subtract must change together, and the page-bit unmark must keep pages with uncopied GPU bytes set.
Expected effect: lower stale share. Risk: correctness (the history of rainbow and stale-data bugs in
these comments). It needs A/B and visual checks.

**F3. Serve the last complete copy for the XPR mask arena (M2).** For mask windows only, use
`TryServePrevious` (SOTC_RB_PREV) so the fault serves the newest complete copy and does not wait.
This would remove 0x1a6d60 and 0x1a72e0 (about 250-300 of the 566-672 waits per second), but it
changes the bytes the game sees. Before it is used, confirm which frame's mask the PS4 consumer
reads (report 02, next measurement item 1: log `rsi` against the bitmap that BuildSetDraws
allocated in the same frame). Check culling visually.

**F4. Stats copy lead and ordering (M4, M5).** Keep a request armed when `total == 0` and retry at the
next boundary, not drop it (2590-2594, and the `window.armed = false` at 2561 runs before this).
Measure the calls and ticks between dispatch and read in the v3 build: if the lead is under one
tick, M3 means no saving, and the only real change is to submit the copy earlier (at the dispatch
boundary) or to take the request at the previous frame.

**F5. Freshness on cp frames (M8).** Use `cp_frame` instead of `flip_count` in `PrefetchNoteWrite`
(2163, 2202) and `PrefetchFreshCovers` (2229-2233), and record fresh spans for writes that happen
before a window becomes recurring. The current class bypass (2638) already relies on the hold rule
for the mask arena. Whether the bypass is safe for ordinary copies is open: the freshness rule exists
to guard against reused memory (comment at 2917-2922 of the earlier version).

**F6. Mask ring (structural, PC plan phase 3).** Give the mask heap at least three slots per cp
frame, so a copy taken after frame N-1 is never rewritten before the read at frame N. This removes M2
at the source. Each bitmap is at most 256 B, so the memory cost is small. The address reuse it
removes is the reason the reads see newer bytes (report 02, 3.4). Inferred; needs the probe in F1.

## 6. Open items (unknown, need a run)

1. Share of prefetch stale that is page-only (M1) versus real byte overlap.
2. Whether PS4 mask reads see the previous cull (3.4) or the current one.
3. Stats: how often `total == 0` at issue (M5), and the dispatch-to-read lead in ticks (M4).
4. Blocked time per waiting readback (M10, M11): the wait timers must read non-zero.
5. Whether the XPR family sub_1a8180, sub_1a8a40 and sub_1a8c60 read different buffers from the
   consumer (not analysed).
