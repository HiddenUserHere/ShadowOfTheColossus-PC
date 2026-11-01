# N4 safe deletions, specialist n4mem (buffer cache, page manager, texture cache)

Uncommitted, worktree `agent-a48358a1beec31f4c`. Files were copied from the main tree first, then edited.

## Per switch

Defaults were confirmed from the reader code. Several audit rows were wrong: the audit marked them as off or as unread, but the code default is on or computed. Each case is kept with the default behaviour and only the non-default path removed.

| Switch | Confirmed default (reader) | Action | Where |
|---|---|---|---|
| SOTC_CLEAN_READS | ON (`!= 0`, unset = true) | Env read removed, path always on | page_manager.cpp TryCleanWalkerLoad |
| SOTC_DESYNC_REPAIR | 2 (count only) | Mode 0 early return and mode 1 Protect/log repair removed. Counting kept | page_manager.cpp RepairStuckWriteFault |
| SOTC_STACK_GUARD | 1 (lock held through host Protect) | Mode 0 unlock branches and StackGuardMode() removed. Lock kept | page_manager.cpp SignalImpl::Protect |
| SOTC_DMA_MERGE | ON (`v[0] != '0'`) | Switch removed, merge always on. The non-merge per-range sweep (else branch) removed. max constant 16 MiB | buffer_cache.cpp SynchronizeDmaBuffers |
| SOTC_DMA_MERGE_MAX | 16 MiB (`mib ? mib : 16`) | constexpr `dma_merge_max = 16ull << 20` | buffer_cache.cpp |
| SOTC_DMA_SYNC_SKIP | off | Removed | buffer_cache.cpp SynchronizeDmaBuffers |
| SOTC_STREAM_THRESHOLD | 0 (effectively only size 0 streams) | constexpr `stream_threshold = 0`, condition kept exact | buffer_cache.cpp BindBuffer path |
| SOTC_IMG_GPU_OVERLAP | off | Removed; `overlap` clause dropped from the base-address test | texture_cache.cpp InvalidateMemoryFromGPU |
| SOTC_MIP_BIAS | computed DLSS bias | Env override removed, computed path kept | sampler.cpp UpscalerMipBias |
| SOTC_BUF_BIND_FAST | off, alias | Removed. Namespace renamed `SotcBufBindFast` -> `SotcSyncStamps`, `kOn` = SOTC_SYNC_FAST only (same behaviour) | region_manager.h, buffer_cache.cpp (2 uses) |
| SOTC_TEX_BIND_FAST | off | `TexBindFast()` removed. `NumFindMemos` = 1024 (array shrunk from 4096, identical indexing). Report prints literal 1024 | texture_cache.h/.cpp |
| SOTC_CEILING | off, measurement only | Removed from sotc_stats.h (kCeiling, CeilingPart); both skip sites removed (SynchronizeMemory, RefreshImage) | sotc_stats.h, buffer_cache.cpp, texture_cache.cpp |
| SOTC_RT_CLASS | off | Whole family removed: rt_class.h and rt_class.cpp deleted; calls removed in texture_cache.cpp (InRtBlock, RtClassSkipTrack, RtClassNeverDownloads, download, CPU-invalidate, track, two FindTexture/FindRenderTarget gates), probe_alloc.cpp (include, OnBlock, OnFreeBlock), driver.cpp (include, Report) | |
| SOTC_RB_CLASS mode 2 | `=2` only | Mode 2 removed: `rb_class2_env`, `rb_prev_book` (now `rb_prev_env`), TryServePrevious class-2 path (c2_win, refuse counters, mask spans, CoveredBy, foreign, unasked-kept, c2 served counters), MaskHookOn, class-2 report line, hooks.h include, RecordMaskSpan/MaskSpansSince/CoveredBy and mask counters in rb_class.cpp/.h, XPR_CONSUME_MASK_INDIRECT handler and RecordMask in probe_xpr.cpp | |
| SOTC_RB_CLASS2_PARTS | default 0 | Removed with kClass2Serve, kClass2WindowDrop, Class2Parts() | rb_class.h/.cpp |
| SOTC_RB_CLASS mode 1 | off | KEPT: `tools/pcmem_matrix.py` profiles (dp_class, rbclass1, rbahead, rb, rt, etc.) set `SOTC_RB_CLASS=1`. Only `=1` is read now | rb_class.cpp On() |
| BufferCache::IsCpuClean, OwedIntersects (coordinator item) | no callers in main tree (grep) | Removed with their decls. Comment-only SOTC_HLE_TABLE_GEN mentions reworded (buffer_cache.h, buffer_cache.cpp, memory_tracker.h, region_manager.h) | |

