# 07. Mesh buffers, draw binding, dynamic and constant buffers

Read-only analysis of the SotC PS4 eboot (EU 1.01). Source: Ghidra pseudocode in `out/re/decomp/<4hex>/<8hex>.c`
and `tools/re/eboot_re.py` (callers, fstr). Addresses are eboot virtual addresses.

Labels: **O** = observed in decompiled code or an index result; **I** = inferred from offsets, masks or call
shape; **U** = unknown (not read or not resolved).

Scope: 0x1264d0 is the mesh deserializer. Two other builders (0x125ea0, 0x1260e0) also call 0x110ef0.
The registration at 0x127860 is taken from the brief and was not re-verified.

---

## 1. Mesh object (CMesh) layout

Allocated in 0x1264d0 as `FUN_00017070(0x130)` with ctor 0x125bd0 (vtable 0x1598110), or 0x178 bytes with ctor
0x128d80 (vtable 0x1598150) when the subtype byte is non-zero. **O**

Offsets are byte offsets from the mesh base.

| Offset | Content | Filled by | Label |
|---|---|---|---|
| +0x00 | vtable | ctor | O |
| +0x60 | resource handle (from the owner) | 0x125bd0 | O |
| +0x70 | layout / stream set, 0x2c8 bytes (section 3) | 0x110ef0 from 0x1264d0 | O |
| +0x80 | part table: 0x50-byte records (begin/end/cap at +0x80/+0x88/+0x90) | 0x128360, copy of stream table | O |
| +0xa0 | AABB min: x,y at +0xa0, z at +0xa8 | union over part records via 0x162210 | O (copy), I (meaning) |
| +0xac | AABB max: x,y at +0xac, z at +0xb4 | as above | O / I |
| +0xb8 | 0x40 object holding a vector of 0x24-byte records (count from stream) | 0x127d90 | O (alloc, read), U (meaning) |
| +0xe0 | u32 array, 2 entries per instance | 0x1264d0 bit 2 | O |
| +0x100 | per-instance, per-vertex blob (V#, stride 12, count = vertices x instances) | 0x8be60 | O |
| +0x128 | shadow index-list object, 0x68 bytes | 0x1264d0 bit 0x10 | O |

Part record (0x50 bytes), read by 0x1286b0 after a u32 count:

| Offset in record | Use | Label |
|---|---|---|
| +0x00..+0x0b | AABB min (floats) | I (min/max via vminps/vmaxps in 0x162210) |
| +0x0c..+0x17 | AABB max (floats) | I |
| +0x38 | dword passed to `FUN_00094bf0` in 0xdb390 (state or material id) | O (use), U (meaning) |
| +0x3c..+0x4b | dword pairs read by the bit-1 B-vector build | O (use), U (meaning) |

Part selection: 0xdb390 uses the dword at `param_5+0xc` as the part index and reads record `mesh+0x80 + 0x38 +
idx*0x50`. How the index is chosen (LOD or submesh) is **U**: `FUN_000e1ed0` and `FUN_000f8250` were not read.

---

## 2. Mesh file read order (0x1264d0)

Stream helpers: `0x8eae0` reads a u32, `0x8de50` reads n bytes, `0x8e740` reads a length-prefixed block,
`0x8e0d0` returns a pointer (zero-copy when contiguous, otherwise a copy from family 1). **O**

| Order | Read | Condition | Reader | Notes |
|---|---|---|---|---|
| 1 | u8 subtype | always | inline | selects 0x130 or 0x178 object |
| 2 | u32 flags F | always | `0x8eae0` | bits below |
| 3 | name/reference block | F&1 | `0x8e740`, `0xf4a80`, `0x14d880` | decode **U**; messages use "Mesh %s" with the name |
| 4 | u32 N, then N x 0x50 part records | F&2 | `0x1286b0` | copied to mesh+0x80 later |
| 5 | u32 A, then A records (0x28 bytes built from a global template plus 2 dwords) | F&2 | `0x162120`, `0x8eae0` | the 0x40 object built here is not stored on the mesh in this function; consumer **U** |
| 6 | u32 B, then B records of 3 dwords (first is the part index, the other two are read and not used) | F&2 | `0x8eae0` | **O** |
| 7 | u32 M, then M x 0x24 records | F&2 | `0x127d90` | to mesh+0xb8 |
| 8 | attribute table: u32 size and blob | F&2 | `0x155f40` | section 3 |
| 9 | index block: u32 type, u32 count, then count x (2 if type 1, 4 if type 0) bytes | F&2 | `0x154e00` | defect note below |
| 10 | layout object 0x2c8 from attribute table and index block | F&2 | `0x110ef0` | section 3 |
| 11 | u32 S, u32 n, u32 m, u32 (last one unused), arrays | F&4 | `0x8eae0`, `0x8de50`, `0x8be60` | instanced block: 2n u32 to +0xe0, blob of vertices x n x 12 bytes to +0x100 |
| 12 | u32 S, u32 n, u32 m, arrays; check n == 2*S*parts | F&0x10 | `0x8de50`, `0x8be60` | shadow index list, object at +0x128 (check form **I**) |

Index block defect (**O**, not exercised here): `0x154e00` allocates `count * iVar2` bytes, where iVar2 is 2 for
type 1, 4 for type 0 and 0 for any other type. It reads `count * 4` bytes for every type except 1. For types other
than 0 and 1 the allocation is zero and the read is not. Verify on real data before relying on those types.

Index element size for the buffer: 2 bytes (type 1, usage 0x18) or 4 bytes (type 0, usage 0x19). **O** (0x110ef0).

Strings in 0x1264d0 that confirm the roles (**O**): "lighting data is not the correct length", "is set to have
instanced data, but no instances are present", "shadow index list is not the expected length".

---

## 3. Vertex attributes and the layout object (0x155f40, 0x110ef0)

### 3a. Attribute table (0x155f40)
Reads a u32 size and a blob into a family-1 heap block, then rebases each entry's data pointer (file offset to
absolute). **O**

