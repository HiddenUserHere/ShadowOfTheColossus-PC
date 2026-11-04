# Pass map: how a material resolves, per render pass, to its VS / PS programs (RE, topic R8)

Read-only static analysis of the EU 1.01 eboot (Ghidra export in `out/re/decomp/<hi16>/<addr>.c`, first line of each
file lists callers). Addresses are eboot VAs. No game dump is in this checkout, so no `.cmat` or `.csdr` byte was read.
Labels: **proven** (decomp read, address given), **inferred** (reasoning, not checked against data), **unknown**.
Builds on `materials-meshes.md` (R3), `shader-collections.md` (R1) and `render-states.md` (R2); their findings are
reused, not repeated.

## 1. Answer in short

- **The pass key is the material slot index k (0..13), and it is set on the pass object, not looked up.** The pass
  object carries k as a dword at `pass+0xf4`. The pass constructors store a constant there (for example 1, 6, 4, 11).
  The binder reads `material + 0x60 + 0x48*k` directly. **proven** (`FUN_000f8990`, section 3)
- **The function that resolves (material, pass) to programs is `FUN_000f8990` (0xf8990)**, wrapped by
  `FUN_000f8c90` (0xf8c90). It binds slot k's records and its render-state words. **proven**
- **The two u32->u32 maps at `+0x4f0` and `+0x528` are not read by any draw path found.** They are filled by the
  parser (`0xf6c90`) and copied with the variant nodes (`0xf8270`). They are not the pass-to-program lookup. **proven
  (absence of readers in the decomp folders searched), inferred (they are not needed for draws)**
- **The list at `+0x490` / `+0x4d0` of 16-byte refs is not on the draw path either.** The records used at draw are
  the per-slot pointers at `B+0x10..+0x30` (section 3.1). **proven (field use in 0xf8990)**
- **Pass names** are the `.csdr` file stems (for example `gbufferbakedambient`, `depth`, `shadowfadeout`), as R1 and
  `tools/csdr/predict_table.py` say. The eboot contains none of these names and none of their FNV-1a 32 hashes as
  bytes, so the port must take the name from the loaded resource, not from the eboot. **proven (no hit), inferred
  (name source)**
- **Program identity** for a slot record is the 64-bit `OrbShdr` hash in the program footer, the same key as
  `user/shader_predict/predict_table.tsv`. The VS/PS role of a record is best taken from the footer type (0 pixel,
  1 vertex, 4 compute), not from the record's index, because the engine's own record-to-stage mapping is not decoded.
  **inferred**
- **Engine-owned passes do not come from a material.** Several draw helpers take their program from fixed indices of
  the engine shader list `*(DAT_02ee3ce0+0x8a8)` (section 5). **proven (code), inferred (that list is engine-shaders.cslt)**

So the port can compute, per material and per pass slot k, the triple (VS hash, PS hash, render-state source), and it
can do so at material load, without a draw. Section 7 gives the algorithm and the check that confirms it.

## 2. Pass key and the pass object

Pass object fields used by the binders (**proven** from `0xf8990`, `0x94ab0`, `0x93960`, `0x1a8c60`):

| Offset | Use | Evidence |
|---|---|---|
| `+0x00` (`*param`) | render context pointer `ctx` | `0x94ec0` and others: `*param_1` passed to `0x86a900`, `0x862510` |
| `+0x08` (`param[1]`) | dirty flags, bit 0 forces a state write | `0x94ec0`: `(*(uint*)(param_1+1) & 1)` |
| `+0x18` (`param[3]`) | byte, bits 1..2 select the mode path | `0x94ec0`: `(*(byte*)(param_1+3) & 6)` |
| `+0x20` (`param[4]`) | pointer to a stage descriptor; dword `+0x00` and dword `+0x10` of it are stage indices | `0x937c0` uses `+0x00`, `0x94ab0` uses `+0x10` |
| `+0x1cc` | cached primitive/mode value | `0x94ec0` |
| `+0xf4` (dword) | **pass slot index k** (0..13) | `0xf8990`: `*(int*)(param_2+0xf4)` indexes the material |
| `+0xb0`, `+0xb8` (qwords) | variant words copied from material `+0x588`/`+0x5c8` | `0xf8c90` |
| `+0xa8` | pointer to the draw-record copy (`0x862c50`) | `0xdb390` (`param_3+0xa8`) |

