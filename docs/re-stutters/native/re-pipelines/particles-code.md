# Particle emitter compute programs: code patterns

Read-only analysis of the 2,907 compute programs named `emitter_<n>_spawn` (1,470) and `emitter_<n>_update` (1,437)
in the translated SPIR-V pack. Question: are these programs compiled from a fixed library of modules (node graph),
so that one generic spawn/update shader driven by per-effect data could reproduce them?

Short answer: **no** for an exact generic shader. The programs share a clear skeleton (header decode, particle
index, buffer addressing, a generic format-decode helper, unpack and clamp code), but the arithmetic that makes an
effect differs from program to program as expression trees, and recurring code covers only a minority of the
instructions. A generic shader can share the skeleton, not the math. Labels: **observed** (tool output, file, SPIR-V
text) and **inferred** (reasoning).

Scope note: the SPIR-V still carries the port's debug names (`ssbo_N`, `push_data`, `buf9_dword_off`,
`generic_buffer_format`, `sotc salt 41`). They name the translator's resources, not the game's particle attributes,
so the attribute meaning of each field is **unknown** from this data.

## 1. Data

- Source: `user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack` (format `SOTCPRE1`), parser from
  `tools/re/compute_dupes.py`. Names: `user/shader_predict/predict_table.tsv` (type 4, `emitter_<n>_spawn|update`).
  All 2,907 selected programs have OpEntryPoint GLCompute. **observed**
- Function-body instructions: 5,968,901 (spawn 3,307,604; update 2,661,297), about 2,050 per program. **observed**
- Instruction mix: all 2,907 programs contain the same core set (OpFMul, OpAccessChain, OpBitcast, OpLoad,
  OpConstant, OpFAdd, OpIAdd, OpCompositeExtract, OpStore, OpBitFieldUExtract, OpSelect, OpExtInst, OpPhi in 2,835,
  OpFNegate and OpFSub in 2,904, ...). Presence differs only for a few ops. **observed**

Tool: `tools/re/particle_patterns.py` (Python stdlib, reads the pack, uses `compute_dupes.py` for parsing; full run
about 2 minutes). Output summary is kept in the scratchpad, not in the repository.

## 2. Regions and recurrence

A region is an instruction run cut at: a load or store to a storage/uniform/push-constant/image variable, an image
op, an atomic, a barrier, a call, a terminator, or a label. A basic block is cut at labels and terminators only.
Each unit is hashed in two modes after renumbering ids by first use inside the unit:

- `shape`: constant values abstracted to their type (for example `Kf32`, `Ki32`);
- `exact`: constant values kept.

"Covered" means instructions in the top N keys, ranked by the number of programs that contain the key, divided by all
function-body instructions. "Unique" means keys that exactly one program contains.

| Unit | Mode | Distinct keys | Top 20 | Top 50 | Top 100 | Top 1000 | Keys for 50% / 90% | Unique to one program |
|---|---|---:|---:|---:|---:|---:|---|---:|
| region | shape | 31,225 | 17.3% | 27.5% | 45.5% | 62.3% | 148 / 12,454 | 22.5% (461 insn/program) |
| region | exact | 59,371 | 13.1% | 19.8% | 31.5% | 50.2% | 986 / 25,727 | 26.2% (537 insn/program) |
| block | shape | 14,626 | 10.9% | 17.9% | 21.5% | 36.1% | 2,504 / 8,968 | 44.6% (916 insn/program) |
| block | exact | 28,120 | 10.9% | 13.0% | 13.7% | 20.4% | 7,664 / 16,619 | 60.1% (1,233 insn/program) |

Split by pass (region, shape): spawn top 100 = 40.9%, unique 26.7%; update top 100 = 52.7%, unique 18.6%.
**observed**