Behaviour change to note: `SOTC_RB_CLASS=2` used to enable mode 1 plus the serve. It now disables the class registry (only `=1` enables). This is the removal the brief asked for.

Also removed as dead code: the `SOTC_DMA_MERGE=0` per-range sweep, and the `desync` repair path. The counters `desync_repairs` and `desync_legit` are kept and still printed.

## Files changed (owned)

- gpu/shadps4/video_core/sotc_stats.h: CEILING block removed (kCeiling, CeilingPart).
- gpu/shadps4/video_core/page_manager.cpp: StackGuardMode, DESYNC_REPAIR mode/repair branch, CLEAN_READS flag.
- gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp: CEILING skip, DMA_SYNC_SKIP, DMA merge (non-merge branch removed, block re-indented), STREAM_THRESHOLD constant, SotcSyncStamps rename, IsCpuClean/OwedIntersects removed, RB class mode 2 (see table), rb_prev_book -> rb_prev_env, c2 report removed, hooks.h include removed.
- gpu/shadps4/video_core/buffer_cache/buffer_cache.h: class-2 comment removed, DMA_MERGE comment reworded, IsCpuClean/OwedIntersects decls removed, SOTC_HLE_TABLE_GEN comments reworded.
- gpu/shadps4/video_core/buffer_cache/region_manager.h: SotcBufBindFast -> SotcSyncStamps, comment reworded.
- gpu/shadps4/video_core/buffer_cache/memory_tracker.h: comment reworded only.
- gpu/shadps4/video_core/texture_cache/texture_cache.cpp/.h: IMG_GPU_OVERLAP, TEX_BIND_FAST (TexBindFast removed, NumFindMemos = 1024), CEILING skip, RT_CLASS calls and include.
- gpu/shadps4/video_core/texture_cache/sampler.cpp: MIP_BIAS override.
- gpu/sotc/rb_class.h/.cpp: mode 2 removed (see table), `#include <algorithm>` replaced by `<cstddef>`.
- gpu/sotc/rt_class.h, gpu/sotc/rt_class.cpp: DELETED (worktree only; no CMake change needed, the glob picks up the removal).
- Caller edits (removing calls only): gpu/sotc/probe_xpr.cpp (RecordMask, OnConsumeIndirect, XPR_CONSUME_MASK_INDIRECT registration, rb_class.h and sotc_stats.h includes); gpu/sotc/probe_alloc.cpp (rt_class include, two RT calls for OnBlock/OnFreeBlock. RB calls kept); gpu/shadps4/core/libraries/videoout/driver.cpp (rt_class include and SotcRtClass::Report line).

## Syntax check

Compiled with `-fsyntax-only` using the main tree's compile_commands flags (clang++ from `C:/msys64/clang64/bin`), with worktree `gpu/shim` and `gpu/shadps4` first in the include path. The sources were checked against an overlay of the main tree's gpu/ tree with my edited files on top. `rb_class.cpp` and `rb_host.cpp` used the probe_xpr.cpp flags (they are not in compile_commands.json). Result: all 11 files pass with no errors or warnings: buffer_cache.cpp, pc_ring_copy.cpp, page_manager.cpp, texture_cache.cpp, sampler.cpp, rb_class.cpp, rb_host.cpp, probe_xpr.cpp, probe_alloc.cpp, particle_prev.cpp, driver.cpp.

