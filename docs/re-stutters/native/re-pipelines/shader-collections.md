# Shader collections (.cslt), shader resources (.csdr) and how a draw reaches them

Read-only static analysis of the EU 1.01 eboot (`out/re/decomp/<hi16>/<addr>.c`, Ghidra export).
No game dump is in this checkout (`../CUSA08809` is absent), so no `.cslt` or `.csdr` byte was read.
Addresses are eboot VAs. Labels: **observed** (decomp, string or table in the repo), **inferred**, **unknown**.

## 1. Answer

- A `.cslt` is a **named list of shader resources** (`ShaderList`, FourCC `cslt`). It holds no shader code.
  Each list entry is a name; the loader resolves it through the resource factory and keeps a 16-byte
  reference. **observed** (0xb0e4d0)
- The shader code lives in **`.csdr` resources** (`Shader`, tag `rdsc`, file magic `RDSC`). One `.csdr` is one
  named pass shader (for example `gbufferbakedambient`, `depth`, `shadow`, `cs_ambientshadow`). Its body is an
  array of 228-byte records, each pointing to one GCN program blob or, for kind 3, to two blobs.
  The `OrbShdr` footer is inside each program blob. **observed (loader) / inferred (footer placement)**
- A **material** (`.cmat`, 0x638-byte object) holds 14 fixed pass slots (72 bytes each, at `+0x60`). Each slot
  names one `.csdr` by reference and lists up to five 32-byte program records from it (VS, PS and so on, chosen
  by a 5-bit mask). A material also has a list of sub-materials at `+0x600` ("variants"), each with the same
  14-slot layout. **observed (layout) / inferred (variant meaning)**
- A draw does not look up a shader by name or hash. It binds the program records already stored in the material
  slot (for example VS at `r13+0xc0b0`, PS at `+0xc0a8`), as described in report 08. The name-to-slot mapping
  and the choice of slot per draw are **unknown** (not traced). **observed (binding) / unknown (choice)**
- Permutation keys (fetch formats, bindings, runtime info) are **not** in any engine resource. They are built by
  the port per draw (see report 08, H4). **observed (absence)**
- Render state (blend, depth, raster) is **per material**, not per pass and not in a `.cslt`. The material header
  selects a state object (`material+0x560`) from a global table (`DAT_02ee3ce0`) or builds a blend word with
  `FUN_0008f1d0`. **observed (code)**. Report `render-states.md` in this folder covers the same area; the material
  side here is only what is needed for the pass structure.
- Counts: 3 engine `.cslt` files are named in code (`engine-shaders`, `particle-shaders`, `lightprobe-shaders`).
  Other `.cslt` files, if any, are **unknown** (not in strings or in the cwks table; need the dump). The game
  has **17,196** `.csdr` shader files (comment in `scripts/hooks.d/csdr.py`). The port's `predict_table.tsv` has
  **262 distinct pass names** over **16,760 programs** (8,135 pixel, 4,364 vertex, 4,253 compute, a few
  geometry and tessellation). **observed (counts from repo files)**
- Boot: the three engine `.cslt` are global references created by static init (`0x143f50`). Whether their
  entries are resolved and loaded at boot, or on first use, is **unknown**. The `.cslt` loader asks for each
  listed resource at a fixed priority (inferred). Area content comes through `ResourceDependenciesFile`
  (`$/workspaces/_ps4/shadow.cwks`), which is loaded at startup by `GameCoreLoop::LoadResourceDependencyFile`.
  **observed (refs, log line) / inferred (eager load)**

## 2. The loaders

### 2a. `.cslt` (ShaderList)

- Registration: `FUN_00b0e3a0` (0xb0e3a0). It builds a factory entry named `"ShaderList"` with build function
  `FUN_00b0e440` (0xb0e440) and calls `FUN_001444e0(factories, 0x63736c74, entry)` (FourCC `cslt`).
  Caller: `FUN_002569d0` (0x2569d0, `AreaLoad_ShaderListAndAssets`), which is called from `FUN_00533100`
  (0x533100, the function the port hooks as AREA_PRECACHE). **observed**
