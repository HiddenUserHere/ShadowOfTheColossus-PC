# Vertex shader duplicates and patterns

Agent R7 of the pipeline-list RE. Scope: the vertex programs in the SOTC_PRETRANSLATE packs, what separates their
permutations, and what the engine does with vertex shader variants. Read-only: no source was edited.
Script: `tools/re/vertex_dupes.py` (stdlib Python, calls `spirv-dis` from SPIRV-Tools v2026.4, clang64).

## 1. Data and method

- Packs: `user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack` (23,300 records) and
  `user/cache/CUSA08809_v8dae828b/sotc_pretranslate.pack` (23,303 records). Format: `PrePackAdd`, `PrePackFinish`,
  `PrePackLoad` in `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:952-1073`
  ("SOTCPRE1", records of u64 hash, u32 perm, u32 meta bytes, u32 spv bytes, meta, spv; index and footer).
- The loose `0x<hash>_<perm>.spv` files are copies that `LoadLazyProgram` writes when a program is first used
  (`vk_pipeline_serialization.cpp:1107-1112`). The pack holds every variant, so the script reads the pack.
- Vertex modules: picked by the OpEntryPoint execution model (0 = Vertex). Both packs have 9,085 vertex
  (hash, perm) records. The predict table's footer type 1 agrees exactly (0 mismatches in both directions).
- Program hashes: 4,363 of the 4,364 vertex hashes in `predict_table.tsv` have a record in the packs.
- Normalization: header comments and debug instructions dropped (OpName, OpMemberName, OpString, OpSource*,
  OpLine, OpModuleProcessed); %ids renumbered by first appearance. Keys: `exact` (normalized text), `shape`
  (same, every numeric literal replaced by `#`), `fetch` (Input and buffer variables, their pointer types and
  decorations), `transform` (the rest). The fetch/transform split is by declaration only: the decode arithmetic
  that consumes a buffer load stays in the transform text, so the fetch shares below are lower bounds.
- Cross-pack check: for every program hash, the multiset of normalized modules is equal in both packs
  (4,363 of 4,363). The raw SPIR-V bytes are never equal (0 of 9,085), and the perm numbers are not stable: perm 0
  in one run can be perm 1 in the other. Compare variants by content, not by (hash, perm). The cause of the raw
  byte difference was not checked (ids or debug data; the normalized text is equal).

## 2. Duplicates

Counts for pack `v13272bcc` (9,085 modules, 14.26 M instructions with debug dropped):

| category | modules | share | instructions | share |
|---|---:|---:|---:|---:|
| identical (exact key shared with another module) | 8,529 | 93.9% | 12.88 M | 90.3% |
| constants (same shape, other literals) | 58 | 0.6% | 0.08 M | 0.6% |
| fetch-only (only fetch declarations differ) | 0 | 0% | 0 | 0% |
| transform-only (fetch equal, transform differs) | 486 | 5.3% | 1.27 M | 8.9% |
| both | 12 | 0.1% | 0.02 M | 0.2% |

- Distinct exact modules: 1,363 (15% of 9,085). Distinct shapes: 1,267. Distinct fetch keys: 157.
- Redundant copies (all but one per exact key): 7,722 modules, 11.2 M instructions = 78.7% of all vertex
  instructions. Estimated pack payload: 272 MB of vertex SPIR-V, of which about 57 MB would be the distinct
  modules (estimate: the size of the first record of each key).
- Where the duplicates sit: only 11 exact keys have all their members under one program hash. 796 exact keys
  span several program hashes. Perm 0 and perm 1 of the same hash are equal in only 11 of 4,722 cases with perm
  greater than 0. The duplication is across programs, not between permutations of one program.

Per pass (pack v13272bcc; "removable" = instructions in redundant identical copies, as a share of the pass):

