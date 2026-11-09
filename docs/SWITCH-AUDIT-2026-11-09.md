# Switch audit (9 Nov 2026)

Every `SOTC_*` / `BB_*` environment switch read by the fork (18a1251) or by upstream/main before the 0.72.2 merge,
classified by six read-only passes (one per name range) from the code defaults, the launcher, `sotc.py`, the local
`sotc.ini`, CHANGELOG and the measurement docs. Rows marked "unsure" were kept in a keep category on purpose.

| Category | Count |
|---|---|
| Remove: switch and code path (off, unused, or measured worse) | 73 |
| Remove the switch, keep the behaviour (always on) | 85 |
| Unused names (no reader, fragments, or upstream-only before the merge) | 57 |
| Keep: player options | 175 |
| Keep: on in normal play | 122 |
| Keep for now: work in progress | 146 |
| Keep: development tools | 305 |
| Total | 963 |

## Remove: switch and code path (off, unused, or measured worse)

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_FSR_SHARPNESS` | n.a. | none | gpu/shim/bbport_settings.cpp:120 | Only fills the settings field sharpness (bbport_settings.cpp:38, 203), which no renderer reads; launcher sharpness uses BB_RCAS_ATTENUATION. |
| `BB_JITTER` | on | none | gpu/shim/bbport_settings.cpp:120 | Only stored in settings field jitter (bbport_settings.h:45) and printed at bbport_settings.cpp:203; no renderer reads it. |
| `BB_OBJECT_MOTION` | on | none | gpu/shim/bbport_settings.cpp:123 | Only stored (bbport_settings.h:47, bbport_settings.cpp:134) and printed; no renderer reads it. |
| `BB_REACTIVE` | off | none | gpu/shim/bbport_settings.cpp:121 | Only stored in settings field reactive (bbport_settings.h:46) and printed; no renderer reads it. |
| `BB_REACTIVE_MAX` | value | none | gpu/shim/bbport_settings.cpp:122 | Only stored in settings field reactive_max and printed; no renderer reads it. |
| `BB_REACTIVE_SCALE` | value | none | gpu/shim/bbport_settings.cpp:121 | Only stored in settings field reactive_scale and printed; no renderer reads it. |
| `BB_REACTIVE_THRESHOLD` | value | none | gpu/shim/bbport_settings.cpp:122 | Only stored in settings field reactive_threshold and printed; no renderer reads it. |
| `BB_READBACK_LINEAR` | off | none | gpu/shim/core/emulator_settings.h:83 | Off by default, no setter, and no tool or doc reference; only reader is texture_cache.cpp:267. |
| `SOTC_ALIAS_LOG` | off | none | - | upstream-only T38 diagnostic (upstream runtime_memory.c:991); no tool and no fork doc uses it |
| `SOTC_ALIAS_SYNC` | off | none | - | upstream-only; upstream runtime_memory.c:727 notes 02/11 mosaic and crashes unchanged with it, so no gain |
| `SOTC_BACKING_WARM` (removed) | off | none | gpu/shim/sotc_backing_warm.cpp:24 | CHANGELOG 32: experimental, and measured without gain together with SOTC_STAGING_CACHED |
| `SOTC_BIG_BIND_LOG` (removed) | off | none | gpu/sotc/region_predrain.cpp:53 | diagnostic of the region predrain, off by default (region_predrain.cpp:53); no ini, tool or doc uses it |
| `SOTC_BIND_CACHE_WIDE` (removed) | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4772 | sotc.ini sets 0 (8 Nov): bind cache for compute off, it cost about 3 ms of draw thread |
| `SOTC_CLEAN_GUEST_READS` | off | none | - | upstream-only opt-in (=1) with no tool or doc; only a counter in upstream sotc_stats.h:85 |
| `SOTC_COPY_HLE_WHOLE` | off | none | - | upstream-only; off, nothing sets it, no doc or tool |
| `SOTC_COPY_LOCK_ONCE` | off | none | gpu/shim/bbgpu.cpp:167 | PLANO-THREAD-DESENHO.md:232 says it gave no gain; no ini or tool sets it |
| `SOTC_CP_FREE_NOW` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4639 | one-off free-frame experiment (liverpool.cpp:4639); only a ROADMAP bisect row names it |
| `SOTC_CP_FREE_TASKS` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4590 | one-off free-task experiment (liverpool.cpp:4590), never measured; no tool, no ini |
| `SOTC_CP_PRECISE_WAIT` (removed) | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:137 | command-path.md:440 says rejected, the depth broke with =2 (5 Nov); no ini, tool or launcher sets it |
| `SOTC_EOP_DEFER` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:5608 | Off and set by nothing; docs/sotc/REVISAO-DO-PROJETO.md:124 records that writing EOP labels early (=1) drops FPS; no tool uses it. |
| `SOTC_FAULT_GRACE` | off | none | gpu/shadps4/video_core/buffer_cache/region_manager.h:58 | Off and set by nothing; UMA page-tracking tuning named only in the switch audit (DELETE-AT-NATIVE); no tool or recent doc uses it. |
| `SOTC_FAULT_GRACE_PAGES` | value | none | gpu/shadps4/video_core/buffer_cache/region_manager.h:62 | Knob of SOTC_FAULT_GRACE; same evidence, nothing sets it. |
| `SOTC_FAULT_GRACE_US` | value | none | gpu/shadps4/video_core/buffer_cache/region_manager.h:69 | Knob of SOTC_FAULT_GRACE; same evidence, nothing sets it. |
| `SOTC_FILL_GPU` (removed) | off | none | gpu/sotc/nonblocking.h:396 | Off and set by nothing; no doc or tool mentions it (only code comments in vk_shader_hle.cpp:610 and :762); one-off experiment. |
| `SOTC_FLAT_ARENA` (removed) | off | none | gpu/sotc/nonblocking.h:378 | Off and set by nothing; memory note smooth-config-2026-11-09.md:18 says "Off on purpose: SOTC_FLAT_ARENA (worse)". |
| `SOTC_GLYPH_LOG` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2931 | texture_cache.cpp:2931 says it is superseded by SOTC_FPS; no setter, no tools |
| `SOTC_GPL_PREDICT_EVICT_S` (removed) | value (180) | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2006 | eviction age of the GPL predict path that sotc.ini turns off; no docs, no tools |
| `SOTC_GPUMOD_WATCH` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6520 | off UMA page-mod watch diagnostic, no setter, no tools or recent docs; switch-audit DELETE-AT-NATIVE |
| `SOTC_GPUPAGE_WRITERS` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1301 | off (only =1 enables), one-off writers diagnostic, no setter, no docs, no tools |
| `SOTC_GUARD_DEDUP` | off | none | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:184 | off, no setter, no tools; UMA heap-guard family, switch-audit DELETE-AT-NATIVE |
| `SOTC_GUARD_STALE` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5961 | off (=1 enables), no setter; UMA heap-guard family, switch-audit DELETE-AT-NATIVE |
| `SOTC_GUARD_USER_POINTER` | off | none | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:472 | off (=1 enables), no setter; UMA heap-guard family, switch-audit DELETE-AT-NATIVE |
| `SOTC_HEAP_WATCH` | off | none | src/probe.c:2260 | off UMA heap watch diagnostic, no setter; switch-audit DELETE-AT-NATIVE |
| `SOTC_HLE_COPY_EXACT` (removed) | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:174 | off (only =1 enables), no setter, no docs, no tools |
| `SOTC_HLE_FILL_SKIP` (removed) | off | none | gpu/sotc/nonblocking.h:369 | off (SwitchOn), no setter, no docs; needs the UMA generation tracking the native model removes |
| `SOTC_HOOKS_LEARNED` (removed) | off | none | gpu/sotc/native/fault_emu.cpp:738 | fault-emulation family (UMA), no setter, no docs |
| `SOTC_HOOK_EXAMPLE_HOOK` | off | none | n.a. | test or example name only (switch-audit DELETE-DEAD) |
| `SOTC_HOOK_LABEL_WRITER` | off | none | n.a. | test or example name only (switch-audit DELETE-DEAD) |
| `SOTC_HOOK_SAMPLE` | off | none | n.a. | sample name only (switch-audit DELETE-DEAD) |
| `SOTC_HOOK_XPRMEM_PROBE` (removed) | off | none | gpu/sotc/probe_xprmem.cpp:3 | UMA xpr-memory probe, no setter; switch-audit DELETE-AT-NATIVE |
| `SOTC_IMG_GPU_OVERLAP` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:527 | off, no setter, no docs; switch-audit DELETE-UNUSED (vendored, the port never sets it) |
| `SOTC_IMG_WATCH` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1554 | UMA image watch diagnostic, no setter; switch-audit DELETE-AT-NATIVE |
| `SOTC_LDS_BARRIERS` | value | none | gpu/shadps4/shader_recompiler/ir/passes (shared memory barrier pass, switch-audit) | vendored shadPS4 option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LDS_SMALL` | value | none | gpu/shadps4/shader_recompiler/ir/passes (shared memory barrier pass, switch-audit) | vendored shadPS4 option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LOAD_WARM_TEX` (removed) | off | none | gpu/sotc/load_warm.cpp:378 | CHANGELOG.md:38 says measured without gain (about 1% of textures); off, no setter |
| `SOTC_LOCK_WATCH` (removed) | off | none | gpu/shadps4/video_core/page_manager.cpp:180 | off UMA lock-watch diagnostic, no setter; switch-audit DELETE-AT-NATIVE |
| `SOTC_LOD_FB_BIAS` | value | none | gpu/shadps4/video_core/sotc_lod_stats.h:247 | vendored feedback option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LOD_FB_HOLD_MS` | value | none | gpu/shadps4/video_core/sotc_lod_stats.h:267 | vendored feedback option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LOD_FB_KEEP` | value | none | gpu/shadps4/video_core/sotc_lod_stats.h:259 | vendored feedback option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LOD_FB_MIN` | value | none | gpu/shadps4/video_core/sotc_lod_stats.h:255 | vendored feedback option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_LOD_KEEP` | off | none | gpu/shadps4/video_core/sotc_lod_stats.h:307 | vendored feedback option, nothing sets it (switch-audit DELETE-UNUSED) |
| `SOTC_MEMTYPE_STATS` (removed) | off | none | gpu/sotc/probe_memtype.cpp:578 | memtype probe statistics, off, no setter, no docs |
| `SOTC_MIP_BIAS` | value | none | gpu/shadps4/video_core/texture_cache/sampler.cpp:21 | vendored option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_MVEC_FILL` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1683 | vendored DLSS option, nothing sets it, no docs (switch-audit DELETE-UNUSED) |
| `SOTC_N1_HOSTCALLER` (removed) | off | none | gpu/shadps4/video_core/page_manager.cpp:1666 | diagnostic for a port routine writing on a guest thread; no doc, tool or launcher reference |
| `SOTC_NATIVE_FREE_DISCARD` (removed) | off | none | gpu/sotc/native/native.cpp:987 | off by default; no doc, tool, launcher or sotc.ini reference |
| `SOTC_NATIVE_GPUMARK_SKIP` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9282 | diagnostic (skips the exact copy over GPU-written bytes); no doc or tool reference |
| `SOTC_NATIVE_REUSE_PARTIAL` (removed) | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:535 | experimental off switch; no doc, tool, launcher or sotc.ini reference |
| `SOTC_NATIVE_TRACK_EMU` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:34 | fault emulation; native-terrain-fix memory says fault emulation was rejected |
| `SOTC_NATIVE_TRACK_EMU_HOT` (removed) | value | none | gpu/sotc/native/fault_emu.cpp:782 | sub-switch of the rejected fault emulation (native-terrain-fix memory) |
| `SOTC_NATIVE_TRACK_RANGE` (removed) | value | none | gpu/shadps4/video_core/page_manager.cpp:1377 | diagnostic page-range filter for TRACK=1; no doc, tool or sotc.ini reference |
| `SOTC_NATIVE_UNIFIED` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6325 | unified memory was rejected (native-terrain-fix memory); off and not in sotc.ini |
| `SOTC_NATIVE_UNIFIED_HOT` (removed) | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8584 | sub-switch of the rejected unified memory (native-terrain-fix memory) |
| `SOTC_NATIVE_UNIFIED_LABELS` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8837 | sub-switch of the rejected unified memory (native-terrain-fix memory) |
| `SOTC_OPS_PROBE` (removed) | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2352 | observe-only probe; no doc or tool references the switch name |
| `SOTC_PC_RING_HEAPSTAT` (removed) | off | none | gpu/sotc/pc_ring.cpp:1808 | per-memtype diagnostic print; no doc or tool reference |
| `SOTC_PC_RING_SLOW` (removed) | off | none | gpu/sotc/pc_ring.cpp:674 | old posted declare kept as a fallback; superseded by the default declare path |
| `SOTC_RB_CHANGED` (removed) | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1382 | write-fault readback diagnostic (with NATIVE_TRACK=1); no doc or tool reference |
| `SOTC_SETENTRY_PROBE` (removed) | off | none | scripts/hooks.d/setentry_probe.py:19 | one-off hook probe of BPXPR_BuildSetEntry; no tool, doc, sotc.ini or launcher use (docs count 0) |
| `SOTC_SHADOW_STAGGER` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1236 | docs/re-stutters/reports2/14-draw-sources.md:32 says the stagger blinks with a still camera; off, set by nothing |
| `SOTC_SKIP_DRAW_VS` (removed) | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3256 | one-off hex mask bisect knob that skips draws by vertex shader; no tool, doc or setter; default 0 |
| `SOTC_STREAM_THRESHOLD` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1127 | upstream-only: default 0 chosen because the arena beats copying (+7% on the heavy benchmark, per the code comment); no setter sets a non-zero value |
| `SOTC_UNTRACK_AUTO` (removed) | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:248 | CHANGELOG.md:290 measured: it does not reduce the faults (29 runs); off and not in sotc.ini; its tuning row must go with it |

## Remove the switch, keep the behaviour (always on)

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `SOTC_ACCESS_REUSE` | on | none | - | upstream-only (absent at 18a1251): upstream vk_runtime.cpp:110 reads !off, default on, nothing turns it off |
| `SOTC_ACCESS_UNION` | on | none | - | upstream-only (absent at 18a1251): upstream vk_runtime.cpp:111 reads !off, default on, nothing turns it off |
| `SOTC_ARENA_RELEASE` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:12115 | on by default since 2026-10-16 (vk_rasterizer.cpp:12108); no ini, tool or doc turns it off |
| `SOTC_ASYNC_COMPUTE` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:666 | default on (vk_pipeline_cache.cpp:662); no ini, tool or doc turns it off |
| `SOTC_ASYNC_SHADERS` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:646 | default on (vk_pipeline_cache.cpp:642); no ini, tool or doc turns it off |
| `SOTC_ASYNC_TRANSLATE` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:656 | default on (vk_pipeline_cache.cpp:652); no ini, tool or doc turns it off |
| `SOTC_BIND_CLAMP1` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:9341 | default on (vk_rasterizer.cpp:9341 and two more reads); no ini, tool or doc turns it off |
| `SOTC_BININFO_CACHE` | on | none | gpu/shadps4/video_core/amdgpu/sotc_bininfo.cpp:34 | default on (sotc_bininfo.cpp:34); no ini, tool or doc turns it off |
| `SOTC_BININFO_L1` | on | none | gpu/shadps4/video_core/amdgpu/sotc_bininfo.cpp:57 | default on (sotc_bininfo.cpp:57); no ini, tool or doc turns it off |
| `SOTC_CACHE_MAP` | on | none | gpu/sotc/nonblocking.h:212 | SwitchOnByDefault (nonblocking.h:212); no ini, tool or doc turns it off |
| `SOTC_CARVE_SUSPEND` | on | none | src/runtime_memory.c:628 | default on (runtime_memory.c:628); no ini, tool or doc turns it off |
| `SOTC_CLAMP_GAP` | on | none | - | upstream-only; default on (upstream runtime_memory.c:1650); nothing turns it off |
| `SOTC_CLEAN_READS` | on | none | - | upstream-only; impl-n4-n4mem.md records the env read removed and the path always on |
| `SOTC_COMPUTE_RELEASE` | on | none | gpu/sotc/nonblocking.h:221 | SwitchOnByDefault (nonblocking.h:221); no ini, tool or doc turns it off |
| `SOTC_COPY_BATCH` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9864 | default on (buffer_cache.cpp:9864); no ini, tool or doc turns it off |
| `SOTC_COPY_PREFETCH` | on | none | - | upstream-only; default on (upstream bbgpu.cpp:156), =0 is one fault per page as before; nothing turns it off |
| `SOTC_CP_YIELD` | on | none | - | upstream-only; default 1 (upstream liverpool.cpp:584); nothing turns it off |
| `SOTC_DEPTH_RO` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10107 | On unless =0 in code; no launcher, sotc.ini or tool sets 0; CHANGELOG 838 only names the off value. |
| `SOTC_DEPTH_TWIN` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:829 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_DLSSG_PARALLEL` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:785 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_DLSSG_SLEEP` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1945 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_DLSS_REPEAT` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1241 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_DLSS_VIGNETTE` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8945 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_DMA_INCR` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7594 | On unless =0 in code; docs/re-stutters/reports3/draw/wave1/v1buf.md:95 calls the default incremental sweep the normal path. |
| `SOTC_DMA_OVERLAP` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:650 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_EARLY_VO_LABEL` | on | none | gpu/shadps4/core/libraries/videoout/driver.cpp:192 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_EVICT_ASYNC` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10866 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FAST_BIND` | on | none | gpu/shadps4/video_core/sotc_stats.h:74 | On unless =0 in code (kFastBind); the CHANGELOG 914 "off by default" note is superseded; nothing sets 0 for play. |
| `SOTC_FG_UI_COPY` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:709 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FILL_HLE` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:687 | On unless =0; the memory note fill-hle-off-black-screen says =0 gives a black scene, so the off path is broken; keep HLE on. |
| `SOTC_FILL_HLE_FAST` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:775 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FILL_HLE_PAGES` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:779 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FIND_MEMO` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3793 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FLIP_LATE` | on | none | gpu/shadps4/core/libraries/videoout/driver.cpp:3588 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_FLIP_PACING` | on | none | gpu/shadps4/core/libraries/videoout/driver.cpp:3336 | On unless =0 in code; nothing sets 0 for play. |
| `SOTC_GC_DOWNLOAD` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:370 | default on in code (0 disables) and nothing sets 0 for play; switch-audit KEEP-DEFAULT-ON |
| `SOTC_GENERAL_SYNC` | on | none | gpu/shadps4/video_core/texture_cache/image.cpp:587 | default on (only =0 restores a barrier) and nothing sets 0; the switch-audit reading "off" is wrong |
| `SOTC_GPL_DYN_STATE` | on | none | gpu/sotc/nonblocking.h:94 | SwitchOnByDefault and nothing in launcher, sotc.ini, modes or tools sets it to 0 |
| `SOTC_GPL_LIB_PHASE` | on | none | gpu/sotc/nonblocking.h:148 | SwitchOnByDefault and nothing sets it to 0 |
| `SOTC_GPL_NODROP` | on | none | gpu/sotc/nonblocking.h:63 | default on (info.h:597 comment), nothing sets 0; it gates SOTC_GPL_PREDICT |
| `SOTC_GPL_PACK_LEAN` | on | none | gpu/sotc/nonblocking.h:185 | SwitchOnByDefault and nothing sets it to 0 |
| `SOTC_GPL_PACK_LIBS` | on | none | gpu/sotc/nonblocking.h:132 | SwitchOnByDefault and nothing sets it to 0 |
| `SOTC_GPL_PARALLEL_LIBS` | on | none | gpu/sotc/nonblocking.h:140 | SwitchOnByDefault and nothing sets it to 0 |
| `SOTC_GPUMOD_NOGROW_SKIP` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6549 | default on (0 disables), nothing sets 0; UMA family, switch-audit DELETE-AT-NATIVE |
| `SOTC_GPU_REGION_PAGE_ISOLATE` | on | none | gpu/sotc/gpu_region.cpp:308 | default on (only =0 turns it off), nothing sets 0, no docs |
| `SOTC_HEAP_FAST` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5689 | default on (only a value of 0 turns it off), nothing sets it; UMA heap-guard family, switch-audit DELETE-AT-NATIVE |
| `SOTC_HEAP_GUARD` | on | none | src/probe.c:1086 | default on (0 disables), nothing sets 0; UMA family, switch-audit DELETE-AT-NATIVE |
| `SOTC_INDIRECT_DIRECT` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:210 | default on (0 disables), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_LOAD_FLUSH` | on | none | src/runtime_file.c:454 | default on (only =0 turns it off), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_LOCK_WAKE` | on | none | src/runtime_memory.c:125 | default on (only =0 turns it off), nothing sets 0; UMA family, switch-audit DELETE-AT-NATIVE |
| `SOTC_LOD_FB_USE` | on | none | gpu/shadps4/video_core/sotc_lod_stats.h:262 | default on (only =0 turns it off), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_MANIFEST_HOLD_FAST` | on | none | gpu/sotc/nonblocking.h:306 | SwitchOnByDefault and nothing sets it to 0 |
| `SOTC_MAP_INVALIDATE` | on | none | gpu/shim/bbgpu.cpp:142 | default on (only =0 turns it off), nothing sets 0; UMA page family, switch-audit DELETE-AT-NATIVE |
| `SOTC_MAYBE_DIRTY_HASH` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2529 | default on (only =0 turns it off), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_MEMO_ADDR_GEN` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3785 | default on (only =0 turns it off), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_META_CLEAR` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8764 | default on (only =0 turns it off), nothing sets 0; switch-audit KEEP-DEFAULT-ON |
| `SOTC_NATIVE_EXACT` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9248 | exact copy is the default; =0 only selects a diagnostic page-mark path; nothing sets 0 for play |
| `SOTC_NATIVE_EXACT_CLEAR` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1658 | on unless =0; nothing sets 0 for play |
| `SOTC_NATIVE_IMAGE_REWRITE` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1590 | on unless =0; nothing sets 0 for play |
| `SOTC_NATIVE_IMAGE_VERIFY` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1454 | on unless =0; nothing sets 0 for play |
| `SOTC_PC_RING8_CLIP` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8456 | on unless =0; nothing sets 0 for play |
| `SOTC_PC_RING8_DEDUPE` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8194 | on unless =0 (duplicate-upload dedupe); nothing sets 0 for play |
| `SOTC_PC_RING8_LABELS` | on | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:230 | on by default (0 disables); nothing sets 0 for play |
| `SOTC_PIPE_MEMO` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:3828 | graphics key memo is on unless set (comment); nothing sets it for play |
| `SOTC_POP_FAST` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:618 | on unless =0 (early-out of PopPendingOperations); nothing sets 0 for play |
| `SOTC_PREFETCH` | on | none | src/probe.c:2256 | archive read-ahead is on unless =0; nothing sets 0 for play |
| `SOTC_PRETRANSLATE_PACK` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:1119 | pack format on by default; =0 writes loose files; unsure whether anything sets 0 |
| `SOTC_PROTECT_FAST` | on | none | src/runtime_memory.c:2420 | on unless =0 (read-lock bypass); nothing sets 0 for play |
| `SOTC_RB_ASYNC` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:188 | asynchronous readbacks on unless 0 (comment at buffer_cache.cpp:174); nothing sets 0 for play |
| `SOTC_RB_EARLY` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8421 | on by default since 2026-10-16 (comment); nothing sets 0 for play |
| `SOTC_RB_NARROW` | on | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:139 | precise readback window is the default (comment at line 156); nothing sets 0 for play; unsure |
| `SOTC_REGION_FAST` | on | none | src/runtime_memory.c:2124 | on unless =0 (misses take the lock); nothing sets 0 for play |
| `SOTC_REGION_LOG` | on | none | src/runtime_memory.c:297 | on unless =0 (VMA range log); nothing sets 0 for play |
| `SOTC_RELEASE_ORDER` | on | none | src/runtime_memory.c:1661 | on unless =0 (zeroing after the GPU unmap hook); nothing sets 0 for play |
| `SOTC_REUSE_SIZE_CHECK` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:939 | on unless =0 (old view when reload size matches); nothing sets 0 for play |
| `SOTC_SAMPLER_MEMO` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3030 | on unless =0; no setter, no UI, no tool; the off path is only an A/B |
| `SOTC_SAVE_BACKUP` | on | none | src/runtime_savedata.c:569 | on unless =0; save backup is always wanted for play; nothing sets 0 |
| `SOTC_STACK_GUARD` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/page_manager.cpp:106 | upstream-only: default 1 (lock held through host Protect); =0 is the old early release for A/B (docs/re-stutters/native/impl-n4-n4mem.md:13); nothing sets 0 |
| `SOTC_STORE_HOOK_RELOCATE` (removed with SOTC_NATIVE_TRACK_EMU) | on | none | gpu/sotc/cpu_patches.cpp:3688 | on unless =0; =0 is a diagnostic that keeps the store faulting; nothing sets it for play |
| `SOTC_TC_BATCH` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.h:292 | on unless =0; no setter, UI or doc of an off run for play |
| `SOTC_TC_BIASED` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.h:207 | on unless =0; no setter, UI or doc of an off run for play |
| `SOTC_TC_SKIP_EMPTY` | on | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:480 | on unless =0; no setter, no doc of an off run |
| `SOTC_TEX_BIND_P1` | on | none | gpu/shadps4/video_core/texture_cache/image.cpp:518 | on unless =0; no setter, no doc of an off run |
| `SOTC_UI_HIRES` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1130 | on unless =0; no setter; off path only for A/B |
| `SOTC_WRITE_FAST` | on | none | src/runtime_memory.c:2148 | on unless =0 (and region log on); no setter; off path only for A/B |

