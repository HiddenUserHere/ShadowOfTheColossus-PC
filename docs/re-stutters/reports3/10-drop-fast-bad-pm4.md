# 10 - SOTC_DROP_FAST and the "bad PM4 packet" crash (1 Nov 2026)

Read only. No repository file was edited or built, and the game was not run. This file is the only
output. Labels: **observed** (source, log or counter), **inferred**, **unknown**. Source line numbers
are for the working tree on 1 Nov 2026; the log line numbers are for the build that crashed (see 4).

## 1. Answer

- **observed:** the crash is a command processor read of a buffer address that the fast-drop guard
  did not hold. The guard (`Liverpool::CmdRanges`) covers a submit's DCB and CCB, the ACB slice, and
  every nested indirect buffer that `Liverpool::CollectNestedIbs` finds in those at submit time. A
  fast drop of a range that overlaps a registered entry takes the waiting path. So for the crash to
  happen, the parser must read a buffer that was **not in CmdRanges** when the range was freed.
- **observed:** the bad dword was **non-zero**: "Wrong PM4 type 1" (bits 31:30 = 01). Zeroed memory
  would read as type 0. A type-1 dword looks like reused data (for example a float in the range
  0x40000000-0x7FFFFFFF), so the memory was most likely reused by the guest after it was freed. The
  reuse fits a save load, which was streaming data and had just run a texture emergency collection.
- **observed:** the address is 1 MiB aligned (0x1120d00000). It was read by the top-level loop of
  `Liverpool::ProcessGraphics` (default branch, "Wrong PM4 type"), so it is the first dword of a
  DCB or of an IB that the parser entered. It is not an offset into a buffer the parser was already
  walking.
- **inferred, rank 1:** the buffer was an **IB whose packet the submit-time walk never reached**, or
  whose contents were not valid at walk time. Two concrete ways:
  (a) `CollectNestedIbs` leaves the walk silently on a non-packet dword, on a dword count that runs
  past the span, or on a zero dword (type 0 is stepped as a 2-dword packet, so a zeroed or not yet
  written area is walked through without finding an IB). None of these exits sets `overflowed` or a
  counter. The "IB walk overflows 0" counter therefore does not prove coverage.
  (b) the IB packet or its target was written after the submit. The walk saw older bytes.
- **inferred, rank 2:** a nested IB whose target was **not mapped** at submit is skipped
  (`drop_fast_unmapped_ibs`). It is registered only when the parser reaches it (`CmdRangeGuard`), which
  leaves a window in which a fast drop is not guarded. The counter read 0 in the crash session, but
  only in a stats line printed before the crash (pool2 log line 213). No stats line was printed
  after it.
- **observed, separate bug on a path the guard does not cover:** `direct_release` zeroes the released
  range (`host_discard`) before its unmap hook runs, unless `SOTC_RELEASE_ORDER` is set. The zeroing
  happens before `CmdRangeInUse` is ever asked. This does not match this crash's type-1 signature, so
  it is rank 3 here, but it is a real hole that the guard cannot close. See section 3, H3.
- **ruled out (observed):** the command-processor completion order (the CCB is joined before the DCB
  task ends, and the two queues pop together) and the EOS label ordering (the guard does not depend
  on labels). Details in section 2.4.

## 2. Evidence

### 2.1 The guard (observed)

- The drop path is `buffer_cache.cpp:BufferCache::DropGpuModified` (644-720), called from
  `vk_rasterizer.cpp:Rasterizer::UnmapMemory` (7070-7084) through the GPU unmap hook in
  `gpu/shim/bbgpu.cpp` (141). `driver.cpp` (1853) only prints the counters. With
  `SOTC_DROP_FAST` unset or not `0` (line 674-676), a range with no GPU-written pages and no owed
  readback is checked with `Liverpool::CmdRangeInUse` (line 690). If that is false, the range is
  marked CPU-modified with `MarkCpuModifiedUnlessGpu` and recorded with `SotcStats::NoteFastDrop`.
  Otherwise the drop waits with `SendCommand<true>` (line ~715). The comment block at 663-671 still
  says "off by default" and "1 of 4 sessions closed"; the code default is on. Update that comment
  when the fix lands.
