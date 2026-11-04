# Pixel shader duplicates and patterns

Read-only analysis (agent R6). Data: the SPIR-V of the Fragment programs in the pre-pack of two cold-run shader
caches. Script: `tools/re/pixel_dupes.py` (stdlib Python, spirv-dis from MSYS2 CLANG64). Decomp: `out/re/decomp`.
Labels: **observed** (counted by the script or read in decomp, address given), **inferred** (reasoning, not confirmed
by data). Full per-variant rows: `scratchpad/r6/pixel_variants_<folder>.tsv` (not in the repo).

## 1. Answer in short

- The predict table's "49 pass names" are not 49 uber-shaders. Each pass name is a `.csdr` file stem (the pass
  that holds the program). Programs inside one pass are almost all structurally different. **observed**
- Of 9,962 pixel variants (8,095 programs, cache `v13272bcc`), only **7,708** distinct shapes remain after dropping
  debug records and renumbering ids. Exact removal of repeats saves 22.6% of the modules and 9.6% of the
  instructions. Constants-only merges add 1.1% more of the instructions. **observed**
- Constants are not the axis of variation. The only varying constants are **32-bit integers** in 101 groups (226
  constant lines, values 9 to 100). No float constant varies. Material coefficients are not compiled in. **observed**
- The variation that does exist is in the code: texture-sample count (0 to 72 per program), format/number conversion
  code, and a few compare opcodes. Permutations of one program (a new runtime key, `spec`) change the opcode
  sequence in 1,163 of the 1,170 programs that have different code per variant. **observed**
- A PC "uber-shader with branches" that covers the whole pixel set would be a union of hundreds of thousands of
  instructions. It does not look practical. A narrower step does look practical: move the format-conversion and
  texture-slot variation to runtime data (see section 7). **inferred**
- Decomp: the engine's pass table is 22 records keyed by eight `XPR_kSMLT_*` kinds (`0xca3e0`). The predict pass names
  probably correspond to these kinds (**inferred**). The permutations are not named in the engine (no feature-mask
  strings found). **observed**

## 2. Data and method

Pack format (`gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp`, `PrePackAdd` at 980,
`PrePackFinish` at 1011, `PrePackLoad` at 1037). **observed**

```
variant record (append order):  u64 hash, u32 perm, u32 meta_size, u32 spv_size, meta bytes, SPIR-V words
trailer:                         u64 index_offset, char magic[8] = "SOTCPRE1"  (last 16 bytes of the file)
index (at index_offset):         u32 count, then count x { u64 hash, u32 perm, u64 offset(meta), u32 meta_size, u32 spv_size }
```

- Stage is read from `OpEntryPoint` (execution model: 0 Vertex, 4 Fragment, 5 GLCompute). **observed**
- `perm` is the index of a new module for the same program. `vk_pipeline_cache.cpp` lines 1466-1478: a variant is
  added when a new `spec` (the runtime binding key) appears for a program whose binding layout matches
  (`SameBindingLayout`). So a perm is a **runtime specialization**, not a material or shader permutation. **observed**
- Counts per cache folder (script output):

| | `CUSA08809_v13272bcc` | `CUSA08809_v8dae828b` |
|---|---|---|
| index entries (all stages) | 23,300 | 23,303 |
| Vertex / Fragment / Compute variants | 9,085 / 9,962 / 4,253 | 9,085 / 9,965 / 4,253 |
| Fragment programs (distinct hash) | 8,095 | 8,095 |
| Fragment programs in predict table as pixel | 8,135 (40 missing from pack, all `gbufferbakedambient`) | same |
| distinct SPIR-V words (raw bytes) | 9,575 | 9,576 |
| distinct shapes, exact (E) | 7,708 | 7,711 |
| distinct shapes, constants masked (C) | 7,490 | 7,493 |
| distinct shapes, decoration literals masked (CD) | 7,487 | 7,490 |
| distinct opcode sequences (S) | 7,174 | 7,176 |
| total instructions | 17,168,676 | 17,166,111 |

The two folders agree to within 3 shapes. The rest of this report uses `v13272bcc`.

