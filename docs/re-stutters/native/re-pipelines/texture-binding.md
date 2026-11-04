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
