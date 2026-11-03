# 05 - Shader objects, materials and per-draw binding

Read-only static analysis of the SotC eboot. Primary source: Ghidra pseudocode in
`out/re/decomp/<hi16>/<addr>.c` (note: the directory is the first 4 hex digits, e.g. `00b0/00b0ced0.c`).
Addresses are eboot VAs. Labels: **observed** (decomp line or call, with address), **inferred**
(from offsets and call patterns), **unknown**.

Context: follows `02-resource-objects.md` (section 10, open items 2, 4) and `01-backend-interface.md`.

## 1. Answers

1. **Shader objects hold the GCN binary location.** A shader object (0x88 bytes, `0xb0d4a0`) owns a
   vector of 32-byte entries (+0x60). Each entry points into a raw file buffer (owned at +0x80) that
   holds the 0xe4-byte stage records. The GCN code is copied once at load into a 256-byte-aligned
   GPU heap block (`0x12820`, family `DAT_02dd9f84`). The register block that is passed to
   `sceGnmSetVsShader`/`sceGnmSetPsShader350` is the file header at `H+8`; its first two dwords are
   overwritten with the GPU code address shifted right by 8. **observed** (writes), **inferred** (names).
2. **Material (0x638 bytes).** It holds 14 "pass slots" (stride 0x48 at +0x60). Each slot references a
   Shader resource, a 5-bit stage mask, and up to 5 pointers to that Shader's entries. The material also
   holds two 16-byte resource lists (+0x490, +0x4d0), two raw constant blobs, two maps, a render-state
   pointer (+0x560) and a parent pointer read at +0x638. Texture T# words are not in the material.
   **observed** (parse), **inferred** (meaning of the lists).
3. **Per-draw binding.** Setters (`0x861600` PS, `0x861730` VS, `0x8612b0` HS, `0x861880` CS) write the
   shader header pointer and the descriptor-map pointer into engine state `S = CRenderContext+0x2e8`.
   Descriptor setters (`0x8619a0`, `0x861ce0`, `0x861e60`, ...) write 16-byte items into per-stage
   user-data arrays in `S`. The bind functions (`0x85fc10`, `0x85f410`) then `memcpy` each dirty
   array into a bump block, put its GPU address into SGPRs with `0x86d4c0`, and call the SetVs/SetPs
   wrappers. The arrays are copied only when their dirty flag is set. The bind code does **not** read
   `CTexture+0xc4`. Where the T# is copied from is **unknown** (section 6).
4. **Variants.** No permutation hash or variant table was found. The keys seen are: the 5-bit stage mask
   per material slot (legal values 0x11, 0x17, 0x19, 0x1f), the shader identity (header bytes
   H+0x18..H+0x33) used to choose Set vs Update, and the setter modifier words (`S+0xc168` etc.).
   **observed** (checks), **inferred** (use as keys).

## 2. Shader object (0x88 bytes)

Factory `0xb0d4a0` (54 bytes, **observed**): `FUN_00017070(0x88)`, then ctor `0xb0ced0`, stores
the object in `*param_2`. Registration `0xb0d3e0` (**observed**): `FUN_00144640(entry,"Shader",0xb0d4a0,0)`
then `FUN_001444e0(factories, 0x63736472, entry)`. ShaderList: `0xb0e3a0` (name "ShaderList",
tag `0x63736c74`), factory `0xb0e440` (0x78 bytes, ctor `0xb0e4d0`, not read).

Ctor `0xb0ced0` (878 bytes) steps (**observed**):

| Step | Address | Effect |
|---|---|---|
| Base ctor | `0xdb8d0` | writes base vtable `0x1597aa8`, empty list at +0x08..+0x18, byte at +0x28 |
| Vtable | `0xb0ced0` | `*obj = 0x15d5850` (Shader vtable) |
| Member copy | `0x14c3a0` | 0x20-byte object at +0x30 (copy of global `DAT_02ed3640`; **unknown** type) |
| Read whole file | `0x8df50(&tbl, stream = build+8, size = build+0x10, align 8)` | heap copy of the resource into `tbl` (+0x80). `tbl[0]` is the record count |
| Entry vector | `0x8df50` per record set, then loop below | vector of 32-byte entries, begin +0x60, end +0x68, cap +0x70 |

