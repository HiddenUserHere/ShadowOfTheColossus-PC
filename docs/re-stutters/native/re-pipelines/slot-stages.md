# Slot stages: which programs a pass binds per material slot (RE, topic K2)

Read-only static analysis of the EU 1.01 eboot (Ghidra export in `out/re/decomp/<hi16>/<addr>.c`; first line of
each file lists callers). Descriptor bytes were read from `out/eboot.elf` with a small ELF reader (no game dump, so
no `.cmat` or `.csdr` byte was read). Addresses are eboot VAs.
Labels: **proven** (decomp read, address given), **inferred** (reasoning, not checked against data), **unknown**.
Builds on `pass-map.md` (R8), `materials-meshes.md` (R3), `shader-collections.md` (R1). Where R8 is wrong, section 6
says so.

## 1. Answer in short

- **The 5-bit mask is the second u32 of each slot entry in the .cmat.** It is stored at `B+0x38` (not a "kind").
  Accepted values are 17, 23, 25, 31 (bitmask `0x82820000`); anything else becomes 17. **proven** (`0xf6c90` lines
  126-133, 232-241; zeroing and default 17 in `0xf6190`).
- **Bit i set means record pointer `B+0x10+8*i` is filled.** It points to `.csdr` record
  `n = popcount(mask & ((1<<i)-1))` (the pointers are compacted in bit order). **proven** (`0xf6c90` lines 209-231).
- **The mask also selects the stage descriptor `D` of the pass:** `0xf8aa0(X, mask)` writes `X[4] = D`, with
  D chosen by the mask (table in section 3). `D+0x00` is the stage code of record 0; `D+0x10` is a storage index for
  record 4. **proven** (`0xf8aa0` tail; bytes read from the eboot).
- **Stage codes:** `D+0x00` = 2 selects the VS setter, 4 the ES setter, 6 the LS setter (`0x937c0`). Record 4 always
  goes to the PS setter (`0x94ab0` -> `0x861600`). **proven**
- **Per mask, record 0 is VS (17), ES (25) or LS (23, 31); record 4 is PS (all four).** **proven** for the codes,
  inferred for the mapping of the middle bits (section 5).
- **The draw path `0xf8990` binds only `B+0x10` and `B+0x30`.** Records at `B+0x18`, `B+0x20`, `B+0x28` (bits 1-3) have
  no reader in the material resolver. **proven** (field use in `0xf8990`, and no other reader of the 0x48-stride
  slot array in the decomp folders searched).
- **Sub-materials (`+0x600`) are not in the .cmat.** They are runtime instances created per render item by `0xf9890`,
  with ids from a 256-bit allocator. **proven** (creation and allocator); the draw-time choice of an instance is
  partly open (section 4).

## 2. The slot entry in the file and the slot record pointers

Per slot entry (`0xf6c90` lines 129-133, reads in this order with `0x8eae0`):

```
u32 k          slot index (0..13), uVar22
u32 mask       5-bit record mask, uVar5 (B+0x38 after the check)
string path    .csdr name, FUN_0014c3f0; resolved by FUN_000fa1a0 and FUN_0014d7b0 (lines 135-205)
```

Slot record pointers (`B = material + 0x60 + 0x48*k`, **proven**; `0xf6c90` lines 209-241):

| Field | Offset from B | Written by | Content |
|---|---|---|---|
| `res` | `+0x00` | line 190 | `.csdr` resource ref (Shader object) |
| `rec[0]` | `+0x10` | line 211 if bit 0 | `Shader.records + 32*0` |
| `rec[1]` | `+0x18` | line 215 if bit 1 | `records + 32*popcount(mask&1)` |
| `rec[2]` | `+0x20` | line 222 if bit 2 | `records + 32*popcount(mask&3)` |
| `rec[3]` | `+0x28` | line 227 if bit 3 | `records + 32*popcount(mask&7)` |
| `rec[4]` | `+0x30` | line 230 if bit 4 | `records + 32*popcount(mask&15)` |
| `mask` | `+0x38` | line 233 (valid) or 241 (0x11) | the mask (17 by default) |
| `interned` | `+0x40` | line 235-237 if `mask & 6` | `FUN_00099720(DAT_02ed37b0, rec[0], rec[1])` |

