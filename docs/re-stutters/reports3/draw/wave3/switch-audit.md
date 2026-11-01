# Switch audit: every environment switch of the port (wave 3, draw/stutter)

Status: uncommitted report. Written by the switchaudit agent on the `re/stutter-architecture` working tree. No code was edited, built or run, and nothing was committed.

## Method

- Index: every token `SOTC_*`, `BB_*`, `SHADPS4_*` in `src/`, `gpu/` (including the vendored shadPS4 tree and `sotc:` edits), `scripts/` (hooks.d included), `launcher/`, `tools/`, `tests/`, `packaging/`, `sotc.py`. `.claude/worktrees/*` copies are excluded. 676 names.
- Readers: a line that quotes the name (`getenv`, `sotc_switch("...")`, `SotcLive::Int{"..."}`, `SotcStats` constants and their helpers) is a reader. The reader and its default come from that line plus the three after it.
- Defaults: inferred from the reader text (`off; =1 on`, `on; =0 off`, `presence`, `value`), then checked against the launcher kind (`flag` / `flag_on`, `launcher/sotc_tuning.py`) and `sotc.py` setdefault. Inferred defaults are marked; **the default of every row must be confirmed before a deletion** (see Limits).
- Hook switches: `scripts/hooks.d/*.py` `switch=` fields, read by the hook engine (`gpu/sotc/hooks.cpp`, `ReadPresence`).
- Evidence: `CHANGELOG.md`, `README.md`, `docs/**` (PC-MEMORY-REWRITE.md section 8, HANDOFF.md, reports3/draw/switches-review.md, LAB docs). "docs" below is the first document that names the switch.
- Native model: per the user decision, every switch that exists only to tune the UMA emulation (sync, protect, readback, ring, class, fault grace, untrack, hot pages, memory release, host import, heap guard) is DELETE-AT-NATIVE, with the code it removes.

## Decisions applied (from the coordinator)

1. The vendored shadPS4 switches are included (384 of 676 names read from `gpu/shadps4`). Vendored options with no port setter and no docs are DELETE-UNUSED.
2. SOTC_DROP_FAST goes OFF by default. Needed change, not made here: `gpu/shadps4/video_core/amdgpu/liverpool.h:367` reads it as "on unless =0" (change to "on only when =1"), and the launcher row in `launcher/sotc_tuning.py` (kind `flag_on` becomes `flag`). The migration entry `tune_migrated_20261030b` (DEFAULT_ON_MIGRATIONS) moved it to on on 30 Oct; a new migration is needed so saved launcher settings do not keep the old value. `docs/re-stutters/HANDOFF.md` already says "stays off by default", so the docs are right and the code is stale.
3. Diagnostics are consolidated into three switches: **SOTC_FPS** (reports and counters, printed with the frame rate), **SOTC_TRACE** (timeline, `.sotctrace`), **SOTC_VERIFY** (every `=2` verify mode; not yet a single switch, it must be created by renaming or aliasing the existing verify switches). Each diagnostic is MERGE-INTO-FPS, MERGE-INTO-TRACE, MERGE-INTO-VERIFY, or deleted with its family.
4. The native memory model: DELETE-AT-NATIVE rows are the page-tracking, fault-readback, region-compare and tracking-ring families.

## Class counts

| Class | Count | Meaning |
|---|---:|---|
| KEEP-DEFAULT-ON | 50 | on by default (code or launcher flag_on); part of the product |
| KEEP-USER-OPTION | 70 | exposed in the launcher (Display and FPS page) or set by it |
| KEEP-INFRA | 173 | paths, config, build, Lab and launcher plumbing, DLSS/FG options |
| KEEP-CONSOLIDATED | 2 | the three targets (SOTC_FPS, SOTC_TRACE; SOTC_VERIFY to create) |
| MERGE-INTO-FPS | 28 | report counters and printers, folded into SOTC_FPS |
| MERGE-INTO-TRACE | 46 | timeline and probes, folded into SOTC_TRACE |
| MERGE-INTO-VERIFY | 4 | verify and compare modes, folded into SOTC_VERIFY |
| DELETE-AT-NATIVE | 158 | UMA emulation (page tracking, readbacks, rings, classes, fault grace, untrack); deleted with the native model |
| DELETE-NO-GAIN | 6 | measured without gain (switches-review.md, PC-MEMORY section 8) |
| DELETE-BROKEN | 3 | measured broken or unsafe, or measurement only with a wrong picture |
| DELETE-SUPERSEDED | 1 | alias or replaced by a newer switch |
| DELETE-UNUSED | 39 | vendored option the port never sets and no doc mentions |
| DELETE-DEAD | 5 | name only in comments, constants, tests or examples |
| UNKNOWN-MEASURE | 82 | no measured result; measurement item listed in the table notes |
| UNKNOWN-DECISION | 9 | native allocator candidates (PC_POOL_NATIVE family); decision needed |
| **total** | **676** | |

Deletable in total (all DELETE-* classes): **212**. Of which DELETE-AT-NATIVE 158, DELETE-UNUSED 39. Mergeable diagnostics: 78. Vendored (read from `gpu/shadps4`): 384.

## Top 30 deletions by lines removed (estimate)

Estimate = lines of the dedicated source files of the family (listed in the code) plus the lines that quote the switches (reader and guarded blocks are only partly counted). It is a ranking aid, not a diff. Shared files (`buffer_cache.cpp`, `liverpool.cpp`, `vk_rasterizer.cpp`, `runtime_memory.c`) hold most UMA code and are underestimated here.

| # | Family | Switches | Class(es) | Est. lines | Dedicated files |
|---:|---|---:|---|---:|---|
| 1 | PC_RING arena ring (modes 1-8, RING8 family, pcring hook) | 15 | DELETE-AT-NATIVE 15 | 3389 | gpu/sotc/pc_ring.cpp, .h; gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp |
| 2 | CLASS_TRACK (CPU-only pools) and CLASS_* options | 4 | DELETE-AT-NATIVE 4 | 2046 | gpu/sotc/mem_classes.cpp, .h |
| 3 | MEMTYPE_CLASSES (per-heap granules) | 2 | DELETE-AT-NATIVE 2 | 759 | gpu/sotc/probe_memtype.cpp |
| 4 | ALLOC_PROBE hook (PC_RING dependency) | 1 | DELETE-AT-NATIVE 1 | 692 | gpu/sotc/probe_alloc.cpp |
| 5 | ARENA_ATTRIB (arena writer attribution) | 1 | DELETE-AT-NATIVE 1 | 653 | gpu/sotc/arena_attrib.cpp, .h |
| 6 | RB_CLASS (XPR mask readback serving) | 2 | DELETE-BROKEN 1, DELETE-AT-NATIVE 1 | 539 | gpu/sotc/rb_class.cpp, .h |
| 7 | HRR (host resident readback ranges) | 3 | DELETE-AT-NATIVE 3 | 494 | gpu/sotc/rb_host.cpp, .h (assumed) |
| 8 | SOTC_RB (readbacks) | 32 | DELETE-AT-NATIVE 31, DELETE-BROKEN 1 | 494 | (shared files only) |
| 9 | PARTICLE_PREV (particle count reads) | 1 | DELETE-AT-NATIVE 1 | 446 | gpu/sotc/particle_prev.cpp |
| 10 | STATIC_CLASS (static mesh and texture clean ranges) | 1 | DELETE-AT-NATIVE 1 | 441 | gpu/sotc/static_class.cpp, .h |
| 11 | XPRMEM probe | 1 | DELETE-AT-NATIVE 1 | 438 | gpu/sotc/probe_xprmem.cpp |
| 12 | WRITE_DECL (write declarations) | 3 | DELETE-AT-NATIVE 3 | 398 | gpu/sotc/write_decl.cpp, .h |
| 13 | STALE_CHECK (hash verify of uploaded pages) | 1 | DELETE-AT-NATIVE 1 | 368 | gpu/sotc/stale_check.cpp, .h |
| 14 | WRITE_EXTENT (narrowed write ranges) | 2 | DELETE-AT-NATIVE 2 | 283 | gpu/shadps4/video_core/sotc_write_extent.h |
| 15 | XPR probe | 1 | DELETE-AT-NATIVE 1 | 207 | gpu/sotc/probe_xpr.cpp |
| 16 | RT_CLASS (render targets not tracked) | 1 | DELETE-NO-GAIN 1 | 203 | gpu/sotc/rt_class.cpp, .h |
| 17 | SOTC_GEN (generation stamps) | 3 | DELETE-AT-NATIVE 3 | 90 | (shared files only) |
| 18 | SOTC_PROTECT (page protection) | 6 | DELETE-AT-NATIVE 6 | 63 | (shared files only) |
| 19 | SOTC_UNMAP_HASH_FENCE | 7 | DELETE-AT-NATIVE 7 | 60 | (shared files only) |
| 20 | SOTC_UNTRACK | 4 | DELETE-AT-NATIVE 4 | 60 | (shared files only) |
| 21 | SOTC_WRITE (write tracking) | 7 | DELETE-AT-NATIVE 7 | 52 | (shared files only) |
| 22 | SOTC_SYNC (memory sync) | 4 | DELETE-AT-NATIVE 4 | 50 | (shared files only) |
| 23 | SOTC_HOST_IMPORT | 3 | DELETE-AT-NATIVE 3 | 44 | (shared files only) |
| 24 | SOTC_DROP (memory release) | 4 | DELETE-AT-NATIVE 4 | 40 | (shared files only) |
| 25 | SOTC_HOT_PAGES | 6 | DELETE-AT-NATIVE 6 | 30 | (shared files only) |
| 26 | SOTC_CARVE_VIEW (memory split and views) | 6 | DELETE-AT-NATIVE 6 | 27 | (shared files only) |
| 27 | SOTC_HEAP_GUARD_LOCK | 9 | DELETE-AT-NATIVE 9 | 25 | (shared files only) |
| 28 | SOTC_PRESENT_UNLOCKED | 1 | DELETE-NO-GAIN 1 | 24 | (shared files only) |
| 29 | SOTC_FAULT (fault grace and granule) | 5 | DELETE-AT-NATIVE 5 | 23 | (shared files only) |
| 30 | SOTC_HLE_TABLE_GEN | 1 | DELETE-NO-GAIN 1 | 20 | (shared files only) |

## Consolidation targets

- **SOTC_FPS** (28 merge): `BB_DUMP_SHADERS`, `BB_FSR4_STATS`, `SOTC_CAM_LOG`, `SOTC_CENSUS`, `SOTC_CMD_STATS`, `SOTC_CS_DUMP`, `SOTC_CS_STATS`, `SOTC_CS_WATCH`, `SOTC_DLSSG_LOG`, `SOTC_DUMP`, `SOTC_FPS_LEAN`, `SOTC_FRAME_LOG`, `SOTC_FSRFG_LOG`, `SOTC_GLYPH_LOG`, `SOTC_HOOKS_LOG`, `SOTC_HOOK_PSTATS`, `SOTC_LOD_POOL_LOG`, `SOTC_LOD_STATS`, `SOTC_MDI_STATS`, `SOTC_OPSTAT`, `SOTC_OP_PHASES`, `SOTC_POOL_WATCH`, `SOTC_RAM_REPORT`, `SOTC_SHADOW_REUSE_STATS`, `SOTC_SHOT_COUNT`, `SOTC_STALL_DETAIL`, `SOTC_TEX_DUMP`, `SOTC_TRACE_SAMPLE`

- **SOTC_TRACE** (46 merge): `BB_FSR4_PROFILE`, `SOTC_BUILDSYNC_PROBE`, `SOTC_CAM_PROBE`, `SOTC_CS_TRACE`, `SOTC_DLSS_PROBE`, `SOTC_DRAW_TAG`, `SOTC_DT_PROBE`, `SOTC_EVF_TRACE`, `SOTC_FIBER_STACKS`, `SOTC_FLIP_TRACE`, `SOTC_HOOK_CVAR_PROBE`, `SOTC_HOOK_HEAP_RANGE`, `SOTC_HOOK_LOADER_NOSTREAM`, `SOTC_HOOK_LOADER_PROBE`, `SOTC_INDIRECT_TRACE`, `SOTC_LOD_TRACE`, `SOTC_MENU_TRIGGER`, `SOTC_PASS_TRACE`, `SOTC_PROFILE`, `SOTC_PROFILE_DELAY`, `SOTC_PROFILE_OUT`, `SOTC_PROFILE_SECONDS`, `SOTC_RDOC_SUBMITS`, `SOTC_RDOC_TRIGGER`, `SOTC_SAVE_TRACE`, `SOTC_SET_TRIGGER`, `SOTC_THREAD_IDS`, `SOTC_TRACE_DIR`, `SOTC_TRACE_FUNCS`, `SOTC_TRACE_FUNC_MIN_US`, `SOTC_TRACE_GPU`, `SOTC_TRACE_IMPORTS`, `SOTC_TRACE_IMPORT_MIN_US`, `SOTC_TRACE_JSON`, `SOTC_TRACE_MAX`, `SOTC_TRACE_MS`, `SOTC_TRACE_ONLY`, `SOTC_TRACE_POST_MS`, `SOTC_TRACE_PRE_MS`, `SOTC_TRACE_RING`, `SOTC_TRACE_SAMPLE_GUEST`, `SOTC_TRACE_SCOPES`, `SOTC_TRACE_ZONES`, `SOTC_TRACE_ZONE_MIN_US`, `SOTC_TSHARP_TRACE`, `SOTC_UI_LINEAR_TRACE`

- **SOTC_VERIFY** (4 merge): `SOTC_BIND_DIFF`, `SOTC_PC_POOL`, `SOTC_PR_VERIFY`, `SOTC_REUSE_SIZE_CHECK` (none of these is a single verify switch yet; SOTC_PC_POOL=2 and SOTC_PR_VERIFY are the closest.)

## Required changes outside this report (not made)

- `gpu/shadps4/video_core/amdgpu/liverpool.h:367`: SOTC_DROP_FAST default to off.
- `launcher/sotc_tuning.py`: SOTC_DROP_FAST kind to `flag` (off), and a `DEFAULT_ON_MIGRATIONS` entry is no longer needed for it.
- `sotc.py`: SOTC_PC_RING default 8 (setdefault), with SOTC_HOOK_ALLOC_PROBE and SOTC_HOOK_FRAME_FLIP set with it. Deleting the native families also removes 65 launcher rows from `TUNING` (of 171 rows), and the matching `LIVE` entries if any.
- The `.claude/worktrees/*` copies still contain the old readers; they are not part of the audit.

## Dependencies to respect when deleting

