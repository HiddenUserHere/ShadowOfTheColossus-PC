# The game's own pipeline list: summary and implementation plan (4 Nov 2026)

## Problem
A player's first session compiled Vulkan pipelines at first use, so the game dropped to ~10 FPS after the loading
screen. A PC game ships the list of pipelines it uses; we cannot ship anything derived from the game. This folder
records where the game itself holds that list, so the port can read it on the player's machine before the first draw.

## Findings (one file per topic in this folder)

| Topic | File | Status |
|---|---|---|
| Shader collections (.cslt) and shader files (.csdr) | shader-collections.md | code read |
| Render-state writers (blend, depth, raster, RT, primitive) | render-states.md | code read |
| Materials (.cmat) and meshes (.cmsh) | materials-meshes.md | code read |
| Pass map: which program each material uses per pass | pass-map.md, slot-stages.md | code read + verified in a run |
| Render targets per pass (formats, samples) | pass-targets.md | code read |
| Port key fields and where the game decides each | key-fields.md | code read |
| Workspaces (.cwks) and area streaming | workspaces-streaming.md | code read + AREA hooks verified in a run |
| Duplicates (compute / pixel / vertex) | compute-duplicates.md, pixel-duplicates.md, vertex-duplicates.md | measured on the cache |
| Particles (engine, data, code) | particles-engine.md, particles-data.md, particles-code.md | code read + measured |

Key facts:
- A material has 14 slots; the pass index k selects slot material+0x60+0x48*k. In the slot, the mask at +0x38
  picks the stages; 17 means VS+PS, and only 17 was seen in a run. rec[0] (B+0x10) is the vertex program and
  rec[4] (B+0x30) the pixel program. The slot names a .csdr, and its OrbShdr footer gives the same 64-bit hash
  as predict_table.tsv. Verified by SOTC_PASS_PROBE.
- k values: 0 shadow, 1 depth, 2/3 gbufferbakedambient (+_motion), 4/5 gbufferdynamicambient (+_motion),
  6 decal, 7 standard, 8 alpha, 10 depthfadeout, 12 depthfadeoutinvlodfade, 13 shadowfadeout, 11 debug.
- Blend comes from the material's state object (material+0x560), or from fixed globals: k == 11, and material kinds
  7/8. Depth, stencil and cull are dynamic state in the port.
- Render targets are ~90 kRTT objects created by 0x9aef0. Their formats are constants; sizes change only with Neo
  mode. Each pass binds a fixed set; the GBuffer set matches the port's pass_layouts rows.
- Particle effects (.cprt) name their three programs per emitter. spawn/update are distinct code per effect, so
  no generic shader can replace them exactly. Particle programs must be built per effect when the effect loads.
- Duplicates: vertex modules 9,085 -> 1,363 distinct (79% of instructions redundant), pixel 9,962 -> 7,708 (10%),
  compute 4,253 -> 3,239 (6% of driver time).
- Port-side multipliers (key-fields.md section 7):
  - the PS key includes all 8 color formats, even unexported ones;
  - the VS key always includes the step rates and SGPR offsets;
  - the write masks are keyed although they are dynamic.

