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
2. Remove the port-side key multipliers above: unexported color formats, unused step rates and SGPR offsets, and
   dynamic write masks.
3. Load-time prebuild from game data:
   - a working material-load hook (0xf9390 factory build or 0xf9530 loader; the 0xf6c90 wrap does not install);
   - per material and pass: (VS hash, PS hash, blend state, RT set of the pass) -> build the pipeline on a worker
     before the first draw;
   - per .cprt effect: its three compute programs;
   - at boot this happens in the in-window "Preparando os shaders do jogo" hold; during play, at ZoneActivate
     (0x34fb30), which fires at each zone change.
4. Content-hash permutation keys (BuildSpecKey) instead of discovery-order indexes, so caches are stable across
   runs.
5. Remaining: the DRAW-only fields (vertex formats per mesh, instance step rates, image T# fields from the material
   texture slots) and the RT-set fill code (pass-targets.md, open items).
