# 16 - Draw path gaps: what the November 3 RE round closed and what is still open

Static analysis by six read-only agents (eboot decompiles under `out/re/decomp`, `tools/re/eboot_re.py`, the port
source and the logs in `user/`). Nothing was built or run for this report. Labels: **observed** (decompile, disassembly,
source line or log line), **inferred** (reasoning from observed code), **unknown**. Addresses are eboot VAs.

It feeds two plans (see section 7):

- **C**: coalesce runs of the game's per-object indirect draws in the port's command processor (CP) into one
  `vkCmdDrawIndexedIndirect(Count)` per group.
- **B**: a hybrid native renderer that hooks the engine's draw emitters.

## 1. XPR per-object draws (`0x1a6d20`, alternate `0x1a7290`)

- **Call chain (observed):** `0x11d7a0` (RecordMaskedDraws) -> `0x19af20` -> `0x1a6d20`. The alternate `0x1a7290` is
  taken when the pass type is not 0/7 and cfg byte `+0x22` is set.
- **Pass masks (observed):**
  - pre-pass masks 0x10/0x11/0x12 are issued in `0xc1860`;
  - G-buffer mask 0x13 in `0xc1ce0`, mask 0x14 in `0xc1fc0`.
- **Loop (observed):** the loop walks the set bits of a 2048-bit mask in ascending object index. Groups are runs of
  set bits with the same material (`plVar5[0]` boundaries) and the same mesh group (`plVar5[1]` boundaries).
- **Between two draws of one group (observed), only two calls run:**
  - `0x85fc10` (Material_BindGraphicsShaders, dirty-gated, inferred to be a no-op after the first draw of a group);
  - the draw itself, `0x86b3d0` (DRAW_INDEX_INDIRECT at `objIdx*0x14`).
  - No per-object constant, transform or register packet is emitted.