The submitters `0x94ec0`, `0x94dd0`, `0x94d00` take a second argument (0, 1 or 2 in the callers seen) that indexes
`DAT_00f9f2e0` and is written to the mode with `0x86a900`. **This is not the slot index k.** It selects the draw
mode, not the material slot (**proven**, `0x94ec0` lines 5-12). Do not key the pass map on it.

### 2.1 Values of k seen as constants (pass constructors and checks)

Constants written to `pass+0xf4` (**proven**, grep of `0xf4) = ` in `out/re/decomp`):

| k | Writers (decomp files) | Notes |
|---|---|---|
| 0 | `000c/000cd330`, `000cd7a0`, `000ce850` | |
| 1 | `000c/000c1860`, `000c6f70`, `000c3a50`, `000ca6d0`, `000caed0`, `0018/001809a0`, `001a/001a8c60` | first draw in `0x1a8c60` |
| 2 | `000c/000c1ce0`, `000c3820` (2 or 7) | `0x1a6d20`, `0x1a7290` treat 2 and 4 alike |
| 3 | `000c/000c1ce0`, `000c3ec0` | |
| 4 | `000c/000c1fc0`, `0010b0a0` and `000db390` (checks) | `0x1a6d20`, `0x1a7290` (checks) |
| 5 | `000c/000c1fc0` | |
| 6 | `000c/000c28c0`, `0018/001809a0`, `001a/001a8c60` | second draw in `0x1a8c60` |
| 7 | `000c/000c2710`, `000c2fa0`, `000c3600`, `0011/0011d040`, `0011d140`, `0011/00118470`, `0018/00183d00` | |
| 8 | `000c/000c3320`, `000c3c70`, `000c4de0` | `0xf8990` uses `DAT+0x30` for k=8 |
| 9 | `000c/000c4f80` | |
| 10 | `000c/000c1860`, `000c4a50` | |
| 11 (0xb) | `000c/000c46c0`, `000c4a50`, `0014/0014c8a0`, `0014c9a0`, `00146ea0` | `0xf8990` uses `DAT+0x48` for k=11 |
| 12 (0xc) | `000c/000c1860` | |
| 13 (0xd) | `000c/000cd330`, `000ce850` | |

`0x1a8c60` (proven, lines 14-38) draws one material item twice: first with k=1 (the mode path with `DAT+0x28c`,
mode `0x80`), then with k=6 (`DAT+0xe0`, `param_4 | 0x40`). So one material item can cover two slots in one call.

## 3. The resolver: `FUN_000f8990(material, pass)` (proven)

Decomp `000f/000f8990.c` (44 lines), callers `0x1a6d20`, `0xf8c90`, `0x1a7290`. Material = `param_1`, pass = `param_2`.
Let `k = *(int*)(pass+0xf4)` and `B = material + 0x60 + 0x48*k` (**proven**: the code reads `param_1+0x70 + k*0x48`,
`param_1+0x90 + k*0x48`, `param_1+0x98 + k*0x48`, which are `B+0x10`, `B+0x30`, `B+0x38`).

```c
// 0xf8990, reconstructed from the decomp (quotes trimmed)
ctx = *pass;                                     // pass+0x00
FUN_000f8aa0(ctx, *(u32*)(B + 0x38));            // kind word (not decoded, see 6)
FUN_000937c0(ctx, *(u64*)(B + 0x10));            // record 0 -> stage bind A (0x937c0)
FUN_00094ab0(ctx, *(u64*)(B + 0x30));            // record 4 -> stage bind B (0x94ab0)
if (k == 0xb)                    state = DAT_02ee3ce0 + 0x48;
else if (kind(material+0x460) in {7,8}) state = DAT_02ee3ce0 + (k==8 ? 0x30 : 0x28);
else if (material+0x458 == 7)    { FUN_00091b50(ctx, DAT_02ee3ce0 + 0xb0); goto skip; }
else                             state = material + 0x560;
FUN_000919e0(ctx, state);                        // blend/depth state object (inferred)
skip:
depth_block = (material+0x45c == 1) ? ctx+0x21c : (material+0x45c == 2) ? ctx+0x228 : ctx+0x210;
FUN_00091cd0(ctx, depth_block);                  // (inferred: second state block)
```

Slot presence (**proven**): `FUN_000f8250(material, k)` returns `*(material+0x70 + k*0x48) != 0`, that is,
`B+0x10` (record 0) is non-null. Its only callers are `0x10aef0`, `0x5b7630`, `0xdb390`, `0x1ae340`.

### 3.1 Slot record pointers

Slot k (**proven**, field use above; **layout from R3 and R1**):