Note: doc 02 says the ctor reads "a table of 32-byte entries". Corrected here: `0x8df50` reads the raw
file. The 32-byte entries are built by the loop from 0xe4-byte records (section 3).

### 2.1 Shader object fields

| Offset | Size | Content | Label |
|---|---|---|---|
| +0x00 | 8 | vtable `0x15d5850` | observed |
| +0x08..+0x27 | 0x20 | base class state (count, list head, flags) | observed (writes in `0xdb8d0`) |
| +0x30..+0x4f | 0x20 | copy of global object (`0x14c3a0`) | observed, type unknown |
| +0x58 | 8 | zero | observed |
| +0x60 | 8 | entries begin (32-byte entries) | observed |
| +0x68 | 8 | entries end | observed |
| +0x70 | 8 | entries capacity | observed |
| +0x80 | 8 | owned raw file buffer (`tbl`), 8-aligned | observed |

### 2.2 Entry (32 bytes, vector at +0x60)

Entry `i` is at `obj+0x60 + i*0x20`. Filled by the loop in `0xb0ced0`: `(record i)` for each record.

| Offset | Content | Label |
|---|---|---|
| +0x00 | record pointer, `tbl+4+i*0xe4` | observed (write `0xb0ced0`) |
| +0x08 | header pointer `H = blob+0x34` (see section 3) | observed |
| +0x10 | GPU code copy, for record types 0, 1, 2, 4, 5, 6 (`0x12820(size, 1, 0x100, 0)` family `DAT_02dd9f84`) | observed |
| +0x10 | type 3: one GPU block of `align256(code) + fetch_size`; code at offset 0, fetch copied at `align256(code)`. The fetch address is written into header dwords `16+b3` and `17+b3` (b3 = byte at H+3) | observed (writes), inferred (meaning) |

### 2.3 Load sequence per record (observed)

For record `i` (`0xb0ced0`, switch on `*(uint*)(record+0)`):

1. `FUN_00b2ecb0(&hdr, blob)` (43 bytes, **observed**): `hdr[0] = blob+0x34` (= H), `hdr[1] = H + (dword[H+8] & ~3)`
   (code source), `hdr[2] (dword) = ushort[H+4]*0x10 + (dword[H] & 0x7fffff)` (code size).
   The naming of hdr[1] as "end" in Ghidra is misleading. Its use as the copy source is what matters.
2. `0x12820(size_aligned_0x100, DAT_02dd9f84, 0x100, 0)`: GPU block. `memcpy(block, code_src, size)`.
3. `H[2] = (uint)(block >> 8)`, `H[3] = (uint)(block >> 40)`. **observed**. The register block (sceGnm
   argument) is `H+8`, so `reg_lo = block>>8` lands at `H+8` (= `blob+0x3c`, the same dword that held the
   load-time code offset). **observed** (writes), **inferred** (register block).
4. `blob = record + 0xe0 + *(int*)(record+0xe0)` (self-relative, 0 means none). **observed**.

Type 3 additionally calls `0xb2ece0` (92 bytes, **observed**) which uses the byte `H+3` as index `b3` into
an offset table at `blob+0x6c + b3*4` to find a second code block (fetch shader, **inferred**).

## 3. Stage record (0xe4 bytes) and the header H

Records start at `tbl+4 + i*0xe4` (`tbl[0]` = count). Record fields (**observed** from the loop and
the bind code; names **inferred**):