- **On a group change (observed):**
  - `0x16b850`/`0xf8990` (material state, per-view entry `part+0x70/+0x98 + view*0x48`);
  - `0xae150` (user data from the group's constant block);
  - index base, size and type (`0x86bf50`, `0x86bfb0`, `0x86bed0`), compared against cached values first;
  - for view types 2/4 (G-buffer), `0x869bb0`, a SET_CONTEXT_REG to reg 0x10c (meaning unknown).
- **The group record (0x20 bytes) holds one index region and one vertex-side handle.** Members of a group are the same
  mesh drawn several times (observed in code, inferred for the assets). They differ only in their 20-byte args record.
- **Args records (observed):**
  - 20 bytes, the `VkDrawIndexedIndirectCommand` layout;
  - one per global object index, 10,000 per set, in a buffer created by `0x11dbe0` at ctx+0xab98 (set state `+0x28`);
  - a group's members are **sparse** in that buffer, not contiguous.
- **Writer of the records (observed): only the GPU cull dispatch `0x11dbe0` (IndexCullXPRObjects).**
  - It clears all records to zero ("Clear Dispatch Draw Buffer"), then dispatches.
  - It is queued by `0xc5130`, gated by ctx `+0xae3c`. That byte is set in `0xca150` from cvar byte `+0x1f`
    (`doIndexBufferCulling`).
  - The cascade variants `0xcdb00` and `0xceae0` are gated by `+0x20` (`doSunShadowIndexBufferCulling`).
  - **Open (blocker for C):** the port runs with both cvars at 0, yet the pre-pass shows indirect draws (the
    `SOTC_NO_PREPASS=3` counters see ~1,500 per frame). Either the records come from another path, or the gate does not
    hold as read. This must be measured in game first.
- **Shader selection (observed):**
  - The draw type passed to `0x86b3d0` comes from ctx `+0xc548`: kind 0 -> type 2, kinds 0x45/0xad -> type 6,
    kind 0xb0 -> type 4.
  - `0xf8aa0` sets the kind from the object's per-view state id: 0x11 -> 0, 0x17 -> 0x45, 0x19 -> 0xb0, 0x1f -> 0xad.
  - Which kind belongs to which pass: unknown.

## 2. The port's handling of indirect draws

- **The CP never reads the args (observed).**
  - `SubmitDrawIndirect` (liverpool.cpp ~1938) stores address, offset, stride, count and the two SGPR offsets in a
    BackOp.
  - `base_vtx_loc`/`start_inst_loc` are SGPR positions, not record fields.
  - docs 11 section 1.1 and 14 section 4.3(3) say the CP writes them from the record; the port does not.
- **Per-draw offsets reach the shader through Vulkan (observed).**
  - The recompiler writes the vertex and instance offset SGPRs from the `BaseVertex`/`BaseInstance` builtins
    (translate.cpp ~101-127; DrawParameters capability; `shaderDrawParameters` enabled).
  - So a `vkCmdDrawIndexedIndirect` with `drawCount > 1` gives every draw its own offsets.
  - The Multi/CountMulti packets already use this path today, with no shader change.
- **What blocks coalescing today (observed):**
  1. One BackOp per packet, holding one contiguous range (`offset + i*stride`); there is no list of offsets.
  2. Per-packet decisions keyed on `max_count == 1`:
     - the pre-pass mark (`SOTC_NO_PREPASS=3`, liverpool.cpp ~1971);
     - the single-record CPU fast path (vk_rasterizer.cpp ~3355);
     - the MDI join.
  3. WRITE_DATA fusion needs the whole span of the run.
  4. Sparse members: one draw needs the group's records gathered into a contiguous buffer first. That can be a GPU
     `vkCmdCopyBuffer` with one 20-byte region per member, or `count = 0` records in the gaps (the zeroed records of
     the cull pass, if the cull runs).

## 3. Textures, samplers, render targets

- **T# (observed):** the port's `AmdGpu::Image` layout (resource.h ~143-185) matches the bit extraction in the base
  writer `0x881e60`. Dword6 bit 24 selects an alternate address path. MSAA types compute samples as
  `1 << LAST_LEVEL`.
- **Where the descriptors enter the material (observed):**
  - The six material setters (`0x8619a0`, `0x861ce0`, `0x861e60`, `0x861fe0`, `0x862160`, `0x8622e0`) are one family:
    they map slots through a u16 table at `stage_map + 0x22/0x42/0x62/0x82` and copy items to
    `S + 0x90 + stage*0x1800`.
  - `0x163d10` copies `CTexture+0xc4` (T#) through `0x8829e0`, and an S# from the global table
    `0x02ee3ce0+0x330 + i*16`.
  - `0xa8c50` also copies a T#.
  - `0x19b400` and `0xde9d0` copy 16-byte pairs from per-object blocks, not from a CTexture.
- **To check:** `0x8829e0` with flag 0x10 rewrites dword3 bit 22, which is inside `TILING_INDEX [24:20]` in the port
  layout (inferred change of the tile index by 4).
- **Render targets (observed):** `0x9aef0` creates about 75 `kRTT_*` targets through `0x97a50(w, h, ..., format id,
  ..., name)`.
- **Unknown:**
  - the tile modes SotC actually uses (no histogram exists);
  - the writers of the sampler table;
  - the render target format-id table (e.g. 0x1a);
  - where the game writes the CB/DB registers.

## 4. Vertex formats, LOD fade, skinning

- **Vertex formats (observed, bytes read from `out/eboot.elf`):**
  - format code -> internal id: `DAT_00fa62b0[0..16]` = `0,1,2,3,0x14,0x13,0xc,0xd,0,0,0,0x10,0,0,0,0x12,0x12`;
  - id -> stride: `DAT_00f9ec50[0..26]` = `4,8,12,16,4,8,12,16,4,8,12,16,4,8,2,4,8,1,4,4,4,4,4,4,2,4,4`;
  - `DAT_00f9ebe0` packs a GNM-style data format in its low nibble (inferred); the num format is unknown.
  - Which formats the meshes use is unknown: the mesh tables are in the game archives. Logging the code at the
    `0x110ef0` entry `+0x4` would give the histogram.
- **LOD fade (partly unknown):**
  - lists 3/4 (`kRS_Depth_Fadeout`, `..._InvLODFade`) take the path `0x11d3d0` -> `0x123cc0` -> `0x123810`;
  - no float fade value, stencil or dither write was found;
  - candidates are the feature word `+0x468`, the object flags `+0x10`, and the cvars `r_ModelFadeOut*` /
    `r_enableLodFadeOut`.
- **Skinning (observed and inferred):**
  - `0x1a6d20` has no skin-specific code;
  - skinned instance data is written before drawing by `UpdateSkinningRenderObjects` (`0xcfdb0`) ->
    `UpdateGPUData_InstanceData` (`0x118100`);
  - `boneMatrices` is a named shader parameter set in `0xef8cd0`; where the palette lands is unknown.

## 5. Pass state and frame order

- **Pass-state global (observed):** `*(0x02ee3ce0+0x8a8)` is written once per area load, in `0x1671d0` (via
  AreaLoad_ShaderListAndAssets `0x2569d0`), not per pass.
  - `0xa69d0` binds the view's VS and PS from the fixed slots `P+0x68+0xf0` and `P+0x68+0xc00(+0x10)`.
- **Frame order (observed, `0xcefa0` / `0xcaed0`):**
  1. RenderShadows `0xcc9b0`;
  2. DepthAndGBufferPasses `0xc59d0`, with decals `0xc28c0` and lights `0xaea10`;
  3. forward and alpha passes:
     - `0xc3c70` and `0xc3ec0`, which reuse the object emitter `0x11d3d0`;
     - particles `0x109ab0`;
     - distortion `0x10aa30`;
     - forward decals `0x1818e0`;
  4. UpsampleDepthAware `0xbbbf0`;
  5. post-processing `0xd04e0` -> `0xb6590`: DOF `0xb6e20`, motion blur `0xb8c60`, distortion `0xb9210`,
     fullscreen blur `0xb9710`, PostFX `0xb4040`; the order inside is unknown;
  6. HUD markers in `0x118470`;
  7. RenderBackend_Frame `0x90ea0`.
- **Draw emitters (observed):**
  - the object emitter `0x11d3d0` serves about 20 pass jobs, the cascades included;
  - decals, lights and forward decals go through `0x128d20` -> `0x94d00` (direct draws);
  - `0x862c50` has no caller in the index, and nothing reads its record at ctx+0xc8b8.
- **Unknown:**
  - water drawing;
  - the HUD draw calls;
  - per-pass draw counts beyond the totals;
  - full coverage of the direct callers of `0x86b3d0`.

## 6. Sun shadows and object lists 0-8

- **Measured counts (observed, `user/crashes/2026-11-02_22-49-11_0x3.log`, SOTC PASS STATS over 180 frames):**
  - about 1,167 sun-shadow draws per frame, 18-25% of all draws;
  - by slice: 0 = 60, 1 = 69, 2 = 306, 3 = 732, 4 = 1.
  - The often-quoted 461 (brief), 3,041/2,049 per slice and ~6,500 (comment at vk_rasterizer.cpp ~1132) have no log
    behind them in the repo.
  - 11,330 is a whole-frame PM4 command count, not a draw count.
  - In the same log, OPCOST gives 1,734 indirect draws per frame (the XPR path, inferred) and 778 direct draws.
- **Object list `0x11d3d0` per entry (observed):**
  - `0x123cc0` -> `0x123810` writes a 0x58-byte instance record at `group+0xf0 + index*0x58`;
  - `0x95320` copies a masked subset of a 10-qword table `cmd+0x90`;
  - then a direct draw through `0x128d20`, or `0x189060` when `obj+0x28` is set.
  - The descriptor table is the same for every object of the pass, and only the mask (per material) varies. So the
    copy can be done once per (material, pass), not per object (inferred).
  - What varies per object: the instance-record index, `obj+0x38/+0x40/+0x48` (index address, count, type) and the
    record itself.
- **Cascades (observed):** `0xce850` calls `0x11d3d0` for passes 0, 1 and 3 in all five cascades, as the pre-pass
  does. A merge in this emitter helps both, but each cascade has its own visible set.

## 7. Consequences for the plans

- **C (coalescing in the CP) stays the first step.**
  - A group is one mesh and one material.
  - Its draws differ only in their args record.
  - The shader already takes per-draw offsets from `BaseVertex`/`BaseInstance`.
- **What C needs:**
  - a run detector in the CP: consecutive DRAW_INDEX_INDIRECT packets with no other packet in between;
  - a BackOp that carries the run's list of offsets;
  - a GPU gather of the sparse records;
  - the per-packet decisions (pre-pass mark, CPU fast path) applied per run.
- **First, in game (P0):**
  - who writes the args records with the cull cvars at 0;
  - the group count and length per frame;
  - whether the zeroed records hold.
- **B (native renderer) is not blocked by the backend.** The draw emitters `0x1a6d20`, `0x11d3d0` and `0x128d20` are
  plain guest functions that hooks can reach. The gaps left for B are in sections 3-5:
  - texture tile modes;
  - samplers;
  - render target formats;
  - LOD fade;
  - water and HUD.
- **Verification:** compare the replaced pass against the original in the same frame (`SOTC_VERIFY_PASS`, proposed). The
  frame hash only sees a 256x144 downscale with an 8x8 average hash, too coarse for a pass check.