```c
struct MatSlot {                 // B = material + 0x60 + 0x48*k
  u64  res;          // +0x00  ref to the .csdr (Shader) resource; name = its path (section 4)
  u8   owned;        // +0x08
  u64  rec[5];       // +0x10 .. +0x30  Shader.records + 32*n, one per set bit of the 5-bit mask
  u32  kind;         // +0x38  (file value 17, 23, 25, 31; other values become 17)
  u64  interned;     // +0x40  FUN_00099720(DAT_02ed37b0, rec0, rec1)
};
```

The 5-bit mask bit i selects `rec[i]`. `rec[i]` holds the 32-byte record of the `.csdr` (R1 section 2b): `+0x00` file
record pointer, `+0x08` blob header (the code address is in dwords 2-3), `+0x10` copied program bytes.

**Which record is VS and which is PS:** `0xf8990` passes `rec[0]` to `0x937c0` and `rec[4]` to `0x94ab0`, and the
stage an individual bind goes to is taken from the pass object, not from the record index (**proven** for the
calls). `0x937c0` reads the stage as dword `pass[4]+0x00` (values 2, 6, 4 pick the setters `0x861520`, `0x861260`,
`0x860bb0`; other values only store the record). `0x94ab0` reads dword `pass[4]+0x10` and always binds through
`0x861600`. So the record index does not name a stage. The port should
use the footer type of each program instead (section 6).

### 3.2 The per-slot draw with a variant (`0xf8c90`, proven)

`FUN_000f8c90(material, pass)` (17 lines) calls `0xf8990(material, pass)`, then, when the material's variant flags
are set, copies `material+0x588/0x58c` into `pass+0xb0` and `material+0x5c8/0x5cc` into `pass+0xb8`. The variant
nodes (`material+0x600`) are parsed by `0xf8270` with the same 14-slot layout (R3 section 3).

## 4. Where pass names come from (proven for the chain, inferred for the port use)

- The `.csdr` for slot k is referenced from `B+0x00` (resource ref, R1 section 3a). Its name is the file path
  (`<pass>[_<hash>]_shipping.csdr`, R1 section 2b and `tools/csdr/predict_table.py`).
- The eboot has none of the pass names as bytes (`grep -a` on `out/re/ghidra/eboot_ghidra.elf` finds
  `gbufferbakedambient`, `gbufferdynamicambient`, `depthfadeout`, `depthfadeoutinvlodfade`, `shadowfadeout`,
  `shadow`, `decal`, `ps_spotlight`, zero times). **proven**
- Their FNV-1a 32 hashes (computed for this report, offset `0x811c9dc5`, prime `0x1000193`, the engine's resource hash
  of R3 section 2) are also absent as 4-byte constants in the eboot (`re.finditer` on the bytes): `gbufferbakedambient`
  `58484bbd`, `depth` `fe759eea`, `depthfadeout` `cf246050`, `shadowfadeout` `099dfa39`, `shadow` `3a43c561`, `decal`
  `de15f6ae`, `standard` `03c95748`, `alpha` `5d8b6dab`. **proven (no hit)**. So if the map keys are pass hashes, they
  are data, and they are not checked here.
