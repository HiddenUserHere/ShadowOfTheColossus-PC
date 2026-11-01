# v1buf: SotcGen table, bind memo (V1 buffer side), DMA sweep skip, static class mode 3

Agent v1buf. Components M2 (static class mode 3) and V1 (generation-validated bind path, buffer side)
of DRAW-THREAD-DESIGN.md, plus the shared SotcGen API that texbind and W1 code against.
Nothing was built, run, or copied to out/. Syntax-only checks (clang, `-fsyntax-only`, flags from
out/gpu/compile_commands.json) pass for buffer_cache.cpp and sotc_generation.cpp.

## 1. Shared API: gpu/shadps4/video_core/sotc_generation.h (+ .cpp, new)

Agreed API, implemented as specified:
- `constexpr u64 kShift = 16` (64 KiB granules), `kAddrBits = 40`.
- `bool On()` (SOTC_GEN_BIND=1 or 2), `bool Verify()` (SOTC_GEN_BIND=2).
- `void Bump(u64 addr, u64 size)` and `u64 Sum(u64 addr, u64 size)`.

Additions (no existing signature changed):
- `DmaOn()` / `DmaVerify()`: SOTC_GEN_DMA=1 / 2 (DMA sweep skip, own switch).
- `Bumps()`: On() || DmaOn(), the test every bump site makes.
- `Covered(addr, size)`: range inside the 40-bit space. Outside it Bump is a no-op and Sum returns 0,
  so callers never store a memo for it.
- `BumpWordPages(word_base, page_mask)`: per-word bump used by region_manager (page bits to granules).
- `Count(Stat, n)`, `Report(std::string&, seconds)`.

Storage (sotc_generation.cpp): u64 counter per granule, 40-bit space = 2^24 granules. Two levels:
4096 atomic chunk pointers (static, zeroed), chunks of 4096 counters (256 MiB of guest space, 32 KiB)
created on the first bump by compare-exchange. Counters only grow and are never reset, so a Sum over
a range never bumped is 0 whether or not its chunk exists. Bump is `fetch_add(release)`, Sum is
`load(acquire)`. Cost of Sum: one atomic load per granule (size / 64 KiB).

Switches are namespace-scope `inline const int` read from getenv only (no dependency on other statics,
so no init-order issue). Off by default.

## 2. Where Bump is called (complete list)

All after the store that changes the state, so the release/acquire pairing holds.

| Site | Why |
|---|---|
| `region_manager.h` `UpdateStateAndProtection` (per changed word, after the bit store) | every CPU-bit rise and GPU-bit fall: covers faults, MarkRegionAsCpuModified, DeclareCleanCpuWrite, MarkAhead, retrack, and any other path that sets a CPU bit. This is the choke point; it mirrors the existing SOTC_SYNC_FAST stamps (same changes), but at 64 KiB granularity and independent of SOTC_SYNC_FAST |
| `region_manager.h` `ClearRingCpuBits` | same rule as the existing stamp (ring clean bits cleared) |
| `region_manager.h` `BumpSyncGen` | HLE-table style write that sets no CPU bit |
| `memory_tracker.h` `MarkRegionAsCpuModified`, `InvalidateRegion` (end), `DeclareCleanCpuWrite` (end) | explicit, after the state change (redundant with the choke point, kept as the contract) |
| `buffer_cache.cpp` `AddGpuModified` (top) | GPU write marks (conservative: the memo never reads GPU bits) |
| `buffer_cache.cpp` `InvalidateMemory` (end) | guest write / unmap invalidation |
| `buffer_cache.cpp` `PcHostApply` | ranges leaving the ring (CPU writes there were never seen) and ranges entering it |
| `buffer_cache.cpp` `BufferCache::BumpSyncGeneration` (top, before its kGenStamps guard) | the HLE sync-gen hint |
| `buffer_cache.cpp` residency release (the `resident_ranges.Subtract` path in the unbind function) | released blocks: memo answers void |
| `buffer_cache.cpp` `MarkRegionAsCpuModified` via eviction | already covered by the choke point |

Map/unmap: see section 6 (call-site snippets, files not owned).

## 3. Bind memo (V1 buffer side): buffer_cache.cpp `ObtainBuffer`, `BindMemoEligible`

- Switch: SOTC_GEN_BIND=1 (memo on), =2 (memo on + verify).
- Eligible: read binding (`!is_written`), non-texel, range Covered, SOTC_GEN_BIND on, not SOTC_STALE_CHECK
  (its per-bind sampling would be skipped), SOTC_PC_RING verify modes 4/5/7 off.
- Placed after the SOTC_HRR dry run and before `EnsureResident`, so the SOTC_CLASS_TRACK and PC_PLACE
  bind evidence (run before it) and the stream-buffer path are unchanged.
- Slot: thread-local, direct-mapped, 4096 entries, key (addr, size). Stores addr, size, gen (Sum), bind_version
  (BindVersion), arena (Buffer*), offset.
- Hit: slot matches and `Sum(now) == gen` and `BindVersion() == bind_version`: returns {arena, offset} at
  once, calls NoteBindUse (so eviction stamps match a residency check), no EnsureResident, no sync, no lock.