| Offset in record | Use | Where read |
|---|---|---|
| +0x00 | type dword: 0, 1, 2, 3, 4, 5, 6 seen in the switch (3 has fetch) | `0xb0ced0` |
| +0x08 .. +0xdf | **descriptor map** `M = record+8` (see section 3.1) | `0x85f410`, `0x85fc10`, `0x8619a0` |
| +0xe0 | self-relative offset to blob (see section 2.3 step 4) | `0xb0ced0` |

Header H = blob+0x34 (**observed** from `0xb2ecb0`, `0x861600`, `0x861730`, `0x85f410`):

| Offset from H | Use |
|---|---|
| +0x00 dword | low 23 bits = code size (bytes); used as the size argument of `0x86a7f0` |
| +0x03 byte | fetch index b3 (type 3) |
| +0x04 ushort | count, added as `*0x10` to the size (**inferred**) |
| +0x08 dword | code offset at load; then `reg_lo = GPU>>8` (SGPR-block dword 0 of SetVs/SetPs) |
| +0x0c dword | `reg_hi` (GPU>>40) |
| +0x08 .. +0x2c | sceGnm register block (VS = 0x1d dwords, PS = 0x28 dwords, from the wrapper arguments) |
| +0x18 .. +0x33 | shader identity (32 bytes), compared in `0x861600` and `0x861730` for Set vs Update |
| +0x24 byte | `&3` -> `S+0xc5d8`; `+0x25` byte `&0x1f` -> `S+0xc5e0` (set in `0x861730`, `0x861880`); meaning **unknown** |
| +0x38 byte | count of words consumed by `0x94ab0` (`FUN_0087f790`, `FUN_0086bc00`); meaning **unknown** |

Note on the VS block: `0x86c170` (sceGnmSetVsShader) is called with `(cursor, 0x1d, H+8, modifier)`.
`0x86c050` (sceGnmSetPsShader350) with `(cursor, 0x28, H+8)`. **observed**. Dword counts 0x1d and 0x28 are
the register-block sizes.

### 3.1 Descriptor map M = record+8

M is what `S+0xc180 + stage*8` points to. Stage numbering **observed** from the bind code:
0 = CS (`S+0xc180`), 1 = PS (`S+0xc188`), 2 = VS (`S+0xc190`), 3 = GS (`S+0xc198`), 4 = ES (`S+0xc1a0`),
5 = HS (`S+0xc1a8`), 6 = LS (`S+0xc1b0`). Offsets below are from M (record+0x08 + n). **observed** unless
marked.

| Offset from M | Use (in `0x85f410`, `0x85fc10`) |
|---|---|
| +0x04 ushort | low byte = SGPR slot written with `S+0xc130` (VS) or `S+0xc140`/`S+0xc150` (other stages); if -1, the high byte is used as the next field |
| +0x05 byte | SGPR slot <- block + dword[+0x18]*4 |
| +0x06 byte | SGPR slot <- block + dword[+0x20]*4 |
| +0x07 byte | SGPR slot <- block base address (`0x86d4c0`, arg = bump block) |
| +0x08 ushort | SGPR slot for block + dword offset `[+0x16]*4`; high byte at +0x09 gives the next slot |
| +0x0a ushort | SGPR slot for block + dword offset `[+0x1c]*4`; byte +0x0b gives the next |
| +0x0c ushort | SGPR slot for `S+0x78` (address); byte +0x0d -> `0x86d5e0` (16-byte dword writer) into `S+0xc0e8`-family |
| +0x0e byte | `0x86d5e0` into `S+0xc108`-family |
| +0x13 byte | output dword (stored into `*param_7`, `*param_6=1`) in VS path |
| +0x14 byte | SGPR slot for `S+0xc5d0` pointer |
| +0x15 byte | `0x86d550` (single dword) or output |
| +0x16, +0x18, +0x1a, +0x1c, +0x1e, +0x20 ushort | dword offsets inside the bump block for the slots above |
| +0x22 + 2*slot (ushort table) | slot -> user-data dword index, read by `0x861ce0` (**observed**) |
| +0x82 + 2*slot (ushort table) | slot -> user-data dword index, read by `0x8619a0` and `0x8618e0` (**observed**) |
| +0xca + 2*slot (ushort table, used with `desc+0xca`) | slot -> user-data index for 16-byte items, read by `0x8611a0` (**observed**) |

