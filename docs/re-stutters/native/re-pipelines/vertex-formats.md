# Vertex formats of meshes (.cmsh): what the game builds, and where the port can read it

Read-only static analysis (agent D1 of the pipeline-list RE). Sources: Ghidra pseudocode in
`out/re/decomp/<hi16>/<addr>.c` (the first line of each file lists the callers), disassembly of
`out/re/ghidra/eboot_ghidra.elf` (llvm-objdump, VA = eboot offset), and the constant tables read from the same
ELF by a small script (`scratchpad/d1/va.py`, not in the repo). Port sources: `gpu/shadps4/video_core/amdgpu/resource.h`,
`gpu/shadps4/shader_recompiler/frontend/fetch_shader.h`, `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp`.

Labels: **observed** (decomp, disassembly or table, address given), **inferred** (reasoning, not run), **unknown**.
No game file was read, so every value below is what the code does, not what a particular mesh contains.

Correction to earlier notes: `materials-meshes.md` 5.1 and 5.2 say the attribute entry is "three dwords, a 12-byte
reference, and a dword" and that the formats are unnamed. The disassembly shows the entry is nine plain dwords
(section 3), and the formats are named by a table of format classes (section 4). `key-fields.md` 6.1 says the vertex
formats are "not named in RE"; section 4 names them.

## 1. Short answer

- **A mesh's vertex layout is fixed at load.** The mesh loader `0x1264d0` builds, for every named vertex stream, a
  stream object with a V# (GCN buffer resource, 16 bytes), and copies those V#s into one 12-slot V# table
  (192 bytes) at the end of the mesh-stream object (`+0x2c0`). **observed** (sections 2, 5)
- **The format of each stream comes from the .cmsh stream record.** Each record carries a 16-bit format index
  (record `+0x04`). Two small engine tables (17 entries each) turn the index into a format class (0..25) and a
  per-element size. The class table (27 entries) gives the V# word3 (data format, number format, destination select).
  **observed** (section 4)
- **Port-readable, no run-time decode needed:** after `0x1264d0` returns, the mesh object is `*param_2`, its
  stream object is `obj+0x70`, and the V# table is `*(obj+0x70)+0x2c0`. Each V# word already holds the final stride,
  record count, base and format. **observed for the code path; inferred for the port hook** (section 9)
- **Set of formats:** 17 format indices, 26 classes, 24 distinct V# word3 values, 23 distinct (data, number)
  pairs. The set is small by construction. Which of them the game's .cmsh files use was **not** checked (no dump read).
- **The 0x24-byte attribute array** (`obj+0xb8`, parser `0x127d90`) is a second table. Its nine dwords per entry are
  stored verbatim, but **no consumer was found**, so its meaning is **unknown** (section 6).
- **What the port key sees:** `vertex_buffer_formats[]` and `vs_attribs` take the data format, number format and
  destination select from the V# of each fetch-shader attribute. These stay in the key under the user's
  `SOTC_GENERIC_BUFFERS=2`. The stride does not (section 8).

## 2. Mesh stream object (the part of the mesh that holds the vertex V#s)

Built by `0x110ef0` (3,495 bytes, called from `0x1264d0` at `0x1264d0`'s bit-1 branch) into a 0x2c8-byte object, which
the mesh object holds at `mesh+0x70` (`plVar21[0xe]`). **observed** (`0012/001264d0.c` lines 452-459, `0011/00110ef0.c`).

Mesh object (`0x1264d0`, `*param_2 = plVar21` at its end, line 703; class `0x130` from `0x125bd0` or `0x178` from `0x128d80`):

| Mesh offset | Content | Evidence |
|---|---|---|
| `+0x70` | stream object `0x2c8` bytes (below) | `plVar21[0xe]`, `0x1264d0` line 459 |
| `+0x80`, `+0xa0` | section table (0x50 records) | `plVar21[0x10]`, `plVar21[0x14]`, lines 237, 463-470 |
| `+0xb8` | attribute block `0x40` bytes, holds the 0x24-byte array (section 6) | `plVar21[0x17]`, line 489 |
| `+0xe0` | index stream (bit 2 path, `0x8be60` at line 559) | `plVar21[0x1c..0x1e]` |

Stream object `0x2c8` (dword offsets in brackets). **observed** (`0x110ef0` disassembly and decomp):

| Offset | Content |
|---|---|
| `+0x000` | u32 vertex count = (POS byte size) / (POS element size), written at `0x110ef0` line 1 via `0x155e30` |
| `+0x008` | index stream object (0x28 bytes) |
| `+0x030` | POS0 stream object |
| `+0x058` | COL0 stream object |
| `+0x080` | COL1 stream object |
| `+0x0a8` | TEX0 stream object |
| `+0x0d0` | TEX1 stream object |
| `+0x0f8` | TEX2 stream object |
| `+0x120` | COL2 stream object |
| `+0x148` | NRM0 stream object |
| `+0x170` | TAN0 stream object |
| `+0x198` | copy of the POS0 stream object header, flags OR `0x400` (section 5.3) |
| `+0x1c0` | TEX3 stream object |
| `+0x1e8` | BONI (blend indices) stream object |
| `+0x210` | BONW (blend weights) stream object |
| `+0x238` | SAT_ (summed-area table) stream object |
| `+0x260 .. +0x2d8` | 12 pointers to the stream objects above (`0x110ef0` lines 141-148) |
| `+0x2c0` | pointer to the **V# table** (0xc0 bytes), set by `0x128a80` (`0x110ef0` line 150) |

Each stream object is 0x28 bytes, and the V# is its first 16 bytes:

```
struct StreamObj {          // 0x28 bytes, observed (0x8be60 writes, 0x8652d0 writes the V#)
  u32 v_word[4];            // +0x00 V# (GCN buffer resource, see section 5)
  void* data;               // +0x10 data pointer (copy of the stream bytes, allocated by 0x12820)
  u32 flags;                // +0x18 0x8be60 param_5 (0 for file streams; 0x400 is ORed in the copies)
  u32 format_class;         // +0x1c format class (section 4), 0..25
  u32 byte_size;            // +0x20 = count * element size
  u8  pad[4];               // +0x24
};
```

Stream slot to V# table index (section 5.2) is fixed by the code, not by the file.

## 3. Stream records (the .cmsh named-stream section) and the FourCC names

The named-stream section is read by `0x155f40` (207 bytes, called from `0x1264d0`): a u32 byte length, then the
payload. The payload starts with a u32 record count N and a u32 (unused here), then N records of 0x18 bytes
(`0x155f40` lines 1-30, **observed**). The fix-up loop adds the section base to the pointer at record `+0x08`.

```
struct StreamRecord {       // 0x18 bytes, base = record start (observed: 0x155f00 and 0x110ef0 offsets)
  u32 fourcc;               // +0x00 name, read as big-endian text, see table below
  u16 format_index;         // +0x04 index into the two tables of section 4
  u16 reserved;             // +0x06 not read
  u8* data;                 // +0x08 pointer to the stream bytes (fixed up by 0x155f40)
  u32 byte_size;            // +0x10 byte length of the stream; count = byte_size / element size
  u32 reserved2;            // +0x14
};
```

`0x155f00` (55 bytes) searches the N records by fourcc (dword compare at `+0x00`). **observed**

| fourcc (hex) | text | `0x110ef0` call site | Stream object |
|---|---|---|---|
| `0x504f5330` | POS0 | line 129 (`kPos0`) | `+0x030` |
| `0x434f4c30` | COL0 | `kCol0` | `+0x058` |
| `0x434f4c31` | COL1 | `kCol1` | `+0x080` |
| `0x434f4c32` | COL2 | `kCol2` | `+0x120` |
| `0x54455830` | TEX0 | `kTex0` | `+0x0a8` |
| `0x54455831` | TEX1 | `kTex1` | `+0x0d0` |
| `0x54455832` | TEX2 | `kTex2` | `+0x0f8` |
| `0x54455833` | TEX3 | `kTex3` | `+0x1c0` |
| `0x4e524d30` | NRM0 | `kNrm0` | `+0x148` |
| `0x54414e30` | TAN0 | `kTan0` | `+0x170` |
| `0x424f4e49` | BONI | `kBlendIndices` | `+0x1e8` |
| `0x424f4e57` | BONW | `kBlendWeight` | `+0x210` |
| `0x5341545f` | SAT_ | `kSummedAreaTable` | `+0x238` |

The index stream is not named; it comes from the index descriptor (section 7). If a fourcc is missing, the
code fills the slot with a default V# from the global table at `DAT_02ee3ce0+0x3b0..0x400`, and the flags get `0x400`
(`0x110ef0` lines 'lVar == 0' branches; **observed**). Thirteen names are looked up in total.

## 4. Format index, format class, element size, V# word3

Three engine tables and one function. All read from the eboot (VA = offset).

**4.1 Format index to class and element size** (record `+0x04`, u16):

- `DAT_00fa62b0` (`u32[17]`): index to format class. Used as `uVar35 = DAT_00fa62b0[index]` when `index < 0x11`,
  else class 0 (`0x110ef0`, e.g. line 'kPos0'). **observed**
- `DAT_00faa9f0` (`u8[17]`): index to element size in bytes. Read by `0x155e30` (`return (&DAT_00faa9f0)[index]`),
  used as the divisor of `byte_size` to get the vertex count. **observed**

| index | class (fa62b0) | class element size (f9ec50) | stream element size (faa9f0) | class meaning (section 4.2) |
|---|---|---|---|---|
| 0 | 0 | 4 | 4 | 32 float x1 |
| 1 | 1 | 8 | 8 | 32_32 float |
| 2 | 2 | 12 | 12 | 32_32_32 float |
| 3 | 3 | 16 | 16 | 32_32_32_32 float |
| 4 | 20 | 4 | 4 | 8_8_8_8 UNORM |
| 5 | 19 | 4 | 4 | 8_8_8_8 UINT |
| 6 | 12 | 4 | 4 | 16_16 float |
| 7 | 13 | 8 | 8 | 16_16_16_16 float |
| 8 | 0 | 4 | 4 | 32 float x1 (see note) |
| 9 | 0 | **4** | **8** | **mismatch**: class says 4 bytes, stream table says 8 |
| 10 | 0 | 4 | 4 | 32 float x1 (see note) |
| 11 | 16 | 8 | 8 | 16_16_16_16 UINT |
| 12 | 0 | 4 | 4 | 32 float x1 (see note) |
| 13 | 0 | 4 | 4 | 32 float x1 (see note) |
| 14 | 0 | 4 | 4 | 32 float x1 (see note) |
| 15 | 18 | 4 | 4 | 2_10_10_10 UNORM |
| 16 | 18 | 4 | 4 | 2_10_10_10 UNORM |

Notes: indices 8, 10, 12, 13, 14 map to class 0 with a 4-byte element, and the stream table agrees on the size. Their
real meaning is therefore unclear (possibly unused, or "class 0" used as a default). Index 9 is an actual
contradiction between the two tables (`0x110ef0` uses the class size for the GPU stride and the stream table for the
count, so a stream with index 9 would be uploaded with a stride of 4 and a count computed with 8). **observed
(tables)**; whether any mesh uses index 9 is **unknown**.

**4.2 Format class to element size and V# word3.** `DAT_00f9ec50` (`u32[27]`, element size, read by `0x8c1d0`:
`return param < 0x1b ? DAT_00f9ec50[param] : 0`) and `DAT_00f9ebe0` (`u32[26]`, format word, read in `0x8be60`
line `uVar6 = DAT_00f9ebe0[class]` for `class < 0x1a`). **observed**

The format word packs `dst_sel` in bits 12-23, `num_format` in bits 8-10 and `data_format` in bits 0-3
(decoded from the writer `0x8652d0`, section 5.1). Decoded:

| class | element | data format | number format | dst_sel (X,Y,Z,W) | format word |
|---|---|---|---|---|---|
| 0 | 4 | 32 | FLOAT | R, 0, 0, 1 | 0x204704 |
| 1 | 8 | 32_32 | FLOAT | R, G, 0, 1 | 0x22c70b |
| 2 | 12 | 32_32_32 | FLOAT | R, G, B, 1 | 0x3ac70d |
| 3 | 16 | 32_32_32_32 | FLOAT | R, G, B, A | 0xfac70e |
| 4 | 4 | 32 | UINT | R, 0, 0, 0 | 0x004404 |
| 5 | 8 | 32_32 | UINT | R, G, 0, 0 | 0x02c40b |
| 6 | 12 | 32_32_32 | UINT | R, G, B, 0 | 0x1ac40d |
| 7 | 16 | 32_32_32_32 | UINT | R, G, B, A | 0xfac40e |
| 8 | 4 | 32 | SINT | R, 0, 0, 0 | 0x004504 |
| 9 | 8 | 32_32 | SINT | R, G, 0, 0 | 0x02c50b |
| 10 | 12 | 32_32_32 | SINT | R, G, B, 0 | 0x1ac50d |
| 11 | 16 | 32_32_32_32 | SINT | R, G, B, A | 0xfac50e |
| 12 | 4 | 16_16 | FLOAT | R, G, 0, 1 | 0x22c705 |
| 13 | 8 | 16_16_16_16 | FLOAT | R, G, B, A | 0xfac70c |
| 14 | 2 | 16 | UINT | R, 0, 0, 0 | 0x004402 |
| 15 | 4 | 16_16 | UINT | R, G, 0, 0 | 0x02c405 |
| 16 | 8 | 16_16_16_16 | UINT | R, G, B, A | 0xfac40c |
| 17 | 1 | 8 | UINT | R, 0, 0, 0 | 0x004401 |
| 18 | 4 | 2_10_10_10 | UNORM | R, G, B, A | 0xfac009 |
| 19 | 4 | 8_8_8_8 | UINT | R, G, B, A | 0xfac40a |
| 20 | 4 | 8_8_8_8 | UNORM | R, G, B, A | 0xfac00a |
| 21 | 4 | 8_8_8_8 | SNORM | R, G, B, A | 0xfac90a |
| 22 | 4 | 8_8_8_8 | SINT | R, G, B, A | 0xfac50a |
| 23 | 4 | 8_8_8_8 | SNORM | R, G, B, A | 0xfac10a |
| 24 | 2 | 16 | UINT | R, 0, 0, 0 | 0x004402 (index 16-bit) |
| 25 | 4 | 32 | UINT | R, 0, 0, 0 | 0x004404 (index 32-bit) |

The dst_sel values are read from the packed word, not checked against a V# in a dump. Classes 24 and 25 are the
index classes (section 7). Classes 26..28 (`0x1a`, `0x1b`, `0x1c`) take special writers (`0x865600`, `0x865500`,
`0x8655d0`) and cannot be reached from the .cmsh format index (max class 20). `0x8655d0` writes stride 16 and
`num_records` in 16-byte units, so it is a constant-buffer writer (**inferred** from the 0x100000 stride bit).

**Distinct values:** 24 word3 values (classes 4 and 25 are equal, 14 and 24 are equal), 23 distinct
(data, number) pairs.

## 5. The V# (buffer resource) writer, and the V# table

### 5.1 Writer `0x8652d0` (143 bytes), called from `0x8be60` and `0x8c280`

`FUN_008652d0(buf, base, fmt_word, stride, num_records)`, **observed** (`0086/008652d0.c`). It writes four dwords:

```
buf[0] = (u32)base;                                  // word0 base low
buf[1] = (stride & 0x3fff) << 16 | (base>>32 & 0xfff);   // word1: stride in bits 16..29
buf[2] = num_records;                                // word2
buf[3] = (fmt_word>>12 & 0xfff)                      // word3 bits 0..11  = dst_sel
       | (fmt_word & 0x700) << 4                     // word3 bits 12..14 = num_format
       | (data_format placed at bit 15 when valid)   // word3 bits 15..18 (from fmt_word & 0xf)
       | 0x20000000;                                 // word3 bit 29
```

Against the port's `AmdGpu::Buffer` (`gpu/shadps4/video_core/amdgpu/resource.h`): word1 holds `stride : 14` at bit 16,
word2 holds `num_records`, word3 holds `dst_sel_*` (0..11), `num_format` (12..14), `data_format` (15..18). The
0x20000000 bit is `mtype` bit 2 in the port's struct (inferred; the port's `Buffer::Null()` does not set it).

The value of `stride` is the class element size. `num_records` is the element count (the `count` argument of `0x8be60`),
not the byte size. For vertex streams: stride 4..16, records = vertices. For indices: stride 2 or 4.

### 5.2 The V# table: 12 slots

`0x110ef0` lines 'puVar12' (the block allocated by `0x17140(0xc0)` and stored at `obj+0x2c0` by `0x128a80`) copies
the V# of each stream into 16-byte slots (**observed**):

| slot | V# source (stream object offset) | stream |
|---|---|---|
| 0 | `+0x08` | index |
| 1 | `+0x30` | POS0 |
| 2 | `+0x58` | COL0 |
| 3 | `+0x80` | COL1 |
| 4 | `+0xa8` | TEX0 |
| 5 | `+0xd0` | TEX1 |
| 6 | `+0xf8` | TEX2 |
| 7 | `+0x120` | COL2 |
| 8 | `+0x148` | NRM0 |
| 9 | `+0x170` | TAN0 |
| 10 | `+0x198` | POS0 copy, flags OR `0x400` (section 5.3) |
| 11 | `+0x1c0` | TEX3 |

Slots 2..11 are the streams that the fetch shader of a skinned or textured vertex shader can read through a V#
table pointer. The mapping from fetch attribute to slot is set by the shader's fetch code, not by the mesh
(section 8).

### 5.3 The position copy (slot 10)

