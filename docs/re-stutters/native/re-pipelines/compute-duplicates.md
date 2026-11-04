# Compute programs: duplicates, shapes and what they would save

Read-only analysis. Question: the game has 4,253 compute programs and the first-session boot hold builds one Vulkan
pipeline for each (686 s of driver time, ~62 s of loading). Do many of them use the same compute shader, differing
only in permutation or constants, so the port could compile each distinct shape once?

Short answer: partly. 1,014 of the 4,253 programs (24%) have SPIR-V identical to another program after
normalisation, but they carry only 42.8 s of the 686.7 s driver time (6.2%), because they are mostly the small
particle `emit` shaders. The large ones (particle `spawn` and `update`, 90% of the driver time) are structurally
distinct. Making constants runtime data adds only 24 more shared pipelines. Labels: **observed** (file, tool output),
**inferred** (reasoning from observed data).

## 1. Data

- Source: the translated SPIR-V in `user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack` (23,300 variants, format
  `SOTCPRE1` in `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp`, `PrePackAdd` / `PrePackFinish`).
  The `_v8dae828b` pack gives the same result (23,303 variants; identical and const class counts equal, skeleton
  count 3,214 instead of 3,213). **observed**
- Compute variants: an OpEntryPoint with execution model 5 (GLCompute). 4,253 variants, one per program (perm 0
  only). The hash set equals the 4,253 compute hashes of `user/shader_predict/predict_table.tsv` (symmetric
  difference 0). Other execution models in the pack: Fragment 9,962, Vertex 9,085. **observed**
- Pass names: `predict_table.tsv` column 3 (194 distinct names: 86 `emitter_<n>_<kind>` and 108 others).
  Names come from the `.csdr` file stem, one label per program (its most frequent file). **observed** (tool
  `tools/csdr/predict_table.py`)
- Driver time per program: `user/gcn-cold5/feedback_w2.tsv` (4,335 rows for 4,253 programs; the extra 82 rows
  are repeated builds of the same name). Sum 686,689 ms, the same total as `SOTC PIPE FEEDBACK: compute 4320 (driver
  683684.5 ms)` in `user/gcn-cold5/cold_w2.log`. **observed**

## 2. Method

Script: `tools/re/compute_dupes.py` (Python stdlib; reads the pack, parses the SPIR-V with the SPIR-V grammar from
the MSYS2 SPIRV-Headers package, `spirv.core.grammar.json`, so that literal words and id words are told apart).

Normalisation (per variant):
- drop the debug and source instructions (OpSource*, OpName, OpMemberName, OpString, OpLine, OpNoLine,
  OpModuleProcessed); the header is not hashed;
- renumber the ids in order of first appearance.

Keys (SHA-1 of the normalised stream, 16 hex digits):
- `full`: everything above. Equal keys = identical programs.
- `const`: `full`, with the literal words of OpConstant/OpSpecConstant values, OpDecorate/OpMemberDecorate
  parameters and OpExecutionMode parameters (LocalSize) zeroed. Equal keys = same shape, constants may differ.
- `skel`: every literal word zeroed. Equal keys = same opcode and id structure.

Caveat: raw SPIR-V differs between all 4,253 modules (4,253 distinct raw SHA-1 values; no byte-identical pair), so the
identity above is after normalisation only.

## 3. Classification

| Level | Classes | Redundant pipelines (programs minus classes) | Singletons |
|---|---:|---:|---:|
| identical (`full`) | 3,239 | 1,014 (23.8%) | 3,088 |
| same shape, constants may differ (`const`) | 3,215 | 1,038 (24.4%) | 3,064 |
| same opcode/id structure (`skel`) | 3,213 | 1,040 | 3,060 |

Programs in identical groups of 2 or more: 1,165 of 4,253. Largest identical groups (variants, instructions):
255 (434), 63 (436), 47 (486), 42 (418), 42 (575), 38 (489), 35 (538), 31 (577), 26 (486), 25 (418).