Reading: the code is not built from a small set of recurring blocks. Half of all instructions sit in 148 region
shapes, but reaching 90% needs about 12,500 shapes. Top-100 coverage is 45%. About a quarter of the instructions
belong to regions that only one program has. Constants add variety (exact 59k vs shape 31k keys), but the shape
variety alone is already large. **inferred** that the variety is in the instruction structure, not only in constants.

Recurring pieces (regions present in all 2,907 programs, the top of the table): bit-field decodes of loaded words
(`OpBitFieldUExtract` with a shift), storage address arithmetic (shifts and adds of constants), and `OpBitcast`
groups. Which of these come from the shared format helper (section 3c) was not separated. **observed** (keys and counts)

## 3. What the recurring code does (SPIR-V text of four programs)

Four programs were dumped and disassembled (`spirv-dis`): spawn `0x4fac1ff` (1,396 insns), `0x5fcd70d` (2,762),
update `0xc094278` (1,112) and `0x66480fea` (2,346). Scratch files in
`C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/pp_dump/`.

Each has the same shape: 2 functions (`main` and one helper), one `OpSwitch`, one `OpFunctionCall`, 4 to 5
`OpAtomicIAdd`, `LocalSize 64 1 1`, input builtins `WorkgroupId` and `LocalInvocationId`, one push-constant block
(`%47`), and storage buffers at set 0, bindings 0 to 9 (binding 0 is declared four times with u8, u16, u32 and f32
views, and bindings 10 to 18 appear in other programs). **observed**

a) **Particle index and header decode.** The push-constant block is read several times and split with
`OpBitFieldUExtract` (fields at bit 0/8, 8/8, 16/8, 16/14), then shifted. The particle index is
`WorkgroupId.x * k + LocalInvocationId.x` (k is probably the workgroup size, 64). Then a word is read from binding 9,
decoded by the format helper, and compared with the index (`OpULessThan`, then a branch). **observed** (main, lines
293 to 411 of the update `0xc094278` dump). **inferred** that this word is a live-particle count.

b) **Particle buffer addressing.** Per-particle words are addressed as
`((index * stride) >> 2) + field_offset`, where `stride` is a 14-bit bit-field of a loaded header word and
`field_offset` is a byte constant. The 6,127 distinct access expressions (table in the full run output) are mostly
variants of this with offsets such as 16, 32, 200, 204, 212 bytes on binding 2. Writes go to bindings 6 to 16, mostly
7 to 11.
The stride coming from data is the one part of the layout the code takes from the header. Which field is age,
lifetime, position or colour is **unknown** (the offsets are immediates, and the game's field names are not in the
SPIR-V).

c) **Format helper (shared code).** `generic_buffer_format` is an `OpFunction` with an `OpSwitch`
that decodes a packed value by format id (bit-casts, `UnpackHalf2x16`, `UnpackUnorm4x8`). The switch has about 20
case targets in the dumped update. It is a function of 228
disassembly lines (after renumbering ids). The helper is **byte-identical** in the four dumped programs (MD5 of the
renumbered text equal). **observed** Its case blocks are among the top recurring regions, so it is likely in all
programs. **inferred** It is therefore a fixed library piece, but it is one call per program, not a per-effect
module.

d) **Clamp and unpack.** `NMin`/`NMax` (clamps) and `UnpackUnorm4x8`/`UnpackHalf2x16` appear in all 2,907 programs.
**observed** (extended-instruction count)

e) **Counters.** `OpAtomicIAdd` 14,046 times over all programs (4 to 5 per module). **observed** Likely the free-list
or alive-count update of spawn and update. **inferred**

f) **Random numbers.** None of 13 known LCG/PCG/hash constants (Numerical Recipes LCG, PCG multipliers, golden ratio,
murmur and lowbias32 mixers) occurs in any program. Only the float scale `2^-24` (0x33800000) occurs, in 10 programs.
**observed** The random source is therefore not an arithmetic constant in the code. It may come from a buffer or
from the game's data. Not checked.