A ushort table entry is: low 14 bits = dword index into the stage's user-data array; bit 15 set = the
item goes straight to an SGPR (`0x86d5e0`) instead of the array. **observed** (`0x8619a0`, `0x8611a0`).

## 4. Engine state S = CRenderContext+0x2e8

`S` is passed as `param_1` to `0x85f410`, `0x85fc10`, `0x861600`, `0x861730`, and is built from
`CRenderContext+0x2e8` in callers (`0x94ab0`, `0x94d00`, `0xdb620`). **observed**. Cursors:
`S+0xc090` = graphics PM4 cursor; `S+0xc098` = second cursor (compute path `0x883e60`/`0x8841d0` family).

| S offset | Content | Written by |
|---|---|---|
| +0x0090 + stage*0x1800 (dword array) | per-stage user-data: stage 0 CS `+0x90`, 1 PS `+0x1890`, 2 VS `+0x3090`, 3 GS `+0x4890`, 4 ES `+0x6090`, 5 HS `+0x7890`, 6 LS `+0x9090` | `0x8619a0`, `0x861ce0`, `0x861e60`, `0x8611a0`, `0x8618e0` (flags) |
| +0xc0a8 | PS shader header H (or NULL) | `0x861600` |
| +0xc0b0 | VS shader header H | `0x861730` |
| +0xc0b8 | GS header | `0x860c50`, `0x860ea0` |
| +0xc0c0 | ES header | `0x860bb0`, `0x860c00` |
| +0xc0c8 | HS header | `0x8612b0` |
| +0xc0d0 | LS header | `0x861260` |
| +0xc0d8 | CS header (and written by `0x861730` as `H + ((H+0x24)+(H+3))*4 + 0x2b...`, see note) | `0x861880`, `0x861730` |
| +0xc0e0 + stage*4 | per-stage (packed) user-data count word | `0x8618e0` |
| +0xc0e8, +0xc0f0, +0xc0f8, +0xc100, +0xc108 ... | VS/PS/LS/HS/GS output-dword slots (per stage: `+0xc0e4 + ...`) | `0x85f410`, `0x85fc10` |
| +0xc130 / +0xc140 / +0xc150 | per-stage aux pointers (VS `+0xc130`, ES `+0xc140`, LS `+0xc150`) (**inferred**) | `0x861730` (`+0xc130`) |
| +0xc168 | VS modifier (4th argument of sceGnmSetVsShader) | `0x861730` param_3 |
| +0xc170 | ES modifier (4th arg of SetEsShader) | `0x85fc10` reads |
| +0xc178 | LS modifier | `0x85fc10` reads |
| +0xc180 + stage*8 | descriptor map M pointer per stage (stage 0..6) | `0x861600`, `0x861730`, `0x8612b0`, `0x861880` |
| +0xc1b8 | secondary map (written with `+0xc0d8` by `0x861730`, `0x861880`) | `0x861730`, `0x861880` |
| +0xc1c0 | zero | `0x861600`, `0x861730` |
| +0xc260 (uint) | stage bind bits (0x1 = HS/LS group, 0x20 = ES/GS group) | `0x85fc10` reads |
| +0xc264 (ushort) | "emitted this flush" bits (0x2 PS, 0x4 VS, 0x8 GS, 0x20 HS) | `0x85f410`, `0x85fc10` |
| +0xc249 .. +0xc24f | dirty: PS(c249), VS(c24a), GS(c24b), ES(c24c), HS(c24d), LS(c24e), CS(c24f) | setters |
| +0xc250 + stage | per-stage user-data dirty (`0x8618e0`, `0x8619a0`) | setters |
| +0xc251 .. +0xc256 | user-data dirty: PS(c251), VS(c252), GS(c253), ES(c254), HS(c255), LS(c256) | setters, `0x8619a0` |
| +0xc257 | CS / secondary dirty (`0x861880`, `0x8619a0` stage 2 with c613) | setters |
| +0xc259 / +0xc25a / +0xc25b / +0xc25d | "same as last" (Update instead of Set): PS / VS / GS / HS | `0x861600`, `0x861730`, `0x8612b0` |
| +0xc5e0, +0xc5d8 | from header H+0x25, H+0x24 (see section 3) | `0x861730`, `0x861880` |
| +0xc5f0, +0xc5f4, +0xc5f8, +0xc608 | HS state (`+0xc5f4` = param_4 of `0x8612b0`) | `0x8612b0` |
| +0xc612, +0xc613 | secondary-path enable flags | `0x8619a0`, `0x8618e0`, `0x861730` |
| +0xc5d0 | 64-bit pointer used by the stage-14 SGPR writes | `0x85f410` |
| +0x78 | 64-bit address value, written to an SGPR slot for `+0x0c` in M | observed reads |