## Unused names (no reader, fragments, or upstream-only before the merge)

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_FRAME_` | n.a. | none | sotc.py:275 | Prefix fragment: appears only in the sotc.py prefix list, no getenv read. |
| `BB_HOST_` | n.a. | none | none | Prefix fragment: no occurrence at 18a1251. |
| `BB_PAD_` | n.a. | none | sotc.py:273 | Prefix fragment: appears only in the sotc.py prefix list, no getenv read. |
| `BB_TRACE` | n.a. | none | sotc.py:275 | Only in the sotc.py prefix list; nothing reads BB_TRACE at 18a1251 (upstream/main has also dropped it). |
| `BB_X` | n.a. | none | none | Fragment: no occurrence at 18a1251. |
| `SOTC_CSDR_` | n.a. | none | sotc.py:271 | prefix fragment only (sotc.py:271 prefix list), no reader of this exact name |
| `SOTC_DESYNC_REPAIR` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; impl-n4-n4mem.md:12 says the repair was removed; upstream/main still has 2 hits, so the merge would bring it back. |
| `SOTC_DET` | n.a. | none | sotc.py:267 | Only a prefix fragment in the env-forwarding list (sotc.py:267); no reader. |
| `SOTC_DET_IO` | off | tools | sotc.py:91 | Set by the sotc.py det profile, but no code reads it (only sotc.py:44 and :91 name it); remove from the profile or wire it up. |
| `SOTC_DLSS_BIAS` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 4 hits, so the merge would bring it back. |
| `SOTC_DLSS_PRESET` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 4 hits, so the merge would bring it back. |
| `SOTC_DMA_MERGE` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; impl-n4-n4mem.md:14 says the switch was removed (merge always on); upstream/main has 3 hits. |
| `SOTC_DMA_MERGE_MAX` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; impl-n4-n4mem.md removed it with SOTC_DMA_MERGE; upstream/main has 2 hits. |
| `SOTC_DMA_SYNC_SKIP` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; impl-n4-n4mem.md:16 says removed; upstream/main has 2 hits. |
| `SOTC_EARLY_Z` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 2 hits. |
| `SOTC_EXAMPLE` | n.a. | none | gpu/sotc/sotc_live.h:9 | Only named in a doc comment as the SotcLive::Int example; no reader. |
| `SOTC_FLAG` | n.a. | none | tests/test_offline_matrix.py:89 | Test fixture name only; no reader. |
| `SOTC_FLOAT_ATOMIC_FALLBACK` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 2 hits. |
| `SOTC_FOO` | n.a. | none | tools/sotc_matrix.py:13 | Example name in a tools docstring and in tests; no reader. |
| `SOTC_FORMAT_FALLBACK` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 4 hits. |
| `SOTC_FORMAT_LOG` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 6 hits. |
| `SOTC_FSR_DEBUG` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 2 hits. |
| `SOTC_FSR_JITTER` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 4 hits. |
| `SOTC_FSR_VERSION` | n.a. | none | (none at 18a1251) | No hit in the 18a1251 tree; upstream/main has 4 hits. |
| `SOTC_HOOK_` | n.a. | none | gpu/sotc/hooks.h:5 | name fragment only (prefix in a comment), nothing reads it |
| `SOTC_LLVM_SYMBOLIZER` | n.a. | tools | n.a. | name only in tools (symbolizer for profiling); no getenv read in the runtime |
| `SOTC_LONG` | n.a. | none | n.a. | name fragment only, one occurrence, nothing reads it |
| `SOTC_MODES` | n.a. | launcher,modes,tools | n.a. | name only: no getenv read; the modes file is used as a path or config by the launcher and tools |
| `SOTC_NAN_GUARD` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code, nothing in the fork reads it |
| `SOTC_NOTE` | n.a. | launcher | launcher/sotc_launcher_win.py:493 | launcher-internal message text, not a game switch; set and read only inside the launcher |
| `SOTC_NOT_A_SWITCH` | n.a. | none | tests/test_sotc_live.py:79 | test fixture name asserting that it is not a live switch |
| `SOTC_NO_BARYCENTRIC` | n.a. | none | n.a. | upstream/main only (1 hit, switch-audit doc); absent from 18a1251 code |
| `SOTC_NO_EXPLICIT_VERTEX` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_NO_RTZ` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_OCCLUSION_STEP` | n.a. | none | n.a. | upstream/main only (1 hit, switch-audit doc); absent from 18a1251 code |
| `SOTC_PASS_TRACE` | n.a. | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:933 | only named in a comment; the code now reads SOTC_TRACE (the comment says it was renamed) |
| `SOTC_PC_MEMORY_PREIMPORT` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_PIPELINE_WORKERS` | n.a. | none | n.a. | upstream/main only (1 hit, switch-audit doc); absent from 18a1251 code |
| `SOTC_POOL_FREEZE` | n.a. | launcher | n.a. | upstream/main only (launcher sets it there); absent from 18a1251 code |
| `SOTC_POOL_FREEZE_TICKS` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_POOL_FREE_TRACE` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_POOL_LOCK_CHECK` | n.a. | none | n.a. | upstream/main only (1 hit); absent from 18a1251 code |
| `SOTC_POOL_OWNER` | n.a. | launcher | n.a. | upstream/main only (launcher sets it there); absent from 18a1251 code |
| `SOTC_PORT_VERSION` | n.a. | launcher | n.a. | upstream/main launcher only; absent from 18a1251 code |
| `SOTC_RB_AGE` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_RB_DIRTY_RETRY` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_RB_TEX_WATCH` | n.a. | none | n.a. | upstream/main only (2 hits); absent from 18a1251 code |
| `SOTC_RDOC_` | n.a. | none | sotc.py:267 | prefix fragment only (listed as a prefix in sotc.py); no reader of this exact name |
| `SOTC_RELEASE_DEFER` | n.a. | launcher | n.a. | upstream/main launcher only; absent from 18a1251 code |
| `SOTC_RUN_` | n.a. | none | sotc.py:267 | prefix fragment only (listed as a prefix in sotc.py); no reader of this exact name |
| `SOTC_SHADOW_REUSE_STATS` | n.a. | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1380 | no getenv read; only a comment saying it is part of SOTC_FPS (kFpsLog) |
| `SOTC_SHOT_` | n.a. | none | sotc.py:268 | prefix fragment only: sotc.py passes SOTC_SHOT_* through; never read as a name |
| `SOTC_TSHARP_TRACE` | n.a. | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10876 | no getenv read; only a comment saying it is part of SOTC_TRACE (SotcTrace::kOn) |
| `SOTC_UI_LINEAR_TRACE` | n.a. | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:11402 | no getenv read; only a comment saying it is part of SOTC_TRACE (SotcTrace::kOn) |
| `SOTC_WINDOW_` | n.a. | none | sotc.py:268 | prefix fragment only: sotc.py passes SOTC_WINDOW_* through; never read as a name |
| `SOTC_X` | n.a. | none | gpu/sotc/lab_commands.h:34 | placeholder name in a comment (lab_commands.h:34) and in tests/test_matrix.py:237; never read by code |
| `SOTC_Y` | n.a. | none | tests/test_matrix.py:237 | placeholder name in a test only; never read by code |

