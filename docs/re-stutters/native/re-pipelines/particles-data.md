# Particle effect files and the runtime data the particle programs read

Read-only static analysis (agent P3). Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c` (first line of
each file lists its callers), the eboot image through `tools/re/eboot_re.py` (`Image` class, string scan), and the
earlier notes `loader.md`, `materials-meshes.md`, `shader-collections.md`, `compute-duplicates.md`. Addresses are
eboot VAs. No game file (`.cprt`, `.csdr`, `.cmat`) was read, so every field offset below comes from parser code,
not from a sample file. Labels: **observed** (decomp, string or table, address given), **inferred**, **unknown**.

## 1. Answer in short

- The particle effect resource is **`ParticleSystem`**, file extension **`.cprt`** (tag `0x63707274`, factory
  `0xfde80`). Its default effect is `$/coredata/enginesupport/particles/_ps4/default.cprt`. **observed**
- The parser (`0xfd250`, 1,787 bytes, called by the build `0xfdf30`) keeps the **file buffer** alive and builds one
  0x188-byte runtime element per file record. The file header holds the record count at +0x04 and a table of
  relative offsets at +0x74. **observed**
- Each file record (I call it **R**) holds: a 4-byte blob size at +0x04 and a relative blob at +0x14 that is copied
  verbatim to the heap; three resource references at +0x28, +0x2c, +0x30; and four more relative references at
  +0x34, +0x38, +0x3c, +0x40. **observed (parser and emitter builder)**. The meaning of the blobs and of the
  references is **unknown** (the dump was not read).
- The GPU data the emitter programs can read is **in buffers and in one per-emitter blob**, not in push constants:
  - a dispatch block of 0x20 bytes (`0x10bfd0`) or 0x50 bytes (`0x10c140`), each holding 16-byte buffer descriptors
    (V#-shaped, **inferred**) and a few dwords (pass mode, thread count, indirect values);
  - per emitter, a heap copy of **R+0x04 bytes** from the file (the blob), wrapped in a V# (format 0x1c), and
    per-instance state buffers (stride from the record, 0x70-byte records, 9-dword records) allocated in `0xffe60`.
    **observed (allocation and copy), inferred (role)**
- The programs appear to read parameters from these buffers (values) and to carry layout offsets as immediates
  (**inferred**, section 6). Evidence is the SPIR-V difference data of `compute-duplicates.md`, not a direct check.
- The particle CPU side reads particle state back: `0x13f0` (`FUN_001013f0`) walks the particle state of each
  emitter on the CPU (positions at +0x0c, +0x1c, +0x2c; a distance is written at +0xd4). **observed**. This is a
  GPU-to-CPU dependency that the port must keep in mind for any native particle path.

## 2. Resource type registration (all types)

### 2a. The helpers

| Helper | Address | Args | Role | Evidence |
|---|---|---|---|---|
| `FUN_00144640` | 0x144640 | factory, name, build fn, flags | builds a type descriptor (name, build fn, alignment, flags) | `0014/00144640.c`; callers listed in 2b |
| `FUN_00144750` | 0x144750 | factory, name, build fn, extra fn, flags | same as above with a second function (used only by ParticleSystem; the extra fn is the unload/destroy hook, decomp `000f/000fdfd0.c`) | `0014/00144750.c`, called from `0xfde80` |
| `FUN_001444e0` | 0x1444e0 | factory, FourCC (u32), descriptor | inserts the descriptor under the FourCC; this is the "registration helper" every type calls | `0014/001444e0.c`; 34 call sites |
| `FUN_0014c870` | 0x14c870 | resource name | key = last four characters of the name (from `loader.md` section 4) | `loader.md` |

Registration functions are run from `FUN_002569d0` (`AreaLoad_ShaderListAndAssets`), from `FUN_00533100` (two
CP3 types) and from `FUN_00098a90` (Texture, through `0x112730`). **observed**

The FourCC constant is the registration key. Its big-endian text is the file extension (for example `0x636d6174`
reads `cmat`). The loader table in `loader.md` section 4 uses the on-disk byte order. Both are given below.

### 2b. Table of registered types

Columns: constant (the value passed to `0x1444e0`); extension (big-endian text of the constant); name (string
passed to `0x144640`); build function; flags; registration function (the caller of `0x1444e0`).

| Constant | Ext | Name | Build fn | Flags | Registration fn | Particle |
|---|---|---|---|---|---|---|
| 0x63766773 | cvgs | VirtualGeomtrySetup (sic) | 0x0d89c0 | 0 | 0x0d8930 | |
| 0x63766d67 | cvmg | VirtualGeometryModelGroup | 0x0d8df0 | 0 | 0x0d8d60 | |
| 0x636d6174 | cmat | Material | 0x0f9390 | 0 | 0x0f9270 | |
| **0x63707274** | **cprt** | **ParticleSystem** (via 0x144750) | **0x0fdf30** (extra fn 0x0fdfd0) | 0 | 0x0fde80 | **yes** |
| **0x63707478** | **cptx** | **ParticleTexture** | **0x10e9b0** | 0 | 0x10e910 | **yes** |
| 0x63666f6e | cfon | Font (DAT_00fac213) | 0x175a60 | 4 | 0x1759c0 | |
| 0x63747863 | ctxc | TextureStreamingChunk | 0x1129a0 | 0 | 0x112900 and 0x112730 | |
| 0x63747872 | ctxr | Texture | 0x98aa0 (passed in) | 0 | 0x112730 | |
| 0x636d646c | cmdl | Model | 0x110630 | 0 | 0x110540 | |
| 0x636d7368 | cmsh | Mesh (DAT_00fa7140) | 0x1264d0 | 0 | 0x127860 | |
| 0x63776b73 | cwks | Workspace | none | 0 | 0x14ef10 | |
| 0x63736d6c | csml | StaticModelList | 0x270b20 | 0 | 0x270a90 | |
| 0x63736c69 | csli | StaticLighting | 0x139760 | 0 | 0x1396d0 | |
| 0x63706964 | cpid | PerInstanceDataAsset | 0x138f10 | 0 | 0x138e80 | |
| 0x63636872 | cchr | CP3CharacterEvents | 0x6a5400 | 0 | 0x6a5370 | |
| 0x2a2a2a2a | `****` | CP3Resource (placeholder tag, see note) | 0x6abd40 | 4 | 0x6abcc0 | |
| 0x63786d6c | cxml | XML (DAT_012cc9d0) | 0xabb3c0 | 0 | 0xab46d0 | |
| 0x63677072 | cgpr | Scripts | 0xa766e0 | 0 | 0xa76650 | |
| 0x2e626e6b | .bnk | SoundBank | 0xa825f0 | 0 | 0xa82950 | |
| 0x2e626e6d | .bnm | SoundMeta | 0xa82670 | 0 | 0xa82950 | |
| 0x2e737864 | .sxd | SndX (DAT_012c4e6e) | 0xa5c560 | 0 | 0xa5c4a0 and 0xa60d60 (same code twice) | |
| 0x73786431 | sxd1 | SndX1 | 0xa5c560 | 0 | 0xa5c4a0 and 0xa60d60 | |
| 0x63747874 | ctxt | Text (DAT_012c210f) | 0xa34ea0 | 0 | 0xa34e20 and thunk 0xa2f850 | |
| 0x63637478 | cctx | CPUTexture | 0xa2ac00 | 0 | 0xa2ab50 | |
| 0x63736b6c | cskl | Skeleton | 0xa2a940 | 0 | 0xa2aab0 | |
| 0x63616e69 | cani | Animation | 0xa26cf0 | 0 | 0xa26d90 | |
| 0x63617078 | capx | PhysicsAPEX | 0xa3f600 | 0 | 0xa3f570 | |
| 0x63726d70 | crmp | ResourceRemapper | 0xa5bc60 | 0 | 0xa5bbd0 | |
| 0x63616c74 | calt | AssetList | 0xa5b620 | 0 | 0xa5b5a0 | |
| 0x636d6170 | cmap | AnimationMapping | 0x419c00 | 0 | 0x419b80 | |
| 0x63636c6d | cclm | GameCollisionMesh | 0x3ca450 | 0x20 | 0x3ca3b0 | |
| 0x63736c74 | cslt | ShaderList | 0xb0e440 | 4 | 0xb0e3a0 | |
| 0x63736472 | csdr | Shader | 0xb0d4a0 | 0 | 0xb0d3e0 | |

Types registered without a FourCC (through `0x144640` only, so not in the table above):
- `FileResource`, build `0x149530`, flags 0, registration `0x1493c0` (line 66). **observed**
- `NullFactory`, build `&LAB_00144620`, flags 2, registration `0x144360` (line 66). **observed**

Notes:
- `0x2a2a2a2a` is a placeholder tag ("****"). The code path replaces a four-character placeholder in a path with a
  code (`loader.md`, `materials-meshes.md` section 2). The constant is not a real FourCC. **observed**
- `0xa5c4a0` and `0xa60d60` contain the same registration code (SndX, SndX1). One may be a duplicate copy in the
  binary. **observed**
- Only 0x144750 (ParticleSystem) carries an extra function. Other types have no second hook. **observed**
- The particle types are the two marked above. `Font` and `CP3*` are not particle types. `Material` and
  `ShaderList`/`Shader` are the types already covered by `materials-meshes.md` and `shader-collections.md`.

## 3. ParticleSystem (`.cprt`): object and parser

### 3a. Factory and object

- Factory descriptor: name `"ParticleSystem"` (string 0xfa548f), build `0xfdf30`, extra fn `0xfdfd0`, 0x144750.
  Strings in the same area: `ParticleSystem`, `ParticleDoubleFrame` (0xfa56d0). **observed**
- Build `0xfdf30` (144 bytes): allocates a **0x70-byte object** (`0x17070`), reads the file buffer through
  `0x8df50` (file data, 8-byte alignment), and calls the parser `0xfd250(obj, &buffer)`. **observed**
- Extra fn `0xfdfd0` (529 bytes): when a system is released, it walks the list at `DAT_02edf048` (the particle
  managers) and removes instances (`0x10b9a0`, `0x14c850`) under a mutex (`+0x10558`), then calls the object's
  virtual destructor. **observed**
- Object (0x70 bytes), from `0xfd250` (**observed** names of fields used by the parser and its callers):

```
ParticleSystem object (0x70)
  +0x00  vtable 0x1597cc8                        (set by 0xfd250)
  +0x60  file buffer pointer (u32 at +0x04 = record count N)
  +0x68  element array: u64 count N, then N elements of 0x188 bytes
         (the array begins at +0x68 + 8; the end pointer is kept at param_1[0xd])