- `FUN_00b0e440` (135 bytes): reads the file buffer (`param_1+8` data, `+0x10` size) through `FUN_0008df50`, makes
  a 0x78-byte object with `FUN_00017070(0x78)`, and calls the parser `FUN_00b0e4d0`. **observed**
- `FUN_00b0e4d0` (0xb0e4d0, 719 bytes): the parser. File layout (**observed**):

```
file:  u32 count
       s32 rel[count]         ; rel[i] at file+4+4*i; target = (address of rel[i]) + rel[i]; 0 means empty
       ...                    ; the names (C strings) at the targets
object (0x78 bytes):
  +0x00  vtable 0x15d5990
  +0x60  pointer to the file buffer (count at +0)
  +0x68  pointer to the entry array: u64 count, then count entries of 16 bytes
         entry = { u64 ref (resource object), u8 owned flag, pad }
```

  For each entry the code builds a name string (`FUN_0014c2f0`) and calls the factory lookup
  (`vtbl+0x18` of `DAT_02ee2cf8`, the resource factory). If the lookup returns a load request (flag at
  `local_40`), it calls `FUN_0014cf20(ref, 0xd, ...)`. The value 0xd is read as a load priority (**inferred**).
  So opening a `.cslt` asks for every shader it lists. **observed (calls) / inferred (priority meaning)**

- Static references (**observed**, `FUN_00143f50`, 0x143f50): global `ResourceRef` objects at `DAT_02ee2a58`
  (`engine-shaders.cslt`), `DAT_02ee2a78` (`particle-shaders.cslt`), `DAT_02ee2a98` (`lightprobe-shaders.cslt`),
  each with an `atexit` destructor. Those are the three engine collections. The same function also names
  `.cmat` and `.cmsh` files, which are not part of this report.

### 2b. `.csdr` (Shader)

- Registration: `FUN_00b0d3e0` (0xb0d3e0). Factory entry `"Shader"`, build `FUN_00b0d4a0` (54 bytes: allocates
  0x88 bytes, calls ctor `FUN_00b0ced0`), tag `0x63736472` (`rdsc`), and a name builder `FUN_00b0cc50` (0xb0cc50,
  624 bytes). Caller `FUN_002569d0`. **observed**
- File name: `FUN_00b0cb80` (0xb0cb80, 201 bytes) appends a build suffix to the resource name. The suffix is
  indexed by the build type (`DAT_015d5820[type]`, index 0 is unused). Strings (**observed**):

| Type index | Enum name (`EShaderTypeToLoad`) | Suffix | Note |
|---|---|---|---|
| 0 | `Debug` | (none, the suffix call is skipped) | **inferred** |
| 1 | `Shipping` | `_shipping.csdr` | the PC build loads this (log: `+shaderTypeToLoad=Shipping`) |
| 2 | `PS4Neo_Debug` | `_neo.csdr` | |
| 3 | `PS4Neo_Shipping` | `_neoshipping.csdr` | |

  The enum table is `DAT_03305e00` (built in `FUN_00b0d580`, `FUN_00b0da00`, `FUN_00b0ddf0`). The cvar
  `shaderTypeToLoad` is read through `Cvar_ShaderTypeToLoad` (0xb0e250). **observed**
- The port's `predict_table.tsv` strips `_<hex>` and `_shipping.csdr` from file names to get the pass name, so
  the file name is `<pass>[_<hash>]_shipping.csdr` (`tools/csdr/predict_table.py`). **observed (tool)**
- Ctor `FUN_00b0ced0` (878 bytes), object 0x88 bytes (**observed**):