| Entry offset | Field | Label |
|---|---|---|
| +0x00 | FourCC semantic, for example `0x504f5330` ("POS0") | O |
| +0x04 | u16 format id (mapped by `DAT_00fa62b0`, then used with `DAT_00faa9f0` for element stride) | O |
| +0x06 | u16 flags, bit 1 cleared by the reader | O (meaning U) |
| +0x08 | data pointer (rebased) | O |
| +0x10 | byte size of the stream | O |

The entry count is the first dword; entries are 0x18 bytes starting at byte 8. Lookup is by FourCC in `0x155f00`.
Vertex count = size / stride (computed in 0x110ef0 and stored as the first dword of the layout). **O**

FourCC names used by 0x110ef0 (**O**): `POS0`, `COL0`, `COL1`, `COL2`, `TEX0`..`TEX3`, `NRM0`, `TAN0`, `BONI`
(blend indices), `BONW` (blend weights), `_SAT` (summed area table).

### 3b. Buffer slot (0x28 bytes, built by 0x8be60 and 0x8bd90)

| Slot field | Offset | Meaning |
|---|---|---|
| V# words 0-3 | +0x00..+0x0f | buffer V# |
| data pointer | +0x10 | address of the allocated copy |
| usage | +0x18 | usage flags (bit 0 selects the CPU family) |
| type | +0x1c | format enum (index into `DAT_00f9ebe0` for type < 0x1a) |
| size | +0x20 | bytes |
| GNM registration id | +0x24 | returned by `sceGnmRegisterResource`, -1 if none |

### 3c. Layout object (0x2c8 bytes, from 0x110ef0)

| Object offset | Slot | Name passed to 0x8be60 |
|---|---|---|
| +0x000 | vertex count (dword) | - |
| +0x008 | index buffer | none (usage 0x18 or 0x19) |
| +0x030 | attribute | kPos0 |
| +0x058 | attribute | kCol0 |
| +0x080 | attribute | kCol1 |
| +0x0a8 | attribute | kTex0 |
| +0x0d0 | attribute | kTex1 |
| +0x0f8 | attribute | kTex2 |
| +0x120 | attribute | kCol2 |
| +0x148 | attribute | kNrm0 |
| +0x170 | attribute | kTan0 |
| +0x198 | copy of kPos0 (words 0-1 from kPos0, words 2-3 from kPos0+0x38) | I: position-only copy |
| +0x1c0 | attribute | kTex3 |
| +0x1e8 | attribute | kBlendIndices |
| +0x210 | attribute | kBlendWeight |
| +0x238 | attribute | kSummedAreaTable |
| +0x260..+0x2bf | 12 pointers to the slots above (in the order of table 3d) | O |
| +0x2c0 | pointer to the 12-V# descriptor table (0xc0 bytes) | O |