- `SOTC_RB_CLASS=2` needs `SOTC_RB_OWNER=1`, `SOTC_RB_MATERIALIZE=1`, `SOTC_HOOK_ALLOC_PROBE=1`, `SOTC_PARTICLE_PREV=1` (PC-MEMORY section 8).
- `SOTC_HLE_TABLE_GEN` needs the generation stamps (`SOTC_SYNC_FAST` or `SOTC_BUF_BIND_FAST`), and is inert without them (switches-review.md). `SOTC_BUF_BIND_FAST` is an alias of `SOTC_SYNC_FAST`.
- `SOTC_PC_RING` needs `SOTC_HOOK_ALLOC_PROBE` (arena blocks) and `SOTC_HOOK_FRAME_FLIP` (flip wait); `SOTC_HOOK_MEMTYPE_PROBE` conflicts with PC_RING on the same hook site (section 8).
- Hook switches are read by the hook engine (`gpu/sotc/hooks.cpp`, `ReadPresence`); the engine-level gate `SOTC_HOOKS` was not traced in this audit.
- `SOTC_TRACE_SCOPES` installs its hook only under its switch (`gpu/sotc/hook_handlers.cpp:43`); the same pattern for `SOTC_TRACE_FUNCS` and `SOTC_TRACE_IMPORTS` was not verified.
- `SOTC_CULL_EXPRESS_*` sub-switch behaviour when `SOTC_CULL_EXPRESS=0` was not verified.

## Full table

Columns: class; default (`inf` = inferred from code text, to confirm); reader file:line (`-` = no quoted reader found, see Limits); launcher row (kind/group); vendored (read from gpu/shadps4); setters outside the code (launcher, sotc.py, tools, tests, scripts); first doc mention; note.

