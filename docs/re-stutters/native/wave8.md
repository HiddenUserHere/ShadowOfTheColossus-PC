# Wave 8 results (1 Nov)

Brief: WAVE8-BRIEF.md. Seven Haiku specialists in worktrees; integration, builds and game runs by the coordinator.
Lab route tests.json, one round each.

| item | result | commit |
|---|---|---|
| Decal masks | served from the last completed copy (producer 0x19ce08, label reset 0xc54c7) | 33a2b96 |
| Unclassified readbacks (0xf9c40, 0x112306, 0x19b435, 0x17b02, 0x135719) | all CPU data on GPU-written pages (dirty-set upload records, a CPU state block, a pool slot index, heap free-list metadata, a std::map bounds cache); no consumer patch; gone with N4 | (finding) |
| N4 step 1: SOTC_NATIVE=1 without page tracking | write faults 0, readbacks 0, stray faults 0 | aff17f2 |
| N4 dead code | STALE_CHECK, STATIC_CLASS, RB_HOST, PARTICLE_PREV modes 1-2, HOOK_PSTATS deleted (-1750 lines) | (see log) |
| N5 parallel record | SOTC_N5_PARALLEL=2 dry run. The pass consumer (recorder, ~9.5 ms/frame) is not on the critical path; the draw thread (~35 ms) is, so a pass worker pool gains nothing until per-op work leaves the draw thread | c90480f |
| N5 flat-buffer skip | not integrated: the recording never memoized (granules read 0, 80% "not memoized"), so no gain, and it changed the SRT walker JIT for every user and the shader cache version | (dropped) |
| N5 async compute | SOTC_N5_ASYNC_COMPUTE=1 (opt-in; needs SOTC_N5_GRAPH, SOTC_PARALLEL_RECORD): 3-4 of ~1100 dispatches per frame move (most bind images or depend on graphics), no device loss; no measurable gain | d6723a7 |

## Route numbers, SOTC_NATIVE=1 SOTC_NATIVE_MASKS=prev SOTC_NATIVE_PARTICLES=prev

| run | FPS | p99 ms | max ms | >50 ms | readbacks/s | write faults |
|---|---|---|---|---|---|---|
| defaults (reference) | 26.1 | 75.5 | 141.7 | 8 | 538 | ~6k/s |
| native, tracking on | 28.4 | 76.5 | 146.8 | 10 | 181 | ~6k/s |
| native, tracking off (aff17f2) | 29.7 | 38.7 | 114.0 | 1 | 0 | 0 |

Final gate (p99 <= 36 ms, max < 50 ms over the route and a 20-minute session): not met yet (p99 38.7, one 114 ms
spike), and not measured over 20 minutes. The visual check of tracking off (stale textures or geometry after area
loads, CPU-written data the announces miss) is pending.

Remaining N4 step 2 (deletions once mode 1 is the default): the tracker's protect/fault/readback paths listed by the
tracking specialist (page_manager write branch, granules, write-ahead, protect batch, readback families in
buffer_cache, TexProtect / TrackImage in texture_cache), RB_CLASS, PC_RING modes 1-7, mem_classes, PC_PLACE glue,
ARENA_ATTRIB, WRITE_DECL, label_honest, and their switches.