Note (`0x861730`): this VS setter also writes `S+0xc0d8 = H + off`, where
`off = (((u16[H+0x24] & 0xff) + H[3]) * 4 + 0x2b + ((u16[H+0x24] >> 8) & 0xff) * 2) & 0xffc`, and
`S+0xc1b8 = param_8`. The CS header slot is therefore touched by a VS bind. The Ghidra output for this
function is labelled `sub_861730` with size 1 and no caller, so it may be an entry point reached through a
table not yet found. **unknown** whether this is intended.

## 5. Material object (0x638 bytes)

### 5.1 Factory and parse

- Factory `0xf9390` (124 bytes, **observed**): reuses `param_2[1]` if non-null, else allocates `0x638`
  and calls ctor `0xf63c0` (not read). Calls parse `0xf6c90(obj, stream = *(param_1+8), full = 1)`. On
  failure, calls the vtable dtor and frees (`0x170b0`). Result to `*param_2`.
- Registration: `Material` at `0xf9270`, build `0xf9390` (**observed**, from doc 02).
- Parse `0xf6c90` (5137 bytes, **observed**): with `full != 0` it first clears the 14 slots (`0xf6190`
  on +0x60, +0xa8, ..., +0x408), then the two resource lists. Stream reads are `0x8eae0` (u32) and
  `0x8de50` (bytes); names are read by `0x14c3f0` and resolved by `0x14d880`.

Parse order (**observed**):

| Order | Read | Stored at |
|---|---|---|
| 1 | u32 | +0x454 |
| 2 | u32 type (3 forces class 0 and u16 = 2) | +0x458 |
| 3 | u32 flags | +0x464 |
| 4 | u32 class (render-state index) | +0x460 |
| 5 | u32 (feature word) | local |
| 6 | u32 (u16 used to compute +0x45c) | +0x45c = `(u16<=2) ? 2-u16 : 2` |
| 7 | u32 count N_slots | loop |
| per slot | u32 slot index (0..13), u32 mask, resource name | slot at +0x60 + idx*0x48 |
| 8 | u32 count N_list1, then 16-byte items | list at +0x490 |
| 9 | u32 count N_list2, then 16-byte items | list at +0x4d0 |
| 10 | u32 size, raw bytes | blob at +0x470 (vector, `0x8de50`) || 11 | u32 size, raw bytes | blob at +0x4b0 |
| 12 | u32 count, 8-byte items (key, value) | map at +0x4f8 |
| 13 | u32 count, 8-byte items (key, value) | map at +0x528 |

Derived fields (**observed**): +0x468 from class and flags (values 0x1, 0x41, 0x200, 0x80, 0x10a, 0x11a,
0x20); +0x450 bitmask of populated slots (section 5.2); +0x560 render-state pointer (section 5.4).

### 5.2 Pass slots (+0x60 + i*0x48, i = 0..13)