## Implementation plan (in order)
1. Content dedupe: one shader module per normalized SPIR-V (without the per-program OpString hash), so one
   pipeline per distinct (modules, state) combination. Big win for vertex shaders; no image change.
   **Done (4 Nov 2026), first form:** the emitted SPIR-V no longer carries the program hash (the OpString, and the
   first field of the SOTC_HEAP / SOTC_SHADER_ADDR verify records); `SOTC_SPV_PROGRAM_IDS=1` puts it back
   (`shader_recompiler/info.h`, `spirv_emit_context.cpp`; cache version 43/140). Equal code is now equal bytes, so the
   driver's pipeline cache builds each repeated pipeline once. Offline, on pack `v13272bcc`: dropping that one
   OpString gives the same counts as the full normalization (vertex 9,085 -> 1,363, pixel 9,962 -> 7,708, compute
   4,253 -> 3,240), and the stored keys of that session repeat by content (graphics 980 -> 710, largest group 107;
   compute 4,335 -> 3,322). Script: `raw_dupes.py`, `pipe_dupes.py` (session scratchpad, not kept).
   Cold A/B on the Lab test route, one round each (`user/dedup1/dd_old.log` with the ids, `dd_new.log` without;
   new salts 61/62, SOTC_PIPE_FEEDBACK=1, SOTC_PRETRANSLATE_COMPUTE=1):

   | | old (ids) | new |
   |---|---|---|
   | compute pipelines / driver cache hits / driver time | 4,270 / 0 / 679.6 s | 4,270 / 1,013 / 634.9 s (-6.6%) |
   | graphics pipelines / driver cache hits / driver time | 811 / 10 / 121.5 s | 814 / 216 / 100.9 s (-17%) |
   | recorder waits on pipelines | 11 (1,872 ms) | 12 (987 ms) |
   | route p99 / max / hitches > 100 ms | 242.6 ms / 961.6 ms / 8 | 192.0 ms / 905.3 ms / 6 |

   Driver time is summed over worker threads. The route numbers are one round each, so take them as a direction,
   not a measure. Not done yet: the port still creates one VkPipeline object per (program, perm) key (the driver
   answers the repeats from its cache: 216 hits, and 214 graphics pipelines took under 1 ms), and the pretranslate pack still stores
   every copy (272 MB of vertex SPIR-V, ~57 MB distinct).
2. Remove the port-side key multipliers above: unexported color formats, unused step rates and SGPR offsets, and
   dynamic write masks.
   **Measured offline (4 Nov 2026), not implemented: no driver gain in SotC.** On the 980 graphics keys of pack
   `v13272bcc` (key layout from the build: 344 bytes, `write_masks` at 280, `cb_shader_mask` 312, `logic_op` 316),
   compared by shader content: 708 distinct pipelines, and still 708 with `write_masks`, `cb_shader_mask`,
   `logic_op`, unused blend slots, `color_samples` or `vertex_buffer_formats` zeroed (alone or together). Those
   fields never split a pipeline in this session (8 distinct write-mask sets, `logic_op` always 0xcc). Permutations
   of one program with the same code: pixel 387 of 3,169 (12%; translation work only, the driver shares them since
   step 1), vertex 11 of 8,962, compute none. The vertex permutations differ in code, so the step-rate and SGPR
   offset fields are not what splits them. Script: `key_mult.py` (session scratchpad, not kept).