## Keep: player options

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_DIRECT_MEMORY_ACCESS` | value | launcher-ui,launcher | gpu/shim/core/emulator_settings.h:66 | Launcher checkbox for full water reflections (launcher:1369) sets it to 0 when unticked; CHANGELOG:1168 explains it. |
| `BB_FSR1` | off | launcher | gpu/shim/core/emulator_settings.h:75 | Launcher turns it on for the fsr1 and dlss upscaler choices (launcher:715). |
| `BB_FULLSCREEN` | off | launcher-ui,launcher | gpu/shim/window.cpp:158 | Launcher Fullscreen checkbox (launcher:1448) sets it; F11 also toggles in game. |
| `BB_GAMEPAD` | n.a. | launcher | src/runtime_pad.c:121 | Launcher controller choice (launcher:743, summary line 491). |
| `BB_LANGUAGE` | value | launcher-ui,launcher | src/runtime_services.c:75 | Launcher system language choice (launcher:1278, set at launcher:635). |
| `BB_PRESENT_MODE` | value | launcher-ui,launcher | gpu/shim/core/emulator_settings.h:30 | Launcher Presentation choice (launcher:1449, set at launcher:633); default Mailbox. |
| `BB_RCAS` | on | launcher | gpu/shim/core/emulator_settings.h:82 | Launcher sets it 0 or 1 from the upscaler choice (launcher:724-726); default on. |
| `BB_RCAS_ATTENUATION` | value | launcher | gpu/shim/core/emulator_settings.h:31 | Launcher sharpness value is passed here (launcher:727); default 250. |
| `BB_USER_NAME` | value | launcher-ui,launcher | src/runtime_services.c:60 | Launcher Player name field (launcher:1280, set at launcher:637); default Hunter. |
| `SOTC_ADAPTIVE_DEFER` | on | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:73 | launcher Display and FPS flag, default on (sotc_adaptive.h:73) |
| `SOTC_ADAPTIVE_FLUSH_US` | value | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:75 | launcher value row, default 4000 us (sotc_adaptive.h:75) |
| `SOTC_ADAPTIVE_HIGH_MS` | value | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:100 | launcher value row, default 36 ms (sotc_adaptive.h:100) |
| `SOTC_ADAPTIVE_HOLD_MS` | value | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:105 | launcher value row, default 1000 ms (sotc_adaptive.h:105) |
| `SOTC_ADAPTIVE_LOAD` | off | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:68 | launcher flag marked experimental, off by default (sotc_adaptive.h:68) |
| `SOTC_ADAPTIVE_LOW_MS` | value | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:103 | launcher value row, default 34.5 ms (sotc_adaptive.h:103) |
| `SOTC_ADAPTIVE_SHADOW` | on | launcher-ui | gpu/shadps4/video_core/sotc_adaptive.h:72 | launcher flag_on row, default on (sotc_adaptive.h:72) |
| `SOTC_ARENA_EVICT_MB` | value | launcher | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10752 | VRAM profile value set by the launcher (VRAM_LIMITS in sotc_launcher_win.py); default 1536 (buffer_cache.cpp:10740) |
| `SOTC_ARENA_TICK_FREE` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10167 | launcher memory flag, off by default (buffer_cache.cpp:10160) |
| `SOTC_ASC_RPTR` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:6713 | launcher choice, off by default (liverpool.cpp:545 comment, gnmdriver.cpp:620) |
| `SOTC_ASYNC_COMPUTE_XLATE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1091 | launcher flag, off by default (vk_pipeline_cache.cpp:1087) |
| `SOTC_ASYNC_IMAGE_CREATE` | off | launcher-ui | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:59 | launcher flag, off by default (liverpool.cpp:2446 path) |
| `SOTC_ASYNC_JOB_MB` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:62 | launcher value row, default 0 (async_upload.cpp:62) |
| `SOTC_ASYNC_STAGE_CACHED` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:54 | launcher flag, off by default (async_upload.cpp:54) |
| `SOTC_ASYNC_TEXTURE_UPLOAD` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:47 | launcher flag, off by default (async_upload.cpp:47) |
| `SOTC_ASYNC_UPLOAD_MIN_KB` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:70 | launcher value row, default 256 KiB (async_upload.cpp:68) |
| `SOTC_ASYNC_UPLOAD_THREADS` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:69 | launcher value row, default 3 (async_upload.cpp:67) |
| `SOTC_BACK_HEAD_CACHE` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:451 | launcher flag, off by default (liverpool.cpp:893) |
| `SOTC_BACK_PF` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:452 | launcher flag, off by default (liverpool.cpp:1141) |
| `SOTC_BIND_MARK_ONCE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:9000 | launcher flag, off by default (vk_rasterizer.cpp:8994) |
| `SOTC_BUFFER_MEMO` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:234 | launcher flag, but the docs report 96-99 percent hits and no gain (BIND-PATH-DESIGN.md:241); candidate to drop from the UI |
| `SOTC_CARVE_NOSTOP` | off | launcher-ui | src/runtime_memory.c:745 | launcher memory flag; CHANGELOG 714 calls it experimental and off (unsure) |
| `SOTC_CPUSETS` | off | launcher-ui,sotc.ini | gpu/shim/bbport_threads.h:45 | launcher flag; sotc.ini sets 0 |
| `SOTC_CP_AHEAD` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:6698 | launcher choice, off by default (gnmdriver.cpp:107) |
| `SOTC_CP_AHEAD_FRAMES` | value | launcher-ui | gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:159 | launcher value row, default 1 (gnmdriver.cpp:154) |
| `SOTC_CP_FLIP_PACE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:651 | launcher flag, experimental and off (vk_presenter.cpp:650) |
| `SOTC_CP_IDLE_TIMEOUT_US` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:481 | launcher value row, default 200 us (liverpool.h:1361) |
| `SOTC_CP_IDLE_US` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:478 | launcher value row, default 0 (liverpool.cpp:478) |
| `SOTC_CULL_EXPRESS` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:1786 | launcher flag whose label says incomplete; no measurement found (unsure) |
| `SOTC_CULL_EXPRESS_ADDR` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:579 | launcher value under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_MARKS` | on | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:577 | launcher flag_on under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_PATTERN` | on | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:578 | launcher flag_on under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_SEG` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:580 | launcher value under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_SIGNALS` | on | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:576 | launcher flag_on under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_SKIP` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:582 | launcher value under the express cull (unsure) |
| `SOTC_CULL_EXPRESS_US` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:583 | launcher value under the express cull (unsure) |
| `SOTC_DEDUP_PUSH` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:36 | Launcher tuning row (bind group, launcher/sotc_tuning.py:114); CHANGELOG 565 lists it as a user option. |
| `SOTC_DEDUP_STATE` | on | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:35 | On by default; launcher flag_on row (sotc_tuning.py:125) lets the player turn it off. |
| `SOTC_DEPTH_SYNC_PIPE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:4218 | Launcher tuning row (sotc_tuning.py:135); CHANGELOG 557 documents =0 as the off path. |
| `SOTC_DESC_BUFFER` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:600 | Launcher tuning row "Descriptor buffers (Vulkan)" (sotc_tuning.py:145); DRAW-THREAD-DESIGN.md:231 says no measured gain; kept because the player can select it, unsure. |
| `SOTC_DLC_DIR` | n.a. | launcher-ui | src/runtime_content.c:120 | DLC folder set from the launcher settings (launcher/sotc_launcher_win.py:616); also forwarded by sotc.py:269. |
| `SOTC_DLSS_COMPARE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1071 | Launcher sets it from the DLSS compare option (sotc_launcher_win.py:722). |
| `SOTC_DLSS_SHARPEN` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1269 | Launcher sets it from the DLSS sharpen setting (sotc_launcher_win.py:718). |
| `SOTC_DLSS_SKY_SHARPEN` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1271 | Launcher sets it from the DLSS sky-sharpen setting (sotc_launcher_win.py:720). |
| `SOTC_DLSS_UI_SHARPEN` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1270 | Launcher sets it from the DLSS UI-sharpen setting (sotc_launcher_win.py:719). |
| `SOTC_DROP_ASYNC` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:835 | Launcher tuning row (memory group, sotc_tuning.py:291); the switch audit marks it DELETE-AT-NATIVE, unsure. |
| `SOTC_FASTBIND_WIDE` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:81 | Launcher tuning row (bind group, sotc_tuning.py:155); command-path.md:460 says it made the route worse, so review the row; kept, unsure. |
| `SOTC_FAULT_GRANULE` | value | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:925 | Launcher choice (memory group, sotc_tuning.py:317), default 4 KiB; kept as a player option, unsure (audit marks DELETE-AT-NATIVE). |
| `SOTC_FENCE_PIECES` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:50 | Launcher upload row (sotc_tuning.py:958); CHANGELOG 760 calls it experimental; kept as player option, unsure. |
| `SOTC_FG_HUDLESS` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10067 | Launcher sets it to 1 for frame generation (sotc_launcher_win.py:687). |
| `SOTC_FILL_HLE_GPU` | on | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:843 | On by default; launcher flag_on row (upload group, sotc_tuning.py:1106) lets the player turn it off. |
| `SOTC_FLIP_SERVICE` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:597 | Launcher tuning row (present group, sotc_tuning.py:1499); CHANGELOG 657 calls it experimental. |
| `SOTC_FLUSH_EVERY` | value | launcher | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8384 | Set by the launcher from the GPU submit setting (sotc_launcher_win.py:674). |
| `SOTC_FLUSH_US` | value | launcher | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8389 | Set by the launcher from the GPU submit setting (sotc_launcher_win.py:672). |
| `SOTC_FPS_CAP` | off | launcher-ui | gpu/shadps4/video_core/sotc_fps_cap.h:21 | Launcher sets 30 or off from the FPS mode (sotc_launcher_win.py:629). |
| `SOTC_FPS_LIMIT` | value | launcher | gpu/shadps4/core/libraries/videoout/driver.cpp:3348 | Launcher sets 60 or 0 from the FPS mode (sotc_launcher_win.py:630). |
| `SOTC_FRAMEGEN` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:24 | Launcher sets it from the frame-generation setting (sotc_launcher_win.py:685). |
| `SOTC_GAME_DIR` | n.a. | launcher,sotc.ini | launcher/sotc_launcher_win.py:611 | game folder chosen in the launcher settings and written to sotc.ini |
| `SOTC_GPUMOD_CONTAINS` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6558 | exposed in the launcher memory group |
| `SOTC_GPU_THREAD_HIGH` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:80 | launcher threads row (GPU threads at high priority) |
| `SOTC_GRASS` | unsure | launcher | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3689 | unsure default; launcher_win.py sets it as a visual option |
| `SOTC_HASH_EPOCH` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6776 | exposed in the launcher upload group |
| `SOTC_HASH_SMALL` | value (0) | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:905 | exposed in the launcher upload group |
| `SOTC_HDR` | n.a. | none | n.a. | upstream-only (absent at 18a1251, present in upstream/main): HDR output choice arriving with the merge; decide after merge |
| `SOTC_HDR_GAIN` | n.a. | none | n.a. | upstream-only HDR tuning value; decide after merge |
| `SOTC_HDR_PAPER` | n.a. | none | n.a. | upstream-only HDR tuning value; decide after merge |
| `SOTC_HDR_WHITE` | n.a. | none | n.a. | upstream-only HDR tuning value; decide after merge |
| `SOTC_HEAP_GUARD_RUNS` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:101 | exposed in the launcher memory group |
| `SOTC_HLE_SHADOW_CULL` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:1761 | exposed in the launcher bind group (shadow cull on the CPU) |
| `SOTC_HLE_SKIP` | choice (unset) | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:51 | exposed in the launcher memory group as a choice |
| `SOTC_HOST_IMAGE_COPY` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:678 | exposed in the launcher upload group |
| `SOTC_HOST_IMPORT` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:817 | exposed in the launcher upload group |
| `SOTC_HOST_IMPORT_MB` | value (512) | launcher-ui | gpu/shadps4/video_core/buffer_cache/host_import.cpp:39 | launcher value row (imported memory limit) |
| `SOTC_HOST_IMPORT_MIN_KB` | value (256) | launcher-ui | gpu/shadps4/video_core/buffer_cache/host_import.cpp:40 | launcher value row (minimum import size) |
| `SOTC_HOT_PAGES` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:112 | exposed in the launcher memory group |
| `SOTC_HOT_PAGES_ADAPT` | on | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:154 | launcher flag_on row (automatic hot page limit) |
| `SOTC_HOT_PAGES_MAX` | value (1024) | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:139 | launcher value row |
| `SOTC_HOT_PAGES_MAX_CEIL` | value (16384) | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:146 | launcher value row |
| `SOTC_HOT_PAGES_MS` | value (2000) | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:132 | launcher value row |
| `SOTC_HOT_PAGES_WINDOW_MS` | value (50) | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:124 | launcher value row |
| `SOTC_HRR` | on | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:1077 | launcher choice row (host-resident ranges, readbacks) |
| `SOTC_HRR_MB` | value (16) | launcher-ui | gpu/shadps4/video_core/buffer_cache/hrr.h:262 | launcher value row |
| `SOTC_HRR_MIN_RB` | value (30) | launcher-ui | gpu/shadps4/video_core/buffer_cache/hrr.h:258 | launcher value row |
| `SOTC_IGPU_BUDGET_MB` | n.a. | none | n.a. | upstream-only integrated-GPU VRAM budget (VRAM profile option); decide after merge |
| `SOTC_IMAGE_POOL_MB` | value (0) | launcher-ui | gpu/shadps4/video_core/texture_cache/image.cpp:149 | launcher value row (texture memory pool) |
| `SOTC_IMAGE_PREFETCH_MAX` | value (128) | launcher-ui | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:42 | launcher value row (images created ahead) |
| `SOTC_INLINE_FUNC` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:471 | exposed in the launcher threads group |
| `SOTC_INVAL_INDEX` | off | launcher-ui | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:659 | exposed in the launcher bind group (texture invalidation by page) |
| `SOTC_LOD_LINGER` | value | launcher | gpu/shadps4/video_core/sotc_lod_stats.h:146 | launcher_win.py sets it (texture linger, player option) |
| `SOTC_LOD_POOL_LOW` | value | launcher | gpu/shadps4/video_core/sotc_lod_stats.h:172 | texture level choice in launcher_win.py (Maximum turns the page pool off) |
| `SOTC_LOW_LATENCY` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:753 | exposed in the launcher present group (Reflex / Anti-Lag) |
| `SOTC_MAX_IN_PRO` | off | launcher | gpu/shim/bbport_overlay.cpp:660 | launcher_win.py sets it for the PS4 Pro mode choice |
| `SOTC_MONITOR_AT` | n.a. | none | n.a. | upstream-only window monitor placement; decide after merge |
| `SOTC_MONITOR_SIZE` | value | none | gpu/shim/window.cpp:50 | window size option, read but not exposed in the launcher |
| `SOTC_MONITOR_SLOT` | value | none | gpu/shim/window.cpp:145 | window monitor slot option, read but not exposed in the launcher |
| `SOTC_MOUSE` | off | launcher,modes,tools | gpu/shim/bbport_settings.cpp:124 | controls: mouse on or off, set by the launcher |
| `SOTC_MOUSE_INVERT` | off | launcher | src/runtime_pad.c:328 | controls option set by the launcher |
| `SOTC_MOUSE_SENS` | value | launcher | src/runtime_pad.c:325 | controls option set by the launcher |
| `SOTC_MUTEX_SPIN_US` | value (20) | launcher-ui | src/runtime_mutex.c:119 | exposed in the launcher threads group |
| `SOTC_NEO` | off | launcher-ui | gpu/shadps4/video_core/texture_cache/sampler.cpp:28 | PS4 Pro mode option (launcher_i18n "Modo PS4 Pro"); set by the launcher option through a name not matched literally |
| `SOTC_OUTPUT_RES` | value | launcher,modes | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:625 | output resolution set from the launcher option and sotc.py |
| `SOTC_PARALLEL_RECORD` | off | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:1769 | launcher Display and FPS flag for parallel record |
| `SOTC_PARALLEL_RECORD_WORKERS` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:609 | worker count of the parallel record option (launcher value) |
| `SOTC_PASS_SEC` | off | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:2964 | launcher Display and FPS flag for secondary command buffers |
| `SOTC_PASS_SEC_MIN` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:80 | draw-count threshold of the PASS_SEC launcher option |
| `SOTC_PROTECT_BATCH` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:472 | launcher Display and FPS flag for protect batching |
| `SOTC_PROTECT_COALESCE` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:572 | launcher Display and FPS flag for coalesced unprotects |
| `SOTC_PROTECT_GAP` | value | launcher-ui | gpu/shadps4/video_core/page_manager.cpp:439 | launcher value for the page bridging gap (default 8) |
| `SOTC_PR_RESOLVE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4345 | launcher Display and FPS flag (resource inputs of GpuPrep draws) |
| `SOTC_RB_AHEAD` | off | launcher | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5400 | set by the launcher from the gpu_submit option (NVIDIA default, launcher line 672) |
| `SOTC_RB_ASYNC_AHEAD` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:2018 | launcher Display and FPS flag for readbacks ahead |
| `SOTC_RB_BATCH` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:11267 | launcher Display and FPS flag for batched readbacks |
| `SOTC_RB_EXACT` | value | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:1313 | launcher choice of the readback publish mode (sotc_stats counters) |
| `SOTC_RB_GROUP` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:251 | launcher Display and FPS flag for group downloads |
| `SOTC_RB_GROW` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:158 | launcher Display and FPS flag for growing readback windows |
| `SOTC_RB_GROW_MAX_KB` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:169 | launcher value for the maximum grown window |
| `SOTC_RB_GROW_US` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:164 | launcher value for the grow time limit (default 1000 us) |
| `SOTC_RB_KEEP_LATER` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:2272 | launcher Display and FPS flag for keeping later writes |
| `SOTC_RB_MATERIALIZE` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:213 | launcher Display and FPS flag for prefetch materialize |
| `SOTC_RB_MATERIALIZE_US` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:225 | launcher value for the materialize look-ahead (default 100 us) |
| `SOTC_RB_OWNER` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:187 | launcher flag; required by the prefetch switches |
| `SOTC_RB_PREFETCH` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:177 | launcher Display and FPS flag for readback prefetch |
| `SOTC_RB_PREFETCH_MAX` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:202 | launcher value for prefetches in flight (default 32) |
| `SOTC_RB_PREFETCH_MB` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:207 | launcher value for prefetch staging memory (default 4 MB) |
| `SOTC_RB_PREFETCH_TICKS` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:197 | launcher value for prefetch lifetime (default 8 ticks) |
| `SOTC_RB_PREV` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:179 | launcher flag for the recurring readback window |
| `SOTC_RB_PREV_FRAMES` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:318 | launcher value for the served guest version age (default 2) |
| `SOTC_RB_SPIN_US` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:49 | launcher value for the readback wait spin (default 150 us) |
| `SOTC_RB_TICK` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:2032 | launcher Display and FPS flag for readback copy ticks |
| `SOTC_RB_WAIT_US` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3255 | launcher value for the readback submit wait; tools/matrices/example.json uses it |
| `SOTC_REBAR_MB` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer.cpp:91 | launcher value for ReBAR bytes (default 512 MB) |
| `SOTC_REBAR_UPLOAD` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:1590 | launcher Display and FPS flag for ReBAR uploads |
| `SOTC_REFLEX` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:754 | low-latency mode option (alias of SOTC_LOW_LATENCY); launcher flag |
| `SOTC_REG_SAME` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:453 | launcher Display and FPS flag for same-register handling |
| `SOTC_RENDER_RES` | value | launcher,modes | gpu/shadps4/video_core/sotc_arch/table_memo.cpp:133 | render resolution option set by the launcher and sotc.py |
| `SOTC_RS_MEMO` | value | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:389 | launcher choice for the render-state memo |
| `SOTC_RUMBLE` | off | launcher | src/runtime_pad.c:648 | controller rumble option; launcher sets 0 (launcher line 745) |
| `SOTC_SHADOW_REUSE` | off | launcher-ui,tools | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1315 | launcher setting shadow_reuse sends it (launcher line 679); far sun shadows on alternate frames, default off (reports2/14) |
| `SOTC_STAGE_BINDINGS` | off | launcher-ui | gpu/shadps4/shader_recompiler/info.cpp:101 | tuning flag (bind group): one descriptor set per stage; off by default |
| `SOTC_STAGING_LARGE_KEEP_MB` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:47 | tuning value row (upload group), idle large upload memory, default 256 MB |
| `SOTC_SYNC_FAST` | off | launcher-ui,sotc.ini | gpu/shadps4/video_core/buffer_cache/region_manager.h:43 | tuning flag (memory group); sotc.ini explicitly sets 0; LOCKED-30-PLAN-2026-11-08.md:38 lists it as a candidate; unsure |
| `SOTC_SYNC_PIPE` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:489 | tuning choice (memory group): off, on, or verify; default off |
| `SOTC_SYNC_PRECHECK` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8038 | tuning flag (memory group); default off; unsure whether it still earns its row |
| `SOTC_SYS_MODULES` | value | launcher,sotc.ini | launcher/sotc_launcher_win.py:614 | folder with the user's own libSceNgs2 for sound (CLAUDE.md); read by sotc.py:142 |
| `SOTC_TEX_REPLACE` | value | launcher-ui,tools | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2792 | launcher sets the xbox glyph folder when the button icons setting is xbox (launcher line 703); tools/make_button_icons.py also uses it |
| `SOTC_UI_LANG` | value | launcher-ui | gpu/shadps4/video_core/sotc_ui_lang.h:13 | language of the overlay and game texts, set from the launcher ui_language setting (launcher line 704) |
| `SOTC_UNBIND_GPU_WAIT` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10168 | tuning flag (memory group); CHANGELOG.md:564 experimental, off by default; unsure |
| `SOTC_UNTRACK_MAX_MB` (removed with SOTC_UNTRACK_AUTO) | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:33 | cap of the untrack list, default 16 MB, live; tuning value row (memory group) |
| `SOTC_UNTRACK_RANGES` | value | launcher-ui | gpu/shadps4/video_core/buffer_cache/region_manager.h:361 | list of unprotected ranges; tuning value row (memory group); empty by default |
| `SOTC_UPDATE_DIR` | value | none | sotc.py:153 | game update folder (default <game>-UPDATE next to sotc.py); read from the environment by sotc.py, the launcher does not set it |
| `SOTC_UPSCALER` | value | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:905 | upscaler choice (dlss and other options) set by the launcher (launcher lines 717 and 926) |
| `SOTC_VIEW_CHUNK_KB` | value | launcher-ui | src/runtime_memory.c:654 | tuning value row (memory group), 0 off; CHANGELOG.md:724 experimental; unsure |
| `SOTC_VIEW_SPLIT_FINE` | off | launcher-ui | src/runtime_memory.c:695 | tuning flag (memory group); CHANGELOG.md:670 experimental, off by default; unsure |
| `SOTC_VOLUME` | value | launcher-ui | launcher/sotc_launcher_win.py:676 | game volume (100 to 300 percent) from the launcher audio setting (ROADMAP 0.21) |
| `SOTC_VRAM_GC_PERCENT` | value | launcher-ui | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:322 | VRAM profile chosen in the launcher (launcher line 675); code default 60 |
| `SOTC_WAIT_NOSPIN` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:443 | tuning flag (threads group): wait without spinning the CPU; docs/re-stutters/native/arch-next/command-path.md:127 |
| `SOTC_WD_FUSE` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:470 | tuning flag (threads group): command processor fused with the draw thread; CHANGELOG.md:532 says off, experimental |
| `SOTC_WORKER_LOW_PRIO` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2843 | tuning flag (threads group): pipeline compile workers at low priority |
| `SOTC_WRITE_AHEAD_ADAPT` | off | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:963 | tuning flag (memory group); CHANGELOG.md:518 experimental, off by default |
| `SOTC_WRITE_AHEAD_ADAPT_US` | value | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:967 | tuning value for SOTC_WRITE_AHEAD_ADAPT, default 1000 us |
| `SOTC_WRITE_AHEAD_MAX_PAGES` | value | launcher-ui | gpu/shadps4/video_core/sotc_stats.h:972 | tuning value for SOTC_WRITE_AHEAD_ADAPT, default 512 pages |
| `SOTC_WRITE_DIET` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:2284 | tuning choice (readback group): readbacks only for written ranges; off by default |
| `SOTC_WRITE_EXTENT` | off | launcher-ui | gpu/shadps4/video_core/sotc_write_extent.h:51 | tuning flag (memory group); CHANGELOG.md:540 experimental, off by default |
| `SOTC_WRITE_EXTENT_FILE` | value | launcher-ui | gpu/shadps4/video_core/sotc_write_extent.h:76 | GPU write rules file for SOTC_WRITE_EXTENT; tuning value row (memory group), empty by default |
| `SOTC_WRITE_FAULT_NARROW` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:281 | tuning flag (memory group); CHANGELOG.md:412 and 427 say off by default, experimental |
| `SOTC_XFER_QUEUE` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:814 | tuning flag (upload group): transfer queue for uploads; off by default |