Confirmed by `0xf8270` and `0xf8d10` (**observed** for the layout; **inferred** for meaning).

| Slot offset | Size | Content |
|---|---|---|
| +0x00 | 8 | resource handle of a **Shader** object (`0x14d880`) |
| +0x08 | 1 | "owned" flag (`0x14d2d0` release if set) |
| +0x10 | 8 | entry pointer #0 of the Shader (`Shader+0x60 + k*0x20` for the first set bit of mask) |
| +0x18 | 8 | entry pointer #1 (compact index = count of lower set bits) |
| +0x20 | 8 | entry pointer #2 |
| +0x28 | 8 | entry pointer #3 |
| +0x30 | 8 | entry pointer #4 |
| +0x38 | 4 | kind code = mask. Legal values **0x11, 0x17, 0x19, 0x1f** (`0x82820000 >> mask`); otherwise 0x11 |
| +0x40 | 8 | unused in parse |
| +0x48 | 8 | `0x99720` result when mask & 6 (**unknown**) |

Mask bit `k` (0..4) selects Shader entry `k` of the compact list. This is the "which stages does this
slot use" key. Populated flag: `+0x450` bit `i` = (slot i, +0x10 non-null). **observed**.

### 5.3 Other material fields

| Offset | Size | Content | Label |
|---|---|---|---|
| +0x000..+0x05f | 0x60 | base class (ctor `0xf63c0`, not read) | unknown |
| +0x450 | 4 | populated-slot bitmask (14 bits) | observed |
| +0x454 | 4 | parse field 1 | observed (role unknown) |
| +0x458 | 4 | type, used by switches | observed |
| +0x45c | 4 | derived (`2 - u16`, clamped) | observed |
| +0x460 | 4 | class (render-state index) | observed |
| +0x464 | 4 | flags (bit 0x40 forces extra bit 0x80) | observed |
| +0x468 | 4 | derived shader feature word | observed |
| +0x470 / +0x478 / +0x480 | 24 | vector<u8>: blob A (constants) | observed |
| +0x490 / +0x498 / +0x4a0 | 24 | vector<16-byte>: resource list 1 (`{handle, owned}`) | observed |
| +0x4b0 / +0x4b8 / +0x4c0 | 24 | vector<u8>: blob B | observed |
| +0x4d0 / +0x4d8 / +0x4e0 | 24 | vector<16-byte>: resource list 2 | observed |
| +0x4f0 .. +0x508 | | map (`0x8ac??`-style RB tree) keyed by u32, count = bytes/8 | observed (tree), key/value layout Ghidra-ambiguous |
| +0x528 .. +0x550 | | second map, same format | observed |
| +0x560 | 8 | render-state object pointer | observed |
| +0x578 | 8 | object built by `0xf5cd0` from blob A and list 1; resolved per entry by `0xf8d10` (`0xf9`, resource pointers) | observed (build), inferred (meaning) |
| +0x5b0 / +0x5b4 | 4 each | counts of list 2 and list 1 (flag `0x11a`, `0x20`) | observed |
| +0x5b8 | 8 | object built from blob B and list 2 | observed |
| +0x5f0 | 4 | flag (bit 0x20) | observed |
| +0x600 | 16 | intrusive list head of child objects (`0xf8270` per node at +0x20) | observed |
| +0x638 | 8 | parent pointer read by `0xf8270` (`lVar12 = *(param+0x638)`) | observed, **suspicious**: one past the 0x638-byte allocation. Either the allocation is larger or the field means something else. Not resolved |

### 5.4 Render-state (class) selection

`0xf6c90` sets `+0x560` from the global table at `DAT_02ee3ce0` by the class word at +0x460 (**observed**):

| Class (+0x460) | Source |
|---|---|
| 0 and default | `DAT_02ee3ce0+0x28` |
| 1 (type 6 special) | `DAT_02ee3ce0+0x58` |
| 2 | `DAT_02ee3ce0+0x48` |
| 3, 8 | `DAT_02ee3ce0+0x30` |
| 4 | `DAT_02ee3ce0+0x70` |
| 5 | `DAT_02ee3ce0+0x60` |
| 6 | `FUN_0008f1d0(DAT_02ed33f0, 0, 4, 5, flags)` |