```
Shader object (0x88)
  +0x60  records base (32-byte records)         [param_1[0xc]]
  +0x68  records end                            [param_1[0xd]]
  +0x70  records capacity end                   [param_1[0xe]]
  +0x80  file buffer pointer (u32 count at +0)  [param_1[0x10]]
  +0x6c, +0x74, +0x70, +0x1c   variant range / wrap flags (used by FUN_00b0da00, see 3c)
```

- File records (228 bytes each, **observed** from the stride and the switch):

```
file: u32 count, then count records starting at file+4
record i at  R = file + 4 + 228*i
  R+0x00   u32 kind          switch: 0,1,2,4,5,6 -> one program blob; 3 -> two blobs
  R+0xE0   s32 rel           blob address = (R+0xE0) + rel   (0 = none)
  blob      FUN_00b2ecb0 (kind != 3) / FUN_00b2ece0 (kind 3) read a header (size, data pointer)
```

  Output: one 32-byte record per file record, written at the records base (`Shader+0x60 + 32*i`) (**observed**):

```
out record (32 bytes)
  +0x00  pointer to the file record (R)
  +0x08  pointer to the blob header ("regs block"): the ctor stores the code address
         into header dwords 2 and 3 (addr >> 8, low and high), which matches the
         "code = (u32)[obj+8] << 8" rule in report 08 section 2a
  +0x10  pointer to the copied program bytes (FUN_00012820, GPU family heap DAT_02dd9f84)
  kind 3 only: the second blob pointer goes into the header at dword index 0x10 + byte(header+3)
```

  So the material's "VS object" is the blob header (`out +0x08`), not the file record. **inferred**
  (from the matching store offsets; the names of the fields are not in the code)

## 3. What a material holds, and how a draw reaches a program

### 3a. Material object (`FUN_000f9390` factory, 0x638 bytes; parse `FUN_000f6c90`, 5,137 bytes)

Fixed pass slots (**observed**): `FUN_000f6c90` clears 14 slots at `material+0x60 + 0x48*k` (k = 0..13, the last ends
at `0x450`). For a slot `B = material+0x60+0x48*k`:

```
B+0x00  ref to the .csdr resource (name, then resolved object through the factory)
B+0x10  record pointer 0   (set when mask bit 0)
B+0x18  record pointer 1   (mask bit 1)
B+0x20  record pointer 2   (mask bit 2)
B+0x28  record pointer 3   (mask bit 3)
B+0x30  record pointer 4   (mask bit 4)
B+0x38  u32 kind           (valid values 17, 23, 25, 31; otherwise 17)
B+0x40  interned object, from FUN_00099720(DAT_02ed37b0, rec0, rec1) (a keyed map, mutex-protected)
```

Parse of one slot entry (**observed**, `0xf6c90` lines ~190-225): the file gives a slot index `k` (u32),
then a 5-bit mask (u32). For each set bit the pointer is `Shader.records + 32 * n`, where `n` counts the set bits
before it, so the slot points into the `.csdr` records. The name is resolved with `FUN_000fa1a0` (factory
lookup, `vtbl+0x18`) and then `FUN_0014d7b0` gives the `Shader` object.

- The 14 slots and the variant list: at `+0x600` a sentinel list head holds sub-material nodes; each node has
  the sub-material at `+0x20`, and `FUN_000f8270` (1,811 bytes) parses it with the same 14-slot layout. The
  meaning of a variant (a define mask, a quality level, a time-of-day case) is **unknown**. **inferred** (same
  struct, one parse per node)
- The slot index `k` to pass name mapping (for example "slot 3 = depth") is **unknown**. The names of the `.csdr`
  files are in the material's name references, which are in the dump only.

### 3b. Binding (report 08, re-used)

- `Material_BindGraphicsShaders_A` (0x85f410) and `_B` (0x85fc10) bind the VS and PS objects (`r13+0xc0b0`,
  `+0xc0a8`), and `Material_BindComputeShader` (0x85f220) binds the CS object (`+0xc0d8`). **observed**
