# Texture descriptors: how a T# reaches the shader, and what the port key can know at material load (RE, topic D3)

Read-only static analysis of the EU 1.01 eboot. Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`
(first line of each file lists callers), `docs/re-backend/02-resource-objects.md`, `docs/re-backend/06-draw-descriptors.md`,
`materials-meshes.md` (R3), `slot-stages.md` (K2), `pass-map.md` (R8), `pass-targets.md` (K1/K5), `key-fields.md` (K3),
the vendored shadPS4 header `gpu/shadps4/video_core/amdgpu/resource.h`, the port's `gpu/shadps4/shader_recompiler/specialization.h`,
`gpu/sotc/pretranslate.cpp`, and the cold-run log `user/gcn-cold5/cold_w2.log`. No game dump was read; no game was run.
Addresses are eboot VAs. Labels: **observed** (decomp or source line, address given), **inferred**, **unknown**.

## 1. Answer in short

- **The texture T# is a 32-byte object inside the CTexture (`0x1c0` bytes, vtable `0x15977d0`) at `CTexture+0xc4`.**
  It is read raw from the `.ctxr` stream (`0x8de50(stream, obj+0xc4, 0x20)` in the build `0x96680`). Only its base
  address (dword 0) is rewritten at residency (`0x881e60`). **observed** (doc 02 section 3; `0x96680` callers).
- **Materials (.cmat) do not hold texture T#s, and the R3 "texture slot" is a shader slot.** The 0x48-stride
  array at `material+0x60` is the stage-program slot array (K2): `B+0x00` is a `.csdr` (Shader) reference,
  `B+0x10..+0x30` are pointers to 32-byte records of that Shader (`Shader+0x60` = record array), and `B+0x38` is the
  stage mask 17/23/25/31. R3's "view table at payload+0x60" is `Shader+0x60`, the record array, not a texture view
  table. R3's `viewMask` bits 0..4 select stage records, not texture views. **proven** (`0xf6c90` lines 126-241,
  `0xf8990`, `0xf8aa0`; see K2 section 2). The contradiction is settled in favour of K2.
- **Where a material's texture references are is still open.** The parser reads no T# and no texture slot. The only
  reference lists in the .cmat are the two path lists at `+0x490` and `+0x4d0` (16-byte `{res, owned}`, counts from
  file dwords 8 and 9). They are loaded by path, and their type comes from the path, not from the list
  (section 3.3). **proven** (absence), **unknown** (which type they hold).
- **At draw time, a texture T# reaches the shader by a copy, from engine code, not from the material.** A caller
  with a CTexture pointer calls a setter (`0x861fe0` for T#, slot table at layout `+0x22`; `0x862160` for T#,
  slot table at `+0x42`). The setter copies the 32 bytes into a per-stage dword array (`S+0x90 + stage*0x1800`)
  or writes them straight to user SGPRs. The per-draw flush `0x85f410` copies the dirty array into a bump chunk
  and writes the chunk base to SGPRs. **observed** (`0x861fe0` decomp; doc 06 sections 2-3). The two callers
  observed that do this are `0x133550` (from `0x187ce0`, first slot 7) and `0xa8c50` (from `0xa6260`, fixed engine
  program at shader-list entry `0xb70`).
- **Which image index a T# goes to** is the `first_slot` argument of the setter, interpreted through the stage's
  layout table (section 4.3). The layout is inside the `.csdr` record (`*rec + 8`). The caller that knows the
  texture is the pass code, not the material. **observed** (setter and caller), **inferred** (the index space).
- **Per-pass global textures** (shadow maps, depth, G-buffer inputs) come from pass code through the same setters
  (`0x133550` takes its CTexture from a caller argument). Their formats are in `pass-targets.md` section 3. **observed**
  path; the list of which pass binds which texture is **unknown**.
- **For the port key:** the image fields of `ImageSpecialization` are type, is_integer, is_storage, dst_select
  (storage only), num_conversion, num_bindings, and is_srgb (forced false by default). is_storage and num_bindings
  come from the shader. The rest come from T# words 1 and 3, which are constant per texture. They can be read from
  `CTexture+0xc4` at texture load. What cannot be known at material load is the mapping from a shader image index to
  a CTexture. So the image part of a prebuilt key is known per texture and per bind site, not per material. Section 6.

## 2. The texture object and its T#

Source: doc 02 section 3 (observed), re-checked here.

| Step | Address | Effect (observed) |
|---|---|---|
| Object alloc, 0x1c0 bytes | `0x17070` via factory `Texture`, tag `0x63747872` (`ctxr`) | CTexture, vtable `0x15977d0` (`particles-data.md` 2b; `0x98aa0` build) |
| Read T# | `0x8de50(stream, obj+0xc4, 0x20)` in `0x96680` | 32 bytes raw from file |
| Mip table | `0x8ea20`, `0x8eae0`, `0x8e410`, `0x99140` | vector at `+0x80`/`+0x88` |
| Data pointer | `0x12820` or `0x12950` | data at `+0xb8` |
| Flag bits in T# | `0x880c30`, `0x8829e0` in `0x96680` | memory-type bits only (section 2.1) |
| View blocks | `0x87e5f0`, `0x87dea0` | 32-byte blocks at `+0xe4`, `+0x124` (doc 02; **conflict**: `pass-targets.md` section 4 calls `+0xe4` a 64-byte colour descriptor for render targets; not resolved) |
| Base address | `0x881e60` | writes base >> 8 into dword 0 on residency (doc 02) |
| Dtor | `0x985d0` | releases views, `sceGnmUnregisterResource` if registered |

Bind-time readers of the T#: `0x133550` (`CTexture+0xc4..0xdc`, 32 bytes) and `0xa8c50` (same 32 bytes). **observed**.

### 2.1 Field layout of the T# (vendored header, cross-checked)

Source: `gpu/shadps4/video_core/amdgpu/resource.h` lines 143-185, `struct Image`. Bit positions below were computed
from the declaration order (64-bit units packed in order). **inferred** from the header; **cross-checked** because the
flag writer `0x8829e0` touches exactly the fields this layout puts at dword 1 bits 6-7 and 30-31 and dword 3 bit 26.

```c
// T# (Image), 8 dwords. Positions are dword:bit ranges, LSB first.
struct TImage {
  u32 w0;   // [31:0]   base address (256-byte units, low 32 bits of base>>8)     rewritten at residency (0x881e60)
  u32 w1;   // [5:0]    base address high bits
            // [7:6]    mtype_l2          <- 0x8829e0 flags[1:0]   (memory type, not format)
            // [19:8]   min_lod
            // [25:20]  data_format       <- key (num_conversion, nothing else)
            // [29:26]  num_format        <- key (is_integer, is_srgb, num_conversion)
            // [31:30]  mtype             <- 0x8829e0 flags[3:2]   (memory type, not format)
  u32 w2;   // [13:0]   width
            // [27:14]  height
            // [30:28]  perf_modulation
            // [31]     interlaced
  u32 w3;   // [2:0]    dst_sel_x  \
            // [5:3]    dst_sel_y   |  <- key (storage images only)
            // [8:6]    dst_sel_z   |
            // [11:9]   dst_sel_w  /
            // [15:12]  base_level
            // [19:16]  last_level
            // [24:20]  tiling_index
            // [25]     pow2pad
            // [26]     mtype2            <- 0x8829e0 flags[4]
            // [27]     atc
            // [31:28]  type              <- key (view type, via GetViewType(is_array))
  u32 w4;   // depth [12:0], pitch [26:13]
  u32 w5;   // base_array [12:0], last_array [25:13]
  u32 w6, w7; // not in the key (min_lod_warn, counter bank, compression, ...)
};
```

Consequences (**observed** for the code, **inferred** for the bit positions):

- `0x8829e0(&T#, flags)` writes dword 1 bits 6-7 and 30-31 and dword 3 bit 26. These are memory-type fields. They do
  not touch data format, number format, dst_sel or type. So the 0x10 flag of `0x133550` and the 0x6d flag of `0xa8c50`
  change only MTYPE, which is not in the port key. **observed** (`0x8829e0` decomp; `0x133550` line 30-40).
- Keyed fields (type, data_format, num_format, dst_sel) are never rewritten after the `0x96680` read, except dword 0 at
  residency. So they are constant per texture. **inferred from the writers found** (`0x881e60`, `0x8829e0`). No other
  writer of `+0xc4` was found.
- sRGB is not a separate T# bit. It is `num_format == 9` (`AmdGpu::NumberFormat::Srgb`, `pixel_format.h` line 85).
  **observed** in the header; the cold log shows `number_format=9` as `R8Srgb` (`cold_w2.log` line 25).

## 3. The material side: what the .cmat holds (settles R3 against K2)

### 3.1 The slot array is the shader slot array

From `0xf6c90` (**observed**, lines 126-241, decomp `000f/000f6c90.c`; K2 section 2):

```c
struct MatSlot {                 // B = material + 0x60 + 0x48*k
  u64  res;          // +0x00  .csdr (Shader) reference, FUN_0014c3f0 / FUN_000fa1a0
  u8   owned;        // +0x08
  u64  rec[5];       // +0x10..+0x30  Shader.records + 32*popcount(mask & ((1<<i)-1)) for bit i of mask
  u32  mask;         // +0x38  17 / 23 / 25 / 31 (other values -> 17), accepted set 0x82820000
  u64  interned;     // +0x40  FUN_00099720 when mask & 6
};
```

- `Shader+0x60` is the record array (`FUN_0014d7b0(res)`, line 205). Each record is 32 bytes: `+0x00` pointer to the
  file record, `+0x08` blob header pointer (`rec[1]` in the decomp), `+0x10` program bytes. **observed** (K2).
- So R3's "view table at payload+0x60", "view 0 at +0x70 ... view 4 at +0x90", and the "32-byte descriptors" are
  these shader records, read through the material's slot pointers. R3's texture view mask is the stage mask. The
  pass resolver `0xf8990` reads `B+0x10` (record 0) and `B+0x30` (record at bit 4) only. **observed**.
- Note on coordinates: in the decomp the setters take `param_1 = O = cursor+0x2e8` (doc 06 section 0). Fields written
  as `ctx+0xc0b0` in R3 and K2 are `O+0xc0b0`. The stage arrays are `O+0x90` (not `ctx+0x90`). The brief's
  `ctx+0x3090` is `O+0x3090 = cursor+0x3378`. **observed**.

### 3.2 Where a material's texture references are not

- No T# words, no texture count and no texture-slot index are decoded in `0xf6c90`. **observed** (the parser's
  stream order in R3 section 3 has no texture record; K2 section 2 agrees).
- The per-stage dword arrays are filled only by setters (section 4), not by the material parser. **observed** (no
  write to `O+0x90..` in `0xf6c90`; the writers are `0x8619a0`, `0x861ce0`, `0x861b40`, `0x861e60`, `0x861fe0`,
  `0x862160`, `0x8622e0`, `0x8611a0`). **observed** (doc 06 section 2).

### 3.3 The two path lists `+0x490` and `+0x4d0`

- Count from file dword 8 (list 1) and dword 9 (list 2), entries `{void* res; u8 owned}` (16 bytes). Each entry is read
  as a path (`FUN_0014c3f0`), then `FUN_000f80f0` loads it through the manager at `DAT_02ee3ce0+0x340`. **observed**
  (`0xf6c90` lines 255-260, 329-334).
- The `0xd` passed to `FUN_0014cf20` is a **priority**, not a type: `0x14cf20` returns early when the ref's count is
  nonzero and `ref+0x98 <= 0xd` (`0014/0014cf20.c`). **observed**. So the list does not say its resource type.
- The resource type is set by the path extension through the factory table (`particles-data.md` 2b): `.ctxr` for a
  texture, `.cslt` for a shader list, `.csdr` for a shader. **observed** (table). Which of these the lists hold is
  **unknown**. `materials-meshes.md` called them shader collections (inferred). Reading one real `.cmat` settles it
  (section 8, item 1).
- These lists are not on the draw path found: `0xf8990` does not read them (K2 section 6; R8 section 1). **proven**
  for `0xf8990`. The only readers of `+0x490` / `+0x4d0` that were searched are in the parser and the variant copy.

### 3.4 The two maps `+0x4f0` and `+0x528`

Keyed by a u32 from the file, not read on the draw path (R8 section 1). They are not texture maps either. **proven**
(absence of readers in the decomp folders searched).

## 4. The bind path: from a texture to the shader's user SGPRs

### 4.1 Per-stage state (observed, doc 06 section 2)

| Item | Address (field of `O`, `O = cursor+0x2e8`) |
|---|---|
| Stage dword array | `O+0x90 + stage*0x1800`: CS `+0x90`, PS `+0x1890`, VS `+0x3090`; 0x600 dwords each |
| Layout pointer per stage | CS `O+0xc180`, PS `O+0xc188`, VS `O+0xc190` (set by `0x861520`, `0x861600`, `0x8616f0`) |
| Mirror layout | `O+0xc1b8` (set by `0x861730`, `0x861880`; used for a second stream on cursor `O+0xc098`) |
| Shader record per stage | CS `O+0xc0a0`, PS `O+0xc0a8`, VS `O+0xc0b0`, ES `O+0xc0c0`, LS `O+0xc0d0` |
| Writer cursor | `O+0xc090` (zeroed in `0x85ed20`; runtime writer unknown, doc 06 section 7 item 3) |
| Dirty bytes | `O+0xc250 + stage` (set by every setter, cleared by `0x85f410`) |

The layout for a stage is `*rec + 8`, where `rec` is the stage's record (`0x937c0` line 11, `0x94ab0` line 18, both
pass `*param_2 + 8`). So the layout is inside the `.csdr` file record of that program, not in the material and not in
the texture. **observed** (decomp call arguments). The R8/K2 statement "PS layout = `rec4+8`" is a slip: the call passes
`*rec4 + 8`. **observed**.

### 4.2 The setters that write a texture or resource

Setter signature (**observed**, `0x861fe0` decomp line 1): `(O, stage, first_slot, count, data)`. For each slot `i` in
`[first_slot, first_slot+count)`:

```c
u16 e = *(u16*)(layout[stage] + TABLE + 2*i);     // TABLE per setter, below
if (e != 0xffff) {
  if ((s16)e < 0)  write_sgpr_direct(cursor, e & 0x3fff, data, size);    // direct user SGPR write (0x86d5e0)
  else             memcpy(O + 0x90 + stage*0x1800 + (e & 0x3fff)*4, data, size);  // into the stage array
}
```