The kRS_* names are not in `tools/re/names.txt` (no hits). The objects behind these offsets are
**unknown**. Note: `DAT_02ee3ce0+0x340` is the default-resource fallback used in the resource lists.

## 6. Per-draw path: material to PM4

### 6.1 Chain (observed unless noted)

1. Engine code picks a Shader per pass from a global state object: `DAT_02ee3ce0+0x8a8`, then `+0x68`,
   then the resource at `+0xf0` (VS, `0xa69d0` -> `0x937c0`) or `+0xc00 + (flag)*0x10` (PS, `0xa69d0` ->
   `0x94ab0`). The global meaning is **unknown**.
2. `0x94ab0(passObj, entry)` (300 bytes, **observed**) calls `0x861600(S, entry[1] = H, entry[0] + 8 = M)`:
   binds PS. It also reads `H[0x38]` and calls `0x87f790`/`0x86bc00` if nonzero.
3. VS: setter `0x861730(S, H, modifier, aux, M, ...)` (**observed** writes; caller not found).
4. Descriptor items: setters `0x8619a0`, `0x861ce0`, `0x861e60`, `0x861fe0`, `0x862160`, `0x8622e0`, `0x861b40`
   write 16-byte items into `S+0x90+stage*0x1800`. The slot is looked up in M's ushort tables (section 3.1).
   Callers pass `(S, stage, first_slot, count, items)`. **observed** (signature), **unknown** (who
   builds the items).
5. Draw entry `0x94d00` (204 bytes, **observed**): sets draw-type via `0x86a900`, calls `0x862510`
   (flush of pending register writes), `0x85fc10` (bind all dirty stages), `0x86b040` (DrawIndexOffset),
   `0x862510` again.
6. `0x85fc10` (3994 bytes, **observed**) binds the stages in order ES/GS (flags `c260 & 0x20`), HS/LS
   (`c260 & 1`), then VS (`c24a`, `0x86c170` or `0x86c250` Update) and PS (`c249`, `0x86c050` or
   `0x86c110` Update). For each stage with user-data dirty (`c25x`), it:
   - `FUN_008643f0(S, count)`: bump allocation of `count` dwords;
   - `memcpy(block, S + stage_base, count*4)` (count from `*(ushort*)(M+0)`); **observed**;
   - writes the block address into SGPRs (`0x86d4c0`, stage id 0..6: CS 0, PS 1, VS 2, GS 3, ES 4, HS 5, LS 6);
   - writes 16-byte items directly into SGPRs when the ushort table says so (`0x86d5e0`).
7. `0x85f410` (2042 bytes, **observed**) is a sibling used from `0x862c50`. It does the same for VS and
   PS only, plus the CS block at `S+0xc24f`.

### 6.2 Is the T# copied from CTexture+0xc4 every draw?

- **Observed:** The bind code (`0x85fc10`, `0x85f410`) never reads a CTexture. It copies the per-stage
  dword array `S+stage*0x1800+0x90` (count from M) to a bump block whenever that stage's user-data
  dirty flag is set.
- **Observed:** The flags are set by setters (`0x861600` -> c251, `0x861730` -> c252, `0x8618e0` /
  `0x8619a0` -> c250+stage) and cleared at the end of the bind (`0x85fc10`, `0x85f410`). So the copy is
  per draw only when a setter ran since the last flush, not every draw.
- **Observed:** The material holds resource handles (+0x490, +0x4d0) and resolves them with `0xf8d10` into
  objects at +0x578/+0x5b8. The T# words are not in the material.
