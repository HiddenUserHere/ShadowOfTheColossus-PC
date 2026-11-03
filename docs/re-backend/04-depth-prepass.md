# 04 - Depth pre-pass: where it is issued, what it draws, and how to turn it off

Read-only static analysis of `out/eboot.elf` (EU 1.01) with `tools/re/eboot_re.py`, the decompiles in
`out/re/decomp`, and the port source. No build, no game run, no commit.

Labels: **observed** (disassembly or pseudocode at the address given, or repo code/comment),
**inferred** (reasoning from observed facts), **unknown** (not decoded).

## 1. Short answers

- **observed** The opaque depth pre-pass is job `0xc1860` ("DepthPrePass", render-job name "RenderDepth").
  It is queued unconditionally by `BPRender_DepthAndGBufferPasses` `0xc59d0`, which `RenderView::MainCamera`
  `0xcaed0` calls at `0xcb396`. There is no cvar, config flag or branch around that call.
- **observed** The pass mask at `view+0xf8` is a constant: `mov dword ptr [r13+0xf8], 0x1f` at `0xc5abd`.
  Nothing else in the game writes it on this path.
- **observed** A second, alpha-tested depth-only pass (`kRS_DepthPrepassAlpha_Normal`, object list 37) is
  issued from `RenderView::MainCamera` through `sub_c3a50` at `0xcbc46` and `0xcbc90`. Another one (list 39)
  is at `0xcbfc5`. They are gated by list occupancy, not by a cvar.
- **observed** The G-buffer draws are object lists 5, 6 (job `0xc1ce0`) and 7, 8 (job `0xc1fc0`). They use
  different render states (`kRS_GBuffer_*`) and different XPR masks (0x13, 0x14).
