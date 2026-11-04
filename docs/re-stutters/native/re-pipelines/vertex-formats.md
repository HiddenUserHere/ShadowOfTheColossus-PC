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