Redundancy by pass kind (from the identical classes, which never mix pass kinds):

| Pass | Programs | Identical classes | Redundant | Driver ms (feedback) |
|---|---:|---:|---:|---:|
| `emitter_*_spawn` | 1,470 | 1,447 | 23 | 347,140 (50.6%) |
| `emitter_*_update` | 1,437 | 1,395 | 42 | 275,247 (40.1%) |
| `emitter_*_emit` | 1,138 | 193 | 945 | 34,735 (5.1%) |
| 108 other names (`cs_main`, `csfurplacement`, ...) | 208 | 204 | 4 | ~29,600 |

Skeleton classes: spawn 1,444 of 1,470 programs, update 1,387 of 1,437, emit 176 of 1,138. So spawn and update programs
are structurally different from each other even when their names share an emitter index. Example (**observed**,
`emitter_0_spawn`): program 0x5fcd70d (2,762 insns, 12,793 words) against 0x6d1fee64 (3,224 insns) differ in OpPhi
(23 vs 39), OpBranch (35 vs 41), OpSelectionMerge (15 vs 21), OpDecorate (438 vs 525) and OpFMul (229 vs 268).
This is control flow and resource code, not a constant.

## 4. Constants that differ (the `const` level)

Only 24 classes merge at `const` beyond the identical level. Of the 17 `const` groups with more than one identical
class (run with `--samples 1000`), each differs in one to three literal words (**observed**):

| Context of the differing constant (OpConstant, Int32) | Groups | Examples (values) |
|---|---:|---|
| used by OpIAdd | 8 | 216 vs 240, 216 vs 220, 240 vs 216, 244 vs 216 |
| used by OpIMul | 3 | 224 vs 228, 228 vs 224 |
| used by OpBitwiseOr | 3 | 134217728 vs 117440512 (0x08000000 vs 0x07000000) |
| used by nothing in the body | 2 | 512 vs 520 (three words) |

No float constants differ. No spec constants exist: 0 compute modules have OpSpecConstant or SpecId
(**observed**). Workgroup sizes (LocalSize, **observed**): 64x1x1 in 2,999 programs, 16x4x1 in 973, 256x1x1 in 168,
8x8x1 in 55, others in fewer than 20 each.

Interpretation (**inferred**): the differing Int32 values are byte or dword offsets (steps of 4 or 8 bytes) into a
data or constant block, and the OR constants are packed field bits. They look like per-emitter-type layout values,
not loop bounds or coefficients. They could become push constants (the port already reserves a push range for
compute, `vk_compute_pipeline.cpp` ~line 86) so that one pipeline serves them. Gain: 24 pipelines, 1.7 s of driver
time (see section 6). Specialization constants would not share a pipeline; none are used here anyway.

## 5. What the game data says about the permutations

- The game names passes and their files, not permutations: a `.csdr` is one named pass shader (`<pass>[_<hash>]_shipping.csdr`).
  Strings: `_shipping.csdr`, `_neo.csdr`, `_neoshipping.csdr`, `.csdr` at eboot 0x12cfbf1..0x12cfc1c, used by sub_b0cb80.
  **observed**. Variant selection inside a `.csdr` is an integer key (`FUN_00b0da00`, 0xb0da00), not a permutation mask
  (**observed**, see `docs/re-stutters/native/re-pipelines/shader-collections.md` section 3c).
- Particle emitter passes: `ParticleRenderer::UpdateSimulation` (0x103ad0 / 0x1051f0, `tools/re/names.txt`) has the
  strings `update parent emitters` (0xfa5da9), `update child emitters` (0xfa5d93), and per-emitter-type counters
  (`clear per-emitter-type particle counters`, 0xfa5c3f). The shader collection `particle-shaders.cslt`
  (0xfa8d86) is loaded by sub_143f50. **observed**. So emitter types each have their own spawn, update and emit
  passes, and the `emitter_<n>` index is the emitter type. **inferred** for the mapping of index to type.