Normalization (`tools/re/pixel_dupes.py`, `normalize`): spirv-dis with `--raw-id --no-header`; drop `OpName`,
`OpMemberName`, `OpString`, `OpSource*`, `OpLine`, `OpNoLine`, `OpModuleProcessed`; renumber ids by first appearance.
The raw binaries differ even when the code is the same. Each module carries an `OpString "0x<hash>"` naming its program (checked on the first pack entry, a compute
module with hash `0xbf4600a0`, whose string is `"0xbf4600a0"`). The binaries therefore differ per program. Dropping
debug records removes it. The `sotc salt N` string seen in the same modules comes from `SOTC_SPV_SALT`, a diagnostic
(`gpu/shadps4/shader_recompiler/info.h` near line 112).

Loose files: 2,578 `0x<hash>_<perm>.spv` files exist. 108 of their keys are not in the pack. Only the pack was
analysed. **observed**

## 3. Pixel programs per pass name

Variants = (program, perm) modules. Shapes: E exact, C constants masked, S opcode sequence. Instr median is over
variants. Pass = the most frequent `.csdr` stem (`tools/csdr/predict_table.py`). **observed**

| Pass (stem) | Programs | Variants | E | C | S | Instr median |
|---|---:|---:|---:|---:|---:|---:|
| gbufferbakedambient | 1,077 (+40 predict-only) | 1,077 | 1,058 | 1,058 | 1,044 | 2,548 |
| gbufferbakedambient_motion | 951 | 1,831 | 1,784 | 1,777 | 1,753 | 1,903 |
| gbufferdynamicambient | 1,094 | 1,094 | 1,076 | 1,075 | 1,063 | 2,394 |
| gbufferdynamicambient_motion | 951 | 951 | 938 | 938 | 926 | 2,301 |
| depthfadeout | 933 | 933 | 602 | 553 | 485 | 1,125 |
| depthfadeoutinvlodfade | 921 | 921 | 594 | 545 | 477 | 1,125 |
| shadowfadeout | 918 | 918 | 608 | 548 | 483 | 732 |
| depth | 797 | 797 | 65 | 33 | 26 | 78 |
| standard | 57 | 291 | 197 | 197 | 191 | 4,823 |
| decal | 109 | 218 | 210 | 210 | 210 | 1,417 |
| ps_spotlight | 54 | 216 | 212 | 212 | 212 | 2,722 |
| alpha | 47 | 192 | 90 | 90 | 90 | 6,048 |
| shadow | 74 | 148 | 46 | 23 | 19 | 241 |
| ps_main | 34 | 205 | 84 | 84 | 51 | (small) |
| ps_pointlight | 18 | 72 | 72 | 72 | 72 | 2,564 |
| ps_directionallight | 14 | 28 | 28 | 28 | 28 | - |
| other 36 names | 1 to 4 each | 1 to 16 each | | | | |

Two passes share a name root: `gbufferX` and `gbufferX_motion` (the `_motion` set holds 880 programs with two or more
perms). Cross-pass exact sharing is tiny for them (5 shapes). The engine's render-target list has `kRTT_Motion`
(`0x9aef0`, line 1385) and this is probably the velocity output of the `_motion` passes. **inferred**

Largest exact groups (**observed**):

| Size | Instr | Passes | Note |
|---:|---:|---|---|
| 672 | 78 | depth 669, depthfadeout 1, ps_main 1, ps_stencil 1 | one depth-only shape shared across pass names |
| 38 | 708 | depthfadeout | pairs with the 38 below |
| 38 | 708 | depthfadeoutinvlodfade | differs from the above by one opcode (below) |
| 38 | 337 | shadowfadeout | |
| 23 + 23 | 721 | depthfadeout, depthfadeoutinvlodfade | same pairing |
| 22 | 240 | shadow (11 programs, 2 perms each) | |
| 20 | 353 | shadowfadeout | |

The `depthfadeout` vs `depthfadeoutinvlodfade` pair is the clearest case: same length, and
`diff` of the normalized text gives two lines: `OpFOrdLessThan` becomes `OpFOrdGreaterThan` (line 514 of the
normalized text). **observed**. The "InvLODFade" name matches an inverted test. **inferred**

Near-duplicates (**observed**): among shapes with equal instruction count (12,391 pairs compared, buckets capped at
400), 1,104 pairs differ by at most 2 lines and 563 differ by 3 to 8 lines. Single-link clustering with a limit of 8
changed lines gives 6,803 clusters from 7,708 shapes (6,180 singletons). Near-duplicates are real but rare.

## 4. Constants-only groups (class C)

101 C-groups hold more than one exact shape. In them, 226 constant lines differ. **observed**