- Miss: Sum and BindVersion are loaded before EnsureResident and the sync, then the existing path runs. After
  the sync, if `memory_tracker->IsSyncClean(addr, size, false)`, the slot is stored with those pre-sync values;
  otherwise the slot is voided (size = 0).
- Why a store is conservative: a change between the pre-sync load and the clean check either shows in the
  clean check (not stored) or bumps after gen0 (Sum differs at the next lookup).
- BindVersion covers arena release and eviction (already bumped there); the residency-release path I found
  without a BindVersion bump gets a SotcGen bump instead, so no other cache changes.
- Mode 2: a hit still runs the full path. If `tl_sync_bytes` moved, `BindVerifyMismatch` counts it (a bump the
  hit did not see). Must be 0.

## 4. Static class mode 3 (SOTC_STATIC_CLASS=3): decision

Decision: wired literally as the brief lists, and it is largely subsumed by the bind memo.

- `SynchronizeMemory`: `SotcStaticClass::TrySkip(addr, size, gen0)` after the SYNC_FAST slot block (read
  binds only; slot stored like the clean branches). `SotcStaticClass::NoteClean(addr, size, gen0)` in both clean
  stores (the SYNC_PIPE early branch and the `clean_after` branch), read binds only.
- static_class.cpp is unchanged.
- Why subsumed: with SOTC_GEN_BIND on, the memo answers every clean read bind, including static mesh ranges, before
  SynchronizeMemory is reached. Mode 3's skip (region generation sum over an armed range) matters only when the
  SYNC_FAST slot collided or was evicted, and with SOTC_GEN_BIND on the memo is what catches that case. So mode 3's
  value is in the no-memo configuration (SOTC_GEN_BIND=0, SOTC_SYNC_FAST=1, SOTC_STATIC_CLASS=3) and it is
  expected to add little on top of the memo. Measure it with the SOTC STATIC SKIP line; if skipped binds stay near 0
  with the memo on, it can be dropped.
- Mode 3 still needs the region stamps (kGenStamps: SOTC_SYNC_FAST or SOTC_BUF_BIND_FAST). Without them it runs as 1,
  as before.

## 5. DMA sweep skip (D): SynchronizeDmaBuffers, `BufferCache::SweepDmaRun`

- Switch: SOTC_GEN_DMA=1 (skip), =2 (sync anyway, count runs that would have uploaded).
- Per dma_runs entry: `dma_run_gen` (Sum loaded before the sync) and `dma_run_valid` (set to `IsSyncClean` of the
  whole run after the sync). Both reset whenever dma_runs is rebuilt.
- Full sweep and incremental sweep both call `SweepDmaRun`. Skip when valid and Sum unchanged.
- Honest expectation: the default incremental sweep (SOTC_DMA_INCR=1) syncs only runs with dirty words, which already
  come from CPU-bit rises (each a bump), so the skip rarely triggers there. The saving is in the full sweeps (after
  each resident_version change, which happens often when blocks come and go). The counters show which.

## 6. Map and unmap (coordinator request, files I do not own: call-site snippets)

The coordinator asked that guest map also bump. The hooks are:
- `gpu/shim/bbgpu.cpp` (`runtime_memory_set_gpu_hooks`): the map hook calls `r->MapMemory(address, size)`, then
  `r->InvalidateMemory` (SOTC_MAP_INVALIDATE, default on). The unmap hook calls `UnmapMemory`.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: `Rasterizer::MapMemory` and `Rasterizer::UnmapMemory`.

Snippets (after the state change, so a Sum read before the mapping read is valid):

1. Add near the other includes of `vk_rasterizer.cpp`:
   `#include "video_core/sotc_generation.h" // sotc: SOTC_GEN_BIND / SOTC_GEN_DMA`

2. `Rasterizer::MapMemory`, as the last statement of the function (after the `mapped_ranges +=` block):
   `SotcGen::Bump(addr, size); // sotc: a new mapping changes the clamp and validity of the ranges (SOTC_GEN_BIND)`

3. `Rasterizer::UnmapMemory`, as the last statement of the function (after the `mapped_ranges -=` block, the final
   statement before the closing brace):
   `SotcGen::Bump(addr, size); // sotc: an unmapped range is void for every cached answer (SOTC_GEN_BIND)`

Note: `ReleaseMemory` runs later on the draw thread (SendOrderedCommand); its residency release already bumps through
the buffer cache (section 2). The InvalidateMemory call from the bbgpu.cpp hook bumps through BufferCache::InvalidateMemory.

## 7. Report line (call-site snippet, driver.cpp is not owned)

In `gpu/shadps4/core/libraries/videoout/driver.cpp`, next to `SotcStaticClass::Report(report, seconds);` (around line 1992):

    SotcGen::Report(report, seconds); // sotc: SOTC_GEN_BIND / SOTC_GEN_DMA

and add the include `#include "video_core/sotc_generation.h"` to driver.cpp.