| Setter | Entry size | `TABLE` (layout offset) | Used for (observed caller) |
|---|---|---|---|
| `0x861fe0` | 32 B (T#) | `0x22` | `0x133550` (`first_slot` 7, count 1, data = CTexture `+0xc4` after `0x8829e0`) |
| `0x862160` | 32 B (T#) | `0x42` | `0xa8c50` (data = CTexture `+0xc4` after `0x8829e0(…, 0x6d)`) |
| `0x8619a0` | 16 B (V#) | `0x82` | `0xde9d0`, `0xa8c50` (via the `0x8c0d0` allocation) |
| `0x861ce0` | 16 B (V#) | `0x22` | `0xde9d0` (`R+0x130`, `R+0x2e8`) |
| `0x861e60` | 16 B (V#) | `0x42` | `0xde9d0` (`R+0x338`) |
| `0x861b40` | 16 B | `0xaa` | not sampled |
| `0x8622e0` | 16 B | `0x62` | `0x133550` (`0x8622e0(…, 0, 1, R[0x330]+0x10)`) |
| `0x8611a0` | 16 B | `0xca` (VS only) | no caller found (doc 06 section 7 item 1) |
| `0x862490` | direct | `0x10` | writes PM4 at once (`0xaf610`, `0xae150`, `0xb9710`) |

Points to note:

- `TABLE` `0x22` is read by both the 16-byte and the 32-byte setters (`0x861ce0` and `0x861fe0`). So the table is one
  index space for the stage, and the caller's `first_slot` picks the entry. **inferred** (doc 06 section 2.2 says the
  element size is chosen by the caller).
- The T# setter writes 32 bytes as four `u64` (`0x861fe0` lines 27-31). A direct entry writes 8 bytes (`0x86d5e0`
  count 8). **observed**.

### 4.3 Order: which texture goes to which shader index

For a draw of program `P` in stage `st`:

1. Shader index `i` for an image is the `first_slot` the caller passes. For the `0x133550` path, `i = 7`.
2. The entry `layout[st][0x22 + 2*i]` is the dword offset of that T# in the stage array (or a direct SGPR if the high bit
   is set). **observed** (setter code).
3. `0x85f410` copies `count` dwords (`layout[st][0]`, u16) from `O+0x3090` (VS) or `O+0x1890` (PS) into a bump chunk
   (`0x8643f0`) and writes the chunk base and sub-pointers to SGPRs through the slots in the layout (`L+0x07`,
   `L+0x08/+0x09`, `L+0x0a/+0x0b`, `L+0x0c/+0x0d`, `L+0x0e`, `L+0x04/+0x05/+0x06`, `L+0x14`; doc 06 section 2.1).
   The shader reads its T# with `s_load` from `base + offset`. **observed** (`0x85f410`), **inferred** (shader side).
4. So the mapping is: `CTexture -> (caller's setter, first_slot i)` -> `layout[0x22+2i]` -> dword offset in the chunk
   -> shader `s_load`. The first two links are in the caller; the last two are in the `.csdr` layout. The port's image
   index is a different numbering (`info.images`, sorted by the translator), so the port must map through the
   resource identity, not by position. **inferred**.

### 4.4 Global textures per pass

- Pass code calls the setter with a texture object that it owns. The observed chain is `0x187ce0` (callers `0xcaed0`,
  which writes k = 1 per `pass-map.md` section 2.1, and `0xc4210`) to `0x133550`. The pass runner of `0xc4210` is not identified.
  Inside `0x133550`, `param_4` is the texture (`*param_4` is the CTexture whose `+0xc4` is copied). **observed**.
- Render targets are CTexture-class objects too: the output object of `pass-targets.md` section 6 has vtable
  `PTR_FUN_015977d0` (= the CTexture vtable). So a G-buffer or depth target that a later pass samples is a CTexture,
  and its T# is the one at `+0xc4` of that object. **observed** (vtable), **inferred** that `+0xc4` is written for
  render targets (no writer found for a target created without a `.ctxr`; doc 02 does not cover it).
- Per-pass globals with the formats from `pass-targets.md` section 3 (the format table `DAT_02ed3680`; ids are the `p6`
  value there):
  - depth sampled as textures: `MainDepth`, `MainDepth_HalfRes`, `OccluderDepth`, `CascadedShadow`, `SpotShadowArray`
    (id 9, 32 FLOAT); `LinearDepthMipped`, `LinearNearDepth` (id 3, 32 FLOAT); `RainBlocker`, `ParticleBlocker` (id 10,
    16 FLOAT);
  - G-buffer inputs: `Normal_Roughness`, `Albedo_AmbientOcclusion`, `PBRProperties`, `ExtraPBRProperties`, `Ambient1`
    (id 4, 32 UINT); `MainColor` and lighting (id 1, 16 FLOAT);
  - shadow and AO: `SunShadowResolve`, `SSAO` (id 0, 8_8_8_8 UNORM); `AmbientShadowRG_SSAOB` (id 0x13, 2_10_10_10 UNORM).
  - `pass-targets.md` section 3 says these ids are decoded by the author with the low byte as the AMD DataFormat and bits
    8..11 as NumberFormat; the rest unresolved. Use them as a guide, not as proof. **inferred**.
- Which pass binds which of these as a T#, and at which image index, is **unknown** (no table found). It is the pass
  runner's business (`0x187ce0` and the other pass runners).

## 5. Pass-specific and draw-time variation

| Question | Finding | Label |
|---|---|---|
| Does the material change the T# words? | No writer of the T# words in the material parser. The per-texture T# is the same for every material that binds it. | observed (writers) |
| Is the shader image index fixed per material? | Not from the material. It is fixed per program (layout table) and per caller. | inferred |
| Is the bind per pass? | Yes, the pass runner chooses the setter and the CTexture. | observed (chain) / inferred |
| Is the base address per frame? | Only on residency change (`0x881e60`). | observed (doc 02) |
| Is there a shadow-map variant? | The shadow texture is a pass target (`CascadedShadow`, `SpotShadowArray`); its format is fixed (id 9). | observed (pass-targets) |

## 6. Port side: the image part of the key

### 6.1 Fields (from `specialization.h`, observed)

`FillImage` (lines 142-154) and `ImageSpecialization` (lines 48-59):

| Field | Source | T# words used | Constant per texture? | In the default config |
|---|---|---|---|---|
| `type` | `sharp.GetViewType(desc.is_array)` | w3[31:28] (`GetType`) | yes | keyed |
| `is_integer` | `IsInteger(num_format)`, num_format 4 or 5 | w1[29:26] | yes | keyed |
| `is_storage` | `desc.is_written` (shader) | none | n/a | keyed |
| `is_srgb` | `keep_srgb && num_format == 9` (non-storage only) | w1[29:26] | yes | **false** unless a sampler of the stage has `force_degamma` (`KeepSrgb`, line 117-150; SOTC_IMAGE_SRGB_NORMALIZE on in `sotc.ini`) |
| `dst_select` | `sharp.DstSelect()`, storage only | w3[11:0] | yes | keyed for storage only |
| `num_conversion` | `GetNumberConversion()` = `MapNumberConversion(num_format, data_format)` | w1[29:20] | yes | keyed |
| `num_bindings` | `desc.NumBindings(info)` (shader array size) | none | n/a | keyed |
| present bit | `GetSharp(info) != null` | the T# pointer in user SGPRs | per draw | keyed |

Excluded: base address, MTYPE bits, width/height, base/last level and array range. Those are not in the image part
(the bitset is per binding; a T# is not hashed as a whole). **observed** (`FillImage`).

Note the `GetViewType` table in `resource.h` lines 306-318: `Color1DArray` maps to `Color1D` when `!is_array`,
`Color2DArray` to `Color2D`, `Color2DMsaaArray` to `Color2DMsaa`. Cube (11) is not folded. So a cube T# keys as
`Cube` (type 11), not as 2D array. **observed**.

### 6.2 What the cold run shows (observed)

The compute prebuild counter line (`cold_w2.log` line 1166, the first one; the last at line 1740):

```
misses by key part: buffers 26 images 40 (img type 8 integer 18 swizzle 33 bindings 1 storage) 1
... (line 1740) buffers 27 images 42 (img type 8 integer 19 swizzle 35 bindings 1 storage) 1
```

- `img type 8`: the view type differs (`GetViewType` result).
- `integer 18-19`: `is_integer` differs. The image is integer in one key and float in the other.
- `swizzle 33-35`: `dst_select` differs. `dst_select` is keyed only for storage images, so these are all storage
  (`s1`) images.
- `bindings 1`, `storage 1`: one shader with a different `num_bindings` and one storage flag.

The per-image miss texts (`LogMiss`, `pretranslate.cpp` lines 306-345) confirm the storage case. Examples from the log:

```
img1 [t9 i0 s1 sw4567 n1]   (7 times)   -> Color2D, storage, dst_sel R,G,B,A
img1 [t9 i0 s1 sw4444 n1]   (6 times)   -> Color2D, storage, dst_sel R,R,R,R
img0 [t9 i1 s0 sw0000 n1]   (5 times)   -> Color2D, integer, sampled
img0 [t13 i1 s1 sw4001 n1]  (2 times)   -> Color2DArray, integer, storage
```

The miss text prints `t<type> i<is_integer> s<is_storage> sw<dst_sel> n<num_bindings>`. Pairs that differ only in `sw`
show that the prebuilt storage key used synthetic sharps, not the real one. **observed** (log). Cause: the prebuild
uses synthetic sharps (`pretranslate.h` lines 1-10, 150-160), and a per-program hint file learns the real T# fields at a
first dispatch (`user/shader_predict/compute_hints.tsv`; `SOTC_PRETRANSLATE_COMPUTE_HINTS`). **observed** (source).
The count of misses grows with the number of first dispatches (382 to 413 in the log). **observed**.

### 6.3 Can the image key be read at material load?

- **Per texture: yes.** Type, data_format, num_format, dst_sel and the sRGB word are in `CTexture+0xc4` after the
  `.ctxr` read (`0x96680`), and they do not change afterwards (section 2.1). So a per-texture key can be built at
  texture load, from the object, without a draw. **inferred** (no other writer found), **observed** (the words).
- **Per material: no.** A material does not name its textures in a decoded field (section 3). The mapping from a
  shader image index to a CTexture is made by the pass caller at bind time (section 4.3). So the port cannot know,
  at material load, which T# a program's image `i` will get. What it can know is the program's own image list
  (`info.images`, `num_bindings`, `is_written`) from the shader, and the stage layout table (`layout[0x22+2i]`).
- **Per program and pass: partly.** The program's `is_written`, `is_array` and `num_bindings` are known from the
  shader. The `type` and number format are known only with the texture. The pass runner's bind sets are known when
  the pass runner is identified (`pass-map.md` section 2.1 names). This is the step that would make a prebuild
  possible: record, per (program, image index), the key fields seen at the first draw (the same hint idea as
  `compute_hints.tsv`, for graphics). **proposal, not implemented**.

## 7. Run-time log spec (to confirm the inferred links)

Each item names the address, the point, and the fields. Use an entry hook (no change to draws), log once per
(address, arguments) to keep the volume down.

1. **Setter arguments.** At entry of `0x861fe0`, `0x862160`, `0x8619a0`, `0x861ce0`: log `caller return address`,
   `stage` (`param_2`), `first_slot` (`param_3`), `count` (`param_4`), and the 8 dwords at `param_5` (T#) or 4 dwords
   (V#). Confirms the stage index and the first-slot numbers per caller.
2. **Layout entries.** At the same entries, log `layout = *(O + 0xc180 + stage*8)` and the u16 at `layout + 0x22 + 2*i`
   (or `+0x42`). Confirms the entry-to-offset rule (section 4.2).
3. **Texture identity.** At `0x133550` entry and before `0x861fe0`: log the CTexture pointer (`param_4` deref) and the
   `CTexture+0xc4..0xdc` words after `0x8829e0`. Log the same at the end of `0x96680` with the texture's source path
   (the `.ctxr` name passed to the resource manager). This links a CTexture to its file name.
4. **Material list types.** At the end of `0xf6c90` (the return, label `0xf80bc`), for each material: log the path
   strings of `+0x490` and `+0x4d0` (the `FUN_0014c3f0` argument) and their extensions. Settles section 3.3 in one run.
   Use the material name too (the `FUN_0014c3f0` argument of the material itself, if it is available).
5. **Pass bind of a texture.** At `0x187ce0` entry, log the pass (`k = *(pass+0xf4)` as in `pass-map.md`) and the
   texture pointers it passes to `0x133550`. Maps shadow and depth textures to pass names.
6. **Per draw image key.** At `0x85f410` (the VS and PS blocks), log `count`, the chunk base, and for each image slot the
   dword values read from the chunk. Compare with the key the port computes from the same words (port log of
   `FillImage`). Confirms that the port sees the words the engine wrote.
7. **Mapping check for the port.** Log, per (program hash, image index), the CTexture pointer, its `.ctxr` name, and the
   key fields. A run over one area gives the table that section 6.3 proposes.

## 8. Open items

1. Read one real `.cmat` (from the user's dump, not in the repo) to see the content of `+0x490` and `+0x4d0`: their
   extensions decide whether any texture reference is in the material. Item 4 above does this without reading the
   file by hand.
2. Find the pass runner that calls `0x187ce0` for each pass, and the argument order of `0x133550`'s `param_4`
   (CTexture or a pass-owned object). **open**.
3. Find the writer of `O+0xc090` (the cursor) and of the stage layouts `O+0xc340`, `O+0xc268`, `O+0xc418`, `O+0xc4f0`
   (doc 06 section 7 items 3 and 5).
4. Settle the `+0xe4` conflict: 32-byte view block (doc 02) or 64-byte colour descriptor (`pass-targets.md` section 4).
5. Check that `0x881e60` rewrites only dword 0 (doc 02). The base address field is 38 bits wide (`resource.h`, `base_address : 38`),
   so the high bits sit in dword 1 bits 0-5; if the engine writes them elsewhere, the T# base would be wrong for large
   addresses. **inferred**, not checked.
6. The bank swizzle of the T# depends on `base_address` bits (`resource.h` `GetBankSwizzle`, lines 290-300). It is not
   in the key, so it does not split keys, but a different base can give a different tiled layout. Not checked here.
7. `0x8611a0` has no caller found (doc 06 item 1). It would be the only VS path for 16-byte resources with a table at
   `+0xca`.

## 9. Evidence list

| Item | Address or file |
|---|---|
| CTexture T# read | `0x96680`, `0x8de50` (doc 02 section 3) |
| T# flag writer | `0x8829e0` (`out/re/decomp/0088/008829e0.c`) |
| Texture bind readers | `0x133550` (`0013/00133550.c`), `0xa8c50` (`000a/000a8c50.c`) |
| Texture bind callers | `0x187ce0` (`0018/00187ce0.c`, called by `0xcaed0`, `0xc4210`); `0xa6260` (`000a/000a6260.c`) |
| T# setters | `0x861fe0` (`0086/00861fe0.c`), `0x862160`, `0x8619a0` (`0086/008619a0.c`), `0x861ce0`, `0x861e60`, `0x8622e0`, `0x8611a0` (`0086/008611a0.c`) |
| Stage bind (record 0, record 4) | `0x937c0` (`0009/000937c0.c`), `0x94ab0` (`0009/00094ab0.c`), setters `0x861520`, `0x860bb0`, `0x861260`, `0x861600` |
| Per-draw flush and copy | `0x85f410` (`0085/0085f410.c`), `0x862c50` (`0086/00862c50.c`), `0x86d4c0` / `0x86d550` (SET_SH_REG writers) |
| Usage byte writer | `0x862510` (`0086/00862510.c`; reads the usage byte at `+0x19` of the blob header held at `O+0xc0c0` / `O+0xc0d0`) |
| Material parse | `0xf6c90` (`000f/000f6c90.c`), lists `0xf80f0` (`000f/000f80f0.c`), priority `0x14cf20` |
| Shader record array | `0xf8990`, `0xf8aa0` (K2 section 3) |
| Factory table | `particles-data.md` 2b |
| Port image key | `gpu/shadps4/shader_recompiler/specialization.h` lines 48-59, 117-154, 425-445 |
| Image bit layout | `gpu/shadps4/video_core/amdgpu/resource.h` lines 143-185, 268-318 |
| Miss counters | `gpu/sotc/pretranslate.cpp` lines 306-345, 440-470; `user/gcn-cold5/cold_w2.log` lines 1166, 1740 |
| Synthetic sharp and hints | `gpu/sotc/pretranslate.h` lines 1-10, 150-165 |

## Material path lists (T1)

Read-only static analysis, decomp only (`out/re/decomp/`), plus the ELF bytes of the vtable `0x1598628` (read from
`out/re/ghidra/eboot_ghidra.elf`, which has the RELRO data as a loadable segment) and the constant `DAT_02ee4210`.
No `.cmat` was read. Addresses are eboot VAs. Labels as above.

### T1.1 Answer in short

- **The two path lists are lists of CTexture references, not shader collections.** Each entry is a
  `{resource*, owned}` pair. The parser and the instance code read the referenced object at CTexture offsets: the
  T# at `+0xc4..+0xdc` (copied through `0x8829e0(…, 0x10)`), the mip vector at `+0x80/+0x88` (0x50-byte records) and
  two u32 fields at `+0x198/+0x19c`. **observed** (addresses in T1.4). Confidence high for list 2 (it has a name
  map), medium-high for list 1 (no name map found, T1.5).
- **The list does not say its type.** An entry is typed only by its path extension: the key is the last four
  characters of the path (`0x14c870`), and the loader maps that FourCC to the build function (`loader.md` section 4).
  **observed**. A list entry is a CTexture only if its path ends in `.ctxr`; the code assumes it, and no `.cmat` was
  read to confirm it. **inferred**.
- **The material's texture T#s are copied into one GPU-visible block per list.** A block holds the raw blob bytes of
  its list (blob A for list 1, blob B for list 2), followed by one 32-byte T# per list entry in list order. The block
  address is written to the pass as `pass+0xb0` (list 1) and `pass+0xb8` (list 2) by `0xf8c90` and `0xf8c30`. So the
  draw reaches the material's textures through these two blocks, not through the per-slot records and not through a
  per-mesh texture set. **observed** (writers and pass fields); the draw-time reader of `pass+0xb0/+0xb8` is **open**
  (T1.7).
- **Roles.** List 2 is the set of named texture slots: map 2 (`+0x528`) maps the FNV-1a hash of a slot name to a list-2
  index. List 1 has no name map that I could find; its entries pair by position with blob A records. **observed** for
  list 2 (`0xf9dc0`, key of `0x1714b0`), **inferred** for list 1.
- **The 0xd argument of `0x14cf20` is a priority, not a type** (T1.2). **observed**.
- **Corrections to the earlier sections of this file.** Section 3.2 says the material holds no T# words. That holds for
  the parser (`0xf6c90`), but the material's list textures do have their T# words copied into the instance blocks at
  run time, by `0xf5cd0`, `0xf9ef0` and `0xf8d10`. The T# source is still the CTexture `+0xc4` of section 2. Section 3.3
  says the lists are not read on the draw path; that holds for `0xf8990` only.

### T1.2 From a path to a resource object (item 1)

The name object (`ResourcePath`, 0x28 bytes, built by `0x14c3f0`; the decompiler drops its stream argument):

```c
struct ResourcePath {          // 0x28 bytes (local_a8 in 0xf6c90)
  u32  hash;                   // +0x00  FNV-1a 32 of the path (0x161f80, offset 0x811c9dc5, prime 0x1000193)
  u32  pad;                    // +0x04
  char *str;                   // +0x08  path bytes (string object: data, size, capacity)
  u64  size;                   // +0x10
  u64  cap;                    // +0x18
  void *shared;                // +0x20  refcounted helper (LOCK-inc in 0x14c3a0)
};
struct ResourceRequest {       // 0x38 bytes: the request used by 0xf6c90 and 0xf80f0
  struct ResourcePath path;    // +0x00
  void *res;                   // +0x28  resource object (0x14d880 = empty sentinel)
  u8    owned;                 // +0x30  1 = a load request at priority 0xd is held (0x95580)
};
```

Steps for one list entry (**observed**: list 1 in `0xf6c90` lines 250-302, list 2 in lines 324-376):

1. `0x14c3f0` reads the path string from the file. If the path contains `****`, its last four characters are replaced
   by a code from `0x147e20(DAT_02ee2d00, last4)`. Then the FNV hash is computed over the path.
2. `0xf80f0` calls `0xa8700` (or `0xfa1a0` for slots). When `+0x28` still holds the empty sentinel and the path is not
   empty, these call the resource manager through `DAT_02ee2cf8`, vtable slot `+0x18`. The vtable is `0x1598628`, and
   slot `+0x18` is `0x14e260` (read from the ELF). The manager object is `0x138` bytes, ctor `0x14db00`.
3. `0x14e260` searches a tree keyed by hash, then `strcasecmp` (`0x14c850`). When there is no match it creates a
   `0x100`-byte resource object (`0x14c8a0`: copies the path to `+0x70`, sets the root at `+0x68` to `DAT_02ee2d00`,
   sets state `+0x98 = 0xb`) and inserts it (`0x150360`). The result comes back with owned = 0. **observed**.
4. `0xf80f0` then (when `0x14cf00` says not ready) stores the resource in the request, and when the owned byte of the
   request is set it calls `0x14cf20(res, 0xd, DAT_02ed3640)`. `0xf6c90` copies `{res, owned}` into the 16-byte list
   entry (lines 289-302 for list 1, 363-376 for list 2). `0x95580` requests the load and sets owned = 1. **observed**.
5. The load starts when the payload is first read, `0x14d7b0(res)`. It calls the root's vtable slot `+0x68`
   (`DAT_02ee2d00`, the ResourceLoader, ctor `0x144ca0`) to get a job, and then the job's vtable `+0x10` starts the
   load. The build function is chosen by the FourCC key (`0x14c870`). **observed** (call pattern); **inferred** that
   the root's slot `+0x68` does the FourCC lookup (`loader.md` section 4 says so for `0x146ea0`).
6. **Priority.** `0x14cf20(res, p, params)` returns at once when another request is already pending and `res+0x98 <= p`.
   For `p == 0xd` it uses 5, or the current value when that is below 10. So `0xd` means "default priority".
   **observed**.

The root `DAT_02ee2d00` (`0x350` bytes, ctor `0x144ca0`) and the manager `DAT_02ee2cf8` are created in `0x2569d0`
(decomp lines 569 and 611). **observed**.

The instance pool object `DAT_02ee3ce0` (`0x8b0` bytes, ctor `0x166f50`) has default-resource slots at `+0x340` and
`+0x350`, both set to the empty sentinel in the ctor, and its default material name is at `DAT_02ee2bf8`. **observed**.
The use of `+0x340` as the second argument of `0xf80f0` is **inferred**: it is the fallback for a request whose load is
not ready (the branch that writes `*(res+0x28) = *param_2`). Not confirmed by a run.

### T1.3 Material object: the list, block and map fields

Stream order of the parser (**observed**, `0xf6c90`): after the slot array, the list-1 count and its path entries
(file dword 8 in R3 numbering), the list-2 count and its entries (dword 9), the size and bytes of blob A (dword 10,
copied to `+0x470`), the size and bytes of blob B (dword 11, copied to `+0x4b0`), the byte size of map 1 (dword 12,
8-byte records: u32 key, u32 value), and the byte size of map 2 (dword 13, same format).

```c
struct MatListEntry { void *res; u8 owned; /* 16 bytes */ };   // +0x490 list 1, +0x4d0 list 2

// Block object built by 0xf5cd0(block, blob_size, blob, list) at +0x578 (list 1) and +0x5b8 (list 2):
struct MatTexBlock {
  void **payload;     // +0x00  one CTexture payload per list entry (0x14d7b0 result)
  u8    *data;        // +0x08  host copy: blob bytes, then count x 32-byte T#
  u64    gpu;         // +0x10  record of 0x8be60 (type 0x1c, flags 0x804) with the guest address of data (inferred)
  u32    size;        // +0x38  blob_size + 32*count; this is the gate of pass+0xb0 and pass+0xb8
  u32    count;       // +0x3c  number of list entries
};
// Material fields:
//   +0x470/+0x478/+0x480  blob A (raw), +0x4b0/+0x4b8/+0x4c0  blob B (raw)
//   +0x490/+0x498/+0x4a0  list 1 (MatListEntry[]), +0x4d0/+0x4d8/+0x4e0  list 2
//   +0x4f0  map 1: key -> u16 byte offset into blob B (node +0x20 key, +0x28 value)
//   +0x528  map 2: key -> u32 list-2 index (node +0x20 key, +0x28 value)
//   +0x518  inherited instance flag (copied from the parent in the variant parse 0xf8270, line 177)
//   +0x578  block of list 1 (MatTexBlock). Its gpu field is +0x588; the stride used by 0xf8d10 is +0x590 (inferred)
//   +0x5b8  block of list 2 (MatTexBlock). Its gpu field is +0x5c8
//   +0x5b0 / +0x5f0 = size of block 1 / block 2 (0 = no block); +0x5b4 / +0x5f4 = count of list 1 / list 2
```

`+0x588` is `block1.gpu` (`+0x578 + 0x10`) and `+0x5c8` is `block2.gpu` (`+0x5b8 + 0x10`). The gates `+0x5b0` and `+0x5f0`
are block sizes: `0xf5cd0` writes `param_1[7]` (block `+0x38`), which is the material's `+0x5b0` or `+0x5f0`. This also
explains why the parser's render flags add `0x11a` (for `+0x5b0`) and `0x20` (for `+0x5f0`) at lines 620-626. **observed**.

`0xf5cd0` (`000f/000f5cd0.c`, 452 bytes): sets `+0x00` (payload array, one entry per list item, each `0x14d7b0(res)`),
allocates the host data of `size` bytes, copies the blob, appends one 32-byte T# per entry from `CTexture+0xc4..0xdc`
through `0x8829e0(…, 0x10)`, and creates the record at `+0x10` through `0x8be60` (the same call the mesh upload uses,
`loader.md` W1), with the guest address of the data. **observed**.

### T1.4 Readers and writers of the lists (item 2)

| Function (eboot VA) | Callers | What it does with the lists | Label |
|---|---|---|---|
| `0xf9ef0` (`000f/000f9ef0.c`) | `0x117160`, `0x117930` (1982-byte twins, no direct caller) | For list 2: refreshes the payload array (`+0x5b8`), then rewrites blob B and the T#s into the host data of block 2 (`+0x5c0`), when `+0x5f0 != 0`. `0x117160` walks the instance pool (bit set at `pool+0x2200d0`, entries at `pool+0x220078`, ids from the K2 allocator `0x115450`). | observed |
| `0xf8d10` (`000f/000f8d10.c`) | none found (vtable or indirect) | Same refresh for list 1 (`+0x578`, `+0x5b4`, gate `+0x5b0`) and list 2 (`+0x5b8`, `+0x5f4`, gate `+0x5f0`), run when a list entry changes (it compares the entry with the new resource first). | observed |
| `0xf8c90` (`000f/000f8c90.c`) and `0xf8c30` (`000f/000f8c30.c`) | `0x1a8c60` (pass submit, `P+0xf4` = 1 then 6), `0x1a6d20`, `0x1a7290` (via `0xf8990`) | `pass+0xb0 = (+0x588) | ((+0x58c) & 0xfff) << 32` when `+0x5b0 != 0`; `pass+0xb8 = (+0x5c8) | ((+0x5cc) & 0xfff) << 32` when `+0x5f0 != 0`. | observed |
| `0xf9dc0` (`000f/000f9dc0.c`) | `0x1714b0` (`0017/001714b0.c`, line 78), `0xb0fb20` (`00b0/00b0fb20.c`), `0x4fbd50` | Setter: looks up a u32 key in map 2 (`+0x528`), takes the list-2 index, replaces that entry's resource with the new one, and requests its load. | observed |
| `0xf9cf0` (`000f/000f9cf0.c`) | `0x1714b0`, `0xb0fb20`, `0x5119d0`, `0x511bb0` | Setter for blob B constants: looks up the key in map 1 (`+0x4f0`), copies the given bytes to blob B at the mapped u16 offset, and marks the instance dirty (bit `inst+0x640` in `DAT_02edf140+0x2200b0`). | observed |
| `0xf9b00` (`000f/000f9b00.c`) | `0x176230`, `0x238c00`, `0x38acd0`, `0x3b2ad0` | Predicate, which closes the K2 open item: true when `+0x518 != 0`; otherwise true when list 2 is not empty. | observed |
| `0xf9b20` (`000f/000f9b20.c`) | `0x4d7d50` (4200 bytes, one caller) | For each ready entry of list 1 and list 2 it reads `FUN_00096f50` (u32 at `res+0x198`) and `FUN_00096f60` (u32 at `res+0x19c`) and keeps the maxima in `*param_2` and `*param_3`. | observed (accessors); the meaning of `+0x198/+0x19c` is **inferred** (streaming levels) |
| `0xf9410` (`000f/000f9410.c`) | none found | For list 1: if any payload has more than one 0x50-byte record in its `+0x80` vector, it calls `0xfb120(DAT_02edef60, material)` and stores a u16 at `+0x630`. For list 2 it only loads the payloads. | observed (code); **inferred** (the 0x50 records are mip records) |
| `0xf8270` (`000f/000f8270.c`) | `0xf6c90`, `0xf9890` | Variant parse: copies the parent's list 1 and 2 entries (`0xfaa20`, lines 152 and 263), its blobs, both maps (`0xfa0c0`, `0xfac90`, `0xfad70`), and `+0x518`; then rebuilds the blocks (`0xf5cd0`, lines 223 and 280). | observed |
| `0xf6c90` (parser) | `0xf9390`, `0xf9530` | Builds both blocks (`0xf5cd0`, decomp lines 402-497) and both maps (`0xfa560` for map 1 at line 509, `0xfa6f0` for map 2 at line 540). | observed |

### T1.5 Strings and the two raw blobs (item 3)

- The only texture slot name on this path is **`font_texture`**. `0x1714b0` computes `DAT_02ee4210 = FNV-1a("font_texture")`
  once (`0x161f80`, lines 30-38), and calls `0xf9dc0(material, DAT_02ee4210, ...)` (line 78). The value I get from the
  same FNV-1a is **0x0c659ccc** (computed here, not read from the ELF). The key is the plain slot name (no path, no
  extension), which shows map 2 is keyed by slot names. If the caller's texture is null, or its flag byte has bit
  `0x8` set, it uses the material's own `param_1 + 0xf0` instead (lines 75-77).
- `"Failed to find texture slot for %s"` and `"Failed to load texture for %s"` are in `0xff430` (`000f/000ff430.c`,
  906 bytes). That is the **particle** texture binding: it works on a 0x3c-byte particle record at `param_1 + 0x388` and
  reads a u16 pair at the resource's `+0x60`. It does not use the material lists. **observed**.
- No `_d`, `_n`, `_s`, `Sampler` or `Param` material-slot strings were found in the decomp literals (all folders
  searched for these patterns). **observed** (absence in the literals searched).
- Blob A (`+0x470`, dword 10) and blob B (`+0x4b0`, dword 11) are raw bytes. Blob B has u16 offsets written into it
  from map 1 (`0xf9cf0`), so it is a constant block. Blob A is copied into the block of list 1, and its consumer is
  **unknown**: nothing I read places it at a fixed shader-readable offset. Whether it holds technique names or constants
  is **unknown**.

### T1.6 Conclusion (item 4)

- **Are the lists textures?** Yes, both lists, by the code that reads their payloads. **observed**. The `.ctxr`
  extension of the paths is **inferred** (no `.cmat` read).
- **Which list is which?** List 2 is the named set: map 2 gives name hash to index, and map 1 places constants in
  blob B. List 1 is index-addressed, with blob A. **observed** for list 2, **inferred** for list 1. The brief's
  "per-pass" or "streamed mips" split is not what the code shows: both lists are textures that go to a block. The
  difference is naming, not function.
- **How does a pass find the textures at draw time?** Through `pass+0xb0` and `pass+0xb8`, the block addresses of the
  material instance. They are written by `0xf8c90` and `0xf8c30` when the block size is nonzero. The block holds the T#
  words of the textures, copied from CTexture `+0xc4`. The index into the block is the list index (list 1) or the map 2
  index (list 2), at `blob size + 32*index` inside the block. **inferred** for the offset rule; the layout is
  **observed**. The reader of `pass+0xb0/+0xb8` on the draw path is **open** (T1.7).
- **Port key.** A material's texture T# words are known when its list textures are ready, which is at material load.
  The parser returns early (value 0) while a slot's shader is not ready (decomp line 208, `goto LAB_000f80bc`), so the
  lists are only filled after the slot shaders load. The port can build the T# part of the key from the block data
  (`data + blob size + 32*i`) instead of from draws. **observed** (early return), **inferred** (the key use).

### T1.7 Open items

1. The reader of `pass+0xb0` and `pass+0xb8` on the draw path. The field grep over `0085` to `001a` found only unrelated
   structures. Look at the users of the pass pointer `P` in the `0x1a8c60` chain, and at the user-data writers
   `0x862510` and `0xae150`. **open**.
2. Who sets `+0x5b0` and `+0x5f0` on a material. They are block sizes, so they come from the parser's own `0xf5cd0`
   call. Confirm on a real file. **open** (no writer outside the instance paths).
3. The consumer of blob A (T1.5). **open**.
4. The list-1 name key, if one exists. No map is used for list 1 in the code found. **open**.
5. What the `gpu` record at `block+0x10` points to after `0x8be60`, and whether `0xf9ef0` re-uploads the block or only
   rewrites the host copy. **open**.
6. The meaning of `CTexture+0x198/+0x19c` (T1.4, `0xf9b20`). **open**.

### T1.8 One-run log spec (what stays open, and where to hook)

Each hook logs once per (material, list entry), with no change to draws.

1. **Name of each list entry.** Entry of `0xf80f0`, with the caller return address (the call at decomp line 260 is list 1,
   the call at line 334 is list 2). Dump the `ResourcePath`: `hash` (u32 at +0), the path bytes (`str` at +0x08,
   length at +0x10). After return, dump `res` (+0x28), `owned` (+0x30), `res+0x98`, `res+0x20` (non-null when loaded)
   and `res+0x68`. This settles the extensions, which are the types.
2. **Maps.** At the return of `0xf6c90` with value 1 (after the loop at decomp lines 683-685; not on the early return of
   line 208), walk map 1 (`+0x4f0`) and map 2 (`+0x528`). For each node dump the key (u32 at node +0x20) and the value
   (u32 at node +0x28). Compare the map 2 keys with the FNV of the list-2 path names and with `0x0c659ccc`.
3. **Block contents.** At `0xf8c90` entry (`param_1` = material, `param_2` = pass): dump `+0x5b0`, `+0x5b4`, `+0x5f0`,
   `+0x5f4`; for each block, the first 0x40 bytes of `data`, and the 32-byte T# words after the blob (at
   `data + blob size + 32*i`). Dump `pass+0xb0` and `pass+0xb8` after the call, and `P+0xf4` (k).
4. **Texture identity.** For each list-2 entry in item 3, dump the CTexture `+0xc4..0xdc` words next to the path from
   item 1, so each path maps to its T#.
5. **Reader of the block address.** Set a read watch on `pass+0xb0` and `pass+0xb8` after the `0xf8c90` write, and log the
   reader's return address. `0x862510` and `0xae150` are the first candidates. This closes T1.7 item 1.
6. **Draw.** At `0x862c50` (or the first `0x85f410` after a material bind), log whether the shader reads the block, by
   comparing the block address with the user SGPR values written.

### T1.9 Evidence list (this section)

| Item | Address or file |
|---|---|
| Parser: reset, slot path, lists, blobs, blocks, maps, render flags, epilogue | `0xf6c90` (`000f/000f6c90.c`): lines 70-101 reset, 135-208 slots, 250-302 list 1, 324-376 list 2, 402-497 blobs and blocks, 503-545 maps, 620-626 flags, 683-693 epilogue |
| Path to name | `0x14c3f0` (`0014/0014c3f0.c`), `0x147e20`, `0x161f80` |
| Resolve and create | `0xa8700` (`000a/000a8700.c`), `0xfa1a0` (`000f/000fa1a0.c`), `0x14e260` (`0014/0014e260.c`), `0x14c8a0`, `0x150360`, `0x14c850` |
| Manager, root, pool ctors | `0x14db00`, `0x144ca0`, `0x166f50` (`0016/00166f50.c`), `0x2569d0` (`0025/002569d0.c`) |
| Vtable of the manager (ELF) | `0x1598628`; slot `+0x18` = `0x14e260` |
| Request and priority | `0x14cf20` (`0014/0014cf20.c`), `0x14cf00`, `0x95580` (`0009/00095580.c`), `0x14d7b0` (`0014/0014d7b0.c`) |
| FourCC key and loader | `0x14c870` (`0014/0014c870.c`); `loader.md` section 4 (`0x146ea0`) |
| Block object | `0xf5cd0` (`000f/000f5cd0.c`) |
| Refresh, setters and predicates | `0xf9ef0`, `0xf8d10`, `0xf8c90`, `0xf8c30`, `0xf9dc0`, `0xf9cf0`, `0xf9b00`, `0xf9b20`, `0xf9410`, `0xf8270` (all `000f/`) |
| Callers | `0x1714b0` (`0017/001714b0.c`, key `font_texture`), `0xb0fb20` (`00b0/00b0fb20.c`), `0x117160` (`0011/00117160.c`, line 131), `0x1a8c60` (`001a/001a8c60.c`), `0x4d7d50` (`004d/004d7d50.c`) |
| Particle texture strings | `0xff430` (`000f/000ff430.c`) |


## Block reader (T7)

Read-only static analysis (agent T7, follows T1 section T1.7 item 1 and T4 sections T4.4-T4.6). Decomp files in
`out/re/decomp/`, field offsets as in D3/T1/T4. No game file read, no game run. Labels as above.

### T7.1 Short answer

- **The "pass" object of `0xf8c90` is the draw item `P`** (the DrawItem of T4.4, not a K1/K5 pass object). `P+0x00` is the
  render context, `P+0xf4` is the slot k, `P+0x90..+0xd8` are the ten pointer slots. The submitter sets `P+0xf4 = k`
  just before the call (`0x1a8c60` lines 15-16 and 26-27: `*(int*)(P+0xf4) = 1`, then `FUN_000f8c90(material, P)`;
  `0x1809a0` the same). **observed**.
- **`P+0xb0` and `P+0xb8` are pointer slots 4 and 5 of the item, not "variant words".** `P[0x12] = P+0x90`, so
  `P+0xb0 = P[0x16]` (slot 4) and `P+0xb8 = P[0x17]` (slot 5). T4.4 ("these are not pointer slots") is wrong. **observed**.
- **The only reader found is the mask compaction `0x95320`.** It copies `P[0x12+k]` for each set bit k of the mask into a
  bump chunk (bit 4 -> `P+0xb0`, bit 5 -> `P+0xb8`). The chunk base goes to the stage SGPRs by `0xae150` -> `0x862490`.
  So the block pointer reaches the shader through the SRT chunk, and the mask (`material+0x468`) decides its index.
  **observed** (decomp); **inferred** that the shader reads the chunk as a pointer chain (T7.5).
- **The brief's caller list was wrong.** `0x1a6d20` and `0x1a7290` do not call `0xf8c90` and do not touch `+0xb0/+0xb8`
  (their only `0xf8` call is `0xf8990`). The real callers are listed in T7.2. This also corrects the T1.4 row.

### T7.2 Writers of `P+0xb0 / P+0xb8` (`0xf8c90`, `0xf8c30`)

```c
// 0xf8c90(material, P) (000f/000f8c90.c, 118 bytes); 0xf8c30 is the same without the 0xf8990 call
if (material->b5b0 != 0) P->b0 = (u64)material->b588 | ((u64)material->b58c & 0xfff) << 32;
if (material->b5f0 != 0) P->b8 = (u64)material->b5c8 | ((u64)material->b5cc & 0xfff) << 32;
```

- `material+0x588/0x58c` are the first two words of the block-1 V# (block `+0x578`, V# at block `+0x10`, written by
  `0x8be60` in `0xf5cd0`; the same 44-bit base layout as the V# writer `0x8652d0`, D1 section 5.1). `material+0x5c8/0x5cc`
  is the same for block 2 (`+0x5b8`). **observed**.
- So `P+0xb0` is the **guest base address of the block's data**, not a V#. `0xf5cd0` passes the data pointer through
  `(host - DAT_02ed2108) + DAT_02ed2110` (its call `FUN_0008be60(param_1+2, 0x1c, ...)`), so the value is a guest
  address. **observed**.
- Callers of `0xf8c90`: `0x1a8c60`, `0x17a580`, `0x11d5b0`, `0x1641d0`, `0x11d3d0`, `0x1809a0`, `0x178cb0`, `0x5b7630`,
  `0xdb390`, `0x10b0a0`. Callers of `0xf8c30`: `0x1b32f0` (line 349), `0x1af430`. **observed** (decomp headers).
- Order check: in `0x1809a0`, `0xdb390`, `0x10b0a0`, `0x11d3d0`, `0x11d5b0`, `0x5b7630`, `0x178cb0` the `0xf8c90` call comes
  **before** the `0x95320` call of the same item, so the chunk sees the block pointer. **observed**. Not checked:
  `0x165360`, `0x17a6c0`, `0x173160`, `0x1b32f0`, `0x1af430` (their order is not visible in the lines read).

### T7.3 The reader: `0x95320` and the mask

`0x95320(mask, &P[0x12], pool)` (252 bytes, `0009/00095320.c`) allocates `popcount(mask & 0x3ff) * 8` bytes and copies
`P[0x12 + k]` for each set bit k, in bit order. Bit to item slot (observed unless marked):

| mask bit | item slot | Meaning |
|---|---|---|
| 0 | `P+0x90` | mesh V# table (`*(mesh+0x70)+0x2c0`, T4.2). Always set in parser masks (T7.4). |
| 1 | `P+0x98` | not written by the paths read; **unknown** (always set in parser masks) |
| 2 | `P+0xa0` | not set in parser masks |
| 3 | `P+0xa8` | 0x28-byte list of `0xdb390` / `0x10b0a0` (T4.4). **inferred** from the bit. |
| 4 | `P+0xb0` | **block 1 base** (`0xf8c90`). Set iff `material+0x5b0 != 0`. |
| 5 | `P+0xb8` | **block 2 base** (`0xf8c90`). Set iff `material+0x5f0 != 0`. |
| 6 | `P+0xc0` | set for type 4/7 (0x41) and the 0x80 flag (T7.4) |
| 8 | `P+0xd0` | always set in parser masks; **unknown** |
| 9 | `P+0xd8` | set for types 1, 4, 7 (0x200) |

Mask source: the callers pass `*(u32*)(material+0x468)` (observed in `0x1809a0` line 13, `0xdb390` line 104,
`0x10b0a0` line 102, `0x11d3d0` line 52, `0x11d5b0` line 38, `0x5b7630` line 214; `0x94fb0` takes it as its `param_2`,
called from `0x165360` and `0x178cb0`). The writes in T7.4 prove that this field is the mask.

Two other builders call `0x95320` with a constant mask, not the material mask: `0x1051f0` line 373 (mask `0x172`,
or `0x17b` when the object has a record) and `0x103ad0` line 374 (`0x172`). Those masks include bits 4 and 5, but their
item path (`0x10c6c0`, `0x10c2e0`) is not a caller of `0xf8c90`. So on that path the block slots hold whatever the
item held before. **Writer not found** (open, T7.7 item 4).

Other readers of `P+0xb0 / +0xb8` checked, none found: `0xae150` (reads `P+0x18`, `P+0x20`, `P+0x1d8+8s`),
`0x862510` (reads only `O` fields and the usage bytes; it is not a reader of the item), `0x862490`, `0x862c50`,
`0x85f410`, `0x85fc10`, `0x86b040`, `0x94d00`, `0x94dd0`, `0x94ec0`, `0x128d20`, `0x1122d0`, `0x1a6d20`, `0x1a7290`.
So the compaction is the only reader on the material path. **observed** (grep over these decomps). The grep is a field
search, so an indirect read through a base pointer would not show (T7.7 item 4 covers the gap).

### T7.4 Which blocks a material has (`material+0x468`, written by the parser)

The writes in `0xf6c90` (**observed**, lines 573-626):

- `material+0x468` is set to `uVar17`, which is `1`, or `0x41` for material type 4 or 7 (line 604). Every path that
  reaches line 604 leaves bit 0 set. The `goto` at line 573 sets `uVar17 = 1` and skips it. **So bit 0 is always set.**
- `0x10a` is OR-ed in (bits 1, 3, 8). `0x200` (bit 9) is added for types 1, 4, 7 (the constant `0x92`, line 608).
  `0x80` is added when `material+0x464 & 0x40` (line 614).
- Bit 4 (`0x10`) is set iff `material+0x5b0 != 0` (line 620: `0x11a` = `0x100|0x10|0x8|0x2`).
- Bit 5 (`0x20`) is set iff `material+0x5f0 != 0` (line 624).

So the bit that gates `P+0xb0` in `0xf8c90` is the same bit that places it in the chunk. The variant parse `0xf8270`
copies `+0x468` from the parent (its line 115). **observed**.

Chunk index of each block (computed from the mask): index = `popcount(mask & ((1<<k)-1))`. Bits 0, 1 and 3 are always set
in parser masks, and bits 6, 8, 9 lie above bits 4 and 5, so they do not move the indices:

| Material blocks | mask (typical) | chunk slots | Block 1 index | Block 2 index |
|---|---|---|---|---|
| both | `0x13b` | [0 `P+0x90`, 1 `P+0x98`, 2 `P+0xa8`, 3 `P+0xb0`, 4 `P+0xb8`, 5 `P+0xd0`] | 3 | 4 |
| block 1 only | `0x11b` | [0 `P+0x90`, 1 `P+0x98`, 2 `P+0xa8`, 3 `P+0xb0`, 4 `P+0xd0`] | 3 | - |
| block 2 only | `0x12b` | [0 `P+0x90`, 1 `P+0x98`, 2 `P+0xa8`, 3 `P+0xb8`, 4 `P+0xd0`] | - | 3 |

**Consequence (inferred from the code, important for the port):** block 2's SRT index is 4 when block 1 exists and 3 when
it does not. A shader compiled once for a program can use one fixed index only if every material of that program has
the same pair of blocks. This is open (T7.7 item 2).

### T7.5 From the chunk to the shader: the SRT path

1. `0x95320` returns the chunk base `C`. `0xae150(P, C)` (441 bytes, `000a/000ae150.c`) walks the set bits `i` of `P+0x18`
   (slot mask), reads the stage `s = *(P+0x20+4i)`, and takes the stage record `R = P[0x3b+s]` (set by `0x937c0` and
   `0x94ab0`). For each `R` whose **file record byte `+0x19` equals 2** (`*(*R+0x19) == 2`), it calls
   `0x862490(O, s, &C, 2)` with `O = ctx+0x2e8`. **observed**.
2. `0x862490` (`0086/00862490.c`, 126 bytes): the SGPR index is the low byte of `*(O+0xc180+8s)+0x10` (the stage layout,
   `*R+8`, D3 section 4.1); the PM4 count is the **high byte** of the same u16. It calls `0x86d5e0(cmd, s, sgpr, &C, count)`.
   The fourth argument `2` of the call in `0xae150` is not used by the decomp body. **observed**.
3. `0x86d5e0` (`0086/0086d5e0.c`) writes `count` dwords from `&C` (`case 1` writes one dword, `case 2` two dwords). **observed**.
   **Correction to T4.5 and T4.6:** T4 said the count for a pointer is 1. A 64-bit chunk base needs `count = 2` (two SGPRs).
   The high byte of the stage layout u16 at `+0x10` must be 2. **inferred** (decode, not run).
4. Per draw, the stage SGPR pair holds `C`. `C[0] = P[0x90]` (mesh table), and `C[j]` follows T7.4. The shader reads a block
   by two loads: `SGPR -> C[j] (block base) -> T# or constant at base + offset`. **inferred**: the port's SRT walker
   (`gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp`, `VisitPointer`) handles nested pointer
   loads; `ir/passes/srt.h` holds only the API.

### T7.6 Inside the block (what the shader's offsets point to)

From the parser and `0xf9ef0`, `0xf9cf0`, `0xf9dc0` (T1.3-T1.5):

| Block | Host copy (port reads it) | Contents | Index of a T# or constant |
|---|---|---|---|
| block 1 (`material+0x578`) | `+0x580` (block `+0x08`) | blob A (`+0x470`, raw), then one 32-byte T# per list-1 entry | T# at `sizeA + 32*i`, `i` = list-1 index; `sizeA = material+0x5b0 - 32*material+0x5b4` |
| block 2 (`material+0x5b8`) | `+0x5c0` | blob B (`+0x4b0`, constants, u16 offsets from map 1 `+0x4f0` via `0xf9cf0`), then one T# per list-2 entry | constant at `off` (map 1); T# at `sizeB + 32*idx2`, `idx2` from map 2 `+0x528` (name hash to index, `0xf9dc0`); `sizeB = material+0x5f0 - 32*material+0x5f4` |

- Map 2 is keyed by the FNV-1a of the slot name. The only name found on this path is `font_texture` (T1.5, `0x1714b0`).
- `sizeA` and `sizeB` are the byte offsets the shader must know to reach the T#s. They are fixed per program only if the
  blob size is fixed per shader. **Unknown** (T7.7 item 1).
- The port can read the T# words at material load from the host copy (`+0x580`, `+0x5c0`), with no draw needed.
  Their values are copies of `CTexture+0xc4..0xdc` (T1.3, `0xf5cd0` `local_58` copy loop). **observed** (code).

### T7.7 Open items and one-run log spec

1. **Blob size per program.** Compare `sizeB` (and `sizeA`) across materials that share a program. If constant, the
   shader's immediate offsets are `sizeB + 32*idx`. **open**.
2. **Mask per program.** Log `material+0x468`, `+0x5b0` and `+0x5f0` per material and per program hash. If block 1 and
   block 2 are not always both present, the chunk index of block 2 moves (T7.4), and the shader must match. **open**.
3. **The gate byte.** At `0xae150` (each stage with `R`), log the stage `s`, `*(*R+0x19)`, and the layout u16 at
   `*(O+0xc180+8s)+0x10`. This shows which stages get the chunk (VS, PS, or both) and the PM4 count (expect 2). **open**.
4. **Writer of the item slots on the `0x1051f0` / `0x103ad0` path.** Set a write watch on `P+0xb0` and `P+0xb8` (or log the
   writes in `0x10c6c0`, `0x10c2e0`, `0x1239a0`, `0x125500`) and log the return address. **open**.
5. **Chunk contents.** At `0x862490` entry, log the 8 bytes at `&C`. At the first draw of a material, check
   `C[3] == material+0x588 | ((material+0x58c)&0xfff)<<32` (block 1) and the block 2 slot (`C[3]` or `C[4]`, T7.4) against
   `+0x5c8/+0x5cc`. This closes T7.5 and the pass-to-block link.
6. **T# identity.** At the same point, check that the T# at `block2 + sizeB + 32*idx2` equals `CTexture+0xc4..0xdc` of the
   texture named in map 2 (T1.8 item 4). Then the shader's image slot can be matched by its SRT offset.
7. **Shader side.** Dump the first `s_load` of the VS and PS SRT (`DumpSrtProgram` in `flatten_extended_userdata_pass.cpp`
   writes `*.srtprogram.txt`). Look for a two-level load (`C[j]`, then the block) and its immediate offset. **open**.

### T7.8 Corrections and evidence

Corrections to earlier sections (kept here, not edited above, per the append-only rule):

- T1.4 row `0xf8c90` / `0xf8c30`: the callers `0x1a6d20` and `0x1a7290` are wrong. Use the list in T7.2.
- T1.7 item 1 (reader of `pass+0xb0`): **closed** here. The reader is `0x95320` (mask compaction), not `0x862510` or `0xae150`.
- T4.4 note "`0xf8c90` ... These are not pointer slots": wrong. `P+0xb0` and `P+0xb8` are slots 4 and 5 of the compacted list.
- T4.5 / T4.6: the pointer count for the stage write is 2 (T7.5 step 3), not 1. Bit 0 of the mask is always set (T7.4), so
  `C[0] = P[0x90]` for all parser-built materials.

| Item | Address or file |
|---|---|
| Pass = draw item, k at `+0xf4`, submit | `0x1a8c60` (`001a/001a8c60.c` lines 15-16, 26-27), `0x1809a0` (`0018/001809a0.c`) |
| Block pointer writers | `0xf8c90` (`000f/000f8c90.c`), `0xf8c30` (`000f/000f8c30.c`) |
| Block build (V# base, guest translation) | `0xf5cd0` (`000f/000f5cd0.c`), `0x8be60` (`0008/0008be60.c`) |
| Mask compaction (the reader) | `0x95320` (`0009/00095320.c`) |
| Mask writes | `0xf6c90` (`000f/000f6c90.c` lines 573-626); variant copy `0xf8270` line 115 |
| Mask callers | `0x1809a0`, `0xdb390` (`000d/000db390.c` line 104), `0x10b0a0` (`0010/0010b0a0.c` line 102), `0x11d3d0`, `0x11d5b0`, `0x5b7630`, `0x94fb0` (`0009/00094fb0.c`), `0x1823e0` (`0018/001823e0.c`) |
| Constant masks 0x172 / 0x17b | `0x1051f0` (`0010/001051f0.c` line 373), `0x103ad0` (`0010/00103ad0.c` line 374) |
| Chunk to SGPR | `0xae150` (`000a/000ae150.c`), `0x862490` (`0086/00862490.c`), `0x86d5e0` (`0086/0086d5e0.c`), `0x937c0` (`0009/000937c0.c`) |
| Shader SRT walker | `gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp` (`VisitPointer`), `ir/passes/srt.h` |
| Checked, no reader of `+0xb0/+0xb8` | `0x862510` (`0086/00862510.c`), `0x862c50`, `0x85f410`, `0x85fc10`, `0x86b040`, `0x94d00`, `0x94dd0`, `0x94ec0`, `0x1a6d20`, `0x1a7290` |

## Block gates (T8)

Read-only static analysis of the EU 1.01 decomp (`out/re/decomp/`), plus the vtables read from the ELF
(`out/re/ghidra/eboot_ghidra.elf`: material class `0x1597c48`, instance class `0x1597c78`). Addresses are eboot VAs.
Labels: **observed** (decomp line, address given), **inferred**, **open**. No game file was read and no game was run.
"Block" means the material's list block (`MatTexBlock`, T1.3): block 1 at `mat+0x578` (list 1), block 2 at `mat+0x5b8`
(list 2). The gate is the block's `+0x38` field.

### T8.1 Answer in short

- **The gate is the block size, not a flag.** `+0x5b0` (block 1) and `+0x5f0` (block 2) hold `blob bytes + 32 * count`,
  an unaligned int. The count is `+0x5b4` / `+0x5f4`. Only `0xf5cd0` writes them. **observed** (`0xf5cd0` lines 39-40).
- **Gate nonzero does not mean "has textures".** A block with blob bytes and no list entries has gate > 0 and count 0.
  The pass binding (`0xf8c90`, `0xf8c30`) tests the gate only, so a reader must also test the count. **observed**.
- **A block is built on every successful parse of a material**, fresh or variant, in the same call that builds the
  lists. Nothing checks list readiness. The only early exit is the slot-shader check of `0xf6c90` (line 208), which
  returns 0 before the lists are reached. **observed**.
- **The T# words are a snapshot.** `0xf5cd0` copies `CTexture+0xc4..+0xdc` of each list payload into the block when it
  builds it. Later refresh comes from `0xf8d10` (material vtable slot `+0x10`, on a resource notification), from
  `0xf9ef0` (block 2, instance-pool walkers) and from `0xf9c40` (block 1 T# only, mip-registry walkers). **observed**
  (writers and callers). The notifier itself is **open**.
- **A draw can see fallback or stale T#s**, because a list entry whose resource is not ready is stored as the pool's
  default resource (`0xf80f0`). **inferred** (section T8.5).
- **A draw with gate 0 does not write the pass fields.** `pass+0xb0` / `pass+0xb8` keep their previous value. **observed**
  (the store is conditional); the value left behind is **open**.
- **Port read point:** the final T# set of a material is `payload[i]+0xc4` for `i < count`, once the material parse has
  returned 1 and each entry's resource is ready and is not the fallback. Read the payload pointers, not the block bytes
  (T8.6 explains why). Section T8.7.

### T8.2 Writers and readers of the four fields

| Field | Writers (observed) | Readers (observed) |
|---|---|---|
| `mat+0x5b0` (gate 1) | `0xf5cd0` via `block+0x38`: called from `0xf6c90` line 447 and `0xf8270` line 224 | `0xf8c90` line 7, `0xf8c30` line 6, `0xf6c90` line 620 (flag `0x11a`), `0xf8d10` line 112, `0xf9c40` line 16 |
| `mat+0x5b4` (count 1) | `0xf5cd0` via `block+0x3c` (same callers) | `0xf8d10` lines 100, 110; high dword of `0xf9c40` line 16 |
| `mat+0x5f0` (gate 2) | `0xf5cd0` via `block+0x38`: `0xf6c90` line 497; `0xf8270` line 280, only when its flag is nonzero | `0xf8c90` line 11, `0xf8c30` line 10, `0xf6c90` line 624 (flag `0x20`), `0xf8d10` line 166 |
| `mat+0x5f4` (count 2) | `0xf5cd0` via `block+0x3c` | `0xf9ef0` lines 20, 30, 32, 36; `0xf8d10` lines 154, 164 |

- Constructor `0xf63c0` (called by the factory `0xf9390` and by the variant ctor `0xf9890`): zeroes `+0x5f0` and
  `+0x5f4` (line 154 and the `param_1+0xbe` store). It also does a 16-byte store at `+0x5b0` (`param_1+0xb6`) of
  `auVar2`, which the decompile never assigns. So `+0x5b0..+0x5bf` are **undefined** until a parse writes them.
  **observed** (store), **inferred** (indeterminate value). Read the gates only after the parse returned 1.
- Other functions in the grep for these offsets write the same numbers in other classes and were checked and excluded:
  `0x9aef0` (render-target setup, `kRTT_*` names), `0x3902b0`, `0x53fc0` (float parameters), `0x433e0`, `0x43fb0`,
  `0x976db0`, `0x198bb0`, `0x25fbd0` (a zeroing ctor of a large object), `0x36210`.

### T8.3 When a block is built

Parser `0xf6c90` order (observed, line numbers of `000f/000f6c90.c`):

1. Reset: lines 70-101.
2. Slot loop, lines 135-241. For each slot: `0x95580` requests the shader (194, 204), `0x14d7b0` gives the payload
   (205), `0x14cf00` gives ready (206). **If not ready, `goto LAB_000f80bc` (208), which returns 0.** Ready means
   state `+0x98 == 0xb` and `+0x20 != 0` (`0x14cf00`).
3. File dwords 8-9 and the lists: list 1, lines 250-302; list 2, lines 324-376. Each entry goes through `0xf80f0`.
4. Blob A and block 1: lines 400-447. Blob B and block 2: lines 448-497.
5. Maps: lines 501-558. Flags: lines 603-626 (`+0x5b0` adds `0x11a`, `+0x5f0` adds `0x20`).
6. Children (variant list `+0x600`): lines 683-687, each `0xf8270(child, param_3)`.
7. Return 1, line 693.

Factory `0xf9390` (`000f/000f9390.c`): `0xf6c90(obj, stream, 1)`. If it returns 0 it returns null. It frees the
object only when the object is freshly allocated (`param_2[1]` was null). **observed**. So a material object that a
caller holds has passed step 2 and has its blocks built. The loader's retry after null is **inferred**
(`loader.md` section 4 is the place to confirm it).

Entry resolution, `0xf80f0` (observed branch): when the requested resource is not ready, the entry's `res` is replaced
by `*param_2`, which is the value at `DAT_02ee3ce0+0x340` (the pool's default resource slot, passed as `param_2` at the
call sites, lines 260 and 334 of `0xf6c90`), and the default is requested at priority `0xd`. So a stored list entry
always has a payload (`0xf5cd0` reads `payload+0xc4` without a null check, line 54), but it may be the default and not
the texture the .cmat names. **observed** (branch); **inferred** (that the `+0x340` slot holds a loaded default texture;
T1.2 says the slot is set to the empty sentinel in the pool constructor).

Variant rebuild `0xf8270` (observed), called by `0xf9890` with flag 1 (line 22) and by the parser for each child
(`0xf6c90` line 686):

- Frees and rebuilds block 1 always (`0xf5cd0` at line 224). Block 1's GPU record is not released first (`0x8be10` is
  called only for `+0x5c8`). Whether that leaks or is overwritten by `0x8be60` is **open**.
- Rebuilds block 2 only when the flag is nonzero (line 280). Block 2 is released first with `0x8be10(+0x5c8)`.
- Both lists and both blobs come from the parent (`0xfaa20` at `+0x490` and `+0x4d0`, the blob copy, lines 152-263),
  so the variant gates equal the parent's counts and sizes.

### T8.4 When a block is rebuilt (refresh paths)

| Function | Trigger (observed) | What it refreshes | Limits |
|---|---|---|---|
| `0xf8d10` (1361 bytes) | Material vtable `+0x10` (`0x1597c48` slot 2 = `0xf8d10`; instance class `0x1597c78` slot 2 = `0xf8d10`). Called with a notify list `param_2`: `*(param_2+8)` to `*(param_2+0x10)`. Caller **open** | For each notified resource R: slots `+0x60` (stride `0x48`, 14 slots) whose `res == R`; the first list-1 entry whose `res == R` (lines 92-99), then all list-1 payloads (100-111) and all block-1 T#s (112-143); the same for list 2 (145-197); then forwards R to the children (203-210) | First match only per list. The T# write address is `V# base + stride*num_records - 32*count` (see T8.6) |
| `0xf9ef0` (290 bytes) | Called from `0x117160` line 131 and `0x117930` line 132 (walkers of the 256-entry instance pool: bitset `pool+0x2200d0`, table `pool+0x220078`). Walk trigger **open** | List-2 payloads (`+0x5b8`, count `+0x5f4`), then if gate 2 is nonzero the whole block-2 host copy: blob B memcpy to `+0x5c0`, then T#s after it | Does not touch block 1 or the V# record |
| `0xf9c40` (76 bytes) | Called from `0xfb1e0` and `0xfb2d0` (walkers of the mip registry: 65536-bit bitset at `registry+8`, table at `registry+0x2008`) | Block-1 T#s only, at `block+0x8` + blob A size | No payload refresh, no V# rewrite |
| `0xf9410` (232 bytes) | No caller; not in material vtable slots 0-11 (ELF read) | If any list-1 payload has more than one `0x50`-byte mip record (`+0x80/+0x88`), calls `0xfb120(registry, mat)` and stores its u16 result at `mat+0x630` (`0xffff` on failure) | This registers the material in the mip registry, so `0xf9c40` refreshes its list-1 T#s |
| `0xf9dc0` (list-2 setter, T1.4) | Caller `0x1714b0` (`font_texture`), `0xb0fb20`, `0x4fbd50` | Replaces one list-2 entry's `res` and requests it | Does not rebuild the block; the block changes at the next `0xf8d10` match or walker tick |

Note on the registries: `0xfb120` (registry, 1024 words of bitset) and the 256-entry instance walker (`0x117160`) use
different bitsets (`+8` and `+0x2200d0`). **observed** (sizes). Whether they are one registry is **open**.

### T8.5 Timing: is the block ready before the first draw?

| Event | Gate and count | Block T# content | Evidence |
|---|---|---|---|
| A slot shader is not loaded | Parse returns 0 before any block is written. A fresh object is freed, so no material exists yet | none | `0xf6c90` line 208; `0xf9390` |
| Parse returns 1 | Gates set as in T8.1 | Copied now from `payload[i]+0xc4`. Entry may be the real texture (ready) or the default (not ready) | `0xf6c90` lines 447, 497; `0xf80f0` branch |
| An entry's real resource becomes ready | No gate change | Refreshed only by a notification whose R equals the entry's `res`. A default entry does not match the real resource, so it may stay stale (see T8.8 item 2) | `0xf8d10` line 99 |
| Walker tick (`0x117160` / `0x117930`) | None | Block-2 T#s rewritten from the list-2 payloads | `0xf9ef0` |
| Pass submit (`0x1a8c60` and others, T1.4) | Reads the gate | `pass+0xb0` / `+0xb8` written when the gate is nonzero | `0xf8c90`, `0xf8c30` |

Answers:

- **The block exists before any draw of that material**: every material object that a caller holds has returned from a
  successful parse. **observed**.
- **The T# set is final only after each list texture is ready and refreshed**, which can be after the first draw.
  **inferred**.
- **A material can draw with gate 0**, when it has no list-1 entries and no blob-A bytes (block 1), or the same for
  block 2. Then `0xf8c90` writes nothing and the pass keeps its previous base. **observed** (the store is conditional);
  the consequence is **open** (see T8.8 item 5).
- **A material can draw with textures that are not its own**, when an entry was the default at parse time and no
  refresh has replaced it. **inferred**.

### T8.6 What the size means

- **`block+0x38` = blob bytes + 32 * count** (`0xf5cd0` lines 39-40: `iVar5 = count*32 + param_2`, stored at
  `param_1[7]`). **observed**. Blob bytes are S1 (blob A, dword 10) and S2 (blob B, dword 11).
- **The GPU record is a 16-byte V#, not the gate value.** `0xf5cd0` line 69 calls `0x8be60(block+0x10, 0x1c, A, 1, 0x804,
  guest, 0)` with `A = (size + 15) & ~15`. `0x8be60` stores flags `0x804`, type `0x1c` and size at `block+0x28..+0x30`
  (`param_1[6..8]`), then for type `0x1c` calls `0x8655d0` (38 bytes), which writes the V#:

```c
struct MatBlockVSharp {            // at block+0x10 (+0x588 for block 1, +0x5c8 for block 2)
  u32 w0;   // base address low 32 bits = guest VA of the host copy (block+0x8)
  u32 w1;   // base address high 12 bits | stride << 16, stride = 0x10 (value 0x100000)
  u32 w2;   // num_records = A / 16
  u32 w3;   // 0x20077fac (constant written by 0x8655d0)
};
// base VA: guest = host - DAT_02ed2108 + DAT_02ed2110 (0xf5cd0 line 70)
```

- **The V# covers round16(S + 32*count) bytes.** `0xf8c90` copies `w0 | (w1 & 0xfff) << 32` into `pass+0xb0` (and the
  block-2 values into `pass+0xb8`): a 40-bit base address, not the V#. **observed**.
- **T# offset mismatch (observed arithmetic).** `0xf5cd0`, `0xf9ef0` and `0xf9c40` write T[i] at byte `S + 32*i`.
  `0xf8d10` (lines 122-125) writes at `base + stride*num_records - 32*count + 32*i`, which equals
  `round16(S+32*count) - 32*count + 32*i` = `round16(S) + 32*i`. The two agree only when `S % 16 == 0`. The shader's read
  offset is **open** (T1.7 item 1). One run can measure this (T8.9 item 4).

### T8.7 Port conclusion: the earliest point for the final T# set

- **Earliest:** the material parse has returned 1 (its blocks, counts and gates are final) and, for each entry `i`,
  `payload[i]` is a ready CTexture (`0x14cf00`) that is not the pool default (`DAT_02ee3ce0+0x340`). From then on the
  entry's T# words are `payload[i]+0xc4..+0xdc`. These are read once at `0x96680` and change only in dword 0 at
  residency (`0x881e60`, doc 02). So the T# type, format and number-format words are final at that point.
- **Where to read:**
  - list 1: array `*(void**)(mat+0x578)`, count `*(int*)(mat+0x5b4)`;
  - list 2: array `*(void**)(mat+0x5b8)`, count `*(int*)(mat+0x5f4)`;
  - per entry: `payload[i]+0xc4` (32 bytes, the T#).
  This avoids the block bytes, which can be stale and whose offset is ambiguous (T8.6).
- **Per-entry refresh:** rebuild the image key of an entry when `payload[i]` changes, which happens in `0xf8d10`
  (notification) and in `0xf9ef0` / `0xf9c40` (walkers). The port can hook `0xf8d10` to recheck entries. Do not key only
  at material load: at that time the entries may be the default.
- **Per-material key:** the pass binding uses the gate, so a pass-keyed prebuild should skip materials with
  `gate == 0` or `count == 0` and use the count to size the T# list.

### T8.8 Inferred and open items

1. **The pool default slot `DAT_02ee3ce0+0x340`** holds a loaded default texture (inferred from the branch of `0xf80f0`
   and T1.2). Who fills it at boot is **open**. The port should read it at run time rather than assume it.
2. **Stale default entries.** `0xf8d10` matches `entry.res == R`. An entry that stayed on the default would never match
   the real texture's notification. **inferred**, to check in the run (T8.9 item 4 logs the match result per entry).
3. **Who calls `0xf8d10` (vtable `+0x10`) and with what notify list.** **open**. The ELF gives only the slot.
4. **Triggers of the walkers `0x117160`, `0x117930`, `0xfb1e0`, `0xfb2d0`.** **open** (no direct caller found).
5. **The pass base when gate is 0.** The value left in `pass+0xb0` / `+0xb8` is not written by any path that was read.
   **inferred** that it is stale. Check the pass object's previous writers (T1.7 item 1).
6. **Which T# offset the shader reads** (`S` or `round16(S)`). **open**; depends on S mod 16, which the run measures.
7. **Retry after the factory returns null.** **inferred** (T8.3). Confirm in `loader.md`.
8. **Block 1 GPU record not released in `0xf8270`** before the rebuild. **observed** absence; leak or overwrite is **open**.
9. **`0xf9410` has no caller** and is not in the material vtable slots 0-11. How it is reached is **open**.

### T8.9 One-run log spec (no change to draws)

Use one area after its first load, about 60 seconds. Log each (material, block) once per change of its values.

1. **`0xf5cd0` exit.** Caller return address (`0xf8270` or `0xf6c90`), block address, S (param_2), count, gate. For each
   entry: `payload[i]`, `payload[i]+0x98` (state), `payload[i]+0x20` (ready), whether `payload[i]` equals the value at
   `DAT_02ee3ce0+0x340` (fallback flag), `payload[i]+0xc4..+0xdc`, and the entry's path (`res+0x70`, the `ResourcePath`
   string, T1.2 step 3).
2. **`0xf6c90` exit with value 1.** Material pointer, `+0x5b0`, `+0x5b4`, `+0x5f0`, `+0x5f4`, and S1 = `+0x478 - +0x470`,
   S2 = `+0x4b8 - +0x4b0`.
3. **`0xf8c90` / `0xf8c30` entry and exit.** Gates and counts; `pass+0xb0`, `pass+0xb8` before and after; `P+0xf4` (k).
   This shows the stale value when a gate is 0 (T8.8 item 5).
4. **`0xf8d10` entry.** The notify list (`*(param_2+8)`, count) and each R's path. Log which slot, list-1 index or list-2
   index matched (or none). **On exit**, for each block with gate > 0: `S % 16`, and the 32-byte words at `data + S + 32*i`
   and at `data + round16(S) + 32*i`, to see which offset holds the T#s (T8.6).
5. **`0xf9ef0` and `0xf9c40` entry.** Material pointer and gates. Count the calls per run and per walker.
6. **`0xf9410` return.** The u16 at `mat+0x630` and whether `0xfb120` was entered.
7. **`0x96680` exit (T# read).** The `.ctxr` path and the 32 bytes at `CTexture+0xc4`, to match each entry to its file.
8. **`0x8655d0` exit.** Words 0-3 of the V#, to confirm `w2 = A/16` and `w3 = 0x20077fac`.

Output of the run: one table per material with S1, c1, gate1, S2, c2, gate2, the fallback flag and match result of each
entry, `S % 16`, and the state of each entry at the first `0xf8c90` call for that material.

### T8.10 Evidence list

| Item | Address or file |
|---|---|
| Gate and count writer | `0xf5cd0` (`000f/000f5cd0.c` lines 23-40 and 69-70) |
| Build callers | `0xf6c90` lines 447 and 497 (`000f/000f6c90.c`); `0xf8270` lines 224 and 280 (`000f/000f8270.c`) |
| Early return | `0xf6c90` lines 205-208; factory `0xf9390` (`000f/000f9390.c`) |
| List entry resolver | `0xf80f0` (`000f/000f80f0.c`), not-ready branch |
| Ready check, payload, request | `0x14cf00` (`0014/0014cf00.c`), `0x14d7b0` (`0014/0014d7b0.c`), `0x95580` (`0009/00095580.c`) |
| Pass bind | `0xf8c90` (`000f/000f8c90.c`), `0xf8c30` (`000f/000f8c30.c`) |
| Refresh | `0xf8d10` (`000f/000f8d10.c`), `0xf9ef0` (`000f/000f9ef0.c`), `0xf9c40` (`000f/000f9c40.c`) |
| Mip registry | `0xf9410` (`000f/000f9410.c`), `0xfb120` (`000f/000fb120.c`), `0xfb1e0` (`000f/000fb1e0.c`), `0xfb2d0` (`000f/000fb2d0.c`) |
| Walkers | `0x117160` (`0011/00117160.c` line 131), `0x117930` (`0011/00117930.c` line 132) |
| GPU record | `0x8be60` (`0008/0008be60.c`), `0x8655d0` (`0086/008655d0.c`), `0x8be10` (`0008/0008be10.c`) |
| Vtables (ELF) | material `0x1597c48` (slot 2 = `0xf8d10`, slot 1 = `0xf6c50`, slot 7 = `0xf9cd0`); instance `0x1597c78` (slot 2 = `0xf8d10`) |
| Destructor body | `0xf6900` (`000f/000f6900.c`), releases blocks at `+0x578/+0x580/+0x5b8/+0x5c0/+0x5c8` |
| Excluded writers | `0x9aef0`, `0x3902b0`, `0x53fc0`, `0x433e0`, `0x43fb0`, `0x976db0`, `0x198bb0`, `0x25fbd0`, `0x36210` (other classes) |

## Blob A (T9)

Read-only static analysis of the EU 1.01 eboot (decomp only). No `.cmat` read, no game run. Addresses are eboot VAs;
line numbers refer to `out/re/decomp/<hi16>/<addr>.c`. Labels as above.

### A.1 Answer in short

- **Blob A is the raw file bytes of the material, copied once at parse and never patched.** Its byte count is dword 10
  of the `.cmat` stream; the bytes go to the byte vector at `+0x470/+0x478/+0x480`. Nothing writes those bytes after the
  parse, except the variant copy from the parent (`0xf8270`). **observed**.
- **Blob A is paired with list 1 exactly as blob B is paired with list 2.** Block 1 (`+0x578`) = blob A bytes at offset 0,
  then one 32-byte T# per list-1 entry, in list order. Block 2 (`+0x5b8`) = blob B bytes, then the list-2 T#s. The
  pairing is built in `0xf5cd0`, called from `0xf6c90` line 447 (block 1) and line 497 (block 2). **observed**.
- **No CPU-side consumer of blob A was found.** No function reads the blob A bytes, and no map, keyed setter or
  material-parameter API writes or reads `+0x470`. The only readers of `+0x470/+0x478` are the parser, the variant copy,
  and `0xf9c40` (which uses only the size). **observed** (section A.4).
- **So blob A reaches the shader only through block 1**, if the shader reads block 1 at all. Block 1 is a GPU buffer
  (section A.5), and its base is in `pass+0xb0`, written by `0xf8c90`. The draw-time reader of `pass+0xb0` was **not
  found**. Whether the shader reads blob A bytes, and as what, is **unknown** until the run in section A.7.

### A.2 How blob A is filled (`0xf6c90`)

Stream order (**observed**, `0xf6c90`), after list 2 (dword 9) and its entries:

| Dword | Decomp lines | Meaning |
|---|---|---|
| 10 | 395-396 | blob A byte count (`uVar22`) |
| 11 | 397-399 | blob B byte count (`uVar24`) |
| 12 | 498 | map 1 entry count; 8-byte records follow (503 onward) |
| 13 | 529 | map 2 entry count; 8-byte records follow (540 onward) |

- The count is read with `0x8eae0` (u32 little-endian, `0008/0008eae0.c`). The bytes are read with `0x8de50(stream, dst,
  n)` (`0008/0008de50.c`), raw and byte-exact. **observed**.
- Blob A: lines 401-440. The vector is grown with `0x17090` (malloc) when the capacity is short; when the new size is
  larger than the old one, the tail is zeroed with `import_memset`. The stored size is not a separate field: it is
  `end - begin` (`+0x478 - +0x470`). **observed**.
- **Size and alignment.** The stream has no padding, so the bytes sit at their exact file offset and their length is the
  dword value. The parser does not align them. In block 1 the T#s start at byte offset = blob A size (`0xf5cd0`,
  `puVar6 = lVar2 + param_2`), so the T#s are 32-byte aligned only when the blob size is a multiple of 32. The block host
  allocation is `0x12820(size, tag, 0x10, 0)`, so the block base is 16-byte aligned (**inferred**: the third argument is
  an alignment).
- `0xf5cd0` (`000f/000f5cd0.c`, 452 bytes), signature `(block, blob_size, blob, list)`: sets `block+0x00` to the payload
  array (one `0x14d7b0(res)` per list entry, after `0x95580`), `block+0x3c` to the entry count, `block+0x38` to
  `32*count + blob_size` (the size that gates `0xf8c90`), copies the blob bytes to `block+0x08` (host data), appends one
  T# per entry from `CTexture+0xc4..0xdc` through `0x8829e0(..., 0x10)`, and registers the block with `0x8be60`.

Call sites (**observed**):

| Call | Block | Blob | List |
|---|---|---|---|
| `0xf6c90` line 447: `0xf5cd0(obj+0x578, size_A, bytes_A, obj+0x490)` | 1 | A (`+0x470`) | 1 (`+0x490`, count dword 8) |
| `0xf6c90` line 497: `0xf5cd0(obj+0x5b8, size_B, bytes_B, obj+0x4d0)` | 2 | B (`+0x4b0`) | 2 (`+0x4d0`, count dword 9) |
| `0xf8270` line 224 (variant) | 1 | A (copied from parent) | 1 (copied from parent, `0xfaa20`) |
| `0xf8270` line 280 (variant) | 2 | B (copied from parent) | 2 |

### A.3 Blob A compared with blob B

| Item | Blob A | Blob B |
|---|---|---|
| File count | dword 10 (`0xf6c90` 395-396) | dword 11 (397-399) |
| Vector | `+0x470 / +0x478 / +0x480` | `+0x4b0 / +0x4b8 / +0x4c0` |
| Paired list | list 1, `+0x490` | list 2, `+0x4d0` |
| Block | `+0x578`; data `+0x580`; gate `+0x5b0`; count `+0x5b4` | `+0x5b8`; data `+0x5c0`; gate `+0x5f0`; count `+0x5f4` |
| Key-addressed patch | none found | map 1 (`+0x4f0`) via `0xf9cf0`: writes the given bytes into the blob B vector at the mapped offset |
| Copy to block host data after parse | none (the block is built once, in `0xf5cd0`) | `0xf9ef0` (lines 33-34) copies the blob B vector into block 2 data (`+0x5c0`) and rewrites its T#s |
| Variant copy | `0xf8270` 117-146 (copies the parent's bytes) | same pattern in `0xf8270` (T1.4) |

Map 1 node (**observed**, `0xf9cf0`): key u32 at node `+0x20`; u16 offset at `+0x28`, u16 size at `+0x2a` (the size
must equal the byte count given by the caller). Blob A has no equivalent map. **observed**.

### A.4 Every reader of `+0x470` / `+0x478` / `+0x480` in the material

Search: field grep over `out/re/decomp` for the offsets (decimal, and `[0x8e]`, `[0x8f]`, `[0x90]` indexed forms) in
files that also use material fields (`0x490`, `0x4b0`, `0x4d0`, `0x4f0`, `0x528`, `0x578`, `0x5b8`, `0x5b0`). 92 files
match the offsets alone; most are other structs (for example `0x97bb30` uses `+0x478` as an inline array with a counter
at `+0x4bc`; `0x7c27d0` does float arithmetic). Indexed forms: none on this object.

| Reader or writer | Decomp | Use | Label |
|---|---|---|---|
| `0xf6c90` | 395-440 | writes the bytes and the vector (growth 427-429, reset 406, 435-436) | observed |
| `0xf6c90` | 442-447 | size = `+0x478 - +0x470`; pointer = `+0x470`; passed to `0xf5cd0` | observed |
| `0xf8270` | 117-146 | variant: copies the parent's bytes (parent at `+0x638`) into the own vector | observed |
| `0xf8270` | 224 | variant: rebuilds block 1 from its own bytes | observed |
| `0xf9c40` | 20-21 | writes block 1 T#s into host data at `+0x580 + blob A size`; uses only the size | observed |
| `0xfb1e0` line 25, `0xfb2d0` line 22 | | call `0xf9c40` for each dirty instance (instance pool `+0x2008`) | observed |

No other reader. In particular there is no CPU read of the blob A bytes. The predicate `0xf9b20` reads `+0x198/+0x19c`
of the payloads, not blob A.

### A.5 Where blob A goes: the GPU buffer of block 1 and the pass pointer

- **Block 1 is a GPU buffer.** `0xf5cd0` calls `0x8be60(block+0x10, type 0x1c, aligned16(size), 1, 0x804, guest_addr, 0)`.
  Type 0x1c goes to `0x8655d0` (`0086/008655d0.c`), which writes the record at `block+0x10`: `+0x588` = guest address low
  dword; `+0x58c` = guest address bits 32..43 `| 0x100000`; `+0x590` = `(aligned size + 15) >> 4` (num_records);
  `+0x594` = `0x20077fac`. Bits 16..29 of `+0x58c` are the stride, so **the stride is 16 bytes**. **observed** (layout);
  the meaning of the words as a buffer V# is **inferred** from the bit fields.
- **`pass+0xb0` and `pass+0xb8` are raw base addresses**, not V#s. `0xf8c90` lines 7-9 and `0xf8c30` lines 6-8 write
  `pass+0xb0 = (+0x588) | ((+0x58c) & 0xfff) << 32` when `+0x5b0 != 0`; `pass+0xb8` uses `+0x5c8/+0x5cc` when
  `+0x5f0 != 0`. **observed**.
- **Callers of `0xf8c90`:** `0x1a8c60` (pass submit; `P+0xf4` = 1, then 6), `0x11d5b0`, `0x11d3d0`, `0x1809a0`,
  `0x17a580`, `0x1641d0`, `0x178cb0`, `0x5b7630`, `0xdb390`, `0x10b0a0`. Callers of `0xf8c30`: `0x1b32f0`, `0x1af430`.
  **observed**.
- **Where `pass+0xb0` is read: not found.** In `0x1a8c60` (358 bytes; `P` = `param_1`, binding = `param_3`) the value is
  stored by `0xf8c90` and not read in that function. The SGPR writes there are `0xae150` (writes the table entry
  `*(param_2+0x30) + u16*4`, not `P+0xb0`), `0x862510` (usage bytes from `ctx+0xc0c0/+0xc0d0`), `0x85fc10`, and `0x86b040`
  (draw). The pass submitters `0x94ec0/0x94dd0/0x94d00` do not read `+0xb0` either. `0xf8990` (called by `0x1a6d20`
  and `0x1a7290`) reads only `+0x70..+0x98` and `+0x560` of the material, not `+0x470` and not the blocks. `0x1a8180`
  reads and writes a field at `+0xb0` of a different object (`param_5`, from `0x19b330`); that is a name collision, not a
  proven reader. **open**.

### A.6 Finding: two refresh routines place the block 1 T#s at different offsets

- `0xf5cd0` (build) and `0xf9c40` (dirty-instance refresh of the host copy) put the T#s at **`blob size`** from the start
  of the block (a plain offset).
- `0xf8d10` (refresh when a list entry changes; block 1 at `+0x578`, block 2 at `+0x5b8`) writes the T#s at
  **`gpu + stride * num_records - 32*count`** (`0xf8d10` lines 118-125 for block 1, 172-179 for block 2). With stride 16 and
  `num_records = aligned16(size) / 16`, this is `gpu + aligned16(blob size)`. The two agree only when the blob size is a
  multiple of 16. **observed** (formula). **inferred** consequence: for a blob whose size is not a multiple of 16, a list
  change moves the T#s by `16 - size % 16` bytes compared with the build and with the dirty-instance refresh. Which
  placement the shader reads is **unknown**. Check in item 5 of A.7.
- Block 2 has the same split: `0xf9ef0` writes its T#s at the plain blob B size (lines 33-45), while `0xf8d10` uses the
  tail formula with `+0x5c8/+0x5cc/+0x5d0` (lines 172-179).

### A.7 One-run log spec (closes A.5 and A.6)

Each hook fires once per material or per event, with no change to draws.

1. **Blob A bytes and count.** At entry of `0xf5cd0` with block = `obj+0x578`: log `param_2` (size), the first 0x40 bytes
   of `param_3`, and the list count `(list[1] - list[0]) >> 4`. Dump the full blob A bytes to a local file (the user's own
   dump, local run only) for offline reading of the constants or names.
2. **Pass block pointer.** At exit of `0xf8c90` and `0xf8c30`: log the material pointer, `P+0xf4` (k), and the values
   written to `P+0xb0` and `P+0xb8`.
3. **First SGPR writer of the block pointer.** At entry of `0x862490`, `0x86d4c0` and `0x86d550`: when the 64-bit value
   equals a block base from item 2, log the return address, the stage and the slot. This names the caller that puts the
   block 1 pointer into user data, which closes A.5.
4. **Draw check.** At entry of `0x86b040`: log the user SGPR pair that holds the block 1 base, and the 0x40 bytes at that
   base. Compare them with item 1 (the first 0x40 bytes should match when block 1 is unchanged).
5. **Placement check.** At entry of `0xf8d10` and `0xf9c40`: log `mat+0x478 - mat+0x470`, `(mat+0x58c >> 16) & 0x3fff`,
   `mat+0x590`, and both candidate T# addresses (blob size, and `aligned16(blob size) - 32*count`). After each call, read
   the 4 dwords at both candidates and compare them with `CTexture+0xc4..0xdc`. This settles A.6 on a material whose blob
   size is not a multiple of 16.
6. **Optional, for the port.** Log the shader's `s_load` offsets into the block (from the shader dump) so that item 4's
   bytes map to a shader constant.

### A.8 Open items

1. The draw-time reader of `pass+0xb0` / `pass+0xb8` (A.5). Closed by item 3 of A.7.
2. Whether the shader reads blob A at all (A.1). Closed by items 4 and 6 of A.7.
3. Block 1 T# placement after a list change for blob sizes that are not multiples of 16 (A.6). Closed by item 5 of A.7.
4. The meaning of the GPU record words `+0x590` and `+0x594` as V# words 2 and 3 (A.5). Not checked.

## Shader-side offsets (T11)

Read-only, static. Question: are the offsets at which a shader reads its material T#s and constants fixed per program
(SGPR pair -> SRT chunk -> chunk[3] or chunk[4] -> T# at blob_size + 32*i)? No script was written, because the cache
data the question needs is not on disk (T11.2). Labels as above. Addresses are eboot VAs.

### T11.1 Answer in short

- **Not answerable from the data on this machine.** The chain (user SGPR -> chunk slot -> block offset) is recorded
  only in host-side `SotcSrtSource` entries (`flatten_extended_userdata_pass.cpp` lines 337-339, 1068-1070), which
  are not serialized into the shader meta. The meta keeps only the destination flat-buffer dword offsets of each
  sharp (`ImageResource::sharp_fetch.offsets`, `SharpLocation` = flat-buffer dword). **observed** (code).
- **So the cache cannot say whether the shader's source offset of an image T# is fixed per program.** It can say which
  flat-buffer slot holds each T# after the walker ran, which is a different question.
- **The chunk index is not checkable here either.** No `*.srtprogram.txt` exists (`DumpSrtProgram`, gated by
  `EmulatorSettings.IsDumpShaders()`, line 1142). No `SOTC_VSIZE_PROBE` log exists for the material programs.
- **Static facts that bear on the question:** the block's T# offset is `S + 32*i` at build time and
  `round16(S) + 32*i` at refresh (T11.4). They match only when S % 16 == 0. Whether real materials have S % 16 != 0
  is **unknown** (no `.cmat` on this machine).

### T11.2 What the cache holds, and why it does not answer the question

- Meta record (`SerializeShaderMeta`, `vk_pipeline_serialization.cpp` lines 304-316): u32 meta version, u32 binary
  version, u64 perm hash, perm index, `StageSpecialization`, then `Info::Serialize` (line 1174): the raw bytes of
  `InfoPersistent`, the flat buffer (`FlatBuf()`), `srt_info` (walker code), variant, has_discard, has_storage_images.
- `InfoPersistent` holds `buffers`, `images`, `samplers`, `fmasks` as `boost::container::static_vector` objects
  (`resource.h` lines 164, 250, 281, 290; capacities NUM_BUFFERS 40, NUM_IMAGES 64, NUM_SAMPLERS 16). They are
  written as raw memory, including the vector's own pointer and size fields (boost static_vector, 1.92 headers).
  Parsing them needs the exact byte layout of `BufferResource`, which was not derived. No parser was written, because
  a wrong layout would give wrong numbers. **open**.
- `ImageResource` (`resource.h` lines 172-186). Derived layout (**inferred** from declaration order and alignment, not
  checked against bytes): `SharpFetch<Image>` 56 bytes (Summary u8, immediates[8] u32, offsets[8] u16, load_mask u8),
  then is_depth, is_atomic, is_array, is_written, is_r128, constant_mip_index, mip_fallback_mode and post_op (one byte
  each), array_size u16 at +64, array_stride_dw u16 at +66; total 68 bytes.
- Pack: `sotc_pretranslate.pack` (1.1 GB; index format in the `tools/re/pixel_dupes.py` header) holds the same metas.
  The cache folder `user/cache/CUSA08809_v13272bcc` holds 2578 `.meta` files.

### T11.3 Constants and the block ranges (static)

- Block 2 constants are u16 byte offsets into blob B, written by map 1 (`0xf9cf0`, T1.4, T1.5). The constant span is
  [0, S2). The last constant's offset plus size gives a lower bound on S2, and it needs no walker data. **Not computed**:
  no `.cmat` and no per-material constant list on this machine.
- Block 1 (blob A, list 1): no consumer found (T1.5 item 3, still **open**). So no constant-based bound for S1.
- Chunk slot per block (T7.4, **observed** from the masks): block 2 is chunk[4] when block 1 exists, chunk[3] otherwise.
  This is a property of the material mask, so it is per material, not per program, unless every material of a program
  has the same pair of blocks (T7.7 item 2, **open**).

### T11.4 T# offset: S or round16(S)

- **Build (`0xf5cd0`, `000f/000f5cd0.c`).** The blob size `param_2` (= S, from the .cmat file, dword 10 or 11) is not
  rounded. The host buffer is `FUN_00012820(S + 32*c, ..., 0x10, 0)` (decomp line 37-38). The S bytes are copied from
  the blob, then the T#s go at `buffer + S + 32*i` (decomp line 57: `puVar6 = lVar2 + param_2`, `puVar6 += 4` per
  entry). So the build writes T[i] at **`S + 32*i`**. **observed**.
- **Size and V# (`0xf5cd0` line 69, `0x8be60`).** The V# covers `A = (S + 32*c + 15) & ~15` bytes, with num_records `A / 16`
  and stride 0x10 (T8.6). **observed** (decomp arithmetic).
- **Refresh (`0xf8d10`, lines 112-125).** The write base is `V#base + stride*num_records - 32*c + 32*i`, that is
  `V#base + A - 32*c + 32*i = V#base + round16(S) + 32*i` (32*c is a multiple of 16). So refresh writes T[i] at
  **`round16(S) + 32*i`**. **observed** (decomp), arithmetic **inferred**.
- **Agreement.** Build and refresh agree only when `S % 16 == 0`. When `S % 16 == 8`, refresh writes each T# 8 bytes
  past the build's position.
- **Which one the shader reads: unknown.** The shader reaches the T# through the block pointer (T7.5). Its byte offset
  is an immediate in the shader. It is fixed per program only if S is fixed per program. Not measured (T7.7 item 1,
  T8.8 item 6).
- **Overflow (inferred).** The host allocation is `S + 32*c` bytes, but refresh writes up to `A = round16(S + 32*c)`.
  When `S % 16 != 0`, refresh can write up to 15 bytes past the allocation. The allocator call takes an alignment of
  0x10, which may round the real allocation, so this is **inferred** only.
- **Port impact (observed in the port's design, T8.7).** The port reads T# words from `payload[i]+0xc4`, not from the
  block bytes. The port does not depend on this offset. The offset matters only to what the shader reads.

### T11.5 Can S % 16 be non-zero? (item 2)

- **Parser (`0xf6c90`, lines 400-447 and 489-497).** Blob A and blob B are read with exactly the file's size
  (`FUN_0008de50(param_2, ptr, size)`, size from the file). No rounding is applied to S before it is stored
  (`+0x478 - +0x470`, `+0x4b8 - +0x4b0`). **observed**.
- So the alignment of S is whatever the .cmat file stores. The engine does not enforce 16-byte multiples. **inferred**.
- **Evidence on this machine: none.** `C:/Users/ptr/Documents/CUSA08809` does not exist, no `.cmat` was found, and
  `user/` holds no material dump. **open**.
- **Consequence.** The mismatch is harmless only if every material's S is a multiple of 16. If not, refresh and build
  disagree for those materials, and the shader's T# can be stale or overwritten (T8.8 items 2 and 6).

### T11.6 What would settle it (one run, no code change)

1. Read S1 and S2 for every material of one area: `material+0x478 - +0x470` and `+0x4b8 - +0x4b0` after `0xf6c90`
   returns 1 (T8.9 item 2). Count the cases with `S % 16 != 0`.
2. Shader side: run once with the shader-dump setting on, so `DumpSrtProgram` writes `*.srtprogram.txt` for the block
   programs. The `s_load` immediates of the block chain are the shader's byte offsets (T7.7 item 7).
3. Or with `SOTC_VSIZE_PROBE=1`: the provenance list gives, per flat-buffer dword, the path (root SGPR, pointer offsets,
   source dword). That is the table T11 needed; it gives the chunk slot and the offset per program.
4. Compare the offset from (2) or (3) with S from (1) for the same material. Equality with `S + 32*i` or with
   `round16(S) + 32*i` settles T11.4.

### T11.7 Answers to the question

- **Can the port compute "material T# i -> shader image index j" for a (program, material) pair at load?** Not from
  the cache, and not from the static code alone. The cache has the destination slots only; the source offsets are
  host-only. **observed**.
- **Must it learn it at the first draw?** Probably, until (1) and (3) of T11.6 run. The first draw of a material can
  read the block pointer and the chunk slot (T7.4) and match the T# words against `payload[i]+0xc4`, the same words
  the block holds at build time. **inferred**, not implemented.
- **Fixed per program?** Unknown. The chunk slot depends on the material mask (T7.4). The block's T# offset depends on
  S, which varies per material (T11.4). The static code gives no guarantee that the offset is fixed per program.
  **inferred**.

### T11.8 Evidence list

| Item | Address or file |
|---|---|
| Build copy: T# at S + 32i, block size S + 32c | `0xf5cd0` (`000f/000f5cd0.c` lines 23-40, 57, 69-70) |
| Refresh: T# at round16(S) + 32i | `0xf8d10` (`000f/000f8d10.c` lines 112-125) |
| Blob size read, unrounded | `0xf6c90` (`000f/000f6c90.c` lines 400-447, 489-497) |
| Meta record layout | `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp` lines 304-316, 373-400, 1174-1190 |
| Resource structs | `gpu/shadps4/shader_recompiler/resource.h` lines 20-130 (SharpFetch), 172-186 (ImageResource), 164/250/281 (lists) |
| Host-only source provenance | `gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp` lines 337-339, 361-368, 1068-1070 |
| SRT dump gate | `flatten_extended_userdata_pass.cpp` line 1142 (`IsDumpShaders`) |
| Serialization primitives | `gpu/shadps4/common/serdes.h` (Writer/Reader memcpy; containers as size plus elements) |
| Pack format | `tools/re/pixel_dupes.py` header; `vk_pipeline_serialization.cpp` lines 953-1130 |
| Boost static_vector layout | `C:/msys64/clang64/include/boost/container/static_vector.hpp` (storage first, then vector members) |

## Stages with the SRT chunk (T12)

Read-only analysis (agent T12). Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`; eboot bytes read from
`out/eboot.elf` with a small ELF segment reader; stored `.csdr` records of the user's dump (`pak0..pak3`, `pakN`,
`cachedFiles` in `C:/Users/ptr/Documents/SOTC/CUSA08809`, read through `tools/csdr/psarc.py`, nothing written). Scratch
scripts (not in the repo): `scratchpad/t12/va.py`, `scratchpad/t12/csdr_scan.py`, `csdr_scan2.py`, `csdr_scan3.py`.
Labels: **observed** (decomp, eboot bytes or data, address given), **inferred**, **unknown**.

### T12.1 Short answer

- **Byte +0x19 is not a usage id and not a generic SRT flag.** It is the high byte of a u16 in the `.csdr` file record:
  `R+0x18` (= layout `+0x10`) holds the user SGPR index in its low byte and the dword count in its high byte. The test
  `== 2` means "this program's pointer slot is a 64-bit pointer". `0xffff` means no slot. **observed** (`0x862490`,
  `0x86d5e0`, data in T12.4).
- **The byte marks the program's single extended-user-data usage slot.** In the shipped data it equals 2 exactly when
  the footer has `is_extended_usage_info = 1` and `num_input_usage_slots = 1`. The name of the usage type (kPtrExtendedUserData
  in the Orbis convention) is **inferred**; the correlation is **observed**.
- **Stage codes** (observed): 0 CS, 1 PS, 2 VS, 3 GS, 4 ES, 5 HS, 6 LS. K2 section 5 is corrected by the descriptor bytes (T12.3).
- **The chunk base goes to user SGPR 0..1** of every stage whose record has the slot: VS register `0x2C4C`, PS `0x2C0C`.
  In the shipped data the SGPR index is 0 in every case where the slot is present. **observed**.
- **Shipped data, programs of the common two-record shape (VS record 0 + PS record 1):** VS has the slot in 6894 of 6894
  programs. PS has it in 2716 of 6894 (39 %). The other PS programs have no slot (3596 with the SRT flag set, 582 with none).
  **observed** (stored files only, see T12.4).
- **Both VS and PS get the same chunk.** The chunk holds the item's pointer slots selected by the material mask
  (`material+0x468`, T12.5). Which entries a shader loads is decided by its own code, which is not decoded here
  (**unknown**). The chunk entries are: mesh V# table at chunk[0] (when bit 0 is set), material block 1 at chunk[3] and
  block 2 at chunk[4] (typical mask, both blocks present).

### T12.2 The record fields around +0x19

Let `R` be the `.csdr` file record (`R = file + 4 + 228*i`, parser `0xb0ced0`, which does not copy the record; the
record stays in the file buffer, `FUN_00b0ced0` lines 91-96). The binders read the layout `L = R+8` (`0x937c0` passes
`*rec + 8`). Fields read by the SGPR writers (**observed**, decomp of `0x85f410`, `0x862490`, `0xae150`):

| File record offset | Layout offset | Reader (eboot VA) | Meaning |
|---|---|---|---|
| `R+0x08` (u16) | `L+0x00` | `0x85f410` line 240 | dword count of the stage-array copy (T3 section 4.3) |
| `R+0x0e` (byte) | `L+0x06` | `0x85f410` line 130 | SGPR index of a pointer write (`0x86d4c0`), `0xff` = none |
| `R+0x0f` (byte) | `L+0x07` | `0x85f410` lines 73, 157, 242 | SGPR index of a pointer write (`0x86d4c0`), `0xff` = none |
| `R+0x15` (byte) | `L+0x0e` | `0x85f410` lines 114, 283 | SGPR index of a 1-dword write to `O+0xc108` / `O+0xc104` |
| `R+0x18` (u16) | `L+0x10` | `0x862490` (`uVar1 = *(u16*)(layout+0x10)`) | low byte: user SGPR index; high byte: dword count |
| `R+0x19` (byte) | `L+0x11` | `0xae150` lines 14, 22, 30, 38, 46, 54 | high byte of the `R+0x18` word; test `== 2` |

- `0x862490` (126 bytes, `0086/00862490.c`) reads `uVar1 = *(u16*)(layout+0x10)`, calls `0x86d5e0(cmd, stage, uVar1 & 0xff,
  src, uVar1 >> 8)`, and for stage 2 with `O+0xc613 != 0` also `0x884240` on the mirror layout `O+0xc1b8`. The decompile
  shows three parameters, so the `2` that `0xae150` passes as the fourth argument is not used here: the count comes from
  the layout. **observed**.
- `0x86d5e0` (322 bytes, `0086/0086d5e0.c`) writes the PM4 header `0xc0007600 | count << 16`, the register
  `sgpr + table[stage]` (`0x2c00` based), then `count` dwords (cases 1 and 2 copy one and two dwords; the switch falls
  through). So `count` is the number of data dwords; 2 is a 64-bit pointer. **observed**.
- Register table read from the eboot at `0x1290410` (u16 per stage code): CS `0x2E40`, PS `0x2C0C`, VS `0x2C4C`,
  GS `0x2C8C`, ES `0x2CCC`, HS `0x2D0C`, LS `0x2D4C`. **observed**.
- `0xae150` reads `*(u8*)(*R + 0x19)`: the first dereference is the 32-byte runtime record `R` (from `P+0x1d8+8*s`), the
  second is the file record pointer `*R`. So the tested byte is in the `.csdr` file record, not in the blob header.
  **observed** (decomp line 14: `*(char*)(*(long*)param_1[..] + 0x19)`).
- Do not confuse with `0x862510` (`0086/00862510.c`): it reads `H+0x19` of the blob header `H = *(R+8)` (held at
  `O+0xc0c0` / `O+0xc0d0`) as two nibbles for the vertex and instance SGPRs, and `H+0x27` for VS. That is a different
  object and a different use. **observed**.

### T12.3 The stage loop of `0xae150` and the descriptor table

`0xae150` (441 bytes, `000a/000ae150.c`), with `P` the draw item (T4.4) and `D = P[4]` the stage descriptor (`X+0x20`,
`0xf8aa0`):

```c
for (k = 0; k < 6; k++)  if (P->mask & (1 << k)) {            // mask = P+0x18 (only bits 0..4 occur)
    s = *(u32*)(D + 4*k);                                       // stage code of bit k
    R = P[0x3b + s];                                            // P+0x1d8 + 8*s, the 32-byte runtime record
    if (R && *(u8*)(*(u64*)R + 0x19) == 2)                      // file record byte
        FUN_00862490(O, s, &chunk, 2);                          // O = *P + 0x2e8; chunk = 8-byte pointer
}
```

Writers of `P[0x3b+s]`: `0x937c0` (stage codes 2, 4, 6 at `D+0x00`) and `0x94ab0` (PS at `D+0x10`, index 1).
**observed**. The bit-to-stage map comes from the descriptor dwords (eboot bytes):

| Mask | Descriptor (VA) | bit 0 | bit 1 | bit 2 | bit 3 | bit 4 |
|---|---|---|---|---|---|---|
| 17 (0x11) | `0x1604370` | 2 VS | - | - | - | 1 PS |
| 25 (0x19) | `0x1604390` | 4 ES | - | - | 3 GS | 1 PS |
| 23 (0x17) | `0x16043b0` | 6 LS | 5 HS | 2 VS (as DS) | - | 1 PS |
| 31 (0x1f) | `0x16043d0` | 6 LS | 5 HS | 4 ES (DS as ES) | 3 GS | 1 PS |
| 32 (0x20) | `0x16043f0` | none | | | | |

- The `D` values are dword arrays: `D[k]` is the stage code of bit `k`. **observed** (bytes).
- K2 section 5 guessed the middle bits; it had VS for bit 2 of mask 31. The bytes say ES (code 4). K2 section 9's open
  item "meaning of D+0x04..+0x0f" is answered by this table. **observed**.
- The stage codes index the per-stage record slots `O+0xc0a0 + 8*code` (CS `c0a0`, PS `c0a8`, VS `c0b0`, GS `c0b8`,
  ES `c0c0`, HS `c0c8`, LS `c0d0`), the same slots K2 lists for the setters. **observed**.
- `0xae150` tests the byte per bit; a program without the byte is skipped silently. No flag bit of the item is
  involved. **observed**.

### T12.4 Which programs carry the byte (shipped data, read-only scan)

Scan of the stored `.csdr` files (trailer descriptor type 0; 15,072 files). Types 3 and 4 (Oodle, 19,320 files) were not
decompressed, so the counts are a subset. Footer fields follow `regs_shader.h` `BinaryInfo` (footer at the `OrbShdr`
signature `p`: type = `(u32 at p+8) >> 2 & 0xf`, `chunk_usage_base_offset_in_dw` at `p+12`, `num_input_usage_slots`
at `p+13`, flag byte at `p+14`: bit 0 `is_srt`, bit 2 `is_extended_usage_info`). **observed** (counts, scripts above).

Two-record programs (VS record 0 and PS record 1 of a mask-17 material), 6894 files:

| Record | Footer type | `is_srt` | `is_extended` | `num_input_usage_slots` | `R+0x19` | Count |
|---|---|---|---|---|---|---|
| 0 | 1 (vertex) | 1 | 1 | 1 | 2 | 6894 |
| 1 | 0 (pixel) | 1 | 1 | 1 | 2 | 2716 |
| 1 | 0 (pixel) | 1 | 0 | 0 | 255 | 3596 |
| 1 | 0 (pixel) | 0 | 0 | 0 | 255 | 582 |

- The byte is 2 exactly when the footer has one extended usage slot. `is_srt` alone does not separate the cases (it is 1
  for 3596 PS programs without the slot). **observed**.
- Single-record programs: compute (type 4) has the byte 2 in 7810 of 7940 files (compute does not go through `0xae150`).
  PS-only files (type 0) have it in 116 of 154. Types 2, 7 and 8 (geometry and tessellation, per `predict_table.py`)
  appear in 10 single-record files, all with 255; too few to conclude. **observed** (counts only).
- In the shipped data the SGPR index (low byte of `R+0x18`) is 0 in all 9,610 programs where the slot is present.
  **observed**.
- Port side: the flags are already in the footer that the port parses (`regs_shader.h` lines 16-31, read through
  `SearchBinaryInfoCached`). So the port can decide, per program at load, whether the program receives the chunk without
  reading the `.csdr` record. **inferred** from the 100 % correlation above.

### T12.5 Chunk contents: which item pointers and in which order

- `0x95320(mask, &P[0x12], pool)` (252 bytes, `0009/00095320.c`) copies `P[0x12+k]` (that is `P+0x90+8k`) for each set
  bit `k` of `mask`, in bit order, into a new array (bits 0..9). **observed**. Callers pass `*(u32*)(material+0x468)`
  (`0x1809a0` line 13, `0xdb390` line 104). The return is the chunk; the callers use it as the value for `0xae150`.
- The mask `material+0x468` is written by the parser `0xf6c90` lines 575-626 (`000f/000f6c90.c`). **observed**:
  - `uVar17 = 1`, or `0x41` for material types 4 and 7; `|= 0x200` for types 1, 4 and 7 (bit test `0x92 >> type`);
    `|= 0x80` when `material+0x464 & 0x40`.
  - `mask = uVar17 | 0x10a` (bits 1, 3 and 8 always), then `| 0x10` (bit 4) when `material+0x5b0 != 0` (block 1
    size), then `| 0x20` (bit 5) when `material+0x5f0 != 0` (block 2 size).
- Typical material, both blocks: mask `0x13b`, bits 0, 1, 3, 4, 5, 8. Chunk:

```
chunk[0] = P+0x90   mesh V# table pointer (bit 0; item builders 0x1122d0 / 0xdb390 / 0x10b0a0, T4.3)
chunk[1] = P+0x98   bit 1, writer not found in the builders searched
chunk[2] = P+0xa8   bit 3, written by 0x10b0a0 line 62, 0xdb390 line 80, 0x10c2e0 line 46
chunk[3] = P+0xb0   bit 4, material block 1 (0xf8c90, gated by +0x5b0), T1
chunk[4] = P+0xb8   bit 5, material block 2 (0xf8c90, gated by +0x5f0), T1
chunk[5] = P+0xd0   bit 8, read by 0x18d6f0 line 372, writer not found in the builders searched
```

- The index depends on the mask: with block 1 absent, block 2 is `chunk[3]`; with bit 0 absent (a material type branch
  that sets `uVar17 = 0`), every index shifts down by one. **observed** (mask rule), **inferred** (index effect).
- `0x18d6f0` (2588 bytes, caller `0x183d10`, `0018/0018d6f0.c`) builds a 4-pointer block from `P+0x98`, `P+0xa8`,
  `P+0xb0`, `P+0xd0` (lines 367-372, the mask `0x11a` order) and sends it through `0x862490` to each stage with the byte.
  It also writes `P+0xb0` itself (`param_2[0x16] = lVar8`, a 0x270-byte block from `FUN_0008c0d0`, line 367). So
  `P+0xb0` has two writers: `0xf8c90` (material block 1) and `0x18d6f0` (a per-item block). Which one a given draw uses
  is **open** (T12.7 item 4). **observed** (writes); the conflict with T1 is new.

### T12.6 Answers per stage

- **VS:** receives the chunk pointer in `s[0:1]` (register `0x2C4C`) in every shipped two-record program (6894 of 6894).
  **observed**. It reads the mesh V# table through `chunk[0]` when bit 0 is set (the default). **inferred** (T4.6: two
  loads, `s[0:1] -> chunk -> table -> V#`; the fetch shader's first `s_load` is the check, T4.10 step 5). The VS can also
  reach `chunk[3]` and `chunk[4]` (the material blocks, for example vertex texture fetch or wind constants): the same
  pointer is in its user data. Whether any VS code does so is **unknown**; no VS was decoded.
- **PS:** receives the chunk pointer in `s[0:1]` (register `0x2C0C`) only in 39 % of two-record programs (2716 of 6894).
  **observed**. Those are the only PS programs that can reach the material blocks through the chunk. **inferred**: the
  material textures go through the chunk (T1), and the globals go through the stage-array flush of T3, not the chunk.
  The other PS programs have no extended slot; they may still take resources through other direct SGPRs, which are not
  decoded here (**unknown**).
- **ES, GS, HS, LS:** get the chunk on the same rule when their record has the byte. Not measured (the scan covers only
  programs of 1 and 2 records). **unknown**.

### T12.7 Open items and one-run log spec

1. **Draw-time SGPR check.** At `0x862490` entry log `stage` (`param_2`), the u16 at `*(O+0xc180+8*stage)+0x10`, and the
   8 bytes at `*param_3`. Expect `0x0200` for VS and for PS in the 39 % case, and `chunk[0] == P[0x90]` (the mesh table,
   `mesh+0x70+0x2c0`) when bit 0 is set. At the first draw of a mesh, read user SGPRs 0 and 1 of the VS and PS (PM4
   `SET_SH_REG` at `0x2C4C` / `0x2C0C`) and compare.
2. **Mask per material.** At `0x95320` entry log `*(u32*)(material+0x468)` with the material identity. Expect
   `0x10b`, `0x11b`, `0x13b` or `0x12b`, plus the `0x41`/`0x201` variants for types 4, 7 and 1. This settles the chunk indices.
3. **Which entries a shader reads.** In the port, log the SRT walker input (`user_data[0..1]` and the dword offsets the
   walker follows, `ir/passes/srt.h`, `info.h` `ReadUdReg` at line 371) for one VS and one PS. Offsets 0 (table), 3 and 4
   (blocks) show what each stage uses. This is the only way to settle the VS and PS question for real shaders.
4. **Writers.** Find the writers of `P+0x98` (bit 1) and `P+0xd0` (bit 8), and decide which of `0x18d6f0` and `0xf8c90`
   writes `P+0xb0` for each draw (T12.5). Log `P` and the caller of `0x18d6f0` at the start of `0x183d10`.
5. **Static check (done for stored files).** Repeat with `scratchpad/t12/csdr_scan3.py`; extending it to the Oodle-packed
   `.csdr` (types 3 and 4) needs the game's own decoder, which the port does not have, so this stays open.

### T12.8 Proven, inferred, unknown (T12)

| Item | Status |
|---|---|
| `R+0x19` is the high byte of the `R+0x18` u16; test `== 2` is a 64-bit pointer slot | observed (`0x862490`, `0x86d5e0`, `0xae150`) |
| Stage codes 0..6 and register table | observed (`0x1290410`, `0x1604370..0x16043f0`) |
| K2 bit-to-stage map (middle bits) corrected; mask 31 bit 2 is ES | observed (descriptor bytes) |
| Chunk goes to SGPR 0..1 of VS and PS when present | observed (data SGPR index 0; register table) |
| Byte 2 iff footer `is_extended_usage_info` and `num_input_usage_slots == 1` | observed (shipped stored files) |
| The usage type is a pointer to an extended user-data table (kPtrExtendedUserData) | inferred (Orbis convention; not in repo) |
| VS reads chunk[0] (mesh table) | inferred (T4.6) |
| PS reads chunk[3] / chunk[4] | unknown (shader-dependent; 39 % of PS carry the slot) |
| Writers of `P+0x98`, `P+0xd0`; which writer of `P+0xb0` is live | unknown |
| The port does not parse the OrbShdr input usage table | observed (grep of `gpu/shadps4`, `gpu/sotc`: no `InputUsage`, `usage_type` or `kPtr*`) |