- All 226 varying constants are `OpConstant` with type `Int 32`. None is a float. **observed**
- Values are small integers, mostly 9 to 100, e.g. `OpConstant %31 10 / 14 / 18 / 19 / 21 / 22` in the `depth`
  pass, and `OpConstant %339 92..100` in `depthfadeout`. **observed**
- Most such groups have two distinct values per constant (172 of 226 lines). **observed**
- Interpretation: integer offsets or slot indices (for example dword offsets into a constant buffer, or
  descriptor/attribute slots). **inferred**. Material coefficients would be floats, and there are none, so the
  recompiler already reads them from buffers. **inferred**
- The class is small: about 1.1% of instructions (C 10.66% minus E 9.61%). Moving these to push data would save
  little. **inferred from the counts above**

## 5. Permutations of one program (perm axis)

- 1,302 programs have more than one variant. 1,170 of them differ in exact text. 1,163 differ in opcode sequence.
  132 have identical variants. 7 differ only in constants. **observed**
- Opcodes whose count changes between the perms of a program (a sample of 300 programs, 326 pairs): `OpBitcast`,
  `OpBitFieldInsert`, `OpShiftRightLogical`, `OpShiftLeftLogical`, `OpUConvert`, `OpFConvert`, `OpConvertFToU`,
  `OpIsNan`, `OpSelect`, `OpBitwiseAnd/Or`, `OpImageSampleImplicitLod`, `OpSampledImage`. **observed**. These are
  bit-field and number-conversion operations, which fit a per-format unpack of a texture or buffer. **inferred**
- Instruction delta between the perms of a program: median -6, range -1,433 to +1,346. **observed**
- The variant count above the program count is 9,962 - 8,095 = 1,867. These are the modules created by the perm axis
  alone. **observed**
- The `spec` key is what changes per perm (`vk_pipeline_cache.cpp` line 1466). `info.h` near line 112 says buffer
  format, swizzle and number conversion already leave the key (FillBuffer). The perms that remain are probably image
  or texture-side. **inferred** (not checked in the code path)

## 6. Texture-block structure and feature flags

Feature signature = (texture ops bucket, kill present, branch bucket, derivative present). 40 signatures occur.
Most common: `tex9+|br5-8` (2,082 variants), `tex9+|br9+` (1,276), `tex0|br0` (725). **observed**

- Texture op count varies per program, inside each pass: `gbufferbakedambient` median 24 (0 to 52, 25 distinct
  values), `gbufferdynamicambient` 31 (0 to 61), `gbufferbakedambient_motion` 18 (0 to 52), `depthfadeout` 10 (0 to 32),
  `standard` 56 (0 to 84), `alpha` 60 (0 to 72), `depth` 0 (0 to 8). **observed**
- Among shapes with the same non-texture opcode histogram, the texture count is usually the same (median 1 distinct
  count per group). So "texture block present or absent" is not the main axis of difference. The order of the code
  is. **observed** (weak measure)
- A permutation of the texture-slot count in a uber would need a loop or a bindless index table. The port has a
  bindless path (`gpu/shadps4/video_core/renderer_vulkan/vk_bindless.cpp`, with the `SOTC_N5_BINDLESS` switch noted in
  `docs/re-stutters/native/ARCH-GCN-NATIVE.md`). **inferred** that it would fit this use

## 7. Cost model for a PC uber-shader (inferred, not measured)

- Compile savings, upper bounds (they overlap): 2,254 modules (22.6%) for exact-shape dedupe, which needs the program
  hash out of the SPIR-V (section 2); 1,867 modules (18.7%) if the perm axis disappeared completely. Sharing identical
  shapes is the cheap part. Measured in the cold
  session with ~800 used pipelines (108-140 s driver time), the used subset decides the real saving. The pack
  alone does not say which programs a session uses.
- A single branching uber across the 7,708 shapes is not viable: the union of code is far larger than any one shape
  (for `gbufferbakedambient` the shapes have 1,600 to 3,200 instructions each, and the pass has 1,077 of them).
- A narrower uber: one shader per pass family with the texture count and format conversions read from a per-draw
  push or a uniform buffer, and the code selected by uniform branches. Uniform (scalar) branches are cheap on GCN and
  RDNA class hardware. The cost is the register high-water mark of the union and the instruction cache. Neither is
  measured here. **inferred**
- Specialization constants would still mean one pipeline per value, so they do not help. **inferred**

## 8. Decomp: how the engine names and permutes these