## Keep: on in normal play

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_CONFIG` | value | launcher | gpu/shim/bbport_settings.cpp:16 | Always pointed at sotc.ini by sotc.py:668 and launcher:746 for normal play. |
| `BB_COPY_THREADS` | value | none | gpu/shim/bbport_copy.cpp:55 | Copy thread count for large guest copies, active in play (runtime_memory.c:1982); no setter. |
| `BB_DMEM_MB` | value | sotc.py | src/runtime_memory.c:37 | sotc.py:596 and :602 set the 9152 MiB pool default for every launch. |
| `BB_GPU_USER_DIR` | value | sotc.py | gpu/shadps4/common/path_util.cpp:89 | sotc.py:640 always sets the GPU cache folder; caches depend on it. |
| `BB_PIPELINE_CACHE` | on | none | gpu/shim/core/emulator_settings.h:81 | Default true and nothing turns it off; read by vk_pipeline_serialization.cpp. |
| `BB_VBLANK_HZ` | value | launcher,sotc.py,tools | gpu/shim/core/emulator_settings.h:44 | Launcher always sets 120 (launcher:631) and sotc.py:606 defaults it to 120. |
| `SOTC_AMD64_LDS` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:784 | on for 64-wide AMD (vk_pipeline_cache.cpp:784); CHANGELOG 954 documents =0 as the off path |
| `SOTC_ARENA_EVICT_IDLE_S` | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10760 | default of the arena eviction that runs in play (buffer_cache.cpp:10741) |
| `SOTC_ARENA_EVICT_STEP_MB` | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10756 | default step of the arena eviction run in play (buffer_cache.cpp:10742) |
| `SOTC_ARENA_GROUP` | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7698 | default arena group of 4 pages in play (buffer_cache.cpp:7693) |
| `SOTC_ASYNC_PRELOAD` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1522 | default on (vk_pipeline_cache.cpp:1516); docs use =0 as the off path |
| `SOTC_ASYNC_UPLOAD` | off | launcher-ui,sotc.ini | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:46 | sotc.ini sets 1 (confirmed play set); the launcher also exposes it |
| `SOTC_AUTO_PREPARE` | off | modes | sotc.py:48 | shader preparation before a play launch (sotc.py:259 and :669); the launcher --prepare run forces it |
| `SOTC_BACK_WRITE_EXACT` | off | sotc.ini | gpu/sotc/nonblocking.h:315 | sotc.ini sets 1 (CP cuts block of the play set) |
| `SOTC_BARYCENTRIC_GS` | on | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:808 | default on (vk_pipeline_cache.cpp:808); docs use =0 as the off path |
| `SOTC_BATCH_SPLIT` | on | none | src/runtime_memory.c:1836 | default on (runtime_memory.c:1836); docs use =0 as the off path |
| `SOTC_BIND_CACHE` | off | launcher-ui,sotc.ini,tools | gpu/shadps4/video_core/sotc_stats.h:89 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_BIND_CACHE_CONTENT` | off | sotc.ini | gpu/shadps4/video_core/sotc_stats.h:343 | sotc.ini sets 1 (confirmed play set), needed with BIND_CACHE_FRAME |
| `SOTC_BIND_CACHE_FRAME` | off | launcher-ui,sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:1611 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_BIND_CACHE_META` | on | sotc.ini | gpu/shadps4/video_core/sotc_stats.h:143 | default 1 (sotc_stats.h:138) and sotc.ini sets 1 |
| `SOTC_BIND_CACHE_RANGES` | off | launcher-ui,sotc.ini | gpu/shadps4/video_core/sotc_stats.h:132 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_BIND_CACHE_XTICK` | off | launcher-ui,sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:1460 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_BIND_DESC_REUSE` | off | sotc.ini | gpu/sotc/nonblocking.h:351 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_BLOCK_EXTENT` | off | sotc.ini | gpu/sotc/probe_alloc.cpp:571 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_CACHE_PACK` | on | none | gpu/shadps4/video_core/cache_storage.cpp:198 | default on (cache_storage.cpp:198); docs use =0 as the off path |
| `SOTC_CLAMP_FAST` | off | sotc.ini | src/runtime_memory.c:2293 | sotc.ini sets 1 (confirmed play set) |
| `SOTC_COMPILE_THREADS` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:103 | default is the physical core count (vk_pipeline_cache.cpp:97); 0 is the former behaviour |
| `SOTC_COMPUTE_SPECFREE` | off | sotc.ini,modes | gpu/shadps4/shader_recompiler/info.h:245 | sotc.ini sets 1 (9 Nov, compute without specialization); sotc.py adds SOTC_RES_LOADED for it |
| `SOTC_COPY_GUEST_VIEW` | on | launcher-ui | gpu/shim/bbgpu.cpp:189 | default on since 30 Oct (bbgpu.cpp:170); OFFLINE-TESTS.md uses =0 |
| `SOTC_CP_LABEL_SLEEP` | off | sotc.ini | gpu/sotc/nonblocking.h:324 | sotc.ini sets 1 (CP cuts block of the play set) |
| `SOTC_CP_LABEL_SLEEP_MS` | value | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:491 | tuning value of SOTC_CP_LABEL_SLEEP (liverpool.h:1383), which is on in play |
| `SOTC_CP_LABEL_SLEEP_ROUNDS` | value | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:488 | tuning value of SOTC_CP_LABEL_SLEEP (liverpool.h:1382), which is on in play |
| `SOTC_CP_THREADS` | on | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:438 | on by default (liverpool.h:308); docs use =0 for the identity check |
| `SOTC_CVARS` | n.a. | launcher,sotc.ini,modes,tools | sotc.py:26 | engine cvars string; sotc.ini sets the play cvars and the launcher builds it from its options |
| `SOTC_DRAW_COALESCE` | off | sotc.ini | gpu/shadps4/video_core/sotc_draw_runs.h:35 | Off unless =1 in code; sotc.ini sets =1 (confirmed play set); command-path.md:342 says keep it. |
| `SOTC_DRIVER_CACHE` | on | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:313 | On by default in code; sotc.ini sets =1. |
| `SOTC_DRIVER_CACHE_MAX_MB` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:329 | Size cap of the driver cache (default 512 MB); the driver-cache memory note depends on it; unsure. |
| `SOTC_DROP_FAST` | off | sotc.ini,launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:849 | sotc.ini sets =1 for play and a launcher row exists (sotc_tuning.py:303); code is on only with =1 (liverpool.h:402); the audit wants it off by default, unsure. |
| `SOTC_DROP_NATIVE` | off | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:859 | On only with =1 in code; sotc.ini sets =1 (confirmed play set). |
| `SOTC_FLIP_COALESCE` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:85 | Off unless =1 in code; sotc.ini sets =1 (confirmed play set). |
| `SOTC_FLIP_IMAGES` | value | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:96 | Flip image count; sotc.ini sets 5 (confirmed play set). |
| `SOTC_FLIP_THREAD` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:80 | Off unless =1 in code; sotc.ini sets =1 (confirmed play set). |
| `SOTC_FPS` | off | sotc.ini,launcher | gpu/shadps4/video_core/sotc_stats.h:32 | Reports and counters; sotc.ini sets 1 (confirmed play set) and the launcher sets it too. |
| `SOTC_FPS_LEAN` | off | sotc.ini,launcher-ui | gpu/shadps4/video_core/sotc_stats.h:48 | Off unless =1; sotc.ini sets 1 (confirmed play set); also a tuning row (sotc_tuning.py:1596). |
| `SOTC_FUTEX_WAIT` | off | sotc.ini | gpu/shadps4/video_core/amdgpu/liverpool.cpp:457 | Off unless =1 (sotc_switch); sotc.ini sets =1 (confirmed play set). |
| `SOTC_GAME_VERSION` | n.a. | launcher | gpu/shim/window.cpp:188 | launcher sets it from the window title (sotc_launcher_win.py:607) for every play window |
| `SOTC_GENERIC_BUFFERS` | off | sotc.ini | gpu/shadps4/shader_recompiler/info.h:104 | sotc.ini sets 2 for play (raw generic buffers) |
| `SOTC_GPL_PREDICT` | on | sotc.ini | gpu/sotc/nonblocking.h:118 | default on in code, but sotc.ini sets 0 for play (SOTC_GPL_PREDICT=0) |
| `SOTC_GPU_PLACE` | off | sotc.ini | gpu/sotc/gpu_sites.cpp:168 | sotc.ini sets 1 (draw combo of the confirmed smooth config) |
| `SOTC_HOOKS_DISABLE` | n.a. | sotc.ini | gpu/sotc/hooks.cpp:693 | sotc.ini sets native_spins for play |
| `SOTC_HOOK_ALLOC_PROBE` | off | sotc.ini,modes,tools | gpu/sotc/probe_alloc.cpp:3 | sotc.ini sets 1 for play; switch-audit DELETE-AT-NATIVE, but the play set still uses it |
| `SOTC_HOOK_FRAME_FLIP` | off | sotc.ini,modes,tools | n.a. | sotc.ini sets 1 for play; switch-audit DELETE-AT-NATIVE, but the play set still uses it |
| `SOTC_HOOK_MEMTYPE_PROBE` | off | sotc.ini,modes,tools | gpu/sotc/probes.h:69 | sotc.ini sets 1 for play |
| `SOTC_IMAGE_SRGB_NORMALIZE` | off | sotc.ini | gpu/shadps4/shader_recompiler/specialization.h:130 | sotc.ini sets 1 for play |
| `SOTC_LAUNCHER_SETTINGS` | n.a. | launcher,modes,tools | gpu/shim/bbport_overlay.cpp:299 | launcher passes the settings file path to every run |
| `SOTC_LAYOUT_CACHE` | off | sotc.ini,launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:45 | sotc.ini sets 1 (confirmed smooth config) and the launcher exposes it |
| `SOTC_NATIVE` | off | sotc.ini | gpu/bbgpu.h:82 | sotc.ini sets 1 for play; native-terrain-fix memory keeps the native path; code default unsure |
| `SOTC_NATIVE_COPIES_FILTER` | off | sotc.ini | gpu/sotc/native/native_copies.cpp:136 | sotc.ini sets 1 for play (copy-entry filter of the native copies index) |
| `SOTC_NATIVE_COPIES_PAGES` | off | sotc.ini | gpu/sotc/native/native_copies.cpp:285 | sotc.ini sets 1 for play (page index of the native copies) |
| `SOTC_NATIVE_DRAIN_AHEAD` | off | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8977 | sotc.ini sets 1 for play; smooth-config-2026-11-09 memory names it |
| `SOTC_NATIVE_INVAL_MERGE` | off | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8970 | sotc.ini sets 1 for play (grouped image invalidation) |
| `SOTC_NATIVE_MASKS` | value | sotc.ini | gpu/sotc/cmd_counters.cpp:16 | sotc.ini sets prev for play (mask served as zero by key) |
| `SOTC_NATIVE_PARTICLES` | value | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:2377 | sotc.ini sets prev for play (native particles) |
| `SOTC_NATIVE_TRACK` | off | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1426 | sotc.ini sets 3 for play; native-terrain-fix memory keeps TRACK=3 |
| `SOTC_NATIVE_TRACK_GPU_ONLY` | off | sotc.ini | gpu/shadps4/video_core/page_manager.cpp:604 | sotc.ini sets 1 for play; native-terrain-fix memory keeps GPU_ONLY |
| `SOTC_NB_IO` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2992 | sotc.ini sets 1 for play (lazy pack load) |
| `SOTC_NB_LAZY_PSO` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:4651 | sotc.ini sets 1 for play (recently used pipelines list) |
| `SOTC_NB_WORKER_CAP` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2711 | sotc.ini sets 1 for play (capped lanes) |
| `SOTC_PARTICLE_PREV` | value | modes | gpu/sotc/particle_prev.h:3 | sotc.py sets 3; PC-native particle serving is the only path left (native_particles.h) |
| `SOTC_PC_RING` | value | sotc.ini | gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:550 | sotc.ini sets 8 for play (the PC ring path) |
| `SOTC_PIPE_MANIFEST` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2508 | shipped pipeline manifest used by the first-launch path (HANDOFF-2026-11-07.md, commit 1d56306); default unsure |
| `SOTC_PIPE_MANIFEST_DIR` | value | modes | gpu/sotc/pipe_manifest.cpp:139 | sotc.py points it at the shipped manifest folder; package_sotc.sh copies the manifest |
| `SOTC_PLAY_QUIET` | on | none | gpu/sotc/nonblocking.h:295 | on by default (SwitchOnByDefault) for play; quiets the pipeline feedback report |
| `SOTC_POOL_RESERVE` | value | modes | scripts/sotc_modes.py:307 | sotc_modes.py computes the reserve for play; the env var only forces it on or off; unsure |
| `SOTC_POOL_WATCH` | on | launcher | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3551 | launcher sets 1 in one mode (launcher line 741); watch of freed-pool T#s; unsure |
| `SOTC_PRECOMPILE` | off | modes | gpu/sotc/pretranslate.cpp:69 | sotc.py sets 1 for the precompile run (PRECOMPILE_DEFAULTS); pipeline prep step |
| `SOTC_PRELOAD_UPLOAD` | value | sotc.ini | gpu/sotc/preupload.cpp:136 | sotc.ini sets 2 for play (idle preload uploader) |
| `SOTC_PRELOAD_UPLOAD_AGE_MS` | value | none | gpu/sotc/preupload.cpp:51 | age a range needs before the play uploader takes it (default 100 ms) |
| `SOTC_PRELOAD_UPLOAD_MB` | value | none | gpu/sotc/preupload.cpp:92 | MB per frame of the play preload uploader |
| `SOTC_PRESENT_WAIT` | on | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:3371 | on by default (flag_on in the launcher); locks flips to the display |
| `SOTC_PRETRANSLATE` | off | sotc.ini | gpu/sotc/pretranslate.cpp:77 | sotc.ini sets 1 for play (pretranslate pack) |
| `SOTC_PRETRANSLATE_COMPUTE` | value | modes | gpu/sotc/pretranslate.cpp:85 | sotc.py sets it for the compute prebuild |
| `SOTC_PROTECT_ASYNC` | on | sotc.ini | gpu/shadps4/video_core/page_manager.cpp:277 | sotc.ini sets 1 for play; smooth-config-2026-11-09 memory names it |
| `SOTC_PS_INPUT_NORMALIZE` | on | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:6173 | on unless =0; sotc.ini sets 1 |
| `SOTC_RB_HOT` | value | launcher | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5515 | launcher sets 2 (launcher line 735) in one mode; applies readback windows; unsure |
| `SOTC_READ_EXTENT` | value | sotc.ini | gpu/shadps4/video_core/sotc_read_extent.h:31 | sotc.ini sets 1 for play (read extent) |
| `SOTC_READ_EXTENT_FILE` | value | none | gpu/shadps4/video_core/sotc_read_extent.h:47 | rule file of the read extent that is on in play (user/read_extent.txt) |
| `SOTC_RECORD_THREAD` | on | launcher | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:598 | launcher sets it from the perf_record_thread option (default on) |
| `SOTC_RES_LOADED` | off | modes | gpu/sotc/pretranslate.h:34 | prerequisite of the pretranslate switches; sotc.py sets it |
| `SOTC_SAMPLER_CACHE` | off | sotc.ini | gpu/sotc/nonblocking.h:359 | sotc.ini sets 1 for normal play (plan B switch, SwitchOn is off unless set) |
| `SOTC_SHADER_EXTENT` | off | sotc.ini | gpu/shadps4/video_core/sotc_shader_extent.h:21 | sotc.ini sets 1 for normal play (confirmed set) |
| `SOTC_SRT_PHI` | on | launcher-ui | gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp:861 | on unless =0 in code; tuning row flag_on (bind group) lets the player uncheck it |
| `SOTC_STAGING_BLOCK_MB` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:30 | upstream-only: staging ring block size at the upstream default 16 MB; no setter; kept as upstream behaviour |
| `SOTC_STAGING_KEEP_MB` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:204 | upstream-only: keep size at the upstream default 128 MB; no setter |
| `SOTC_STAGING_KEEP_VRAM_MB` | value | launcher | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:209 | upstream-only: upstream launcher setdefault 0 for normal play (upstream launcher line 853); fork lacks it, so the merge brings it in |
| `SOTC_STAGING_LARGE_POW2` | on | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:40 | on unless =0 in code; tuning row flag_on (upload group); migrated to on on 30 Oct (tune_migrated_20261030) |
| `SOTC_STALE_MS` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/page_manager.cpp:1006 | upstream-only: readback staleness window at the upstream default 100 ms; no setter |
| `SOTC_SYNC_PIPELINE_MS` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1460 | sync-pipeline threshold, 30 ms default in code; behaviour value, no setter |
| `SOTC_TAA_OFF` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1469 | upstream-only: unset means TAA follows DLSS (behaviour); =0 or =1 forces |
| `SOTC_TEX_BLOCK` | off | sotc.ini,launcher-ui,tools | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10175 | sotc.ini sets 1 (confirmed play set); tuning choice (bind group), off by default; tools/pcmem_matrix.py runs it |
| `SOTC_TEX_DIRECT` | off | launcher | absent@18a1251; upstream/main:gpu/shadps4/video_core/sotc_stats.h:187 | upstream-only: upstream launcher setdefault 1 for normal play (upstream launcher line 904); the fork has no code for it, so the merge decision matters |
| `SOTC_TIMER_RES` | on | launcher-ui | src/probe.c:1908 | on unless =0; 1 ms Windows timer; tuning row flag_on (threads group) lets the player uncheck it |
| `SOTC_TSC` | value | none | src/probe.c:1516 | clock source: auto (QPC only when cores disagree), qpc or native; documented in docs/sotc/README-SotC.md:175; no setter |
| `SOTC_TSHARP_ARRAY` | value | none | gpu/shadps4/shader_recompiler/info.cpp:399 | dynamic T# array size: 16 for SotC in code when unset; behaviour default, value override only |
| `SOTC_UI_LINEAR` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3076 | upstream-only: override for the UI linear rule; unset follows the resolution rule (behaviour), =0 or 1 forces |
| `SOTC_UNBIND_DEFER` | off | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:10564 | sotc.ini sets 1 for normal play |
| `SOTC_UPLOAD_BUDGET_MB` | value | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2621 | per-frame upload budget, default 32 MB in code (docs/re-stutters/reports2/03-streaming-bursts.md:97); no setter |
| `SOTC_UPLOAD_EXPIRED_MB` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1220 | upstream-only: cap on expired uploads, default 128 MB (0 = no cap); no setter |
| `SOTC_USER_DIR` | value | launcher | sotc.py:224 | user folder for saves and caches; launcher sets it for normal play (launcher line 612); sotc.py defaults to ./user |
| `SOTC_VIS_PROGRAMS` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/sotc_stats.h:165 | upstream-only: a non-empty default list of four program hashes is active; behaviour unsure, kept |
| `SOTC_VS_BINDING_BASE` | off | sotc.ini | gpu/shadps4/shader_recompiler/backend/bindings.h:25 | sotc.ini sets 1 for normal play |
| `SOTC_VS_BUFFER_BASE` | value | none | gpu/shadps4/shader_recompiler/backend/bindings.h:43 | vertex buffer binding base, default kVsBufferBase in code; layout value, no setter |
| `SOTC_VS_DRAW_KIND` | off | sotc.ini | gpu/shadps4/shader_recompiler/info.h:145 | sotc.ini sets 1 for normal play |
| `SOTC_VS_UNIFIED_BASE` | value | none | gpu/shadps4/shader_recompiler/backend/bindings.h:33 | unified binding base, default 48 in code; layout value, no setter |
| `SOTC_WAIT_SPIN_US` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp:23 | spin length, 4000 us in code when unset (vk_semaphore.cpp:54); no setter |
| `SOTC_WATCHDOG` | on | none | src/probe.c:2253 | hang watchdog thread, on unless =0; a safety net, so kept although nothing sets it |
| `SOTC_WAVE64_IDENTITY` | on | none | gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp:241 | on unless =0 in code; no setter, no doc; unsure whether the off path is still needed |
| `SOTC_WAVE64_UNIFORM` | value | none | gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp:204 | unset means auto; no setter, no doc; unsure |
| `SOTC_WIDE_ASPECT` | value | modes | scripts/sotc_modes.py:220 | modes sets it from the window shape (on for wide screens, scripts/sotc_modes.py:202); normal play |
| `SOTC_WINDOW_ICON` | value | launcher | gpu/shim/window.cpp:182 | launcher sets the icon path for normal play (launcher line 610) |
| `SOTC_WQM_COPY` | on | none | gpu/shadps4/shader_recompiler/frontend/translate/scalar_alu.cpp:114 | on unless =0 in code; no setter, no doc; unsure |
| `SOTC_WRITE_AHEAD_PAGES` | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:941 | write-ahead window, default 15 pages in code; no setter |

## Keep for now: work in progress

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_COPY_GPU_BUFFERS` | off | none | gpu/shim/core/emulator_settings.h:58 | Unsure: off by default with no setter; reports2/01 and reports3/10 still describe it as the copy mode. |
| `BB_HDR` | off | none | gpu/shim/core/emulator_settings.h:76 | Unsure: off with no setter, launcher row or doc; only gates HDR surface support in vk_swapchain.cpp:189. |
| `SOTC_AB_PR` | off | none | gpu/shadps4/video_core/sotc_stats.h:1831 | off by default, no setter; DRAW-THREAD-7C-DESIGN documents it with no measured result (unsure) |
| `SOTC_ADDR_FALLBACK` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:896 | report of the ADDR table mode, prints only in table mode (driver.cpp:2505); ADDR work is in progress |
| `SOTC_ADDR_TABLE` | off | sotc.ini | gpu/shadps4/video_core/sotc_arch/addr_table.cpp:298 | ADDR_TABLE is on the WIP list; sotc.ini sets it to 0 for play |
| `SOTC_ADDR_VERTEX` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:66 | vertex pulling of the ADDR table work (vk_pipeline_serialization.cpp:66); off in the play set |
| `SOTC_AREA_PREWARM` | off | none | gpu/sotc/area_prewarm.cpp:390 | first-frame-peak route (FIRST-FRAME-PEAK.md:54, HANDOFF.md:169); off by default and not in the play set |
| `SOTC_AREA_PREWARM_MB` | value | none | gpu/sotc/area_prewarm.cpp:399 | replay budget of SOTC_AREA_PREWARM (area_prewarm.cpp:399), follows that route |
| `SOTC_ASYNC_PRESENT` | off | none | - | upstream-only; env override for presenting on a thread, measured with DLSS-G (upstream driver.cpp:296); unsure |
| `SOTC_BARRIER_GUEST` | off | none | gpu/shadps4/video_core/sotc_arch/guest_barrier.cpp:117 | SwitchMode returns 0 when unset (sotc_arch.cpp:12); guest barrier mode of the sotc_arch work, not in the play set (unsure) |
| `SOTC_BARRIER_STATIC` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2444 | SwitchMode returns 0 when unset (passes.cpp:31); static barrier rule of the sotc_arch work (unsure) |
| `SOTC_BDA` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2646 | block-table groundwork, off by default (vk_bda_table.h:3); plan V1 in the docs |
| `SOTC_BDA_DIRECT` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_bda_table.cpp:105 | direct BDA path, default 0 (vk_bda_table.h:51); plan V1 in the docs implements it behind this switch |
| `SOTC_BINDLESS_CACHE_VERIFY` | off | none | gpu/shadps4/shader_recompiler/info.cpp:192 | verify mode of the bindless cache key (info.cpp:188); bindless is on the WIP list |
| `SOTC_BINDLESS_SAMP` | off | launcher-ui,sotc.ini,tools | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:1068 | bindless is on the WIP list; sotc.ini sets 0 and the launcher exposes a flag |
| `SOTC_BINDSET` | off | none | gpu/shadps4/video_core/sotc_arch/table_memo.cpp:382 | SwitchMode returns 0 when unset (table_memo.cpp:382); BindSet reverse-index plan in the docs |
| `SOTC_BIND_CACHE_HEAP` | off | none | gpu/shadps4/video_core/sotc_arch/table_memo.cpp:1436 | heap resolution of the bind cache, part of the HEAP work (sotc.ini sets SOTC_HEAP=0) |
| `SOTC_BIND_CACHE_PACKET` | off | none | gpu/shadps4/video_core/sotc_stats.h:324 | default 0 (sotc_stats.h:308), not in the play set, no doc; unsure whether it belongs to the bind cache series |
| `SOTC_BPE_FIX` | off | none | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:664 | heap guard fix candidate with diagnostics (probe.c:1230, PERF4-BRIEF); unsure |
| `SOTC_COMPILE_STREAM` | off | none | gpu/sotc/nonblocking.h:236 | experimental RAM batch writes, plan item 4.2 in COLD-COMPILE-CPU-RAM doc, default off |
| `SOTC_COMPUTE_FAST` | off | none | gpu/sotc/nonblocking.h:254 | default off (nonblocking.h:247), no doc and no tool; unsure whether SPECFREE supersedes it |
| `SOTC_COMPUTE_PACK` | off | modes | gpu/sotc/nonblocking.h:200 | code default off (nonblocking.h:198) but sotc.py defaults it to 1; COLD-COMPILE doc says superseded by SPECFREE (unsure) |
| `SOTC_COMPUTE_PACK_MAX` | value | none | gpu/sotc/nonblocking.h:262 | cap of the compute pack, follows SOTC_COMPUTE_PACK (nonblocking.h:262) |
| `SOTC_CP_LABEL_SKIP` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:162 | off by default (liverpool.cpp:159); reports2 H1 plans to enable and measure it, never measured (unsure) |
| `SOTC_CP_SRT_PLAN` | off | none | gpu/shadps4/shader_recompiler/info.cpp:48 | walker plan switch, off by default (info.cpp:48, liverpool.cpp:150); SRT plan work not in the play set (unsure); its liverpool.cpp reader went with SOTC_CP_PRECISE_WAIT (removed), info.cpp=2 trace stays |
| `SOTC_DESCRIPTOR_HEAP` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_descriptor_heap.cpp:138 | Phase-2 VK_EXT_descriptor_heap path, off unless 1 (re-stutters/native/DESCRIPTOR-HEAP.md); not in launcher or sotc.ini. |
| `SOTC_DRAW_GATE` | off | none | gpu/sotc/draw_gate.cpp:63 | Draw/CP cut family left switched off by commit a54e374 ("all switches off"); needs SOTC_PRELOAD_UPLOAD=2; not in launcher or sotc.ini. |
| `SOTC_DRAW_GATE_FRAMES` | value | none | gpu/sotc/draw_gate.cpp:32 | Knob of the draw gate (default 3 frames) from the draw/CP cuts work. |
| `SOTC_DRAW_GATE_MB` | value | none | gpu/sotc/draw_gate.cpp:30 | Knob of the draw gate (default 10 MB) from the draw/CP cuts work. |
| `SOTC_DRAW_GATE_MIN_KB` | value | none | gpu/sotc/draw_gate.cpp:31 | Knob of the draw gate (default 256 KB) from the draw/CP cuts work. |
| `SOTC_DRAW_MDI` | off | none | gpu/shadps4/video_core/sotc_draw_merge.h:57 | Opt-in draw merge, not set by launcher or sotc.ini; command-path.md:342 says consider it only with a measured gain. |
| `SOTC_FLAT_NOTES` | off | sotc.ini | gpu/shadps4/video_core/sotc_write_extent.h:58 | Flat native bookkeeping of the native plan (re-stutters/native/PERF3-BRIEF.md W3, PERF4-BRIEF.md:23); sotc.ini sets 0 for now. |
| `SOTC_FLIP_OFFTHREAD` | off | tools | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:71 | Flip wait off the draw thread (F1, re-stutters/HANDOFF.md:152); off by default, set only by tools/pcmem_matrix.py; not measured for play; unsure. |
| `SOTC_FRAME_AHEAD` | off | tools | gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:198 | Command processor one frame ahead, off by default; re-stutters/reports3/draw/wave1/f2.md says "not measured"; sotc.py:547 and pcmem_matrix need it. |
| `SOTC_GCN_BARRIER` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:162 | GCN-NATIVE-PLAN step S5 barriers, behind a switch, off, not measured in play |
| `SOTC_GCN_BUF` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:932 | GCN-NATIVE-PLAN step S2 buffers by address, off by default, not in sotc.ini |
| `SOTC_GCN_BUF_FMT` | off | none | gpu/shadps4/shader_recompiler/info.h:169 | GCN-NATIVE-PLAN S2 formatted buffers, default off (info.h), only the S2 image test sets it |
| `SOTC_GCN_COMMIT` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:2159 | GCN-NATIVE-PLAN step S6 thin draw thread, eligibility only; all stages on crashed the game |
| `SOTC_GCN_MEM` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:931 | GCN-NATIVE-PLAN step S1 page table, built behind a switch, not in play |
| `SOTC_GCN_MISS_EMIT` | off | none | gpu/shadps4/shader_recompiler/info.h:158 | GCN-NATIVE-PLAN S2 image fix needs =1; code default off although the docs say on |
| `SOTC_GCN_SKIP` | off | none | gpu/shadps4/video_core/sotc_arch/gcn_commit.cpp:8 | GCN-NATIVE-PLAN step 4 skip counters behind a switch, not measured |
| `SOTC_GCN_SYNC` | off | none | gpu/shadps4/video_core/sotc_arch/addr_table.cpp:669 | GCN-NATIVE-PLAN S4 coherence batch, built behind a switch |
| `SOTC_GCN_TEX` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_bindless.cpp:129 | GCN-NATIVE-PLAN S3 texture heap-slot table, off by default, the most complete stage |
| `SOTC_GCN_TEX_REGISTER` | off | none | gpu/shadps4/video_core/sotc_arch/gcn_tex.cpp:1114 | GCN-NATIVE-PLAN step 2 registration ahead of draws, off by default, needs SOTC_GCN_TEX |
| `SOTC_GCN_TEX_REGISTER_BUDGET` | value (64) | none | gpu/shadps4/video_core/sotc_arch/gcn_tex.cpp:1122 | tuning value of the step 2 registration budget per submit |
| `SOTC_GCN_TEX_REGISTER_LOG` | off | none | gpu/shadps4/video_core/sotc_arch/gcn_tex.cpp:1119 | one-time log for the step 2 registration, no setter outside the plan |
| `SOTC_GCN_TEX_REGISTER_SOURCES` | value (15) | none | gpu/shadps4/video_core/sotc_arch/gcn_tex.cpp:1116 | source mask of the step 2 registration (gcn_native.h:194) |
| `SOTC_GCN_TEX_SKIP_RESOLVE` | off | none | gpu/shadps4/video_core/sotc_arch/gcn_core.cpp:191 | GCN-NATIVE-PLAN texture resolve skip, off by default |
| `SOTC_GPL_URGENT_WORKERS` | value (2) | none | gpu/sotc/nonblocking.h:278 | unsure: worker count for urgent GPL libraries, no setter, one doc hit |
| `SOTC_GPU_REGION_QUARANTINE_FRAMES` | value (kDefault) | none | gpu/sotc/gpu_region.cpp:311 | unsure: region quarantine frame count, no setter, no docs |
| `SOTC_GPU_REGION_RING_FRAMES` | value (kDefault) | none | gpu/sotc/gpu_region.cpp:306 | unsure: region ring frame count, no setter, no docs |
| `SOTC_HEAP` | off | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:2429 | heap family of the GCN-native plan; sotc.ini sets 0 |
| `SOTC_HEAP_COMPOSITE_OP` | off | sotc.ini | gpu/shadps4/video_core/sotc_arch/heap.cpp:256 | heap family of the GCN-native plan; sotc.ini sets 0 |
| `SOTC_HEAP_EARLY_FLUSH` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_descriptor_heap.cpp:71 | descriptor heap of the GCN-native plan, default off, no setter, no docs |
| `SOTC_HEAP_GPU_VERIFY` | off | none | gpu/shadps4/video_core/sotc_arch/heap.cpp:146 | heap verify mode (=1) of the GCN-native plan |
| `SOTC_HEAP_UI_LINEAR` | off | none | gpu/shadps4/video_core/sotc_arch/table_memo.cpp:151 | heap family (UI linear) of the GCN-native plan, default off, no setter |
| `SOTC_HEAP_VS` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:5583 | heap family for the vertex stage of the GCN-native plan; sotc.ini sets 0 |
| `SOTC_INDEX_EXTENT` | unsure | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2797 | unsure: sibling of the EXTENT switches set in sotc.ini (READ, SHADER, BLOCK), no setter, 9 hits |
| `SOTC_LAYOUT_UNIFIED` | off | sotc.ini | gpu/shadps4/video_core/sotc_arch/heap.cpp:244 | heap/bindless family of the GCN-native plan; sotc.ini sets 0 |
| `SOTC_LEAN_SUBMIT` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8928 | unsure: draw/CP cut family, default off, no setter, 3 doc hits |
| `SOTC_LOAD_STREAM` | off | none | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:57 | unsure: streaming-hitch work (memory notes), off, 7 doc hits, no setter |
| `SOTC_LOAD_STREAM_MB` | value | none | gpu/sotc/load_warm.cpp:59 | unsure: budget of the LOAD_STREAM path, no setter |
| `SOTC_LOAD_WARM` | off | none | gpu/sotc/load_warm.cpp:44 | unsure: off (only =1 enables), 11 doc hits; CHANGELOG measured the TEX variant only |
| `SOTC_MIP_BUDGET` | off | none | gpu/sotc/mip_budget.cpp:59 | unsure: streaming mip budget, off (unset returns 0), 1 doc hit, no setter |
| `SOTC_MIP_TAIL` | value (2) | none | gpu/shadps4/video_core/texture_cache/texture_cache.h:58 | unsure: mip tail mode, default 2, 3 doc hits; switch-audit UNKNOWN-MEASURE |
| `SOTC_N5_ASYNC_COMPUTE` | off | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:2383 | N5 async-compute classification (docs/re-stutters/native/impl-n5-asynccomp.md); sotc.ini sets 0 |
| `SOTC_N5_ASYNC_EVERY` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_frame_graph.cpp:96 | sampling period of the N5 async report (default 8); impl-n5-asynccomp.md |
| `SOTC_N5_BDA` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2647 | N5 buffer device address path; PERF3-BRIEF.md; not in sotc.ini or launcher |
| `SOTC_N5_BDA_BUFFERS` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_bda_table.cpp:56 | N5 BDA descriptor-write mode (1 drop, 2 verify); PERF-BRIEF.md |
| `SOTC_N5_BINDLESS` | off | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:2385 | bindless heap of N5 (impl-n5-bindless docs); sotc.ini sets 0 |
| `SOTC_N5_DESCBUF` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_descriptor_buffer.cpp:137 | N5 descriptor-buffer layout; DESCRIPTOR-HEAP.md; ignored when SOTC_DESC_BUFFER is set |
| `SOTC_N5_GRAPH` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2381 | N5 frame-graph report and observe/verify mode; impl-n5-graph.md |
| `SOTC_N5_PARALLEL` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.cpp:78 | N5 parallel record; impl-n5-parallel.md; not in sotc.ini or launcher |
| `SOTC_N5_PARALLEL_TRANSLATE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.cpp:111 | producer emission census of N5 parallel record; read once; part of the N5 work |
| `SOTC_N5_PSO` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2359 | N5 PSO path; impl-n5-psos2.md; not in sotc.ini or launcher |
| `SOTC_N5_PSO_GRACE_S` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:714 | grace window tuning of the N5 PSO path; impl-n5-psos2.md |
| `SOTC_N5_PSO_WAIT_S` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:3319 | wait window tuning of the N5 PSO prebuild; impl-n5-psos2.md and 15-burst-stutters.md |
| `SOTC_N5_QUEUES` | off | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:2380 | N5 queue routing and report; sotc.ini sets 0 |
| `SOTC_NATIVE_OWNER_DISCARD` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:651 | HEAP-MAP.md heap work; off and not in sotc.ini; unsure whether still needed |
| `SOTC_NATIVE_OWNER_DISCARD_NOW` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9486 | off-by-default sub-switch of OWNER_DISCARD (HEAP-MAP.md); unsure |
| `SOTC_NB_GPL` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp:193 | non-blocking library lock (nonblocking-frame.md); not in sotc.ini; unsure |
| `SOTC_NB_PREFETCH` | off | none | gpu/sotc/nonblocking.h:29 | P1 prefetch of the pretranslate pack; off; command-path.md plan; unsure |
| `SOTC_NB_SCHED` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2348 | worker yielding to game threads; COLD-COMPILE-CPU-RAM and nonblocking-frame docs; unsure |
| `SOTC_NB_WARM` | off | none | gpu/sotc/nonblocking.h:155 | P4 pre-build of shader libraries; off; nonblocking-frame.md plan |
| `SOTC_OP_COMPUTE_BIND` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:1542 | compute-bind mode of the op plan (ARCH-GCN-NATIVE, opcost); unsure |
| `SOTC_OP_OVERLAP` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:637 | plan step 7c.0 footprint overlap (DRAW-THREAD-7C-DESIGN.md); unsure |
| `SOTC_PACER` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:3478 | frame pacer of LOCKED-30-PLAN; smooth-config memory says untested in real play |
| `SOTC_PACER_RATE` | value | none | gpu/shadps4/core/libraries/videoout/driver.cpp:3482 | forced rate of the frame pacer; LOCKED-30-PLAN; follows SOTC_PACER |
| `SOTC_PACKET` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp:225 | packet record of the N5/wave work; ARCH-WAVE-BRIEF.md; unsure |
| `SOTC_PASSES` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2441 | sotc_arch pass report and mode; ARCH-WAVE-BRIEF.md; unsure |
| `SOTC_PASS_RECORD` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.cpp:19 | pass record of the packet path; ARCH-WAVE-BRIEF.md; unsure |
| `SOTC_PASS_WORKERS` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.cpp:221 | pass worker count of the N5 pool; ARCH-PARALLEL-DRAW.md |
| `SOTC_PC_MEMORY` | off | none | gpu/shadps4/video_core/renderer_vulkan/sotc_guest_memory.cpp:446 | PC memory rewrite stage 1 (PC-MEMORY-REWRITE.md); may be superseded by SOTC_PC_RING; unsure |
| `SOTC_PC_PLACE` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2493 | placement glue (reports3/11-placement-by-class.md); not in sotc.ini or launcher; unsure |
| `SOTC_PC_PLACE_WINDOWS` | off | none | gpu/sotc/pc_pool_place_glue.cpp:280 | measured-window input of the placement glue; part of PC_PLACE |
| `SOTC_PC_POOL_NATIVE` | off | none | gpu/sotc/pc_pool.cpp:1206 | native pool path; impl-windows.md and 14-loading-hangs.md; unsure |
| `SOTC_PC_POOL_PATHS` | value | none | gpu/sotc/pc_pool.cpp:1266 | eligible paths of the native pool (default mask); part of PC_POOL_NATIVE |
| `SOTC_PC_POOL_REFILL` | off | none | gpu/sotc/pc_pool.cpp:1256 | opt-in native node refill; impl-windows.md; unsure |
| `SOTC_PC_RECYCLE` | value | none | gpu/sotc/pc_pool_place_glue.cpp:179 | recycle mode of the placement glue; unsure |
| `SOTC_PC_RECYCLE_FRAMES` | value | none | gpu/sotc/pc_pool_place_glue.cpp:188 | quarantine length in frames (default 120, open to measure per its header) |
| `SOTC_PC_RING8_FLIPWAIT` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4559 | flip-wait mode 2 under PC ring 8; HANDOFF.md and PC-MEMORY-REWRITE.md; unsure |
| `SOTC_PC_RING8_GROUP` | off | none | gpu/sotc/pc_ring.cpp:1759 | group imports, off unless =1; wave1/m1.md and switch-audit; unsure |
| `SOTC_PC_RING8_HEAPS` | off | none | gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp:408 | heap granule runs; M1 plan (wave1/m1.md); off by default; unsure |
| `SOTC_PC_RING8_HEAPS_MODE` | value | sotc.ini | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8080 | M1 add-only and whole-heap modes; sotc.ini sets 0; unsure |
| `SOTC_PC_RING_HOT_EPOCHS` | value | none | gpu/sotc/pc_ring.cpp:309 | hot-page window of the ring; documented in pc_ring.h; unsure |
| `SOTC_PIPE_DIRTY` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:48 | dirty-pipeline tracking named as the N5 default in native.h; unsure |
| `SOTC_PIPE_FAST` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:4203 | full pipeline lookup path; PERF2-BRIEF and PERF4-BRIEF docs; default unsure |
| `SOTC_PIPE_GPL` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp:38 | GPL library keying of the first-launch path; default unsure |
| `SOTC_PIPE_PREDICT` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h:982 | pipe predict state (pipe_predict.cpp); sotc.ini sets 0; unsure |
| `SOTC_PRELOAD_UPLOAD_RESC` | off | none | gpu/sotc/preupload.cpp:239 | adds memcpy ranges to the preload uploader; off, no doc; unsure |
| `SOTC_PRETRANSLATE_COMPUTE_HINTS` | off | none | gpu/sotc/pretranslate.cpp:416 | hints switch of the compute prebuild; unsure |
| `SOTC_PROTECT_ASYNC_UNPROTECT` | off | none | gpu/sotc/nonblocking.h:389 | async unprotect sub-switch of PROTECT_ASYNC; not in sotc.ini; unsure |
| `SOTC_PROTECT_SPAN` | off | none | src/runtime_memory.c:2414 | off by default; protects a range crossing view chunks; one doc mention; unsure |
| `SOTC_PR_GEN` | off | tools | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4358 | GpuPrep generation (use or verify); tools/pcmem_matrix.py sweeps it; unsure |
| `SOTC_PR_PRED` | off | none | gpu/sotc/nonblocking.h:461 | prepared-resource prediction mode; unsure |
| `SOTC_RB_STREAM` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1441 | sequential-read window stream for guest reads; also in upstream/main; unsure |
| `SOTC_RB_STREAM_MAX_KB` | value | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1445 | maximum window of RB_STREAM; unsure |
| `SOTC_REGION_DIRECT` | off | none | src/runtime_memory.c:2042 | region direct path, read before any fault; also in upstream/main; unsure |
| `SOTC_REGION_PREDRAIN` | off | none | gpu/sotc/region_predrain.cpp:49 | region predrain uploads; no doc mention; unsure |
| `SOTC_REGION_PREDRAIN_MB` | value | none | gpu/sotc/region_predrain.cpp:58 | MB per frame of region predrain (default 8); unsure |
| `SOTC_RING8_CACHE` | off | none | gpu/sotc/nonblocking.h:332 | ring-8 runtime memory table cache switch; no doc mention; unsure |
| `SOTC_RT_FAST` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:521 | render-target fast test that the RS memo repeats; default unsure |
| `SOTC_SHADER_ADDR` | off | sotc.ini | gpu/shadps4/video_core/sotc_arch/shader_addr.cpp:80 | GCN-native shader-computed buffer addresses (GCN-NATIVE-DECISIONS.md); sotc.ini sets 0 |
| `SOTC_SHADOW_CACHE` | off | none | scripts/hooks.d/shadow_cache.py:21 | sun-shadow cascade cache hooks; off by default and not in sotc.ini; documented in docs/re-backend/12-shadow-cache.md; not adopted, unsure |
| `SOTC_SPEC_MEMO` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:5947 | docs/re-stutters/native/arch-next/command-path.md:460 says SOTC_SPEC_MEMO=1 gave the best route numbers of the round; not in sotc.ini yet, so kept for evaluation |
| `SOTC_SPV_FUNCS` | off | none | gpu/shadps4/shader_recompiler/info.h:181 | HANDOFF-2026-11-07.md:112 measured table-mode compile slower; part of the GCN-native plan (inverted default noted in GCN-NATIVE-PLAN-2026-11-09.md:29) |
| `SOTC_STAGING_CACHED` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:110 | listed as experimental in docs/sotc/README-SotC.md; HANDOFF-2026-11-07.md:144 says maybe better, within noise; not adopted |
| `SOTC_SYNC_EVENTS` | off | none | gpu/shadps4/video_core/buffer_cache/sync_events.h:48 | sync-event mode 0/1/2 of the UMA sync family; no setter, no docs; unsure |
| `SOTC_TC_FAST_LOOKUP` | value | none | gpu/shadps4/video_core/texture_cache/tc_fast.h:22 | value 0 default; no setter, no docs; unsure whether the texture cache still uses it |
| `SOTC_TC_SINGLE_WRITER` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:466 | off unless =1; same switch read in driver.cpp:3108; no setter, no docs; unsure |
| `SOTC_TEX_REPEAT` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10778 | off; no setter, no docs; unsure whether it is an open experiment |
| `SOTC_TINY_ASYNC` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1285 | docs/re-backend/15-burst-stutters.md:210 says default 0 (tiny draws built inline); no measured result; unsure |
| `SOTC_TINY_GPL` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1328 | off unless =1; no setter; one doc mention in switch-audit only; unsure |
| `SOTC_TRACK_IGNORE_RIPS` | off | none | gpu/shadps4/video_core/page_manager.cpp:1928 | presence switch in the write-tracking path; no setter, no docs; unsure |
| `SOTC_UNMAP_PARK` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.h:92 | off until measured (docs/re-stutters/reports2/13-memory-aliasing.md:173); no setter; unsure |
| `SOTC_UNMAP_PARK_MB` | value | none | gpu/shadps4/video_core/texture_cache/texture_cache.h:100 | size value for SOTC_UNMAP_PARK, default 128 MB; follows its fate |
| `SOTC_UNMAP_READBACK` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:12099 | off; no setter; the UMA readback family is being decided in the native plan; unsure |
| `SOTC_USLEEP_YIELD` | off | none | src/runtime_kernel.c:172 | off; frame-pacing candidate in docs/re-stutters/reports2/05-frame-pacing.md:230, not adopted; unsure |
| `SOTC_USLEEP_YIELD_US` | value | none | src/runtime_kernel.c:173 | threshold of SOTC_USLEEP_YIELD, default 10 us; follows it |
| `SOTC_VERTEX_EXTENT` | off | none | gpu/shadps4/video_core/sotc_vertex_extent.h:37 | sibling of SOTC_SHADER_EXTENT (play sets it) but off with no setter or doc; unsure |
| `SOTC_VIEW_CHUNK_MB` | value | none | src/runtime_memory.c:655 | fallback view chunk size (2 MB) used when the KB switch is unset; no setter; unsure |
| `SOTC_VIS_DIRECT` | off | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/sotc_stats.h:159 | upstream-only experiment: labels written late by address (liverpool.cpp:312); off; unsure |
| `SOTC_WAIT_MARKER` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp:49 | listed as experimental in docs/sotc/README-SotC.md:165; off, no setter |
| `SOTC_WATCH_SKIP` | off | none | gpu/sotc/nonblocking.h:422 | off; part of the native GPU-only protect policy (page_manager.cpp:2345 comment); unsure |
| `SOTC_WRITE_SCOPE_FLAT` | off | none | gpu/shadps4/video_core/buffer_cache/write_set.h:38 | off unless set; no setter, no docs; unsure |
| `SOTC_YIELD_BACKOFF` | off | none | src/runtime_thread.c:618 | off; experimental yield backoff in docs/re-stutters/reports3/draw/wave3/gameside.md:48; unsure |

## Keep: development tools

| Switch | Default | Set by | Where | Reason |
|---|---|---|---|---|
| `BB_AUDIO` | value | none | src/runtime_audio.c:130 | Headless switch (BB_AUDIO=none) per runtime_audio.c:7 comment; no launcher or tool sets it. |
| `BB_AUDIO_DUMP` | off | none | src/runtime_audio.c:189 | Raw PCM dump for audio debugging; diagnostic with no setter. |
| `BB_AUDIO_STATS` | off | none | src/runtime_audio.c:188 | Audio gap statistics printer; diagnostic only. |
| `BB_AUDIO_TRACE` | off | none | src/runtime_file.c:256 | Missing sound file trace documented in runtime_file.c:255 comment. |
| `BB_BUNDLED_VK_DRIVER_FILES` | n.a. | none | launcher/bbport_vulkan.py:64 | Unsure: packaging plumbing read by the launcher Vulkan helper; no setter. |
| `BB_DATA_DIR` | n.a. | none | launcher/bbport_vulkan.py:80 | Unsure: data folder path override read by the launcher Vulkan helper; no setter or doc. |
| `BB_DUMP_DIR` | value | tools | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1477 | Dump folder used by tools/sotc_play.py, sotc_shots.sh and gameplay_bench.py. |
| `BB_DUMP_SHADERS` | off | none | gpu/shim/core/emulator_settings.h:74 | docs/sotc/AGUA.md:82 tells how to read the GCN IR it writes; impl-n4-diag.md keeps it as a tool. |
| `BB_FAULT_RING` | off | none | src/probe.c:1989 | Crash record file documented in docs/sotc/README-SotC.md:154; diagnostic. |
| `BB_FPS_LIMIT` | value | none | gpu/shim/core/emulator_settings.h:52 | Uncapped A/B benchmark variable in docs/history/parallel_gpu.md:113; launcher uses SOTC_FPS_LIMIT instead. |
| `BB_FRAME_DUMP_TRIGGER` | n.a. | tools | gpu/shim/bbgpu.cpp:775 | Frame dump trigger used by tools/sotc_play.py, sotc_shots.sh and raw2png.py. |
| `BB_FRAME_STATS` | off | tools | gpu/shim/bbport_toggles.h:97 | Stall diagnostic used by tools/ab.sh and docs/history/CURRENT_STATUS_2026-09-29.md:64. |
| `BB_GPU_ID` | value | none | gpu/shim/core/emulator_settings.h:27 | GPU index override read by tools/gpu_capabilities.c:98; no launcher row. |
| `BB_GPU_LOG` | value | tools | gpu/shim/common/logging/log.cpp:11 | Log level for GPU code; tools/SotC-Shaders.ps1:87 sets info. |
| `BB_IME_TEXT` | off | none | src/runtime_services.c:622 | Preset IME text for scripted runs; no setter; unsure. |
| `BB_INTERNAL_HEIGHT` | value | none | gpu/shim/core/emulator_settings.h:29 | Unsure: fixed internal height override (default 1080); no setter or doc, launcher uses SOTC_OUTPUT_RES. |
| `BB_INTERNAL_WIDTH` | value | none | gpu/shim/core/emulator_settings.h:28 | Unsure: fixed internal width override (default 1920); no setter or doc, launcher uses SOTC_OUTPUT_RES. |
| `BB_LIST_IMPORTS` | off | none | src/probe.c:2192 | Import listing for diagnosis; tools/sotc_run.sh:3 names it. |
| `BB_MEMORY_LOG` | off | none | src/runtime_memory.c:1751 | Reserved range log named in docs/re-stutters/reports/04-memory-model.md:108. |
| `BB_MODS_DIR` | n.a. | none | src/runtime_file.c:341 | Mods folder override read with BB_MOD_TRACE; no launcher row; unsure whether mods stay supported. |
| `BB_MOD_TRACE` | off | none | src/runtime_file.c:341 | Mod file trace; diagnostic with no setter. |
| `BB_NULL_GPU` | off | none | gpu/shim/core/emulator_settings.h:77 | Null GPU diagnostic read by vk_rasterizer.cpp; no doc or tool; unsure. |
| `BB_NVIDIA_LIB_DIR` | n.a. | none | launcher/bbport_vulkan.py:67 | Unsure: NVIDIA library folder override read by the launcher helper; documented only in docs/original-readme/README.md:194. |
| `BB_OVERLAY_MENU` | on | none | gpu/shim/bbport_overlay.cpp:134 | Unsure: bbport_overlay.cpp:129 says the SotC port shows only game dialogs; =1 forces the Bloodborne menu. |
| `BB_PAD_FILE` | off | sotc.py,tools | src/runtime_pad.c:449 | Scripted input for automated runs; sotc.py:701 and tools set it. |
| `BB_PAD_RECORD` | off | tools | src/runtime_pad.c:505 | F9 route recording used by tools/sotc_bench_shrine.py:108. |
| `BB_PAD_REPLAY` | off | tools | src/runtime_pad.c:539 | Replay of a recorded route, used by tools/sotc_bench_shrine.py:110. |
| `BB_READBACKS` | value | launcher | gpu/shim/core/emulator_settings.h:36 | Unsure: set only from a saved setting (launcher:640-641) with no launcher UI row; part of the readback family. |
| `BB_RENDER_RES` | n.a. | none | gpu/shim/bbport_settings.cpp:171 | Explicit start-resolution compatibility patch described in docs/history/CURRENT_STATUS_2026-09-29.md:77. |
| `BB_RESTART_COMMAND` | n.a. | none | src/probe.c:1874 | Unsure: runtime restart command; its comment cites run.py, which is not in the tree at 18a1251. |
| `BB_STACKS_TRIGGER` | off | tools | src/probe.c:1990 | Thread stack dump trigger used by tools/sotc_play.py and tools/stacks_sym.py. |
| `BB_STRICT_HANDLES` | off | none | src/probe.c:1998 | Handle diagnostics per the probe.c:1367 comment. |
| `BB_TOGGLE_FILE` | on | tools | src/runtime_memory.c:2015 | A/B toggle file used by tools/ab.sh; docs/history/CURRENT_STATUS_2026-09-29.md:65. |
| `BB_TRACE_SEMA` | off | none | src/runtime_sema.c:72 | Deadlock diagnosis print named in its code comment. |
| `BB_UPSCALER` | n.a. | none | gpu/shim/bbport_settings.cpp:113 | Environment override for scripts and A/B tests (bbport_settings.cpp:112 comment); launcher uses SOTC_UPSCALER. |
| `BB_UPSCALE_PRESET` | value | none | gpu/shim/bbport_settings.cpp:123 | Unsure: preset is consumed at bbport_settings.cpp:178, but no launcher row or doc sets it. |
| `BB_VK_MARKERS` | off | none | gpu/shim/core/emulator_settings.h:88 | Vulkan debug markers for RenderDoc and profiling; off by default. |
| `BB_VK_VALIDATION` | off | none | gpu/shim/core/emulator_settings.h:91 | Vulkan validation layer switch; off by default, development diagnostic. |
| `BB_VK_VALIDATION_SYNC` | off | none | gpu/shim/core/emulator_settings.h:93 | Vulkan sync validation switch; off by default, development diagnostic. |
| `BB_VULKANINFO` | value | none | launcher/bbport_vulkan.py:122 | Unsure: vulkaninfo tool path override read by the launcher helper; no setter. |
| `BB_WRITE_LOG` | off | none | gpu/shim/bbport_write_log.cpp:42 | Guest write log for GPU fault debugging, named in docs/history/SESSION_2026-09-30.md:216. |
| `SOTC_ALLOC_SITES` | off | none | gpu/sotc/alloc_sites.cpp:449 | observe-only allocation registry; tools/gen_gpu_sites.py builds its table from it |
| `SOTC_ALLOC_SITES_FILE` | value | none | gpu/sotc/alloc_sites.cpp:311 | output path of the alloc sites table (alloc_sites.cpp:311), same tool workflow |
| `SOTC_ALLOC_TRACE` | off | tools | gpu/sotc/alloc_sites.cpp:83 | trace records read by tools/alloc_trace_rules.py |
| `SOTC_ALLOC_TRACE_FILE` | value | none | gpu/sotc/alloc_sites.cpp:353 | output path of the alloc trace table (alloc_sites.cpp:353) |
| `SOTC_AMD_AS_NV` | off | none | - | upstream-only shader-test switch (upstream vk_pipeline_cache.cpp:356); unsure, not in the fork |
| `SOTC_AREA_TRACE` | off | none | gpu/sotc/probe_area.cpp:273 | diagnostic state trace of the area loaders (probe_area.cpp:273) |
| `SOTC_ARENA_ATTRIB` | off | tools | gpu/sotc/arena_attrib.cpp:379 | tools/arena_attrib.py ranks the writers this switch logs |
| `SOTC_BIG_SYNC_LOG` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9713 | diagnostic log of big syncs (buffer_cache.cpp:9713), cited in four docs |
| `SOTC_BLOCK_LEDGER` | off | tools | gpu/sotc/block_ledger.h:25 | observe-only ledger; tools/build_clang64.ps1 toggles it and DRAW-STUTTER-PLAN cites it |
| `SOTC_BOOT_BOSS` | n.a. | modes,tools | sotc.py:32 | boots straight into a colossus for SotC Lab routes (sotc.py:528); tests and docs/sotc/LAB.md use it |
| `SOTC_BUILDSYNC_PROBE` | off | none | gpu/sotc/probe_buildsync.cpp:71 | observe-only buildsync hook probe (scripts/hooks.d/buildsync.py:26) |
| `SOTC_CALL_PROBE` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4967 | observe-only entry hook probe (scripts/hooks.d/calls.py:27), with a run check in indirect-callers.md |
| `SOTC_CAM_LOG` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:492 | camera log read by the upstream tools/gameplay_bench.py and tools/save_place.py; observe-only in vk_dlssg.cpp |
| `SOTC_CAM_PROBE` | off | none | gpu/sotc/freecam.cpp:201 | observe-only camera hook probe (scripts/hooks.d/cam_probe.py:25) |
| `SOTC_CARVE_LOG` | off | launcher-ui | src/runtime_memory.c:780 | launcher diagnostic flag (diag group) for memory view splits (runtime_memory.c:780) |
| `SOTC_CENSUS` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3704 | observe-only texture census (texture_cache.cpp:3704, set enables); cited as a report |
| `SOTC_CLASS_MAX_MB` | value | none | gpu/shadps4/video_core/buffer_cache/region_manager.h:270 | budget of the CLASS_TRACK family (region_manager.h:270); read only, part of the pcmem experiment (unsure) |
| `SOTC_CLASS_POOLS` | off | sotc.ini,tools | gpu/sotc/mem_classes.cpp:440 | sotc.ini sets 0 (old PageAllocator path); tools/pcmem_matrix.py sets it for experiments |
| `SOTC_CLASS_TRACK` | off | tools | gpu/sotc/mem_classes.cpp:1090 | tools/pcmem_matrix.py sets it for the class experiments; HANDOFF records the results |
| `SOTC_CLASS_WARMUP_S` | value | none | gpu/sotc/mem_classes.cpp:567 | warm-up of the class-track family (mem_classes.cpp:567), used in HANDOFF runs (unsure) |
| `SOTC_CLIP_EMU` | off | none | - | upstream-only shader-test switch (upstream cache_storage.cpp:49); unsure |
| `SOTC_CMD_COUNTERS` | off | none | gpu/sotc/cmd_counters.h:17 | observe-only command counters (cmd_counters.h:2); measurement helper (unsure) |
| `SOTC_CMD_STATS` | off | tools | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3857 | measurement counters; tools/pcmem_matrix.py drawverify config sets it |
| `SOTC_COMMIT_CHUNKS` | off | tools | src/runtime_memory.c:1107 | tools/pcmem_matrix.py chunk configs set it (memory experiment) |
| `SOTC_COMMIT_ON_DEMAND` | off | tools | src/runtime_memory.c:1107 | tools/pcmem_matrix.py commit config sets it (memory experiment) |
| `SOTC_CP_STRICT` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:440 | identity-check switch in the docs (SOTC_CP_THREADS=0 SOTC_CP_STRICT=1, 10-port-redesign.md:103) |
| `SOTC_CSDR_DUMP_DIR` | off | none | gpu/sotc/probe_csdr.cpp:65 | dump directory of the CSDR probe (probe_csdr.cpp:65) |
| `SOTC_CSDR_DUMP_MAX` | value | none | gpu/sotc/probe_csdr.cpp:66 | dump limit of the CSDR probe (probe_csdr.cpp:14) |
| `SOTC_CSDR_EXTRACT` | off | tools | gpu/sotc/probe_csdr.cpp:110 | extract mode used by tools/csdr/pack_csdr.py |
| `SOTC_CSDR_EXTRACT_OUT` | value | tools | gpu/sotc/probe_csdr.cpp:234 | output of the CSDR extract mode used by tools/csdr/pack_csdr.py |
| `SOTC_CSDR_PROBE` | off | modes,tools | gpu/sotc/probe_csdr.cpp:223 | observe-only probe (scripts/hooks.d/csdr.py:20) driven by tools/csdr/pack_csdr.py |
| `SOTC_CSDR_PROBE_OUT` | value | none | gpu/sotc/probe_csdr.cpp:60 | output file of the CSDR probe (probe_csdr.cpp:60) |
| `SOTC_CS_DUMP` | off | modes | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8511 | compute dump switch (vk_rasterizer.cpp:8511) used by the dev dump routes (unsure) |
| `SOTC_CS_STATS` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2012 | launcher diag-group flag, measurement only (vk_rasterizer.cpp:2005) |
| `SOTC_CS_TRACE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3810 | compute trace switch (vk_rasterizer.cpp:3810) |
| `SOTC_CS_WATCH` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3815 | compute watch switch (vk_rasterizer.cpp:3815) |
| `SOTC_CTL` | off | launcher-ui,modes,tools | gpu/sotc/ctl_server.cpp:42 | SotC Lab control pipe name, launcher diag row and sotc.py |
| `SOTC_CTL_LIVE` | n.a. | tools | tests/test_ctl_protocol.py:3 | test gate of tests/test_ctl_protocol.py (no game reader) |
| `SOTC_CTL_PIPE` | value | tools | tests/test_ctl_protocol.py:4 | pipe name read by tests/test_ctl_protocol.py (no game reader) |
| `SOTC_DEBUG_MENU` | off | launcher | src/runtime_file.c:269 | Launcher sets it unconditionally for the debug menu (launcher/sotc_launcher_win.py:738); Lab and diagnostic plumbing. |
| `SOTC_DECOMMIT_BUDGET` | off | tools | gpu/sotc/decommit_budget.cpp:36 | Memory measurement hook read through scripts/hooks.d/decommit.py; unsure, no doc records a result. |
| `SOTC_DETERMINISTIC` | off | tools | sotc.py:470 | Deterministic-run switch read by sotc.py and forced by tools/offline_matrix.py (FORCED_KEYS); verify tooling. |
| `SOTC_DET_CLOCK` | off | tools | src/runtime_detclock.c:55 | Deterministic clock for route replay; set by the sotc.py det profile (sotc.py:90). |
| `SOTC_DET_CLOCK_HOST_RT` | off | none | src/runtime_detclock.c:134 | Sub-option of SOTC_DET_CLOCK; keep with its parent. |
| `SOTC_DET_CLOCK_HZ` | value | none | gpu/shadps4/video_core/sotc_det_flip.h:43 | Clock-rate knob of the deterministic clock (default 30); keep with its parent. |
| `SOTC_DET_CLOCK_TSC` | off | none | src/runtime_detclock.c:148 | Sub-option of SOTC_DET_CLOCK (TSC source); keep with its parent. |
| `SOTC_DET_EXIT_FRAMES` | value | tools | gpu/sotc/det_route.h:50 | Route exit frame count; set by tests/test_offline_matrix.py and read by the det route. |
| `SOTC_DET_FLIP` | off | tools | gpu/shadps4/video_core/sotc_det_flip.h:25 | Deterministic flip mode; set by the sotc.py det profile (sotc.py:90). |
| `SOTC_DET_REALTIME` | off | tools | gpu/shadps4/video_core/sotc_det_flip.h:34 | Det-route realtime option; set by the sotc.py profile (sotc.py:92). |
| `SOTC_DET_ROUTE` | off | tools | gpu/sotc/det_route.h:22 | Deterministic route replay for the Lab; set by sotc.py:91, read also by src/runtime_pad.c:380. |
| `SOTC_DET_ROUTE_FPS` | value | none | gpu/sotc/det_route.h:31 | Route clock frame rate (default 30); knob of the route replay. |
| `SOTC_DET_ROUTE_TICK_HZ` | value | none | gpu/sotc/freecam.cpp:302 | Route tick rate (default 60); knob of the route replay. |
| `SOTC_DET_STALL_MS` | value | none | src/runtime_detclock.c:59 | Stall release time of the deterministic clock (default 50 ms); knob. |
| `SOTC_DET_WAIT_CAP_MS` | value | none | src/runtime_detclock.c:60 | Wait cap of the deterministic clock (default 10000 ms); knob. |
| `SOTC_DLSSG` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:39 | Legacy alias of SOTC_FRAMEGEN; the launcher strips old SOTC_DLSSG=1 (sotc_launcher_win.py:566), but tools/gameplay_bench.py:273 still asks for it; unsure. |
| `SOTC_DLSSG_ASSUME_FOCUS` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:982 | DLSS-G focus knob; no launcher, tool or doc sets it; unsure. |
| `SOTC_DLSSG_IMMEDIATE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1521 | DLSS-G immediate-present knob; no doc records a result; unsure. |
| `SOTC_DLSSG_LOG` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:916 | Log level of the DLSS-G module (logging only). |
| `SOTC_DLSSG_ONLY_GENERATED` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:878 | Diagnostic that shows only generated frames; unsure. |
| `SOTC_DLSS_DEPTH_FLIP` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:453 | DLSS depth-flip experiment, off; no launcher, tool or doc use recorded; unsure, not clearly measured. |
| `SOTC_DLSS_MVEC_MUL` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:449 | Motion-vector multiplier knob for DLSS; no launcher or doc use; unsure. |
| `SOTC_DLSS_PROBE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10071 | Probe switch used by the N4 diagnostics (re-stutters/native/impl-n4-diag.md:25) and the trace. |
| `SOTC_DLSS_SANITIZE` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1156 | Sanitize-mode knob of DLSS; no launcher or doc use recorded; unsure. |
| `SOTC_DLSS_VIEW` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1507 | DLSS debug view (diagnostic); not set by launcher or sotc.ini. |
| `SOTC_DRAW_CALLERS` | off | tools | gpu/sotc/probe_xpr_groups.cpp:92 | Draw-caller probe; read through scripts/hooks.d/xpr_groups.py. |
| `SOTC_DRAW_MDI_NOEXT` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2973 | Diagnostic that makes the MDI path issue consecutive draws; keep with SOTC_DRAW_MDI. |
| `SOTC_DRAW_MERGE_STATS` | off | sotc.ini | gpu/shadps4/video_core/sotc_draw_merge.h:35 | Merge counters; sotc.ini sets 0; 14-draw-reduction.md:213 uses it for verification. |
| `SOTC_DRAW_RUNS` | off | none | gpu/shadps4/video_core/sotc_draw_runs.h:20 | Indirect draw-run report printed every 5 s (driver.cpp:2489); diagnostic. |
| `SOTC_DRAW_SITES` | off | none | gpu/sotc/probe_drawsite.cpp:107 | Draw-site probe; diagnostic. |
| `SOTC_DRAW_SPLIT` | off | tools | gpu/shadps4/video_core/sotc_stats.h:1190 | Counters only (docs/sotc/PLANO-THREAD-DESENHO.md:116); tools/pcmem_matrix.py:64 sets it. |
| `SOTC_DRAW_TAG` | off | tools | scripts/hooks.d/drawtag.py:18 | Hook switch read by the hook engine (no C reader); draw-tag trace. |
| `SOTC_DRAW_WORK` | off | tools | gpu/shadps4/video_core/sotc_stats.h:1882 | Draw-work metric; set by tools/pcmem_matrix.py:65. |
| `SOTC_DRIVER_CACHE_ALWAYS` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1159 | Off unless =1; restores the old save rule (re-stutters/native/arch-next/nonblocking-frame.md:241); unsure. |
| `SOTC_DROP_TRACE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5072 | Trace of the drop path (also sotc_stats.h:1698); diagnostic. |
| `SOTC_DT_PROBE` | off | tools | scripts/hooks.d/dt.py:23 | Hook switch for the DT probe; diagnostic. |
| `SOTC_DUMP` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:8541 | Guest data dump for debugging; only forwarded by sotc.py:271. |
| `SOTC_ENGINE_CMD` | off | tools | scripts/hooks.d/enginecmd.py:26 | Lab engine-command switch read by the hook engine; sotc.py:533 uses it. |
| `SOTC_EOP_TICK` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:509 | Diagnostics group of the launcher (sotc_tuning.py, "measuring only"); the code warns it is diagnostic only (~130 GPU round trips per frame). |
| `SOTC_EOP_TICK_US` | value | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3250 | Interval of SOTC_EOP_TICK (default 2000); diagnostics group. |
| `SOTC_EVENT_LOG` | off | launcher | src/runtime_event.c:50 | Launcher starts the event log for each run (sotc_launcher_win.py:2184, :2497); run log. |
| `SOTC_EVF_TRACE` | off | none | src/runtime_sotc.c:337 | Trace of EVF calls; diagnostic. |
| `SOTC_EXPAND_COPY_STALE` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1181 | Rollback of the upstream #5444 ExpandImage fix (docs/sotc/ROADMAP.md:100 says "=1 volta"); unsure whether to keep. |
| `SOTC_FAULT_OSCOST` | off | launcher-ui | gpu/shadps4/video_core/page_manager.cpp:1362 | Diagnostics row of the launcher (sotc_tuning.py:1586), measuring only. |
| `SOTC_FAULT_OWNERS` | off | none | gpu/sotc/probe_memtype.cpp:1313 | Page-owner probe; diagnostic. |
| `SOTC_FG_JITTER` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:651 | Jitter mode knob of FSR frame generation (default 1); no launcher or doc use; unsure. |
| `SOTC_FIBER_STACKS` | off | none | src/runtime_fiber.c:251 | Fiber stack logging; diagnostic. |
| `SOTC_FILE_DEBUG` | off | none | src/runtime_file.c:313 | Prints the host path of files not found; diagnostic. |
| `SOTC_FLIP_TRACE` | off | none | gpu/shadps4/core/libraries/kernel/equeue.cpp:638 | Part of SOTC_TRACE; diagnostic. |
| `SOTC_FPS_CAP_TEST_CYCLE` | value | none | gpu/shadps4/video_core/sotc_fps_cap.h:64 | Test cycle of the FPS cap; measurement. |
| `SOTC_FRAMEGEN_TEST_CYCLE` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:156 | Test cycle of frame generation; diagnostic. |
| `SOTC_FRAME_HASH` | off | tools | gpu/shadps4/video_core/renderer_vulkan/sotc_frame_hash.cpp:42 | Per-frame image hash for determinism checks; forced by tools/offline_matrix.py (FORCED_KEYS). |
| `SOTC_FRAME_HASH_PNG_EVERY` | value | none | gpu/shadps4/video_core/renderer_vulkan/sotc_frame_hash.cpp:27 | PNG cadence of the frame hash tool. |
| `SOTC_FRAME_LOG` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:2596 | Frame log level; diagnostic. |
| `SOTC_FRAME_METRICS` | off | tools | gpu/shadps4/video_core/sotc_frame_metrics.h:20 | Per-frame metrics file read by the tools; keep. |
| `SOTC_FRAME_RINGS` | off | tools | gpu/sotc/probe_frame_rings.cpp:41 | Frame-ring probe; scripts/hooks.d/frame_rings.py sets it. |
| `SOTC_FREECAM` | off | tools | gpu/sotc/freecam.cpp:210 | Lab free camera; also set by SOTC_LAB paths (sotc.py:533). |
| `SOTC_FREECAM_CARRY` | value | none | gpu/sotc/freecam.cpp:205 | Lab free-camera knob (default 1, on). |
| `SOTC_FREECAM_DROP` | value | none | gpu/sotc/freecam.cpp:206 | Lab free-camera knob (default 8). |
| `SOTC_FREECAM_MAXSPEED` | value | launcher-ui | gpu/sotc/freecam.cpp:292 | Launcher diagnostics row (sotc_tuning.py:1824, default 60) for the Lab free camera. |
| `SOTC_FREECAM_SPEED` | value | none | gpu/sotc/freecam.cpp:204 | Lab free-camera knob. |
| `SOTC_FSRFG_LOG` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:294 | FSR frame-generation log level; diagnostic. |
| `SOTC_FSRFG_ONLY_GENERATED` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:2232 | Diagnostic that shows only generated FSR-FG frames; unsure. |
| `SOTC_GAME_AFFINITY` | off | none | src/runtime_thread.c:326 | unsure: off unless set to 1, no launcher row, no setter, one doc hit; switch-audit says check whether anything uses it |
| `SOTC_GCN_CENSUS` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3141 | measurement-only GCN op census named in GCN-NATIVE-PLAN |
| `SOTC_GEN_BIND` | off | sotc.ini,tools | gpu/shadps4/video_core/sotc_generation.h:25 | tools/pcmem_matrix.py configs use it; sotc.ini sets 0; switch-audit says delete at native |
| `SOTC_GEN_DMA` | off | tools | gpu/shadps4/video_core/sotc_generation.h:32 | tools/pcmem_matrix.py configs use it; switch-audit says delete at native |
| `SOTC_GEN_TEX` | off | tools | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1768 | tools/pcmem_matrix.py configs use it; switch-audit says delete at native |
| `SOTC_GPU_CHECKPOINTS` | off | sotc.ini | gpu/shadps4/video_core/sotc_checkpoints.cpp:225 | GPU checkpoint diagnostic still in use for hangs; sotc.ini sets it 0 |
| `SOTC_GPU_VENDOR` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:133 | device vendor override for testing, CHANGELOG.md:1062 documents it |
| `SOTC_HEADLESS` | off | modes,tools | gpu/shim/window.cpp:37 | headless window for unattended and matrix runs; docs/sotc/OFFLINE-TESTS.md documents it |
| `SOTC_HOOKS` | off | none | gpu/sotc/hooks.cpp:668 | master switch of the hook engine used by scripts/hooks.d; kept for development |
| `SOTC_HOOK_CVAR_PROBE` | off | none | gpu/sotc/probe_cvars.cpp:3 | observe-only probe, switch-audit MERGE-INTO-TRACE |
| `SOTC_HOOK_HEAP_RANGE` | off | none | gpu/sotc/probes.h:16 | observe-only heap-range probe, switch-audit MERGE-INTO-TRACE |
| `SOTC_HOOK_LOADER_NOSTREAM` | off | none | gpu/sotc/probes.h:12 | observe-only loader probe, switch-audit MERGE-INTO-TRACE |
| `SOTC_HOOK_LOADER_PROBE` | off | none | gpu/sotc/probe_loader.cpp:3 | observe-only loader probe, switch-audit MERGE-INTO-TRACE |
| `SOTC_HOOK_XPR_PROBE` | off | modes,tools | gpu/sotc/probes.h:8 | set by modes and tools; switch-audit DELETE-AT-NATIVE, but still used by them |
| `SOTC_IMAGE_BASE` | value | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:204 | launcher diagnostics row (game image base, measuring only) |
| `SOTC_IMAGE_DEDICATED` | n.a. | none | n.a. | upstream-only, unsure; decide after merge |
| `SOTC_IMAGE_DEDICATED_TEXELS` | n.a. | none | n.a. | upstream-only, unsure; decide after merge |
| `SOTC_IMG_CREATE_AHEAD` | off | tools | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:60 | unsure: tools mention, off, 4 doc hits; switch-audit UNKNOWN-MEASURE |
| `SOTC_IMG_POOLED` | off | tools | gpu/shadps4/video_core/texture_cache/image.cpp:134 | unsure: tools mention, 10 doc hits; switch-audit UNKNOWN-MEASURE |
| `SOTC_INDIRECT_TRACE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:214 | trace span of indirect draws, merged into SOTC_TRACE per switch-audit |
| `SOTC_INTEL_PIN_SUBGROUP` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_instance.h:402 | unsure: Intel-only subgroup pin, no setter, 1 doc hit (ROADMAP); switch-audit UNKNOWN-MEASURE |
| `SOTC_LAB` | off | sotc.ini,launcher-ui,modes,tools | gpu/sotc/freecam.cpp:206 | SotC Lab (menu, free camera, automated tests); sotc.ini sets 1 |
| `SOTC_LABELS_HONEST` | off | none | gpu/sotc/label_honest.cpp:30 | labels for captures and Lab runs, 17 doc hits |
| `SOTC_LAB_ORCH_CMD` | off | none | gpu/sotc/lab_commands.cpp:1011 | overrides the matrix orchestrator command (tools/sotc_matrix.py) |
| `SOTC_LAB_ROUTE` | off | modes,tools | gpu/sotc/engine_cmd.cpp:228 | automated test route, memory automated-test-route.md |
| `SOTC_LAB_ROUTE_DELAY` | value (0) | modes | gpu/sotc/lab_commands.cpp:734 | route timing set by modes |
| `SOTC_LAB_ROUTE_EXIT` | value (1.5) | modes | gpu/sotc/lab_commands.cpp:735 | route timing set by modes |
| `SOTC_LAB_ROUTE_GAME_DRAWS` | value | none | gpu/sotc/lab_commands.cpp:778 | Lab route tuning, no setter in the repo |
| `SOTC_LAB_ROUTE_LOAD_FLIPS` | value | none | gpu/sotc/lab_commands.cpp:779 | Lab route tuning, no setter in the repo |
| `SOTC_LAB_ROUTE_LOAD_MAX` | value (60) | none | gpu/sotc/lab_commands.cpp:780 | Lab route tuning, no setter in the repo |
| `SOTC_LAB_ROUTE_WAIT` | value | none | gpu/sotc/engine_cmd.cpp:233 | Lab route wait, no setter in the repo |
| `SOTC_LOD_FB_EVERY` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:190 | unsure: switch-audit DELETE-UNUSED but 5 doc hits, kept until checked |
| `SOTC_LOD_FEEDBACK` | off | none | gpu/shadps4/shader_recompiler/info.cpp:390 | unsure: off, 5 doc hits including CHANGELOG.md:816; switch-audit UNKNOWN-MEASURE |
| `SOTC_LOD_LEVEL` | value | none | gpu/shadps4/video_core/sotc_lod_stats.h:153 | unsure: forced LOD level for measurement, 6 doc hits, no setter |
| `SOTC_LOD_POOL_LOG` | off | none | gpu/shadps4/video_core/sotc_lod_stats.h:192 | pool log, merged into SOTC_FPS per switch-audit; measured on 21 Oct (CHANGELOG-era notes) |
| `SOTC_LOD_STATS` | on | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:5917 | diagnostic counters on by default, merged into SOTC_FPS per switch-audit; kept as a report |
| `SOTC_LOD_TRACE` | value | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:5933 | LOD trace, merged into SOTC_TRACE per switch-audit |
| `SOTC_MANUAL_INTERP` | n.a. | none | n.a. | upstream-only, unsure; decide after merge |
| `SOTC_MAP_LOG` | off | none | src/probe.c:1236 | mapping history dump for diagnostics, 5 doc hits |
| `SOTC_MDI_STATS` | off | launcher-ui | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:158 | launcher diagnostics row, counts mergeable indirect draws |
| `SOTC_MEMTYPE_CLASSES` | value | tools | gpu/sotc/probe_memtype.cpp:1033 | unsure: UMA memtype classes, tools mention; switch-audit DELETE-AT-NATIVE |
| `SOTC_MENU_TRIGGER` | off | tools | gpu/shim/bbport_overlay.cpp:1832 | opens the Lab menu for tools-driven runs, 3 doc hits |
| `SOTC_MINIDUMP` | on | modes | src/probe.c:950 | crash minidump, default on; modes/sotc.py sets it; kept for crash diagnosis |
| `SOTC_MIRROR` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8939 | unsure: mirror verify mode (SwitchMode), 5 doc hits |
| `SOTC_MOUSE_DEBUG` | off | none | gpu/shim/window.cpp:373 | mouse debug logging, 1 doc hit |
| `SOTC_NATIVE_BUFFER_VERIFY` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6207 | verify mode of native buffer binds (write generations); no launcher or sotc.ini use |
| `SOTC_NO_PREPASS` | off | sotc.ini | gpu/shadps4/core/libraries/videoout/driver.cpp:742 | measurement mode (=2 report) cited by re-backend/04 and 14 docs; sotc.ini sets 0 |
| `SOTC_NULL_AUDIO` | on | modes | sotc.py:91 | headless offline-test default set by sotc.py (also PRECOMPILE_DEFAULTS); OFFLINE-TESTS.md |
| `SOTC_OPCOST` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:2058 | op-cost profiler of one op per thread; opcost.md |
| `SOTC_OPSET_STATS` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2194 | measurement of a possible per-op record (OpSet); ARCH-BINDSET.md |
| `SOTC_OPSTAT` | off | launcher-ui | gpu/shadps4/video_core/amdgpu/liverpool.cpp:454 | op statistics; listed as a diagnostic flag in the launcher; impl-n4-diag.md |
| `SOTC_OPT_OFF` | off | none | gpu/shadps4/video_core/sotc_stats.h:1407 | switches off the 2026-10-14 command-processor optimizations to replicate old behaviour (CHANGELOG); unsure |
| `SOTC_OP_PHASES` | off | launcher-ui | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:2340 | phase counters; launcher diagnostic choice; op-phases taxonomy docs |
| `SOTC_OVERLAY` | off | none | gpu/shim/bbport_overlay.cpp:222 | performance overlay switch (F10); switch-audit doc; SOTC_FPS sits alongside it |
| `SOTC_PACK_REFERENCED` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2144 | records late pipeline libraries for the next launch (diagnostic); sotc.ini sets 0; unsure |
| `SOTC_PASS_PROBE` | off | none | gpu/sotc/hook_handlers.cpp:48 | observe-only pass probe; re-pipelines pass-map and README docs |
| `SOTC_PASS_STATS` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:141 | draws per frame into the sun shadow map per slice; re-backend/00-SUMMARY.md |
| `SOTC_PASS_VERIFY` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.cpp:262 | verify mode of the N5 pass pool (checked and mismatch counts) |
| `SOTC_PC_POOL` | off | none | gpu/shadps4/core/libraries/videoout/driver.cpp:2458 | pool report and verify mode (=2); MODELO-MEMORIA-PC-2 and poolflex-hang docs; unsure |
| `SOTC_PC_RING8_VERIFY` | off | none | gpu/sotc/pc_ring.cpp:1751 | verify mode of ring-8 binds (costs FPS per its comment); HANDOFF.md |
| `SOTC_PC_RING8_VERIFY_SAMPLE` | value | none | gpu/sotc/pc_ring.cpp:1767 | sampling rate of the ring-8 verify mode |
| `SOTC_PC_RING_BUMP` | off | none | gpu/sotc/pc_ring.cpp:718 | comparison-only hook PC_RING_BUMP; scripts/hooks.d/pcring.py and switch-audit |
| `SOTC_PC_RING_DISPATCH` | off | none | gpu/sotc/pc_ring.cpp:918 | comparison-only hook of the generic allocator; impl-winhooks.md; unsure |
| `SOTC_PC_RING_VERIFY_SAMPLE` | value | none | gpu/sotc/pc_ring.cpp:300 | sampling rate of the ring verify mode; switch-audit doc |
| `SOTC_PEEK` | off | none | src/probe.c:2255 | debug peek of image offsets after a delay; ROADMAP.md and switch-audit |
| `SOTC_PEEK_DELAY` | value | none | src/probe.c:361 | delay before SOTC_PEEK prints |
| `SOTC_PIPE_COMPILE_CHECK` | off | none | gpu/shadps4/video_core/renderer_vulkan/sotc_pipe_feedback.cpp:4 | pipeline compile measurement (feedback family) |
| `SOTC_PIPE_FEEDBACK` | off | none | gpu/shadps4/video_core/renderer_vulkan/sotc_pipe_feedback.cpp:4 | pipeline feedback measurement (TSV); nonblocking-frame and re-pipelines docs |
| `SOTC_PIPE_FEEDBACK_OUT` | value | none | gpu/shadps4/video_core/renderer_vulkan/sotc_pipe_feedback.cpp:55 | TSV output file of the pipeline feedback measurement |
| `SOTC_PLAY_WRAP` | value | tools | tools/sotc_play.py:76 | launcher prefix for RenderDoc capture runs (sotc_play.py) |
| `SOTC_PREDICT_BUFFERS_OUT` | value | none | gpu/sotc/predict_probe.cpp:730 | output file of the predict probe (buffer formats); observe only |
| `SOTC_PREDICT_IMAGES_OUT` | value | none | gpu/sotc/predict_probe.cpp:640 | output file of the predict probe (bound images); observe only |
| `SOTC_PREDICT_LAYOUTS` | value | modes | gpu/sotc/predict_probe.cpp:231 | layout file of the predict probe; sotc.py sets the default path; observe only |
| `SOTC_PREDICT_PROBE` | off | modes | gpu/sotc/predict_probe.cpp:299 | predict probe; tools/csdr/predict_table.py reads its output; sotc.py sets the path |
| `SOTC_PREDICT_TABLE` | value | modes | gpu/sotc/predict_probe.cpp:185 | prediction table path; sotc.py sets the default; csdr tooling |
| `SOTC_PRELOAD_UPLOAD_SOURCES` | value | none | gpu/sotc/preupload.cpp:99 | source filter for the preload report |
| `SOTC_PREPASS_PROBE` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:3372 | prepass probe and skip mode; probe_prepass report; cited by docs |
| `SOTC_PRESENT_FRAMES` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:619 | frame count of the present report; docs |
| `SOTC_PROFILE` | off | modes,launcher-ui | src/probe.c:1991 | sampling profiler; CLAUDE.md profiling workflow; sotc.py and sotc_tuning set it |
| `SOTC_PROFILE_DELAY` | value | none | src/probe.c:568 | profiler start delay (default 20 s) |
| `SOTC_PROFILE_OUT` | value | modes | src/probe.c:679 | profiler output file; sotc.py forwards it |
| `SOTC_PROFILE_SECONDS` | value | none | src/probe.c:568 | profiler sampling length (default 15 s) |
| `SOTC_PROFILE_SLOW_MS` | value | none | src/probe.c:590 | keeps samples of frames longer than this (profile output filter) |
| `SOTC_PR_BCPROBE` | off | none | gpu/sotc/nonblocking.h:472 | frame bind-cache probe (E2), observe only; draw_prep.h documents it |
| `SOTC_PR_KEYS` | off | sotc.ini | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4405 | heap-table keys report; sotc.ini sets 0 |
| `SOTC_RAM_REPORT` | off | tools | gpu/sotc/ram_report.cpp:100 | RAM report; tools/pcmem_matrix.py reads it |
| `SOTC_RB_CHANGES` | off | none | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5892 | statistics of whether GPU-changed bytes changed (readback stats); switch-audit doc |
| `SOTC_RB_CLASS` | off | tools | gpu/sotc/rb_class.cpp:65 | readback class report; tools/pcmem_matrix.py sweeps it |
| `SOTC_RB_CLASS_AHEAD` | off | tools | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:359 | class copies ahead, comparison only; tools/pcmem_matrix.py sweeps it |
| `SOTC_RB_STATS` | off | none | gpu/shadps4/video_core/page_manager.cpp:1482 | readback fault statistics printed every 10 s; docs |
| `SOTC_RDOC_SUBMITS` | value | none | gpu/shim/bbgpu.cpp:738 | RenderDoc capture span in submissions; tools/rdoc and sotc.py prefix list |
| `SOTC_RDOC_TRIGGER` | value | tools | gpu/shim/bbgpu.cpp:724 | RenderDoc trigger file; tools/sotc_play.py sets it |
| `SOTC_REC_SLEEP_US` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:2476 | A/B override of the recorder idle wait; unsure |
| `SOTC_REC_SPIN_US` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:2466 | A/B override of the recorder spin; unsure |
| `SOTC_RENDER_EXTRA` | value | modes | scripts/sotc_modes.py:232 | extra float sites to scale to the output size; finder aid used with the user's tests (per comment) |
| `SOTC_RENDER_KEEP` | value | modes | scripts/sotc_modes.py:218 | sites left at the 1080p value while locating HUD sites |
| `SOTC_RES_DUMP` | off | modes | gpu/sotc/probe_dump.cpp:67 | resource and body dump (observe only); sotc.py sets it for probes |
| `SOTC_RES_DUMP_MAX` | value | none | gpu/sotc/probe_dump.cpp:99 | files per type limit of the dump (default 20) |
| `SOTC_RES_DUMP_TYPES` | value | none | gpu/sotc/probe_dump.cpp:85 | extension list of the dump |
| `SOTC_RE_PROBE` | off | none | gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp:377 | observe-only SRT walk log; docs cite it |
| `SOTC_RT_MAXEXTENT` | value | tools | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1346 | render-target max-extent experiment; tools/pcmem_matrix.py sweeps it |
| `SOTC_RUN_FLAGS` | value | tools | gpu/shim/bbport_overlay.cpp:1251 | run label of the offline matrix runner (tools/offline_matrix.py) |
| `SOTC_RUN_INDEX` | value | tools | gpu/shim/bbport_overlay.cpp:1248 | run index of the offline matrix runner (tools/offline_matrix.py) |
| `SOTC_RUN_LABEL` | value | tools | gpu/shim/bbport_overlay.cpp:1242 | run label of the offline matrix runner (tools/offline_matrix.py); forces the overlay run panel |
| `SOTC_SAVE_CORRUPT` | off | none | src/runtime_services.c:350 | test hook forcing the corrupt-save dialog; no tool or doc uses it now; unsure, kept |
| `SOTC_SAVE_PROMPT` | value | tools | src/runtime_services.c:354 | tools/gameplay_bench.py, sotc_play.py, sotc_play_real_save.py and sotc_bench_shrine.py set auto to skip the save dialog |
| `SOTC_SAVE_TRACE` | off | none | src/runtime_savedata.c:332 | printf logging of savedata calls; logging switch, audit MERGE-INTO-TRACE |
| `SOTC_SET_TRIGGER` | value | tools | gpu/shim/bbport_overlay.cpp:1833 | trigger for a set of actions; tools/sotc_play.py sets it for automation |
| `SOTC_SHADER_EXTENT_LOG` | off | none | gpu/shadps4/shader_recompiler/ir/passes/sotc_shader_extent_pass.cpp:660 | log of the shader-extent pass for a switch that play uses; diagnostic |
| `SOTC_SHOT_COUNT` | value | none | gpu/shim/bbgpu.cpp:777 | screenshot count for captured frame sequences; used by the bisect notes in docs/re-stutters/HANDOFF.md:89 |
| `SOTC_SHOT_GENERATED` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:2817 | screenshot of the generated frame for frame-generation checks (ROADMAP T42) |
| `SOTC_SHOT_HUDLESS` | off | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1359 | hudless screenshot for frame-generation checks (ROADMAP T42) |
| `SOTC_SHOT_OVERLAY` | off | none | gpu/shim/bbgpu.cpp:770 | presence switch: screenshot including the overlay; listed in docs/sotc/REVISAO-DO-PROJETO.md |
| `SOTC_SHOT_SEQ` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:486 | sequence screenshot trigger for captures; off unless set |
| `SOTC_SLOW_OPS` | off | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/sotc_stats.h:201 | upstream-only: prints split counters around slow ops (diagnostic, off by default) |
| `SOTC_SPV_PROGRAM_IDS` | off | none | gpu/shadps4/shader_recompiler/info.h:131 | documented diagnostic in docs/sotc/README-SotC.md:166 (program hash in SPIR-V); off since 4 Nov |
| `SOTC_SPV_SALT` | value | none | gpu/shadps4/shader_recompiler/info.h:119 | cold-compile salt for measurements (HANDOFF-2026-11-07.md:45 and 186); value 0 default |
| `SOTC_STALE_READS` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/page_manager.cpp:981 | upstream-only: eboot-offset list for stale-read diagnosis, empty by default; unsure, kept |
| `SOTC_STALL_DETAIL` | off | launcher-ui,sotc.ini | gpu/shadps4/video_core/sotc_stats.h:38 | stall details in the log; tuning flag in the diagnostics group; sotc.ini sets 0 |
| `SOTC_STALL_MS` | value | none | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:2617 | threshold of the SOTCSTALL log line, default 70 ms (docs/re-stutters/reports/07-first-use.md:49) |
| `SOTC_TASK_GUARD` | off | none | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4598 | debugging trap that protects freed pages; used to bisect the memory bug of ROADMAP T27 |
| `SOTC_TEXCAP_AFTER` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2185 | upstream-only: one capture after N seconds for tests; no tool reads it; unsure, kept |
| `SOTC_TEX_DUMP` | off | none | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2898 | texture dump diagnostic; report counter in switch-audit (MERGE-INTO-FPS) |
| `SOTC_TEX_LIFECYCLE` | off | none | gpu/sotc/texlife.cpp:177 | texture lifecycle validation (0 off, 1 fast, 2 verify); documented in docs/re-backend/08-texture-lifecycle.md:244 |
| `SOTC_TEX_PROTECT_BATCH` | off | tools | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:75 | off; tools/pcmem_matrix.py runs it as a matrix arm |
| `SOTC_THREAD_IDS` | off | none | gpu/shadps4/common/thread.cpp:202 | prints thread ids; diagnostic, audit MERGE-INTO-TRACE |
| `SOTC_TRACE` | value | sotc.ini,launcher-ui,tools | gpu/shadps4/video_core/sotc_trace.h:45 | timeline recorder (off, 1 slow frames, 2 whole session); tuning choice (diag group); used by tools/sotc_matrix.py and tools/trace; sotc.ini sets 0 |
| `SOTC_TRACE_DIR` | value | launcher-ui,tools | gpu/shim/sotc_trace.cpp:674 | trace output folder; tuning value row (diag); tools/sotc_matrix.py sets it |
| `SOTC_TRACE_FUNCS` | value | launcher-ui,tools | gpu/sotc/hooks.cpp:307 | time-hook group switch whose value is a file of eboot functions; tools/trace/sotc_trace.py and tools/sotc_matrix.py use it |
| `SOTC_TRACE_FUNC_MIN_US` | value | none | gpu/sotc/hooks.cpp:716 | minimum span length of the function trace, default 10 us (docs/HOOK-DESIGN.md:84) |
| `SOTC_TRACE_GPU` | off | launcher-ui,sotc.ini | gpu/shim/sotc_gpu_trace.h:35 | real GPU times in the timeline; tuning flag (diag group); sotc.ini sets 0 |
| `SOTC_TRACE_IMPORTS` | value | launcher-ui,tools | src/runtime_trace_imports.c:338 | PS4 import counters and spans (0 off, 1 counters, 2 spans); tuning choice (diag); tools/trace_thunk_test.c sets it |
| `SOTC_TRACE_IMPORT_MIN_US` | value | tools | src/runtime_trace_imports.c:341 | minimum span for TRACE_IMPORTS=2, default 50 us (docs/sotc/README-SotC.md:208); tools/trace_thunk_test.c sets it |
| `SOTC_TRACE_JSON` | off | none | gpu/shim/sotc_trace.cpp:63 | JSON trace output switch; documented in docs/sotc/LAB.md:208 |
| `SOTC_TRACE_MAX` | value | launcher-ui | gpu/shim/sotc_trace.cpp:280 | maximum trace files per session, default 40; tuning value row (diag); docs/sotc/LAB.md:211 |
| `SOTC_TRACE_MS` | value | launcher-ui | gpu/shim/sotc_trace.cpp:277 | slow-frame threshold, default 60 ms; tuning value row (diag); docs/sotc/LAB.md:204 |
| `SOTC_TRACE_ONLY` | value | none | gpu/shim/sotc_trace.cpp:118 | filter of trace event names (unset means all); CHANGELOG.md:40 |
| `SOTC_TRACE_POST_MS` | value | launcher-ui | gpu/shim/sotc_trace.cpp:279 | time recorded after a slow frame, default 40 ms; tuning value row (diag) |
| `SOTC_TRACE_PRE_MS` | value | launcher-ui | gpu/shim/sotc_trace.cpp:278 | time recorded before a slow frame, default 150 ms; tuning value row (diag) |
| `SOTC_TRACE_RING` | value | launcher-ui | gpu/shim/sotc_trace.cpp:283 | event ring size multiplier; tuning value row (diag) |
| `SOTC_TRACE_SAMPLE` | value | launcher-ui | gpu/shim/sotc_sampler.cpp:525 | stack sampling rate in Hz, 0 off; tuning value row (diag); used in HANDOFF-2026-11-07.md:196 |
| `SOTC_TRACE_SAMPLE_GUEST` | value | none | gpu/shim/sotc_sampler.cpp:546 | guest-side sampling rate; only read when the sampler is on |
| `SOTC_TRACE_SCOPES` | off | launcher-ui,sotc.ini | gpu/sotc/probe_scopes.cpp:2 | engine profiler scopes as spans; read by the hook engine (scripts/hooks.d/profiler_scopes.py); tuning flag (diag); sotc.ini sets 0 |
| `SOTC_TRACE_ZONES` | off | launcher-ui | gpu/shadps4/video_core/sotc_trace.h:58 | shadPS4 zones into the timeline; tuning flag (diag group); used in HANDOFF-2026-11-07.md:196 |
| `SOTC_TRACE_ZONE_MIN_US` | value | tools | gpu/shadps4/video_core/sotc_trace.h:63 | minimum zone length, default 20 us (docs/sotc/README-SotC.md:206); tools/build_clang64.ps1 sets it to 0 |
| `SOTC_TSLOT` | value | none | gpu/shadps4/video_core/texture_cache/tsharp_slot_cache.cpp:50 | T# slot cache switch; docs/re-stutters/native/PERF4-BRIEF.md:63 measured with SOTC_TSLOT=2 (verify diagnostics) |
| `SOTC_TUNING_TSV` | value | none | gpu/shim/sotc_lab_menu.cpp:297 | path of the tuning table written for the Lab page (default out/tuning.tsv); docs/sotc/LAB.md:339 |
| `SOTC_UNTRACK_LOG` | off | launcher-ui | gpu/shadps4/core/libraries/videoout/driver.cpp:1127 | per-frame memory block log; tuning flag (diag group) |
| `SOTC_UPDATE_URL` | value | none | launcher/sotc_launcher_win.py:843 | overrides the update-check address so scripted tests can point at a test release (launcher comment near line 2523) |
| `SOTC_VERIFY` | value | none | gpu/sotc/native/n1verify.cpp:501 | single verify switch of the native model (native.cpp:173, verify.cpp:578); switch-audit consolidation target |
| `SOTC_VIEW_CHECK` | off | none | absent@18a1251; upstream/main:src/runtime_memory.c:951 | upstream-only: T38 diagnostic comparing host views with the region table, off by default |
| `SOTC_VK_NAMES` | off | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:114 | upstream-only: Vulkan object names for RenderDoc and validation; on with validation or debug markers |
| `SOTC_VMA_BLOCK_MB` | value | none | absent@18a1251; upstream/main:gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:775 | upstream-only: A/B override of the VMA block size; unset uses the VMA default; unsure, kept |
| `SOTC_VSIZE_PROBE` | off | none | gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp:369 | provenance probe for flat-buffer dwords; docs/re-stutters/native/re-pipelines/texture-binding.md:1234; hook in scripts/hooks.d/vsize_probe.py |
| `SOTC_WINDOW_BEHIND` | off | tools | gpu/shim/window.cpp:164 | window created behind others; tools/gameplay_bench.py sets it for benchmarks |
| `SOTC_WRAP` | value | none | sotc.py:689 | JSON command prefix to launch the game under a wrapper such as RenderDoc (sotc.py:686) |
| `SOTC_WRITE_DECL` | off | none | src/runtime.c:179 | declared-writes histogram mode (1 or 2); used by docs/re-stutters/native/impl-writes7.md:34 for caller analysis |
| `SOTC_WRITE_DECL_MAX` | value | none | gpu/sotc/write_decl.cpp:188 | size cap of declared writes, default 1 MiB; diagnostic with SOTC_WRITE_DECL |
| `SOTC_WRITE_DECL_MIN` | value | none | src/runtime.c:180 | size floor of declared writes, default 256 bytes; used with SOTC_WRITE_DECL=2 in impl-writes7.md |
| `SOTC_WRITE_JOURNAL` | off | sotc.ini | gpu/shadps4/video_core/sotc_write_journal.h:36 | write journal diagnostic (1 or 2); sotc.ini explicitly sets 0; unsure, kept |
| `SOTC_WRITE_TRACE` | off | none | absent@18a1251; upstream/main:src/probe.c:1067 | upstream-only: records the CPU write faults inside a range (diagnostic, off) |
| `SOTC_XPR_GROUPS` | off | none | gpu/sotc/probe_xpr_groups.cpp:172 | observe-only P0 hook of the XPR draw loop (scripts/hooks.d/xpr_groups.py); unsure, may still serve the draw plan |