```

  The element array is created by the parser with `0x17090(N*0x188 + 8)`. **observed**

### 3b. File header (offsets used by the parser and the runtime)

| Offset | Type | Use | Evidence |
|---|---|---|---|
| +0x04 | u32 | N, number of records (elements) | `0xfd250` loop bound |
| +0x58 | u32 | type index into the particle manager's float table (`param_4+0x104ec`, stride 0x18); a float `<= 0` makes the system skip spawning | `0x103ad0` line 129 (`param_4+0x104ec+...`), `0x1013f0` |
| +0x5c | float | a per-system value (used in a distance computation) | `0x1013f0` (`*(obj+0x60)+0x5c`) |
| +0x60 | float | a per-system value; `<= 0` (or NaN) disables a check | `0x1013f0` line 26, 55-56 |
| +0x74 | u32 table | N relative offsets; entry i is at file+0x74+4i, the record is entry + rel | `0xfd250` loop (`lVar6` from 0x74, step 4) |

The header bytes 0x00-0x03, 0x08-0x57 and 0x59-0x5b, 0x61-0x73 are not read by the code seen here. **unknown**

### 3c. File record R (one per emitter-level entry)

Each record is at `entry + rel`. The parser (`0xfd250`, lines for each `lVar12 + off`) and the emitter builder
(`0xfef40`, 785 bytes, which takes the record as `param_2`) give:

| R offset | Kind | Target in runtime element E (0x188) | Check helper | Evidence |
|---|---|---|---|---|
| +0x00 | (not read) | | | |
| +0x04 | u32 | blob size (bytes); copied with `memcpy` to a heap buffer (`0x17090`) | | `0xfef40` lines 14-20 |
| +0x08 | u32 | stride of a per-instance buffer, format 0x1c/0x1b (`0x8be60`, count = instances) | | `0xfef40` line 82 |
| +0x14 | rel | blob source (blob = R+0x14+rel) | | `0xfef40` line 22 |
| +0x18, +0x1c | u32 | (inferred) particle-state stride and per-spawn multiplier; read by `0xffe60` through `*(obj+0x10)+0x18/+0x1c` | | `0xffe60` (inferred mapping) |
| +0x28 | rel to name | E+0x00 hash, E+0x08 string, E+0x28 resolved ref | `0xfa1a0` | `0xfd250` |
| +0x2c | rel to name | E+0x38 hash, E+0x40 string, E+0x60 resolved ref | `0xfa1a0` | `0xfd250` |
| +0x30 | rel to name | E+0x70 hash, E+0x78 string, E+0x98 resolved ref | `0xfa1a0` | `0xfd250` |
| +0x34 | rel to name | E+0xa8 hash, E+0xb0 string, E+0xd0 resolved ref | `0xf4e40` | `0xfd250` |
| +0x38 | rel to name | E+0xe0 hash, E+0xe8 string, E+0x108 resolved ref | `0xdc2f0` | `0xfd250`; used as the child-emitter test in `0xfef40` (ref at E+0x108) |
| +0x3c | rel to name | E+0x118 hash, E+0x120 string, E+0x140 resolved ref | `0xb2d20` | `0xfd250` |
| +0x40 | rel to name | E+0x150 hash, E+0x158 string, E+0x178 resolved ref | `0xa8700` | `0xfd250` |

- The name-to-hash step is the same as for every resource reference: `0x14c0f0` makes the reference from a path
  (FNV-1a, `materials-meshes.md` section 2). **observed**
- The helpers `0xfa1a0`, `0xf4e40`, `0xdc2f0`, `0xb2d20` and `0xa8700` are the same generic code (identical
  bodies, 409 bytes each). They look up the reference in the resource factory (`vtbl+0x18` of `DAT_02ee2cf8`) and
  return whether the object is loaded. The four `0xf4e40`/`0xdc2f0`/`0xb2d20`/`0xa8700` variants are template
  instances. **observed (identical code); the resource kinds are unknown**
- The element stride 0x188 includes 7 references of 0x28 bytes each (E+0x00 ... E+0x178). The element also
  stores the blob copy and the buffer V# slots (E+0xa0 onward, from `0xfef40`). **observed**
- The meaning of the references at +0x28, +0x2c, +0x30 is **unknown**. They may be the per-emitter spawn, update and
  emit programs (the `emitter_<n>_spawn/update/emit` names of `compute-duplicates.md`), but no name was resolved.
  **inferred, not verified**

### 3d. Runtime emitter objects built from R (`0xfef40`, 785 bytes, called from `0xfffa0`)

- The emitter builder takes the record R as `param_2` and the element as `param_3`, and stores them at +0x10 and
  +0x18 of its object. **observed**
- It copies R+0x04 bytes from the blob (R+0x14+rel) to a heap buffer and keeps the pointer at object+0x48. A
  V# for that buffer is created at object+0xa0 with format 0x1c and a stride of 0x120 (no child) or 0x110
  (with child) (`0x8be60`). **observed (calls); the blob's internal layout is unknown**
- Without a child reference (E+0x108 is null), it calls `0xff430` (906 bytes) with the record blob at R+0x34,
  and stores its result at piVar[0] of the heap buffer. If the result is -1 it fails. **observed**
- With a child reference, it calls `0xff260` (result at piVar[2]) and builds a 0x78-byte object that holds two
  reference objects and two `0x8bd90` slots, then calls `0xff3a0` and `0xfe320` with the record blob at R+0x38.
  **observed (calls)**
- At the end it calls `0xff7c0`. **observed (call)**

### 3e. Per-instance GPU buffers (`0xffe60`, 318 bytes)

Called from the update loop (`0x103ad0` line 149) with the emitter-instance object and the frame index:

```
count  = instances * T.+0x1c            (instances = system instance count, or 1 if zero)
buf_A  = stride T.+0x18 bytes * count   (V# slot at obj+0xc8 + 0x28*frame; format code 0x1b, flags 0x804)
buf_B  = 0x70 bytes * count             (V# at obj.+0x30 +0x28 ; sub-object)
buf_C  = 9 dwords (0x24 bytes) * count  (V# at obj.+0x30 +0x50)
```

- `T` is the object at +0x10 (the record, see 3d). The per-particle state size is therefore taken from the file.
  **inferred**
- The allocation call is `0x12820` (GPU family heap), and the V# is written by `0x8be60`. The function arguments
  are (V# slot, format, stride, count, flags, base, extra). **observed (signature from the call pattern)**
- The format code 0x1b and flags 0x804 are passed unchanged; their meaning is **unknown**.

## 4. ParticleTexture (`.cptx`)

- Factory `ParticleTexture` (string 0xfa5fd0), build `0x10e9b0`, registration `0x10e910` (called from `0x2569d0`).
  Strings: `ParticleTextureAtlas` (0xfa5fe5), `AllocateRegion(uint32)` (0xfa6047). **observed**
- The build (235 bytes) finds the texture through `0x96680` (texture manager, `DAT_02dd9f80`), gets an id through
  `0x96f50`, looks up a packed handle through `0x10eaa0` in a table at `DAT_02edf140->0xb8`, then stores an entry
  at `table[(handle & 0xffff) * 0x30]` through `0x98800`. It returns a 0x68-byte object (vtable 0x1597d60) that
  keeps the packed handle (u32 at +0x60). Which table is the atlas is **inferred** from the string
  `ParticleTextureAtlas`. **observed (calls); inferred (atlas)**
- So a ParticleTexture is an **atlas slot handle**, not a texture with its own T#. Particle textures are read
  through the atlas (`ParticleTextureAtlas`), not per emitter. **inferred from the build code**

## 5. Compute dispatch data for the emitter programs

### 5a. The dispatch helpers

Both helpers take a context (`param_3`, the GPU command context with the compute binding at +0x1ae0), a command
buffer (`param_2`), and a 16-byte record (`param_4`, `param_5`, `param_8`). They copy the records into a small
block, bind the compute program, write the block pointer into the user-data registers, and dispatch.

| Helper | Address | Block | Dispatch | Callers |
|---|---|---|---|---|
| `FUN_0010bfd0` | 0x10bfd0 (357 bytes) | 0x20 bytes (section 5b) | `FUN_00883870(ctx+0x10, ceil(p6/64), 1, 1, flags)` (`sceGnmDispatchDirect`) | `0x103ad0` lines 109-138 (clear counters and the per-system-instance counters) |
| `FUN_0010c140` | 0x10c140 (411 bytes) | 0x50 bytes (section 5c) | `FUN_00883870(ctx+0x10, 1, 1, 1, flags)` (indirect args prepared, one group) | `0x103ad0` lines 312, 544; `0x106950` (`5` as `p6`) |

- The compute program is taken from the particle-system object (`*(manager+0x38)+0x68`), at offset +0x150 for
  `0x10bfd0` and +0x170 for `0x10c140`, through `0x95580` (reference) and `0x14d7b0` (resolve), then its record at
  `Shader+0x60` is bound with `0x16edc0`. This is the record-list layout of `shader-collections.md` 2b. **observed**
- The user-data register write is `FUN_0085ecd0` (35 bytes), which calls `FUN_00884240` with the CS blob header's
  usage byte (+0x19 of the record, value 2 selects the path). The value written is the address of an 8-byte cell
  (allocated with `0x12820(8, ...)`), and that cell holds the block address. So the program receives a
  pointer-to-pointer through user data (**inferred** from the code order; `0x884240` was not decompiled).
- Labels passed to `0x10bfd0` ("clear global counters", "clear per-system-instance particle counters", ...) are the
  profiler markers for the stage (strings at 0xfa5c29 and below). **observed**

### 5b. Block A (`0x10bfd0`, 0x20 bytes)

```
struct ParticleDispatchA {        // written by FUN_0010bfd0, one per dispatch
  u32  mode;          // +0x00  p5: 3 for "clear global counters", 0 for the others (meaning unknown)
  u32  threads;       // +0x04  p6: number of threads (20 for "clear global counters"); groups = (threads+63)/64
  u32  p7;            // +0x08  p7: taken from the high dword of the call argument (instance or frame value)
  u32  _pad;          // +0x0c  not written
  u8   desc[16];      // +0x10  16-byte record copied from param_4 (V#-shaped, inferred), the counter/state buffer
};
```

### 5c. Block B (`0x10c140`, "prep indirect compute", 0x50 bytes)

```
struct ParticleDispatchB {        // written by FUN_0010c140
  u32  p6;            // +0x00  thread count (indirect size input)
  u32  p9;            // +0x04
  u32  p7;            // +0x08
  u32  p10;           // +0x0c  zero when param_8 is null
  u32  p11;           // +0x10
  u8   _gap[0x10];    // +0x14..+0x1f not written
  u8   desc_a[16];    // +0x20  param_5 (16 bytes)
  u8   desc_b[16];    // +0x30  param_8, or param_5 when param_8 is null
  u8   desc_c[16];    // +0x40  param_4 (16 bytes)
};
```

The records `desc_a`, `desc_b`, `desc_c` are the three buffers that the indirect-prep program needs (for example the
spawn-request list, the counters and the emitter state). Their identity is **unknown** but the code treats them as
16-byte buffer descriptors. **inferred**

### 5d. What the dispatch helpers do not carry

- No effect field (rate, lifetime, curve, colour, spawn shape) is written into blocks A or B. Those come from the
  record blob (R+0x14 copy) and from the element and instance buffers (section 3). **inferred from the writes seen**
- The indirect data written by the prep pass (`0x10c140`) and the spawn requests are the only per-frame counts.
  **observed (writes)**

## 6. Do the programs read data or have it embedded?

Evidence available:

1. The SPIR-V of the 4,253 compute programs (`compute-duplicates.md`) shows that only 24 programs differ from another
   program with the same shape, and then only by one to three Int32 constants. Those constants look like byte or
   dword offsets (steps of 4 and 8) and bit fields (section 4 of that note). **observed (that note)**
2. Spawn and update programs of the same emitter index are structurally different (control flow and resource code).
   Programs of one emitter type do not differ only by constants. **observed (that note, section 3)**
3. The dispatch side writes the parameters as buffers and blobs (section 5) and writes no per-effect number into the
   block (section 5d). **observed**

Conclusion (**inferred**): the values the programs use (counts, rates, curve values, positions) come from buffers and
from the record blob. The layout offsets (which slot and which field of the blob) are fused in the program code as
immediates. So a program is tied to one emitter type's layout, and the values come from data. Two programs of the
same layout could share one pipeline if the offsets are equal. The 24 `const`-only duplicates are the only
evidence of that sharing. Not verified against the blob layout (unknown).

## 7. Open items (next RE step)

1. Decode the record blob (R+0x14) field layout: it is the per-emitter constant block. Needs one `.cprt` byte dump
   (not in this checkout) and the blob's reader (`0xff430` for R+0x34, `0xff260` for the child branch).
2. Name the three references at R+0x28, +0x2c, +0x30 (spawn/update/emit `.csdr` candidates) by reading one `.cprt`
   and checking the names against `predict_table.tsv` pass names.
3. Decode `0xff430` (906 bytes): what it builds from R+0x34 (curve or module table?).
4. Find the caller chain of `0xfffa0` (emitter instance creation) to confirm the stride fields R+0x18 and R+0x1c.
5. Check the CS blob usage byte at +0x19 for the particle programs (the usage value 2 that selects the user-data
   write in `0x10bfd0`).
6. Check the format code 0x1b and the flags 0x804 in `0x8be60` to know the V# format of the particle buffers.

## 8. Evidence list

| Item | Address | File |
|---|---|---|
| Registration helper (insert) | 0x1444e0 | `out/re/decomp/0014/001444e0.c` |
| Descriptor helpers | 0x144640, 0x144750 | `0014/00144640.c`, `0014/00144750.c` |
| ParticleSystem registration | 0xfde80 | `out/re/decomp/000f/000fde80.c` |
| ParticleSystem build and extra fn | 0xfdf30, 0xfdfd0 | `000f/000fdf30.c`, `000f/000fdfd0.c` |
| ParticleSystem parser | 0xfd250 | `000f/000fd250.c` |
| Emitter builder | 0xfef40 | `000f/000fef40.c` |
| Emitter buffers | 0xffe60 | `000f/000ffe60.c` |
| ParticleTexture registration and build | 0x10e910, 0x10e9b0 | `0010/0010e910.c`, `0010/0010e9b0.c` |
| Simulation loop | 0x103ad0 | `0010/00103ad0.c` |
| Dispatch helpers | 0x10bfd0, 0x10c140 | `0010/0010bfd0.c`, `0010/0010c140.c` |
| CPU particle walk | 0x1013f0, 0x101720 | `0010/001013f0.c`, `0010/00101720.c` |
| User-data write | 0x85ecd0 | `0085/0085ecd0.c` |
| Dispatch wrapper | 0x883870 (import 0x8838d0 `sceGnmDispatchDirect`) | `0088/00883870.c`, icall output |
| Texture registration wrapper | 0x98a90 | `0009/00098a90.c` |
| Particle strings | 0xfa548f (ParticleSystem), 0xfa91a1 (default.cprt), 0xfa5fd0 (ParticleTexture), 0xfa8d86 (particle-shaders.cslt) | eboot string scan |