A missing attribute gets the global default V# words from `DAT_02ee3ce0 + 0x3b8..0x3d8` instead of data. **O**

Every present attribute calls `0x8be60` with: the type from the attribute format, stride from `0x8c1d0(type)`,
count = vertex count, the data pointer, and the registration name. A non-null name triggers
`sceGnmRegisterResource`. **O**

### 3d. Descriptor table (0xc0 bytes, 12 V#s)
`FUN_00017140(0xc0, DAT_02dd9f84)` (GPU family, align 8) followed by 12 copies of 16 bytes. **O**

| Index | Source slot | Name |
|---|---|---|
| 0 | +0x008 | index buffer |
| 1 | +0x030 | kPos0 |
| 2 | +0x058 | kCol0 |
| 3 | +0x080 | kCol1 |
| 4 | +0x0a8 | kTex0 |
| 5 | +0x0d0 | kTex1 |
| 6 | +0x0f8 | kTex2 |
| 7 | +0x120 | kCol2 |
| 8 | +0x148 | kNrm0 |
| 9 | +0x170 | kTan0 |
| 10 | +0x198 | kPos0 copy |
| 11 | +0x1c0 | kTex3 |

BlendIndices, BlendWeight and SAT are not in this table (their use is **U**). The table is built once at load and
is not rewritten in this path (**I**).

---

## 4. Draw binding (0xdb390 and callees, per object)

Caller of 0xdb390: 0xdb700 (not read). **O** unless marked.

| Step | Call | Effect | Label |
|---|---|---|---|
| a | `FUN_000e1ed0(model, param_5+0xc)` | part or LOD lookup | O (call), U (logic) |
| b | `FUN_000f8250(lVar21, param_3+0xf4)` | gate; a type value of 4 adds state | O / U |
| c | `FUN_0016b8c0` ... `FUN_0016b900` | scope around the draw | O / U |
| d | `FUN_0008c0d0(param_1+0x248)`, then 16 qwords written | per-draw 0x80-byte block, family DAT_02ed1f54 | O |
| e | `*(param_3+0xa8) = block` | link | O |
| f | `*(param_3+0x90) = mesh_layout+0x2c0` | 12-V# descriptor table pointer into the draw state | O |
| g | `FUN_001122c0(layout, param_3)` -> `FUN_00094c60(*param_3, layout+0x8)` | index V# to draw state; this path leads to the INDEX_BASE 0x26 packet (0x86bf50; callers include 0x94c60) | O (chain), I (packet) |
| h | `FUN_00094bf0(param_2, part+0x38)` | per-part state | O |
| i | `FUN_00095320(*(lVar21+0x468), param_3+0x90, DAT_02ed1f54)` | allocates popcount(mask) x 8 bytes from family DAT_02ed1f54 and copies the selected qwords | O |
| j | `FUN_000ae150(ctx, list)` -> `FUN_00862490` | writes pointer user-data registers for entries of type 2 | O (0xae150), I (register effect) |
| k | `FUN_000db620(ctx, param_1+0x360, (param_6 + param_7*8)*0x20)` | virtual geometry draw, section 5b | O |

Where the V#s go: the 12-V# table pointer is placed in the draw state (f), and a user-data pointer list that
contains it is written to shader registers (i, j). The vertex fetch code reads V#s through that pointer (**I**; the
fetch shader itself was not read).

Index buffer, per draw (**O** unless marked):
- Path of 0xdb390: layout+0x8 V# -> 0x94c60 -> INDEX_BASE packet (0x86bf50, header 0xc0012600 per doc 03). **I**
  for the packet details, from doc 03.
- Path of 0xdb620: its own inline packet from 0x86ad40, header 0xC0021100, plus 0x2 when the low bit of the
  second argument is set. Four dwords: address low, address high (8 bits), 1, 0. The address comes from the
  caller's qword at `param_1+0x360`. The opcode 0x11 does not match the 0x26 INDEX_BASE of doc 03. Meaning **U**.
- `sceGnmSetIndexBuffer` is not called (no caller in `icallers`). **O**
- Element size: 2 bytes for type 1 (16-bit), 4 bytes for type 0 (32-bit). **O**