`records` is `Shader+0x60` of the loaded `.csdr` (`FUN_0014d7b0` result, line 205); record size 32 bytes (R1
section 2b). `0xf6190` (line 55-68 and the slot clear) zeroes `B+0x10..+0x30` and sets `B+0x38 = 0x11` before the
slot is filled, so an unset bit leaves a NULL pointer. **proven**

Mask-to-record table (**proven** from the bit rule, `records` index in brackets):

| Mask | Bits set | rec at B+0x10 | rec at B+0x18 | rec at B+0x20 | rec at B+0x28 | rec at B+0x30 |
|---|---|---|---|---|---|---|
| 17 (0x11) | 0, 4 | [0] | - | - | - | [1] |
| 23 (0x17) | 0, 1, 2, 4 | [0] | [1] | [2] | - | [3] |
| 25 (0x19) | 0, 3, 4 | [0] | - | - | [1] | [2] |
| 31 (0x1f) | 0, 1, 2, 3, 4 | [0] | [1] | [2] | [3] | [4] |

Note: `B+0x30` is the record for bit 4, not `.csdr` record 4, except for mask 31. The port must use the compacted index.

Presence (**proven**, `0xf8250`): slot k exists iff `B+0x10 != 0`. Since bit 0 is set in all four valid masks,
this is also "the slot has record 0".

## 3. The mask -> stage descriptor D (`0xf8aa0`, `0xf8990`)

`0xf8990(material, pass)` (decomp `000f/000f8990.c`): with `X = *pass` (the pass's first qword; `X[0]` is the render
context `ctx`, so `X` is a per-pass bind object, not the context), `k = pass+0xf4`:

```c
FUN_000f8aa0(X, *(u32*)(B+0x38));       // mask: picks D = X[4], usage, state (section 3.1)
FUN_000937c0(X, *(u64*)(B+0x10));       // record 0, stage code D+0x00
FUN_00094ab0(X, *(u64*)(B+0x30));       // record at bit 4, storage index D+0x10, always PS
```

Note: R8 wrote "`pass[4]`" for `X[4]`. The stage descriptor is `X+0x20`, and `X` is `*pass`.

### 3.1 `0xf8aa0`: the mask chooses D (proven)

Switch on the mask (decomp `000f/000f8aa0.c`, 387 bytes). Cache: if `X[3] == mask` and dirty bit `X+1 & 1` is 0,
return. Otherwise each case sets `ctx+0xc548` (user-data usage id), calls `0x86d160(ctx, usage)`, and at the end
`X[4] = *(PTR)`:

| Mask | Case | `ctx+0xc548` | Table slot (eboot VA) | D (VA) | `D+0x00` (rec0 stage code) | `D+0x10` (rec4 index) |
|---|---|---|---|---|---|---|
| 17 (0x11) | `0x11` | 0 | `0x1604408` | `0x1604370` | **2** (VS) | 1 |
| 23 (0x17) | `0x17` | 0x45 | `0x1604418` | `0x16043b0` | **6** (LS) | 1 |
| 25 (0x19) | `0x19` | 0xb0 | `0x1604410` | `0x1604390` | **4** (ES) | 1 |
| 31 (0x1f) | `0x1f` | 0xad | `0x1604420` | `0x16043d0` | **6** (LS) | 1 |
| 32 (0x20) | `0x20` | (flag `X+0x14` = 1) | `0x1604428` | `0x16043f0` | -1 (unused) | - |

The `0x20` case is not reachable from a parsed material (the parser maps it to 17). Values read with a small ELF
reader on `out/eboot.elf` (same result on `out/re/ghidra/eboot_ghidra.elf`). **proven**

Consistency check (**inferred**): the usage ids match the stage of record 0. Usage 0 is used for mask 17 (VS slot
`O+0xc0b0`); usage 0xb0 for mask 25 (ES slot `O+0xc0c0`); usage 0x45 and 0xad for masks 23 and 31 (LS slot
`O+0xc0d0`). `0x862510` reads usage 0xb0 from `O+0xc0c0` and 0x45/0xad from `O+0xc0d0`, as R3 notes (**proven** for
those reads). This supports the stage codes in the table above.

### 3.2 Record 0 setter: `0x937c0` (proven)

