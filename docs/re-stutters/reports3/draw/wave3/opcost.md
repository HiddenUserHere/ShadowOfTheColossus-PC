# opcost (wave 3): per-operation CPU cost of dispatch and draw

Branch: worktree-agent-abd04fb186915d259. Switch: SOTC_OP_COMPUTE_BIND (0 off by default, 1, 2 verify).
Not built with ninja and not run in game; syntax-only check (clang -fsyntax-only) passed for
vk_rasterizer.cpp and videoout/driver.cpp. The worktree's gpu/third_party submodules are empty, so the
check used the main tree's copies.

## Evidence

Trace session user/trace/20261101-085933, 40 slow-frame files, draw thread (shadPS4:GpuDraw) total
1312.05 ms. Span totals (time, calls):

| span | ms | calls | note |
|---|---|---|---|
| obtain buffer | 92.83 | 545 | inside bind buffers |
| sync memory | 82.07 | 520 | CPU-written re-upload, inside obtain buffer |
| sync copy | 76.31 | 2606 | the copies of sync memory |
| bind buffers | 106.52 | 601 | about one per dispatch |
| bind textures | 52.21 | 348 | |
| bind resources | 74.41 | 573 | draws only (Draw, DrawIndirect) |
| DispatchDirect | 91.61 | 406 | |
| dispatch | 85.61 | 206 | |
| DispatchIndirect | 47.02 | 154 | |
| dispatch indirect | 44.80 | 76 | |
| Draw / DrawIndirect | 46.47 / 50.86 | 321 / 515 | |
| rb finish | 25.87 | 207 | readbacks on the draw thread |
| wait gpu | 24.84 | | |
| sync protect | 12.70 | 214 | re-protect |
| image upload | 16.10 | 61 | |
| RefreshImage | 16.08 | 61 | |
| find image | 10.41 | 37 | |

The spans nest, so the rows are not additive. The sample profile is empty in these traces (the sampler was
off), so no per-function split is available.

## Findings

1. Area re-upload dominates the buffer cost. sync memory plus sync copy (158 ms of 1312 ms) is CPU-written
   memory being copied again, not per-op overhead. It belongs to the buffer and area work.
2. Dispatches were never bind-cache or fast-bind eligible. BufferCache-side bind work (bind buffers, 601 calls)
   runs about once per dispatch (406 + 154 dispatches). BindCacheEligible and FastBindStatic returned false for
   compute, so every dispatch rebuilt its descriptors. This is the gap the new switch targets.
3. Per-op bookkeeping is small. Trace scopes, SplitTimer, SOTC_OP_PHASES, MdiBreak and the stats counters are
   gated by static flags, and the span data shows no measurable share for them.
4. No per-op heap allocation on the steady dispatch path. CommitBindCacheFrame uses assign() into entry vectors
   that keep their capacity.
5. Pipeline::BindResources (vk_pipeline_common.cpp) pushes descriptors on every dispatch. Push constants are
   already deduplicated (push_constant_cache); descriptor sets are not. The desc-heap path commits a new set per
   call, so skipping needs a set and command-buffer generation check. Not fixed; the gain is probably small.

## Changes (file:function)

- sotc_stats.h: kOpComputeBind (reads SOTC_BIND_CACHE, _RANGES, _XTICK, _FRAME itself, not derived from other
  inline flags) and counters op_compute_eligible, op_compute_hits, op_compute_verify_checks,
  op_compute_verify_mismatches.
- vk_rasterizer.cpp, Rasterizer::BindCacheEligible: compute pipelines are eligible when the switch is on.
  Compute rejects uses_dma and sharp tables. The write, storage-image and shared-memory checks apply unchanged.
- vk_rasterizer.cpp, Rasterizer::BindResources: compute_bind and compute_verify. In mode 2 no frame-cache hit
  is taken for a dispatch. The would-be entry (BindCacheFindFrame, valid, flush_gen and buffer_version
  current) is found before the full bind. After the full bind and before CommitBindCacheFrame, push data,
  write metadata, write_info indices and both info arrays are compared with the entry.
- driver.cpp, the bind-cache block of the 5 s report: one line "SOTC OP COMPUTE BIND mode N ...".

## Ordering and safety

- A compute write still flushes nothing, as before. Invalidation of a cached read after a GPU write relies on
  the same write stamps (write_stamps, BindCacheInvalidateFrame) that protect draw entries.
- A hit replays barriers through Runtime::Transit and IsBufferAccessed, as for draws. DispatchDirect and
  DispatchIndirect use needs_barrier after BindResources, so the replay sets it the same way.
- Mode 2 does not check stamps. A stale entry can therefore show as a mismatch. A mismatch on a fresh entry is
  a real difference.

## How to test in game (one round, when the user authorizes it)

1. Verify first: SOTC_BIND_CACHE=1 SOTC_BIND_CACHE_RANGES=1 SOTC_BIND_CACHE_XTICK=1
   SOTC_BIND_CACHE_FRAME=1 SOTC_OP_COMPUTE_BIND=2. The mismatch count should be 0. Check any mismatch against
   SOTC_BIND_DIFF before calling it a bug.
2. Then SOTC_OP_COMPUTE_BIND=1 against default, with 5 s reports and hitch.md. Look at the hit count against the
   eligible count, and at bind buffers and bind textures on the draw thread.

## Left unfixed, ranked by measured share

Every row is a draw-thread share from the table above, 1312.05 ms total. Rows nest, so they are not additive.

1. Area re-upload: BufferCache::ObtainBuffer and its sync path (buffer_cache.cpp; the sync memory and sync copy
   spans), 82.07 + 76.31 ms, 2606 copies in 40 files. This is the largest item. The PC-native memory model
   (no page tracking) is the replacement path; the memo here does not reduce it.
2. Readbacks on the draw thread: Rasterizer readback finish and command (vk_rasterizer.cpp, rb paths),
   rb finish 25.87 ms over 207 calls, plus command 21.30 ms.
3. Texture binds: Rasterizer::BindTextures and BindTexturesFull (vk_rasterizer.cpp), bind textures 52.21 ms over
   348 calls. Dispatches now hit only if SOTC_OP_COMPUTE_BIND=1 is on and the entry is fresh; the hit rate is
   unmeasured.
4. Dispatch descriptor writes: Pipeline::BindResources (vk_pipeline_common.cpp) on DispatchDirect and
   DispatchIndirect. No measured share is separable from dispatch 85.61 ms self time. Not fixed.
5. Re-protect: sync protect 12.70 ms over 214 calls (buffer and memory tracking, VirtualProtect). Goes away with
   the PC-native memory model.
6. Image creation on the draw path: RefreshImage 16.08 ms and image upload 16.10 ms over 61 calls; find image
   10.41 ms (texture_cache). Outside the dispatch and draw op cost.

## Risks

- Compute hit rate is not measured. The entry table is shared with draws, so dispatches may evict draw entries.
- Verify mode does not check stamps, so it can over-report.
- Nothing above has run in game.