Output line (per 5 s window, only when a switch is on):
`SOTC GEN BIND: bind=1 dma=0 window=5.0s memo hits=N misses=M verify mismatches=K; DMA runs skipped=S synced=Y verify mismatches=J`

Meaning:
- hits: memo answers with no sync (bind=1 or 2).
- misses: eligible read binds that took the full path.
- verify mismatches (bind): mode 2 only; hits whose full path uploaded bytes. Must be 0. Any value is a missed bump.
- DMA skipped: runs the sweep did not sync. synced: runs it synced (gen on).
- DMA verify mismatches: mode SOTC_GEN_DMA=2 only; must be 0.

## 8. Files changed

- NEW `gpu/shadps4/video_core/sotc_generation.h`, `gpu/shadps4/video_core/sotc_generation.cpp`
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`: includes; BindMemoSlot (anonymous namespace,
  placed just before ObtainBuffer); `ObtainBuffer` memo lookup and store;
  `BindMemoEligible`; `SynchronizeMemory` TrySkip / NoteClean; `SynchronizeDmaBuffers` (dma_run reset on rebuild,
  both sweep branches call SweepDmaRun); new `SweepDmaRun`; `AddGpuModified`, `InvalidateMemory`, `PcHostApply`,
  `BumpSyncGeneration`, residency release bumps.
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.h`: include; `dma_run_gen`, `dma_run_valid`; declarations of
  `SweepDmaRun`, `BindMemoEligible`.
- `gpu/shadps4/video_core/buffer_cache/region_manager.h`: include; `UpdateStateAndProtection` bump (choke point);
  `ClearRingCpuBits` bump; `BumpSyncGen` bump.
- `gpu/shadps4/video_core/buffer_cache/memory_tracker.h`: include; bumps in `MarkRegionAsCpuModified`,
  `InvalidateRegion`, `DeclareCleanCpuWrite`.
- `gpu/sotc/static_class.{h,cpp}`: unchanged (see section 4).

## 9. Tests (in game)

Build as usual (`bash build.sh`), then run the Lab route with the bind memo and compare:
- Correctness: `SOTC_GEN_BIND=2` (and `SOTC_GEN_DMA=2`) for one long session on the route. Mismatch counters must be 0
  in every window. Also run `SOTC_STALE_CHECK=16` in a separate run (the memo is off when it is on, so it is a
  separate baseline, not a combined run).
- Gain: `SOTC_GEN_BIND=1` against 0, alternated over three one-round matrix runs with `--repeat 1`
  (per the memory rule), metric = draw work (M0), per the design gate. Likewise `SOTC_GEN_DMA=1` against 0.
- Visual: three long sessions with the memo on, check for stale textures or vertex corruption.
- Static class: `SOTC_STATIC_CLASS=3` with `SOTC_GEN_BIND=0` vs 1 to see whether the mode-3 skip adds anything
  (STATIC SKIP line).

## 10. Risks

- Completeness of bumps is the whole risk. Every CPU-bit rise goes through UpdateStateAndProtection (choke point),
  which is at least as complete as the existing SOTC_SYNC_FAST stamps. Paths that change data without a bit change
  (host-served ranges, unmap, map) are covered by explicit bumps. A miss shows as verify mismatches in mode 2.
- Host-served (ring 8) memory: the memo is never consulted for PcHostBind hits; ranges entering or leaving the ring
  are bumped in PcHostApply.
- Cost: a bind's Sum reads size/64 KiB atomics. A 16 MiB bind is 256 loads per bind. Measure with the draw-work metric.
- Thread-local memo: entries hold Buffer* and offset. Valid because BindVersion moves on any arena change. Only one
  BufferCache exists per process.
- The memo skips `SynchronizeMemory`'s per-call bookkeeping (SOTC_SYNC_FAST skip counter, sync_fast_lookups)
  on hits. Only statistics change.
- BindVersion is read by the draw thread only (bind paths); eviction releases also run on the draw thread
  (assumed from the call path, not checked on every path).

## 11. Verified vs assumed

Verified in code:
- Bump sites listed in section 2 (grep of buffer_cache.cpp, memory_tracker.h, region_manager.h).
- Existing SOTC_SYNC_FAST stamp sites in region_manager.h (the choke point mirrors them).
- The map and unmap hooks and their callers (bbgpu.cpp, vk_rasterizer.cpp).
- The DMA sweep branches and the dma_runs rebuild.
- Syntax-only compile of buffer_cache.cpp and sotc_generation.cpp: passes (warnings only from the flag line).

Not verified (no build or run, per the brief):
- The memo's hit rate, the verify counts, and whether the DMA skip triggers in the default incremental sweep.
- That no unbind path leaves a memo answer valid without a bump (the residency release path was checked; others were
  covered by BindVersion, by reading the grep results only).
- The region word base is 256 KiB aligned (region base = page_index << HIGHER_PAGE_BITS, 16 MiB; assumed from
  memory_tracker.h:578).

## 12. Not finished

- Call-site snippets for driver.cpp (report), vk_rasterizer.cpp (map/unmap), and the include lines are given in
  sections 6 and 7; the edits were not made.
- No in-game measurement.