Draw calls: `0x86b3d0` wraps `sceGnmDrawIndexIndirect` (9 dwords). Its callers are 0xdb620, 0x1a6d20 and 0x1a7290,
the GPU-visibility consumers (doc 03). The argument buffer that holds the draw counts is not established in this
path: its producer is **U**. Non-virtual draws use the `sceGnmDrawIndex` and `sceGnmDrawIndexOffset` wrappers
(0x86aa41, 0x86b021). Their callers include 0x133550 (a pass with a 1-record V#, **O**) and 0x94d00 / 0x94dd0 (not read).

---

## 5. Dynamic buffers

### 5a. Per-instance data (0x118100 -> 0x123410)
- 0x118100 `UpdateGPUData_InstanceData` (name confirmed by string) is called from `UpdateSkinningRenderObjects`
  (0xcff13), which is called from `EndFrameViewParams` (0xcf710). It runs every frame on the CPU. **O**
- 0x123410 (1017 bytes) writes a 0x70-byte (28-dword) per-instance block into a fresh allocation from
  `FUN_0008c0d0(DAT_02ee3ce0+0x480)`, with family DAT_02ed1f54, on every call. A second block (up to dword 0x1b,
  about 0x6c bytes) goes to `DAT_02ee3ce0+0x458`. **O**
- Both blocks are bound by `FUN_000928f0` (section 6c). Two V#s per call:
  - fresh block, usage 0x10, per call
  - the instance's stable buffer (+0x90 with stride from +0x94 and count from +0x98; +0x68 for the second call),
    usage 0x6d. **O**
- The writer of the stable buffers is not in 0x123410. It may be the skinning output, but that is **U**
  (0x120130, 0x121ed0, 0x193e40, 0x18aa20 were not read).

### 5b. Virtual geometry (0xdb390, 0xdb620)
- 0xdb390 allocates a 0x80-byte block per draw from `param_1+0x248` (family DAT_02ed1f54) and fills it from a
  source record. It also builds the user-data pointer list (section 4 step i). **O**
- The CPU writes these blocks for each visible object. The callers of 0xdb620 are 0x189060, 0x18d6f0, 0x10b0a0
  and 0xdb390. **O** (callers), **I** (the blocks are per frame).
- The producer of the indirect argument buffer is **U**. The GPU visibility mask (XPR, `0x1a6d20`) is described in
  doc 03 section 5. The link from that mask to the argument buffer is not established.

### 5c. Particles and grass
- Particle records are written by 0x101720 (doc 03, observed). Not read here: buffer layout and allocator **U**.
- Grass or other virtual geometry beyond 0xdb390: **U**.

---

## 6. Constant buffers and V# builders

### 6a. Allocator (0x12820)
`FUN_00012820(size, family, align, flags)` uses the per-family heap object stored at `DAT_02dfaab0 + family*8`,
created on first use by 0x13380. It calls vtable slot +0xa0 to allocate, +0x98 to probe, and falls back to 0x124f0. **O**

The family globals (DAT_02dd9f80, DAT_02dd9f84, DAT_02ed1f54, DAT_02ed1f60, DAT_02dd9fa8) are not in the file image:
their numeric values are set at run time and are **U**. Roles inferred from use:

| Global | Users (examples) | Role | Label |
|---|---|---|---|
| DAT_02dd9f84 | 0x96680 (texture object), 0x110ef0 (descriptor table), 0x8be60 with usage bit 0 clear | GPU-visible persistent data | I |
| DAT_02dd9f80 | 0x8be60 with usage bit 0 set, particle textures | CPU-side copy | I |
| DAT_02ed1f54 | about 113 functions, including 0x8c0d0, 0x95320, 0x928f0, 0xdb390 | per-call transient memory | I |
| DAT_02ed1f60 | 0x12950 (texture streaming pool) | streaming pool | O (doc 02) |

Lifetime of DAT_02ed1f54 allocations: no explicit free appears in these paths. Reset or ring semantics are **U**.

### 6b. Buffer and constant V# builders

| Function | Output | Label |
|---|---|---|
| 0x8652d0 | generic buffer V#: word0 base low; word1 base high (12 bits) and stride (bits 16-29); word2 count; word3 from the format word, with bit 29 set | O |
| 0x865360(v#, usage) | word1 bits 12-15 and word3 bits 26-28 from usage | O / I |
| 0x865410(v#, base, fmt, count) | buffer V#; 0x928f0 calls it with format 0x4404 | O |
| 0x865500 | V# with format from FUN_008666d0(4,7,4,0,0,1) | O / U |
| 0x865600 | V# with word3 0x2000c004 and count = count | O |
| 0x8655d0 | V# with word3 0x20077fac, count = (size + 15) >> 4, word1 bit 20 set | O; I: constant-buffer V# (16-byte units) |

Type codes 0x1a, 0x1b and 0x1c use the three special builders (0x865600, 0x865500, 0x8655d0). Other types use the
format word table `DAT_00f9ebe0` (26 entries, for example 0x4404, 0x4402, 0x22c70b). The element size table is
`DAT_00f9ec50` (27 entries): `0x8c1d0(6)` = 12, `0x8c1d0(0x18)` = 2, `0x8c1d0(0x19)` = 4. **O**

### 6c. Per-object and per-view constants
- Per-view constants: `SetViewParams` (0xbe1f0) and `EndFrameViewParams` (doc 03; not re-read). **O** (doc 03).
- Per-draw constants: the 0x123410 blocks (section 5a), bound by 0x928f0. `0x928f0` (511 bytes) creates three V#s
  and binds them through `FUN_00861ce0`, `FUN_00861e60` and `FUN_008619a0`:
  - slot 0: fresh block, usage 0x10
  - slot 1: stable instance buffer, usage 0x6d
  - slot 2: an 8-byte block holding (count, count), from `FUN_00012820(8, DAT_02ed1f54, 4)`, word3 0x20077fac
  **O**. The user-data base is taken from `*(render object + 0x60)` (`FUN_0014d7b0`). **O**
- Constant buffer lifetime: no per-object free path found. **U**.
- Material constants: 0x117160 (`updateDynamicGPUDataJob`, doc 03). **U** here.

---

## 7. Answers to the brief

| Question | Answer | Label |
|---|---|---|
| 1. Mesh object layout | Mesh object of 0x130 or 0x178 bytes. Layout object (0x2c8) with one 0x28-byte buffer slot per vertex attribute, the index buffer, and a 0xc0 table of 12 V#s. Part records of 0x50 bytes with an AABB and a state id. | O |
| 1b. Vertex data source | CPU arrays in the mesh file, rebased by 0x155f40. Each attribute gets its own copy in a GPU-family allocation and a V#. No sceGnm create-buffer call. | O |
| 1c. LODs | Part index at `param_5+0xc`, and a 0x24-record vector at mesh+0xb8. LOD selection not read. | U |
| 2. Draw binding | 12-V# table pointer in the draw state; user-data pointer list (0x95320, 0xae150). Index V# via 0x94c60 (INDEX_BASE packet), or an inline packet in 0xdb620. Virtual geometry draws use DrawIndexIndirect (0x86b3d0). | O / I |
| 3. Dynamic buffers | Instance blocks written by the CPU every frame (0x118100 -> 0x123410). Virtual geometry per-draw block by the CPU (0xdb390). Visibility mask written by the GPU and read by the CPU (doc 03). Skinning output writer: U. Particles: U. | O / U |
| 4. Constant buffers | Per-call blocks from family DAT_02ed1f54 (0x8c0d0), bound with V#s from 0x928f0 and builders 0x8655d0 and 0x8652d0. Lifetime: U. | O / I / U |

---

## 8. Open items

1. Decode the bit-1 A and B vectors and the 0x40 object, whose consumer is not visible in 0x1264d0. Check 0x162120 and 0x158a50 for the A-record size.
2. Read 0x1a6d20 and 0x1a8c60 to see how the index V# and the indirect argument buffer are chosen per visible object.
3. Read 0x120130, 0x121ed0, 0x193e40 and 0x18aa20 (callees of 0x118100) to find the skinning and palette writers.
4. Resolve the run-time values and reset semantics of DAT_02ed1f54 (the allocator object from 0x13380).
5. Read 0xe1ed0 and 0xf8250 for part and LOD selection in 0xdb390.
6. Decode the opcode 0x11 packet written by 0x86ad40 against the PM4 table in doc 03.
7. Check the index block defect in 0x154e00 (section 2) against real files before relying on index types other than 0 and 1.
8. Not covered: 0x117930 (streaming GPU step) and 0x1478d0 (DeleteResource), which are open items in doc 02.