- Conclusion: the port reads the name from the loaded `.csdr` resource (the path string built by `0x14c2f0` or the
  resource object's name), not from a table in the eboot. **inferred**

## 5. Engine-owned passes: fixed indices into the engine shader list (proven code, inferred identity)

Several draw helpers take their programs from `*(DAT_02ee3ce0 + 0x8a8)`, a shader-list object. Its `+0x68` is the
entry array (16-byte refs, R1 section 2a). The helper does `FUN_00095580(entry)` (ensure loaded) and `FUN_0014d7b0(ref)`
(loaded object), then `object+0x60` is the record base, and it passes `rec0` and `rec[1]` to a bind. Fixed byte
offsets (entry index = offset / 16; entry 0 is the u64 count, **inferred**):

| Offset in the list (bytes) | Entry index | Caller (decomp) | Use |
|---|---|---|---|
| `0x00f0` | 15 | `0xb4040` | pass helper, `FUN_000937c0` then `0x94ab0` |
| `0x0c00` (+0x10 when `arg != 0`) | 192 / 193 | `0xa69d0` | pass helper |
| `0x1600`, `0x1630` | 352, 355 | `0xb6e20` | pass helpers |
| `0x1650` | 357 | `0xb0570` | pass helper |
| `0x16c0` | 364 | `0xb9710` | fixed pass draw |
| indexed `DAT_00fa06f8[i] * 0x10` | table | `0xb6e20` | per-kind lookup |

These are the only fixed-index shader references found in this pass. They are not materials and do not carry slots.
Their names are in the `.cslt` file (not decoded). **proven (indices), inferred (list = engine-shaders.cslt)**

The pass draws `0x93690` and `0xb9710` also read `DAT_02ee3ce0 + 0x28`, `+0xc8`, `+0x20c`, `+0x28c`, `+0xe0`,
`+0x29c`, `+0x2ac` as state objects (R2 section 2). These are fixed states, not material data.

## 6. Program identity: footer type and hash (proven format, inferred use)

- The program footer `OrbShdr` (32 bytes, the `regs_shader.h` BinaryInfo): `+8` u32 bitfield, type in bits 2-5
  (0 pixel, 1 vertex, 4 compute), code length in bits 8-31; `+16` u64 hash. **proven** (`tools/csdr/predict_table.py`
  docstring, verified there against the machine code)
- The same hash is the first column of `predict_table.tsv`. So for a slot record, the port can get (type, hash) from the
  program bytes at `rec+0x10` (footer at the end of the code) and map it to a pass name with the table.
- **Open:** whether the engine's stage choice (`0x937c0` for `rec0`, `0x94ab0` for `rec4`) always matches the footer
  type. The port should record both and compare on a few materials (section 7 step 4).

## 7. The algorithm for the port (what to compute when a material is ready)

Hook point: after `0xf6c90` returns (`0xf80bc`, **inferred**), or in the factory build `0xf9390` with flag 1. At that
point the slots, the lists and the state words are set (R3 section 7).

For each material M in the area's list, for each k in 0..13:

1. Skip the slot if `M+0x70 + 0x48*k == 0` (presence, section 3, `0xf8250`).
2. Name: read the `.csdr` name of `B+0x00`. Pass = its stem without `_<hash>_shipping`. (**inferred** name source)
3. Records: take every non-null pointer `rec = *(B+0x10 + 8*i)`, i in 0..4. (The 5-bit mask that the file stores
   for these records is not located in this pass; use the non-null pointers. **unknown**.) Read the program at `rec+0x10`, find its
   `OrbShdr` footer (the end of the code given by the bitfield, `+8`), read type and `+16` hash.
4. Triple per slot: VS hash = the one with type 1; PS hash = type 0; CS = type 4. Keep the record index too, and log
   whether the stage order (`rec0` to `0x937c0`, `rec4` to `0x94ab0`) agrees with the type. (**inferred**)
5. Render state: from section 3 (`+0x560` or `DAT_02ee3ce0 + 0x28/0x30/0x48`, depending on kind and k). The state
   object contents are R2's open item. Render-target formats and sample count are not in the material (R3 section 6).
6. Result per material: a list of `(pass name, VS hash, PS hash, state source)`. The port then has the program keys of
   the pass. The render-target part still comes from the draw (`ctx+0xc5d8`, `ctx+0xc5e0`).

**What one game run must confirm (log line at the hook):** at `0xf8990` entry (or at the material build) log
`material ptr`, `k = *(pass+0xf4)`, `B+0x00` name, the 5-bit mask, `rec[i]` footer type and hash, `kind` (`B+0x38`)
and `material+0x460`, `+0x458`, `+0x45c`. A run through one area shows, for example, `k=1` with a `gbuffer*` name and a
PS of type 0. A second check: the names for k=1 and k=6 on the same material (both used by `0x1a8c60`).

## 8. Proven, inferred, unknown: summary

| Item | Status |
|---|---|
| Pass key is slot k = `pass+0xf4` (0..13) | proven |
| `0xf8990` binds slot k: rec0 via `0x937c0`, rec4 via `0x94ab0`, state from kind/k | proven |
| Presence = `B+0x10 != 0` (`0xf8250`) | proven |
| `+0x4f0` / `+0x528` maps are not read on the draw path | proven (no reader found) |
| `+0x490` / `+0x4d0` 16-byte refs are not read on the draw path | proven (field use) |
| Pass name = `.csdr` stem, from the resource | inferred (R1 and predict_table) |
| Map keys are FNV-1a of the pass names | unknown (hashes absent as constants) |
| Record index names the stage (VS vs PS) | unknown; use footer type |
| Engine passes via fixed indices of `*(DAT_02ee3ce0+0x8a8)` | proven code, inferred identity |
| Meaning of the kind word `B+0x38` (17/23/25/31) and `0xf8aa0` | unknown |
| Mask bits and the mask field's exact offset | unknown (R1 lists them as open) |
| Who calls `0x19af20` (item dispatch with stride 0xa8 and selector `+0x20`) | proven call chain to `0x1a6d20`/`0x1a7290`/`0x1a8c60`; the item type is unknown |

## 9. Open items (for the next step)

1. Decode the mask: read the 5-bit mask field in `0xf6c90` (around the slot loop, lines ~190-225) and check whether the
   slot record count equals the popcount.
2. Name the item type of `0x19af20` (stride 0xa8, selector at `param_3+0x20`, fields at `+0, +8, +0x10, +0x18`): is it a
   per-mesh draw item that holds materials for four pass families? That decides which k a draw uses per item.
3. Read `0x937c0` and `0x94ab0` fully to map the stage index (`pass[4]+0x10`) to a stage name, and then compare with
   the footer types (section 6).
4. Read `0xf8aa0` (the kind word) and `0x919e0` / `0x91cd0` (the state objects).
5. Run the log at the resolver (section 7) on one area.

## 10. Evidence list

| Item | Address | Decomp file |
|---|---|---|
| Pass resolver | 0xf8990 | `out/re/decomp/000f/000f8990.c` |
| Variant wrapper | 0xf8c90 | `out/re/decomp/000f/000f8c90.c` |
| Slot presence | 0xf8250 | `out/re/decomp/000f/000f8250.c` |
| Variant node parse | 0xf8270 | `out/re/decomp/000f/000f8270.c` |
| Material parser (maps and lists) | 0xf6c90 | `out/re/decomp/000f/000f6c90.c` (lines 70-130, 495-565) |
| Stage bind A / B | 0x937c0 / 0x94ab0 | `out/re/decomp/0009/000937c0.c`, `0009/00094ab0.c` |
| Pass submitters (mode from `DAT_00f9f2e0[kind]`) | 0x94ec0, 0x94dd0, 0x94d00 | `out/re/decomp/0009/00094ec0.c` etc. |
| Draw with stage bind (k=1 then k=6) | 0x1a8c60 | `out/re/decomp/001a/001a8c60.c` |
| Draw item dispatch | 0x19af20 | `out/re/decomp/0019/0019af20.c` |
| Callers of the item dispatch | 0x1a8a40, 0x1a8dd0, 0x1a6d20, 0x1a7290 | `out/re/decomp/001a/` |
| Fixed-index engine passes | 0xa69d0, 0xb0570, 0xb4040, 0xb6e20, 0xb9710 | `out/re/decomp/000a/`, `000b/` |
| Engine shader-list object | `*(DAT_02ee3ce0+0x8a8)` | used in the files above |
| Program footer format | - | `tools/csdr/predict_table.py` (docstring) |
| Pass names absent from eboot | - | `grep -a` on `out/re/ghidra/eboot_ghidra.elf` |

## Verified in a run (4 Nov 2026, SOTC_PASS_PROBE=1, log user/pass1/pass_probe.log)

The PASS_RESOLVE probe (entry of 0xf8990) fired ~200,000 times per 5 s and logged 2,000 distinct (material, k)
pairs. Every slot name resolved (0 "name=?"). The slot index k selects the pass. The .csdr stem of the slot was the
same for every material seen:

| k | pass (.csdr stem) | (material, k) pairs seen |
|---|---|---|
| 0 | shadow | 227 |
| 1 | depth | 638 |
| 2 | gbufferbakedambient | 459 |
| 3 | gbufferbakedambient_motion | 48 |
| 4 | gbufferdynamicambient | 104 |
| 5 | gbufferdynamicambient_motion | 26 |
| 6 | decal | 121 |
| 7 | standard | 15 |
| 8 | alpha | 10 |
| 10 | depthfadeout | 214 |
| 12 | depthfadeoutinvlodfade | 1 |
| 13 | shadowfadeout | 136 |

- rec[0] (B+0x10) was always a vertex program (footer type 1) and rec[4] (B+0x30) always a pixel program (type 0),
  in all 1,999 logged slots. Records 1-3 were always null.
- The slot kind word B+0x38 was 0x11 (17) in every slot seen: VS + PS only (bits 0 and 4, matching rec[0] and
  rec[4]).
- MATERIAL_LOADED (wrap of 0xf6c90) could not be installed: "function has an unresolved indirect branch". A
  load-time hook needs another site (the factory build 0xf9390, or the outer loader 0xf9530).