Pass names (**observed**):

- The string table `XPR_kSMLT_*` at `0xfa10bf` to `0xfa1166` (eight names: Shadow, Depth, GBufferAmbientBaked,
  GBufferAmbientDynamic, DepthFadeOut, DepthFadeOutInvLODFade, Debug, ShadowFadeOut). The strings are used by
  `FUN_000ca3e0` (`out/re/decomp/000c/000ca3e0.c`).
- `FUN_000ca3e0` fills a table of 22 records, each 0x28 bytes (layout observed from the stores):
  `+0x00` name pointer, `+0x10` u32 kind, `+0x14` u32 slot.

  | kind | name | slots |
  |---|---|---|
  | 0 | Shadow | 0 to 7 |
  | 1 | Depth | 0 |
  | 2 | GBufferAmbientBaked | 0 |
  | 3 | GBufferAmbientDynamic | 0 |
  | 4 | DepthFadeOut | 0 |
  | 5 | DepthFadeOutInvLODFade | 0 |
  | 6 | Debug | 0 |
  | 7 | ShadowFadeOut | 0 to 7 |

- `FUN_000ca150` (`0xca150`) copies it into the context at `+0xa800` (`0x370` bytes = 22 records). `FUN_000ca120`
  (`0xca120`) writes one record: name from `PTR_s_XPR_kSMLT_Shadow_01597980[kind]`, kind at `+0xa820`, slot at
  `+0xa824`.
- `FUN_0011d7a0` (`0x11d7a0`) runs one pass from one record: it reads name, kind and slot, calls `FUN_0016b8c0` with
  the name (a scope or marker helper, **inferred**), and draws the pass list. For kinds 0 and 7 it writes `%s%s_shadowPass%d_info.csv` (the slot in the
  name) to `$/Misc/Telemetry/`. **observed**.

Relation to the predict table (**inferred**): the `.csdr` stems (`depth`, `depthfadeout`, `depthfadeoutinvlodfade`,
`shadowfadeout`, `gbufferbakedambient`...) match the kinds by meaning, not by spelling (`GBufferAmbientBaked` vs
`gbufferbakedambient`). `shadow` and `shadowfadeout` are the kind-0 and kind-7 families, and their slots 0 to 7 match
the cascade or light index. The `_motion` stems have no kind of their own. They are probably the same kinds with a
motion output.

Permutation evidence (**observed**): no feature-mask, `#define` or permutation string appears in the decomp (searched
`permutation`, `feature_mask`, `define`, `shaderdefine`). The engine does not name the perms. The perm axis is created
by the port's runtime spec, not by game data.

Material link (**observed** in `docs/re-stutters/native/re-pipelines/materials-meshes.md`, section 3.4): `.cmat` has a
pass-keyed map (keys are FNV-1a 32 hashes of pass names, to be checked). That map is the place where a material
chooses a program per pass. Counting programs per material through it would show whether programs are per-material
or per-kind. Not done here (game files were not read).

## 9. What the port could do

1. Module dedupe on the program's SPIR-V with the `OpString` hash removed (diagnostic salt off). Merges up to 2,254
   variants (22.6%) and 9.6% of instructions with no change to the code. The binding layout check
   (`SameBindingLayout`) must still pass for each merged variant. **observed counts; implementation inferred**.
2. Constants: not worth a change (about 1% of instructions, integer slots only).
3. Perms: find which spec fields cause the 1,163 structural differences. Check whether the texture-side spec
   (number type, format, swizzle, sRGB) can be a per-draw uniform. Upper bound 1,867 modules (18.7%).
4. Uber-shader: do not build it from this data. A per-pass uber would need the register and I-cache cost measured
   first.
5. Next data step: for one pass (`depthfadeout`, 933 programs, 602 shapes), read the material `.cmat` pass maps and
   count the programs each material uses. This shows if one material means one program.

## 10. Caveats

- Analysis covers the boot-hold pretranslation pack only. Programs not in the pack (40 gbufferbakedambient) and loose
  files outside it (108 keys) are not counted.
- "Same shape" is judged from spirv-dis text after renumbering. Two modules can differ only in an id order the
  renumbering does not fix. The near-duplicate numbers (section 3) are approximate.
- Compile cost is not measured per shape. The only number is the brief's "108-140 s for ~800 pipelines".
- The `kind` and `slot` reading of the record table is from stores in one decomp function. No runtime check was made.