- **inferred** The G-buffer depends on the pre-pass through an EQUAL depth test. The port has a comment that
  says so (`gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp:696`, "still need an equal depth
  test"). The depth state of each G-buffer draw is **unknown** (section 4).
- **Recommended patch point:** the mask immediate at `0xc5abd` (set 0 to skip all pre-pass draws) plus a state
  change on the G-buffer draws (EQUAL to LESS_EQUAL with depth write on). Mask alone is **not** image-identical
  (section 6).

## 2. Call chain (observed)

| Step | Address | What it does |
|---|---|---|
| Frame | `RenderView::MainCamera` `0xcaed0` (6077 bytes) | Calls `RenderShadows` (`0xcc9b0`) at `0xcb04c`, then `BPRender_DepthAndGBufferPasses` (`0xc59d0`) at `0xcb396`. Stores the pass-list container `render_ctx+0xd8` in `view[0xc]` (decompile `param_3[0xc] = lVar11`). |
| Driver | `BPRender_DepthAndGBufferPasses` `0xc59d0` (2936 bytes) | Calls `FUN_000c53d0` (RenderCullXPR). Calls `0x932a0(...,6)` at `0xc5aa2` (depth-target setup, outside the job). Stores `view+0xf8 = 0x1f` at `0xc5abd`. Queues job `0xc1860` through `RunFunctionAsRenderJob` (`0xc12a0`, flags `0x302`) at `0xc5b5a`. |
| Pre-pass job | `0xc1860` (`DepthPrePass` string `0xfa0d05`, render-job name `RenderDepth`) | Runs the object passes selected by `view+0xf8` (bits 1, 2, 4, 8, 0x10). |
| Between | `0xa6640` (`0xc5bb8`), `ResolveSunShadowJob` (`0xafca0`, queued at `0xc5f47`) | Not decoded (section 5). |
| G-buffer | `0xc1ce0` (`RenderGBufferBakedPerVertex`, queued at `0xc5fec`), `0xc1fc0` (`RenderGBufferBakedPerProbe`, queued at `0xc6091`) | Both queued by `0xc59d0` with flags `0x302`. |
| After the G-buffer | `0xbae70` (`CreateDepthTextureSet`, called at `0xc611a`), a job at `0xc61ac`, `RenderDecal` (`0xc28c0`, queued at `0xc624d`) | Depth consumers (section 5). |

Naming corrections to the brief (observed):
- `0xc2650` is `FurRendering::PreRenderJob`, not `RenderView::MainCamera`. `MainCamera` is `0xcaed0`.
- `0xc2750` is `FurRendering::PostRenderJob`.
- The G-buffer driver is `0xc59d0` (`BPRender_DepthAndGBufferPasses`), called only from `MainCamera` at `0xcb396`.
- `0xc59d0` is queued by `MainCamera`, and the job runs on the render thread. The job itself is not queued from
  `MainCamera` directly.

## 3. Question 1: who sets the mask, and is there a switch

**Mask (observed).** `view+0xf8` (the `uint` at `param_1+0x1f` in `0xc1860`):
- Written once on this path: `0xc5abd`, `mov dword ptr [r13+0xf8], 0x1f`. `r13` is the view (`param_3` of `0xc59d0`).
- Read in `0xc1860` at `0xc19b2`-`0xc1a08` and used as `uVar6` for the bit tests.
- The value is not loaded from a field, cvar or config. It is an immediate.

**Bit meaning inside `0xc1860` (observed):**

| Bit | Gate | Work |
|---|---|---|
| 0x01 | `(view[0x1f]&1)` and (`cfg[0x1c]==0` or list `view[0xc]+0x10 == +8`) | object pass 0 (`kRS_Depth_Occluders`), `view+0xf4 = 1` |
| 0x02 | | object pass 1 (`kRS_Depth_Normal`) with `cfg[0x4e]`/`cfg[0xf5]` gating for virtual geometry; `FUN_000db700` x2 (grass); `FUN_0010aee0` (particles, if `*(ctx[0x40]+0xdc0) != 0`); XPR masked draws 0x10 |
| 0x04 | | object pass 2 (`kRS_Depth_DiscardTest`), `view+0xf4 = 1` |
| 0x08 | | object pass 3 (`kRS_Depth_Fadeout`), XPR masked draws 0x11 |
| 0x10 | | object pass 4 (`kRS_Depth_Fadeout_InvLODFade`), XPR masked draws 0x12 |

**Inside `0xc1860` before the passes (observed):**
- `view[9][0x19a]` and `view[9][0xa8c]` select one of three constant blocks (`ctx+0x118`, `+0xd0`, `+0x100`)
  that are copied into the view constants. This selects the depth view constants, not whether draws happen.
- `FUN_00091e10(ctx, cfg+0x17c, 0xff)` runs before the passes.

**Switches (observed):**
- No cvar string related to the pre-pass exists. Searching all strings for `prepass`, `ZPrepass`, `DepthPrePass`
  gives only `DepthPrePass` (`0xfa0d05`), `kRS_DepthPrepassAlpha_Normal` (`0xfa6bab`) and the two `World ZPrepass` and
  `Character ZPrepass` strings (`0x12668c1`, `0x12668d0`). The `RegisterXprCvars` list (`0x6efe0`, report 14 section
  2.6) contains no pre-pass cvar.
- The port's own cvars do not gate this either (grep of `gpu/`, `src/`, `launcher/`, `sotc.py`: no prepass switch).
- The `0xc59d0` call to `0xc1860` is not conditional (no branch between `0xc5abd` and `0xc5b5a`).

**Other writers of view+0xf8 (inferred).** The decompile hits for `+0x1f` are many and generic, so this was not
exhaustive. On this path (`0xc59d0` to `0xc1860`), the only store is `0xc5abd`.

**Second depth source, `sub_c3a50` (observed).** `sub_c3a50(ctx, ..., view, param_4=1, param_5=pass, param_6)`
calls `0x11d3d0(..., pass, 1)` once and sets `view+0xf4 = 1`. Its callers in `MainCamera` pass `param_5`:
- `0xcbc46`: pass `0x25` (37 = `kRS_DepthPrepassAlpha_Normal`), gated by `ctx+0x5e0 != ctx+0x5e8`.
- `0xcbc90`: pass `0x25`, after `sub_c3c70`.
- `0xcbfc5`: pass `0x27` (39 = `kRS_SpecialCloakEffect`), gated by `ctx+0x620 != ctx+0x628`.
- `sub_c4110` (3 sites) and `sub_cd680` (1 site) also call it. Their pass values were not decoded (**unknown**).

Gate summary for the alpha pre-pass: list occupancy only (observed). No cvar (observed).

## 4. Question 2: the G-buffer draws and their depth state

**Object lists (observed).** `0x11d3d0` (object pass) takes a pass index `p` and walks the list
`view[0xc] + 0x68 + p*0x20` (begin at `+0x68`, end at `+0x70`). Its label is `(&PTR_kRS_Depth_Occluders)[p]`
(table `0x1597ea0`). So **the pass index is the render-state id**. Table `0x1597ea0` has 44 entries (ids 0..43):

| id | name | used by |
|---|---|---|
| 0 | kRS_Depth_Occluders | pre-pass bit 0x01 |
| 1 | kRS_Depth_Normal | pre-pass bit 0x02 |
| 2 | kRS_Depth_DiscardTest | pre-pass bit 0x04 |
| 3 | kRS_Depth_Fadeout | pre-pass bit 0x08 |
| 4 | kRS_Depth_Fadeout_InvLODFade | pre-pass bit 0x10 |
| 5 | kRS_GBuffer_Ambient_Baked | G-buffer `0xc1ce0` (second call) |
| 6 | kRS_GBuffer_Ambient_Baked_Motion | G-buffer `0xc1ce0` (first call) |
| 7 | kRS_GBuffer_Ambient_Dynamic | G-buffer `0xc1fc0` (second call) |
| 8 | kRS_GBuffer_Ambient_Dynamic_Motion | G-buffer `0xc1fc0` (first call) |
| 9-13 | kRS_SkirtDecal* | `0xc28c0`/decal path (unknown) |
| 14-23 | kRS_Standard_* (Normal, Sky, SkyMinus1..8) | forward/sky passes |
| 24-33 | kRS_Alpha_Sky* | forward alpha |
| 34 | kRS_Water | water |
| 35 | kRS_Alpha_Normal | forward alpha |
| 36 | kRS_Alpha_Normal_Depth | alpha with depth (unknown use) |
| 37 | kRS_DepthPrepassAlpha_Normal | alpha pre-pass (`sub_c3a50` at `0xcbc46`, `0xcbc90`) |
| 38-43 | SpecialScreenEffect, SpecialCloakEffect, Alpha_AfterParticles, EditorZFail, Debug, CollisionDebug | misc |

Pointer table `0x1598000`+ holds code bytes, not names, so the name table ends at id 43.

**Where the depth state lives (observed/inferred).** Per draw, `0x11d3d0` does:
- `FUN_0014d7b0(obj)` returns `*(state+0x20)` for the object's material.
- `FUN_00123cc0` and `FUN_00095320(state[0x468], ...)` bind per-state data.
- `FUN_000f8c90` writes two 64-bit words into the view record at `+0xb0` and `+0xb8` when the state's
  counters at `+0x5b0` and `+0x5f0` are non-zero (observed).
- `FUN_001222e0` emits the draw (`FUN_00128d20`, or `FUN_00189060` when `ctx[0x28]` is set).

The state object is therefore per material, and the render-state id selects which list an object is in.
On the GPU side the depth function and write enable are the Liverpool `DB_DEPTH_CONTROL` fields that the port
reads per draw (`gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:10707-10712`:
`depth_write_enable && !depth_clear_enable`, `CompareOp(depth_func)`).

**Values per kRS state: unknown.** The static path from `kRS_*` id to `DB_DEPTH_CONTROL` values was not decoded
(the state objects are built at load time from data, not from a table at `0x1597ea0`).

**What the port says (observed).** `spirv_emit_context.cpp:696`: barycentric-GS invariance is needed "since the
depth pre-pass and colour pass can pick different pipelines and still need an equal depth test". The port
authors read the G-buffer as EQUAL.

**What would have to change for the G-buffer without the pre-pass (inferred).**
- Depth compare on G-buffer draws (lists 5-8): EQUAL to LESS_EQUAL (or LESS), depth write on (otherwise the
  depth target stays empty and later consumers differ).
- Pre-pass draws removed: lists 0-4, 37, 39, the XPR masks 0x10-0x12, and the `0xdb700`/`0x10aee0` calls inside
  `0xc1860`.
- The call at `0xc5aa2` (`0x932a0`, `param_5=6`, outside the job) must stay. Its arguments include a 1.0
  constant (`0x3f800000`), so it probably clears depth (**inferred**). The job itself does not clear (observed:
  no clear call in `0xc1860`).

**Identifying a G-buffer draw in the port (observed).** `gpu/sotc/probe_scopes.cpp` already hooks
`Profiler::PushScope` (`0x16b8c0`) and records the name. The per-pass name is the `(&PTR_kRS_*)[p]` string
pushed by `0x11d3d0` at its entry (`FUN_0016b8c0(local_48, name[p], view, 1)`). So the port can tag each draw
with its list (`kRS_GBuffer_*` or `kRS_Depth_*`) through the GNM push marker stream. This is the only hook found
that separates the pre-pass draws from the G-buffer draws without guest changes.

## 5. Question 3: other consumers and the object sets

**Other consumers of the pre-pass result (inferred unless stated).**
- `0xc59d0` queues `LightRendering::ResolveSunShadowJob` (`0xafca0`) between the pre-pass and the G-buffer
  (observed). It probably reconstructs position from depth (**inferred**; not decoded).
- After the G-buffer (observed): `FUN_000bae70` (`CreateDepthTextureSet`, from `0xc59d0`) builds the linear
  depth set and its mips (`0xbab40` `XPRCreateOccluderDepthMipMaps`, `0xbb190`, `0xbb840` half-res,
  `0xbb310` fog per-tile depth). These read the main depth target, so any depth-only draw that the pre-pass
  contributes is visible to them (report 04, section "Depth pyramids").
- `RenderDecal` (`0xc28c0`, queued at `0xc624d`, uses `0x11d5b0` for its passes) runs after the G-buffer and
  projects onto depth (**inferred**; not decoded).
- The `sub_c3a50` alpha pre-pass (list 37) is issued from the later part of `MainCamera` (`0xcbc46`, `0xcbc90`),
  after the `0xc3ec0` and `0xc4210` calls (observed order). It is not part of the `RenderDepth` job.

**The "World ZPrepass" and "Character ZPrepass" strings (observed, not on the main-view path).**
- Used by `sub_5b7d30` (`0x5b7d30`, 549 bytes, a pass dispatcher over ids 1..0x20). Id 2 gets the label
  "World ZPrepass" when `DAT_02fdaa68 != 0 && DAT_02dca41c != 0`. Id 5 gets "Character ZPrepass" when
  `DAT_02fdaa74 != 0 && DAT_02fda234 != 0`. Each id runs `sub_5b7630` when `DAT_02fdaa60[id] != 0`.
- Callers: `sub_5b7f90` and `sub_5b8680` (which is called by `sub_5c7980`). Not called by `0xcaed0`.
- The flags are written by `sub_5b9f90` (46880 bytes, decompile timed out) at `0x5c1d7d`, `0x5c1e11`,
  `0x5c2720`, `0x5c29f0`, and read by `sub_5b7d30`. Their meaning (a boss or shadow-object scene) is **unknown**.
- Conclusion (**inferred**): this is a separate depth path for another scene type. It does not change the
  main-view pre-pass.

**Why 3,413 depth-only draws against 1,967 G-buffer draws (inferred).**
- Lists 0-4 (occluders, discard-test, two fade lists) are depth-only by construction. They have no G-buffer
  counterpart in the same list. Pass 0 (`kRS_Depth_Occluders`) is probably the occlusion set.
- Alpha-tested objects (list 2) and fade objects (lists 3, 4) are drawn only in the pre-pass if they are not in
  lists 5-8.
- XPR masks: the pre-pass has three (0x10, 0x11, 0x12) and the G-buffer two (0x13, 0x14).
- Grass (`0xdb700`) is called twice in `0xc1860` and twice in `0xc1fc0`. So grass is drawn four times per
  main view (observed: call sites). Particles (`0x10aee0`) are drawn only in the pre-pass.
- The exact membership of lists 0-4 against 5-8 was **not measured** (it needs the list builder
  `0x19b400`/`0x19b760` decode or a run).

## 6. Question 4: feasible ways to drop the pre-pass

| # | Patch | Where | Effect | Image risk |
|---|---|---|---|---|
| A | Mask immediate `0x1f` to `0x00` | `0xc5abd` (`mov dword ptr [r13+0xf8], 0x1f`) | `0xc1860` emits no draws. Job still runs its setup (scope, `0x92140`, `0x91e10`). The depth clear at `0xc5aa2` stays. | **High**: G-buffer EQUAL test then fails everywhere (cleared depth vs no depth written). Expect black or missing surfaces. Also drops occluder depth (list 0) for `CreateDepthTextureSet` consumers. |
| B | A plus G-buffer depth state change | Port: `ComputeDepthStencil` `vk_rasterizer.cpp:10707-10712`, selected by the `kRS_GBuffer_*` scope tag (section 4) | Lists 5-8 use LESS_EQUAL with depth write on. Nearest surface wins as before. | **Medium**: identical for opaque surfaces with distinct depths. Coplanar overlaps (decals, skirts) can change. Occluder depth (list 0) and alpha-tested depth (list 2) are missing unless kept. |
| C | Skip only lists 1-4 (keep list 0, occluders) | Port: skip draws whose scope tag is `kRS_Depth_Normal`, `kRS_Depth_DiscardTest`, `kRS_Depth_Fadeout*` | Depth-only draws that are not drawn by the G-buffer are lost. Pre-pass cost falls by most of 3,413 draws. | **Medium to high**: needs the list-membership measurement (section 5). Without it, not identical. |
| D | NOP the `RunFunctionAsRenderJob` call | `0xc5b5a` (`call 0xc12a0`, 5 bytes) | Same result as A, minus the job setup. Counter at `ctx+0xe88` was already bumped at `0xc5b3d`. | **High** (as A). Also desyncs the job counter (`ctx+0xe88`) unless the bump is also removed. |
| E | Cvar | none | None exists (section 3). | n/a |

**Recommended.** Do not use A or D alone. The sound path is B, with C only after the list-membership
measurement. Use the port's `Profiler::PushScope` hook (`gpu/sotc/probe_scopes.cpp`) to tag draws with the
`kRS_*` name (G-buffer lists 5-8 and pre-pass lists 0-4), then:
1. Log per-draw `depth_control` (`depth_func`, `depth_write_enable`, `depth_enable`) keyed by the tag, for one
   frame. This answers the state question (section 4) with data. Hook point: `vk_rasterizer.cpp:10707`.
2. Count draws per tag and check, per frame, whether each pre-pass draw has a G-buffer draw with the same mesh
   and transform. This gives the removable subset for C.
3. Only then change `0xc5abd` (mask) or the G-buffer compare, and compare screenshots against a reference
   build.

**Risks, all cases (observed or inferred).**
- Depth-equal fights on coplanar geometry (decals, skirts, `kRS_SkirtDecal*` lists 9-13): LESS_EQUAL and EQUAL
  can pick different winners.
- The barycentric-GS invariance (`spirv_emit_context.cpp:696`) exists only because two pipelines must match
  depth. Removing the pre-pass removes that need, but the G-buffer must still write depth consistently.
- Shader cache: changing the compare is state, not shader, so the cache should stay valid. Changing the draw
  set does not change shaders. (**inferred**)
- Any change to what the depth target holds changes `CreateDepthTextureSet`, fog and decals (section 5).

## 7. Unknowns

- The `kRS_*` id to `DB_DEPTH_CONTROL` mapping (the core unknown). Needs the state build path or the port log.
- Membership of lists 0-4 against 5-8 (the 3,413 vs 1,967 split).
- The list builder that fills `view[0xc]+0x68..` (`0x19b400`/`0x19b760` region, not decoded for this question).
- Which pass values `sub_c4110` and `sub_cd680` pass to `sub_c3a50`.
- The meaning of `sub_5b9f90`'s flags (`0x2fdaa60`..`0x2fda238`).
- `ResolveSunShadowJob` (`0xafca0`) and `0xa6640` (queued between pre-pass and G-buffer): what they read.
- Meaning of `view+0xf4` (stage marker written before each pass: 1, 2, 4, 10, 12). It is probably used by the draw
  emitter or the port for pipeline selection. Not decoded.
- Decompiler arguments: `0x11d3d0` is called with 5 arguments (the 5th is the `1`/`0` flag) but its body reads
  only four (observed). The flag is therefore unused on this path.

## 8. SOTC_NO_PREPASS (measurement only; removal not implemented)

**Switch.** `SOTC_NO_PREPASS` (`gpu/shadps4/video_core/sotc_noprepass.h`):
- `0` or unset: nothing counted, nothing printed.
- `1`: reserved for the pre-pass removal. Not implemented; the pre-pass stays, and the report prints
  `SOTC NO PREPASS: mode 1 is not implemented, the pre-pass is kept` once.
- `2`: the pre-pass stays. Counts per draw into the main depth target, printed with the SOTC_FPS
  report (driver.cpp) as `SOTC NO PREPASS: mode 2, gbuffer draws N (equal E, lequal L, other O, writes on W),
  depth-only draws D (per frame over F frames)`. Values are per frame: window total divided by the flips
  counted in the window.

**Main depth target.** Learned at run time: the depth target with pitch != height, pitch >= 512 and the
largest area seen so far. Square targets (shadow maps) are not counted. A draw is "G-buffer" when any colour
target mask (`regs.color_target_mask`) is non-zero, otherwise "depth-only". `equal`/`lequal`/`other` use
`depth_control.depth_func`; `writes on` uses `depth_control.depth_write_enable`. The depth test enable is not
part of the count, so a draw with the test off is still counted by its func.

**Counting points.** `Rasterizer::ComputeDepthStencil` is reached through `ComputeDynamic`, which has three
callers. The counter sits at the two draw-state entries only: the prepared path (`PrepareDraw`-side, once per
draw) and the unprepared branch of `ApplyDynamic`. The verify branch that recomputes the dynamic state
(`SOTC_PARALLEL_RECORD` verify) is not counted, so a draw may be counted once per path it takes. Expect small
double counting only if both paths run for one draw; the main session should check that G-buffer plus depth-only
roughly matches the 3,413 + 1,967 totals of the test route.

**Eboot patch site (observed, not applied).** The pre-pass mask store is `0xc5abd`:
`41 c7 85 f8 00 00 00 1f 00 00 00` (`mov dword ptr [r13+0xf8], 0x1f`). The immediate byte is at `0xc5ac4`.
Writing 0 there is a one-byte patch with a byte check; it is not in the tree because it is unsafe alone
(section 6, case A) and the removal is not implemented.

**Why the removal is not implemented: ResolveSunShadows reads the pre-pass depth (inferred, not confirmed).**
The job order in `BPRender_DepthAndGBufferPasses` (`0xc59d0`) is: pre-pass (`0xc1860`), then `0xa6640`
(observed: this is `FogParticles`, not an unknown job, it draws particles and has no depth read visible),
then `ResolveSunShadowJob` (`0xafca0` -> `0xafcc0` / twin `0xb0100`, queued at `0xc5f47`), then the G-buffer
(`0xc1ce0`, `0xc1fc0`). `ResolveSunShadows` is a screen-space pass: it takes the screen size
(`1/w`, `1/h` written into a constant chunk returned by `FUN_0008c0d0`) and a matrix block from the view (`lVar3+0xc4..0xdc`). A sun-shadow
resolve of this kind rebuilds world position from scene depth, so it most likely samples the main depth
target that the pre-pass fills. Since it runs before the G-buffer, removing the pre-pass would leave it
sampling a cleared target. The decompile was not traced to the texture binding, so this stays inferred.
Until the bound resources of `0xb0100` are decoded, the removal is **not safe**. Mode 1 is therefore not
implemented; mode 2 is the only deliverable.

**Risks for a future removal (observed or inferred).**
- **Sun shadow resolve** (above): the main risk. Verify the bound textures of `0xb0100` first.
- **Alpha-tested objects** (list 2 `kRS_Depth_DiscardTest`, and the alpha pre-pass list 37 via `sub_c3a50`
  at `0xcbc46`/`0xcbc90`): their depth exists only if they are also in a G-buffer list. Dropping lists 1-4
  loses their depth in the G-buffer test.
- **Objects only in the pre-pass**: occluders (list 0), fade lists (3, 4), particles (`0x10aee0`, called only
  inside `0xc1860`), and grass drawn in the pre-pass. Depth-only draws with no G-buffer counterpart disappear
  from `CreateDepthTextureSet` (`0xbae70`) and the depth pyramids.
- **Coplanar geometry** (decals, skirts, lists 9-13): EQUAL and LESS_EQUAL can pick different winners.
- **Depth state of lists 5-8 is unmeasured**: the port log of mode 2 is the first data on it.
- **Shader cache**: changing compare or depth write is pipeline state, not shader code (inferred), so the
  shader cache should stay valid. Changing the draw set does not change shaders.