- No string in the eboot holds `permut`, `cs_%` or a permutation mask. The game data does not say which programs
  are permutations of each other. The grouping above comes only from the SPIR-V. **observed (absence in the
  `eboot_re.py str` query)**
- The `emit` shaders are reused across emitter types (193 identical shapes for 1,138 programs). The spawn and update
  shaders are generated per emitter type (no reuse). **inferred** from the classes above.

## 6. Saving estimate

Driver time model from the feedback data (**observed** fit over 4,253 programs): driver_ms = -10.65 + 0.0790 x insns,
r = 0.919. Mean 161.5 ms per program. Instructions: 9,263,944 in total.

| Option | Pipelines | Driver ms saved (actual) | Share of 686,689 ms | Instructions saved |
|---|---:|---:|---:|---:|
| identical programs share one pipeline | 4,253 -> 3,239 | 42,792 | 6.2% | 697,117 (7.5%) |
| plus constants as push data (`const`) | -> 3,215 | 44,482 | 6.5% | 720,547 (7.8%) |

The brief's cost model (4.7 ms per pipeline + 0.04 ms per instruction) gives 391 s for all and 358 s deduplicated,
which agrees with the 6.2% share. **inferred** that the driver cost scales with instructions, supported by r = 0.92.

Not saved by sharing: the GCN-to-SPIR-V translation (ASYNC TRANSLATE 58,641 ms on workers over 23,439 stages,
`cold_w2.log`). Every GCN program still has to be translated to know that it is a duplicate.

## 7. Does the session use these? (what can and cannot be said)

- The cold log's `SOTC PRETRANSLATE compute: ... first dispatches N, key hit M` lines show 382, 387, 403 and 413
  first dispatches (about 410 programs per session), with key hits 318 to 346. **observed**
- The programs used in a session are not recorded by name in these logs (only 40 `PRETRANSLATE compute miss` lines
  with hashes). So the number of distinct pass names, and the driver time, of the ~410 used programs cannot be
  computed from the repository. **unknown**
- If the ~410 were a random sample of the 4,253, their driver time would be about 66 s (410/4,253 of 686.7 s).
  **inferred**, and it is an upper bound for a session that uses only the non-duplicated programs.

## 8. Implications for the port

1. Dedup by canonical SPIR-V: canonicalise the emitted module (drop debug, renumber ids in first-use order) and key
   the shader module and the VkPipeline by that hash plus the binding-layout signature. Saves 1,014 pipelines and
   about 43 s of boot driver time. Guard with `SameBindingLayout` (`vk_pipeline_cache.h` line 54). Identity of the
   descriptor layout for identical SPIR-V is **inferred** (the layout comes from Info buffers and images, which the
   SPIR-V's own bindings reflect); not checked against the meta bytes.
2. Canonical bytes may also let NVIDIA's own on-disk cache hit for duplicates. **Untested**; the raw modules differ
   today.
3. Constants as push data: 24 more pipelines, 1.7 s. Not worth the change alone.
4. The large lever is not duplicates: spawn and update programs (2,907 programs, 622 s, 90.6% of the driver time) are
   distinct. Only building those that a session needs would help. Record the set of compute hashes that get a first
   dispatch in a session (the port already counts them) and build only that set on the first session, with the rest
   built in the background or on first use. This is outside the scope of the duplicates question and needs the
   per-session list first. Estimated upper bound on the first-session driver time: about 66 s if the session set is
   typical (see section 7). **inferred**

## 9. Reproduce

```
python -I tools/re/compute_dupes.py                       # summary (about 45 s)
python -I tools/re/compute_dupes.py --samples 1000 --max-groups 0   # constant differences per const group
python -I tools/re/compute_dupes.py --tsv <outside repo>/cd_v13.tsv  # one row per compute variant
```
Driver-time join (scratch analysis, not in the repo): `user/gcn-cold5/feedback_w2.tsv` rows with `compute`, joined to
the TSV by hash.
