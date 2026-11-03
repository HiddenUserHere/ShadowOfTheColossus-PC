# RE of the BPE renderer: summary and decision (2 Nov)

Question: can the port reach 60 FPS by replacing or bypassing the engine's GNM layer, and where are the GPU ops
of a frame coming from? Reports 01-09 in this folder hold the evidence (observed / inferred / unknown labels).

## Decision

No native backend. The engine has no backend interface to replace (01): its GNM wrappers take raw GNM arguments,
about 233 engine functions write PM4 directly (03), and per-draw descriptors are copied into bump memory by the
engine (05, 06). A native backend would mean rewriting the BPE renderer. Instead, use the RE to cut ops and per-op
work while the port keeps translating PM4.

## Where the ops come from (test route, gameplay at 40-50 s, SOTC_PASS_STATS=1, SOTC_DRAW_SITES=1)

| Pass / source | Draws or dispatches per frame |
|---|---|
| Depth pre-pass (2048x1152, depth only, job 0xc1860) | 3,413 |
| G-buffer (7 colour targets) | 1,967 |
| Sun shadow map (all cascades) | 461 |
| Compute dispatches | ~1,300-1,500 |
| Of the draws: BPXPR_ConsumeVisibilityMask (0x1a6d20), one DrawIndexIndirect per visible object | ~3,980 (57%) |

The sun shadow is 6% here (78% in the Shrine-type scenes measured in docs/re-stutters/reports2/14).

## Levers

| Lever | Evidence | Gain on the route | Image |
|---|---|---|---|
| Content-keyed multi-draw in the port | SOTC_DRAW_MERGE_STATS=2: 291,582 of ~728,000 ops (40%) join runs when resources are keyed by content (key D + flat buffer without user data); keyed by bound descriptors: 0 runs, because 06 shows the engine copies the same descriptors to a new bump address for every dirty draw. Runs end mostly on a pipeline change (material). | ~40% of draw-thread ops | Identical (same draws, gathered arguments) |
| Depth pre-pass off (04) | Queued unconditionally (mask 0x1f stored at 0xc5abd), no cvar. G-buffer likely tests EQUAL, so the G-buffer depth state must change to LESS_EQUAL with writes on. | up to ~45% of ops | Must be verified; risky (alpha lists, consumers) |
| XPR loop in the engine (0x1a6d20) | Per visible object: flush (0x85fc10) + DrawIndexIndirect at offset object*0x14; objects grouped by material and mesh | Would also cut CP ops | Needs the cull-shader argument layout first |
| Texture lifecycle events (08) | Base address written only at creation; streaming changes base/last level via 0x880c30; no relocation found; RTs named kRTT_* and registered | Per-op texture lookups and write tracking | Identical |
| Frame rings as CPU-written-only memory (09) | Two rings rotated once per frame in EndFrameViewParams (0x48270): "RenderContextFrameCPU" 130 MiB and "RenderContextFrameGPU" 20 MiB, set up by 0x481f0 (base and size in its args). Caveat: the GPU does write into DCB memory (EOP labels, WAIT_REG_MEM sources, NOP payloads), so the per-flip DCB/CCB must stay tracked | Write tracking and sync for ring ranges | Identical |
| Shadow cascade caching | 14-draw-sources.md | Small here, large in shadow-heavy scenes | Small lag in far cascades |

## Next step

Implement the content-keyed multi-draw (port side, verify mode =2): consecutive draws with the same pipeline,
targets, dynamic state and content key, no barrier between, are recorded as one vkCmdDrawIndexedIndirect over a
gathered argument array (one vkCmdCopyBuffer with N regions per run). The bind runs once per run.
