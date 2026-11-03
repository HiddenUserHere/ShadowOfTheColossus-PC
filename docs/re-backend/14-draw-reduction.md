# 14 - Draw and dispatch reduction: where the ops are, and which cuts keep the image

Read-only analysis. Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`, `tools/re/eboot_re.py` (dis, callers, refs,
fstr), the port source under `gpu/shadps4/video_core/`, and reports 00, 04, 11, 12, 13, `reports2/04`, `reports2/14`,
`reports2/15`, `reports3/02`. No build, no game run, no commit. Addresses are eboot VAs (EU 1.01).

Labels: **observed** (decompile or source line at the address given, or a measured number from the brief/reports),
**inferred** (reasoning from observed code plus the measured totals), **unknown** (not decided by the files read).

## 0. Summary

- The target (under 3,000 ops per frame) is **not reached by image-identical changes alone** in this estimate.
  The image-identical set (section 5, R1, R2, R4, R6, R7) gives about 3,100-3,500 ops. Getting under 3,000 needs
  the depth-only object lists (R3, about 1,300 ops, high risk) or the far sun cascades (R5, blink risk).
- The biggest safe lever is **draw merging of the XPR per-object draws**. The port already has it as opt-in
  code (`SOTC_DRAW_MDI`, section 3.5). It is image-identical by construction, and it is worth about 40% of draws.
- The **pre-pass XPR drop** (`SOTC_NO_PREPASS=3`) is also already in the port, but it is opt-in and its image
  safety depends on whether `ResolveSunShadows` reads the main depth (report 13 is still provisional). Run the
  existing probe `SOTC_PREPASS_PROBE` before enabling it.
- Two premises of the brief need correcting: `0x1a7290` is not a second dispatch (it is an alternate draw
  consumer), and the per-object compute dispatches are in the index-cull job, which is probably off with the
  launcher defaults (section 2).

## 1. Op accounting (per frame, test route)

Baseline from the brief: about 5,600 draw ops, and 1,300-1,500 compute dispatches. The brief's table (3,413 + 1,967 +
461 = 5,841) matches the draw count within 5%. Dispatch cost is about 3.9 us each (report `reports2/04`, BIND-PATH),
draw cost about 4 us (brief), so both count as ops. Working total: about **7,000 ops**. Target: under 3,000.

| Bucket | Draws per frame | Label | Basis |
|---|---|---|---|
| XPR per-object, pre-pass (`0x11d7a0` with masks 0x10, 0x11, 0x12, inside job `0xc1860`) | ~1,900 | observed count (brief), ~210 alpha-tested | brief |
| Object lists 0-4 in pre-pass (`0x11d3d0`, one draw per object, `0xc1860` bits 0x01, 0x02, 0x04, 0x08, 0x10) | ~1,300-1,400 | inferred: 3,413 minus XPR, VG, particles | section 4, R3 |
| Virtual geometry and particles in pre-pass (`0xdb700` x2, `0x10aee0`) | ~100 | inferred remainder | unknown split |
| XPR per-object, G-buffer (`0x11d7a0` masks 0x13 in `0xc1ce0`, 0x14 in `0xc1fc0`) | ~1,900 | inferred: brief says all pre-pass XPR draws are redrawn | observed count of G-buffer total 1,967 |
| G-buffer object lists 5-8, VG, particles | ~70 | inferred: 1,967 minus XPR | depends on the redraw claim |
| Sun shadow, 5 cascades (`0xce850`), all kinds | 461 | observed (brief) | route; 78% in Shrine scenes (reports2/14) |
| XPR per-object total (all views) | ~3,980 (57%) | observed (brief) | sum of the rows above is about 3,800 + 180 in shadows |
| Compute dispatches | 1,300-1,500 | observed total, composition **unknown** | section 2.3 |

Split of the XPR per-object draws: one per visible object (observed in `0x1a6d20`, section 2.1). The masked
lists are called from `0xc1860`, `0xc1ce0`, `0xc1fc0`, `0xce850` (cascades), `0xcd330` (spot shadows) and
`0xc46c0` (debug view only, section 6).

## 2. Corrections and new facts from the decompile

### 2.1 The XPR consumer (`0x1a6d20`) emits one draw per visible object (observed)

- Loop over the set bits of the 2048-bit mask (`param_2`) (`0x1a6d20` lines 40-190 of the decompile). For each
  visible bit `objIdx`: the material and mesh group-change blocks run only on a change of group (state only), then
  `FUN_0085fc10(ctx+0x2e8)` (material flush, dirty-gated, observed) and **one** `FUN_0086b3d0(ctx, objIdx*0x14,
  stage, vsSlot, isSlot, 0)`, gated by the shader kind `[ctx+0xc548]` (values 0, 0x45, 0xad, 0xb0, 0x145, 0x1ad;
  other kinds emit nothing).
- So the group is a state group, not a draw group. Every visible object is one indirect draw with data offset
  `objIdx * 0x14`. The brief's description holds.
- Bounds: `plVar5[0]` (material exclusive ends), `plVar5[1]` (mesh exclusive ends), count `[entry+0x60]`, object
  count `[entry+0x64]` (report 11, section 2.1). The scans only move forward, so the code assumes ascending bounds.
- Membership is tested here (`plVar5[4]` bit test, line 77 of the decompile, observed). The emitters `0x1a8180` and
  `0x1a85f0` do not test it (report 11, section 1.5).

### 2.2 `0x1a7290` is an alternate consumer, not a second dispatch (observed, correction)

- `0x19af20` (`BPXPR_SelectMaskSource`) calls `0x1a6d20` when `(uVar2 & 0xff00000000) == 0` and (the variant is 0 or 7,
  or `[cfg+0x22] == 0`), otherwise `0x1a7290` (decompile `0x19af20`, the last branch).
- `0x1a7290` has the same structure: membership test, one `FUN_0086b3d0` per visible object (line 151 of its
  decompile), the same shader-kind gate. It is a second **draw** consumer selected by a config byte, and it emits
  the same number of draws as `0x1a6d20` for the same mask. Count it as draws, not dispatches.

### 2.3 The per-object compute dispatches are in the index-cull job, which is probably off by default (observed/inferred)

- `0x1a8180` (and its twin `0x1a85f0`) loop the visible bits and, for each, write a 0x20-byte cull record and call
  `FUN_0086b9e0(ctx, (indexCount/3 + 63) >> 6, 1, 1, ...)` (or `FUN_00883870` in `0x1a85f0`) (observed, lines
  120-160 of `0x1a8180`). That is **one dispatch per visible object per cull job** (observed).
- Those emitters are reached only from `0x19b330` (called from the index-cull job `0x11dbe0`), which is queued from
  two places:
  - main view: `sub_c5130` (`XPR_IndexCullObjectsNormal`) runs only when the byte `*(ctx+0xae3c) != 0`
    (observed gate, decompile `000c5130.c`);
  - sun shadow: `sub_cdb00` runs only when `*(cfg+0x20) != 0` (observed gate, decompile `000cdb00.c`).
- Inferred: those bytes are the cvars `doIndexBufferCulling` and `doSunShadowIndexBufferCulling` (names used in the
  same `RegisterXprCvars` table, report `reports2/15`). The launcher default is 0 for both (`perf_culling`, observed in
  `launcher/sotc_launcher_win.py:642-645`). So with the launcher defaults the per-object cull dispatches probably
  do **not** run.
- Consequence (inferred): the 1,300-1,500 dispatches on the route are probably not the per-object cull. If the
  measurement was taken with the cvars on, they could be a large part of it (up to one dispatch per visible object).
  **Unknown:** what writes the 20-byte records when the cull job is off. The port draws the same image in this
  config, so something does; the answer decides whether the draws in R1 can be merged safely. Check with a
  runtime count of `0x11dbe0` calls per frame (`reports3/02`, section 4).

### 2.4 Other facts that change the candidate list

- **Alpha depth passes (observed, `0xcaed0`).** `MainCamera` calls `sub_c3ec0` with the "TransparentDepth" scope,
  object pass 36 (`kRS_Alpha_Normal_Depth`), unconditionally (lines 476 and 514). It calls `sub_c3a50` (scope "Depth",
  object pass 37 `kRS_DepthPrepassAlpha_Normal`) **twice** when `[ctx+0x5e0] != [ctx+0x5e8]` (lines 478 and 480),
  with `sub_c3c70` (scope "Alpha") between them. Both `c3a50` calls use pass 0x25 (report 04, section 3). The
  depth walk of list 37 therefore runs twice per frame when the list is non-empty. Whether the two walks use the
  same depth constants (argument `param_6`) is **unknown**.
- **Spot shadows are located (observed).** `sub_cc690` ("RenderSpotLightShadows", string at `0xcc6ce`, scope
  `SpotShadows` at `0xcc707`) queues one `RenderSingleSpotShadow` job (`0xcd330`, string `0xcc86f`) per spot slot,
  each with the object passes 0, 1 and 3 and two masked lists. This answers report 14, open question 3. The number
  of active spot lights on the route is **unknown**.
- **`0xc46c0` is a debug view (observed).** It is queued by `MainCamera` (`0xcc188`) only when the view flag `+0x44`
  is set, under the scope "RenderDebug" (`0xc46db`). It draws grass twice, particles and the masked list 0x15. It
  is not a normal draw source; ignore it in the normal route.
- **Grass and particles are drawn in the pre-pass and again in the G-buffer (observed calls).** Pre-pass `0xc1860`
  calls `0xdb700` (grass) once or twice and `0x10aee0` (particles) once. The G-buffer per-probe job `0xc1fc0` does
  the same (`0xdb700` twice, `0x10aee0` once). The two grass calls in each job use different stage modes
  (`(param_4&3)+2` or `param_4&1`), so they may draw disjoint subsets (**unknown**).
- **Virtual geometry draws are one indirect per batch (observed).** `0xdb700` iterates the batch list `[view+0x10]+0x48..0x50`
  (16-byte entries) and calls `0xdb390` per entry. `0xdb390` gates on the stage visibility `FUN_000f8250(..., stage)`,
  then `0xdb620`, which emits one `FUN_0086b3d0` (indirect, data offset `(param_6+param_7*8)*0x20`). So grass costs
  one draw per visible batch per call, not per blade. Batch counts are **unknown**.
- **Object passes emit one draw per object (observed).** `0x11d3d0` walks the 0x20-byte entries of list `p`
  (`[view+0x68+p*0x20]` to `[+0x70]`), changes the material state only on a material change (`0x14d7b0`), and calls
  `FUN_001222e0(obj, cmd)` once per object. Per object it also copies descriptors (`FUN_00095320`, `FUN_00123cc0`).
- **Index-cull clear (observed).** `0x11dbe0` clears the 10000-record args buffer with a 50000-dword fill
  (`0x92b50`) every time it runs, then writes the records of visible objects only.

## 3. Ranked candidates

### 3.1 Question 1: engine LOD, draw distance and culling knobs

Cvar names and ranges are observed (report `reports2/15`, the `RegisterXprCvars` strings). Their **defaults are
unknown** (893 of 1,316 are undecoded in that report). Image changes below are inferred from the names.

| Cvar | Consumer (observed or inferred) | Draws cut | Image change |
|---|---|---|---|
| `r_virtualGeometryNumShadowCascades` (launcher `GRASS_SHADOWS`, 1-3) | gate `index < [cfg+0x49c]` in `0xce850` (observed) | grass batches in the far sun cascades (unknown count) | grass shadows end at about 3 m with 1 (launcher comment). Low to medium |
| `r_virtualGeometryScaleMultiplier` (0-10) | VG detail (inferred from name) | fewer VG batches at distance | less distant grass. Medium |
| `r_fadeOutInstanceOverDistance` (0-200), `r_enablInstanceDistanceFading` | instance fade in the VG/instance path (`reports2/04`, observed gate names) | VG instances beyond the fade distance | distant detail ends sooner. Medium |
| `r_ModelFadeOutOverrideEnable`, `r_ModelFadeOutStartOverride`, `r_ModelFadeOutEndOverride`, `r_enableLodFadeOut` | model fade-out (names only, inferred) | per-object draws beyond the end distance | distant models pop out. Medium |
| `sunShadowMaxDistance` (0-1000), `sunShadowCascadeDistance0-4` | cascade frustum (inferred) | XPR visible set and object passes in the cascades | shadow end moves closer. Medium |
| `r_enableCascadeShadows` | sun shadow on or off (inferred) | all 461 sun draws | no sun shadow. High, not a candidate |
| `r_enableSpotShadows`, `r_enableHiResSpotShadows` | `RenderSpotLightShadows` (`0xcc690`) loop (observed) | 3 lists plus masked lists per spot light | spot shadows lost. High |
| `r_EnableTwoPassSkirtRendering` (`r_skirtEnable`) | skirt second pass (inferred from name) | one of two passes of the skirt lists 9-13 | small change at skirt edges (inferred). Low to medium |
| `r_projectedDecalEnable` | decal jobs (`0xc28c0`, `0xc59d0`) | decal draws | decals lost. High |
| `r_furEnable` | fur jobs, main view only (report 14) | fur draws | fur lost. High |
| `particleNumShadowCascades`, `particleEnableLOD`, `particleLowResBudgetFactor` | particle LOD and shadow cascades (names; `particleNumShadowCascades` has no located reader) | particle draws | fewer distant particles. Low to medium |
| `doIndexBufferCulling`, `doSunShadowIndexBufferCulling` | index cull job gates (section 2.3) | cull dispatches (per visible object) | none expected (already 0 in the launcher) |
| `forceUseLastIndexBufferCullingResults` | reader not found (reports3/02, section 2.5) | cull dispatches | one frame of popping on view changes (reports2/15) |
| `drawJustOccluders`, `drawJustShadowCasters`, `drawJustRainBlockers`, `drawJustParticleBlockers` | debug filters (names) | everything except one class | debug only, not a candidate |

Conclusion for question 1: the cvars that cut draws all change the image at distance or in shadows. None is
image-identical. The launcher already exposes the grass shadow cascade count (`r_virtualGeometryNumShadowCascades`).
Measure each with the per-scope draw counter of section 6 before choosing one.

### 3.2 Question 2: merging per-object draws

- **Engine option (inferred feasibility).** In `0x1a6d20`, the visible objects form runs. A run is a maximal set of
  consecutive set bits within one (material, mesh) group. Its records are contiguous (`objIdx*0x14`, observed), so
  one `sceGnmDrawIndexIndirectCountMulti` (wrapper `0x86b470`, observed, import present, no caller in the eboot)
  over `count = run length` with `data_offset = first*0x14` draws exactly the same records in the same order.
  Culled objects inside a run are not touched, because a run never spans a gap. This variant is image-identical
  if the records are valid (same condition as the single draws today).
- **Records of culled objects (unknown).** If a run is allowed to cover a gap, it draws stale records. The safe
  rule is runs over consecutive set bits only.
- **Port option (observed, opt-in).** `SOTC_DRAW_MDI` (`vk_rasterizer.cpp:2509-2662`, `sotc_draw_merge.h`) already
  joins consecutive draws with the same content key into one `VK_EXT_multi_draw` run. Its verify mode is
  `SOTC_DRAW_MERGE_STATS=2`. The earlier measurement (`00-SUMMARY.md`, table row 1) gives 291,582 of about 728,000
  ops joining runs (40%). Neither `launcher/sotc_launcher_win.py` nor `sotc.py` sets `SOTC_DRAW_MDI`, so it is off
  by default.
- **Relation of the two options.** The port option merges at the draw thread and needs no engine change. The engine
  option also cuts command-processor ops (one PM4 packet per run instead of per object), so it helps the CP
  thread, which the CLAUDE.md says is the limit. They overlap; pick the port option first (already built), and
  use the engine option if the CP cost stays high after R1.
- **Group count (unknown).** The number of (material, mesh) groups per frame is not measured. The counter is
  cheap: increment at the two group-change branches of `0x1a6d20` (the `FUN_0016b850` calls).
- **Object passes (`0x11d3d0`).** One draw per object, each with its own descriptor copy (`FUN_00095320`). Merging
  needs the same index buffer, which is probably not true across objects. **Unknown.** Not a first candidate.

### 3.3 Question 3: compute dispatches

Dispatch sources and scope names are in `reports2/04` (observed). Per-frame counts are **unknown**. Candidates:

- Fog (scope `Fog`, `0xfa009d`): 6-9 dispatches when its gate is open (observed gate, `sub_a5e80` family A
  wrappers). It is temporal (ping-pong history `+0xb4`). Decimating it to every second frame halves those 6-9 and is
  a small visual change (inferred). Gain about 3-5 ops per frame.
- Particle simulation `ParticleRenderer::UpdateSimulation` (`0x103ad0` family A, `0x1051f0` family B, from frame
  job `d0040`) and `PrepForRendering` (sort and merge passes whose count depends on particle count, unknown). Running
  the simulation every second frame changes particle motion (inferred). Gain unknown.
- XPR cull dispatches (`CullSunShadowCascade 0-4`, `CullSpotShadow 0-3`, the main cull): 9-10 per frame (observed
  names). The sun cascade cull can be skipped with its cascade (section 3.4).
- Per-object index-cull dispatches (`0x1a8180`, `0x1a85f0`): not in the default config (section 2.3). If the
  measurement used them, this is the largest compute item and the fix is the cvar, not an engine change.
- Label-wait dispatch (`0x92b50`, 14 + 9 callers): same op count; its cost is the wait, not the op. Not an op cut.

### 3.4 Question 4: shadow cascades

- Far cascades 2 and 3 on alternate frames (report 12, mode 1; report 14, H2). Cascade body `0xcdbc0` (signature
  unique, report 14). Skipping it also skips the object passes and masked lists of that cascade (observed in
  `0xce850`). On the route the sun shadow is 461 draws; the far cascades are about 40% of that (inferred), so the
  saving is about 150-250 per frame. In shadow-heavy scenes (78% of the draws, reports2/14) the saving is much larger.
  Visual risk is high: the blink of the port's stagger (15 Hz, 0.36% of pixels per frame) is already measured.
- The mode 1 design needs the texel and matrix pairing that report 12 did not find (`view_state[4]` writer
  unknown). Do not enable without that.
- `CullSunShadowCascade` (compute) can be skipped together with its cascade. Gain: part of the cull dispatches.

### 3.5 Question 5: things drawn twice

| Duplicate | Where | Image-identical fix | Label |
|---|---|---|---|
| XPR per-object in pre-pass and G-buffer (about 1,900 each) | `0xc1860` masks 0x10-0x12; `0xc1ce0`/`0xc1fc0` masks 0x13/0x14 | Remove the pre-pass draw (`SOTC_NO_PREPASS=3`, section 4 R2) or merge both (R1) | observed (brief); the removal needs the resolve check |
| Alpha pre-pass list 37 walked twice when non-empty | `0xcaed0` lines 478-480 (two `sub_c3a50`, pass 0x25) | Drop the second walk if its constants equal the first (`param_6`) | observed call pattern; constants unknown |
| Grass and particles in pre-pass and G-buffer | `0xc1860` (bit 0x02), `0xc1fc0` | Drop from the pre-pass if the grass shader does not discard (for depth-test equality) | unknown (grass discard not checked) |
| `0x1a7290` as a second dispatch | brief premise | none: it is a second consumer, same draws (section 2.2) | corrected |
| Debug view `0xc46c0` | `MainCamera` `0xcc188` | none needed on the route | observed gate |

## 4. Ranked list (ops saved per frame on the route, image risk, effort)

Start: about 7,000 ops (5,600 draws + about 1,400 dispatches). Savings are estimates on the route; labels in brackets.

| Rank | Change | Saves (ops per frame) | Image risk | Effort | Label |
|---|---|---|---|---|---|
| R1 | Enable the port's draw merge for XPR and object draws (`SOTC_DRAW_MDI=1`, verify with `SOTC_DRAW_MERGE_STATS=2`) | about 2,200 (40% of draws) | low (same draws, gathered arguments) | low: existing code, needs verify and default | observed ratio, inferred for this route |
| R2 | Pre-pass XPR drop (`SOTC_NO_PREPASS=3`, existing) | 1,900 before R1, about 1,100 after R1 | **high until** `ResolveSunShadows` reads no main depth (report 13 verdict A, provisional). Run `SOTC_PREPASS_PROBE` first | low: existing, opt-in | observed (code), inferred (saving) |
| R3 | Object lists 0-4 out of the pre-pass (depth-only objects) | about 1,300 | high: occluder depth, `CreateDepthTextureSet`, sun resolve, EQUAL test | high: needs membership measurement and a DB_DEPTH_CONTROL change | inferred |
| R4 | Second list-37 walk removed (`0xcaed0` lines 478-480) | up to the size of list 37 (unknown; at most about 210) | low, if the constants match | low: one hook at the second call | observed pattern; constants unknown |
| R5 | Far sun cascades 2-3 on alternate frames (`0xcdbc0` entry hook) | 150-250 on the route; 2,500 in shadow-heavy scenes | high (blink) | high | inferred |
| R6 | Fog and particle simulation every second frame | 100-200 dispatches | medium (temporal) | medium | inferred |
| R7 | Grass and particles out of the pre-pass (`0xc1860`) | unknown (bounded by the VG batch count) | medium: needs grass discard check | low to medium | unknown |
| R8 | Cvars: `r_virtualGeometryNumShadowCascades=1`, `sunShadowMaxDistance`, `r_fadeOutInstanceOverDistance` | unknown; measure first | low to medium | none (command line) | observed names; defaults unknown |
| R9 | Per-object index-cull dispatches | 0 on launcher defaults (inferred); up to one per visible object if enabled | none expected | none | inferred |

Sum of R1, R2 (after R1), R4, R6, R7 on the route: about 7,000 - 2,200 - 1,100 - 200 - 150 - (R7) = about 3,300 or
less. Under 3,000 needs R3 (about 1,300 more, high risk) or R5 (about 200 on the route).

## 5. What the port needs to measure first (no build done here)

1. A per-scope draw and dispatch counter: the port already hooks `Profiler::PushScope` (`gpu/sotc/probe_scopes.cpp`,
   `0x16b8c0`). Tag each op with the scope name of its object pass, masked list, VG or particle call. This gives the
   per-bucket table in section 1 and the membership split for R3. It is the only measurement that settles most
   unknowns below.
2. Count the (material, mesh) group changes per frame in `0x1a6d20` (section 3.2).
3. Count `0x11dbe0` calls and per-object dispatches per frame, with the two index-cull cvars at 0 and 1 (section 2.3).
4. Run `SOTC_PREPASS_PROBE=1` on the route and check whether the resolve input equals the main depth (R2 gate).
5. Compare the two list-37 walks (`param_6` at `0xcbc46` and `0xcbc90`) (R4).

## 6. Unknowns

- The cvar defaults (893 of 1,316 undecoded, `reports2/15`). The defaults decide which cvars are already set.
- The membership of lists 0-4 against lists 5-8 (the 1,400 versus 70 split is inferred from totals).
- What writes the 20-byte cull records when the index-cull job is off (section 2.3).
- Whether `ResolveSunShadows` reads the main depth (report 13, section 7).
- The number of visible runs and (material, mesh) groups per frame.
- The batch count of virtual geometry, and whether the grass shader discards.
- Whether the two `sub_c3a50` calls draw the same depth targets.
- The active spot light count on the route.

## 7. Files read

- `docs/re-backend/00-SUMMARY.md`, `04-depth-prepass.md`, `11-xpr-args.md`, `12-shadow-cache.md`,
  `13-resolve-sun-shadows.md`; `docs/re-stutters/reports2/14-draw-sources.md`, `reports2/04-compute-passes.md`,
  `reports2/15-cvar-inventory.md`; `docs/re-stutters/reports3/02-xpr-readback.md`.
- Decompiles: `out/re/decomp/000c/000caed0.c` (lines 190-200, 270-272, 330-340, 430-480, 510-560, 580-610),
  `000c1860.c`, `000c1ce0.c`, `000c1fc0.c`, `000c3a50.c`, `000c3c70.c`, `000c3ec0.c`, `000c46c0.c`, `000cc690.c`,
  `000cd330.c`, `000cdb00.c`, `000ce850.c`, `000c5110.c`, `000c5130.c`, `0011/0011d3d0.c`, `0011d7a0.c`,
  `0011dbe0.c`, `0019af20.c`, `0019b330.c`, `001a/001a6d20.c`, `001a7290.c`, `001a8180.c`, `000d/000db700.c`,
  `000db390.c`, `000db620.c`.
- Port: `gpu/shadps4/video_core/sotc_prepass_probe.h`, `sotc_draw_merge.h`, `amdgpu/liverpool.cpp` (1739, 1946),
  `renderer_vulkan/vk_rasterizer.cpp` (2509-2662 MDI notes), `launcher/sotc_launcher_win.py` (642-653).
- Tools: `tools/re/eboot_re.py` (dis, callers, refs, fstr, str).