g) **Curves and textures.** `OpImageSampleExplicitLod` in 422 programs, `OpImageFetch` in 34, `OpImageSampleDref` in
18. Curve lookups through textures are possible, but not checked. **inferred**

h) **Math features.** Extended instructions by number of programs: `Fract` 2,363, `Floor` 2,261, `Fma` 2,110,
`Trunc` 2,089, `InverseSqrt` 2,880, `Sin` 1,869, `Cos` 1,852, `Sqrt` 1,044, `FAbs` 672, `Log2` and `Exp2` 255 each.
Float constants: `1023` (2,907), `3` (2,907), `6.28319` = 2π (2,907), `1000` (2,830), `0.5` (2,864), `-0.5` (2,862),
`-9.8` (933, gravity-like). **observed**

## 4. What differs between programs

- **Module sets (proxy).** A proxy set per program is (storage bindings used, GLSL ops used, image ops used, RNG
  tags). It has 605 distinct values over 2,907 programs. The five most common cover 721 programs (25%), and they
  differ only by trig/sqrt/unpack/ops present. **observed** This is consistent with features that are on or off per
  effect, but **inferred** only: a feature is not a separable code module in the SPIR-V.
- **Constants only.** Shape keys 31,225 vs exact 59,371 (ratio 1.9). The constants that differ are offsets, masks and
  a few float values (the earlier duplicate study found the same kind of difference). **observed**
- **Ordering and structure.** Two programs with the same skeleton differ in the number and order of loads, stores and
  arithmetic (the 22.5% unique remainder). The spawn and update programs of one emitter index are not related by
  shared code beyond the skeleton. **observed** (earlier study, `compute-duplicates.md` section 3)

## 5. Conclusion

**Feasibility of a generic spawn/update shader that reproduces these programs exactly: not feasible.**

- The skeleton (header decode, index, address arithmetic from a data stride, atomic counters, the format helper,
  unpack and clamp) is common, so one shader could hold it. That alone does not remove the per-effect math.
- The per-effect math is 22.5% to 26% unique by region, plus a long tail (12,454 region shapes for 90%). Replacing it
  with per-effect data would need an interpreter for arbitrary arithmetic, which is a second GCN translator.
- A flag-driven uber shader would need the module flags to be known (they are not separable here) and the immediates
  to become data (offsets, constants). Even then, the operation order is fixed per program.

Obstacles for any generic shader:

1. **Float operation order.** The SPIR-V keeps each program's order of operations. Changing order or contracting FMA
   changes rounding. Bit-exact output needs the same expression trees; visually identical output needs only the same
   formulas. **inferred**
2. **Immediates.** Field byte offsets, the 2π, 1000, 1023 and -9.8 constants, and bit-field masks are embedded in the
   code. They would have to move to a per-effect buffer. Offsets are the easy part. **observed**
3. **Format aliasing.** Binding 0 is declared with four storage views (u8/u16/u32/f32). A generic shader must keep the
   same views or reproduce the format decode. **observed**
4. **Random source not located.** The code has no RNG constants. Spawn randomness has to be found in the buffers or
   the game's data before a spawn shader can match the game. **unknown**
5. **Attribute meaning unknown.** Without the field names, the data layout (age, lifetime, colour, size) cannot be
   set from the code. **unknown**

Consequence for the port: the duplicate and uber-shader routes do not remove the 2,907 spawn/update pipelines. The
lever stays the one in `compute-duplicates.md` section 8: build only the compute programs a session dispatches (the
port already counts first dispatches), and build the rest in the background or on first use.

## 6. Reproduce

```
python -I tools/re/particle_patterns.py                               # full summary, about 2 minutes
python -I tools/re/particle_patterns.py --dump 0000000004fac1ff --dump-dir <outside repo>   # SPIR-V for spirv-dis
C:/msys64/clang64/bin/spirv-dis.exe <file>.spv -o <file>.dis
```