```c
void FUN_000937c0(long *X, long *rec) {            // decomp 0009/000937c0.c, 124 bytes
  uVar1 = *(uint *)X[4];                           // D+0x00
  X[uVar1 + 0x3b] = rec;                           // X+0x1d8 + 8*code
  if (uVar1 == 2) FUN_00861520(*X + 0x2e8, rec->blob, 0, 0, rec->res + 8);   // VS
  if (uVar1 == 6) FUN_00861260(*X + 0x2e8, rec->blob, 0, 0, rec->res + 8);   // LS
  if (uVar1 == 4) FUN_00860bb0(*X + 0x2e8, rec->blob, 0, 0, rec->res + 8);   // ES
  // other codes: only the store above
}
```

`rec->blob` is `rec+0x08` (blob header; `param_2[1]`), `rec->res` is `rec+0x00` (file record). **proven**

### 3.3 Record at bit 4: `0x94ab0` (proven)

`0x94ab0` (decomp `0009/00094ab0.c`, 300 bytes) stores `X[(D+0x10) + 0x3b] = rec4` (`D+0x10` = 1 in all four
masks, so `X+0x1e0`), then always calls `FUN_00861600(*X + 0x2e8, rec4->blob, rec4 + 8)`, then if the blob flag
byte `blob+0x38` is set, runs `0x87f790` and `0x86bc00`. The stage is always **PS**: `0x861600` writes the PS
field. **proven**

## 4. Stage setters and the hardware stage (proven for the Gnm names; SH index from the port's register table)

Setter base `O = *X + 0x2e8` (the `Material_BindGraphicsShaders_B` object, `ctx+0x2e8`). Each setter stores the blob
header in a field of `O` and sets a dirty byte. Emit: `0x85fc10` (`Material_BindGraphicsShaders_B`, 3,994 bytes,
called from `0x93690` and the other pass submitters).

| Setter (decomp) | Field `O+` | Dirty byte `O+` | Emit in `0x85fc10` | Gnm call | Hardware stage | SH reg (`mmSPI_SHADER_PGM_LO_xx`) |
|---|---|---|---|---|---|---|
| `0x861520` (stage code 2) | `0xc0b0` | `0xc24a` | line 368-380: `0x86c170` / `0x86c250` | `sceGnmSetVsShader` / `sceGnmUpdateVsShader` | VS | `0x2C48` |
| `0x860bb0` (code 4) | `0xc0c0` | `0xc24c` | line 33-51: `0x86c2c0` | `sceGnmSetEsShader` | ES | `0x2CC8` |
| `0x861260` (code 6) | `0xc0d0` | `0xc24e` | line 222: `0x86c760` | `sceGnmSetLsShader` | LS | `0x2D48` |
| `0x861600` (rec at bit 4) | `0xc0a8` | `0xc249` | line 388-395: `0x86c050` / `0x86c110` | `sceGnmSetPsShader350` / `sceGnmUpdatePsShader350` | PS | `0x2C08` |
| `0x860c50`, `0x860ea0` | `0xc0b8` | `0xc24b` (not verified) | line 53-64: `0x86cb00` / `0x86cb60` | `sceGnmSetGsShader` / `sceGnmUpdateGsShader` | GS | `0x2C88` |
| `0x8612b0`, `0x8613e0` | `0xc0c8` | `0xc24d` | line 239-242: `0x86c330` / `0x86c460` | `sceGnmSetHsShader` / `sceGnmUpdateHsShader` | HS | `0x2D08` |
| `0x861730`, `0x861880` | `0xc0d8` | `0xc24f` / `0xc257` | `0x85f220` path, `0x86c7d0` | `sceGnmSetCsShaderWithModifier` | CS | (compute) |

- Dirty bytes: `0x861520` sets `O+0xc24a` (decomp line 5), `0x860bb0` sets `O+0xc24c`, `0x861260` sets `O+0xc24e`,
  `0x861600` sets `O+0xc249`, `0x8612b0` / `0x8613e0` set `O+0xc24d`. **proven**. `0x860c50` / `0x860ea0` write
  `O+0xc0b8` (proven), but I did not verify which dirty byte they set (`O+0xc24b` is read by `0x85fc10` line 53).