| Switch | Class | Default | Reader | Launcher | Vendored | Setters | Docs | Note |
|---|---|---|---|---|---|---:|---:|---|
| `BB_ASYNC_FENCES` | DELETE-AT-NATIVE | no reader | - | - | - | 0 | 0  | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_ATRAC9` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_AUDIO` | KEEP-INFRA | off (set enables) (inf) | src/runtime_audio.c:66 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_AUDIO_DUMP` | KEEP-INFRA | off (set enables) (inf) | src/runtime_audio.c:125 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_AUDIO_STATS` | KEEP-INFRA | off (set enables) (inf) | src/runtime_audio.c:124 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_AUDIO_TRACE` | KEEP-INFRA | off (1 enables) (inf) | src/runtime_ajm.c:80 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_BUNDLED_VK_DRIVER_FILES` | KEEP-INFRA | no reader | - | - | - | 4 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_COMPAT_WIN_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_CONFIG` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:236 | - | - | 4 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_COPY_GPU_BUFFERS` | DELETE-AT-NATIVE | check (inf) | gpu/shim/core/emulator_settings.h:58 | - | - | 0 | 4 docs/re-stutters/reports/06-render-frame.md:77 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_COPY_THREADS` | DELETE-AT-NATIVE | value | gpu/shim/bbport_copy.cpp:55 | - | - | 0 | 0  | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_DATA_DIR` | KEEP-INFRA | no reader | - | - | - | 3 | 1 docs/history/SESSION_2026-09-30.md:191 | paths, config, build, launcher or Lab plumbing |
| `BB_DIRECT_MEMORY_ACCESS` | DELETE-AT-NATIVE | check (inf) | gpu/shim/core/emulator_settings.h:66 | - | - | 1 | 2 CHANGELOG.md:1168 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_DMEM_MB` | KEEP-INFRA | value | src/runtime_memory.c:37 | - | - | 2 | 3 docs/history/SESSION_2026-09-30.md:133 | paths, config, build, launcher or Lab plumbing |
| `BB_DUMP_DIR` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1147 | - | yes | 5 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_DUMP_SHADERS` | MERGE-INTO-FPS | check (inf) | gpu/shim/core/emulator_settings.h:74 | - | - | 0 | 1 docs/sotc/AGUA.md:82 | report counters and printers |
| `BB_FAULT_RING` | DELETE-AT-NATIVE | off (set enables) (inf) | src/probe.c:1703 | - | - | 0 | 1 docs/sotc/README-SotC.md:154 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_FAULT_WINDOW` | DELETE-AT-NATIVE | no reader | - | - | - | 0 | 1 docs/history/parallel_gpu.md:94 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_FONT_PATH` | KEEP-INFRA | no reader | - | - | - | 0 | 1 docs/re-stutters/reports3/draw/wave1/l1.md:153 | paths, config, build, launcher or Lab plumbing |
| `BB_FPS_LIMIT` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:52 | - | - | 0 | 6 docs/history/parallel_gpu.md:113 | paths, config, build, launcher or Lab plumbing |
| `BB_FRAME_DUMP_TRIGGER` | KEEP-INFRA | value | gpu/shim/bbgpu.cpp:762 | - | - | 6 | 2 docs/re-stutters/HANDOFF.md:89 | paths, config, build, launcher or Lab plumbing |
| `BB_FRAME_STATS` | KEEP-INFRA | off (1 enables) (inf) | gpu/shim/bbport_toggles.h:97 | - | - | 1 | 6 docs/history/CURRENT_STATUS_2026-09-29.md:64 | paths, config, build, launcher or Lab plumbing |
| `BB_FSR1` | KEEP-INFRA | on (0 disables) (inf) | gpu/shim/bbport_overlay.cpp:556 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_FSR4CAP_SHELL` | UNKNOWN-MEASURE | no reader | - | - | - | 2 | 0  | documented or set by tools only; no measured result found |
| `BB_FSR4_DIR` | UNKNOWN-MEASURE | no reader | - | - | - | 1 | 1 docs/history/CURRENT_STATUS_2026-09-29.md:199 | documented or set by tools only; no measured result found |
| `BB_FSR4_OPT` | UNKNOWN-MEASURE | no reader | - | - | - | 1 | 0  | documented or set by tools only; no measured result found |
| `BB_FSR4_PROFILE` | MERGE-INTO-TRACE | no reader | - | - | - | 0 | 2 docs/original-readme/README.md:210 | timeline and trace spans |
| `BB_FSR4_STATS` | MERGE-INTO-FPS | no reader | - | - | - | 0 | 0  | report counters and printers |
| `BB_FSR4_TIERS` | UNKNOWN-MEASURE | no reader | - | - | - | 2 | 0  | documented or set by tools only; no measured result found |
| `BB_FSR_SHARPNESS` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:120 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_FULLSCREEN` | KEEP-INFRA | check (inf) | gpu/shim/bbport_settings.cpp:125 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_GAMEPAD` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:119 | - | - | 2 | 1 docs/sotc/README-SotC.md:76 | paths, config, build, launcher or Lab plumbing |
| `BB_GPU_ID` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/core/emulator_settings.h:27 | - | - | 3 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_GPU_LOG` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/common/logging/log.cpp:11 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_GPU_USER_DIR` | KEEP-INFRA | off (set enables) (inf) | src/runtime_file.c:267 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_HDR` | UNKNOWN-MEASURE | check (inf) | gpu/shim/core/emulator_settings.h:76 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_HOT_PAGES` | DELETE-AT-NATIVE | no reader | - | - | - | 0 | 1 docs/history/parallel_gpu.md:73 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_IME_TEXT` | KEEP-INFRA | off (set enables) (inf) | src/runtime_services.c:618 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_INTERNAL_HEIGHT` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/core/emulator_settings.h:29 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_INTERNAL_WIDTH` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/core/emulator_settings.h:28 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_JITTER` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:120 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_LANGUAGE` | KEEP-INFRA | value | src/runtime_services.c:75 | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_LIST_IMPORTS` | KEEP-INFRA | check (inf) | src/probe.c:1903 | - | - | 1 | 1 docs/re-stutters/reports3/12-poolflex-hang.md:92 | paths, config, build, launcher or Lab plumbing |
| `BB_LTO` | KEEP-INFRA | no reader | - | - | - | 0 | 1 docs/history/parallel_gpu.md:241 | paths, config, build, launcher or Lab plumbing |
| `BB_MEMORY_LOG` | KEEP-INFRA | off (set enables) (inf) | src/runtime_memory.c:1564 | - | - | 0 | 1 docs/re-stutters/reports/04-memory-model.md:108 | paths, config, build, launcher or Lab plumbing |
| `BB_MIMALLOC` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_MODS_DIR` | KEEP-INFRA | off (1 enables) (inf) | src/runtime_file.c:328 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_MOD_TRACE` | KEEP-INFRA | off (1 enables) (inf) | src/runtime_file.c:328 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_NULL_GPU` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:77 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_NVIDIA_LIB_DIR` | KEEP-INFRA | no reader | - | - | - | 2 | 2 docs/original-readme/README.md:194 | paths, config, build, launcher or Lab plumbing |
| `BB_OBJECT_MOTION` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:123 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_OVERLAY_MENU` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/bbport_overlay.cpp:131 | - | - | 0 | 2 CHANGELOG.md:1008 | paths, config, build, launcher or Lab plumbing |
| `BB_PAD_FILE` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:439 | - | - | 8 | 2 docs/history/SESSION_2026-09-30.md:209 | paths, config, build, launcher or Lab plumbing |
| `BB_PAD_RECORD` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:495 | - | - | 4 | 3 docs/original-readme/README.md:214 | paths, config, build, launcher or Lab plumbing |
| `BB_PAD_REPLAY` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:529 | - | - | 2 | 3 docs/original-readme/README.md:214 | paths, config, build, launcher or Lab plumbing |
| `BB_PGO` | KEEP-INFRA | no reader | - | - | - | 1 | 2 docs/history/parallel_gpu.md:238 | paths, config, build, launcher or Lab plumbing |
| `BB_PGO_CLANG` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_PGO_DIR` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_PGO_GENERATE` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_PIPELINE_CACHE` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:81 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_PRESENT_MODE` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/core/emulator_settings.h:30 | - | - | 1 | 5 docs/ARCHITECTURE-TARGET.md:116 | paths, config, build, launcher or Lab plumbing |
| `BB_RCAS` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/bbport_overlay.cpp:586 | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_RCAS_ATTENUATION` | KEEP-INFRA | value | gpu/shim/bbport_overlay.cpp:590 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_REACTIVE` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:121 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_REACTIVE_MAX` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:122 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_REACTIVE_SCALE` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:121 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_REACTIVE_THRESHOLD` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:122 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `BB_READBACKS` | DELETE-AT-NATIVE | check (inf) | gpu/shim/core/emulator_settings.h:36 | - | - | 1 | 3 docs/re-stutters/reports2/16-job-system.md:36 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_READBACK_LINEAR` | DELETE-AT-NATIVE | check (inf) | gpu/shim/core/emulator_settings.h:83 | - | - | 0 | 0  | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_RENDER_RES` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/bbport_settings.cpp:171 | - | - | 4 | 4 docs/history/CURRENT_STATUS_2026-09-29.md:77 | paths, config, build, launcher or Lab plumbing |
| `BB_RESTART_COMMAND` | KEEP-INFRA | off (set enables) (inf) | src/probe.c:1591 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_RUNTIME` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_RUNTIME_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_RUNTIME_MEMORY_CHUNKS_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_STACKS_TRIGGER` | KEEP-INFRA | check (inf) | src/probe.c:1704 | - | - | 3 | 1 docs/sotc/README-SotC.md:155 | paths, config, build, launcher or Lab plumbing |
| `BB_STATIC_LIBCXX` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_STRICT_HANDLES` | KEEP-INFRA | check (inf) | src/probe.c:1709 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_TOGGLE_FILE` | KEEP-INFRA | off (set enables) (inf) | src/runtime_memory.c:1797 | - | - | 2 | 6 docs/history/CURRENT_STATUS_2026-09-29.md:65 | paths, config, build, launcher or Lab plumbing |
| `BB_TRACE_SEMA` | KEEP-INFRA | check (inf) | src/runtime_sema.c:77 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_UFFD` | DELETE-AT-NATIVE | no reader | - | - | - | 1 | 1 docs/history/parallel_gpu.md:95 | UMA emulation (BB_ engine switch for host page tracking or copies) |
| `BB_UPSCALER` | KEEP-INFRA | check (inf) | gpu/shim/bbport_settings.cpp:113 | - | - | 1 | 3 docs/history/CURRENT_STATUS_2026-09-29.md:195 | paths, config, build, launcher or Lab plumbing |
| `BB_UPSCALE_PRESET` | UNKNOWN-MEASURE | check (inf) | gpu/shim/bbport_settings.cpp:123 | - | - | 1 | 0  | documented or set by tools only; no measured result found |
| `BB_USER_NAME` | KEEP-INFRA | check (inf) | src/runtime_services.c:60 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_VBLANK_HZ` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:44 | - | - | 7 | 4 docs/re-stutters/reports2/12-flip-present.md:33 | paths, config, build, launcher or Lab plumbing |
| `BB_VK_MARKERS` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:88 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_VK_VALIDATION` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:91 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_VK_VALIDATION_SYNC` | KEEP-INFRA | check (inf) | gpu/shim/core/emulator_settings.h:93 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_VULKANINFO` | KEEP-INFRA | no reader | - | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `BB_WRITE_LOG` | KEEP-INFRA | value | gpu/shim/bbport_write_log.cpp:42 | - | - | 0 | 7 docs/history/parallel_gpu.md:556 | paths, config, build, launcher or Lab plumbing |
| `SHADPS4_ABSOLUTE_INSTANCE_ID` | DELETE-UNUSED | check (inf) | gpu/shadps4/shader_recompiler/frontend/translate/translate.cpp:28 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SHADPS4_IEEE_MINMAX` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp:16 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:41 | documented or set by tools only; no measured result found |
| `SHADPS4_LOOP_LIMIT` | DELETE-UNUSED | value | gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp:171 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SHADPS4_LOOP_LIMIT_PER_LOOP` | DELETE-UNUSED | value | gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp:190 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SHADPS4_UNBOUNDED_BUFFER_MB` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2856 | - | yes | 0 | 1 docs/re-stutters/HANDOFF.md:75 | documented or set by tools only; no measured result found |
| `SOTC_AB_PR` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1608 | - | yes | 0 | 1 docs/DRAW-THREAD-7C-DESIGN.md:10 | documented or set by tools only; no measured result found |
| `SOTC_ACCESS_REUSE` | DELETE-UNUSED | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp:112 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_ACCESS_UNION` | DELETE-UNUSED | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp:113 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_ADAPTIVE_DEFER` | KEEP-DEFAULT-ON | on | gpu/shadps4/video_core/sotc_adaptive.h:73 | flag_on/threads | yes | 1 | 1 CHANGELOG.md:609 | on in the code or the launcher without a user action (default read from code) Defer background work during relief |
| `SOTC_ADAPTIVE_FLUSH_US` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_adaptive.h:75 | value/threads | yes | 1 | 1 CHANGELOG.md:608 | exposed in the launcher Submit interval during relief (us) |
| `SOTC_ADAPTIVE_HIGH_MS` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_adaptive.h:100 | value/present | yes | 1 | 1 CHANGELOG.md:601 | exposed in the launcher Slow frame time that starts relief (ms) |
| `SOTC_ADAPTIVE_HOLD_MS` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_adaptive.h:105 | value/present | yes | 1 | 1 CHANGELOG.md:603 | exposed in the launcher Wait before ending relief (ms) |
| `SOTC_ADAPTIVE_LOAD` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_adaptive.h:68 | flag/present | yes | 1 | 1 CHANGELOG.md:600 | exposed in the launcher Adaptive load control (experimental) |
| `SOTC_ADAPTIVE_LOW_MS` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_adaptive.h:103 | value/present | yes | 1 | 1 CHANGELOG.md:602 | exposed in the launcher Frame time to end relief (ms) |
| `SOTC_ADAPTIVE_SHADOW` | KEEP-DEFAULT-ON | on | gpu/shadps4/video_core/sotc_adaptive.h:72 | flag_on/bind | yes | 1 | 2 CHANGELOG.md:606 | on in the code or the launcher without a user action (default read from code) Alternate distant shadows during relief |
| `SOTC_AMD64_LDS` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:377 | - | yes | 0 | 3 CHANGELOG.md:954 | documented or set by tools only; no measured result found |
| `SOTC_AREA_PREWARM` | UNKNOWN-MEASURE | check (inf) | gpu/sotc/area_prewarm.cpp:390 | - | - | 5 | 8 docs/re-stutters/FIRST-FRAME-PEAK.md:54 | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_AREA_PREWARM_MB` | UNKNOWN-MEASURE | check (inf) | gpu/sotc/area_prewarm.cpp:399 | - | - | 0 | 1 docs/re-stutters/reports2/08-shader-first-use.md:167 | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_ARENA_EVICT_IDLE_S` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9035 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ARENA_EVICT_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9027 | - | yes | 1 | 2 docs/sotc/ROADMAP.md:15 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ARENA_EVICT_STEP_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9031 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ARENA_GROUP` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7210 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ARENA_RELEASE` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:7278 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ARENA_TICK_FREE` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8514 | flag/memory | yes | 1 | 3 CHANGELOG.md:566 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Reuse video memory only after the GPU |
| `SOTC_ARENA_ATTRIB` | DELETE-AT-NATIVE | value | gpu/sotc/arena_attrib.cpp:379 | - | - | 3 | 1 docs/re-stutters/HANDOFF.md:24 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_ASC_RPTR` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:5264 | choice/threads | yes | 1 | 2 docs/re-stutters/reports2/01-cmdbuf-lifetime.md:76 | exposed in the launcher Compute queue read pointer mode |
| `SOTC_ASYNC_COMPUTE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:356 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_ASYNC_COMPUTE_XLATE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:548 | flag/threads | yes | 1 | 2 CHANGELOG.md:649 | exposed in the launcher Translate compute shaders in parallel |
| `SOTC_ASYNC_IMAGE_CREATE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:59 | flag/upload | yes | 1 | 6 CHANGELOG.md:632 | exposed in the launcher Create textures ahead in parallel |
| `SOTC_ASYNC_JOB_MB` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:56 | value/upload | yes | 1 | 1 CHANGELOG.md:329 | exposed in the launcher Max upload job size (MiB) |
| `SOTC_ASYNC_PRELOAD` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:773 | - | yes | 0 | 3 CHANGELOG.md:951 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_ASYNC_PRESENT` | DELETE-UNUSED | off (1 enables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:572 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_ASYNC_SHADERS` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:336 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_ASYNC_STAGE_CACHED` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:54 | flag/upload | yes | 1 | 1 CHANGELOG.md:330 | exposed in the launcher Upload staging in cached memory |
| `SOTC_ASYNC_TEXTURE_UPLOAD` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:47 | flag/upload | yes | 1 | 2 CHANGELOG.md:731 | exposed in the launcher Async copy for texture uploads |
| `SOTC_ASYNC_TRANSLATE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:346 | - | yes | 0 | 1 docs/sotc/PLANO-THREAD-DESENHO.md:385 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_ASYNC_UPLOAD` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:46 | flag/upload | yes | 1 | 5 CHANGELOG.md:230 | exposed in the launcher Async copy for buffer uploads |
| `SOTC_ASYNC_UPLOAD_MIN_KB` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:64 | value/upload | yes | 1 | 1 CHANGELOG.md:754 | exposed in the launcher Minimum size for async copy (KiB) |
| `SOTC_ASYNC_UPLOAD_THREADS` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:63 | value/upload | yes | 1 | 1 CHANGELOG.md:755 | exposed in the launcher Async upload copy threads |
| `SOTC_BACKING_WARM` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shim/sotc_backing_warm.cpp:24 | - | - | 0 | 3 CHANGELOG.md:32 | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_BACK_HEAD_CACHE` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:255 | flag/threads | yes | 1 | 2 CHANGELOG.md:303 | exposed in the launcher Cache the start of the operation ring |
| `SOTC_BACK_PF` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:256 | flag/threads | yes | 1 | 2 CHANGELOG.md:303 | exposed in the launcher Preload the next ring operation |
| `SOTC_BARYCENTRIC_GS` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:403 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:60 | documented or set by tools only; no measured result found |
| `SOTC_BATCH_SPLIT` | KEEP-DEFAULT-ON | on (0 disables) (inf) | src/runtime_memory.c:1644 | - | - | 0 | 3 CHANGELOG.md:808 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_BDA` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:2160 | - | yes | 1 | 25 docs/re-stutters/HANDOFF.md:159 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_BINDLESS_SAMP` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:843 | flag/bind | yes | 2 | 4 CHANGELOG.md:573 | exposed in the launcher Bindless samplers (global heap) |
| `SOTC_BIND_CACHE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:101 | flag/bind | yes | 3 | 6 CHANGELOG.md:250 | exposed in the launcher Bind cache for read-only draws |
| `SOTC_BIND_CACHE_FRAME` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_stats.h:165 | flag/bind | yes | 1 | 5 CHANGELOG.md:283 | exposed in the launcher Bind cache that lasts the whole frame |
| `SOTC_BIND_CACHE_RANGES` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:123 | flag/bind | yes | 1 | 2 CHANGELOG.md:487 | exposed in the launcher Drop binds only for written ranges |
| `SOTC_BIND_CACHE_XTICK` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/sotc_stats.h:143 | flag/bind | yes | 1 | 4 CHANGELOG.md:414 | exposed in the launcher Bind cache that crosses submits |
| `SOTC_BIND_CLAMP1` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5428 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_BIND_DIFF` | MERGE-INTO-VERIFY | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2987 | flag/diag | yes | 2 | 2 CHANGELOG.md:280 | verify or compare mode Diagnostic: why the bind shortcut misses |
| `SOTC_BIND_MARK_ONCE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5316 | flag/bind | yes | 1 | 4 CHANGELOG.md:316 | exposed in the launcher Mark bind writes only once |
| `SOTC_BININFO_CACHE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/amdgpu/sotc_bininfo.cpp:34 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_BININFO_L1` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/amdgpu/sotc_bininfo.cpp:57 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_BOOT_BOSS` | KEEP-INFRA | no reader | - | - | - | 8 | 3 docs/sotc/LAB.md:150 | paths, config, build, launcher or Lab plumbing |
| `SOTC_BUFFER_MEMO` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:173 | flag/bind | yes | 1 | 5 CHANGELOG.md:283 | exposed in the launcher Buffer memo for the bind cache |
| `SOTC_BUF_BIND_FAST` | DELETE-SUPERSEDED | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:39 | - | yes | 4 | 8 docs/re-stutters/reports3/draw/switches-review.md:4 | alias of SOTC_SYNC_FAST; kOn never read |
| `SOTC_BUILDSYNC_PROBE` | MERGE-INTO-TRACE | value | gpu/sotc/probe_buildsync.cpp:71 | - | - | 3 | 0  | observe-only hook probe (timeline or counters) |
| `SOTC_CACHE_PACK` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/cache_storage.cpp:149 | - | yes | 0 | 2 docs/re-stutters/reports/07-first-use.md:50 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_CAM_LOG` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:482 | - | yes | 3 | 0  | report counters and printers |
| `SOTC_CAM_PROBE` | MERGE-INTO-TRACE | no reader | - | - | - | 2 | 5 docs/re-stutters/freecam-re.md:85 | observe-only hook probe (timeline or counters) |
| `SOTC_CARVE_LOG` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:667 | flag/diag | - | 1 | 2 CHANGELOG.md:721 | diagnostic of a UMA family: deleted with its family Diagnostic: log memory view splits |
| `SOTC_CARVE_NOSTOP` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:632 | flag/memory | - | 1 | 1 CHANGELOG.md:714 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Split memory without stopping the game |
| `SOTC_CARVE_SUSPEND` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:517 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_VIEW_CHUNK_KB` | DELETE-AT-NATIVE | value | src/runtime_memory.c:543 | value/memory | - | 1 | 2 CHANGELOG.md:223 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Memory view size (KiB) |
| `SOTC_VIEW_CHUNK_MB` | DELETE-AT-NATIVE | value | src/runtime_memory.c:544 | - | - | 0 | 5 CHANGELOG.md:222 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_VIEW_SPLIT_FINE` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:584 | flag/memory | - | 1 | 1 CHANGELOG.md:670 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Split memory views into fine pieces |
| `SOTC_CEILING` | DELETE-BROKEN | off (set enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:63 | choice/diag | yes | 1 | 1 CHANGELOG.md:285 | measurement only, the picture is wrong by design Measurement ceiling: skip draw-thread work |
| `SOTC_CENSUS` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3262 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_CLASS_MAX_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:273 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_CLASS_POOLS` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/mem_classes.cpp:440 | - | - | 7 | 2 docs/re-stutters/HANDOFF.md:51 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_CLASS_TRACK` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/mem_classes.cpp:1090 | - | - | 9 | 21 CHANGELOG.md:132 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_CLASS_WARMUP_S` | DELETE-AT-NATIVE | value | gpu/sotc/mem_classes.cpp:567 | - | - | 0 | 1 docs/re-stutters/HANDOFF.md:111 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_CLEAN_READS` | DELETE-UNUSED | value | gpu/shadps4/video_core/page_manager.cpp:1723 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_CMDLINE_RESERVE` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_CMD_STATS` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:31 | - | yes | 1 | 8 CHANGELOG.md:270 | report counters and printers |
| `SOTC_COMMIT_CHUNKS` | UNKNOWN-MEASURE | off (1 enables) (inf) | src/runtime_memory.c:979 | - | - | 2 | 5 CHANGELOG.md:125 | about 3 GB less committed in verify; not tracking; measure in the native model |
| `SOTC_COMMIT_ON_DEMAND` | UNKNOWN-MEASURE | off (1 enables) (inf) | src/runtime_memory.c:979 | - | - | 1 | 7 CHANGELOG.md:125 | about 3 GB less committed in verify; not tracking; measure in the native model |
| `SOTC_COPY_BATCH` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8211 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_COPY_GUEST_VIEW` | KEEP-DEFAULT-ON | on | gpu/shim/bbgpu.cpp:187 | flag_on/upload | - | 1 | 3 CHANGELOG.md:9 | on in the code or the launcher without a user action (default read from code) Uploads read through the game address |
| `SOTC_COPY_HLE_WHOLE` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:269 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_COPY_LOCK_ONCE` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shim/bbgpu.cpp:165 | - | - | 0 | 2 docs/sotc/PLANO-THREAD-DESENHO.md:189 | documented or set by tools only; no measured result found |
| `SOTC_CPUSETS` | KEEP-USER-OPTION | off (1 enables) (inf) | src/runtime_thread.c:317 | flag/threads | - | 1 | 10 CHANGELOG.md:566 | exposed in the launcher Pin GPU threads to fast cores |
| `SOTC_CP_AHEAD` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:5254 | choice/threads | yes | 1 | 27 CHANGELOG.md:361 | exposed in the launcher Let the command processor run ahead |
| `SOTC_CP_AHEAD_FRAMES` | KEEP-USER-OPTION | value | gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:155 | value/threads | yes | 1 | 9 docs/re-stutters/STUTTER-ARCHITECTURE.md:201 | exposed in the launcher Pending closed frames (mode 2) |
| `SOTC_CP_FLIP_PACE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:621 | flag/present | yes | 1 | 7 CHANGELOG.md:660 | exposed in the launcher Free-frame wait on the command processor |
| `SOTC_CP_FREE_NOW` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3355 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:90 | documented or set by tools only; no measured result found |
| `SOTC_CP_FREE_TASKS` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3306 | - | yes | 0 | 2 CHANGELOG.md:917 | documented or set by tools only; no measured result found |
| `SOTC_CP_IDLE_TIMEOUT_US` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:276 | value/threads | yes | 1 | 1 CHANGELOG.md:779 | exposed in the launcher Max command processor sleep (us) |
| `SOTC_CP_IDLE_US` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:273 | value/threads | yes | 1 | 1 CHANGELOG.md:777 | exposed in the launcher Sleep when the command processor waits (us) |
| `SOTC_CP_LABEL_SKIP` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:119 | - | yes | 0 | 11 docs/re-stutters/reports2/00-PLAN.md:34 | documented or set by tools only; no measured result found |
| `SOTC_CP_STRICT` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:244 | - | yes | 0 | 7 docs/ARCHITECTURE-TARGET.md:6 | documented or set by tools only; no measured result found |
| `SOTC_CP_THREADS` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:242 | - | yes | 0 | 5 docs/ARCHITECTURE-TARGET.md:6 | documented or set by tools only; no measured result found |
| `SOTC_CP_YIELD` | DELETE-UNUSED | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3024 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_CS_DUMP` | MERGE-INTO-FPS | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4910 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_CS_STATS` | MERGE-INTO-FPS | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1670 | flag/diag | yes | 2 | 3 CHANGELOG.md:262 | report counters and printers Diagnostic: repeated compute dispatches |
| `SOTC_CS_TRACE` | MERGE-INTO-TRACE | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2337 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_CS_WATCH` | MERGE-INTO-FPS | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2342 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_CTL` | KEEP-INFRA | on (0 disables) (inf) | gpu/sotc/ctl_server.cpp:42 | value/diag | - | 9 | 7 CHANGELOG.md:70 | paths, config, build, launcher or Lab plumbing SotC Lab control channel (pipe name) |
| `SOTC_CTL_LIVE` | KEEP-INFRA | no reader | - | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_CTL_PIPE` | KEEP-INFRA | no reader | - | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_CULL_EXPRESS` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1563 | flag/threads | yes | 1 | 4 CHANGELOG.md:413 | launcher label says incomplete; no measurement found Priority queue for culling (incomplete) |
| `SOTC_CULL_EXPRESS_ADDR` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:352 | value/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Forced label address (express cull) |
| `SOTC_CULL_EXPRESS_MARKS` | UNKNOWN-MEASURE | on | gpu/shadps4/video_core/amdgpu/liverpool.cpp:350 | flag_on/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Frame marks in express cull |
| `SOTC_CULL_EXPRESS_PATTERN` | UNKNOWN-MEASURE | on | gpu/shadps4/video_core/amdgpu/liverpool.cpp:351 | flag_on/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Recognize dispatch end in express cull |
| `SOTC_CULL_EXPRESS_SEG` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:353 | value/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Operations per express segment |
| `SOTC_CULL_EXPRESS_SIGNALS` | UNKNOWN-MEASURE | on | gpu/shadps4/video_core/amdgpu/liverpool.cpp:349 | flag_on/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Label signals in express cull |
| `SOTC_CULL_EXPRESS_SKIP` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:355 | value/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Operations that may run ahead |
| `SOTC_CULL_EXPRESS_US` | UNKNOWN-MEASURE | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:356 | value/threads | yes | 1 | 0  | launcher label says incomplete; no measurement found Max cull decision time (us) |
| `SOTC_CVARS` | KEEP-INFRA | no reader | - | - | - | 8 | 13 docs/BIND-PATH-DESIGN.md:37 | paths, config, build, launcher or Lab plumbing |
| `SOTC_DEBUG_MENU` | KEEP-INFRA | off (set enables) (inf) | src/runtime_file.c:256 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DECOMMIT_BUDGET` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/sotc/decommit_budget.cpp:36 | - | - | 3 | 0  | about 3 GB less committed in verify; not tracking; measure in the native model |
| `SOTC_DEDUP_PUSH` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:31 | flag/bind | yes | 1 | 1 CHANGELOG.md:565 | exposed in the launcher Skip repeated push constants |
| `SOTC_DEDUP_STATE` | KEEP-DEFAULT-ON | on | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:30 | flag_on/bind | yes | 2 | 1 CHANGELOG.md:561 | on in the code or the launcher without a user action (default read from code) Skip resending the vertex format |
| `SOTC_DEPTH_RO` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5694 | - | yes | 0 | 1 CHANGELOG.md:838 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_DEPTH_SYNC_PIPE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1449 | flag/bind | yes | 1 | 4 CHANGELOG.md:557 | exposed in the launcher Build shadow pipelines on the spot |
| `SOTC_DEPTH_TWIN` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:713 | - | yes | 0 | 2 docs/re-stutters/reports2/13-memory-aliasing.md:23 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_DESC_BUFFER` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:512 | flag/bind | yes | 2 | 4 CHANGELOG.md:566 | exposed in the launcher Descriptor buffers (Vulkan) |
| `SOTC_DESYNC_REPAIR` | DELETE-UNUSED | value | gpu/shadps4/video_core/page_manager.cpp:1291 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_DLC_DIR` | KEEP-INFRA | off (set enables) (inf) | src/runtime_content.c:120 | - | - | 4 | 1 CHANGELOG.md:1243 | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:39 | - | yes | 4 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG_ASSUME_FOCUS` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:968 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG_IMMEDIATE` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1503 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG_LOG` | MERGE-INTO-FPS | value | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:903 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_DLSSG_ONLY_GENERATED` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:865 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG_PARALLEL` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:777 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSSG_SLEEP` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1935 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_COMPARE` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1056 | - | yes | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_DEPTH_FLIP` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:442 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_MVEC_MUL` | KEEP-INFRA | value | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:438 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_PROBE` | MERGE-INTO-TRACE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5784 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_DLSS_REPEAT` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:951 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:105 | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_SANITIZE` | KEEP-INFRA | value | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1141 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_SHARPEN` | KEEP-INFRA | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:979 | - | yes | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_SKY_SHARPEN` | KEEP-INFRA | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:981 | - | yes | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_UI_SHARPEN` | KEEP-INFRA | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:980 | - | yes | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_VIEW` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1489 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DLSS_VIGNETTE` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5261 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_DMA_INCR` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7097 | - | yes | 0 | 1 docs/re-stutters/reports3/draw/wave1/v1buf.md:95 | documented or set by tools only; no measured result found |
| `SOTC_DMA_MERGE` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7056 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_DMA_MERGE_MAX` | DELETE-UNUSED | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7060 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_DMA_OVERLAP` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:534 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:101 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_DMA_SYNC_SKIP` | DELETE-UNUSED | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6891 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_DRAW_SPLIT` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:994 | - | yes | 2 | 11 CHANGELOG.md:220 | documented or set by tools only; no measured result found |
| `SOTC_DRAW_TAG` | MERGE-INTO-TRACE | no reader | - | - | - | 2 | 0  | observe-only hook probe (timeline or counters) |
| `SOTC_DRAW_WORK` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1659 | - | yes | 2 | 5 docs/re-stutters/reports3/draw/FINAL-DESIGN.md:141 | documented or set by tools only; no measured result found |
| `SOTC_DRIVER_CACHE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:54 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:97 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_DROP_ASYNC` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:744 | flag/memory | yes | 1 | 1 CHANGELOG.md:552 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Release memory without waiting for the draw thread |
| `SOTC_DROP_FAST` | DELETE-AT-NATIVE | on | gpu/shadps4/video_core/amdgpu/liverpool.h:374 | flag_on/memory | yes | 2 | 29 CHANGELOG.md:23 | crash in CHANGELOG; default is ON in code (liverpool.h:367) and launcher: must go OFF first Fast release of memory the GPU only read |
| `SOTC_DROP_TRACE` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1475 | - | yes | 0 | 0  | diagnostic of a UMA family: deleted with its family |
| `SOTC_EVICT_ASYNC` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9141 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_DT_PROBE` | MERGE-INTO-TRACE | no reader | - | - | - | 3 | 1 docs/re-stutters/reports3/draw/wave1/f2.md:100 | observe-only hook probe (timeline or counters) |
| `SOTC_DUMP` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4940 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_EARLY_VO_LABEL` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:135 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_EARLY_Z` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:248 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_ENGINE_CMD` | KEEP-INFRA | no reader | - | - | - | 5 | 4 docs/re-stutters/reports3/09-draw-freeze.md:18 | paths, config, build, launcher or Lab plumbing |
| `SOTC_EOP_DEFER` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4220 | - | yes | 0 | 2 docs/sotc/MODELO-MEMORIA-PC.md:34 | documented or set by tools only; no measured result found |
| `SOTC_EOP_TICK` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:294 | flag/diag | yes | 1 | 4 CHANGELOG.md:567 | exposed in the launcher Check end-of-pipe labels (diagnostic) |
| `SOTC_EOP_TICK_US` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:2079 | value/diag | yes | 2 | 0  | exposed in the launcher Max label wait (us, diagnostic) |
| `SOTC_EVENT_LOG` | KEEP-INFRA | off (set enables) (inf) | src/runtime_event.c:50 | - | - | 3 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_EVF_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | src/runtime_sotc.c:329 | - | - | 0 | 6 docs/re-stutters/STUTTER-ARCHITECTURE.md:166 | timeline and trace spans |
| `SOTC_EXPAND_COPY_STALE` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1064 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:100 | documented or set by tools only; no measured result found |
| `SOTC_FASTBIND_WIDE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:93 | flag/bind | yes | 1 | 1 CHANGELOG.md:565 | exposed in the launcher Reuse bind in later submits too |
| `SOTC_FAST_BIND` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/sotc_stats.h:86 | - | yes | 0 | 8 CHANGELOG.md:596 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FAULT_GRACE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:61 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_FAULT_GRACE_PAGES` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:65 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_FAULT_GRACE_US` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:72 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_FAULT_GRANULE` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/sotc_stats.h:730 | choice/memory | yes | 1 | 5 CHANGELOG.md:288 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Block invalidated per write fault |
| `SOTC_FAULT_OSCOST` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/page_manager.cpp:1127 | flag/diag | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Measure system cost per fault |
| `SOTC_FG_HUDLESS` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5783 | - | yes | 1 | 4 CHANGELOG.md:1041 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FG_JITTER` | KEEP-INFRA | value | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:645 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FG_UI_COPY` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:703 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:105 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FIBER_STACKS` | MERGE-INTO-TRACE | off (set enables) (inf) | src/runtime_fiber.c:251 | - | - | 0 | 0  | timeline and trace spans |
| `SOTC_FILE_DEBUG` | UNKNOWN-MEASURE | check (inf) | src/runtime_file.c:300 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `SOTC_FILL_HLE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:624 | - | yes | 0 | 1 docs/sotc/REVISAO-DO-PROJETO.md:154 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FILL_HLE_FAST` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:665 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FILL_HLE_GPU` | KEEP-DEFAULT-ON | on | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:741 | flag_on/upload | yes | 1 | 3 CHANGELOG.md:14 | on in the code or the launcher without a user action (default read from code) Fill tables on the GPU |
| `SOTC_FILL_HLE_PAGES` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:669 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FIND_MEMO` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3362 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FLIP_LATE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:2726 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:19 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FLIP_OFFTHREAD` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:66 | - | yes | 3 | 5 docs/re-stutters/HANDOFF.md:152 | documented or set by tools only; no measured result found |
| `SOTC_FLIP_PACING` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:2612 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_FLIP_SERVICE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:370 | flag/present | yes | 1 | 5 CHANGELOG.md:657 | exposed in the launcher Serve the game while waiting for a frame |
| `SOTC_FLIP_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shadps4/core/libraries/kernel/equeue.cpp:638 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_FLOAT_ATOMIC_FALLBACK` | DELETE-UNUSED | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.h:310 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_FLUSH_EVERY` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4783 | - | yes | 1 | 0  | exposed in the launcher |
| `SOTC_FLUSH_US` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4788 | - | yes | 1 | 2 docs/sotc/REVISAO-DO-PROJETO.md:80 | exposed in the launcher |
| `SOTC_FOO` | DELETE-DEAD | off (set enables) (inf) | gpu/sotc/sotc_live.h:9 | - | - | 3 | 2 docs/sotc/LAB.md:427 | test or example name only |
| `SOTC_FPS` | KEEP-CONSOLIDATED | off (set enables) (inf) | src/runtime_kernel.c:165 | - | - | 11 | 109 CHANGELOG.md:92 | consolidation target (report or timeline) |
| `SOTC_FPS_CAP` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/sotc_fps_cap.h:21 | - | yes | 5 | 1 docs/re-stutters/reports2/12-flip-present.md:285 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FPS_CAP_TEST_CYCLE` | KEEP-INFRA | value | gpu/shadps4/video_core/sotc_fps_cap.h:64 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FPS_LEAN` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:45 | flag/diag | yes | 1 | 10 CHANGELOG.md:213 | report counters and printers Lighter FPS summary |
| `SOTC_FPS_LIMIT` | KEEP-INFRA | value | gpu/shadps4/core/libraries/videoout/driver.cpp:2624 | - | yes | 4 | 1 docs/sotc/ROADMAP.md:167 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FRAMEGEN` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:24 | - | yes | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FRAMEGEN_TEST_CYCLE` | KEEP-INFRA | value | gpu/shadps4/video_core/renderer_vulkan/vk_framegen.cpp:156 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FRAME_AHEAD` | UNKNOWN-MEASURE | value | gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:194 | - | yes | 2 | 19 docs/re-stutters/HANDOFF.md:159 | documented or set by tools only; no measured result found |
| `SOTC_FRAME_LOG` | MERGE-INTO-FPS | value | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1684 | - | yes | 0 | 1 CHANGELOG.md:582 | report counters and printers |
| `SOTC_FREECAM` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:206 | - | - | 2 | 3 docs/re-stutters/freecam-re.md:86 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FREECAM_CARRY` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:201 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FREECAM_DROP` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:202 | - | - | 0 | 2 docs/sotc/LAB.md:54 | paths, config, build, launcher or Lab plumbing |
| `SOTC_FREECAM_MAXSPEED` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:288 | value/diag | - | 2 | 4 docs/sotc/LAB.md:69 | paths, config, build, launcher or Lab plumbing Free-cam maximum speed (m/s) |
| `SOTC_FREECAM_SPEED` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:200 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_FSRFG_LOG` | MERGE-INTO-FPS | value | gpu/shadps4/video_core/renderer_vulkan/vk_fsrfg.cpp:288 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_FSRFG_ONLY_GENERATED` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1326 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_GAME_AFFINITY` | UNKNOWN-MEASURE | off (1 enables) (inf) | src/runtime_thread.c:311 | - | - | 0 | 0  | no launcher row, no setter, no docs: check whether anything uses it |
| `SOTC_GAME_DIR` | KEEP-INFRA | no reader | - | - | - | 27 | 2 docs/re-stutters/reports2/08-shader-first-use.md:207 | paths, config, build, launcher or Lab plumbing |
| `SOTC_GAME_VERSION` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/window.cpp:109 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_GC_DOWNLOAD` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:363 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:101 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_GEN_BIND` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_generation.h:25 | - | yes | 4 | 17 docs/re-stutters/HANDOFF.md:154 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GEN_DMA` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_generation.h:32 | - | yes | 3 | 8 docs/re-stutters/HANDOFF.md:164 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GEN_TEX` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1525 | - | yes | 3 | 15 docs/re-stutters/HANDOFF.md:155 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GENERAL_SYNC` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/texture_cache/image.cpp:535 | - | yes | 0 | 1 CHANGELOG.md:838 | documented or set by tools only; no measured result found |
| `SOTC_GLYPH_LOG` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2506 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_GPUMOD_CONTAINS` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6122 | flag/memory | yes | 1 | 2 CHANGELOG.md:315 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Lock-free check for an already written range |
| `SOTC_GPUMOD_NOGROW_SKIP` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6113 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GPUMOD_WATCH` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6084 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_IMG_WATCH` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1361 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_MAP_INVALIDATE` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shim/bbgpu.cpp:140 | - | - | 0 | 2 docs/re-stutters/reports3/draw/wave1/v1buf.md:103 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_STALE_PAGES` | DELETE-AT-NATIVE | value | gpu/sotc/stale_check.cpp:71 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GPU_THREAD_HIGH` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:63 | flag/threads | yes | 1 | 2 CHANGELOG.md:565 | exposed in the launcher GPU threads at high priority |
| `SOTC_GPU_VENDOR` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:128 | - | yes | 0 | 2 CHANGELOG.md:1062 | documented or set by tools only; no measured result found |
| `SOTC_GR` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_GRASS` | KEEP-INFRA | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2259 | - | yes | 1 | 1 docs/re-stutters/reports2/14-draw-sources.md:120 | paths, config, build, launcher or Lab plumbing |
| `SOTC_GUEST_CALLER` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_GUEST_CALLER_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_GUARD_DEDUP` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:180 | - | yes | 0 | 2 CHANGELOG.md:808 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GUARD_STALE` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5744 | - | yes | 0 | 2 CHANGELOG.md:811 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_GUARD_USER_POINTER` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:453 | - | yes | 0 | 2 CHANGELOG.md:811 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HEAP_FAST` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5466 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HEAP_GUARD` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/probe.c:940 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HEAP_GUARD_RUNS` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp:97 | flag/memory | yes | 1 | 1 CHANGELOG.md:709 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Reuse heap locations during readbacks |
| `SOTC_HEAP_WATCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/probe.c:1971 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_LOCK_WAKE` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:125 | - | - | 0 | 2 CHANGELOG.md:806 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_LOCK_WATCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:138 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HLE_SHADOW_CULL` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1538 | flag/bind | yes | 1 | 2 CHANGELOG.md:411 | exposed in the launcher Shadow cull computed on the CPU |
| `SOTC_HLE_SKIP` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:44 | choice/memory | yes | 1 | 4 CHANGELOG.md:257 | exposed in the launcher Marks of emulated shader copies |
| `SOTC_HLE_TABLE_GEN` | DELETE-NO-GAIN | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp:57 | - | yes | 1 | 5 docs/re-stutters/reports3/draw/switches-review.md:5 | draw-thread switch, no measured gain (switches-review.md) |
| `SOTC_HOOKS` | KEEP-INFRA | off (set enables) (inf) | gpu/sotc/hooks.cpp:668 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_HOOKS_LOG` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/sotc/hooks.cpp:726 | - | - | 0 | 0  | report counters and printers |
| `SOTC_HOOK_ALLOC_PROBE` | DELETE-AT-NATIVE | no reader | - | - | - | 32 | 20 CHANGELOG.md:86 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HOOK_CVAR_PROBE` | MERGE-INTO-TRACE | no reader | - | - | - | 2 | 0  | observe-only hook probe (timeline or counters) |
| `SOTC_HOOK_EXAMPLE_HOOK` | DELETE-DEAD | no reader | - | - | - | 1 | 0  | test or example name only |
| `SOTC_HOOK_FRAME_FLIP` | DELETE-AT-NATIVE | no reader | - | - | - | 13 | 7 CHANGELOG.md:87 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HOOK_HEAP_RANGE` | MERGE-INTO-TRACE | no reader | - | - | - | 1 | 0  | observe-only hook probe (timeline or counters) |
| `SOTC_HOOK_LABEL_WRITER` | DELETE-DEAD | no reader | - | - | - | 1 | 0  | test or example name only |
| `SOTC_HOOK_LOADER_NOSTREAM` | MERGE-INTO-TRACE | no reader | - | - | - | 1 | 1 docs/re-stutters/reports2/03-streaming-bursts.md:127 | observe-only hook probe (timeline or counters) |
| `SOTC_HOOK_LOADER_PROBE` | MERGE-INTO-TRACE | no reader | - | - | - | 2 | 1 docs/re-stutters/reports2/03-streaming-bursts.md:123 | observe-only hook probe (timeline or counters) |
| `SOTC_HOOK_PSTATS` | MERGE-INTO-FPS | no reader | - | - | - | 3 | 3 CHANGELOG.md:381 | report counters and printers |
| `SOTC_HOOK_SAMPLE` | DELETE-DEAD | no reader | - | - | - | 1 | 0  | test or example name only |
| `SOTC_HOOK_XPRMEM_PROBE` | DELETE-AT-NATIVE | no reader | - | - | - | 3 | 1 docs/re-stutters/reports3/draw/wave1/r1.md:33 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HOOK_XPR_PROBE` | DELETE-AT-NATIVE | no reader | - | - | - | 7 | 4 docs/re-stutters/PC-MEMORY-REWRITE.md:108 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_HOST_IMAGE_COPY` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:245 | flag/upload | yes | 1 | 2 CHANGELOG.md:569 | exposed in the launcher Upload textures by host copy |
| `SOTC_HOST_IMPORT` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:943 | flag/upload | - | 1 | 9 CHANGELOG.md:227 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Copy large uploads straight from game memory |
| `SOTC_HOST_IMPORT_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/host_import.cpp:39 | value/upload | yes | 1 | 1 CHANGELOG.md:770 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Imported memory limit (MiB) |
| `SOTC_HOST_IMPORT_MIN_KB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/host_import.cpp:40 | value/upload | yes | 1 | 1 CHANGELOG.md:768 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Minimum size to import (KiB) |
| `SOTC_HOT_PAGES` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:115 | flag/memory | yes | 1 | 4 CHANGELOG.md:408 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Hot pages briefly left unprotected |
| `SOTC_HOT_PAGES_ADAPT` | DELETE-AT-NATIVE | on | gpu/shadps4/video_core/buffer_cache/region_manager.h:157 | flag_on/memory | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Automatic hot page limit |
| `SOTC_HOT_PAGES_MAX` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:142 | value/memory | yes | 1 | 1 CHANGELOG.md:744 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Maximum number of hot pages |
| `SOTC_HOT_PAGES_MAX_CEIL` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:149 | value/memory | yes | 1 | 1 CHANGELOG.md:437 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Hot page limit ceiling |
| `SOTC_HOT_PAGES_MS` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:135 | value/memory | yes | 1 | 1 CHANGELOG.md:742 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Hot page lifetime (ms) |
| `SOTC_HOT_PAGES_WINDOW_MS` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/region_manager.h:127 | value/memory | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Window to mark a hot page (ms) |
| `SOTC_HRR` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:1006 | choice/readback | yes | 1 | 4 CHANGELOG.md:313 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Host-resident ranges |
| `SOTC_HRR_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/hrr.h:262 | value/readback | yes | 1 | 1 CHANGELOG.md:479 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Resident ranges limit (MiB) |
| `SOTC_HRR_MIN_RB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/hrr.h:258 | value/readback | yes | 1 | 1 CHANGELOG.md:478 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Readbacks per second to pick a range |
| `SOTC_IMAGE_BASE` | KEEP-USER-OPTION | value | gpu/shadps4/core/libraries/videoout/driver.cpp:147 | value/diag | yes | 1 | 0  | exposed in the launcher Game image base address (hex) |
| `SOTC_IMAGE_POOL_MB` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/texture_cache/image.cpp:152 | value/upload | yes | 1 | 3 CHANGELOG.md:733 | exposed in the launcher Texture memory pool (MiB) |
| `SOTC_IMAGE_PREFETCH_MAX` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:42 | value/upload | yes | 1 | 3 CHANGELOG.md:331 | exposed in the launcher Images created ahead of time |
| `SOTC_IMG_CREATE_AHEAD` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/image_prefetch.cpp:60 | - | yes | 3 | 3 docs/re-stutters/HANDOFF.md:156 | documented or set by tools only; no measured result found |
| `SOTC_IMG_GPU_OVERLAP` | DELETE-UNUSED | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:527 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_IMG_POOLED` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/image.cpp:137 | - | yes | 3 | 3 docs/re-stutters/HANDOFF.md:156 | documented or set by tools only; no measured result found |
| `SOTC_INDIRECT_DIRECT` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2101 | - | yes | 0 | 2 docs/sotc/README-SotC.md:159 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_INDIRECT_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2149 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_INLINE_FUNC` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:266 | flag/threads | yes | 1 | 1 CHANGELOG.md:537 | exposed in the launcher Small queue blocks without allocation |
| `SOTC_INTEL_PIN_SUBGROUP` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_instance.h:384 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:119 | documented or set by tools only; no measured result found |
| `SOTC_INVAL_INDEX` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:543 | flag/bind | yes | 1 | 3 CHANGELOG.md:316 | exposed in the launcher Texture invalidation by page |
| `SOTC_LAB` | KEEP-INFRA | check (inf) | gpu/sotc/freecam.cpp:206 | flag/diag | - | 12 | 17 CHANGELOG.md:64 | paths, config, build, launcher or Lab plumbing SotC Lab: menu F2, free-cam F3, colossi, automated tests |
| `SOTC_LABELS_HONEST` | KEEP-INFRA | off (1 enables) (inf) | gpu/sotc/label_honest.cpp:30 | - | - | 0 | 15 docs/re-stutters/HANDOFF.md:151 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAB_ORCH_CMD` | KEEP-INFRA | off (set enables) (inf) | gpu/sotc/lab_commands.cpp:912 | - | - | 0 | 3 docs/sotc/LAB.md:293 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAB_ROUTE` | KEEP-INFRA | off (set enables) (inf) | gpu/sotc/engine_cmd.cpp:225 | - | - | 4 | 5 docs/re-stutters/PC-MEMORY-REWRITE.md:108 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAB_ROUTE_DELAY` | KEEP-INFRA | check (inf) | gpu/sotc/lab_commands.cpp:700 | - | - | 1 | 5 docs/re-stutters/reports3/14-loading-hangs.md:112 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAB_ROUTE_EXIT` | KEEP-INFRA | check (inf) | gpu/sotc/lab_commands.cpp:701 | - | - | 1 | 2 docs/sotc/LAB.md:274 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAB_ROUTE_WAIT` | KEEP-INFRA | value | gpu/sotc/engine_cmd.cpp:230 | - | - | 0 | 3 docs/re-stutters/reports3/14-loading-hangs.md:137 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAUNCHER_SETTINGS` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/bbport_overlay.cpp:294 | - | - | 2 | 1 docs/sotc/ROADMAP.md:50 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LAYOUT_CACHE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp:40 | flag/bind | yes | 1 | 1 CHANGELOG.md:571 | exposed in the launcher Descriptor layout cache |
| `SOTC_LDS_BARRIERS` | DELETE-UNUSED | value | gpu/shadps4/shader_recompiler/ir/passes/shared_memory_barrier_pass.cpp:180 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LDS_SMALL` | DELETE-UNUSED | value | gpu/shadps4/shader_recompiler/ir/passes/shared_memory_barrier_pass.cpp:195 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LLVM_SYMBOLIZER` | KEEP-INFRA | no reader | - | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_LOAD_FLUSH` | KEEP-DEFAULT-ON | on (0 disables) (inf) | src/runtime_file.c:441 | - | - | 0 | 1 CHANGELOG.md:925 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_LOAD_WARM` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/sotc/load_warm.cpp:44 | - | - | 0 | 5 CHANGELOG.md:33 | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_LOAD_WARM_TEX` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/sotc/load_warm.cpp:330 | - | - | 0 | 2 CHANGELOG.md:38 | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_LOD_FB_BIAS` | DELETE-UNUSED | value | gpu/shadps4/video_core/sotc_lod_stats.h:247 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_FB_EVERY` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:190 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_FB_HOLD_MS` | DELETE-UNUSED | value | gpu/shadps4/video_core/sotc_lod_stats.h:267 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_FB_KEEP` | DELETE-UNUSED | value | gpu/shadps4/video_core/sotc_lod_stats.h:259 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_FB_MIN` | DELETE-UNUSED | value | gpu/shadps4/video_core/sotc_lod_stats.h:255 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_FB_USE` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/sotc_lod_stats.h:275 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_LOD_FEEDBACK` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/shader_recompiler/info.cpp:67 | - | yes | 0 | 4 CHANGELOG.md:816 | documented or set by tools only; no measured result found |
| `SOTC_LOD_KEEP` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/sotc_lod_stats.h:307 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_LOD_LEVEL` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/sotc_lod_stats.h:153 | - | yes | 0 | 4 CHANGELOG.md:1192 | documented or set by tools only; no measured result found |
| `SOTC_LOD_LINGER` | KEEP-INFRA | value | gpu/shadps4/video_core/sotc_lod_stats.h:146 | - | yes | 3 | 1 docs/sotc/ROADMAP.md:133 | paths, config, build, launcher or Lab plumbing |
| `SOTC_LOD_POOL_LOG` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/sotc_lod_stats.h:192 | - | yes | 0 | 2 docs/sotc/ROADMAP.md:58 | report counters and printers |
| `SOTC_LOD_POOL_LOW` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/sotc_lod_stats.h:172 | - | yes | 1 | 2 docs/sotc/ROADMAP.md:60 | exposed in the launcher |
| `SOTC_LOD_STATS` | MERGE-INTO-FPS | on (0 disables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4512 | - | yes | 0 | 3 CHANGELOG.md:1192 | report counters and printers |
| `SOTC_LOD_TRACE` | MERGE-INTO-TRACE | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:4528 | - | yes | 0 | 1 docs/sotc/REVISAO-DO-PROJETO.md:146 | timeline and trace spans |
| `SOTC_LOW_LATENCY` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:627 | flag/present | yes | 1 | 1 CHANGELOG.md:570 | exposed in the launcher Low latency (Reflex / Anti-Lag) |
| `SOTC_MAX_IN_PRO` | KEEP-INFRA | off (1 enables) (inf) | gpu/shim/bbport_overlay.cpp:655 | - | - | 2 | 1 CHANGELOG.md:857 | paths, config, build, launcher or Lab plumbing |
| `SOTC_MAYBE_DIRTY_HASH` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2107 | - | yes | 0 | 2 CHANGELOG.md:559 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_MDI_STATS` | MERGE-INTO-FPS | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1586 | flag/diag | yes | 2 | 1 CHANGELOG.md:308 | report counters and printers Count mergeable indirect draws |
| `SOTC_MEMO_ADDR_GEN` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3354 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_HOOK_MEMTYPE_PROBE` | DELETE-AT-NATIVE | no reader | - | - | - | 14 | 5 docs/re-stutters/PC-MEMORY-REWRITE.md:328 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_MEMTYPE_CLASSES` | DELETE-AT-NATIVE | value | gpu/sotc/probe_memtype.cpp:544 | - | - | 9 | 5 CHANGELOG.md:132 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_MENU_TRIGGER` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shim/bbport_overlay.cpp:1547 | - | - | 1 | 0  | timeline and trace spans |
| `SOTC_META_CLEAR` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5084 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_MINIDUMP` | KEEP-DEFAULT-ON | on (0 disables) (inf) | src/probe.c:808 | - | - | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_MIP_BIAS` | DELETE-UNUSED | value | gpu/shadps4/video_core/texture_cache/sampler.cpp:21 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_MIP_BUDGET` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/sotc/mip_budget.cpp:59 | - | - | 2 | 0  | load and streaming feature, not UMA tracking; the 8 doc hits must be checked |
| `SOTC_MIP_TAIL` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.h:55 | - | yes | 0 | 2 docs/sotc/PLANO-THREAD-DESENHO.md:34 | documented or set by tools only; no measured result found |
| `SOTC_MODES` | KEEP-INFRA | no reader | - | - | - | 7 | 2 CHANGELOG.md:1034 | paths, config, build, launcher or Lab plumbing |
| `SOTC_MOUSE` | KEEP-INFRA | check (inf) | gpu/shim/bbport_settings.cpp:124 | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_MOUSE_DEBUG` | KEEP-INFRA | off (set enables) (inf) | src/runtime_pad.c:358 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_MOUSE_INVERT` | KEEP-INFRA | off (1 enables) (inf) | src/runtime_pad.c:328 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_MOUSE_SENS` | KEEP-INFRA | off (1 enables) (inf) | src/runtime_pad.c:325 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_MUTEX_SPIN_US` | KEEP-USER-OPTION | value | src/runtime_mutex.c:119 | value/threads | - | 1 | 4 CHANGELOG.md:17 | exposed in the launcher Short spin on game mutexes (us) |
| `SOTC_MVEC_FILL` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1683 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_NEO` | KEEP-INFRA | off (set enables) (inf) | src/runtime_sotc.c:49 | - | - | 8 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_NOTE` | KEEP-INFRA | no reader | - | - | - | 6 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_NOT_A_SWITCH` | DELETE-DEAD | no reader | - | - | - | 1 | 0  | test or example name only |
| `SOTC_NO_BARYCENTRIC` | DELETE-UNUSED | check (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:396 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_OCCLUSION_STEP` | DELETE-UNUSED | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3976 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_OPSTAT` | MERGE-INTO-FPS | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:258 | flag/diag | yes | 1 | 4 CHANGELOG.md:305 | report counters and printers Draw thread operation stats |
| `SOTC_OPT_OFF` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1196 | - | yes | 0 | 1 CHANGELOG.md:749 | documented or set by tools only; no measured result found |
| `SOTC_OP_OVERLAP` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:1592 | - | yes | 0 | 3 docs/DRAW-THREAD-7C-DESIGN.md:10 | documented or set by tools only; no measured result found |
| `SOTC_OP_PHASES` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_op_phases.h:36 | choice/diag | yes | 1 | 9 CHANGELOG.md:213 | report counters and printers Draw thread time per phase |
| `SOTC_OUTPUT_RES` | KEEP-INFRA | off (set enables) (inf) | src/probe.c:1850 | - | - | 6 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_OVERLAY` | KEEP-INFRA | on (0 disables) (inf) | gpu/shim/bbport_overlay.cpp:219 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_PARALLEL_RECORD` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:378 | flag/threads | yes | 3 | 20 CHANGELOG.md:323 | exposed in the launcher Parallel draw preparation |
| `SOTC_PARALLEL_RECORD_WORKERS` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/amdgpu/liverpool.cpp:382 | value/threads | yes | 1 | 1 CHANGELOG.md:629 | exposed in the launcher Preparation threads |
| `SOTC_PARTICLE_PREV` | DELETE-AT-NATIVE | no reader | - | - | - | 11 | 3 docs/re-stutters/PC-MEMORY-REWRITE.md:226 | prerequisite of RB_CLASS=2 (readback serving) |
| `SOTC_PASS_SEC` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:70 | flag/threads | yes | 1 | 9 CHANGELOG.md:571 | exposed in the launcher Large passes on their own thread |
| `SOTC_PASS_SEC_MIN` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:79 | value/threads | yes | 1 | 1 CHANGELOG.md:572 | exposed in the launcher Minimum draws for own thread |
| `SOTC_PASS_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:729 | - | yes | 0 | 3 docs/re-stutters/reports2/14-draw-sources.md:15 | timeline and trace spans |
| `SOTC_PATCHES_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_PC_MEMORY` | UNKNOWN-DECISION | off (1 enables) (inf) | src/runtime_memory.c:943 | - | - | 0 | 7 CHANGELOG.md:220 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_PLACE` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool_place_glue.cpp:207 | - | - | 2 | 1 docs/re-stutters/reports3/11-placement-by-class.md:217 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_PLACE_WINDOWS` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool_place_glue.cpp:208 | - | - | 0 | 1 docs/re-stutters/reports3/11-placement-by-class.md:217 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_POOL` | MERGE-INTO-VERIFY | off (set enables) (inf) | gpu/sotc/pc_pool.cpp:1186 | - | - | 11 | 11 CHANGELOG.md:120 | observer and verify of the game allocator model (98 pct match) |
| `SOTC_PC_POOL_MODEL_ONLY` | UNKNOWN-DECISION | no reader | - | - | - | 0 | 0  | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_POOL_NATIVE` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool.cpp:1206 | - | - | 15 | 14 CHANGELOG.md:122 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_POOL_PATHS` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool.cpp:1266 | - | - | 5 | 3 docs/re-stutters/HANDOFF.md:16 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_POOL_REFILL` | UNKNOWN-DECISION | value | gpu/sotc/pc_pool.cpp:1256 | - | - | 1 | 3 docs/re-stutters/HANDOFF.md:30 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_RECYCLE` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool_place_glue.cpp:207 | - | - | 1 | 1 docs/re-stutters/reports3/11-placement-by-class.md:218 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_RECYCLE_FRAMES` | UNKNOWN-DECISION | off (set enables) (inf) | gpu/sotc/pc_pool_place_glue.cpp:208 | - | - | 0 | 1 docs/re-stutters/reports3/11-placement-by-class.md:218 | allocator candidate for the native model (6454/6454 exact in game); keep or delete |
| `SOTC_PC_RING` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/sotc_guest_memory.cpp:386 | - | yes | 24 | 50 CHANGELOG.md:77 | default 8 (sotc.py); UMA arena ring. Removing it needs sotc.py changed too |
| `SOTC_PC_RING8_CLIP` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7837 | - | yes | 0 | 3 CHANGELOG.md:82 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_DEDUPE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7585 | - | yes | 0 | 1 docs/re-stutters/HANDOFF.md:40 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_FLIPWAIT` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/pc_ring.cpp:513 | - | - | 0 | 6 CHANGELOG.md:85 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_GROUP` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/sotc/pc_ring.cpp:1687 | - | - | 0 | 1 docs/re-stutters/reports3/draw/wave1/m1.md:18 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_HEAPS` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp:409 | - | yes | 2 | 5 docs/re-stutters/HANDOFF.md:127 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_HEAPS_MODE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/pc_ring.cpp:1595 | - | - | 0 | 7 docs/re-stutters/HANDOFF.md:153 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_LABELS` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:162 | - | yes | 0 | 6 docs/re-stutters/HANDOFF.md:32 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_VERIFY` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/sotc/pc_ring.cpp:1679 | - | - | 0 | 1 docs/re-stutters/PC-MEMORY-REWRITE.md:334 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING8_VERIFY_SAMPLE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/pc_ring.cpp:1695 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING_BUMP` | DELETE-AT-NATIVE | no reader | - | - | - | 2 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING_DISPATCH` | DELETE-AT-NATIVE | no reader | - | - | - | 4 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING_HOT_EPOCHS` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/pc_ring.cpp:294 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING_SLOW` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/sotc/pc_ring.cpp:655 | - | - | 0 | 2 docs/re-stutters/reports3/09-draw-freeze.md:18 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PC_RING_VERIFY_SAMPLE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/pc_ring.cpp:285 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PEEK` | KEEP-INFRA | on (0 disables) (inf) | src/probe.c:1966 | - | - | 0 | 1 docs/sotc/ROADMAP.md:58 | paths, config, build, launcher or Lab plumbing |
| `SOTC_PEEK_DELAY` | KEEP-INFRA | value | src/probe.c:360 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_PIPELINE_WORKERS` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:537 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_PIPE_DIRTY` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1756 | - | yes | 3 | 12 docs/re-stutters/HANDOFF.md:158 | documented or set by tools only; no measured result found |
| `SOTC_PIPE_GPL` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:404 | - | yes | 3 | 10 docs/re-stutters/HANDOFF.md:158 | documented or set by tools only; no measured result found |
| `SOTC_PIPE_MEMO` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1191 | - | yes | 0 | 6 docs/re-stutters/reports2/00-PLAN.md:33 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_PIPE_REMAP` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:132 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_PLAY_WRAP` | KEEP-INFRA | no reader | - | - | - | 3 | 1 docs/sotc/REVISAO-DO-PROJETO.md:149 | paths, config, build, launcher or Lab plumbing |
| `SOTC_POOL_RESERVE` | KEEP-INFRA | no reader | - | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_POOL_WATCH` | MERGE-INTO-FPS | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3109 | - | yes | 1 | 0  | report counters and printers |
| `SOTC_POP_FAST` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:622 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_PREFETCH` | KEEP-DEFAULT-ON | on (0 disables) (inf) | src/probe.c:1967 | - | - | 0 | 4 CHANGELOG.md:923 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_PRESENT_FRAMES` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:589 | - | yes | 0 | 4 docs/re-stutters/reports2/00-PLAN.md:71 | documented or set by tools only; no measured result found |
| `SOTC_PRESENT_UNLOCKED` | DELETE-NO-GAIN | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h:521 | - | yes | 3 | 7 docs/re-stutters/reports3/draw/switches-review.md:5 | draw-thread switch, no measured gain (switches-review.md) |
| `SOTC_PRESENT_WAIT` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:2643 | flag/present | yes | 1 | 3 CHANGELOG.md:567 | exposed in the launcher Flip pacing by the display |
| `SOTC_PROBE` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_PROFILE` | MERGE-INTO-TRACE | check (inf) | src/probe.c:1705 | - | - | 4 | 15 CHANGELOG.md:50 | timeline and trace spans |
| `SOTC_PROFILE_DELAY` | MERGE-INTO-TRACE | value | src/probe.c:537 | - | - | 0 | 2 docs/DRAW-THREAD-7C-DESIGN.md:38 | timeline and trace spans |
| `SOTC_PROFILE_OUT` | MERGE-INTO-TRACE | check (inf) | src/probe.c:611 | - | - | 6 | 3 docs/re-stutters/reports3/09-draw-freeze.md:102 | timeline and trace spans |
| `SOTC_PROFILE_SECONDS` | MERGE-INTO-TRACE | value | src/probe.c:537 | - | - | 0 | 2 docs/DRAW-THREAD-7C-DESIGN.md:38 | timeline and trace spans |
| `SOTC_PROTECT_BATCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:286 | flag/memory | yes | 3 | 6 CHANGELOG.md:642 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Batched page re-protection |
| `SOTC_PROTECT_COALESCE` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:386 | flag/memory | yes | 1 | 1 CHANGELOG.md:434 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Fewer page protection calls |
| `SOTC_PROTECT_FAST` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:2145 | - | - | 0 | 2 CHANGELOG.md:807 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PROTECT_GAP` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/page_manager.cpp:257 | value/memory | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Page gap bridged |
| `SOTC_PROTECT_SPAN` | DELETE-AT-NATIVE | off (1 enables) (inf) | src/runtime_memory.c:2139 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_TEX_PROTECT_BATCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:69 | - | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_PR_GEN` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2787 | - | yes | 2 | 11 docs/re-stutters/HANDOFF.md:171 | documented or set by tools only; no measured result found |
| `SOTC_PR_RESOLVE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2774 | flag/bind | yes | 1 | 9 CHANGELOG.md:306 | exposed in the launcher Descriptors resolved before the draw thread |
| `SOTC_PR_VERIFY` | MERGE-INTO-VERIFY | value | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:7402 | value/diag | yes | 1 | 3 CHANGELOG.md:307 | verify or compare mode Verify prepared draws (every N) |
| `SOTC_RAM_REPORT` | MERGE-INTO-FPS | on (0 disables) (inf) | gpu/sotc/ram_report.cpp:100 | - | - | 4 | 2 docs/re-stutters/HANDOFF.md:26 | report counters and printers |
| `SOTC_RB_AHEAD` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5179 | - | yes | 2 | 1 docs/sotc/REVISAO-DO-PROJETO.md:154 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_ASYNC` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:158 | - | yes | 0 | 3 CHANGELOG.md:1287 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_ASYNC_AHEAD` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1706 | flag/readback | yes | 1 | 1 CHANGELOG.md:686 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) GPU read without waiting for the current submit |
| `SOTC_RB_BATCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:9508 | flag/readback | yes | 1 | 1 CHANGELOG.md:677 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Readbacks batched in one submit |
| `SOTC_RB_CHANGES` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5669 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_CLASS2_PARTS` | DELETE-BROKEN | value | gpu/sotc/rb_class.cpp:96 | - | - | 1 | 1 docs/re-stutters/HANDOFF.md:23 | mode 2 gave vertex explosions (HANDOFF, section 8); UMA readback tuning |
| `SOTC_RB_CPU_GUARD` | DELETE-AT-NATIVE | no reader | - | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_EARLY` | DELETE-AT-NATIVE | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4820 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_EXACT` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:1242 | choice/readback | yes | 1 | 16 CHANGELOG.md:394 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Exact GPU readbacks |
| `SOTC_RB_GROUP` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:151 | flag/readback | yes | 1 | 3 CHANGELOG.md:410 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Early group readbacks |
| `SOTC_RB_GROW` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:128 | flag/readback | yes | 1 | 2 CHANGELOG.md:405 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Growing readback window |
| `SOTC_RB_GROW_MAX_KB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:139 | value/readback | yes | 1 | 1 CHANGELOG.md:590 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Max readback window (KiB) |
| `SOTC_RB_GROW_US` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:134 | value/readback | yes | 1 | 1 CHANGELOG.md:589 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Window growth interval (µs) |
| `SOTC_RB_HOST` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/sotc/rb_host.cpp:101 | - | - | 1 | 17 docs/re-stutters/HANDOFF.md:157 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_HOT` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5292 | - | yes | 1 | 5 CHANGELOG.md:470 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_KEEP_LATER` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1960 | flag/readback | yes | 1 | 1 CHANGELOG.md:680 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Keep a readback when the GPU rewrites it |
| `SOTC_RB_MATERIALIZE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:148 | flag/readback | yes | 11 | 8 CHANGELOG.md:405 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Early occlusion masks |
| `SOTC_RB_MATERIALIZE_US` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:195 | value/readback | yes | 1 | 1 CHANGELOG.md:473 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Mask check interval (µs) |
| `SOTC_RB_NARROW` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:109 | - | yes | 0 | 2 CHANGELOG.md:591 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_OWNER` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:157 | flag/readback | yes | 11 | 9 CHANGELOG.md:439 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Single owner for pages being read |
| `SOTC_RB_PREFETCH` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:147 | flag/readback | yes | 1 | 2 CHANGELOG.md:463 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Early copies of recurring windows |
| `SOTC_RB_PREFETCH_MAX` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:172 | value/readback | yes | 1 | 1 CHANGELOG.md:705 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Early copies at the same time |
| `SOTC_RB_PREFETCH_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:177 | value/readback | yes | 1 | 1 CHANGELOG.md:706 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Copy staging memory (MiB) |
| `SOTC_RB_PREFETCH_TICKS` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:167 | value/readback | yes | 1 | 1 CHANGELOG.md:704 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Submits before dropping an unused copy |
| `SOTC_RB_PREV` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_shadow_cull.cpp:339 | flag/readback | yes | 1 | 11 CHANGELOG.md:449 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Previous frame masks |
| `SOTC_RB_PREV_FRAMES` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:310 | value/readback | yes | 1 | 1 CHANGELOG.md:455 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Max age of the previous version (frames) |
| `SOTC_RB_SPIN_US` | DELETE-AT-NATIVE | check (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:40 | value/readback | yes | 8 | 1 CHANGELOG.md:511 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Busy wait before blocking (µs) |
| `SOTC_RB_STATS` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/page_manager.cpp:1222 | - | yes | 0 | 1 docs/sotc/MODELO-MEMORIA-PC.md:61 | diagnostic of a UMA family: deleted with its family |
| `SOTC_RB_STREAM` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1180 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_STREAM_MAX_KB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1184 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RB_TICK` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1720 | flag/readback | yes | 1 | 4 CHANGELOG.md:405 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Reads on the transfer queue |
| `SOTC_RB_WAIT_US` | DELETE-AT-NATIVE | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:2086 | value/readback | yes | 3 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Wait before sending deferred readbacks (µs) |
| `SOTC_RB_CLASS` | DELETE-BROKEN | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:292 | - | yes | 11 | 18 CHANGELOG.md:115 | mode 2 gave vertex explosions (HANDOFF, section 8); UMA readback tuning |
| `SOTC_RB_CLASS_AHEAD` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:370 | - | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_RDOC_SUBMITS` | MERGE-INTO-TRACE | value | gpu/shim/bbgpu.cpp:725 | - | - | 1 | 1 docs/sotc/AGUA.md:69 | timeline and trace spans |
| `SOTC_RDOC_TRIGGER` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shim/bbgpu.cpp:711 | - | - | 2 | 1 docs/sotc/REVISAO-DO-PROJETO.md:149 | timeline and trace spans |
| `SOTC_REBAR_MB` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/buffer_cache/buffer.cpp:90 | value/upload | yes | 1 | 1 CHANGELOG.md:568 | exposed in the launcher Resizable BAR memory limit (MB) |
| `SOTC_REBAR_UPLOAD` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:1276 | flag/upload | yes | 1 | 4 CHANGELOG.md:331 | exposed in the launcher Uploads through Resizable BAR memory |
| `SOTC_RECORD_THREAD` | KEEP-INFRA | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:549 | - | yes | 1 | 6 docs/DRAW-THREAD-7C-DESIGN.md:68 | paths, config, build, launcher or Lab plumbing |
| `SOTC_REC_SLEEP_US` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:1797 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:16 | documented or set by tools only; no measured result found |
| `SOTC_REC_SPIN_US` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_recorder.cpp:1787 | - | yes | 0 | 4 docs/re-stutters/HANDOFF.md:145 | documented or set by tools only; no measured result found |
| `SOTC_RED_ZONE` | KEEP-INFRA | no reader | - | - | - | 3 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_REFLEX` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:628 | flag/present | yes | 1 | 0  | exposed in the launcher Low latency (Reflex / Anti-Lag) |
| `SOTC_REGION_DIRECT` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:1822 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_REGION_FAST` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:1904 | - | - | 0 | 2 CHANGELOG.md:807 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_REGION_LOG` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:297 | - | - | 0 | 2 CHANGELOG.md:807 | diagnostic of a UMA family: deleted with its family |
| `SOTC_REG_SAME` | KEEP-USER-OPTION | check (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:257 | flag/bind | yes | 1 | 2 CHANGELOG.md:303 | exposed in the launcher Skip repeated register writes |
| `SOTC_RELEASE_ORDER` | KEEP-DEFAULT-ON | on (0 disables) (inf) | src/runtime_memory.c:1475 | - | - | 0 | 4 docs/re-stutters/reports3/10-drop-fast-bad-pm4.md:35 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_RENDER_EXTRA` | KEEP-INFRA | no reader | - | - | - | 3 | 2 CHANGELOG.md:844 | paths, config, build, launcher or Lab plumbing |
| `SOTC_RENDER_KEEP` | KEEP-INFRA | no reader | - | - | - | 2 | 2 CHANGELOG.md:844 | paths, config, build, launcher or Lab plumbing |
| `SOTC_RENDER_RES` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:6678 | - | yes | 6 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_REUSE_SIZE_CHECK` | MERGE-INTO-VERIFY | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:823 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:101 | verify or compare mode |
| `SOTC_RS_MEMO` | KEEP-USER-OPTION | off (set enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:211 | choice/bind | yes | 1 | 9 CHANGELOG.md:254 | exposed in the launcher Reuse render targets |
| `SOTC_RT_CLASS` | DELETE-NO-GAIN | off (set enables) (inf) | gpu/sotc/rt_class.cpp:32 | - | - | 6 | 9 CHANGELOG.md:112 | render-target class: 27.8 vs 27.2 FPS, within noise (PC-MEMORY-REWRITE section 8) |
| `SOTC_RT_FAST` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:343 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_RT_MAXEXTENT` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1228 | - | yes | 3 | 9 docs/re-stutters/HANDOFF.md:155 | documented or set by tools only; no measured result found |
| `SOTC_RUMBLE` | KEEP-INFRA | on (0 disables) (inf) | src/runtime_pad.c:638 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_SAMPLER_MEMO` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2604 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_SAVE_BACKUP` | KEEP-INFRA | on (0 disables) (inf) | src/runtime_savedata.c:569 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_SAVE_CORRUPT` | KEEP-INFRA | off (set enables) (inf) | src/runtime_services.c:346 | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_SAVE_PROMPT` | KEEP-INFRA | off (set enables) (inf) | src/runtime_services.c:350 | - | - | 4 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_SAVE_TRACE` | MERGE-INTO-TRACE | check (inf) | src/runtime_savedata.c:332 | - | - | 0 | 1 docs/sotc/REVISAO-DO-PROJETO.md:145 | timeline and trace spans |
| `SOTC_SET_TRIGGER` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shim/bbport_overlay.cpp:1548 | - | - | 1 | 1 docs/sotc/ROADMAP.md:88 | timeline and trace spans |
| `SOTC_SHADOW_REUSE` | KEEP-USER-OPTION | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:985 | - | yes | 3 | 5 CHANGELOG.md:606 | exposed in the launcher |
| `SOTC_SHADOW_REUSE_STATS` | MERGE-INTO-FPS | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1050 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_SHADOW_STAGGER` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:906 | - | yes | 0 | 2 docs/re-stutters/reports2/14-draw-sources.md:32 | documented or set by tools only; no measured result found |
| `SOTC_SHOT_COUNT` | MERGE-INTO-FPS | value | gpu/shim/bbgpu.cpp:764 | - | - | 0 | 1 docs/re-stutters/HANDOFF.md:89 | report counters and printers |
| `SOTC_SHOT_GENERATED` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1902 | - | yes | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_SHOT_HUDLESS` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1080 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:105 | paths, config, build, launcher or Lab plumbing |
| `SOTC_SHOT_OVERLAY` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/bbgpu.cpp:757 | - | - | 0 | 1 docs/sotc/REVISAO-DO-PROJETO.md:150 | paths, config, build, launcher or Lab plumbing |
| `SOTC_SHOT_SEQ` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:441 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:105 | paths, config, build, launcher or Lab plumbing |
| `SOTC_SRT_PHI` | KEEP-DEFAULT-ON | on | gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp:837 | flag_on/bind | yes | 1 | 1 CHANGELOG.md:562 | on in the code or the launcher without a user action (default read from code) Resolve branch values in shaders |
| `SOTC_STACK_GUARD` | DELETE-UNUSED | value | gpu/shadps4/video_core/page_manager.cpp:112 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_STACK_SLOTS` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_STAGE_BINDINGS` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/shader_recompiler/info.cpp:40 | flag/bind | yes | 1 | 3 CHANGELOG.md:571 | exposed in the launcher Per-stage resource bindings |
| `SOTC_STAGING_BLOCK_MB` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:32 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_STAGING_CACHED` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:117 | - | yes | 0 | 8 CHANGELOG.md:31 | documented or set by tools only; no measured result found |
| `SOTC_STAGING_KEEP_MB` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:276 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_STAGING_LARGE_KEEP_MB` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:54 | value/upload | yes | 1 | 1 CHANGELOG.md:751 | exposed in the launcher Idle large upload memory (MB) |
| `SOTC_STAGING_LARGE_POW2` | KEEP-DEFAULT-ON | on | gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:47 | flag_on/upload | yes | 2 | 2 CHANGELOG.md:20 | on in the code or the launcher without a user action (default read from code) Reuse large upload buffers |
| `SOTC_STALE_CHECK` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/sotc/stale_check.cpp:107 | - | - | 0 | 25 CHANGELOG.md:129 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_STALL_DETAIL` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:35 | flag/diag | yes | 1 | 2 CHANGELOG.md:332 | report counters and printers Stall details in the log |
| `SOTC_STALL_MS` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:1705 | - | yes | 0 | 4 CHANGELOG.md:581 | documented or set by tools only; no measured result found |
| `SOTC_STATIC_CLASS` | DELETE-AT-NATIVE | no reader | - | - | - | 12 | 11 CHANGELOG.md:109 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_STREAM_THRESHOLD` | DELETE-UNUSED | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:5945 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_SYNC_FAST` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:274 | flag/memory | yes | 2 | 18 CHANGELOG.md:243 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Skip scanning memory that has not changed |
| `SOTC_SYNC_PIPE` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:303 | choice/memory | yes | 1 | 5 CHANGELOG.md:243 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Single-pass memory sync |
| `SOTC_SYNC_PIPELINE_MS` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:723 | - | yes | 0 | 3 docs/re-stutters/reports/07-first-use.md:44 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_SYNC_PRECHECK` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:7455 | flag/memory | yes | 1 | 2 CHANGELOG.md:315 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Quick check before memory sync |
| `SOTC_SYS_MODULES` | KEEP-INFRA | no reader | - | - | - | 7 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_T` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_TAA_OFF` | DELETE-UNUSED | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2286 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_TASK_GUARD` | UNKNOWN-MEASURE | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:3314 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:90 | documented or set by tools only; no measured result found |
| `SOTC_TC_BATCH` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.h:289 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_TC_BIASED` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.h:204 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_TC_SKIP_EMPTY` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:474 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_TEXCAP_AFTER` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4997 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_TEX_BIND_FAST` | DELETE-NO-GAIN | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:3330 | - | yes | 3 | 5 docs/re-stutters/reports3/draw/switches-review.md:4 | draw-thread switch, no measured gain (switches-review.md) |
| `SOTC_TEX_BIND_P1` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/texture_cache/image.cpp:500 | - | yes | 0 | 0  | on in the code or the launcher without a user action (default read from code) |
| `SOTC_TEX_BLOCK` | KEEP-USER-OPTION | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:5764 | choice/bind | yes | 2 | 8 CHANGELOG.md:247 | exposed in the launcher Reuse texture blocks |
| `SOTC_TEX_DUMP` | MERGE-INTO-FPS | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:246 | - | yes | 0 | 0  | report counters and printers |
| `SOTC_TEX_REPLACE` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:246 | - | yes | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_THREAD_IDS` | MERGE-INTO-TRACE | check (inf) | src/runtime_thread.c:507 | - | - | 0 | 1 CHANGELOG.md:300 | timeline and trace spans |
| `SOTC_TIMER_RES` | KEEP-DEFAULT-ON | on | src/probe.c:1625 | flag_on/threads | - | 1 | 1 CHANGELOG.md:551 | on in the code or the launcher without a user action (default read from code) Windows timer at 1 ms |
| `SOTC_TINY_ASYNC` | UNKNOWN-MEASURE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:619 | - | yes | 0 | 1 docs/re-stutters/reports3/draw/wave1/p1b.md:156 | documented or set by tools only; no measured result found |
| `SOTC_TRACE` | KEEP-CONSOLIDATED | off (set enables) (inf) | gpu/shadps4/video_core/sotc_trace.h:45 | choice/diag | yes | 15 | 42 CHANGELOG.md:30 | consolidation target (report or timeline) Record timeline (tracing) |
| `SOTC_TRACE_C_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_TRACE_DIR` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shim/sotc_trace.cpp:674 | value/diag | - | 8 | 3 docs/sotc/LAB.md:211 | timeline and trace spans Timeline folder |
| `SOTC_TRACE_FUNCS` | MERGE-INTO-TRACE | no reader | - | value/diag | - | 8 | 5 CHANGELOG.md:51 | timeline and trace spans Timed eboot functions (file) |
| `SOTC_TRACE_FUNC_MIN_US` | MERGE-INTO-TRACE | value | gpu/sotc/hooks.cpp:691 | - | - | 0 | 1 docs/HOOK-DESIGN.md:84 | timeline and trace spans |
| `SOTC_TRACE_GPU` | MERGE-INTO-TRACE | off (1 enables) (inf) | gpu/shim/sotc_gpu_trace.h:35 | flag/diag | - | 1 | 6 CHANGELOG.md:42 | timeline and trace spans Real GPU times in the timeline |
| `SOTC_TRACE_IMPORTS` | MERGE-INTO-TRACE | off (1 enables) (inf) | src/runtime_trace_imports.c:338 | choice/diag | - | 4 | 3 CHANGELOG.md:49 | timeline and trace spans PS4 system calls (imports) |
| `SOTC_TRACE_IMPORTS_H` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_TRACE_IMPORT_MIN_US` | MERGE-INTO-TRACE | value | src/runtime_trace_imports.c:341 | - | - | 3 | 1 docs/sotc/README-SotC.md:199 | timeline and trace spans |
| `SOTC_TRACE_JSON` | MERGE-INTO-TRACE | off (1 enables) (inf) | gpu/shim/sotc_trace.cpp:63 | - | - | 0 | 2 docs/sotc/LAB.md:208 | timeline and trace spans |
| `SOTC_TRACE_MAX` | MERGE-INTO-TRACE | value | gpu/shim/sotc_trace.cpp:280 | value/diag | - | 1 | 4 CHANGELOG.md:378 | timeline and trace spans Maximum timeline files |
| `SOTC_TRACE_MS` | MERGE-INTO-TRACE | check (inf) | gpu/shim/sotc_trace.cpp:277 | value/diag | - | 3 | 3 CHANGELOG.md:375 | timeline and trace spans Slow frame threshold (ms) |
| `SOTC_TRACE_ONLY` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shim/sotc_trace.cpp:118 | - | - | 0 | 2 CHANGELOG.md:40 | timeline and trace spans |
| `SOTC_TRACE_POST_MS` | MERGE-INTO-TRACE | value | gpu/shim/sotc_trace.cpp:279 | value/diag | - | 1 | 3 CHANGELOG.md:376 | timeline and trace spans Time recorded after a slow frame (ms) |
| `SOTC_TRACE_PRE_MS` | MERGE-INTO-TRACE | value | gpu/shim/sotc_trace.cpp:278 | value/diag | - | 1 | 3 CHANGELOG.md:376 | timeline and trace spans Time recorded before a slow frame (ms) |
| `SOTC_TRACE_RING` | MERGE-INTO-TRACE | value | gpu/shim/sotc_trace.cpp:283 | value/diag | - | 1 | 2 docs/sotc/LAB.md:209 | timeline and trace spans Event ring size (multiplier) |
| `SOTC_TRACE_SAMPLE` | MERGE-INTO-FPS | check (inf) | gpu/shim/sotc_sampler.cpp:525 | value/diag | - | 1 | 4 CHANGELOG.md:50 | report counters and printers Stack sampling (Hz) |
| `SOTC_TRACE_SAMPLE_GUEST` | MERGE-INTO-TRACE | check (inf) | gpu/shim/sotc_sampler.cpp:546 | - | - | 0 | 0  | timeline and trace spans |
| `SOTC_TRACE_SCOPES` | MERGE-INTO-TRACE | no reader | - | flag/diag | - | 4 | 2 CHANGELOG.md:49 | observe-only hook probe (timeline or counters) Engine measurement scopes as spans |
| `SOTC_TRACE_ZONES` | MERGE-INTO-TRACE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_trace.h:58 | flag/diag | yes | 1 | 3 CHANGELOG.md:47 | timeline and trace spans shadPS4 zones in the timeline |
| `SOTC_TRACE_ZONE_MIN_US` | MERGE-INTO-TRACE | value | gpu/shadps4/video_core/sotc_trace.h:63 | - | yes | 2 | 2 docs/sotc/LAB.md:210 | timeline and trace spans |
| `SOTC_TRAMPOLINE_SIZE` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_TSC` | UNKNOWN-MEASURE | off (set enables) (inf) | src/probe.c:1233 | - | - | 0 | 1 docs/sotc/README-SotC.md:167 | documented or set by tools only; no measured result found |
| `SOTC_TSHARP_ARRAY` | UNKNOWN-MEASURE | value | gpu/shadps4/shader_recompiler/info.cpp:76 | - | yes | 0 | 1 CHANGELOG.md:1193 | documented or set by tools only; no measured result found |
| `SOTC_TSHARP_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:6273 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_TUNING_JSON` | KEEP-INFRA | no reader | - | - | - | 0 | 1 docs/sotc/README-SotC.md:175 | paths, config, build, launcher or Lab plumbing |
| `SOTC_TUNING_TSV` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/sotc_lab_menu.cpp:297 | - | - | 3 | 1 docs/sotc/LAB.md:339 | paths, config, build, launcher or Lab plumbing |
| `SOTC_UD_SWAP` | DELETE-NO-GAIN | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:1250 | - | yes | 1 | 8 docs/re-stutters/reports3/draw/memmove.md:81 | draw-thread switch, no measured gain (switches-review.md) |
| `SOTC_UI_HIRES` | KEEP-DEFAULT-ON | on (0 disables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:1115 | - | yes | 0 | 1 docs/sotc/README-SotC.md:168 | on in the code or the launcher without a user action (default read from code) |
| `SOTC_UI_LANG` | KEEP-INFRA | off (set enables) (inf) | src/runtime_event.c:24 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_UI_LINEAR` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:6675 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_UI_LINEAR_TRACE` | MERGE-INTO-TRACE | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:6705 | - | yes | 0 | 0  | timeline and trace spans |
| `SOTC_FENCE_PIECES` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/async_upload.cpp:50 | flag/upload | yes | 1 | 3 CHANGELOG.md:760 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Write fence waits only for needed pieces |
| `SOTC_HASH_EPOCH` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:6330 | flag/upload | yes | 1 | 2 CHANGELOG.md:315 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Skip hash lock outside the range |
| `SOTC_HASH_SMALL` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/sotc_stats.h:710 | value/upload | yes | 1 | 4 CHANGELOG.md:355 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Skip unchanged small buffer upload |
| `SOTC_UNBIND_GPU_WAIT` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:8515 | flag/memory | yes | 1 | 4 CHANGELOG.md:564 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Unbind memory without waiting for the GPU |
| `SOTC_UNMAP_PARK` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/texture_cache/texture_cache.h:89 | - | yes | 0 | 3 docs/re-stutters/reports2/00-PLAN.md:70 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_UNMAP_PARK_MB` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/texture_cache/texture_cache.h:97 | - | yes | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_UNMAP_READBACK` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:7262 | - | yes | 0 | 3 CHANGELOG.md:1193 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_UNTRACK_AUTO` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:251 | flag/memory | yes | 1 | 5 CHANGELOG.md:290 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Leave temporary memory unprotected |
| `SOTC_UNTRACK_LOG` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/core/libraries/videoout/driver.cpp:1056 | flag/diag | yes | 1 | 2 CHANGELOG.md:361 | diagnostic of a UMA family: deleted with its family Log per-frame memory blocks |
| `SOTC_UNTRACK_MAX_MB` | DELETE-AT-NATIVE | check (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:30 | value/memory | yes | 2 | 1 CHANGELOG.md:328 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Unprotected memory cap (MB) |
| `SOTC_UNTRACK_RANGES` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/buffer_cache/region_manager.h:364 | value/memory | yes | 1 | 5 CHANGELOG.md:360 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Unprotected memory ranges |
| `SOTC_UPDATE_DIR` | KEEP-INFRA | off (set enables) (inf) | src/runtime_file.c:117 | - | - | 4 | 1 CHANGELOG.md:1237 | paths, config, build, launcher or Lab plumbing |
| `SOTC_UPDATE_URL` | KEEP-INFRA | no reader | - | - | - | 8 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_UPLOAD_BUDGET_MB` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:2199 | - | yes | 0 | 5 CHANGELOG.md:808 | documented or set by tools only; no measured result found |
| `SOTC_UPSCALER` | KEEP-INFRA | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp:892 | - | yes | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_USER_DIR` | KEEP-INFRA | no reader | - | - | - | 4 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_USLEEP_YIELD` | UNKNOWN-MEASURE | off (set enables) (inf) | src/runtime_kernel.c:157 | - | - | 0 | 10 docs/re-stutters/reports2/00-PLAN.md:44 | documented or set by tools only; no measured result found |
| `SOTC_USLEEP_YIELD_US` | UNKNOWN-MEASURE | value | src/runtime_kernel.c:158 | - | - | 0 | 4 docs/re-stutters/reports2/05-frame-pacing.md:165 | documented or set by tools only; no measured result found |
| `SOTC_VK_NAMES` | DELETE-UNUSED | off (set enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:116 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_VK_POOL` | DELETE-NO-GAIN | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:33 | - | yes | 2 | 5 docs/re-stutters/reports3/draw/switches-review.md:4 | draw-thread switch, no measured gain (switches-review.md) |
| `SOTC_VMA_BLOCK_MB` | DELETE-UNUSED | value | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:1335 | - | yes | 0 | 0  | vendored shadPS4 option: nothing in the port sets it, no docs |
| `SOTC_VOLUME` | KEEP-INFRA | value | src/runtime_audio.c:157 | - | - | 1 | 1 docs/sotc/ROADMAP.md:33 | paths, config, build, launcher or Lab plumbing |
| `SOTC_VRAM_GC_PERCENT` | KEEP-USER-OPTION | value | gpu/shadps4/video_core/texture_cache/texture_cache.cpp:326 | - | yes | 1 | 1 docs/sotc/ROADMAP.md:26 | exposed in the launcher |
| `SOTC_WAIT_MARKER` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp:49 | - | yes | 0 | 2 CHANGELOG.md:1288 | documented or set by tools only; no measured result found |
| `SOTC_WAIT_NOSPIN` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:247 | flag/threads | yes | 1 | 2 CHANGELOG.md:566 | exposed in the launcher Wait without spinning the CPU |
| `SOTC_WAIT_SPIN_US` | UNKNOWN-MEASURE | value | gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp:23 | - | yes | 0 | 2 docs/re-stutters/reports3/09-draw-freeze.md:18 | documented or set by tools only; no measured result found |
| `SOTC_WATCHDOG` | KEEP-INFRA | on (0 disables) (inf) | src/probe.c:1964 | - | - | 0 | 1 CHANGELOG.md:962 | paths, config, build, launcher or Lab plumbing |
| `SOTC_WAVE64_IDENTITY` | UNKNOWN-MEASURE | value | gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp:241 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:41 | documented or set by tools only; no measured result found |
| `SOTC_WAVE64_UNIFORM` | UNKNOWN-MEASURE | value | gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp:204 | - | yes | 0 | 1 docs/sotc/ROADMAP.md:41 | documented or set by tools only; no measured result found |
| `SOTC_WD_FUSE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/amdgpu/liverpool.cpp:265 | flag/threads | yes | 1 | 2 CHANGELOG.md:532 | exposed in the launcher Small write packets ride along with draws |
| `SOTC_WIDE_ASPECT` | KEEP-INFRA | no reader | - | - | - | 1 | 2 CHANGELOG.md:844 | paths, config, build, launcher or Lab plumbing |
| `SOTC_WINDOW_BEHIND` | KEEP-INFRA | off (1 enables) (inf) | gpu/shim/window.cpp:89 | - | - | 1 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_WINDOW_ICON` | KEEP-INFRA | off (set enables) (inf) | gpu/shim/window.cpp:103 | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_WORKER_LOW_PRIO` | KEEP-INFRA | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:824 | flag/threads | yes | 1 | 1 CHANGELOG.md:554 | paths, config, build, launcher or Lab plumbing Compile pipelines at low priority |
| `SOTC_WQM_COPY` | UNKNOWN-MEASURE | value | gpu/shadps4/shader_recompiler/frontend/translate/scalar_alu.cpp:114 | - | yes | 0 | 1 docs/sotc/AGUA.md:64 | documented or set by tools only; no measured result found |
| `SOTC_WRAP` | KEEP-INFRA | no reader | - | - | - | 3 | 1 docs/sotc/AGUA.md:68 | paths, config, build, launcher or Lab plumbing |
| `SOTC_WRITE_AHEAD_ADAPT` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_stats.h:768 | flag/memory | yes | 1 | 4 CHANGELOG.md:517 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Adaptive write-ahead window |
| `SOTC_WRITE_AHEAD_ADAPT_US` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/sotc_stats.h:772 | value/memory | yes | 1 | 1 CHANGELOG.md:524 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Sequential write interval (µs) |
| `SOTC_WRITE_AHEAD_MAX_PAGES` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/sotc_stats.h:777 | value/memory | yes | 1 | 1 CHANGELOG.md:526 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Maximum write-ahead window (pages) |
| `SOTC_WRITE_AHEAD_PAGES` | DELETE-AT-NATIVE | value | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:824 | - | yes | 0 | 1 docs/re-stutters/reports/03-cpu-gpu-writers.md:62 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_WRITE_DIET` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:1972 | choice/readback | yes | 1 | 5 CHANGELOG.md:240 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Readbacks only for the written range |
| `SOTC_WRITE_FAST` | DELETE-AT-NATIVE | on (0 disables) (inf) | src/runtime_memory.c:1928 | - | - | 0 | 3 CHANGELOG.md:229 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_WRITE_FAULT_NARROW` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:251 | flag/memory | yes | 1 | 2 CHANGELOG.md:412 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Write fault reads back only the touched page |
| `SOTC_WRITE_DECL` | DELETE-AT-NATIVE | value | src/runtime.c:178 | - | - | 0 | 1 docs/re-stutters/reports/10-port-redesign.md:113 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_WRITE_DECL_MAX` | DELETE-AT-NATIVE | check (inf) | gpu/sotc/write_decl.cpp:188 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_WRITE_DECL_MIN` | DELETE-AT-NATIVE | value | src/runtime.c:179 | - | - | 0 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) |
| `SOTC_WRITE_EXTENT` | DELETE-AT-NATIVE | off (1 enables) (inf) | gpu/shadps4/video_core/sotc_write_extent.h:50 | flag/memory | yes | 1 | 2 CHANGELOG.md:540 | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) Narrow written range for listed shaders |
| `SOTC_WRITE_EXTENT_FILE` | DELETE-AT-NATIVE | off (set enables) (inf) | gpu/shadps4/video_core/sotc_write_extent.h:66 | value/memory | yes | 1 | 0  | UMA emulation family (sync/protect/readback/ring/class/fault/untrack) GPU write rules file |
| `SOTC_X` | KEEP-INFRA | check (inf) | gpu/sotc/lab_commands.h:27 | - | - | 2 | 1 docs/sotc/LAB.md:147 | paths, config, build, launcher or Lab plumbing |
| `SOTC_XFER_QUEUE` | KEEP-USER-OPTION | off (1 enables) (inf) | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:687 | flag/upload | yes | 1 | 2 CHANGELOG.md:569 | exposed in the launcher Transfer queue for uploads |
| `SOTC_Y` | KEEP-INFRA | no reader | - | - | - | 2 | 0  | paths, config, build, launcher or Lab plumbing |
| `SOTC_ZONE_CAT` | KEEP-INFRA | no reader | - | - | - | 0 | 0  | paths, config, build, launcher or Lab plumbing |

## Limits and open items

- Readers that do not quote the name (macros, `constexpr` names built from a switch string) show `-`. Their class still comes from the family rule, but the default is unverified.
- Defaults marked `(inf)` are read from the code text, not executed. `SOTC_MINIDUMP` and others with a value-style read are the first to check. A deletion is safe only after the default is confirmed in the code.
- `SOTC_STATIC_CLASS` mode 3 is written but not wired (HANDOFF). `SOTC_RT_CLASS` has no VRAM-only path yet. Their doc status is in PC-MEMORY section 8.
- UNKNOWN-MEASURE rows (82) need a measurement per row before deletion. Priority: SOTC_COMMIT_CHUNKS, SOTC_COMMIT_ON_DEMAND (about 3 GB), the LOAD_WARM and AREA_PREWARM family, CULL_EXPRESS (incomplete), and the BB_FSR4 and REACTIVE set (vendored upscaler options).
- UNKNOWN-DECISION (PC_POOL_NATIVE family, PC_PLACE, PC_RECYCLE): these are the allocator candidates for the native model; they are not deleted by this audit until the user decides.
- The `SOTC_VERIFY` target does not exist yet. Creating it is a code change and is listed as a required change, not made here.
- DELETE-DEAD names are constants, comments or test-only names; removing them is a cleanup of the grep surface, with no runtime effect.