| pass | modules | distinct exact | identical | transform-only | removable |
|---|---:|---:|---:|---:|---:|
| gbufferbakedambient | 1,834 | 424 | 1,645 | 164 | 68.1% |
| depthfadeout | 1,485 | 191 | 1,438 | 39 | 84.9% |
| depth | 1,440 | 130 | 1,384 | 56 | 81.5% |
| gbufferdynamicambient_motion | 1,384 | 134 | 1,352 | 24 | 89.9% |
| gbufferbakedambient_motion | 1,380 | 176 | 1,326 | 38 | 87.2% |
| gbufferdynamicambient | 1,228 | 286 | 1,118 | 100 | 67.6% |
| shadow | 125 | 34 | 112 | 13 | 75.3% |
| decal | 76 | 13 | 72 | 4 | 79.1% |
| alpha | 64 | 38 | 44 | 20 | 33.5% |
| standard | 19 | 15 | 7 | 12 | 15.0% |

The six large gbuffer/depth passes hold 8,751 of the 9,085 modules. Passes named `vs_*`, `*_vs`, `vsmain*`
have 1 to 14 modules each.

Largest exact groups (instructions per module in brackets): 325 modules [732] all in `depth`; 325 [731] the
other perm; 234 [730] `depth` (233) and `alpha` (1); 113 [791] `depthfadeout`; 87 [1194] `gbufferdynamicambient`
and `depthfadeout`; 87 [1478] `gbufferdynamicambient_motion`; 65 [1296]; 65 [1577]; 64 [1737]
`gbufferbakedambient_motion`. Each of these exact keys has a single shape, so these are verbatim copies.

## 3. Fetch versus transform

- Fetch declarations are 7.1% of vertex instructions (1.01 M of 14.26 M). A module declares on average 2.99 Input
  variables and 23.6 buffer variables (descriptors are declared whether or not used), and performs on average
  87 buffer access chains or image reads of buffer variables.
- No module pair differs only in fetch declarations (fetch-only = 0). Permutations differ in the transform text.
- Sampled pairs (137 fetch groups with more than one exact key, first two compared, script section 4b, opcode
  sequence with ids dropped): the differing runs hold float math (OpFMul 1,178, OpFAdd 742, OpFSub 198,
  OpFNegate 114, OpCompositeExtract 360), buffer reads (OpAccessChain 1,000, OpLoad 846), and format-decode
  style ops (OpBitcast 386, OpBitFieldUExtract 260, OpConvertUToF 200, OpBitwiseAnd 124, shifts 214). Only 1 of
  the 137 pairs has the same opcode multiset. Reading: the transform differences are mostly the transform itself,
  but part of them (decode-like ops and buffer reads) is the vertex fetch hidden inside the transform text.
  This is an inference from opcode families, not a proof.
- Example pair (`gbufferdynamicambient_motion`, same fetch key, 2,347 and 2,348 instructions): the first
  difference is one buffer read. One module reads a variable directly (`OpLoad`), the other reads a buffer
  element through `OpAccessChain` with a constant index, then `OpLoad`. Everything after that is shifted by
  renumbering, so a positional diff of the text looks much larger than it is.

What the port already does toward a runtime fetch (grep of `gpu/shadps4`, `gpu/sotc`):
- `SOTC_GENERIC_BUFFERS=1` (`shader_recompiler/info.h`): unswizzled buffers read their stride from the V# at run
  time; the stride leaves the key (`specialization.h` `FillBuffer`, `spec.stride = 0`).
- `SOTC_GENERIC_BUFFERS=2` (`info.h`, `GenericBufferFormat`): formatted loads of read-only buffers decode the
  V# format at run time (`lower_buffer_format_to_raw.cpp`); the format leaves the key.
  This is the case that matches the decode-like ops above.
- `SOTC_VS_BINDING_BASE=1` (`backend/bindings.h`, default off): vertex bindings start at a fixed base, so one
  vertex key works with any pixel stage.
- Still in the key: the fetch shader parse and per-attribute specialization (`specialization.h` `BuildSpecKey`,
  `VsAttribSpecialization`, `FillVsAttrib`). This is why the same SPIR-V can come under several perms (perm = the
  index of a new `StageSpecialization`, `vk_pipeline_cache.cpp:1475`).
- The GCN fetch shader is parsed at translation time (`frontend/fetch_shader.cpp`, `translate.cpp:1180`); no
  run-time table is looked up.

## 4. Constants-only groups