Not checked: vk_rasterizer.cpp, which will fail until the snippet below is applied (it calls `TexBindFast()`).

## Snippets and lines for files I do not own

1. gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, `Rasterizer::TexBlockMode` (about line 5900). Replace the body with:
```cpp
int Rasterizer::TexBlockMode() {
    // sotc: SOTC_TEX_BLOCK, read once. 1: a stage's texture block is replayed from the memo when
    // it can be; 2: also runs the full bind on a would-be hit and compares the two (verify).
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TEX_BLOCK");
        if (!v) {
            return 0;
        }
        return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
    }();
    return mode;
}
```
   Also drop the `#include` use only if unused. Comments at lines 3608 and 5633 mention SOTC_STREAM_THRESHOLD (reword).
2. gpu/sotc/static_class.h:24-25 comment mentions SOTC_BUF_BIND_FAST. Reword to SOTC_SYNC_FAST only.
3. scripts/hooks.d/staticclass.py:10 comment: same rewording.
4. gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:389 comment mentions SOTC_BUF_BIND_FAST (reword).
5. gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp:102 comment mentions SOTC_CLEAN_READS=0 (remove the parenthetical).
6. gpu/shim/bbgpu.cpp:322 comment mentions SOTC_HLE_TABLE_GEN (coordinator's item, not mine).
7. scripts/hooks.d/xpr.py: the XPR_CONSUME_MASK_INDIRECT record (lines 42-53, plus docstring lines 8-9 and comment line 46) is now without a handler. Delete the record, otherwise it still installs an unused detour. Keep the XPR_CONSUME_MASK record: its label probe is a live diagnostic.

## Rows and lines that name deleted switches (for the coordinator)

- launcher/sotc_tuning.py:1562: `('SOTC_CEILING', ...)` row. Delete.
- tools/pcmem_matrix.py:
  - RT_CLASS: lines 91, 96, 102, 110, 196, 204 (profiles dp_class, dp_*, rt, etc.). Remove the key, or the profile stops measuring.
  - RB_CLASS=2 and RB_CLASS2_PARTS: line 122 (rb2) and 126-128 (rb2serve, with RB_CLASS2_PARTS=1). Remove the profiles.
  - TEX_BIND_FAST and BUF_BIND_FAST: lines 140, 142 (drawfast, drawverify), 190 (drawall). Remove those keys.
  - RB_CLASS=1 stays (profiles rbclass1, rbahead, rb, dp_class).
- Docs (not edited, per brief): CHANGELOG.md:112 (RT_CLASS), :115 (RB_CLASS mode 2), :285 (CEILING); docs/sotc/MODELO-MEMORIA-PC-2.md:46 (RT_CLASS row); docs/re-stutters/HANDOFF.md:23 (RB_CLASS2_PARTS); docs/re-stutters/PC-MEMORY-REWRITE.md:211, 225, 320 (RT_CLASS); docs/re-stutters/native/loader.md:170, 181 (rt_class.cpp); docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md:215, 227 (RT_CLASS); docs/re-stutters/reports3/draw/switches-review.md:4, 13, 14, 36-75, 141, 148 (TEX/BUF_BIND_FAST); docs/re-stutters/native/impl-results.md:177 (XPR_CONSUME_MASK_INDIRECT); docs/BIND-PATH-DESIGN.md (mentions the removed names, not checked line by line).

## Not done / remaining

- Nothing committed. No game run, no build.
- The `dma_runs` comment and "class 2" wording in `buffer_cache.h` are cleaned. Mode-1 `SOTC_RB_CLASS` comments remain (they are still live).
- Main tree may have newer buffer_cache.* than my copies (I copied before the HLE_TABLE_GEN commit). I applied the coordinator's hunks on top of my copies. The overlay check passed on the combined result.