- Each bind uses the record's blob header; the engine does not index a collection by name at draw time.
  **observed** (only these binders call the graphics shader imports)
- The caller that picks slot `k` for a draw is **unknown**: `sub_862c50` (0x862c50) builds the draw record from
  the VS object and has no direct caller in the decomp (vtable callers).

### 3c. Variant selection inside a `.csdr` (kind selection, not permutation)

- `FUN_00b0da00` (994 bytes) maps an integer value to a record index. It uses the object's range `[+0x6c, +0x74]`
  and a wrap flag (bit 0 of `+0x1c`), or looks the value up in a `{int key, name}` table. **observed**
- `FUN_00b0d860` (113 bytes) adds an integer offset and clamps it, then calls `FUN_00b0da00`. **observed**
- These are a generic "integer parameter selects a program variant" path (an enum or LOD-like value). Nothing
  in the code ties them to draw state. **inferred**

### 3d. Render state (per material)

- Material header words (**observed**): `+0x454`, `+0x458` (kind, 0..7), `+0x45c` (a clamped value from `+0x460`),
  `+0x460` (`uVar25`, selects a case in the switch at 0xf6c90 ~line 556), `+0x464` (flags; bit 0x40 is special),
  `+0x450` (flags, set from sub-blocks at `+0x3d0`, `+0x418`).
- `material+0x560` is one state pointer per material. It is taken from the global table `DAT_02ee3ce0` at
  offsets `+0x28`, `+0x30`, `+0x48`, `+0x58`, `+0x60`, `+0x70`, or built by `FUN_0008f1d0` (171 bytes). **observed**
- `FUN_0008f1d0` packs a 32-bit blend word with the layout of the GCN `CB_BLEND0_CONTROL` register, as the
  bit positions in `FUN_0087c3b0` (color src bits 0-4, comb 5-7, dst 8-12), `FUN_0087c3d0` (alpha src 16-20,
  comb 21-23, dst 24-28), `FUN_0087c390` (bit 30, enable) and `FUN_0087c400` (bit 29, separate alpha) show.
  The default call uses src 4 (SRC_ALPHA), dst 5 (ONE_MINUS_SRC_ALPHA), comb 0 (ADD). **observed (bits)** /
  **inferred (register name and factor values)**
- So blend, depth and raster state are chosen by the material, not by the `.cslt` or by a pass. The pass slots
  hold only shader records. Depth and raster are not traced here (see `render-states.md`). **observed**

## 4. Number of collections and when they load

- Engine `.cslt` named in code: 3 (`0x143f50`). **observed**
- Dump size: 17,196 `.csdr` files (from `scripts/hooks.d/csdr.py`, a comment; not re-counted here). **observed (comment)**
- Pass names in the port table: 262 distinct stems (`user/shader_predict/predict_table.tsv`, column 3). These are
  the `.csdr` names that appear in the dump. **observed (repo file)**
- Other `.cslt` files: **unknown**. They would appear in the psarc listing (`tools/csdr/psarc.py`, needs the dump).
- Boot: the static references exist from static init; their resolution time is **unknown**. The resource
  dependency file (`shadow.cwks`, log line `Resource dependency file loaded`) lists `ResourceRef`s
  (`ResourcePool::LoadResourceDependencies`, 0x14efb0). Whether a `.cslt` is among them is **unknown**.
  **observed (refs) / unknown (timing)**

## 5. Port use (run-time, no new engine hook needed for the data)

1. **Material walk (best candidate).** After a material is built by `FUN_000f9390` (factory `Material`, tag
   registered at `0xf9270`), walk its 14 slots at `material+0x60 + 0x48*k` and each variant node from `+0x600`.
   For every non-null record `rec` read `rec+0x08` (the blob header, with the code address in dwords 2-3) and
   `rec+0x10` (the copied code). This gives the set of program code addresses that the material can bind. Only
   materials that are loaded are visible, so the set is the loaded part of the game. **inferred (design)**