- 80 shape groups have more than one exact key. Across them only 94 literal slots differ, all in
  `OpCompositeExtract` (the component index), none in fetch lines. So the masked "same shape" groups are not
  constant-value variants; they are swizzle variants.
- OpConstant lines also differ in the transform-only pairs (the sampled-run summary keys them by value, so they
  are not counted apart). The sampled pair shows one of them as the descriptor index of a buffer read (constant
  index into a buffer variable array). Inference: those are slot or offset indices, not values of wind or skinning
  parameters. They could move to push data only if the shader indexes a descriptor array with a runtime value,
  which the SPIR-V does not show (the index is a constant).
- No group was found where the difference is one scalar constant (wind amplitude, morph weight and similar).

## 5. What the engine does with vertex shader variants (decomp and strings)

- Vertex shader binding: `FUN_0085f410` (0x85f410, 2,042 bytes, material bind) calls `GnmShader_SetVs` (0x86c170,
  the `sceGnmSetVsShader` import, NID `gAhCn6UiU4Y`) with the modifier at `param_1+0xc168`. `FUN_00861520`
  (0x861520) stores that modifier. Its two callers traced (`FUN_000937c0` at 0x937c0, `FUN_00093690` at 0x93690)
  both pass 0. Other writers of `+0xc168`: `sub_861730` (0x861730, from a parameter), `sub_8615f0` (0x8615f0, from a
  parameter), `FUN_00860ea0` and `FUN_00860c50` (store 0). Their callers were not traced, so the claim is limited
  to the paths checked: no non-zero modifier was seen.
- The modifier is a mask, not a code selector: `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2100-2125`
  ORs it into bits of `SPI_SHADER_PGM_RSRC1_VS`, allowing only the mask `0x030003c0`. So the engine has no
  code-level vertex permutation through this call (on the paths checked).
- Vertex format of a mesh: string `VertexFormatParameters` (0x1315629) and the reflection setup `sub_e55720`
  (0xe55720) with fields `vertexCount` and `vertexFormat`, and `buffers`. This describes mesh data, not a shader
  permutation. Inference: the per-mesh vertex format is in the mesh resource (`.cmsh`), and the variant the port
  sees (perm) comes from the V# and attribute formats at draw time.
- `Models Skinning` (0xf9ee01) is a GPU memory statistics category in `sub_8c370`, not a shader flag. Other
  skinning strings: `UpdateSkinningRenderObjects` (0xfa1517), `filterParticle_EmitFromSkinnedMesh`
  (0xf9aec4). Vertex animation: `Vertex Animation` (0xf9cfe2, used by `sub_55160`).
- Not found: a vertex permutation mask, a skinning flag passed to the shader, or a morph/wind constant table
  in the vertex stage. The pattern in the data (gbuffer/depth passes, two perms per program) suggests that
  the specialization key and the pass decide the variant, not an engine permutation flag.

## 6. Use at run time

- Deduplicate by exact SPIR-V key. Pack payload would drop from about 272 MB to about 57 MB of vertex
  SPIR-V (estimate), and 1,363 distinct modules replace 9,085 in pretranslation. Keep the pack index as (hash, perm) ->
  key. Driver pipeline work is per pipeline, so the saving in compile time is only for identical modules that
  share a pipeline layout (not measured).
- The fetch layout does not create duplicates on its own (fetch-only = 0). Merging by fetch data alone would not
  reduce the count; the transform text would still differ.
- Perm numbers are not stable between runs. A pack lookup must use content, or the pack must store perms in the
  order of the first use of the same run.
- Limits: one pass of the 4,363 programs; the two cold runs are from today; the fetch share is a lower bound;
  the instruction count is of the normalized text, not of the ISA.

## Evidence and reproduction

- `python tools/re/vertex_dupes.py --packs user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack --packs
  user/cache/CUSA08809_v8dae828b/sotc_pretranslate.pack --table user/shader_predict/predict_table.tsv --tsv <out>`
  (about 5 minutes). Output of the run used here: `scratchpad/vertex_run.txt`; per-module TSV `scratchpad/vertex_modules.tsv`.