- SH register indexes: from `gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h` (lines 6249-6254): ES `0x2CC8`,
  GS `0x2C88`, HS `0x2D08`, LS `0x2D48`, PS `0x2C08`, VS `0x2C48`. The Gnm wrapper names above are the proof that
  each setter feeds that stage; the SH values are the port's table, not read from the eboot. **proven (names),
  inferred (SH values as used by the Gnm wrapper)**
- The emit also writes the code address and size from the blob header: address `H[2] << 8` (header dword 2, i.e. R1's
  "code = dword[obj+8] << 8"), size `H[0] & 0x7fffff`, through `0x86a7f0`, when `O+0x88` is set. **proven**
- `0x937c0` and `0x94ab0` store nothing for GS and HS. GS and HS setters have no decomp caller (vtable or indirect
  only). **proven (no direct caller)**

## 5. The meaning of the mask bits (inferred)

The five bits are stage slots. Bit 0 is always the stage of `D+0x00`. Bit 4 is always PS. The middle bits have no
reader in `0xf8990`, but the setters for GS (`O+0xc0b8`) and HS (`O+0xc0c8`) exist. Combining the bit positions with
the stage codes gives this **hypothesis** (not checked against data):

| Mask | Record 0 (bit 0) | Bit 1 | Bit 2 | Bit 3 | Bit 4 | Hypothesis |
|---|---|---|---|---|---|---|
| 17 | VS | - | - | - | PS | plain VS + PS |
| 25 | ES | - | - | GS | PS | ES + GS + PS (geometry) |
| 23 | LS | HS | VS (as DS) | - | PS | LS + HS + DS + PS (tessellation) |
| 31 | LS | HS | VS (as DS) | GS | PS | tessellation with geometry |

The hypothesis fits the usage ids (section 3.1). It is the first thing to test (section 7). The footer types of the
records (0 pixel, 1 vertex, 4 compute, plus the geometry and tessellation types that `predict_table.tsv` notes) show
which bit holds which stage.

## 6. Corrections to R8 (pass-map.md)

- `pass[4]` in R8 section 3 is `X[4]` where `X = *pass`, not `pass+0x20`. The stage descriptor D is written by
  `0xf8aa0` from the mask, so R8's "kind" wording for `B+0x38` should be "mask".
- R8 section 3.1 says the `B+0x38` value is a "kind" and that the parser fills it from the file. It is the mask (line
  233). The `0xf8aa0` consumer is the mask-to-D and mask-to-usage selector.
- R8's record-to-stage question is answered: rec0 is decided by D (2 VS, 4 ES, 6 LS), rec4 is PS.
- Pass classes and their slot ids (new): the constructor names in the eboot give the engine pass class, and
  the slot k is the constant written to `pass+0xf4`:

| Pass class (name passed to `0x16b8c0`) | Constructor | k written (`pass+0xf4`) |
|---|---|---|
| `DepthPrePass` | `0xc1860` | 1, 10, 12 (three branches) |
| `RenderGBufferBakedPerVertex` | `0xc1ce0` | 2, 3 |
| `RenderGBufferBakedPerProbe` | `0xc1fc0` | 4, 5 |
| `Decal` | `0xc28c0` | 6 |
| `Debug` | `0xc46c0` | 11 |