2. Verify first with a probe that logs, per material, the slot count and the code addresses (no change to draws).
   Then compare the logged code addresses with the program hashes the port already sees on draws (report 08).
3. **Shader objects** (`.csdr`, 0x88 bytes) can be walked from the record list at `+0x60` (end at `+0x68`); the
   file buffer is at `+0x80`. These are available as soon as the `.csdr` is loaded.
4. **Collections**: the `.cslt` object (0x78 bytes) gives the list of names it asked for at `+0x68` (entries of
   16 bytes). It can be read after `FUN_00b0e4d0` returns, to learn which `.csdr` files a collection needs.
5. Permutations and render state still come from draw state. Only the program code and the per-material state
   pointer can be read in advance.

## 6. Open questions

- The names inside a `.cslt` (are they pass names like `depth`, or `.csdr` ids with a hash?). Needs the dump.
- The meaning of the 5-bit mask bits and of the kind values 17/23/25/31, and whether kind 3 is a fetch or a
  copy program.
- Which `.cslt` (if any) other than the three engine ones are loaded per area.
- How a draw chooses a slot `k` (the pass index from the caller) and whether it uses a variant node from `+0x600`.
- The owner of `DAT_02ee3ce0` (what each state entry holds) and of `DAT_02ed37b0` (the keyed map of interned
  record pairs).
- Whether `FUN_0014cf20(ref, 0xd, ...)` is a load request with priority 13 (observed call, meaning inferred).

## 7. Evidence list (addresses and files)

| Item | Address | Source file |
|---|---|---|
| Static `.cslt` refs (three engine files) | 0x143f50 | `out/re/decomp/0014/00143f50.c` |
| `ShaderList` registration (`cslt`) | 0xb0e3a0 | `out/re/decomp/00b0/00b0e3a0.c` |
| `.cslt` object build | 0xb0e440 | `out/re/decomp/00b0/00b0e440.c` |
| `.cslt` parser | 0xb0e4d0 | `out/re/decomp/00b0/00b0e4d0.c` |
| `Shader` (`rdsc`) registration | 0xb0d3e0 | `out/re/decomp/00b0/00b0d3e0.c` |
| `Shader` factory and ctor | 0xb0d4a0, 0xb0ced0 | `out/re/decomp/00b0/00b0d4a0.c`, `00b0ced0.c` |
| `.csdr` name builder and suffix | 0xb0cc50, 0xb0cb80 | `out/re/decomp/00b0/00b0cc50.c`, `00b0cb80.c` |
| Build-type enum and selector | 0xb0d580, 0xb0da00, 0xb0ddf0 | `out/re/decomp/00b0/00b0d580.c`, `00b0da00.c`, `00b0ddf0.c` |
| Suffix table `DAT_015d5820` (read from the eboot) | 0x15d5820 | eboot bytes (read with `tools/re/eboot_re.py` Image) |
| Material factory and parse | 0xf9390, 0xf6c90 | `out/re/decomp/000f/000f9390.c`, `000f6c90.c` |
| Variant node parse | 0xf8270 | `out/re/decomp/000f/000f8270.c` |
| Ref lookup through the factory | 0xfa1a0 | `out/re/decomp/000f/000fa1a0.c` |
| Blend word builder | 0x8f1d0, 0x87c3b0, 0x87c3d0 | `out/re/decomp/0008/0008f1d0.c`, `0087/0087c3b0.c`, `0087/0087c3d0.c` |
| Area precache and loader setup | 0x533100, 0x2569d0 | `out/re/decomp/0053/00533100.c`, `0025/002569d0.c` |
| Pass names and counts | - | `user/shader_predict/predict_table.tsv`, `tools/csdr/predict_table.py` |
| Build-type cvar and strings | 0x12cfbf1, 0x12cfc34-0x12cfcaf | eboot strings (`tools/re/eboot_re.py str`) |