- `Liverpool::CmdRanges` (`liverpool.h` ~350-410): a mutex-protected vector of `(VAddr, bytes)`.
  `Add` and `Remove` are exact-match, and `Remove` removes one entry, so it is a multiset. `Intersects`
  is an overlap test with saturating ends. `CmdRangeInUse` also fails closed while any submit with
  `overflowed` is in flight (`cmd_overflow_live`).
- `Liverpool::SubmitGfx` (`liverpool.cpp` ~4768-4807): `ProcessGraphics` builds the task (not started),
  then the DCB span, the CCB span and `CollectNestedIbs` results are added to `cmd_ranges`. The
  `SubmitRanges` entry is pushed to `queue.submit_ranges` under `queue.m_access`, in step with
  `queue.submits`.
- `Liverpool::SubmitAsc` (~4810-4850): the same for the ACB slice. gnmdriver passes a wrapped ring
  as two spans (`gnmdriver.cpp` ~431), so each span is registered separately.
- `Liverpool::Process` (~2969-2980): when a task is done, it pops `queue.submits` and
  `queue.submit_ranges` together, removes the entries, and decrements `cmd_overflow_live` if needed.
- Parse-time guards: `CmdRangeGuard` at `ProcessCeUpdate` IndirectBufferConst (~3164), `ProcessGraphics`
  IndirectBuffer (~4116), and `ProcessCompute` IndirectBuffer (~4331). Each holds the nested range
  while its nested task runs.

### 2.2 The walk, `Liverpool::CollectNestedIbs` (liverpool.cpp ~4705-4765) (observed)

- Caps: depth 4 (`kMaxDepth`), 1024 ranges, 2^20 packets. Hitting a cap sets `overflowed` and is
  counted (`drop_fast_overflows`). This path is fail-closed.
- Walk: type 2 steps 1 dword; type 0 steps `NumWords()+1`; type 3 steps `NumWords()+1` and, for
  IndirectBuffer and IndirectBufferConst, registers `[base, base+ib_size*4)` and recurses. The
  parser's own stepping (`NextPacket`, `header->type3.NumWords()+1`) is the same, so on a valid
  stream the two agree.
- **Silent exits (the gap):**
  - `else { return; // not a packet }`: a type 1 dword stops the walk. No flag, no counter.
  - `if (words > cb.size()) return;`: a packet that runs past the span stops the walk. No flag, no
    counter.
  - type 0 is accepted as a packet, so a zeroed region is walked in 2-dword steps and the walk ends
    at the span end without finding anything. A DCB with an IB written after the submit is walked
    past the place where the IB will be.
- **Unmapped IB:** `IsValidMapping(data, bytes)` (full-range check through `runtime_memory_is_mapped`,
  `src/runtime_memory.c` 2046) fails, the IB is skipped and `drop_fast_unmapped_ibs` is incremented.
  The IB is not flagged, so a fast drop is allowed until the parser reaches it.
- Conditional execution is not modelled. The parser skips `exec_count` dwords after a false
  `IT_COND_EXEC` (`liverpool.cpp` ~4197-4212), but the walk steps through the body as packets. That
  only over-registers for well-formed bodies, so it is **not** a hole by itself.
- Dedup: a repeated `(data, bytes)` pair is walked once per submit (linear search). Correct.

### 2.3 The crash (observed)

- Log `user/pcmem/20261101-014856/logs/pool2-r2-a1.log` (Lab run, pool2 round 2 attempt 1; the route
  did not finish). Last lines before the stop:
  - line 273: `Loading a save or a new area`
  - line 275-276: `Texture cleanup (level 2 of 2): 61 dropped, video memory in use 2878 MB`
  - line 277: `NoteBadPacket: sotc: bad PM4 packet at 0x1120d00000; in a fast-dropped range: yes, 39 drop(s) ago`
  - line 278-281: `ProcessGraphics: Unreachable code!` then `Wrong PM4 type 1`, then the stop.
- The only stats line with the drop counters is line 213: `drop fast 3465, waited 0 (0 of them over a
  command buffer in use); IB walk overflows 0, unmapped IBs skipped 0`. It was printed earlier in the
  load. No stats line was printed after it, so the state at the crash is unknown.
  (The line is from the pool2 log, not the source; the printer is `driver.cpp` ~1853.)