3. Load-time prebuild from game data:
   - a working material-load hook (0xf9390 factory build or 0xf9530 loader; the 0xf6c90 wrap does not install);
   - per material and pass: (VS hash, PS hash, blend state, RT set of the pass) -> build the pipeline on a worker
     before the first draw;
   - per .cprt effect: its three compute programs;
   - at boot this happens in the in-window "Preparando os shaders do jogo" hold; during play, at ZoneActivate
     (0x34fb30), which fires at each zone change.
   **Measured offline (4 Nov 2026), before implementing.** Same pack and keys as steps 1-2
   (`user/cache/CUSA08809_v13272bcc`, 980 graphics keys; scripts `key_fields.py`, `predictor.py`, `vs_verdict.py`,
   `vs_pairdiff.py`, session scratchpad, not kept):
   - What a pipeline key holds in SotC: vertex formats always zero (the shaders fetch their own vertices), 979 of 980
     keys are VS+PS, 18 render-target states (the top 2 hold 916 pipelines), 10 blend states (940 share one),
     logic op and sample counts constant.
   - Every material pass has exactly one render-target state and one other state: depth (289 pipelines),
     gbufferbakedambient (247), depthfadeout (114), gbufferdynamicambient (84), shadowfadeout (68), decal (45),
     the motion passes (1-2). So the non-shader part of a material pipeline is a constant of its pass.
   - 895 (VS program, PS program) pairs; 837 use one pipeline, 58 use two. The PS variant is almost always 0.
   - The VS variant is the open part (perm 0 in 408 pipelines, perm 1 in 271, not decided by the pass). Two causes:
     1. Images the VS reads without sampling, whose texel type the code does not show: SOTC_PRETRANSLATE builds both
        verdicts. In all 296 used variants where the verdicts differ, the used one reads the image as uint.
     2. The draw kind: an indirect draw with `vertex_sgpr_offset` replaces user SGPR 2 with the BaseVertex builtin
        and subtracts BaseVertex from the vertex id (`translate.cpp:101-139`); a direct draw reads SGPR 2 as user
        data (`ud_2`). The two SPIR-V files differ only there (3 pairs compared with spirv-dis).
   - Floor for this step, Lab test route: cold after step 1 p99 192.0 ms, max 905.3 ms, 6 frames over 100 ms
     (`user/dedup1/dd_new.log`); warm, same config and salt, p99 66.9 ms, max 109.2 ms, 1 frame over 100 ms
     (`dd_new_warm.log`).
   - Design that follows (not built yet): (a) one VS for both draw kinds: the SGPR named by a per-draw push word is
     selected from BaseVertex / BaseInstance, so `vertex_sgpr_offset` and `instance_sgpr_offset` leave the key;
     (b) unsampled VS images pretranslated as uint first; (c) at material load (hook 0xf9390, fires in run GJ2) each
     pass slot's (VS, PS) with that pass's state gives a pipeline key, built on a low-priority worker through the
     same path as a cached key (`LoadGraphicsPipeline`); the pass states are learned from the session's first
     draw of each pass and kept on disk.
   - **(a) done (4 Nov 2026), switch `SOTC_VS_DRAW_KIND=1`, default off.** Correction to cause 2 above: the field
     is `instance_sgpr_offset` (the start instance of SotC's indirect instanced draws, user SGPR 2 = 0x4c + 2), not
     the base vertex. An instanced vertex program (VGPR component count 3, hardware VS stage, no base-vertex SGPR,
     instance SGPR 0 or 0x4e) now has one key for both kinds: the SGPR is translated as user data + BaseInstance
     (`translate.cpp`, `SwVertexRuntimeInfo::instance_sgpr_add`), a direct draw has BaseInstance 0 (no fetch shader:
     `GetDrawOffsets`), and an indirect draw sees user data 0 in that SGPR (`bind_stage`, `vs_draw_kind_user_data`).
     A program with a fetch shader keeps the old translation. SOTC_PRETRANSLATE makes only the shared variant. Cache
     version term +128 with the switch.
     Cold run on the Lab test route, salt 63 (`user/dedup1/vk_new.log`) against `dd_new.log` (salt 62), one round:

     | | dd_new | vk_new |
     |---|---|---|
     | vertex programs pretranslated | 9,085 | 4,604 |
     | VS permutation used by draws (saved keys) | perm 0 in 408, perm 1 in 271 | perm 0 in all |
     | (VS, PS) program pairs with more than one pipeline | 58 of 895 | 7 of 908 |
     | saved graphics keys / distinct by content | 980 / 708 | 942 / 684 |
     | pipelines created, driver time | 814, 100.9 s | 911, 105.3 s |
     | boot preparation | 64.8 s (24,508 translations) | 63.0 s (20,027) |
     | route p99 / max / frames over 100 ms | 192.0 / 905.3 ms / 6 | 225.9 / 742.7 ms / 5 |

     This run used more programs (908 pairs against 895), so its pipeline count is higher; the route numbers are one
     cold round each and within the run-to-run spread. The boot preparation is dominated by the 4,270 compute
     pipelines of SOTC_PRETRANSLATE_COMPUTE, not by the vertex translations. What (a) buys is that the VS variant of
     a material is known before its draw (always the shared one), which (c) needs. Visual check (4 Nov 2026): a Lab
     session with the switch on (`user/last_run.log` of that session) looked right to the user; no crash, and only the
     40 pixel export failures that `dd_new.log` also has.
4. Content-hash permutation keys (BuildSpecKey) instead of discovery-order indexes, so caches are stable across
   runs.
5. Remaining: the DRAW-only fields (vertex formats per mesh, instance step rates, image T# fields from the material
   texture slots) and the RT-set fill code (pass-targets.md, open items).