- **Unknown:** the caller that turns CTexture objects into 16-byte items for `0x8619a0` and friends. The
  candidate callers (not read) are the draw-path builders `0x19b400`, `0xde9d0`, `0x19b760`, `0xde0c0`,
  `0x5e3b50`, `0xa8c50`, `0x10d690`, `0x928f0` (compute post pass, uses `0x865410` to build V# inline),
  `0x163d10`. The T# at CTexture+0xc4 (32 bytes) would need two 16-byte items. This pass did not find that
  copy. **unknown**.

## 7. Answers to the brief

| Question | Answer | Label | Evidence |
|---|---|---|---|
| GCN binary pointer | Register block at H+8; dwords 0..1 = GPU>>8 | observed | `0xb0ced0`, `0xb2ecb0`, `0x85f410` |
| How it is loaded | Raw file buffer (`0x8df50`), records of 0xe4, blob at record+0xe0 | observed | `0xb0ced0` |
| Header parse | size = dword[H]&0x7fffff; code = H + dword[H+8]&~3 | observed (name of hdr[1] is inferred) | `0xb2ecb0` |
| VS/PS pick per draw | Material slot mask and entry index; global pass state picks the Shader | inferred | `0xa69d0`, `0x94ab0`, `0x937c0` |
| Permutation key | No single key. Stage mask (legal 0x11/0x17/0x19/0x1f), identity H+0x18..0x33 for Set/Update, modifiers | observed (checks), inferred (use) | `0x861600`, `0x861730`, `0x85fc10` |
| Material fields | 14 pass slots, two resource lists, two blobs, two maps, render-state pointer, parent pointer | observed | `0xf6c90`, `0xf8270`, `0xf8d10` |
| Texture slots | Resource handles at +0x490 and +0x4d0, resolved to +0x578/+0x5b8 | observed (layout), inferred (texture meaning) | `0xf6c90`, `0xf8d10` |
| Sampler state | Not located (S# not seen in bind code) | unknown | - |
| Per-draw resource table | Dword arrays `S+stage*0x1800+0x90` memcpy'd when dirty; slot map in M | observed | `0x85fc10`, `0x8619a0`, `0x8611a0` |
| T# copied each draw from CTexture+0xc4 | Not observed. Copy happens before setters, source not located | unknown | - |

## 8. Corrections to doc 02

- Section 5 of doc 02 says `0x85f410` copies from `ctx+0x3090`. That is the **VS** array. PS is `+0x1890`,
  GS `+0x4890`, ES `+0x6090`, HS `+0x7890`, LS `+0x9090`, CS `+0x90` (stage stride 0x1800). **observed**.
- Section 4 of doc 02 says the shader ctor reads "a table of 32-byte entries". The table is 0xe4-byte records;
  the 32-byte entries are built from them (section 2). **observed**.
- The `FUN_00b2ecb0` naming in Ghidra (`param_1[1] = end`) reads as an end pointer, but the code uses it as
  the copy source. **observed**.

## 9. Open items

1. Find the caller of `0x861730` (VS setter, `sub_861730`, size 1, no direct caller) and of `0x861520`.
2. Find the source of the 16-byte items passed to `0x8619a0` / `0x861ce0` for textures (T# from CTexture+0xc4)
   in `0x19b400`, `0xde9d0`, `0xde0c0`, `0x163d10`.
3. Identify the global `DAT_02ee3ce0+0x8a8+0x68` (pass state) and its `+0xf0` / `+0xc00` resources.
4. Decode `0x99720`, `0x8f1d0`, `0x14c3a0`, `0x87f790`, `0x86bc00` (unknown helpers in the Shader and Material paths).
5. Resolve the material +0x638 field and the ctor `0xf63c0` size.
6. Name the render-state objects behind `DAT_02ee3ce0 + {0x28,0x30,0x48,0x58,0x60,0x70}`.
7. Confirm the record type meaning (0..6) against the Gnm stage enum and what type 3 (fetch) is.
8. Decode `0x861880` and the CS path with its second cursor `S+0xc098` (`0x883e60`, `0x8841d0`).