These names are in the eboot (`0x16b8c0`, `0x16bb70`), so the port can name the pass by the constructor string.
The `.csdr` stems of R1 (`gbufferbakedambient`, ...) are still the slot resource names. **proven (constructor and
k), inferred (k as the port's pass key)**

## 7. Sub-materials (`+0x600`) and variant words (proven unless marked)

- **The file has no sub-material records.** `0xf6c90` reads the 14 slots, lists, blobs and maps, then re-parses the
  existing nodes of `+0x600` (lines 683-685, `0xf8270(node+0x20, 1)`). It never creates nodes (no call to `0xf9890`).
  **proven**
- **Creation:** `0xf9890(material, id)` (244 bytes, decomp `000f/000f9890.c`) allocates a 0x648-byte object (vtable
  `PTR_FUN_01597c78`), sets `obj+0x638 = material` (parent) and `obj+0x640 = id`, parses it with `0xf8270(obj, 1)`,
  and inserts a node in the tree at `material+0x600` (node+0x20 = object, node+0x28 = id). **proven**
- **Id allocator:** `0x115450(out, pool, material)` takes the lowest clear bit of a 256-bit map at `pool+0x220090`
  (`+0x220098`, `+0x2200a0`, `+0x2200a8`), calls `0xf9890(material, id)`, and stores a 16-byte ref in
  `pool+0x220078[id]`. The `pool` is the caller's context. **proven**
- **Who asks:** `0x115410` (wrapper) from `0xb0f740`, `0xb0fcf0`, `0xb10130`; and directly in
  `0x176230` (line 88, `FUN_00115450(&local_48, DAT_02edf140-based pool, material)` when the predicate `0xf9b00`
  says so) and `0x238c00` (line 86). `0x176230` stores the instance ref at the item's `+0x18` (line 97). **proven**
  (call sites); the predicate `0xf9b00` and what selects `id` are **unknown**.
- **Variant words (`0xf8c90`, proven):** after `0xf8990`, `pass+0xb0 = material+0x588 | (material+0x58c & 0xfff) << 32`
  when `material+0x5b0 != 0`; `pass+0xb8 = material+0x5c8 | (material+0x5cc & 0xfff) << 32` when `material+0x5f0 != 0`.
  Who sets `+0x5b0` and `+0x5f0` is **unknown** (outside the parser). The draw-time reader of `pass+0xb0/+0xb8` is
  **unknown** (not in the `0x862c50` field list of R3).
- **Instance parse (`0xf8270(obj, 1)`):** clears the object's slots and sub-lists, then parses from its own data.
  Whether it copies the parent's slots (`obj+0x638`) is **inferred** (the body of `0xf8270` is not decoded).

## 8. Algorithm for the port (what to compute when a material is ready)

Hook point (**inferred**): after `0xf6c90` returns (label `0xf80bc`), as in pass-map.md section 7. Per material M,
per slot `k = 0..13` with `B = M + 0x60 + 0x48*k`:

1. Skip if `*(B+0x10) == 0` (`0xf8250`).
2. `mask = *(u32*)(B+0x38)`. Name of slot = stem of `*(B+0x00)` (`Shader` object).
3. Records: for `i` in 0..4 with bit i set in `mask`, `rec_i = *(B+0x10+8*i)`. The blob header is `H = *(rec_i+8)`.
   Code address = `dword[H+8] << 8` (header dword 2), size = `dword[H+0] & 0x7fffff`. The copied program bytes are at
   `rec_i + 0x10`. The `OrbShdr` footer of the program gives type and hash (pass-map.md section 6).
   (The blob header and the register block are used the same way by `0x86a7f0` and the Gnm wrappers, section 4.)
4. Stage of record 0: `D = *(X+0x20)` for the pass that draws M. Since D depends on the mask only (section 3.1),
   the stage of record 0 is known from the mask alone: 17 -> VS, 25 -> ES, 23 and 31 -> LS. Record 4 (`B+0x30`) is PS.
   Records at bits 1-3 are GS/HS/VS per the hypothesis (section 5), **inferred**.
5. Output per material and slot: `(slot name, mask, { stage -> program hash })`, with stages as above, plus the
   footer type of each record so that the hypothesis in section 5 can be checked.
6. The pass class (which `k` a draw uses) is from the draw, not from the material (pass-map.md section 3). A
   material slot k is reached only through the pass with that `k` at `pass+0xf4` (section 6 table).

**Check to run in one game session (log at `0xf8990` entry, no change to draws):**

- Log `k = *(P+0xf4)`, `mask = *(B+0x38)`, `D = *(X+0x20)`, `*(D+0)`, `*(D+0x10)`, and the footer type and hash for
  each set bit, once per (material, k).
- For mask 31, expect five programs. The footer types should show LS (or type 1), HS (tessellation type), VS (type 1),
  GS (geometry type), PS (type 0). If bit 1 is not HS and bit 3 is not GS, the hypothesis in section 5 is wrong.
- For mask 25, expect ES, GS, PS (type 0 for PS).

## 9. Proven, inferred, unknown: summary

| Item | Status |
|---|---|
| Mask is the second u32 of the slot entry; stored at `B+0x38`; valid {17,23,25,31} | proven |
| Record pointer `i` = `records[popcount(mask & ((1<<i)-1))]` | proven |
| `B+0x10` and `B+0x30` are the only record pointers bound by `0xf8990` | proven |
| `D` (`X+0x20`) is chosen by the mask in `0xf8aa0` (table in 3.1) | proven |
| Record 0 stage: 2 VS, 4 ES, 6 LS (setters `0x861520`, `0x860bb0`, `0x861260`) | proven |
| Record at bit 4 is always PS (`0x861600`) | proven |
| SH register per stage | proven (Gnm names), inferred (SH value from the port's table) |
| GS and HS setters `0x860c50`, `0x860ea0`, `0x8612b0`, `0x8613e0`: stage codes and callers | proven (stage fields), unknown (callers) |
| Bits 1-3 meaning (tessellation, geometry) | inferred (section 5) |
| Sub-materials are runtime instances created by `0xf9890`, ids from `0x115450` | proven |
| Predicate `0xf9b00` and the id source at item binding | unknown |
| `pass+0xb0`, `pass+0xb8` consumer at draw | unknown |
| Who sets `material+0x5b0`, `+0x5f0` | unknown |
| Meaning of `D+0x04..+0x0f` (values 5, 2, 4, 3 for some masks) | unknown (not used by `0xf8990`) |
| Pass class name to `k` mapping (section 6) | proven (constructors), inferred (port key) |

## 10. Evidence list

| Item | Address | Decomp file |
|---|---|---|
| Material parser: slot entry, mask, pointers | 0xf6c90 (lines 126-241) | `out/re/decomp/000f/000f6c90.c` |
| Slot clear and default mask 17 | 0xf6190 | `out/re/decomp/000f/000f6190.c` |
| Presence | 0xf8250 | `out/re/decomp/000f/000f8250.c` |
| Resolver (rec0, rec4, D) | 0xf8990 | `out/re/decomp/000f/000f8990.c` |
| Mask to D, usage | 0xf8aa0 | `out/re/decomp/000f/000f8aa0.c` |
| Variant words | 0xf8c90 | `out/re/decomp/000f/000f8c90.c` |
| Record 0 setter | 0x937c0 | `out/re/decomp/0009/000937c0.c` |
| Record 4 setter | 0x94ab0 | `out/re/decomp/0009/00094ab0.c` |
| Setters VS/ES/LS/PS | 0x861520, 0x860bb0, 0x861260, 0x861600 | `out/re/decomp/0086/00861520.c` etc. |
| GS/HS/CS setters | 0x860c50, 0x860ea0, 0x8612b0, 0x8613e0, 0x861730, 0x861880 | `out/re/decomp/0086/` |
| Emit (bind B) | 0x85fc10 | `out/re/decomp/0085/0085fc10.c` |
| Gnm wrappers VS / PS / ES / LS / HS / GS | 0x86c170, 0x86c050, 0x86c2c0, 0x86c760, 0x86c330, 0x86cb00 | `out/re/decomp/0086/` |
| Code address packet | 0x86a7f0 | `out/re/decomp/0086/0086a7f0.c` |
| Draw with stage bind (k=1 then k=6) | 0x1a8c60 | `out/re/decomp/001a/001a8c60.c` |
| Engine shader-list draw (fixed entry, same bind) | 0x93690 | `out/re/decomp/0009/00093690.c` |
| Variant node parse | 0xf8270 | `out/re/decomp/000f/000f8270.c` |
| Variant instance create | 0xf9890 | `out/re/decomp/000f/000f9890.c` |
| Variant id allocator | 0x115450, wrapper 0x115410 | `out/re/decomp/0011/` |
| Instance request in item binding | 0x176230 (line 88) | `out/re/decomp/0017/00176230.c` |
| Pass constructors (names and k) | 0xc1860, 0xc1ce0, 0xc1fc0, 0xc28c0, 0xc46c0 | `out/re/decomp/000c/` |
| Descriptor table reads | `0x1604408..0x1604428`, D at `0x1604370`, `0x16043b0`, `0x1604390`, `0x16043d0`, `0x16043f0` | `out/eboot.elf` (read by a scratch ELF reader) |
| Register names | `mmSPI_SHADER_PGM_LO_*` | `gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h` |