- The crash log `user/crashes/2026-11-01_01-56-00_0x17.log` has the same stop signature.
- Other drop lines from the same Lab folder (for example "drop fast 5116 ... 31 waited ... 0 over a
  command buffer in use, 0 overflows, 0 unmapped") show the guard fired **zero** times in earlier
  sessions. So the guard path has never been exercised on a real overlap in these logs, which means
  its correctness is untested.
- The build that crashed is a few lines behind the working tree (log says `liverpool.cpp:3105`
  for NoteBadPacket and `:3215` for the unreachable; the current file has them at ~3110 and ~3230).
  The "IB walk" counters are in the log, so `CollectNestedIbs` was in that build.

### 2.4 Ruled out (observed)

- **CCB removed too early:** `ProcessGraphics` runs the CE task (`ce_task`, line 3210-3215) and
  joins it before it returns (`while (!ce_task.handle.done())`, line 4227-4231). The CCB ranges are
  removed only after both are done.
- **Queue pop order:** `queue.submits` and `queue.submit_ranges` are pushed and popped together
  under `queue.m_access` (SubmitGfx, SubmitAsc, Process). They stay in step, and the overflow counter
  is decremented only for the entry that was popped.
- **Multiset removal:** the parse-time guard and the submit entry for the same IB are two entries.
  Each is removed by its own `Remove`, so both can coexist (exact match, one entry each).
- **Copy mode:** `CopyCmdBuffers` (~4672) moves only the top-level spans into host copies. Nested IBs
  still point to guest memory, and the walk runs on the copies, so the nested IBs are still
  registered. Copy mode is off by default (reports2/01, `BB_COPY_GPU_BUFFERS`).
- **Label ordering:** EOS labels are written by the draw thread (`liverpool.cpp` ~3713-3760), so a
  label can be seen before later packets are parsed. That can make the game free memory early, but
  the guard does not depend on labels. A buffer that was registered at submit is protected whatever
  the label says. So label order is not the cause of a missed range (it is still relevant to the
  game-side argument in reports2/01).

### 2.5 Why "39 drops ago" (observed + inferred)

- `SotcStats::FastDropAge` (sotc_stats.h ~641) searches the last 256 fast drops. "39" means the
  address was inside a range dropped on the fast path 39 unmaps before the crash. That is recent,
  during the same load. The drop was taken with no overlapping registered entry, so at that time
  nothing the CP was known to read was in that range.
- The ring does **not** say whether the buffer was read by the CP before or after the drop, or
  whether it was registered later. The drop may be the free that started the reuse. The read came
  afterwards, through a buffer that was not registered at the drop.
- The ring only records the fast path (`NoteFastDrop` in DropGpuModified). A drop through the waiting
  path is not in the ring, so "yes" tells you the freed range was handled by the fast path. It does
  not say whether a waiting drop of the same range was also taken.

## 3. Hypotheses, ranked by evidence

| rank | hypothesis | evidence for | evidence against | how to test |
|---|---|---|---|---|
| H1 | Walk missed an IB: silent exit (non-packet, span overrun, zero dwords) or IB content written after submit | Walk exits are uncounted; type-1 read fits reuse; drop taken with no overlap; counters cannot prove coverage | No direct evidence yet; a well-formed DCB should not hit these exits; the game's write-after-submit is unverified | Count walk gaps (new counter); in `NoteBadPacket`, print the submit tick and whether `addr` came from the submit list or from a parse-time guard |
| H2 | IB unmapped at submit, mapped and used later, registered only at parse | `drop_fast_unmapped_ibs` path is real and not flagged; the window matches "fast drop before parse" | Counter 0 in the only stats line; the game normally maps before submit (unverified) | Read the counter at crash time (print it in NoteBadPacket); add a late-registration counter in the parse-time guards |
| H3 | `direct_release` zeroes before the GPU unmap hook (`src/runtime_memory.c` `direct_release` ~1378-1400, `host_discard` before `flush_hooks`) | Real ordering bug, observed; the guard never sees the zeroing, so no wait can help | Zero reads as type 0, not type 1; crash was type 1 (possible only if reused after the zeroing) | Count `host_discard` calls over ranges with `CmdRangeInUse` true; check the default of `SOTC_RELEASE_ORDER` |
| H4 | CCB/ACB or queue order bug in CmdRanges | Ruled out (2.4) | - | - |
| H5 | Chained IB (`chain` bit, IT_INDIRECT_BUFFER bit 20) is walked as a call, not a jump | The port's parser and walker both treat it as a call | Unverified what the CP does with the chain bit; the crash path does not show a chain | Log the chain bit at walk and parse |

## 4. Minimal change (not applied)

The walk is the only part that decides what the guard sees, so the fix is there. Keep the waits
short by making the walk **fail closed** only where it cannot prove coverage.

1. `liverpool.cpp: Liverpool::CollectNestedIbs`:
   - At `else { return; }` (non-packet), at `if (words > cb.size()) return;`, and for type 0 or type 1
     dwords (which the parser never accepts: the DCB loop calls UNREACHABLE on them), set
     `out.overflowed = true` and increment a new `SotcStats::drop_fast_walk_gaps` before returning.
   - On the unmapped-IB branch, also set `out.overflowed = true` (the IB may become mapped and used
     later). Keep `drop_fast_unmapped_ibs` for the count.
   - Effect: `cmd_overflow_live` makes unmaps wait for the lifetime of that one submit. Only submits
     with a gap take the waiting path, so the cost stays small.
2. `liverpool.cpp: ProcessGraphics` (IndirectBuffer, ~4116), `ProcessCeUpdate` (IndirectBufferConst,
   ~3164), `ProcessCompute` (IndirectBuffer, ~4331): before `CmdRangeGuard`, check whether
   `(data, bytes)` was already in the submit's list (add a per-submit set, or count matches).
   If it was not, increment `drop_fast_late_ibs`. This is the diagnostic that separates H1/H2 from
   everything else. It does not change behaviour.
3. `sotc_stats.h`: store the submit tick in each `DropSlot` (the `NoteFastDrop` ring), and print the
   submit tick of the last registration for `addr` in `NoteBadPacket`. That tells whether the buffer
   was registered and then dropped, or never registered.
4. `src/runtime_memory.c: direct_release` (and `flex_free`): when the GPU unmap hook is pending and
   `SOTC_DROP_FAST` is on, the discard must come after the guard decision. The simplest form: call the
   hook (or its drop) before `host_discard`, as `direct_unmap` already does, or make
   `release_order()` the default. This is H3 and is independent of the walk.
5. Comments: update `buffer_cache.cpp` 663-671 (the "off by default" text).

Expected cost: the waits come back only for submits with a walk gap, which should be rare in normal
play. The walk itself stays bounded by the existing caps. Cost is unmeasured.

## 5. Verification

- Run one save-load route with `SOTC_DROP_FAST=1` and the new counters. Expected for H1/H2: a
  non-zero `drop_fast_walk_gaps` or `drop_fast_late_ibs` in the session that crashed. If both stay
  zero through a crash, H1 and H2 are out and H3 is the next candidate.
- Repeat the route 4 to 6 times. The old rate was about 1 in 4, so zero crashes in 8 runs with the
  counters reporting no gaps would be weak evidence. A single crash with a counter that fired is strong.
- Keep `SOTC_DROP_FAST_CHECK=2` from reports2/01 as a check mode: take the fast drop, but record any
  parser read of a fast-dropped range (the `NoteBadPacket` ring extended to IB reads).

## 6. Open questions

1. Does the game write any command or indirect buffer content, or any IB packet, after the submit that
   references it? This decides whether H1(b) is possible. It needs a runtime check (hash the walked
   bytes at submit and again at parse), not a guess.
2. What is the default of `SOTC_RELEASE_ORDER` and was it on in the crashing build? (not checked)
3. Does the CP honour the chain bit as a jump? The port treats it as a call (H5).
4. `reports2/01-cmdbuf-lifetime.md` (section 1) said the walk did not exist and the missing IB
   registration was the most likely cause. The walk now exists (`CollectNestedIbs`), so that report
   is partly stale: the parse-time gap it describes is closed for IBs the walk reaches. What remains
   is H1 and H2 above.
