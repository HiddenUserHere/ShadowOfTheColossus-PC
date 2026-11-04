# Materials (.cmat) and meshes (.cmsh): what they hold and what they fix in a pipeline

Read-only static analysis (agent R3). Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`
(first line of each file lists the callers), `out/re/ghidra/eboot_ghidra.elf` for strings, and the
earlier notes in `docs/re-backend/02-resource-objects.md`, `docs/re-stutters/reports2/08-shader-first-use.md`
and `docs/re-stutters/native/loader.md`. Addresses are eboot VAs. Labels: **observed** (decomp or string,
address given), **inferred** (reasoning from decomp, not confirmed by data), **unknown**.

Not done: no game data was read (the dump is not inspected), so no field value below is confirmed on a real
file. Every layout is from the parsers only.

## 1. Answer in short

- **Material (.cmat) holds the texture-slot table, a pass-keyed lookup, two shader-collection reference
  lists, and a render-state selector.** It does not hold the shader binaries, the render-target formats, the
  blend/depth state objects, or the vertex format. **observed (parser)**
- **Mesh (.cmsh) holds the vertex attribute array, the index format and count, the section table, and the
  vertex/index data.** The mesh parser never reads a primitive type or a per-section material id in the
  parts decoded here. **observed (parser) / primitive type: unknown**
- **Draw-time state that selects the program:** the VS/PS/CS objects bound per draw (ctx+0xc0b0, +0xc0a8,
  +0xc0d8), the pass object passed to the draw-record builder, and the render-target and mask state of the
  context (ctx+0xc5d8, +0xc5e0, +0xe0..+0x120). These are set by the caller, not by the material. **observed
  for the reads; the setter of the VS/PS objects is not found**
- **Pipeline key ahead of first draw: partly possible.** Texture-slot formats (the static T# words), the
  texture slot layout, the vertex attribute array and the index format are known once the resources are
  loaded. The shader binary per pass and the render-target formats are not known until the draw. See section 6.

## 2. Resource registration and loaders

| Item | Address | Evidence |
|---|---|---|
| Material type registration | `0xf9270` (278 bytes), called from `0x2569d0` (`AreaLoad_ShaderListAndAssets`) | decomp `000f/000f9270.c` |
| Factory name / tag | name `"Material"`, build function `0xf9390`, tag value `0x636d6174` | `000f9270.c` (`FUN_00144640`, `FUN_001444e0`) |
| Default material loaded at registration | `DAT_02ee2bf8` = `$/coredata/enginesupport/materials/_ps4/default.cmat` | `0x143f50` (string list), `0xfa9002` |
| Factory build | `0xf9390` (124 bytes): allocates the 0x638-byte object (`0x17070`), calls the parser `0xf6c90(obj, stream, 1)` | `000f/000f9390.c` |
| Outer loader with header check | `0xf9530` (857 bytes): reads dword `0x4d415453` (`"STAM"` in file order) and dword `0x39` (version 57), then one resource ref, then `0xf6c90(obj, stream, 0)` | `000f/000f9530.c` |
| Parser | `0xf6c90` (5,137 bytes), returns 1 on success | `000f/000f6c90.c` |
| Mesh registration | tag `0x636d7368` (`cmsh` in the tag value, file bytes `hsmc`), build `0x1264d0` (4,988 bytes) | `docs/re-stutters/native/loader.md` (mesh rows) |
| Mesh object | two classes: `0x130` bytes (`FUN_00125bd0`) or `0x178` bytes (`FUN_00128d80`), chosen by one flag byte read first | `0012/001264d0.c` lines 224-231 |

Tag and magic values are 32-bit little-endian reads (the same convention as `.cslt` `0x63736c74`).
**observed**. The file bytes are the reverse of the name in the constant; the port should compare the dword
value, not the text.

Resource identity (**observed**, `0x161f80`, `0x14c0f0`, `0x14c3f0`): a resource is named by path. The name
is hashed with FNV-1a 32-bit (`0x811c9dc5` offset, prime `0x1000193`, xor-then-multiply). Before hashing, a
4-character placeholder `"****"` (string at `0xfaa184`) in the path is replaced by a 4-character code from
`FUN_00147e20(DAT_02ee2d00, last4)`. The paths in the material and mesh files do not contain `"****"`
(**observed** for the constant strings only). So a reference in a file is a path, and the port can compute the
same hash to find a loaded resource.

## 3. Material object (.cmat), layout from `0xf6c90`

Object: 0x638 bytes. Parse order and offsets (**observed** unless marked). The parser reads the stream in this
order: dword A, dword B, dword C, dword D, dword E, dword F, count N, then N texture-slot records; then
two reference lists; then two raw blobs; then two hash maps.

| Offset | Type | Content | Notes |
|---|---|---|---|
| `+0x060 + i*0x48`, i = 0..13 | `MatSlot[14]` | texture slots (see 3.1) | parser does not check the slot index against 14 |
| `+0x450` | u32 | populated-slot mask: bit i = slot i has a non-null view 0 | computed at end of parse. **observed** |
| `+0x454` | u32 | file dword A | not interpreted in the parser (**unknown**) |
| `+0x458` | u32 | file dword B, "kind" | selects the render-state object and flag bits (3.3) |
| `+0x45c` | i32 | `(kind > 2) ? 2 : 2 - dword 6` | not interpreted outside the parser (**unknown**) |
| `+0x460` | u32 | file dword D, "pass kind" (set to 0 when kind == 3) | selects the state object (3.3) |
| `+0x464` | u32 | file dword C, flags; bit `0x40` is tested | changes the state choice and adds `0x80` to `+0x468` |
| `+0x468` | u32 | render flags, computed (3.3) | |
| `+0x470`, `+0x478`, `+0x480` | ptr | blob A: begin, end, capacity | raw bytes from file, size = dword 10 |
| `+0x490`, `+0x498`, `+0x4a0` | vector | list 1 of 16-byte refs `{void* res; u8 owned}` | count = dword 8. Shader-collection candidate (**inferred**) |
| `+0x4b0`, `+0x4b8`, `+0x4c0` | vector | blob B | size = dword 11 |
| `+0x4d0`, `+0x4d8`, `+0x4e0` | vector | list 2 of 16-byte refs | count = dword 9 |
| `+0x4f0` | map header | map 1: `u32 key -> {u32 key, u32 value}` (node `+0x20` key, value at `+0x28`) | size in bytes = dword 12, 8-byte records |
| `+0x528` | map header | map 2, same format | size in bytes = dword 13, 8-byte records |
| `+0x560` | ptr | render-state object chosen by kind (3.3) | points into a global table at `DAT_02ee3ce0` (contents unread) |
| `+0x578` | `FUN_000f5cd0` object | per-item payloads of list 1 (`+0x3c` count, `[0]` payload array, `[1]` copy of blob A) | built from blob A |
| `+0x5b8` | same | per-item payloads of list 2, from blob B | |
| `+0x5b0`, `+0x5f0` | i32 | nonzero adds `0x10` / `0x20` to `+0x468` | set outside the parser (**unknown**) |
| `+0x600` | list head | list of sub-objects; each node `+0x20` is passed to `0xf8270` | **inferred**: sub-material or parameter blocks |

Blob contents are not decoded. Blob A and blob B are copied and each list-1/list-2 item is given a 32-byte
record built from them (`0xf5cd0`). Whether they hold technique or pass names, or constants, is **unknown**.
No separate constant-buffer read appears in `0xf6c90`.

### 3.1 Texture slot (MatSlot, stride 0x48)

For slot i the base is `obj + 0x60 + i*0x48`. Layout (**observed** from `0xf6c90` lines 250-400 of the decomp):

```
struct MatSlot {              // 0x48 bytes, base = obj + 0x60 + i*0x48
  void*   texRes;             // +0x00  resource ref (texture), loaded with type 0xd  (inferred)
  u8      ownsRef;            // +0x08  release flag
  void*   view[5];            // +0x10 .. +0x30  pointers to 32-byte descriptors (T#/V#), see below
  u32     viewMask;           // +0x38  file-provided value (only 17, 23, 25, 31 are accepted, else 0x11)
  void*   extra;              // +0x40  built by 0x99720 when (viewMask & 6) != 0 (inferred: sampler/second view)
};
```

- File fields per slot: the slot index (uVar22), then `viewMask` (uVar5), then a resource path.
- `viewMask` is a 5-bit selection of descriptor views inside the resource (bit 0 = view 0 at `+0x70`, bit 1 at
  `+0x78`, bit 2 at `+0x80`, bit 3 at `+0x88`, bit 4 at `+0x90`). The stored descriptor pointers are compacted
  in bit order: `res_payload + 0x60 + 0x20*k`, with k counting set bits. **observed**
- Accepted `viewMask` values are the set `{17, 23, 25, 31}` (bitmask `0x82820000`). Other values are replaced
  by `0x11` (17). **observed**
- The resource payload pointer comes from `FUN_0014d7b0(res)` (**inferred**: returns
  the loaded object of a resource). The 32-byte descriptors sit at payload `+0x60`. This does **not** match
  the CTexture T# at `+0xc4` in `docs/re-backend/02-resource-objects.md`. The two objects may differ
  (texture resource vs its view table); not resolved.

### 3.2 Mapping slots to the T# fields we need

The file stores the T# raw. `docs/re-backend/02-resource-objects.md` section 3 says the texture object keeps its
32-byte T# from the `.ctxr` stream (`0x8de50`) and rewrites only dword 0 (base address, `0x881e60`) on
residency. So the format, type, depth, and sRGB words are fixed at load and can be read once. **observed
(doc) / inferred (which object holds the view table, see 3.1)**

### 3.3 Render-state selector and flags (observed)

- `+0x560` is set from file dword D (`+0x460`) with `switch`:
  - case 1: if kind == 6 then `DAT_02ee3ce0+0x58`, else the default (`+0x30`)
  - case 2: `+0x48`; case 3 and 8: `+0x30`; case 4: `+0x70`; case 5: `+0x60`;
  - case 6: if `(flagsC & 0x40)` then `+0x30`, else `FUN_0008f1d0(DAT_02ed33f0, 0, 4, 5, flag)` (builds an object)
  - default: `+0x28`.
  Contents of the table are unread. **observed (selector), unknown (meaning)**
- `+0x468` (render flags) is built as: base 1 (or `0x41` when kind is 4 or 7); `|0x200` when kind is in
  `{1, 4, 7}` (mask `0x92`); `|0x80` when flagsC has `0x40`; then `|0x10a`, or `|0x11a` when `+0x5b0` is
  nonzero; `|0x20` when `+0x5f0` is nonzero. **observed**. Bit meanings are
  **unknown** (names not in `tools/re/names.txt`).
- `+0x450` (populated mask) is the only per-slot summary the port can use without reading the slots. **observed**

### 3.4 Shader references and pass lookup

- List 1 (`+0x490`) and list 2 (`+0x4d0`) hold `{void* res; u8 owned}` refs. Each entry is read as a path
  (`0x14c3f0`, FNV hash) and loaded (`0xf80f0` with the manager at `DAT_02ee3ce0+0x340`). Given the
  `.cslt` parser (`docs/re-stutters/reports2/08-shader-first-use.md` section 2b: a `.cslt` is a list of named
  references), list 1 and list 2 are most likely shader collections. **inferred**
- Maps 1 and 2 (`+0x4f0`, `+0x528`) are keyed by a u32 read from the file, with a u32 value. The key is probably
  a hash of a pass or technique name. This is the only pass-to-entry lookup in the material. **inferred**
  (the key's meaning is not checked against any name list)
- The pass object used at draw is not found in the material parse. The bind code (section 4) takes pass
  objects from the caller. **unknown**

## 4. Draw path: which object selects the program and states

Chain (**observed** call graph; each step checked in the decomp listed):

1. Pass submitters `0x94ec0` (234 bytes), `0x94dd0` (231) and `0x94d00` (204), file `0009/…`. Parameters:
   a context `ctx` (`*param_1`), a pass kind (`param_2`, table `DAT_00f9f2e0[kind]` gives the value passed to
   `0x86a900`, which sets a mode), and the draw arguments. Each one:
   - writes the mode (`0x86a900` with `DAT_00f9f2e0[kind]`, or `9` when `param_1[3] & 6` is set);
   - calls `0x862510` (writes SGPR user data for the object usage bytes at `+0xc0c0`/`+0xc0d0`, via `0x86d550`);
   - calls `0x85fc10` (`Material_BindGraphicsShaders_B`, object `ctx+0x2e8`);
   - draws with `0x86b040` (`0x94dd0`, `0x94d00`) or `0x86ac10` (`0x94ec0`). The draw kinds are inferred from the names of the callees only.
   The callers of `0x94ec0` include `0x118470` (`Frame::InsertWaitFlipDone` in names.txt, which is probably a wrong hint).
2. `0x85f410` (`Material_BindGraphicsShaders_A`, 2,042 bytes, called only by `0x862c50`). Per bind it sets:
   - VS at `ctx+0xc0b0` (code `obj+8 << 8`, size `obj+0 & 0x7fffff`), emitted via `0x86c170`/`0x86c250`;
   - PS at `ctx+0xc0a8`, via `0x86c050`/`0x86c110`;
   - CS at `ctx+0xc0d8` (`0x85f220` compute path; `0x861880` sets it, see report 08);
   - outputs `*p2 = ctx+0xc5d8` (u8) and `*p3 = 0xffff << (ctx+0xc5e0 & 31)`: two state fields that look like
     target and export masks (**inferred**). Both are set outside this function.
   - user-data dwords copied from `ctx+0x3090` into a bump block, then `0x86d4c0` per usage slot.
3. `0x862c50` (`sub_862c50`, 1,248 bytes) builds the **draw packet**: 0x54 bytes from `ctx+0x40` pool
   (`0x883f80`), at `ctx+0xc8b8`. Field sources (**observed**):
   - `[0x00..0x0f]` = `ctx+0xe0`, `ctx+0xe8` (8 bytes each);
   - `[0x10..0x1f]` = pass `+0x10`, `+0x18` (the pass argument `param_2`);
   - `[0x20]` u16 = pass `+0x04`; `[0x22]` u16 from `ctx+0xf0`; `[0x24]` u32 = `ctx+0xe8` low dword;
   - `[0x2a], [0x2b]` = pass `+0x08` bytes; `[0x34]` u32 = `ctx+0xf8`; `[0x3c]` u32 = `ctx+0x100`;
   - `[0x42]` u16 = `ctx+0x11c`; `[0x44..0x4f]` = `ctx+0x104`, `ctx+0x10c`.
   The record is built from context state, so whatever sets `ctx+0xe0..0x120` decides it.
   Its only known caller is a vtable slot (not found). **observed / caller unknown**
4. `0x862510` (211 bytes) selects usage bytes `+0x19` from `ctx+0xc0c0` (`0xb0`) or `ctx+0xc0d0` (`0x45`, `0xad`,
   `0x145`, `0x1ad`) and writes them with `0x86d550`. **observed**

### 4.1 Mesh draw inputs (observed)

- The mesh's index and vertex data are uploaded once to GPU memory in the build job (`0x8be60`, see
  `docs/re-stutters/native/loader.md` W1). After that the draw reads them by address. The material does not
  change them.
- Per draw, the port knows the index size and count from the mesh (section 5).

## 5. Mesh object (.cmsh), layout from `0x1264d0`

Header read order (**observed**, `0012/001264d0.c`):

1. 1 byte `cVar38`: chooses the object class (`0x130` or `0x178` bytes).
2. dword `flags` (`uVar12`):
   - bit 0: one resource reference read by name (`0xf4a80` + `0x14c0f0`), stored as the mesh's parent link.
     What it points to is **unknown** (material or shader list are the candidates; not checked).
   - bit 1: vertex attribute block (section 5.1).
   - bit 2: index block for the main draw (section 5.2).
   - bit 4: second index block with its own object (`0x68` bytes, `0x8be60` upload).
   - the final call `vtbl+0x28` on the new object completes the load with the flags.

### 5.1 Attribute block (bit 1)

- Section table: count, then `count` records of `0x50` bytes via `0x1286b0`. Fields used later: two 8-byte
  values at `+0x3c` and `+0x44`. Primitive type is **not** found in the parts read. **unknown**
- Element array: count N, then N records of `0x28` bytes (4 qwords and a dword). Built by `0x162970`/`0x162120`
  reads. Meaning **unknown**.
- Second array: count, then records of `0x30` bytes. Each reads an index into the section table (dword), then two
  qwords from `section+0x3c` and `section+0x44`, and a dword. Likely the draw ranges per section. **inferred**
  (the record uses `table[idx]` fields, so the index is a section id).

### 5.2 Index and vertex data (bit 2)

- `0x154e00` reads an index descriptor: dword `A` = index type (1 = 16-bit, 0 = 32-bit, others: size 0 but
  still reads 4 bytes per entry, **observed** oddity), dword `B` = count. The data is `2*B` or `4*B` bytes.
  **observed**
- Vertex attribute array: `0x127d90` reads N entries of `0x24` bytes. Each entry is three dwords, a 12-byte
  reference (`0x162120`), and a dword. Stored in the object at `+0xb8`-area (`plVar21[0x17]`). Field meanings
  are **unknown**. The attribute formats needed for the Vulkan vertex input are not named in these bytes.
- Vertex stride: the object returned by `0x110ef0` (`0x2c8` bytes) has a dword at its start multiplied with
  the index count for the upload size (`0x8be60`, count `uVar13 * iVar2`). **inferred**. The stride is not named.
- Zero-copy: `0x8e0d0` returns a pointer into the file buffer when alignment allows (**observed**). The copy to
  GPU memory is `0x8be60` in the build job.

## 6. Can the full pipeline key be computed before the first draw?

Per Vulkan graphics pipeline key part (what the port needs), where it comes from:

| Key part | Source | Known at load? | Evidence |
|---|---|---|---|
| Vertex shader and pixel shader code | pass object chosen at draw (`ctx+0xc0b0`, `+0xc0a8`) | No: the pass-to-object map is not found | section 4 |
| Shader permutation (fetch formats, bindings, runtime info) | port side (`StageSpecialization`), from draw state | No | report 08 |
| Texture T# content (format, type, depth, sRGB, pitch) | resource T# words, raw in file | Yes, after the texture resource is loaded | section 3.1, 3.2 |
| Texture slot count and which views are used | material `MatSlot` array, `+0x450` mask | Yes, after material parse | section 3 |
| Image dimension and array state per slot | `viewMask` and T# type word | Yes | section 3.1 |
| Vertex attribute formats and offsets | mesh attribute array (`0x127d90`, `0x24` entries) | Yes, after mesh build (formats unnamed) | section 5.2 |
| Index type | mesh index descriptor `A` | Yes | section 5.2 |
| Primitive type | not located | Unknown | section 5.1 |
| Blend, depth, stencil, raster state | render-state object at `+0x560` (table `DAT_02ee3ce0`) | Yes if the table is read; contents not decoded | section 3.3 |
| Render-target formats and count, sample count | pass or frame setup, not in these objects | No: draw-time | `ctx+0xc5d8`, `ctx+0xc5e0` are set outside |
| Write mask / export mask | `ctx+0xc5d8`, `ctx+0xc5e0` | No: set by the caller | section 4 |

Conclusion (**inferred**): a full key is possible per material pass only if (a) the pass-to-object map is
decoded, (b) the render-target state is taken from the pass that draws the material, and (c) the vertex
attribute formats are named. Texture and vertex-format parts can be computed at load. The state object
contents and the primitive type are still open.

## 7. Run-time use in the port (proposal, not implemented)

- **Hook point (inferred):** after the material parser returns (`0xf6c90`, return at label `0xf80bc`), or
  in the factory build `0xf9390` with flag 1. At that point the object is complete and the references are
  loaded. A second point is `0xf9530` (the `"STAM"` path, flag 0). The mesh parser `0x1264d0` finishes after
  the upload.
- **What to read:** `obj+0x450` (populated mask), the slot array at `obj+0x60`, `obj+0x458` (kind), `obj+0x460`,
  `obj+0x468` (render flags), the lists at `+0x490` and `+0x4d0` (the shader references), and the two maps.
- **Port key candidates:** per slot `(T# words, viewMask)`; per mesh `(index type, attribute array)`. The
  pass-keyed entries of the maps stay unused until the pass table is decoded.
- **Timing:** materials and meshes load with the area (`AreaLoad_ShaderListAndAssets`, `0x2569d0`), so a
  pre-warm can run after the area's loads finish. Default material and default textures are loaded at
  startup (section 2).

## 8. Open items (for the next RE step)

1. Find the pass-to-object map: decode the map value (`+0x28`) and check whether the map key is FNV-1a 32 of a
   pass name (test with names from `user/shader_predict/pass_layouts.txt`, e.g. `depth`, `shadowfadeout`).
2. Read the render-state table `DAT_02ee3ce0` (offsets `+0x28..+0x70`) and the state object fields.
3. Decode the attribute array (`0x24` entries) and the `0x28`/`0x30` records; name the attribute formats.
4. Find the primitive type (`0x50` section table, or the object's virtual `+0x28`).
5. Identify the mesh's bit-0 reference target (a material or a shader list).
6. Find the caller of `0x862c50` (vtable slot) to name the render-item type.
7. Confirm the view-table object (`payload+0x60` vs CTexture `+0xc4`) on a loaded texture.
8. Read a real `.cmat` and `.cmsh` from the user's dump to confirm the field offsets and the accepted `viewMask` set.
