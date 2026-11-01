# N4 diagnostics consolidation (n4diag): result

Saved by the coordinator from the specialist's hand-back; integrated in commit adec598. Rules: N4-DIAG-BRIEF.md.

## Merged (11)
- SOTC_FPS: SOTC_CAM_LOG, SOTC_GLYPH_LOG, SOTC_SHADOW_REUSE_STATS, SOTC_HOOKS_LOG.
- SOTC_TRACE: SOTC_PASS_TRACE, SOTC_INDIRECT_TRACE, SOTC_TSHARP_TRACE, SOTC_UI_LINEAR_TRACE, SOTC_FLIP_TRACE.
- SOTC_VERIFY=1: SOTC_BIND_DIFF, SOTC_PR_VERIFY (now every prepared draw is checked and the inline values are used:
  heavy).

## Kept, by class
- KEEP-TOOL (dumps, triggers): BB_DUMP_SHADERS, SOTC_CS_DUMP, SOTC_DUMP, SOTC_TEX_DUMP, SOTC_SHOT_COUNT,
  SOTC_MENU_TRIGGER, SOTC_RDOC_SUBMITS, SOTC_RDOC_TRIGGER, SOTC_SET_TRIGGER.
- KEEP-PARAM (settings of one tool): SOTC_CS_WATCH, SOTC_CS_TRACE, SOTC_FPS_LEAN, SOTC_FRAME_LOG,
  SOTC_TRACE_SAMPLE, SOTC_EVF_TRACE, SOTC_LOD_TRACE, SOTC_PROFILE(_DELAY/_OUT/_SECONDS), SOTC_TRACE_DIR, _FUNCS,
  _FUNC_MIN_US, _IMPORT_MIN_US, _JSON, _MAX, _MS, _ONLY, _POST_MS, _PRE_MS, _RING, _SAMPLE_GUEST, _ZONE_MIN_US.
- NOT-MERGED, not cheap or changes behaviour:
  - SOTC_CENSUS: walks every cached image under the texture-cache lock every 5 s.
  - SOTC_CMD_STATS: also switches the texture-cache lock path, device-memory callbacks and protect counters;
    =2 is a per-dispatch hash map.
  - SOTC_CS_STATS, SOTC_MDI_STATS: per-dispatch/per-draw work.
  - SOTC_OPSTAT, SOTC_OP_PHASES: per-op clock reads.
  - SOTC_STALL_DETAIL: per-frame thread CPU times.
  - SOTC_LOD_POOL_LOG: its counter paces the linger check.
  - SOTC_DLSS_PROBE: also disables the bind-cache block memo.
  - SOTC_TRACE_GPU, SOTC_TRACE_ZONES, SOTC_TRACE_IMPORTS: recording cost; SOTC_TRACE_IMPORTS is a candidate for a
    SOTC_TRACE_ONLY list.
  - SOTC_LOD_STATS, SOTC_POOL_WATCH, SOTC_RAM_REPORT: default on.
  - SOTC_REUSE_SIZE_CHECK: a default-on correctness fix, not a verify mode.
  - SOTC_HOOK_PSTATS: read by scripts/sotc_modes.py and changes the patched code.
  - SOTC_DLSSG_LOG, SOTC_FSRFG_LOG: SDK log levels.
- NOT-MERGED, needs an engine or C change first:
  - Hook-record switches: SOTC_BUILDSYNC_PROBE, SOTC_CAM_PROBE, SOTC_DRAW_TAG, SOTC_DT_PROBE, SOTC_HOOK_CVAR_PROBE,
    SOTC_HOOK_HEAP_RANGE, SOTC_HOOK_LOADER_NOSTREAM, SOTC_HOOK_LOADER_PROBE, SOTC_TRACE_SCOPES. They need a
    presence rule in the hook engine (a record that follows SOTC_TRACE) and build-time gating in sotc_modes.py.
  - C readers: SOTC_FIBER_STACKS, SOTC_SAVE_TRACE, SOTC_THREAD_IDS. They need a C-side SOTC_TRACE check.
  - SOTC_PC_POOL: hook-engine switch; the pool2 matrix preset sets =2.
- No reader found: BB_FSR4_STATS, BB_FSR4_PROFILE (names only in docs).

## Behaviour notes
SOTC_FPS=1 prints four more kinds of line. SOTC_VERIFY=1 changes rendering to the inline prepared values (the verify
semantic). With none of the three set nothing changes.