`0x110ef0` copies the POS0 stream object header (V#, data pointer, flags, class, size; 0x28 bytes) to `+0x198`,
and ORs `0x400` into the flags word (`0x110ef0` lines 'param_1[0x66]' to '[0x6e]'). **observed**. The meaning of the
second POS is **unknown**. The `0x400` also marks the defaults of missing streams (section 3), so it is likely a
"not from file" marker. Inferred.

## 6. The attribute block (0x24-byte entries, `obj+0xb8`) and its companion arrays

Parser `0x127d90` (538 bytes, called from `0x1264d0` line 445, bit 1 of the mesh flags). **observed**:

- `count` from the stream, then `count` entries of `0x24` bytes. Each entry is nine dwords read in file order by
  `0x8eae0` (three direct reads) and `0x162120` (which calls `0x158a50` twice, each reading three dwords). So:

```
struct AttribEntry {        // 0x24 bytes, stored verbatim (observed, 0x127d90 copy loop)
  u32 d0, d1, d2;           // +0x00, +0x04, +0x08   three direct reads
  u32 d3, d4, d5;           // +0x0c, +0x10, +0x14   first triple (0x162120 -> 0x158a50)
  u32 d6, d7, d8;           // +0x18, +0x1c, +0x20   second triple (0x162120 -> 0x158a50)
};
```

This corrects the brief's "three dwords, a 12-byte reference, a dword". `0x162120` reads two triples, so the entry has
nine dwords, not three plus a 12-byte reference plus one dword.

- The entries are kept in a vector at `obj+0xb8` (vector begin `+0x08`, end `+0x10`, cap `+0x18`).
- After the entries, `0x127fc0` (540 bytes) reads a second array into `obj+0xc8`: a count, then 3-dword records
  stored as three qwords each (0x18-byte stride). **observed**.

**Meaning: unknown.** The fields are not named by the parser, and no reader of `obj+0xb8` was found. Candidates
(inferred, unverified): a per-attribute record (element id, stream index, offset, ...). The 9 dwords could be the
engine's own attribute declaration, as the reflection type `VertexFormatParameters` suggests (section 10).

Also in the same branch, the 0x28-byte element array and the 0x30-byte second array are built on a temporary
object `pauVar22` (`0x1264d0` lines 238-300); its later owner was not traced.

## 7. Index descriptor (the index stream)

`0x154e00` reads the index descriptor. In the mesh loader it becomes `local_140` (`0x1264d0` line 448). **observed**:

- `local_140.type` (u32): 1 = 16-bit indices, 0 = 32-bit, other = no class (count still read).
- `local_140.count` (u32, the high dword of the first qword): the count is rounded up to an even number
  (`0x110ef0` line 'iVar14' area: `(count + 1) & ~1`).
- `local_140.data` (pointer, `param_3[1]`).

The index stream then uses class `(type != 1) | 0x18` (24 = 16-bit UINT, 25 = 32-bit UINT) with element size
`2` for type 1, `4` for type 0 (and `0` otherwise). The V# is slot 0. **observed**

## 8. Draw path (from the draw loop to the V# reads)

Chain (**observed** call graph, decomp files in `out/re/decomp`):

1. `0x11d7a0` (1,066 bytes), draw loop; per render item calls `0x19af20` (`Item draw dispatch`, 484 bytes).
2. `0x19af20` reads a per-section record of `0xa8` bytes at `param_2+0x38 + section*0xa8` (`section = item+0x148`),
   chooses one of four 8-byte slots by kind (`+0x00`, `+0x08`, `+0x10`, `+0x18`), and calls either `0x1a6d20`
   (1,377 bytes) or `0x1a7290` (1,454 bytes).
3. `0x1a6d20` walks the 64-bit section mask. For each section range it:
   - binds the material with `0xf8990` (`FUN_000f8990`, the material/state bind; see `pass-map.md`, section 3);
   - calls the setters `0x86bf50(ctx, base)`, `0x86bfb0(ctx, size)` and `0x86bed0(ctx, is32, 2)`. The third one takes
     `is32 = (stride field == 4)`, i.e. 32-bit indices (**observed** in the decomp; the register each setter writes
     is **inferred**, most likely index base, index count and index type);
   - reads a 0x20-byte range record at `lVar8 + (range-1)*0x20` with the fields `+0x00` (8-byte base), `+0x08`,
     `+0x0c` (stride field in bits 16..29), `+0x10`, `+0x18` (ushort), `+0x1a` (byte). These are the index ranges of
     the section, not the vertex streams (**observed** field offsets, meaning **inferred**).
4. `0x1a7290` is the same pattern (plus `0x85fc10`, `0x869bb0`, `0x86b3d0`).

**Not found:** the write of the vertex V# table pointer (`obj+0x2c0`) into a user SGPR, and the write of the
per-stream V#s into the flat user-data area. Neither draw function calls the `0x862510`/`0x86d550` user-data writers
that `pass-map.md` lists. The V# table pointer is therefore the open link (**unknown**). The fetch shader of the
port reads the V# of each attribute from `ReadUdReg(sgpr_base, dword_offset)` (`fetch_shader.h:37-45`), so the
engine must put a pointer to a V# table in a user SGPR. The table is most likely the 0xc0 block (**inferred**).

## 9. Is the format fixed at load, and can the port read it at load?

- **Fixed at load (inferred from the code path):** the V# words are written only in `0x110ef0` (and `0x8be60`).
  Nothing in the draw path rewrites a stream V# (`0x1a6d20`, `0x1a7290`, `0x19af20` do not write `+0x30..+0x1f0`).
  The post-load writers were not searched; this is an inference, not a proof.
- **Port hook:** after `call 0x1264d0` returns, `*param_2` is the mesh object. `mesh+0x70` is the stream object,
  `*(mesh+0x70)+0x2c0` is the V# table, and `table+16*k` is slot k (section 5.2). Reading the table gives, per slot:
  base (word0, word1 low 12 bits), stride (word1 bits 16..29), `num_records` (word2), and the format fields
  (word3). The class of each stream is at stream object `+0x1c`, and the byte size at `+0x20`.
- **Timing:** the mesh loader runs in the area load (`AreaLoad_ShaderListAndAssets`, `0x2569d0`, per
  `materials-meshes.md`). So a read at the return of `0x1264d0` is available before the first draw of that mesh.
- **What the port can know per mesh before any draw:** the stream set (which of the 13 fourccs exist), each
  stream's format class and element size, the vertex count, the index type and count. This is the whole
  per-mesh part of the vertex input.

## 10. The reflection type `VertexFormatParameters` (0x1315629)

String `VertexFormatParameters` at `0x1315629` (**observed**). Its reflection registration is `0xe55720` (1,132 bytes,
called from `0xe556d0`, `0xe55b90`): it defines a struct `STRUCT` with fields `vertexCount` (type 13), `vertexFormat`
(type 6) and `buffers` (a list). **observed**. This is a reflection (data serialization) descriptor. The mesh loader
does not call it. Inference: `vertexFormat` is the 16-bit format index of section 4, and `buffers` is the stream list.
It is not needed for the port.

## 11. Port side: what in the key depends on the format

Port code (`arch/gcn-native`):

- `vertex_buffer_formats[i]` (`vk_pipeline_cache.cpp:3086-3098`): for each fetch attribute `i` (in fetch-shader
  order), `LiverpoolToVK::SurfaceFormat(V#.GetDataFmt(), V#.GetNumberFmt())`. Filled only when the vertex input
  is not dynamic (`!instance.IsVertexInputDynamicState()`). **Not affected by `SOTC_GENERIC_BUFFERS`**: the
  `GenericBufferFormat` switch (`info.h:147`) only changes the buffer spec, not this array. So with
  `SOTC_GENERIC_BUFFERS=2` the vertex format still splits the graphics key.
- `vs_attribs` (`specialization.h:79-91`, `FillVsAttrib`): `divisor` (only for instance-rate attributes, from
  `step_rate_0/1`), `num_class` (from `V#.num_format`) and `dst_select` (from `V#.dst_sel`). These depend on the
  format class, so they are fixed per stream class.
- Stride: when the vertex input is not dynamic, the stride is a dynamic state (`VertexInputBindingStride`,
  `vk_graphics_pipeline.cpp:213-214`), and `SOTC_GENERIC_BUFFERS=1` removes the stride from the buffer spec
  (`specialization.h:96`). So the stride is not in the key under the user's setting.
- Base address and `num_records`: not in the key.

**Under the user's `sotc.ini` (`SOTC_GENERIC_BUFFERS=2`, `SOTC_VS_BINDING_BASE=1`):**

| Decoded field | In the key? | Notes |
|---|---|---|
| data format + number format of each attribute (`vertex_buffer_formats`, `num_class`) | yes | 23 possible (data, number) pairs (section 4.2); predictable per stream from the .cmsh format index |
| destination select (`dst_select`) | yes (`vs_attribs`) | 7 forms (section 4.2) |
| element stride | no (dynamic) | equal to the class element size; the port can ignore it for keys |
| base address, `num_records` | no | per draw |
| instance step rate | yes, only for instance-rate attributes | DRAW (section 8 of `key-fields.md`) |
| index type | no (dynamic `IndexType`, per draw) | from section 7; known at load |
| which stream feeds which attribute | yes (fetch shader) | fetch shader is per VS program; slot mapping is in section 5.2 |

So the vertex part that is worth predicting is the mapping (fetch attribute to V# slot, slot to class), and the
key differences it causes are the (data, number, dst_select) triples. The strides and base addresses do not change the key.

## 12. Run-time confirmation (exact log to add)

The static findings can be checked in one run with a log at two points. Addresses are eboot VAs; the port maps the
eboot image at its own base (see the boot log), so add that base to each address.

1. **Mesh load, end of `0x1264d0`.** Hook the return of `0x1264d0` (or its caller). At return, `param_2` (`rsi` at
   entry, saved in a register) points to the output slot; read `m = *(void**)param_2`. Then log:
   - `s = *(void**)(m+0x70)`, `table = *(void**)(s+0x2c0)`;
   - for k in 0..11: `table[16k..16k+15]` (the V# words), and `*(u32*)(s + off_k + 0x1c)` (class), `*(u32*)(s + off_k + 0x20)` (byte size) with `off_k` from section 5.2;
   - `m+0xb8` vector: begin `*(void**)(m+0xb8+8)`, end `*(void**)(m+0xb8+0x10)`, and dump `(end-begin)/0x24` entries of nine dwords.
2. **Named records, return of `0x155f40`.** Log the record count `*(u32*)(buf+0)` and, per record, `fourcc` (+0x00),
   `format_index` (u16 at +0x04), `byte_size` (+0x10). This gives the per-mesh format indices, which can be
   checked against section 4.
3. **Draw side (to close section 8).** Hook the write of `obj+0x2c0` (table pointer) into the user-data area, or
   log the user SGPRs at the first draw of a mesh, by setting a watch on the table address from step 1. Comparing
   the first draw's SGPR values with `table` shows which SGPR carries the V# table.
4. **Attribute array consumer.** Watch reads of `m+0xb8` during a draw (page watch or a hook on `0x127d90`'s
   callers), to name the nine dwords.

## 13. Open items and confidence

- **Proven (observed):** stream object layout (section 2), V# table slots (5.2), V# writer (5.1), class and index
  tables (4.1, 4.2), named stream list and FourCC mapping (3), index descriptor (7), attribute entry is nine dwords (6).
- **Inferred:** the V# table is the one a user SGPR points to (8, 9); the 0x400 flag meaning (5.3, 3); the
  `mtype` bit (5.1); the `vertexFormat` identity (10); fixed-at-load (9).
- **Unknown:** the meaning of the nine attribute dwords (6); the consumer of `obj+0xb8` and `obj+0xc8`; the owner of the
  0x28/0x30 arrays built on `pauVar22`; whether index 9 is used (4.1); which formats the game's meshes actually use
  (no dump read); the draw-time user-SGPR write of the V# table (8).
- **Not checked:** post-load writers of the stream objects; the `0x8c280` caller of the V# writers; any mesh data.

## 14. Evidence files

- `out/re/decomp/0012/001264d0.c` (mesh loader, lines 224-231, 237-260, 445-489, 448-459, 522-559, 703)
- `out/re/decomp/0012/00127d90.c` (attribute entry parser), `0012/00127fc0.c` (second array)
- `out/re/decomp/0016/00162120.c` (two triple reads), `0015/00158a50.c` (triple reader), `0008/0008eae0.c` (dword reader)
- `out/re/decomp/0011/00110ef0.c` (stream object builder, 0x2c8), `0011/00110ef0` tail (V# table copy, lines 141-150)
- `out/re/decomp/0015/00155f00.c`, `0015/00155f40.c`, `0015/00155e30.c` (named stream lookup and element size)
- `out/re/decomp/0008/0008c1d0.c` (class element size), `0008/0008be60.c` (buffer create), `0086/008652d0.c` (V# writer),
  `0086/00865360.c`, `0086/00865500.c`, `0086/00865600.c`, `0086/008655d0.c` (other V# writers)
- `out/re/decomp/0011/0011d7a0.c` (draw loop), `0019/0019af20.c` (item dispatch), `001a/001a6d20.c`, `001a/001a7290.c` (section draws)
- `out/re/decomp/0012/00128a80.c` (stores the V# table pointer at `+0x2c0`)
- `out/re/decomp/00e5/00e55720.c` (reflection `VertexFormatParameters`)
- Tables (read from `out/re/ghidra/eboot_ghidra.elf` by `scratchpad/d1/va.py`): `DAT_00fa62b0` (0xfa62b0), `DAT_00faa9f0` (0xfaa9f0),
  `DAT_00f9ec50` (0xf9ec50), `DAT_00f9ebe0` (0xf9ebe0), string `VertexFormatParameters` (0x1315629).
- Port: `gpu/shadps4/video_core/amdgpu/resource.h` (V# bit layout), `gpu/shadps4/shader_recompiler/frontend/fetch_shader.h`
  (GetSharp), `gpu/shadps4/shader_recompiler/specialization.h:79-115` (FillVsAttrib, FillBuffer),
  `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:3086-3098` (vertex_buffer_formats),
  `gpu/shadps4/shader_recompiler/info.h:102-150` (GenericBufferStride/Format), `sotc.ini:45,53`.

## V# table binding (T4)

Read-only static analysis (agent T4, follows D1 section 8 and section 12). Same sources as D1: Ghidra pseudocode in
`out/re/decomp/<hi16>/<addr>.c`, the full eboot disassembly (`llvm-objdump -d out/re/ghidra/eboot_ghidra.elf`, saved to
`scratchpad/t4_disasm.txt`, not in the repo), and the port sources named in D1. Addresses are eboot VAs. Labels as in D1:
**observed**, **inferred**, **unknown**. No game file read, no game run.

### T4.1 Short answer

- **No setter writes the 0xc0-byte table into a user SGPR.** The 0xc0-byte table is never copied. What is copied per
  draw is an 8-byte pointer to it, into a per-item pointer list. That list is then compacted into a per-draw "SRT"
  chunk, and the chunk base goes to the VS user SGPR pair. **observed** (code path), **inferred** (SRT shape).
- The write chain is: mesh stream `+0x2c0` -> item `+0x90` (`0x1122d0`, `0xdb390`, `0x10b0a0`) -> chunk
  (`0x95320`) -> `0xae150` -> `0x862490` -> `0x86d5e0` (PM4 `SET_SH_REG`). **observed**.
- **Remap:** none in the engine. The mesh table is passed whole. Which V# a fetch attribute reads is decided by the
  guest fetch shader's `dword_offset` (slot = `dword_offset / 4`). **inferred** (T4.7).
- **Two corrections to earlier notes:** `0x1a6d20` / `0x1a7290` (the D1 draw path) do not bind the mesh table; they
  bind a pointer into the 0xa8-byte section buffer. And D2's "V# via `0x861fe0` in slot 7" is a T# (32-byte) setter,
  not a V# (T4.8).

### T4.2 The table pointer store (`0x128a80`)

- `0x128a80(obj, table)` (46 bytes, **observed**): frees the old `*(obj+0x60)` through `0x90300`, then stores `table`
  at `obj+0x60`. The caller passes `stream + 0x260`, so the table pointer lands at **stream `+0x2c0`**. This matches D1
  section 5.2. The 12-pointer array at stream `+0x260..+0x2d8` is written just before the call (`0x110ef0` lines
  141-148, `0x110c90` lines 2-14).
- Callers of `0x128a80` (decomp header lists, **observed**): `0x110ef0` (line 417, the file mesh builder, used by
  `0x1264d0`); `0x110c90` (fixed-offset builder, no static caller, so probably called through a pointer);
  `0x111ca0` (from `0x125bd0`, mesh class `0x130`); `0x111f30` (from `0x368c00`); `0x110e10` (from `0x368a70`);
  `0x192f20` (from `0x10f760`; it also writes `obj+0x78`).

### T4.3 Readers that copy the table pointer into a draw item (the missing link of D1 section 8)

The draw loop `0x11d7a0` never reads stream `+0x2c0`. The reader is one level higher, in the item builders. Each one
copies the pointer into the item's qword at **item `+0x90`** (`P[0x12]`):

| Site | Form | Callers / context | Label |
|---|---|---|---|
| `0x1122d0` (15 bytes) | `*(P+0x90) = *(stream+0x2c0)` | `0x10c2e0` (line 48; mesh = `*(object+0x140)` then `+0x70`), `0x10c6c0` (line 48), `0x180ab0` (line 'FUN_001122d0(uVar3, param_3, 1)'), `0x1b32f0` (line 287), `0xadd50` (line 96) | observed |
| `0xdb390` line 83 | inline copy, `*(P+0x90) = *(stream+0x2c0)` | indirect-draw item builder, caller `0xdb700` | observed |
| `0x10b0a0` line 85 | inline copy, `P[0x12] = *(stream+0x2c0)` | caller `0x10aef0` | observed |
| `0x10c2e0` else-branch | `*(P+0x90) = *(0x193560(section, P)+0x60)` | a stage record pointer, not the mesh table | observed; not on the mesh path |

- `0x180ab0` takes its mesh from the global `*(*(DAT_02ee3e10+0xe0)+0x70)` (**observed**), not from an object. This is
  probably a utility or full-screen mesh (**inferred** from use). Its material is `FUN_0014d7b0(obj+0x18)` (**inferred**).
- Callers of `0x180ab0`: `0x1817a0`, `0x181690`, `0x1818e0` (the last one from `0xcaed0`, the pass runner noted in
  D3 section 4.4).
- `0x180ab0` also calls `0x1122c0(stream, P)` = `0x94c60(*P, stream+8)` (**observed**): the index stream object
  (stream `+0x08`) is bound through the same item.

### T4.4 The item and its pointer list (**observed** field uses, meaning **inferred**)

Fields of the item `P`, as the builders and `0xae150` use them:

```c
struct DrawItem {                  // only the fields used by the builders and 0xae150
  u64 cmd;            // +0x00  *P: command buffer / context handle (0x94c60, 0xae150)
  u32 slot_mask;      // +0x18  bits 0..5: which slots below are live (0xae150 reads param_1[3])
  u32 stage_of[6];    // +0x20  stage index per slot bit (0xae150 reads param_1[4] + 4*bit)
  u64 ptr[10];        // +0x90..+0xd8  pointer slots; slot 0 = V# table pointer (mesh stream +0x2c0)
  u64 list;           // +0xa8  (0xdb390 writes the 0x28-byte list here)
  u64 rec[];          // +0x1d8 + 8*stage: stage record of the bound program (0xae150 reads param_1[0x3b + stage])
  u32 pass;           // +0xf4  pass id (1, 4, 6, 8, 0xb seen in the builders)
};
```

- `0xf8c90(material, P)` (**observed**) also writes `P+0xb0` and `P+0xb8` from `material+0x588` and `+0x5c8` (guarded by
  `material+0x5b0` and `+0x5f0`). These are not pointer slots.
- The item is built per pass: `0x1809a0` sets `P+0xf4 = 1`, then `= 6`, and submits twice (**observed**).

### T4.5 Compaction into an SRT chunk (`0x95320`) and the SGPR write (`0xae150`, `0x862490`)

1. `0x95320(mask, &P[0x12], pool)` (252 bytes, **observed**). `mask` is a u32 read from the material: `*(u32*)(material+0x468)`
   at `0x1809a0` (`FUN_00095320(*(undefined4*)(param_3+0x468), param_1+0x12, ...)`) and at `0xdb390`
   (`*(uint*)(lVar21+0x468)`). The material object is the one `0xf8c90` and `0xf8990` read (`+0x458..+0x460`).
   **inferred** that `+0x468` is the material's slot mask. The function allocates `popcount(mask) * 8` bytes with
   `0x12820` (bump allocator) and copies `P[k]` for each set bit `k` (bits 0..9, so `P+0x90..+0xd8`), in bit order.
   The return value is the chunk base (**inferred**: the callers use the return value, the decompile shows `void`).
2. `0xae150(P, chunk)` (441 bytes, **observed**). For each set bit `i` of `P+0x18`: stage `s = u32 at P+0x20 + 4*i`,
   record `R = P[0x3b + s]`. If `R+0x19 == 2`, it calls `0x862490(O, s, &chunk, 2)`. The same chunk base goes to every
   qualifying stage. `O = cursor+0x2e8`, as in D3.
3. `0x862490(O, s, src, n)` (126 bytes, **observed**): `uVar1 = *(u16*)(*(O+0xc180 + 8*s) + 0x10)`. The low byte is the
   user SGPR index. The high byte is passed as the PM4 count. It calls `0x86d5e0(cmd, s, sgpr, src, count)`. For
   `s == 2` (VS) and `O+0xc613 != 0`, it also calls `0x884240` on the mirror layout `O+0xc1b8`. Layout indices in
   this code: 0 = CS, 1 = PS, 2 = VS (D3 section 4.1).
4. `0x86d5e0` (322 bytes, **observed**): PM4 header `0xc0007600 | count << 16` (SET_SH_REG; the count field is dwords
   minus 1), then the SGPR index, then `count + 1` dwords copied from `src`. So an 8-byte pointer uses count 1, and the
   byte at layout `+0x10` (high byte) is 1 for a pointer. **inferred** from the header encoding.

**Which setter, answered:** the VS table pointer is not written by `0x86d4c0` or `0x86d550` on this path. It goes
through `0x862490` (table `+0x10`), with the SRT chunk base as the value. `0x862510` writes only the vertex and instance
SGPR indices (the usage nibbles of the VS record, D2 section 3.2). `0x86d4c0` writes the stage-array flush pointers
(O+0x78, O+0xc130, O+0xc5d0) in `0x85f410` and `0x85fc10`, which is a different path (T4.7).

### T4.6 The SRT shape (what the shader sees)

- Per draw: user SGPR pair `S` (VS; index = low byte of `layout[2]+0x10`) = chunk base. `chunk[0]` = `P[0x90]` = the
  mesh V# table pointer, **if bit 0 of `material+0x468` is set** (**inferred**; `chunk[k]` is `P[0x90 + 8*k]` for the
  k-th set bit). So the table is reached by two loads: `S -> chunk -> table -> V#[slot]`.
- The port's `Info::ReadUdReg` (`info.h:371-381`) reads one level (the SGPR pair holds the table pointer directly). The
  SRT walker (`ir/passes/srt.h`, `flatten_extended_userdata_pass.cpp`) follows pointer chains and is the piece that must
  handle this shape. **inferred**; confirm with the fetch shader's first `s_load` (T4.10, step 5).
- If `material+0x468` does not have bit 0, the VS has no table pointer in its chunk. That would contradict the fetch of
  mesh attributes, so the bit is likely always set for mesh draws. **inferred**.

### T4.7 Copy or point, and the slot mapping

- **Point, not copy, for the mesh path.** The 0xc0 table is not copied. Only the 8-byte pointer is copied (item `+0x90`,
  then the chunk). **observed**.
- **Transient V# copies exist on another path.** `0x1a6450` (652 bytes, called from `0x1a6ab0` and `0x1a6b30`, used by
  the `0x133xxx` draws of D2) gathers up to 16 V#s (16 bytes each) from the pointer slots of its object, then writes
  them with `0x8619a0` (count 8, table `0x82`) and `0x861ce0` (count 16, table `0x22`) into the stage array
  (`O+0x90+stage*0x1800`, D3 section 4.1). `0x85f410` / `0x85fc10` then copy that array to a chunk (`0x8643f0`) and write
  its base to the layout SGPRs `L+7` etc. (D3 section 4.3). This is not the mesh table. **observed** (calls);
  **inferred** (the copy destinations, from the setter table index).
- **Slot mapping.** The engine does not remap the 12 slots. The mapping is set by the fetch shader's `dword_offset`
  (`VertexAttribute::dword_offset`, "dword offset of the V#", `fetch_shader.h:17-45`). With the 16-byte V# stride,
  **slot = dword_offset / 4**. **inferred** (no fetch shader bytes read). Slot k is `table + 16*k` (D1 section 5.2):

  | slot | stream | slot | stream |
  |---|---|---|---|
  | 0 | index (stream `+0x08`) | 6 | TEX2 |
  | 1 | POS0 (`+0x30`) | 7 | COL2 |
  | 2 | COL0 | 8 | NRM0 |
  | 3 | COL1 | 9 | TAN0 |
  | 4 | TEX0 | 10 | POS0 copy, flags `|= 0x400` |
  | 5 | TEX1 | 11 | TEX3 |

- **Not in the 12 slots:** BONI (`+0x1e8`), BONW (`+0x210`) and SAT_ (`+0x238`) are stream objects but have no slot in
  the table (`0x110ef0` lines 141-148, **observed**). A skinned mesh's blend data therefore does not come through this
  table. Its route is **unknown** (T4.10).

### T4.8 Draw functions involved

| Function | Role in the binding | Label |
|---|---|---|
| `0x11d7a0` -> `0x19af20` -> `0x1a6d20` / `0x1a7290` | The section draw. `0x1a6d20` line 100 and `0x1a7290` line 119 call `0xae150(*param_4, u16(range+0x18)*4 + *(draw-list+0x38))`: a pointer into the 0xa8-byte section buffer, not the mesh table. Also `0xf8990`. | observed |
| `0x180ab0` -> `0x1809a0` -> `0x128d20` -> `0x94d00` | Mesh draw with the global mesh, pass 1 and pass 6. Writes `P+0x90` (`0x1122d0`), then `0x95320`, `0xae150`, then submits. `0x94d00` calls `0x862510` (VS SGPR indices), `0x85fc10` (stage flush) and `0x86b040` (DrawIndexOffset2). | observed |
| `0xdb390` -> `0xdb620` | Indirect mesh draw. Writes `P+0x90`, then `0x95320`, `0xae150`, then `0xdb620` (`0x86b3d0` DrawIndexIndirect). | observed |
| `0x10c2e0` (via `0x103ad0`) | Object mesh draw. Mesh from the object's handle at `+0x140`. Writes `P+0x90` in the no-texture branch. | observed |
| `0x133bd0` (D2) | List-driven instanced draw. Slot 5 = `obj+8` (index stream object), slot 6 = `obj+0x30` (POS0 stream object), slot 7 = an instance list V#, built by `0x8be60(…, class 0x1b, stride 4, count = bytes/4)`. These pointers are stored with `0x1a6440` at `obj+0x50+8*k` and gathered per draw by `0x1a6450`. **Correction to D2:** the slot-7 bind in `0x133550` is `0x861fe0`, which is the T# setter (32 bytes, table `0x22`, D3 section 4.2), not a V#. | observed (calls); inferred (slot meanings) |

### T4.9 Mapping for the port: what it can know at mesh load

1. **Per mesh, once** (after `0x1264d0` returns, D1 section 9): `table = *(mesh+0x70)+0x2c0`. The 12 V# words (base,
   stride, `num_records`, word 3) and the class of each slot (stream `+0x1c`) are fixed at load. **observed** (code path).
2. **Per VS, from the fetch shader:** for attribute `i`, `slot_i = dword_offset_i / 4`. `sgpr_base_i` must equal the
   SRT SGPR of the VS (T4.5). If it differs, the attribute does not come from the mesh table. **inferred**.
3. **Key part:** `vertex_buffer_formats[i]` and `vs_attribs` (D1 section 11) depend only on the V# words of `slot_i`
   (data format, number format, `dst_sel`). So the port can build them before the first draw from the mesh table. A
   missing stream gives the default V# with flags `0x400` (D1 section 3), so those slots must be marked as "default"
   in the key.
4. **Dynamic, per draw:** the SRT chunk base (its contents are the pointer list), the stage-array copies of T4.7, and
   the base address `table[slot].base`.

### T4.10 Refined one-run log spec (replaces D1 section 12 steps 3-4)

Each point is an entry hook. Log once per address and argument set.

1. **Item store.** At `0x1122d0`, and at the store in `0xdb390` (line 83) and `0x10b0a0` (line 85): log `P`, the stream
   pointer, and `*(stream+0x2c0)`. Confirms which builder fills `P+0x90` for each pass.
2. **Compaction.** At `0x95320` entry: log `mask` (the u32 argument) and `P[0..9]`. At return: log the returned chunk
   and its first 8 bytes. Confirms the bit order and the return value (T4.5 step 1).
3. **SGPR write.** At `0x862490` entry: log the stage (`param_2`), the `u16` at `*(O+0xc180+8*stage)+0x10`, and the 8
   bytes at `src`. Confirms the SGPR index and the count byte of the VS (T4.5 step 3).
4. **Draw side.** At the first draw of a mesh, read the VS user SGPRs (from the PM4 stream or the register file). Check
   that the SGPR named in (3) holds the chunk base, that `*(chunk) = P[0x90]`, and that `*(P[0x90]) = mesh+0x70+0x2c0`.
   This closes T4.6.
5. **Fetch shader.** Dump the first `s_load` of the VS fetch shader (`sgpr_base` and `dword_offset` of each attribute).
   Check whether there is one `s_load` (direct V#) or two (SRT pointer first). This settles T4.6 and the slot rule of
   T4.7.
6. **Material mask.** At `0xdb390` entry and `0x1809a0` entry: log `*(u32*)(material+0x468)` per material. Settles T4.5
   step 1 and the bit 0 question of T4.6.
7. **Remaining unknowns**, which no static read answered:
   - The callers of `0x8615f0` (sets `O+0xc130` and `O+0xc168`) and of `0x861730` and `0x861880` (mirror layout). The
     disassembly has no direct `call` to any of them, and no data reference. Ghidra shows them as size-1 stubs, but
     `objdump` shows real code (`0x8615f0`: `movl %esi,0xc168(%rdi)`, `movq %rdx,0xc130(%rdi)`, `ret`). They are reached
     indirectly or not at all (**unknown**).
   - The writers of `O+0xc0e8` and `O+0xc108` (read as direct 8-byte SGPR sources in `0x85f410` lines 111 and 114). No
     writer found in the decomp, so they may use an indirect or block copy (**unknown**).
   - The BONI / BONW / SAT_ route for skinned meshes (T4.7, **unknown**).
   - The callers of the item builders (traced in T6, `indirect-callers.md` section 2): `0x103ad0` and `0x1051f0` are
     tail-jumped from `0x103aa0`, which is entry 0 of the 7-entry function table at `0x15fea00` (REACHED; the call that
     uses the table is unknown). `0xdb700`, `0xcaed0`, `0x1b0980`, `0xaea10` have direct callers. The writers of
     `O+0xc0e8` / `O+0xc108` and the BONI/BONW route remain unknown.

### T4.11 Evidence files (T4)

- `out/re/decomp/0012/00128a80.c` (setter), `0011/00110c90.c`, `0011/00110ef0.c` (line 417), `0011/00111ca0.c`, `0019/00192f20.c`
- `out/re/decomp/0011/001122d0.c` (copy), `0010/0010c2e0.c`, `0010/0010c6c0.c`, `0018/00180ab0.c`, `0018/001809a0.c`,
  `0011/001122c0.c`, `000d/000db390.c`, `0010/0010b0a0.c`, `001b/001b32f0.c`, `000a/000add50.c`
- `out/re/decomp/0009/00095320.c` (compaction), `000a/000ae150.c` (SRT SGPR write), `0086/00862490.c`, `0086/0086d5e0.c`,
  `0086/00862510.c`, `0086/0086d550.c`, `0086/0086d4c0.c`
- `out/re/decomp/000f/000f8c90.c`, `000f/000f8990.c` (material fields), `0013/00132350.c` (stage record lookup)
- `out/re/decomp/001a/001a6440.c` (slot store), `001a/001a6450.c` (V# gather), `001a/001a6ab0.c`, `001a/001a6b30.c`,
  `001a/001a6420.c`, `001a/001a6d20.c` (line 100), `001a/001a7290.c` (line 119)
- `out/re/decomp/0013/00133bd0.c` (instance list, slots 5-7), `0013/00133550.c`
- `out/re/decomp/0085/0085f410.c` (VS flush, lines 74-150), `0085/0085fc10.c`, `0086/00862c50.c` (commit), `0009/00094d00.c`,
  `0012/00128d20.c`
- Disassembly: `scratchpad/t4_disasm.txt` (full `llvm-objdump -d` of `out/re/ghidra/eboot_ghidra.elf`, 6.6 M lines), used
  for the `0x8615f0` / `0x861730` bodies and the empty call lists of the stubs.
- Port: `gpu/shadps4/shader_recompiler/frontend/fetch_shader.h:17-45`, `gpu/shadps4/shader_recompiler/info.h:371-381`
  (`ReadUdReg`), `gpu/shadps4/shader_recompiler/ir/passes/srt.h`, `flatten_extended_userdata_pass.cpp`.

## Skinning streams (T5)

Read-only static analysis (agent T5, follows D1 section 3, T4 section T4.7 and T4.10). Same sources as D1/T4:
Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c` (the first line of each file lists its callers; some callers
are missing where Ghidra did not resolve indirect calls). Addresses are eboot VAs. Labels: **observed** (code, call,
constant), **inferred** (reasoning from observed code), **unknown**. No game file read, no game run.

### T5.1 Short answer

- **The skinned mesh is deformed by a compute shader, not by the vertex shader.** A job named
  `UpdateSkinningRenderObjects` (`0xcfdb0`, string `0xfa1517`) runs per frame, walks the skinned objects in a bitset,
  and for each skinned mesh record dispatches one compute program with `ceil(vertexCount / 64)` groups. **observed**
  (T5.2, T5.3).
- **BONI and BONW reach the GPU through that compute pass, not through the 12-slot table.** The compute input group
  is five V#s in this order: POS0 (`stream+0x30`), NRM0 (`+0x148`), TAN0 (`+0x170`), BONI (`+0x1e8`), BONW (`+0x210`),
  copied from the mesh stream object by `0x1122e0` (**observed**). The 12-slot table (D1 5.2) is not involved in the
  skin dispatch. So brief hypothesis (b), a second V# table for a vertex shader, is **not** the skin route.
- **Skin outputs are three V#s in a per-object record**, not mesh streams. The first output is double-buffered by
  frame parity (`obj+0xe8 + parity*0x28`). **observed** (writes); the roles (position, normal, tangent) are
  **inferred**.
- **SAT_ is a per-triangle cumulative table used by particle mesh emission.** The emitter copies the mesh's SAT_ V# and
  its record count into the spawn request. **observed** (the copy and the count); the meaning (summed areas for
  area-weighted sampling) is **inferred**.
- **The bone palette writer is not found.** The string `boneMatrices` only appears in APEX-style parameter code
  (T5.6). Candidates and the log that settles them are in T5.6 and T5.11.
- **The draw-side consumer of the skin outputs is not found** (T5.7). This is the main open point.

### T5.2 Entry: the job and its two variants (observed)

| Step | Address | What it does | Evidence |
|---|---|---|---|
| Job creation | `0xcfdb0` (406 bytes, caller `0xcefa0`) | Creates job `UpdateSkinningRenderObjects` with body `0xd06f0` and submits it (`0xcfe56`). Then job `UpdateTextureStreamingGPUData` (body `0xd0710`). Then calls `0x118100` (kind dispatch, strings "Update GPU Data" / "InstanceData") | `000c/000cfdb0.c` |
| Job body | `0xd06f0` (21 bytes) | If `param_1[1] != 0` calls `0x116e20(DAT_02edf140, param_1[1], param_1)`, else `0x116fb0(DAT_02edf140, *param_1, param_1)` | `000d/000d06f0.c` |
| Variant A | `0x116e20` (376 bytes) | Walks a 4096-bit set at `manager+0x218070` (cleared at the end, 0x8000 bytes). For each set bit: object from `FUN_00120130(manager+0x20, idx)`; if `obj+8 == 1` calls `0x125520`. Ends with `0x16ee00(cmd, 1, *(ctx+0xe8)+8)` | `0011/00116e20.c` |
| Variant B | `0x116fb0` (376 bytes) | Same walk, calls `0x1255d0`. Ends with `0x92540(cmd, 1, *(ctx+0xe8)+8)` | `0011/00116fb0.c` (diff against A: only the callee and the end call) |
| Per-object list | `0x125520` and `0x1255d0` (172 bytes each) | `scratch = 0x8c0d0(DAT_02ee3ce0+0x520, count*0x30 + 0x10)` with `count = *(*(obj+0x100)+0x18)`; `0x123330(obj)` copies the 0x30-byte nodes of the `obj+0x100` chain (next pointer at node `+0x20`) into the scratch. Then for each record `i < *(short*)(obj+0xf8)` at `obj+0xf0 + 0x58*i` calls `0x193810` (A) or `0x193aa0` (B) | `0012/00125520.c`, `0012/001255d0.c`, `0012/00123330.c` |

Notes:
- The bitset `manager+0x218070` is a copy of a source bitset (`import_memcpy` from one source, 0x8000 bytes, in
  `0x116d80` and `0x118370`). **Who sets the bits: unknown.**
- Variant A vs B: `param_1[1]` decides. **inferred:** A is the path with a command-context handle, B is the default.
  Which one a given frame takes was not traced.
- `0x123330` is called with one argument in `0x125520`/`0x1255d0`, but its decompiled prototype has three. The output
  pointer is not reliable in the decompile. **inferred:** it writes into the scratch returned by `0x8c0d0`.
- The kind value `obj+8 == 1` is the same number `0x118100` uses for `0x123410` (InstanceData_WriteStream). So a kind-1
  object is one render object that may hold instance data and 0..N skinned mesh records (**inferred**, from the two
  bitsets and the shared handle table).

### T5.3 Per-record compute dispatch (observed)

Record `rec` is 0x58 bytes. Fields used:
- `rec+0x20`: handle of the mesh resource. `FUN_0014d7b0(rec+0x20)` gives the mesh object, and its `+0x70` is the
  stream object of T4/D1 (`0x2c8` bytes). So `stream = *(mesh+0x70)`, and `*(stream+0)` is the vertex count (D1 section 2).
- `rec+0x40`: output object `out` (T5.4).

Dispatch function `0x193810` (641 bytes), in order:
1. Compute program. `base = *(*(DAT_02ee3ce0+0x8a8) + 0x68)`, and the handle is `base+0xef0`
   (`FUN_00095580` initialises it lazily, `FUN_0014d7b0` resolves it). `FUN_0016edc0(cmd, *(prog+0x60))` binds the
   program. This is one global program, not a per-mesh program. The `+0x68` layout is the same as the particle
   programs (`particles-engine.md` 2b), but the base differs. **observed**
2. Inputs: `0x1122e0(stream, cmd, ctx)` (T5.5). **observed**
3. Outputs: `out+0xe8 + p*0x28`, `out+0x138`, `out+0x160` as three V#s, with `p = *(ctx+0xd70) & 1` (the frame parity
   of `0x193560` and `0x193580`). For each output whose record flag has `& 0x18` set (flags at `+0x100 + p*0x28`,
   `+0x150`, `+0x178`), `FUN_00883640(cmd+0x10, 1, slot, 0, data + size, 4, 1)` is called for slots 0, 4 and 8. The
   three V#s are then written with `0x85e9a0(cmd+0x50, 0, 3, outs)` (group 0x21, T5.5). **observed** (calls). The
   meaning of the `data + size` argument is **unknown**.
4. Extras: `0x85e820(cmd+0x50, 0, 2, {*scratch, ring})`. The first V# is the first 16 bytes of the scratch allocator
   `DAT_02ee3ce0+0x520` (`*param_4`). The second is the global V# at `DAT_02ee3ce0+0x548`. The count (`*(stream+0)`)
   is written to a 4-byte block from `FUN_0008c0d0(DAT_02ee3ce0+0x548, 0)`. **observed**
5. `0x85e5d0(cmd+0x50)` flush, only if `*(cmd+0x1adc) != 0`. **observed**
6. Dispatch: `0x883870(cmd+0x10, (count + 63) >> 6, 1, 1, flag)`. `0x883870` is the `sceGnmDispatchDirect` wrapper
   (D1 section 11). **observed.** So there are 64 threads per group, one thread per vertex. **inferred** from the 64
   divisor and the vertex count.

Variant B (`0x193aa0`, 682 bytes) does the same in the stage-array form. Inputs go through `0x112380`
(`0x861ce0(*cmd+0x2e8, stage, 0, 5, ...)`), outputs through `0x861e60(..., 0, 3, ...)`, extras through
`0x8619a0(..., 0, 2, ...)`. Then `0x85f220` and the PM4 dispatch `0x86b9e0(cmd, groups, 1, 1, flag)`. **observed.**
The three setters are the stage-array counterparts of the user-data writers of T5.5 (D3 section 4.1, T4 T4.7).
**inferred.**

### T5.4 Skin output record (observed layout, inferred role)

`out` is the object at `rec+0x40`. The skin path reads and writes these fields (**observed**):

```c
struct SkinOutRec {          // 0x28 bytes, same shape as D1 StreamObj (V# first 16 bytes)
  u32 v_word[4];             // +0x00 V# (RW buffer for the compute writer)
  void* data;                // +0x10
  u32  flags;                // +0x18 (tested with & 0x18 by 0x193810)
  u32  format_class;         // +0x1c
  u32  byte_size;            // +0x20
};
// out+0xe8 + parity*0x28 : output A (position, inferred)
// out+0x138              : output B (normal, inferred)
// out+0x160              : output C (tangent, inferred)
// flags of A at out+0x100 + parity*0x28; of B at out+0x150; of C at out+0x178
```

- Only output A is parity-indexed. B and C are single copies. **observed**
- The writer of these records (the object constructor) was not traced. **unknown**
- The class (format) of each output is **unknown**. If the outputs are built like the mesh streams (D1 section 4), they
  reuse the input classes. **inferred**.

### T5.5 The five inputs: BONI and BONW (observed)

`0x1122e0` (150 bytes, caller `0x193810`) and `0x112380` (163 bytes, caller `0x193aa0`) are the only readers of
`stream+0x1e8` and `stream+0x210` as V#s in the decompile (grep of `+ 0x1e8` and `+ 0x210` in `out/re/decomp`). Each
reads 16 bytes from each offset (`*(stream+off)` and `*(stream+off+8)`), in this order (**observed**):

| Input index | Stream object offset | Stream |
|---|---|---|
| 0 | `+0x30` | POS0 |
| 1 | `+0x148` | NRM0 |
| 2 | `+0x170` | TAN0 |
| 3 | `+0x1e8` | BONI |
| 4 | `+0x210` | BONW |

`0x1122e0` passes them to `0x85e8e0(cmd+0x50, 0, 5, src)` (its line 28). `0x85e8e0` writes each V# to the user-data
shadow at `cursor+0x98 + 4*sgpr`, with the SGPR taken from a stage table at `cursor+0x18a8` at index `stage + 0x11`.
**observed** (decomp of `0x85e8e0`). The output and extra groups use the same writer with a different index base:

| Writer | Index base (`param_2 + k`) | Table index used | Count | Used for |
|---|---|---|---|---|
| `0x85e8e0` | `+0x11` | `0x11..0x15` | 5 | skin inputs (POS, NRM, TAN, BONI, BONW) |
| `0x85e9a0` | `+0x21` | `0x21..0x23` | 3 | skin outputs A, B, C |
| `0x85e820` | `+0x41` | `0x41..0x42` | 2 | scratch V# and count V# |

(The three functions differ only in that index constant: `+0x11`, `+0x21`, `+0x41`. **observed** by diff.)

**Consequence for the port.** BONI and BONW are never part of a draw V# table in this path. They are bound only for
the skin compute dispatch. A missing stream falls back to the default V# (D1 section 3, flags `0x400`), so a mesh with no
BONI would still run the same dispatch with the default V#. **inferred**.

### T5.6 Bone palette: candidates, not found (open)

- **No dedicated palette writer was found** for the game's own skinned meshes. The dispatch reads no palette V# beyond
  the two extras of T5.3 step 4. So the palette is either inside the scratch table (the `0x30`-byte nodes copied by
  `0x123330` from `obj+0x100`, T5.2), or inside the BONI/BONW data, or a per-object constant. **unknown**.
- **Candidate A (inferred, not proven):** the `0x30`-byte nodes of the `obj+0x100` chain are per-instance records copied
  verbatim into the scratch (`0x123330`). If the node holds a 3x4 matrix, the scratch is the palette. The node writer
  is not in the decompile. Check with the log of T5.11 step 6.
- **Candidate B (object transform, inferred):** `0x123410` (InstanceData_WriteStream, kind 1) reads object fields
  `obj+0x14..0x54` and a 16-byte block through `*(obj+0x110)` (or `DAT_02ee3ce0+0x380`) at `+0xc4..+0xe4`. This is
  per object, not per bone. It is probably not the palette.
- **`boneMatrices` strings (observed):** the name appears as the argument of a vtable call (slot `+0x90`) in
  `00a3e170` line 127, `00f53f00` line 828, and `00ef8cd0`, `00f094e0`. `00f1dd70` does the same with `"bones"`. The
  same code declares `fallbackSkinning`, `skinningNormals`, `morphDisplacements`, `graphicalLods`, `boneSpheres`,
  `multiplyGlobalPoseIntoBones`, and classes `nvidia::clothing::...` and `physx::...`. **inferred:** this is APEX
  (NVIDIA cloth and rendering) parameter code, not the game's own skin path. It is not on the skin dispatch path.
- **Ruled out for the palette:** `0x55160` (26 KB) uses the string `Vertex Animation` only as a debug-menu label
  (menu entries via `0x27120`). `0x2950a0` (1,059 bytes) reads a tagged resource (FourCC `0x6ad4907c`), not a joint
  evaluation. `0x8c370` (`Models Skinning`, `0xf9ee01`) only creates memory-statistics handles (`0x11bc0`).
- **Animation evaluator:** the strings `r_enableAnimations` (`0x24db80`, 3,243 bytes) and `CP10-Anim` (42 uses) mark
  the animation code. It was not traced further. **unknown** whether it writes the palette.

### T5.7 Draw side: not found (open)

- The skin outputs (T5.4) are written as V#s in the skin dispatch. **No draw item builder that reads
  `out+0xe8/+0x138/+0x160` was found.** Outside `0x193810` and `0x193aa0`, the `obj+0x40` object does not appear in the
  decompile of the skin path.
- The mesh item builders (`0x1122d0` callers `0x10c2e0`, `0x10c6c0`, `0x180ab0`, `0x1b32f0`, `0xadd50`) all copy the
  mesh table pointer `stream+0x2c0` (T4). **observed.** None reads the skin outputs.
- So a skinned draw binds the skin outputs through some other item pointer (`P+0x98..`, the SRT chunk of T4.5/T4.6), or
  through a table built per frame. **unknown.** This is the main open point. The port's VS would then fetch POS from a
  V# that is not the mesh table.
- **Slot 10 (POS0 copy, `stream+0x198`, flags `0x400`, D1 5.3):** the skin path does not touch it (its input is POS0 at
  `+0x30`). Its meaning is still **unknown**.

### T5.8 Hypothesis (c): SAT_ (observed use, inferred meaning)

- **Stream:** `stream+0x238` (SAT_, fourcc `0x5341545f`, D1 section 3). Readers found: `0x10c2e0` and `0x10c6c0` (both
  return `*(mesh+0x70) + 0x238`, line 70), and the particle `UpdateSimulation` (`0x103ad0`, lines 357-409; `0x1051f0`
  has the same code). **observed**
- These functions are used only when the emitter's mesh handle (`emitter+0x140`, resolved through `0x14d7b0`) is
  non-null: the "mesh emission" branch (`particles-engine.md` 3b, typed block B). **observed**
- `0x103ad0` lines 377-409: the returned SAT V# (16 bytes) is copied into the spawn request at byte offset `+0x70`.
  Its record count, `*(u32*)(SAT+8)` (V# word 2), goes to the request at `+0x1c`. If the SAT flags byte `+0x19 & 1` is
  set, the count is 0 instead. Without a mesh, the request gets the default V# at `DAT_02ee3ce0+0x3b8`. **observed**
- **Meaning (inferred):** a summed-area (cumulative) table of per-triangle areas, sampled by the spawn compute with a
  random value (`0x158370`, written to request `+0x04`). Area-weighted random triangle selection is the usual way to emit
  particles over a mesh surface. The string `filterParticle_EmitFromSkinnedMesh` (`vertex-duplicates.md`, near `0xfa1517`)
  suggests the emitter can also draw from a skinned mesh. Its code was not found in the decompile, so that link is
  **unknown**.
- Element format of SAT_ (class at `+0x1c`): **unknown**. It is a per-mesh stream, so its class and count follow the
  `.cmsh` format index (D1 section 4). Not read.

### T5.9 Mesh-level facts the port can read at load

- At the return of `0x1264d0` (D1 section 12, step 1) the mesh has stream objects for BONI (`+0x1e8`), BONW (`+0x210`) and
  SAT_ (`+0x238`), with the same layout as the other streams (D1 section 2). Whether a mesh has real BONI/BONW data is
  the bit that decides skinning. **inferred**, from the dispatch reading them without a check. A missing BONI or BONW
  is the default V# (flags `0x400`).
- The skin dispatches per frame = the number of skin records in the active bitset (T5.2). Each is one dispatch of
  `ceil(vertexCount / 64)` groups (T5.3). The vertex count is `*(stream+0)` (D1 section 2), so the port can compute the
  dispatch sizes at load.

### T5.10 Pipeline-key and prebuild consequences (inferred unless marked)

1. **Vertex shaders:** no skin-specific `vs_attribs` are needed for the skin outputs, provided the draw path (T5.7)
   reads them as plain V#s. The VS key gains no skin flag from this pass. **observed** that the skin dispatch is compute,
   so no vertex shader is generated for it.
2. **Compute programs:** one global skin program (handle `base+0xef0`, T5.3 step 1), so the set is small. Its name is
   held in the handle and must be read at run time (T5.11 step 3). **observed** (one program per handle).
   **inferred:** variants exist only through the input and output formats (item 3).
3. **Compute key fields:** the five input V#s (POS0, NRM0, TAN0, BONI, BONW) and three outputs. If the port keys
   buffer formats for compute as it does for vertex buffers (D1 section 11), each distinct (POS, NRM, TAN, BONI, BONW)
   format set splits the key. **inferred.** The number of distinct format sets in the game's meshes is **unknown**
   (no .cmsh read).
4. **Particle spawn compute:** the spawn request for mesh emitters carries the SAT V# and its count (T5.8). Its key may
   split on the SAT class. **inferred.**
5. **Variant A vs B:** `0x193810` (user-data path) and `0x193aa0` (stage-array path) dispatch the same program. They differ
   in how the V#s are written. Which one runs is decided by `param_1[1]` (T5.2). **inferred** that a single program is
   seen per skinned mesh.

### T5.11 Log spec: one run, what stays open

Add entry logs at these eboot VAs (add the port's load base, as in D1 section 12). Each costs a few dwords per object
per frame.

1. `0x116e20` / `0x116fb0`: log the variant (`param_1[1]` zero or not) and the number of set bits in the 0x8000-byte set.
   Confirms the variant and the number of skin objects per frame.
2. `0x125520` / `0x1255d0`: log `obj`, `*(obj+0xf8)` (record count), and `*(*(obj+0x100)+0x18)` (node count).
3. `0x193810` and `0x193aa0`: per record, log `rec`, the mesh handle `*(rec+0x20)`, `*(mesh+0x70)` (stream), the vertex count
   `*(stream)`, and the 16-byte V#s at `stream+0x30, +0x148, +0x170, +0x1e8, +0x210`. For BONI and BONW also log the flags
   (`+0x18`), class (`+0x1c`) and byte size (`+0x20`). A flags word with `0x400` means the stream is missing. Log the
   program blob header `*(prog+0x60)` (the program hash is in it) and the group count given to `0x883870` or `0x86b9e0`.
4. `0x193810` at the output step: log `out`, the parity `*(ctx+0xd70) & 1`, and the three V#s at `out+0xe8 + p*0x28`,
   `out+0x138` and `out+0x160`.
5. `0x10c2e0` mesh branch: log the SAT V# (`stream+0x238`), its flags byte (`+0x19`), and the count written to the
   spawn request. This shows whether SAT is populated and its element count.
6. **Bone palette (open):** log the first 48 bytes of each `0x30`-byte node of `obj+0x100` once per frame (the `0x123330`
   input), and the first 16 bytes of the scratch returned by `0x8c0d0(DAT_02ee3ce0+0x520)` after `0x123330`. If the
   palette is there, the 3x4 matrices show up as plausible floats (a 3x4 with a rotation block and a translation column).
7. **Draw side (open):** at the first draw after a skin dispatch of object `obj`, log the VS user SGPRs and check whether
   any of the three output V# base addresses (T5.4) appears in them, or in a V# table they point to. A memory watch on
   output A's base address (`out+0xf8 + p*0x28`) during the draw answers whether the VS reads it.
8. **Still open after the run:** who writes the `obj+0x100` node chain and the `0x58` records (the object constructor or an
   update before `0x116e20`); who sets the dirty bits of `manager+0x218070` (callers of `0x116d80`); the format class of
   each output; the SAT element format.

### T5.12 Evidence (addresses and files)

- Job and dispatch chain: `out/re/decomp/000c/000cfdb0.c`, `000d/000d06f0.c`, `0011/00116e20.c`, `0011/00116fb0.c`,
  `0012/00125520.c`, `0012/001255d0.c`, `0012/00123330.c`, `0019/00193810.c`, `0019/00193aa0.c`, `0011/00118100.c`
  (kind dispatch), `0012/00123410.c` (kind 1, InstanceData).
- Inputs and writers: `0011/001122e0.c`, `0011/00112380.c`, `0085/0085e8e0.c`, `0085/0085e9a0.c`, `0085/0085e820.c`
  (differ only in the index constant), `0086/00861ce0.c`, `0086/00861e60.c`, `0086/008619a0.c`.
- Dispatch and binds: `0088/00883870.c` (sceGnmDispatchDirect wrapper), `0086/0086b9e0.c` (PM4 dispatch), `0088/00883640.c`,
  `0086/0086a700.c`, `0016/0016edc0.c` (bind compute), `0009/00095580.c` (lazy handle), `0014/0014d7b0.c` (handle to object).
- Frame parity: `0019/00193560.c`, `0019/00193580.c` (`*(ctx+0xd70) & 1`).
- SAT consumer: `0010/0010c2e0.c` (return `+0x238`, line 70), `0010/0010c6c0.c`, `0010/00103ad0.c` (lines 357-409),
  `0010/001051f0.c`.
- Mesh item builder: `0011/001122d0.c` (copies `stream+0x2c0`), callers in T4.3.
- Strings: `Models Skinning` (`0xf9ee01`, `0008/0008c370.c` line 97), `UpdateSkinningRenderObjects` (`0xfa1517`),
  `boneMatrices` (`00a3e170`, `00f53f00`, `00ef8cd0`, `00f094e0`), `Vertex Animation` (`0005/00055160.c` line 1333).
- Not used: `0x368a70` (`ProceduralMesh` stream builder, up to 15 streams with flag `0x800`, called from `0x368c00`) is a
  different mesh type and does not use the 13 fourcc names. Not traced further.
- Corrections to earlier notes: T4.10 item 7 ("the BONI / BONW / SAT_ route ... unknown") is answered for BONI and BONW
  (T5.5) and for SAT_ (T5.8). D1 section 5.3 ("the meaning of the second POS is unknown") still holds: the skin path does
  not write slot 10.

## Skinning palette and draw (T14)

Read-only static analysis (agent T14, closes the two open items of T5: the bone palette and the draw side). Same sources
as T4/T5: decomp in `out/re/decomp/<hi16>/<addr>.c`, and the saved eboot disassembly `scratchpad/t4_disasm.txt` (from
`llvm-objdump -d out/re/ghidra/eboot_ghidra.elf`) for the loops the decompile gets wrong or hides (`0x123330`,
`0x125520`, `0x10c2e0`). Addresses are eboot VAs. Labels: **observed**, **inferred**, **unknown**. No game file read,
no game run.

### T14.1 Short answer

- **The T5 palette candidate is confirmed in structure.** `obj+0x100` is a palette head (count at `+0x18`, array
  pointer at `+0x20`). The array holds `NumBones` nodes of `0x30` bytes, each a 3x4 affine matrix. It is created with
  the skeleton (`TryCreateMesh`, `0x10f760`), initialised to identity, filled each frame from pose records, and copied
  into the skin scratch by `0x123330` on every skin dispatch. **observed** (layout, init, copy). The matrix values
  come from an animation producer that is **not** identified (T14.3, open).
- **Correction to T5.2:** `0x123330` does not walk a linked chain. The nodes are an array at head `+0x20`, indexed by
  the loop. The "next pointer at node `+0x20`" reading is wrong. **observed** (disassembly).
- **The palette V# is the scratch V#, extra index 0x41.** The skin inputs (POS0, NRM0, TAN0, BONI, BONW, indices
  `0x11..0x15`) and outputs (`0x21..0x23`) are as T5.5. The palette and the vertex count go in the extras `0x41` and
  `0x42`. **observed** (`0x193810`).
- **The draw side is found.** A skinned draw does not read a new kind of table. Each skinned record has an output
  object `out` (`rec+0x40`), and `out` holds two 0x68-byte "slot holders", one per frame parity. Slot 1 (POS0) of
  holder `p` is skin output A of parity `p`, slot 8 (NRM0) is output B, slot 9 (TAN0) is output C. The object mesh draw
  (`0x10c2e0`, `0x10c6c0`) puts holder `p`'s V# table into item `P+0x90`. So the output addresses reach the VS through
  the T4 table-pointer path (T4.5-T4.7), with the parity picked at draw time. **observed** (stores, loads, parity
  expression). The VS reading POS from slot 1 of that table is **inferred** (T4.7 slot rule; no fetch shader read).
- **Memory model:** the skin outputs are written and read by the GPU, and no CPU read was found. They are a class not
  in `memory-model.md` (not RESULT, which the CPU reads; not STREAM, which the CPU writes). The palette array is CPU
  written each frame and reaches the GPU only through the per-frame scratch, so the scratch is STREAM (T14.7).

### T14.2 The palette object `obj+0x100` (layout observed, meaning inferred)

Creation in `TryCreateMesh` (`0x10f760`, decomp `0010/0010f760.c`). `uVar1 = NumBones = *(u32*)(mesh_resource + 0x16c)`
for the first LOD that has a skeleton (the string at line 120 names it: "LODs have mismatched skeletons: LOD0(NumBones
= %d, SkelId = %d)"). Lines 176-212 build the palette:

```c
struct PaletteHead {            // 0x28 bytes, FUN_0016cd90 at 0x10f760 line ~178; stored at obj+0x100
  u64 pad[3];                   // +0x00..0x17 (zero)
  u32 count;                    // +0x18  = NumBones
  u32 pad1;
  float* nodes;                 // +0x20  -> NumBones * 0x30 bytes
};
struct PaletteNode {            // 0x30 bytes, 12 floats; column-major 3x4 (inferred)
  float w[12];                  // init: w0 = w4 = w8 = 1.0, w1..w3, w5..w7, w9..w11 = 0
};
```

- **Identity init (observed):** every node gets `[0] = 1.0f`, `[4] = 1.0f`, `[8] = 1.0f` and zeros elsewhere (the
  `*puVar13 = 0x3f800000` stores and the `puVar13[2]`/`puVar13[4]` stores of lines 178-212). Read as four columns of
  three floats, the diagonal is words 0, 4, 8 and the translation is words 9..11. So the initial value is the identity
  with zero translation. **inferred:** column-major 3x4 (the translation is the fourth column).
- **Scratch copy (observed, `0x123330`, 198 bytes; disassembly `123330`-`1233f4`).** The loop reads the array base from
  `head+0x20` each time, then for node `j` (stride `0x30`) it stores twelve dwords with this permutation, source word
  `w` to destination word `k`:

  | dst k | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | src w | 0 | 3 | 6 | 9 | 1 | 4 | 7 | 10 | 2 | 5 | 8 | 11 |

  So `dst[4c + r] = src[3r + c]`. If `src` is the column-major 3x4 above, `dst` is the same matrix as a row-major 3x4
  (three rows of four floats). **observed:** the permutation; **inferred:** the layouts it implies.
- **Scratch header (observed):** `dword[0] = 1`, `dword[1] = count`, nodes from byte `+0x10`. Size `count*0x30 + 0x10`
  (`0x125520` disassembly `12555a`-`12555d`: `lea (rax,rax,2)`, `shl 4`, `add 0x10`).
- **Scratch argument (observed):** `0x125520` calls `0x8c0d0(DAT_02ee3ce0+0x520, size)` and passes its return value in
  `rdx` to `0x123330` (`125568`-`12556e`). The decompile shows one argument, which is why T5 flagged the pointer.

**Verdict on T5.6 candidate A:** `obj+0x100` is an array of `NumBones` 3x4 affine matrices, identity at load, updated
per frame, and copied into the scratch on every skin dispatch. **Proven:** structure, count source, init, copy. **Not
checked:** that the skin shader indexes this array with BONI (T14.8, log step 4). Candidate B (`0x123410`, the object
world matrix at `obj+0x14..0x44`) is a different thing: the world matrix, copied from the pose record (T14.3).

### T14.3 Per-frame writer of the palette (observed up to the producer)

- **Consumer `0x4938e0`** (307 bytes; caller `0x536050` line 162: `FUN_004938e0(DAT_02ef8c08, plVar5)`). `0x536050`
  (1067 bytes, no static caller; per frame) sets the frame context and calls it. **observed.**
- Pending records are a singly linked list at `manager+0xa8 + idx*0x20`, where `idx = manager+0x20 - 1` (or 1 when the
  count is 0). The list is per frame; **inferred** that the manager keeps two slots. Record fields (observed):

  ```c
  struct PoseRec {                 // one object's pose for one frame (4938e0 view)
    struct PoseRec* next;          // +0x00
    u64 obj;                       // +0x08  render object
    u32 count;                     // +0x10  node count from the producer
    float world[12];               // +0x14  48 bytes -> obj+0x14..0x44 (0x123db0)
    float bones[];                 // +0x44  count*0x30 bytes -> palette array
  };
  ```

- For each record, `0x4938e0` calls `0x123db0(obj, rec+0x14)` (which writes `obj+0x14..0x44`), then gets the array base
  with `0x123f10(obj, count)` (returns `*(obj+0x100)+0x20`), then copies `min(count, NumBones) * 0x30` bytes from
  `rec+0x44` (lines 22-37). **observed.** The test `*(obj+8) != -1` (line 39) only gates a call to `0xd3450`; it does not
  skip the copy.
- The second consumer `0x493a20` (136 bytes, no static caller) has the same work with a different layout: the
  arguments are `(manager, count, rec)`, the world is at `rec+0x00` and the bones at `rec+0x30`. **observed.**
- **Dirty set (answers T5.11 item 8 in part).** `0x123f10` (line 16) and `0x123db0` (lines 5-9) set bit `obj+0xc` in
  the bitset at `DAT_02edf140 + 0x208070`. `0x116d80` and `0x118370` copy that bitset to `+0x210070` and `+0x218070`.
  `0x116e20` walks `+0x218070` (lines 15 and 33) and clears it (line 58, `memset 0x8000`). So T5's "manager+0x218070"
  is the walker's copy, and the writers set the `+0x208070` bits. **observed.**
- **Producer (open).** `0x493ad0` (235 bytes; callers `0x58df60` line 73 and `0x5902c0` line 107) calls `0x493bc0`
  (389 bytes; about twelve callers, e.g. `0x78bc50`, `0x6ffa30`, `0x7fd2b0`). `0x493bc0` builds a reference-counted
  work item (vtable `PTR_FUN_015bf980`) from its arguments and queues it. **observed.** Whether this item is the pose
  record, and where the bone matrices are computed (the animation evaluator), is **unknown**. The T5.6 leads
  `r_enableAnimations` (`0x24db80`) and `CP10-Anim` were not traced.

### T14.4 The skin dispatch: which V# carries the palette (answers T5.3 step 4)

Per object (`0x125520` for variant A, `0x1255d0` for B), in this order (**observed**):

1. Scratch: `0x8c0d0(DAT_02ee3ce0+0x520, count*0x30+0x10)` sets the base words of the 16-byte V# at
   `DAT_02ee3ce0+0x520` to a new block (`0x8c0d0` writes `param_1[0..1]` only). `0x123330` fills the block (T14.2).
2. For each skin record `rec` (`obj+0xf0 + 0x58*i`, `i < obj+0xf8`): `0x193810(rec, cmd, ctx, &scratch_V#)`.
3. In `0x193810`, extras go through `0x85e820` (T5.5): index `0x41` = the scratch V# (`*param_4`, 16 bytes; base = the
   palette block). Index `0x42` = the V# at `DAT_02ee3ce0+0x548`, whose 4-byte block holds `*(stream+0)`, the **vertex
   count** (`0x193810` lines 19-26). **observed.**
4. Palette V# (index `0x41`): base = the scratch block. Node `k` is at `base + 0x10 + 0x30*k`. The bone count is
   `dword[1]` of the block. Stride and record count (V# words 2-3) come from the template at `DAT_02ee3ce0+0x520`,
   which was not read (**unknown**). The palette is per object: all records of one object share one block, because the
   scratch is set once per object before the record loop. **observed** (`0x125520` lines 4-8).
5. Index and weight streams: BONI (`stream+0x1e8`, input 3) and BONW (`stream+0x210`, input 4). The shader's bone index
   selects node `k` (**inferred**, T14.8).

Variant B (`0x193aa0`) writes the same palette V# through the stage-array writer `0x8619a0` (T5.3). **observed.**

### T14.5 The draw side: per-parity holder tables (the missing link of T5.7)

**How an object gets its output record (observed, `0x10f760` lines 306-352).** For each LOD record `rec` (`0x58`
bytes, `lVar9`), the builder chooses by the LOD's NumBones (`uVar1`):

- NumBones == 0 (plain mesh): `rec+0x38 = stream+0x260` (the mesh's 12-pointer slot array, T4.2). `rec+0x40` stays 0
  (`0x193280` zeroes `rec+0x30..0x4f`).
- NumBones != 0 (skinned): `out = FUN_0016cd90(...)`, `FUN_00192de0(out)` (constructor, vtable
  `PTR_FUN_01598930`), `out+0xd8 = mesh handle`, `FUN_00192f20(out)`, then `rec+0x38 = out+8` and `rec+0x40 = out`.

`0x192f20` (862 bytes) builds `out` from the mesh stream `stream = *(mesh+0x70)`:

- **Two holders.** A holder is a 0x68-byte struct: 12 slot pointers at `+0x00..0x5f`, and a table pointer at `+0x60`
  (set by `0x128a80`, which stores at `obj+0x60`, T4.2). `out` has holder `H0 = out+0x08` and holder `H1 = out+0x70`,
  with tables `*(out+0x68)` and `*(out+0xd0)`. Each table is `0xc0` bytes (`0x17140(0xc0)`).
- **Output V# objects inside `out` (observed, `0x8be60` calls at the end of `0x192f20`):**
  - A[0] at `out+0xe8` and A[1] at `out+0x110`: two separate V# objects, class 2 (`0x8be60(…, 2, 0x8c1d0(2), count, 4, 0, 0)`).
    Together they are the parity pair of T5.4 (`out+0xe8 + p*0x28`).
  - B at `out+0x138` and C at `out+0x160`: class `0x12` (`0x8be60(…, 0x12, 0x8c1d0(0x12), count, 4, 0, 0)`).
  - `count = *(stream+0)`, the vertex count (D1 section 2). The third argument is multiplied by the count
    (`0x8be60` line `param_1[8] = param_4 * param_3`). It is probably the element stride, but `0x8c1d0` was not read,
    so the element size is **unknown**.
  - The entry flags are at `+0x18` of each entry (`out+0x100`, `+0x150`, `+0x178`), as T5.4 says. **observed.**
- **Pointers that fill the holders (observed, `0x192f20` lines 1-70 and the tail):**

  | holder slot | H0 (`out+0x08`) | H1 (`out+0x70`) |
  |---|---|---|
  | 0 | stream slot 0 (copied) | stream slot 0 (copied) |
  | 1 (POS0) | `out+0x10` = `out+0xe8` (A[0]) | `out+0x78` = `out+0x110` (A[1]) |
  | 2..7 | stream slots (copied) | stream slots (copied) |
  | 8 (NRM0) | `out+0x48` = `out+0x138` (B) | `out+0xb0` = `out+0x138` (B) |
  | 9 (TAN0) | `out+0x50` = `out+0x160` (C) | `out+0xb8` = `out+0x160` (C) |
  | 10 (POS0 copy, flag 0x400 in D1 5.3) | `out+0x58` = `out+0x110` (A[1]) | `out+0xc0` = `out+0xe8` (A[0]) |
  | 11 | stream slot (copied) | stream slot (copied) |

  The stream slot pointers are overwritten with the output addresses before the V# copy loop (lines 70-140). That loop
  dereferences each slot pointer and copies its first two qwords into the table at `holder+0x60`. So it copies the
  output V#s. **observed** (order and stores).
- **Holder selection (observed, `0x193560`, 31 bytes):** `FUN_00193560(rec, ctx)` returns `rec+0x38 + 0x68*p`, where
  `p = *(*(ctx+0x10)+0xd70) & 1` when `rec+0x40 != 0`, else `p = 0`. For a skinned record this is `H0` or `H1`.
  **observed.** The skin dispatch reads the parity the same way (`0x193810`: `*(*(ctx+0x10)+0xd70) & 1`, and `0x193580`).

**The draw item (observed, `0x10c2e0`, 498 bytes; `0x10c6c0` is the same with `0x1239a0` in place of `0x125500`):**

```c
// 0x10c2e0(render_ctx, P, ...)   P = draw item; called from 0x103ad0 (and 0x1051f0 for 0x10c6c0)
// lVar6 = the object's skin instance, found through the 0x198 bitset of the render object; 0 for a plain mesh.
if (lVar6 == 0) {
    FUN_001122d0(stream, P, 0x20);           // plain mesh: P+0x90 = stream+0x2c0 (T4.3)
} else {
    FUN_00125500(lVar6, ctx, P, lVar6+0x68); // P+0xa8 = 44-bit base of obj+0x68
    // loop over records (FUN_00123da0(lVar6, i)): keep the record whose mesh == the item's mesh;
    // if none matches, the last record.
    rec    = FUN_00123da0(lVar6, i);
    holder = FUN_00193560(rec, P);           // parity from *(*(P+0x10)+0xd70) & 1
    P[0x12] = *(holder + 0x60);              // P+0x90 = per-parity V# table (12 entries)
}
```

- For a skinned object, `P+0x90` is the per-parity table `*(holder+0x60)`. Its slot 1 (`table + 16*1`) is a copy of
  A[p] (skin output POS), slot 8 is B (NRM0), slot 9 is C (TAN0). **observed** (copy and stores). **inferred:** the
  VS reads POS from slot 1 (the T4.7 slot rule) through the SRT chunk of T4.5.
- **Parity source for the draw (inferred, check at run time):** `0x10c2e0` reads the parity from `*(P+0x10)+0xd70`,
  whereas `0x193810` reads it from `*(ctx+0x10)+0xd70`. Both are `+0xd70` of a pointer at `+0x10`. They match only if
  `P+0x10` and the walker's `ctx+0x10` point to the same frame state. The log spec below checks it.
- **The skin path is taken only when the object has a set bit** in the `0x198` bitset (lines 16-24). The gate at line
  15 tests a flag of the resource at `+0x60` (`*(*(lVar3+0x60)+0x54) & 2`). Its meaning is **unknown**.
- **Consequence for T4.6:** the table is reached in one load from the SRT (`chunk[0] = P+0x90`, T4.5). No extra
  indirection beyond what T4.6 states.

**Answer to the brief's three options for the skinned draw:**
1. An item builder that copies the output V#s into item slots: **partly.** The builders copy pointers. The V# copy is
   done once, at construction (`0x192f20`), into the holder tables.
2. The output written into the mesh table `stream+0x2c0`: **no.** `stream+0x2c0` is untouched. The per-object holder
   table is what gets the output addresses (at construction, not per frame).
3. The double-buffer parity applied at draw time: **yes.** `0x193560` picks holder `p` from the frame parity.

### T14.6 Per-frame skinned path, end to end

1. **Load** (`0x10f760`, TryCreateMesh): object `obj` with the identity palette (`obj+0x100`, `NumBones` nodes), `0x58`-byte
   records (`obj+0xf0`, count `obj+0xf8`). Each skinned LOD gets an output object `out`: two holders, A[0]/A[1], B, C,
   and the two copied V# tables. **observed.**
2. **Frame, pose** (`0x493ad0`/`0x493bc0` queue; producer open): the pose record (world plus palette) is queued and then
   applied by `0x4938e0` (from `0x536050`) to `obj+0x14..0x44` and the palette array; the dirty bit is set. **observed**
   up to the producer.
3. **Frame, skin job** (`UpdateSkinningRenderObjects`, `0xcfdb0`, body `0xd06f0` -> `0x116e20` or `0x116fb0`): the
   dirty bitset is snapshotted (`0x116d80`/`0x118370`) and walked (`0x116e20`). Per object, `0x125520`: scratch and
   palette copy (`0x123330`). Per record, `0x193810`: program, inputs, outputs A[p]/B/C, extras (palette, count),
   dispatch of `ceil(vertexCount/64)` groups. **observed.**
4. **Frame, draw** (`0x103ad0` / `0x1051f0` -> `0x10c2e0` / `0x10c6c0`): item `P+0x90` = holder `p` table. The section
   draw submits it through the SRT chunk of T4.5 (`0x95320`, `0xae150`, `0x862490`). **observed** up to the SGPR write;
   the VS side is **inferred**.
5. **Synchronisation between the skin compute and the draw:** not checked. `0x193810` ends with `0x883870` (dispatch)
   and calls `0x85e5d0` (flush) only if `*(cmd+0x1adc) != 0` (line 44). **unknown** whether a barrier is issued. The port
   needs one unless the flush covers it (**inferred**).

### T14.7 Memory-model classes (for `arch-next/memory-model.md`)

| Buffer | Written by | Read by | Lifetime | Class (inferred) |
|---|---|---|---|---|
| Palette array `obj+0x100 +0x20` (NumBones x 0x30) | CPU, job (`0x4938e0`) | CPU (`0x123330`) only | persistent, per object | CPU-only guest memory; not a GPU input |
| Scratch block (palette copy, bump from `DAT_02ee3ce0+0x520`) | CPU (`0x123330`) | GPU, same frame (skin compute) | per frame | STREAM |
| Count block (`DAT_02ee3ce0+0x548`) | CPU (`0x193810`) | GPU, same frame | per frame | STREAM |
| Skin outputs A[0]/A[1], B, C (`out+0xe8`, `+0x110`, `+0x138`, `+0x160`) | GPU (skin compute) | GPU (draws this frame; the other parity through slot 10 next frame, **inferred**) | persistent | new class: GPU-written and GPU-read, no CPU read found. Not RESULT, not STREAM. Device-local in the port. |

- The mesh streams (POS0, NRM0, TAN0, BONI, BONW) stay STATIC, as the rest of the mesh (T4.9).
- A port that imports every GPU-written range as RESULT (memory-model section 2.5) would import A/B/C needlessly, because
  the CPU never reads them. **inferred.**

### T14.8 Open items and the one-run log spec

Open:
1. **The producer of the pose matrices.** `0x493bc0` is the queue. Who fills the pose record is unknown.
2. **The skin shader's use of BONI** as an index into `obj+0x100` (node index = BONI value, no remap). Not read.
3. **Output V# element size** (`0x8c1d0(2)`, `0x8c1d0(0x12)`, not read) and the format of palette V# words 2-3.
4. **Whether the VS reads POS from slot 1 of the holder table**, and the use of slot 10 (other parity: motion vectors?).
5. **Barrier** between the skin dispatch and the draw.
6. **Parity source** `*(P+0x10)` (draw) versus `*(ctx+0x10)` (walker): same frame state or not.
7. **Gate** at `0x10c2e0` line 15.

One run, no code change. Add entry logs at the VAs below (add the port load base, as in D1 section 12). Log once per
address and argument set, one line each.

1. `0x4938e0` entry: `obj`, `*(manager+0x20)`, record count `rec+0x10`, `rec+0x14` (48 bytes), and the first 48 bytes of
   `rec+0x44`. Expect animated frames to give a palette that changes between frames and is not identity. This identifies
   the producer's data (open item 1).
2. `0x493bc0` entry: the argument qwords and the return address. Expect one of the callers listed in T14.3. Names the
   producer (open item 1).
3. `0x123330` exit: `*(obj+0x100)+0x18` and scratch dwords `[0..1]` (must be `1, count`), and node 0 (12 floats, row-major
   after the permutation). Expect node 0 to be the source node 0 with the permutation of T14.2 applied.
4. `0x193810` after the outputs: the palette V# base (`*param_4`), the count V# value (`*(stream+0)`), and the maximum BONI
   value over the vertices on the first dispatch (one CPU scan of the mesh buffer `stream+0x1e8`). The maximum must be
   below `NumBones`. This settles open item 2.
5. `0x10c2e0` at the store to `P+0x90`: `p` from both sources (`*(P+0x10)+0xd70` and `*(ctx+0x10)+0xd70` at the walker),
   `holder`, `*(holder+0x60)`, and the 16-byte V# at `table+0x10`. The V# base must equal `out+0xe8` (p = 0) or
   `out+0x110` (p = 1). This settles the draw mapping and open item 6.
6. First draw of a skinned mesh: the VS user SGPR pair (from the PM4 stream) and the first `s_load` of the fetch shader.
   Check that the POS attribute has `dword_offset / 4 == 1` and that the SRT chunk `[0] == P+0x90`. Settles open item 4.
7. Calls to `0x85e5d0` between the skin dispatch and the draw: record whether a flush or barrier occurs (open item 5).

### T14.9 Corrections to earlier notes

- T5.2: `0x123330` copies an array at `head+0x20`, not a chain (T14.2).
- T5.2 and T5.11 item 8: the dirty bits are set in `DAT_02edf140+0x208070` by `0x123f10` and `0x123db0`. `+0x218070`
  is the walker's copy (T14.3).
- T5.3: the parity is read as `*(*(ctx+0x10)+0xd70) & 1`, not `*(ctx+0xd70)`.
- T5.4: `out = rec+0x40` is an object built by `0x192de0`/`0x192f20` (vtable `PTR_FUN_01598930`), not a plain stream
  object. A[0] and A[1] are two objects (T14.5).
- T5.7: resolved. The draw reads the holder table through `0x10c2e0` (T14.5).
- T4.6: for a skinned record, `chunk[0] = P+0x90` is the per-parity holder table, not the mesh table.
- D1 section 5.3 (slot 10): for a skinned object, slot 10 is the other parity's POS (T14.5). Its use is still
  **inferred**.

### T14.10 Evidence (addresses and files)

- Palette: `out/re/decomp/0010/0010f760.c` (lines 1-20, 120, 150-212), `0012/00123330.c`, `0012/00125520.c`,
  `0012/001255d0.c`; disassembly `scratchpad/t4_disasm.txt` at `123330`-`1233f4` and `125520`-`1255bd`.
- Consumers and dirty set: `0049/004938e0.c`, `0049/00493a20.c`, `0053/00536050.c` (line 162), `0012/00123f10.c`,
  `0012/00123db0.c`, `0011/00116d80.c`, `0011/00118370.c`, `0011/00116e20.c`.
- Producer candidates: `0049/00493ad0.c`, `0049/00493bc0.c`, `0058/0058df60.c` (line 73), `0059/005902c0.c` (line 107).
- Skin output object: `0019/00192de0.c`, `0019/00192f20.c`, `0019/00193280.c`, `0019/001932e0.c`, `0019/00193f00.c`
  (object factory; `0x8be60` call shapes), `0019/00193560.c`, `0019/00193580.c`, `0012/00123da0.c`, `0008/0008be60.c`,
  `0008/0008c0d0.c`.
- Draw item: `0010/0010c2e0.c` (lines 15-51), `0010/0010c6c0.c`, `0012/00125500.c`, callers `0010/00103ad0.c` and
  `0010/001051f0.c`.
- Dispatch: `0019/00193810.c`, `0019/00193aa0.c`, `0012/00125520.c`.
