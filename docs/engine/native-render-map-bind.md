# Structure map: SHADER / MATERIAL BIND and RENDER-TARGET BIND of the SOTC RenderJob

Phase 2.4/2.5 deliverable, branch `native/render-vk13`. Companion to `native-render-map.md`
(contract), `native-render-map-pass.md` (pass/draw/material-block detail) and
`native-render-abi.md` (SGPR ABI). All addresses are EU 1.01 eboot VAs (CUSA08809); decompiles
at `out/re/decomp/<hi16>/<addr>.c`, the first comment line carries size + callers. Tags:
**proven** = read in the named decompile line(s) or raw eboot bytes; **inferred** = reasoning
from proven facts; **unknown** = not established.

Everything here is read-only RE of the binary; no C++ was modified.

## 0. Summary of the two mechanisms

- **Shader/material bind = a dirty-flag command generator.** There is no guest-side pipeline
  object. `0x85fc10` ("Material_BindGraphicsShaders_B") scans the dirty bytes of the material
  block (`cb+0x2e8`) and emits `sceGnmSetXxxShader` + per-stage `SET_SH_REG` (user-SGPR /
  user-data) PM4 packets into the command stream. A "unique pipeline" is identified only by
  *content*: the set of shader-program records (their 32-bit GPU code addresses) plus the
  user-data register window contents the shaders dereference. The Vulkan side must cache on the
  same content key (§2.9).
- **Render-target bind = cached CB_COLORn/DB register block emission.** `0x92140` takes an
  array of 9 pass-target descriptors, resolves each to an 8-qword CB descriptor block, and
  emits `0x86a1a0` (CB_COLORn) / `0x86a290` (DB_Z…) register packets only for slots whose
  descriptor *pointer* changed. Targets are referenced by host pointers to target-descriptor
  objects owned by the target manager `M = *(F+0xd98)`; the guest PM4 carries **GPU VA**
  addresses extracted from those descriptors (`addr>>8` in CB_COLOR0_BASE / DB_Z_READ_BASE).

## 1. Material block (`cb+0x2e8`) — bind-side field map (delta over pass-map §1.3)

All proven from `0085fc10.c`, `00860bb0.c`, `00860c50.c`, `00861260.c`, `008612b0.c`,
`00861520.c`, `00861600.c`, `008616f0.c`:

| Offset | Field | Evidence |
|---|---|---|
| `+0xc260` | u32 **group-pending word**; bit0 = LS/HS (tessellation) group, bit5(0x20) = ES/GS group; the VS/PS group is checked unconditionally. The whole word is also the *draw kind* consumed by `0x862510` (kinds 0/0x45/0xad/0xb0/0x145/0x1ad — their bit patterns carry the same pending bits). Aliased with `cb+0xc548`: material = cb+0x2e8, so `material+0xc260 == cb+0xc548` — one field, two views (draw-kind selector vs group-dirty flags). | `0085fc10.c:28-29,194`; `00862510.c:12-34`; `00093960.c:81` |
| `+0xc264` | u16 accumulator, **bit s = "slot s's user data staged but not yet flushed"** (s=1 PS→2, s=2 VS→4, s=3 GS→8, s=5 HS→0x20). The flush ORs bits in (`0085fc10.c:55,237,372,385`); the setters test-then-clear them to decide Set-vs-Update (`00861600.c:17-30` `&2`, `00861520.c:17` `&4`, `00860c50.c:20` `&8`, `008612b0.c:20` `&0x20`). | `0085fc10.c:55,237,372,385` |
| `+0xc266` | sibling latch of `+0xc264` (same bits OR-ed 2/4/8/0x20 by the setters alongside the +0xc264 update); exact role not decoded | `00861520.c:24`, `00861600.c:31`, `00861260.c`, `00860c50.c:27`, `008612b0.c:37` |
| `+0xc248+s`, `+0xc250+s`, `+0xc258+s` | three per-slot byte arrays, **slot ordinal s = GNM shader stage: 0=CS, 1=PS, 2=VS, 3=GS, 4=ES, 5=HS, 6=LS, 7=spare** (same ordinal as the SET_SH_REG stage index): `+0xc248+s` = record changed (rebind required), `+0xc250+s` = user data pending, `+0xc258+s` = layout-identical (use Update). `0x85f190(material, s)` clears all three plus the record/layout pointers. | `0085f190.c:16-20`, `0085fc10.c`, `008616f0.c`, `00861600.c`, `00861520.c`, `00860c50.c`, `00860bb0.c`, `008612b0.c`, `00861260.c` |
| `+0xc249` | PS changed | `00861600.c:15-16`, tested `0085fc10.c:383` |
| `+0xc24a` | VS changed | `00861520.c:13-14`, `00860c50.c:40-41`, tested `0085fc10.c:369` |
| `+0xc24b` | GS changed | `00860c50.c:16-17`, tested `0085fc10.c:53` |
| `+0xc24c` | ES changed | `00860bb0.c:8`, tested `0085fc10.c:32` |
| `+0xc24d` | HS changed | `008612b0.c:14`, tested `0085fc10.c:226` |
| `+0xc24e` | LS changed | `00861260.c:8`, tested `0085fc10.c:221` |
| `+0xc251` | PS user-data pending (set unconditionally when a PS record is given: `= (param_2 != 0)`) | `00861600.c:12` |
| `+0xc252` | VS user-data pending | `00861520.c:11`, `00860c50.c:32`, `008619a0.c:35`, `008611a0.c:35` |
| `+0xc253` | GS user-data pending (flushed as stage 3) | `0085fc10.c:132`, `00860c50.c:14` |
| `+0xc254` | ES user-data pending (flushed as stage 4) | `0085fc10.c:66`, `00860bb0.c:7` |
| `+0xc255` | HS user-data pending (flushed as stage 5) | `0085fc10.c:314`, `008612b0.c:13` |
| `+0xc256` | LS user-data pending (flushed as stage 6) | `0085fc10.c:248`, `00861260.c:7` |
| `+0xc259` | PS layout-identical (→ `sceGnmUpdatePsShader350`) | `00861600.c:19-29`, read `0085fc10.c:384` |
| `+0xc25a` | VS layout-identical (→ `sceGnmUpdateVsShader`) | `00861520.c:17-22`, `00860c50.c:43-48`, read `0085fc10.c:371` |
| `+0xc25b` | GS layout-identical (→ update variant `0x86cb60`) | `00860c50.c:20-25`, read `0085fc10.c:54` |
| `+0xc25d` | HS layout-identical (→ update variant `0x86c460`) | `008612b0.c:23-34`, read `0085fc10.c:236` |
| `+0xc0a0` | CS record pointer (`0x8616f0` = the CS/compute setter) | `008616f0.c:8,11` |
| `+0xc0a8` | PS record pointer (the PS-header setter `0x861600` writes it) | `00861600.c:33` |
| `+0xc0b0` | VS record pointer (embedded-VS sub-record when GS active) | `00861520.c:26` |
| `+0xc0b8` | GS record pointer | `00860c50.c:29` |
| `+0xc0c0` | ES record pointer | `00860bb0.c:12` |
| `+0xc0c8` | HS record pointer | `008612b0.c:39`, consumed by the `sceGnmSetHsShader` call `0085fc10.c:195,239` |
| `+0xc0d0` | LS record pointer | `00861260.c:12`, consumed by the `sceGnmSetLsShader` call `0085fc10.c:196,222` |
| `+0xc180+8*slot` | per-slot **layout record** pointer (CS..HS = slots 0..7). Its fields drive every user-SGPR write (see §2.3) | `00862490.c:8`, `008616f0.c:10`, `00860bb0.c:11`, `00860c50.c:28`, `00861520.c:25`, `00861260.c:11`, `008612b0.c:38` |
| `+0xc130/+0xc140/+0xc150` | u64 constants staged for stages 2/4/6 (set to 0 by the GS path, `param_4` by VS/ES setters) | `00861520.c:27`, `00860bb0.c:13`, `00860c50.c:53` |
| `+0xc168/+0xc170/+0xc178` | u32 `shader_modifier` staged for VS/ES/GS | `00861520.c:28`, `00860bb0.c:14`, `00861260.c:14` |
| `+0xc1b8` | tessellation-mirror layout record (stage-2 mirror when `+0xc613` set) | `00862490.c:11` |
| `+0xc5e4` | "ES uses record-embedded user data" flag | `00860bb0.c:15`, `0085fc10.c:34` |
| `+0xc5e8` | u32 `(hi16*width) * (lo16*height)` — texture size constant written to stage-2 and stage-3 SGPRs | `0085fc10.c:182-185,465-471` |
| `+0xc5f4` | LS u32 param (LS_HSIZE) consumed by `0x8805c0` | `008612b0.c:40`, `0085fc10.c:227` |
| `+0xc5f8` | 0x24-dword scratch: the tessellation LS/HS constant block built by `0x880530` | `0085fc10.c:200-219` |
| `+0xc600` | u64 staged into the `+0x7890` (stage-5) table | `0085fc10.c:212-216` |
| `+0xc608` | s32 HS clamp parameter (sign bit → `0x880480`) | `0085fc10.c:233-235` |
| `+0xc5c8/+0xc5cc` | on-chip GS ring config cache (`0x180003` magic + size class) → `0x86c650` | `00860c50.c:70-75` |
| `+0xc612` | byte: LS/HS path present | `0085fc10.c:197` |
| `+0xc613` | byte: GS tessellation mirror present (`record->u16@0x14 != 0xffff`) | `00861520.c:29`, `00860c50.c:55` |
| `+0xc5d9` | byte = `layout->u8@0x24 & 3` (CS) | `008616f0.c:12` |
| `+0x88` | byte: SQTT (thread-trace) capture enabled → emits `0x86a7f0` markers after each shader upload | `0085fc10.c:49,62,223,244,379,396` |
| `+0x78` | qword: SQTT marker buffer address (written to SGPR slot 0xc of every stage's user data when its layout requests it) | `0085fc10.c:105,171,287,353,439` |
| `+0x90,+0x1890,+0x3090,+0x4890,+0x6090,+0x7890,+0x9090` | per-slot staging arrays (0x600 dwords = 0x1800 bytes apart): stage 0 CS @+0x90, 1 PS @+0x1890, 2 VS @+0x3090, 3 GS @+0x4890, 4 ES @+0x6090, 5 HS @+0x7890, 6 LS @+0x9090 | `0085fc10.c` memcpy sources; `008619a0.c:16` (`+0x90 + stage*0x1800`) |
| `+0xc090` | GNM sub-cursor (`cb+0xc090`) — first arg of every PM4 emitter | `0085fc10.c:35` etc. |
| `+0x20` (alloc header) | user-data heap: `[0]`=grow callback, `[4]`=bump cursor, `[5]`=base, `[6]`=end | `008643f0.c:11-27` |

Note the mapping `cb+0xc398 / 0xc3a8 / 0xc3b8` (used by the indirect draw objects to find the
start-vertex nibble byte) are exactly `material+0xc0b0 / 0xc0c0 / 0xc0d0` (the VS / ES / LS
record pointers) — the two maps agree (`0010add0.c:20`, `00862510.c:16,23,33` vs
`0085fc10.c:33,67,133`). Likewise `cb+0xc8b0/c8b4` = `material+0xc5c8/c5cc` — the
on-chip-GS-ring cache whose `!= 0` test gates the vertex-cache reset in the pass functions
(`00860c50.c:74-75` writes it, `00093960.c:82-88` tests it).

### 1.1 The three group-pending bits

`0x85fc10` is one function with three independent dirty groups, all reading
`uVar8 = *(u32*)(material+0xc260)`:

1. **Group A** (`bit 0x20`, `0085fc10.c:29-193`) — the *ES/GS (tessellation) group*: ES
   shader (`+0xc24c`), GS shader (`+0xc24b`), ES user-data (stage **4**, `+0xc254`), GS
   user-data (stage **3**, `+0xc253`). Clears `+0xc254`, `+0xc253`, `+0xc24b..c24c`,
   `+0xc25b` at the end (188-191).
2. **Group B** (`bit 1`, `0085fc10.c:194-367`) — the *LS/HS (tessellation-control) group*:
   tessellation constant block, LS shader (`+0xc24e`), HS shader (`+0xc24d`), LS user-data
   (stage **6**, `+0xc256`), HS user-data (stage **5**, `+0xc255`). Clears `+0xc256`,
   `+0xc255`, `+0xc24d`, `+0xc25d` (363-366).
3. **Group C** (`0085fc10.c:368-527`) — the *VS/PS group*, checked unconditionally (not
   gated on a `+0xc260` bit): VS shader (`+0xc24a`), PS shader (`+0xc249`), VS user-data
   (stage **2**, `+0xc252`), PS user-data (stage **1**, `+0xc251`). Clears `+0xc251`,
   `+0xc249`, `+0xc259`, `+0xc252`, `+0xc24a`, `+0xc25a` (522-527).

The compute sibling `0x85f220` (489 B) handles the CS group (`+0xc248`): CS shader via
`0x86c7d0` = `sceGnmSetCsShaderWithModifier(pm4, 0x19, record+8, 0)` + CS user data; the
combined graphics+compute bind is `0x85f410` (2042 B, called by `0x862c50`).

## 2. `0x85fc10` — Material_BindGraphicsShaders_B, 3994 B (full flow)

`void FUN_0085fc10(material)` — one arg, the material block (`cb+0x2e8`). Every PM4 write goes
to the GNM sub-cursor `*(material+0xc090)` (the standard `cb+0x08/0x10/0x18/0x20` reserve/
cursor contract, `0086d4c0.c:10-23`).

### 2.1 How the VS/PS(/GS) pair is selected

The bind does **not** choose shaders; it drains whatever the record setters staged:

- **Per-draw path** (`0x11d3d0` chain): `0x14d7b0(record[1])` resolves the draw record to a
  material — it returns `*(record+0x20)`, lazily building through the vtable at
  `record+0x68` (short-circuits when `+0x98==0xb` or `+0x60==0`) — **proven**
  `0014d7b0.c:11-31`.
  `0xf8c90` → `0xf8990` then binds the slot's records by pass slot `k = *(int*)(ctx+0xf4)`
  (stride 0x48): `FUN_000f8aa0(cb, material+0x98+0x48*k)` (misc state), `FUN_000937c0(cb,
  *(material+0x70+0x48*k))` (a stage record by `*(u32*)ctx[4]`: slot idx 2 → `0x861520` (VS),
  6 → `0x861260` (LS), 4 → `0x860bb0` (ES)), `FUN_00094ab0(cb, *(material+0x90+0x48*k))`
  (PS: `FUN_00861600(material, ps_record, ps_record+8)` + SPI_PS_INPUT_CNTL via `0x86bc00`),
  then blend/SC-mode refresh (`0x919e0`/`0x91cd0`). — proven `000f8990.c:9-41`, `000937c0.c`,
  `00094ab0.c:17-18`.
- **Fullscreen-rect path** (`0x93690`): the engine's clear shader list `*(DAT_02ee3ce0+0x8a8)`
  → `+0x68` → load `0x95580` → resolve `0x14d7b0(*(list+0x20))` → `plVar1 = *(mat+0x60)` =
  a stage record; bound by the same `pass[4]` stage selector. — proven `00093690.c:10-24`.

Each setter writes the pending/changed bytes AND stages (a) the record pointer, (b) the
32-bit `shader_modifier`, (c) the 64-bit stage constant, (d) the layout-record pointer, and
computes the "new record is register-identical to the old" bit from the record's `+0x18..+0x34`
words (`00861520.c:18-21` VS, `00861600.c:20-27` PS — 3 words for VS, 7 for PS). That bit
selects `sceGnmSetXxxShader` vs `sceGnmUpdateXxxShader` in the bind
(`0085fc10.c:373-377` VS: `cVar6 == 0 ? 0x86c170 : 0x86c250`; PS `0085fc10.c:390-394`).

### 2.2 Shader-record setters (the staging API)

| VA | Setter | Writes (besides dirty bytes) |
|---|---|---|
| `0x8616f0` | CS record | `+0xc0a0=rec`, `+0xc180=layout`, `+0xc5d9 = rec->u8@0x24 & 3` |
| `0x861600` | PS record | `+0xc0a8=rec`, `+0xc188=layout`, "identical" bit from rec+0x18..0x34 (7 dwords) |
| `0x861520` | VS record (+embeds the GS-path VS sub-record) | `+0xc0b0=rec`, `+0xc190=embedded_vs`, `+0xc130=const`, `+0xc168=modifier`, `+0xc613 = layout->u16@0x14 != 0xffff` |
| `0x860c50` | GS record (578 B) | `+0xc0b8=rec`, `+0xc198=layout`; **re-stages the embedded VS**: picks `rec+0x38+4*rec->u8@3` (or the `material+0xc268` alt block when the selected dword's `& 0xff000000 == 0x1000000`), sets `+0xc0b0`, `+0xc130=0`, `+0xc168=0`; computes the on-chip GS ring size class → `0x86c650`, caches `+0xc5c8=0x180003`, `+0xc5cc=class` |
| `0x860bb0` | ES record | `+0xc0c0=rec`, `+0xc1a0=layout`, `+0xc140=const`, `+0xc170=modifier`, `+0xc5e4=0` |
| `0x861260` | LS-record family setter | `+0xc0d0=rec`, `+0xc1b0=layout`, `+0xc150=const`, `+0xc178=modifier` |
| `0x8612b0` | HS record (293 B) | `+0xc0c8=rec`, `+0xc1a8=layout`, `+0xc5f0` (LS const), `+0xc5f4=param_4`, `+0xc608=0` |
| `0x8619a0` | bulk **stage-2 (VS) staging**: for each of `n` u64s, if the layout's u16 table at `layout+0x82+2*i` names a slot, either `0x86d5e0` (negative = direct SGPR write) or stash into the `+0x3090` staging array | `008619a0.c:12-37` |
| `0x8611a0` | explicit stage-2 (VS) staging of n u64 values (u16 table at `layout+0xca`) | `008611a0.c:12-36` |

### 2.3 What the bind emits, group by group (exact calls)

**Group A — ES/GS (tessellation geometry), `0085fc10.c:29-193`:**
- ES changed (`+0xc24c`): `puVar13 = ES record` (`+0xc0c0`); if `+0xc5e4 == 0` pass
  `record+8` (the 7-dword register image at `rec[2..]`) with modifier `*(u32*)(+0xc170)`;
  else copy the record's first 0x18 bytes through `FUN_00880330` (patch) first. Then
  `FUN_0086c2c0(pm4, regs, modifier)` = `sceGnmSetEsShader(pm4, 0x50, regs, modifier)`
  (`0085fc10.c:31-48`); SQTT marker if `+0x88`.
- GS changed (`+0xc24b`): `FUN_0086cb00(pm4, gs_record+8)` or `FUN_0086cb60()` (update
  variant, when the "identical" byte `+0xc25b` is set) = `sceGnmSetGsShader` (0x74 dwords).
- **Stage-4 (ES) user data** (`+0xc254`, `0085fc10.c:66-131`): `n = *(u16*)layout(+0xc1a0)`;
  `dst = FUN_008643f0(material, n)` (bump-alloc n dwords in the material's user-data heap);
  `memcpy(dst, material+0x6090, n*4)`; then, driven by the layout record's u16 fields
  (each u16 encodes an SGPR slot: byte 0 = SGPR index, byte 1 = "point into the uploaded
  block at dword offset", 0xffff = absent, byte1==0xff of a nonzero u16 = "write the block
  base at dword offset"):
  - `+0x7` → `FUN_0086d4c0(pm4, stage=4, sgpr, dst)` (block base, 64-bit);
  - `+0x8/0xa/0xc` → `FUN_0086d4c0(pm4, 4, sgpr, dst + off*4)` (sub-block pointers at
    u16 offsets `+0x16/0x1a/0x1c`);
  - `+0xc` second half → `FUN_0086d4c0(pm4, 4, sgpr, material+0x78)` (the SQTT buffer);
  - `+0xd` → `FUN_0086d5e0(pm4, 4, sgpr, material+0xc0f0, 1)` (one dword from
    `+0xc0e4..0xc118` per stage);
  - `+0xe` → `FUN_0086d5e0(pm4, 4, sgpr, material+0xc110, 1)`;
  - tail (from the stage-4 staging pointer `+0xc1a0`): u16@+4 → `+0xc140` (64-bit const)
    or a block offset; `+0x6` → block offset.
- **Stage-3 (GS) user data** (`+0xc253`, `0085fc10.c:132-187`): same pattern against
  `+0x4890`/layout `+0xc198`/`+0xc0ec`/`+0xc10c`, plus the texture-size constant:
  `local = (u32)( (*(u32*)(+0xc5e8) >> 0x10) * (*(u32*)(+0xc5e8) & 0xffff) )`,
  `FUN_0086d5e0(pm4, 3, sgpr, &local, 1)` (`0085fc10.c:180-186`).
- Clears the group's pending bytes (`188-191`).

**Group B — LS/HS (tessellation control), `0085fc10.c:194-367`:**
- If `+0xc612` (LS/HS present): when LS or HS changed, rebuild the 0x24-dword tessellation
  constant block: `FUN_008643f0(material, 9)` → `FUN_00880530(material+0xc60c, dst,
  ls_rec+0x24, hs_rec->u32@0x1c, material+0xc5f4)` → stash through `0x8655d0`/`0x865360`
  into `+0xc5f8`. Then the HS layout's u16@+0xa8 names an SGPR slot: negative →
  `FUN_0086d5e0(pm4, 5, slot&0x3fff, material+0xc5f8, 4)` (16 bytes of the block directly);
  positive → stash 16 bytes into the stage-7 staging table at
  `material+0x7890+4*slot` (`0085fc10.c:199-217`). Sets `+0xc255=1` (HS data changed) and
  `FUN_008619a0(material, (bit5 ? 4 : 2), 0x13, 1, material+0xc5f8)` — stages the block
  into stage **4 (ES)** when the `+0xc260` ES/GS bit is still set at the line-192 re-read,
  else stage **2 (VS)** — through the target layout's u16 table at index `0x13`
  (`0085fc10.c:219`, `008619a0.c:14-29`, slot map at `layout+0x82`).
- LS changed (`+0xc24e`): `FUN_0086c760(pm4, ls_record+8, *(u32*)(+0xc178))` =
  `sceGnmSetLsShader` (0x5c dwords).
- HS changed (`+0xc24d`): builds the 4-qword HS register image (`uStack_54..`) with
  `FUN_008805c0(local_40, ls_rec+0x24, material+0xc5f4)`; if `*(s32*)(+0xc608) < 0`,
  patches via `FUN_00880480(&regs, *(u32*)(+0xc608) & 0x7fffffff)`; then
  `FUN_0086c330(pm4, &regs, local_40)` or `FUN_0086c460()` (update) =
  `sceGnmSetHsShader` (0x78 dwords).
- Stage-6 (LS) user data (`+0xc256`): against `+0x9090`/layout `+0xc1b0`/staged
  `+0xc0f8`/`+0xc118`.
- Stage-5 (HS) user data (`+0xc255`): against `+0x7890`/layout `+0xc1a8`/staged
  `+0xc0f4`/`+0xc114`.
- Clears `+0xc256`, `+0xc255`, `+0xc24d`, `+0xc25d` (`363-366`).

**Group C — VS/PS, `0085fc10.c:368-527`:**
- VS changed (`+0xc24a`): `FUN_0086c170(pm4, vs_record+8, *(u32*)(+0xc168))` =
  `sceGnmSetVsShader(pm4, 0x1d, regs, modifier)`, or `FUN_0086c250()` =
  `sceGnmUpdateVsShader` when the "identical" byte `+0xc25a` is set (`0085fc10.c:369-378`).
- PS changed (`+0xc249`): `ps_regs = ps_record ? ps_record+8 : NULL`;
  `FUN_0086c050(pm4, ps_regs)` = `sceGnmSetPsShader350(pm4, 0x28, ps_regs)` or
  `FUN_0086c110()` = `sceGnmUpdatePsShader350` (`0085fc10.c:383-395`).
- Stage-2 (VS) user data (`+0xc252`): against `+0x3090`/layout `+0xc190`/staged
  `+0xc0e8`/`+0xc108`, plus the texture-size dword exactly like stage 3
  (`0085fc10.c:465-471`).
- Stage-1 (PS) user data (`+0xc251`): against `+0x1890`/layout `+0xc188`/staged
  `+0xc0e4`/`+0xc104`.
- Clears `+0xc251`, `+0xc249`, `+0xc259`, `+0xc252`, `+0xc24a`, `+0xc25a` (`522-527`).

### 2.4 The SET_SH_REG user-data writers (exact packets; proven)

All three take the GNM sub-cursor `pm4 = *(material+0xc090)` and consult
`DAT_01290410[stage]` (u16 per stage; eboot bytes at VA `0x1290410`, file offset `0x1294410`).

- **`0x86d550` — one dword** (`0086d550.c:19-21`; the `(stage==0)` addend appears twice, so
  stage 0 gets +2):
  header `0xC0017600` (stage 0: `0xC0017602` — header bit 1 = the *Compute* shader-type
  bit),
  `[1] = param_3 - 0x2c00 + DAT_01290410[stage]`, `[2] = value`.
- **`0x86d4c0` — 64-bit** (`0086d4c0.c:19-23`):
  header `0xC0027600` (stage 0: `0xC0027602`), same reg formula, `[2..3]` = value lo/hi.
- **`0x86d5e0` — N dwords** (`0086d5e0.c:18-61`):
  writes a type-3 header pair: `[0] = 0xC0001000`, `[1] = 0x6875000d` (a
  `PM4Type3Header{Nop}` + literal `0x6875000d` = prefix dword), then
  `[2] = 0xC0007600 | (N << 16) | ((stage==0) << 1)`,
  `[3] = param_3 - 0x2c00 + DAT_01290410[stage]`, then the N dwords (fully unrolled switch).
  Both writers check `(end-cursor)/4` and call the reserve callback at `cb+0x18` first.

Header bit1 = `PM4ShaderType` (`pm4_cmds.h:62`: 0 = Graphics, 1 = Compute) — the port decodes
`SET_SH_REG` with the low byte of `[0]` only as shader-type context (`liverpool.cpp:1307/1533/2360`);
the register body lands at `ShRegWordOffset + reg_offset`. (The ABI doc's §1 header values
`0xC0017601`/`0xC0027601` for stage 0 are off by one: the guest adds the shader-type addend
twice, giving `…602`.)

**Stage table ↔ hardware SPI blocks (resolved, correcting the ABI doc's labels).** The stage
ordinal is the GNM shader-stage enum — **0=CS, 1=PS, 2=VS, 3=GS, 4=ES, 5=HS, 6=LS** (the
same order as the port's `enum ShaderStages { Cs, Ps, Vs, Gs, Es, Hs, Ls }`,
`gnmdriver.h:46-56`, and the same ordinal as the material slot arrays §1). Each graphics
stage occupies a 0x40-wide SH register block with `SPI_SHADER_PGM_LO_x` at block+8 (the
port's own setters: `SetPsShader350` PGM at SH offset 8, `SetVsShader` 0x48, `SetGsShader`
0x88, `SetEsShader` 0xc8, `SetHsShader` 0x108, `SetLsShader` 0x148 — `gnmdriver.cpp:1746,
1848, 1616, 1587, 1643, 1682`), so `SPI_SHADER_USER_DATA_x_0 = block + 0x0c`:

| stage | PGM_LO | DAT_01290410[stage] (USER_DATA_0) |
|---|---|---|
| CS (0) | — (compute bank, `sceGnmSetCsShaderWithModifier`) | **0x2e40** |
| PS (1) | 0x2c08 | **0x2c0c** |
| VS (2) | 0x2c48 | **0x2c4c** |
| GS (3) | 0x2c88 | **0x2c8c** |
| ES (4) | 0x2cc8 | **0x2ccc** |
| HS (5) | 0x2d08 | **0x2d0c** |
| LS (6) | 0x2d48 | **0x2d4c** |

Every value in the dump matches block+0x0c exactly, which also explains the `(stage==0)`
header addend: stage 0 is the only *compute* stage, and the PM4 type-3 header bit 1 is the
shader-type field (`pm4_cmds.h:62`, 0 = Graphics, 1 = Compute). Usage cross-check inside the
bind itself: the stage-4 flush pairs with the ES layout (`+0xc1a0`), stage-3 with GS
(`+0xc198`), stage-5 with HS (`+0xc1a8`), stage-6 with LS (`+0xc1b0`), stage-2 with VS
(`+0xc190`), stage-1 with PS (`+0xc188`) — i.e. the user-data flush stage, the layout-record
slot and the SET_SH_REG window are all the same ordinal. This supersedes the ABI doc's
"stage 0 = VS" label (which belonged to the `DAT_01290410` dump's unlabelled row). One
detail stays inferred: in the compute decode the 0x2e40 write lands at `reg_offset-0x200 =
0x40` of the port's `ComputeProgram` — i.e. the *last* of the 16 `COMPUTE_USER_DATA`
dwords — so the guest's CS user-SGPR slot numbering is probably GNM's reversed compute
map (slot 0 → highest HW register); unproven, immaterial to the port (the recompiler reads
the 16 dwords as a flat array either way).

### 2.5 The `0x8643f0` user-data heap

`FUN_008643f0(material, n_dwords)` bumps the material's own user-data heap
(`material[4]` = cursor, `material[5]` = base, `material[6]` = end; on overflow calls the
grow callback `material[0]` and re-bases, `008643f0.c:11-27`) and returns a **GPU-visible
pointer** used directly as the SET_SH_REG value. So the guest does bindless-once: the 16-dword
user-SGPR window *points into* these per-material uploaded blocks.

### 2.6 Interaction with the material block at `cb+0x2e8`

`0x85fc10`'s only argument **is** `cb+0x2e8` — the "material block" is a sub-struct of the
device/cursor object (`0x93960:94` calls `FUN_00861600(*param_1 + 0x2e8, …)`; the draw
objects call `FUN_0085fc10(cb + 0x2e8)`, `00094d00.c:23-25`). It holds: the staging arrays
(§1), the record/layout pointers (§1), the dirty bytes (§1), the user-data heap (§2.5), and
the GNM sub-cursor pointer `+0xc090`. All PM4 emitters in §2.3 write through that sub-cursor
into the same DCB the pass functions write to.

### 2.7 Root-table builders (per-draw resources)

- **`0x95320` — root-table builder (252 B).** `FUN_00095320(u32 mask, u64* src, u32 tag)`:
  popcount(mask) (SWAR) → `FUN_00012820(popcount*8, tag, 8, 0)` (a **bump allocator** keyed
  by an arena id: `tag==1` selects the TLS arena at `fs:[-0x18]+0x400[...]`, else
  `DAT_02dfaab0[tag]`; vtable call `+0xa0` = alloc, `+0x98` = "can free" query,
  `00012820.c:18-49`) → copy `src[i]` for each set bit of `mask` ascending
  (`00095320.c:14-53`). The **return value is the allocation pointer**: raw eboot bytes
  show `e8 …` (call `0x12820`, at `0x95360`) followed immediately by `48 89 c1`
  (`mov rcx, rax` — the
  copy cursor starts at the allocation) and the epilogue
  (`0x95400`, file `0x99400`: `49 8b 56 40 48 89 11 48 83 c1 08 f6 c7 02 74 07 49 8b 56 48
  48 89 11 5b 41 5e 5d c3`) never writes `rax` — so `rax` still holds the table base at
  `ret`. That closes master-map open item 1: the decompile's `void` was a recovery
  artifact; callers (`0x11d3d0:52-53`, `0x94fb0`, `0x11d5b0`, …) consume the returned
  table address. In practice `mask` is `u32(material+0x468)` (the material's binding mask)
  and `src = job_ctx+0x90` (`param_3 + 0x12` qwords, `0011d3d0.c:52` — the 10-qword
  binding window the pass runner filled with view constants / object blocks / V#/T#
  tables), so the table = the subset of those 10 bindings the material's shaders actually
  reference, freshly packed per draw into frame memory. → Vulkan: this is the **per-draw
  descriptor payload**; the address handed to the shaders is what the port's SRT walker
  flattens.
- **`0xae150` — user-data table binder (441 B).** `FUN_000ae150(pass_ctx, u64 table)` —
  `binder` is the **pass context** (its first qword is the cb): enable mask
  `uVar1 = *(u32*)(binder+0x18)` (6 bits; the pass functions write it directly —
  `0x93960:92` writes `0x11` = bits 0,4, `0x94600:29` writes `0x20` = bit 5); for each set
  bit i (0..5), stage `s = *(u32*)(*(binder+0x20) + 4*i)` (a static per-pass stage-index
  table: `PTR_DAT_01604408` / `PTR_DAT_01604428`, whose dwords are the same ordinals
  `0x937c0`/`0x94ab0` use to pick the VS/PS record), guarded by
  `binder[0x3b+s] != NULL && *(u8*)(binder[0x3b+s]+0x19) == 2` (the cached shader record at
  `pass+0x1d8+8s` must be a type-2 record = "accepts the draw table"), then
  `FUN_00862490(*binder + 0x2e8, s, &table, 2)` (`000ae150.c:12-58`; `*binder` = cb).
  So the **same 8-byte root-table pointer** is written into every active stage's user-data
  window.
- **`0x862490` — root-table → SET_SH_REG (126 B).** `FUN_00862490(material, stage, ptr, 2)`:
  reads the stage's layout record `*(material+0xc180+8*stage)`, takes its u16@+0x10, and
  calls `FUN_0086d5e0(pm4, stage, lo16&0xff, ptr, hi16>>8)` — i.e. the layout decides *which
  SGPR slot* (`lo16 & 0xff`) and *how many dwords* (`hi16 >> 8`, here 2) receive the table
  pointer. If `stage == 2` and `material+0xc613` (GS tess mirror), also writes the mirror
  layout `+0xc1b8` via `FUN_00884240` on a second cursor `material+0xc098` (`00862490.c:8-14`).
  The trailing `2` = dword count: **the root table is a 2-dword user-SGPR write — the 64-bit
  GPU address of the packed table.**
- **`0x123810` / `0x123cc0` — instance/object block writers.** `FUN_00123810(renderable,
  pass, index, flag, uint* inst)` (`00123810.c`): (1) writes the **instance V# descriptor**
  to `pass[0x15]` (= `pass+0xa8`): `inst[0] | (inst[1]&0xfff)<<32`; (2) optional scope bind
  `0xab310(*(renderable[0xb]+0x78), pass)`; (3) `lVar2 = *(renderable+0xf0)`,
  `FUN_00193580(lVar2 + index*0x58, pass)` (per-instance scope/constant bind); (4) when bit 2
  of the cb's qword at `cb+0x8` is clear: load `0x95580`, resolve the material `0x14d7b0`,
  then index-buffer bind
  `0x94c60(cb, *(renderable+0x50+index*0x58)+0x28)` or the flag path `0x1122c0`; (5) if
  `*(int*)(pass+0xf4) - 2 < 4` (pass slot k ∈ {2,3,4,5}) compute the stencil ref word
  `uVar3 = (renderable->u32@0x10 >> 0x1a & 0x3c) | (>> 0x13 & 0xf) | 0x40` and, when changed
  against the cached words, `FUN_00869bb0(cb, front, back)` = DB_STENCILREFMASK (regs
  0x10c/0x10d) and cache `pass+0x124/0x126/0x127` (front) / `+0x129/0x12a/0x12b` (back).
  `0x123cc0` is a tail-call thunk. → Vulkan: `vkCmdBindIndexBuffer` + the instance buffer
  descriptor + `vkCmdSetStencilReference`.

### 2.8 The exact Vulkan mapping of the bind (per the ABI doc)

Per-stage user-data windows (2.4) are exactly the 16-dword `ShaderProgram::user_data` the
port already flattens (`native-render-abi.md` §2). Concretely, per stage:

| Guest user-SGPR write | Vulkan (native backend) |
|---|---|
| `0x86d4c0(pm4, s, sgpr, heap_ptr)` — block base (u64) | BDA of a `VkBuffer` whose device memory holds the uploaded block; becomes the descriptor set's buffer binding (the SRT walker flattens the pointed-to dwords) |
| `0x86d4c0(pm4, s, sgpr, heap_ptr + off*4)` — sub-block pointer | same buffer, `offset = off*4` (a `VkDescriptorBufferInfo` with the exact offset) |
| `0x86d5e0(pm4, s, sgpr, material+0xc0e4..c118, 1)` — 1 dword | push constant (4 bytes at the stage's SGPR slot) |
| `0x86d5e0(pm4, s, sgpr, &tex_size, 1)` — (hi16*w)*(lo16*h) | push constant (texture size; used by the stage-2/3 samplers) |
| `0x86d5e0(pm4, s, sgpr, material+0x78, 1)` — SQTT marker buffer | profiling-only; drop (emit nothing) when SQTT is off |
| `0x86d5e0(pm4, 5, sgpr, material+0xc5f8, 4)` — LS/HS const block (16 B) | push constants (tessellation factors/levels) or a small UBO |
| `0x86d5e0(pm4, s, lo, &root_table, 2)` — 64-bit table address (`0x862490`) | the per-draw descriptor payload: the 64-bit BDA of the packed root table → the SRT walker's input; descriptor set 0 |
| `0x86d550(pm4, s, nibble, start_vertex/base_instance)` (`0x862510`) | the classic `gl_BaseVertex`/`gl_BaseInstance` SGPRs → push constants (or `vkCmdDraw` firstVertex/firstInstance when the layout allows) |

`0x86c170`/`0x86c050`/`0x86c2c0`/`0x86cb00`/`0x86c760`/`0x86c330` (Set*Shader) → the
`vkCmdBindPipeline` equivalent: the shader *program* addresses (`record[0]` = 32-bit GPU VA,
`record[1]` must be 0 = hi) select the pipeline; the record's `RSRC1/2`, `SPI_*_FORMAT`,
`PA_CL_VS_OUT_CNTL`, `SPI_VS_OUT_CONFIG`, `DB_SHADER_CONTROL`, `CB_SHADER_MASK` words
(`gnmdriver.cpp:1746-1758`, `:1848-1852`) are pipeline state → they belong in the
`vk_graphics_pipeline_state` key, not in dynamic state. `Update*Shader` (identical-record
path) re-writes only the PGM_LO/RSRC pair — i.e. a pipeline re-bind with changed module
offsets only.

### 2.9 The shader-pair cache key (what identifies a unique pipeline)

The guest has **no cache object**; uniqueness is content-addressed at four levels
(all proven):

1. **Program identity** = the 7-dword record image passed to `sceGnmSetXxxShader`: dword 0 =
   32-bit shader GPU VA (`regs[1]` must be 0), dwords 2..3 = `SPI_SHADER_PGM_RSRC1/2`,
   dwords 4..6 = per-stage extra regs (`SPI_VS_OUT_CONFIG`/`SPI_SHADER_POS_FORMAT`/
   `PA_CL_VS_OUT_CNTL` for VS; `SPI_SHADER_Z_FORMAT`/`SPI_SHADER_COL_FORMAT`/
   `SPI_PS_INPUT_ENA`/`SPI_PS_INPUT_ADDR`/`SPI_PS_IN_CONTROL`/`SPI_BARYC_CNTL`/
   `DB_SHADER_CONTROL`/`CB_SHADER_MASK` for PS; `VGT_STRMOUT_CONFIG`/`VGT_GS_OUT_PRIM_TYPE`/
   `VGT_GS_INSTANCE_CNT` for GS; `VGT_HOS_*_TESS_LEVEL`/`VGT_TF_PARAM`/`VGT_LS_HS_CONFIG`
   for HS/LS). → the port's pipeline key must include **(code address, RSRC1, RSRC2, and
   those per-stage state words)**.
2. **Shader modifier** = `material+0xc168/0xc170/0xc178` (u32 staged per setter) — folded
   into `RSRC1` by the driver (`var = (rsrc1 & 0xfcfffc3f) | modifier`,
   `gnmdriver.cpp:1586,1847`); part of level 1.
3. **User-data layout identity** = the layout record per stage
   (`material+0xc180+8*stage`): its u16 table (SGPR slot ↔ meaning) decides *which* dwords
   the shader dereferences; two materials sharing the same program but different layouts
   are different pipelines (they read different SGPR windows). → include the layout-record
   identity (or its flattened SGPR-slot map) in the key.
4. **The "identical" fast path** (`+0xc264` bits) compares only the *record content* words
   (`+0x18..+0x34`), never the user-data contents — so user-data changes never re-select a
   pipeline, matching the port's split of pipeline (static state) vs descriptors (dynamic).

A safe Vulkan-side key is therefore: `{VS code VA, PS code VA, (GS/ES/LS/HS code VAs when
present), each stage's RSRC1|modifier, RSRC2, per-stage state words, per-stage layout-record
identity, PS input-ena/addr words, CB_SHADER_MASK, DB_SHADER_CONTROL}` + the render-pass/
format state from the RT binder (§3) + the fixed-function state already mapped in the pass
map (blend, DB, SC-mode). The first two elements alone are *necessary but not sufficient*.

## 3. Render-target binding

### 3.1 `0x92140` — render-target set binder, 924 B

`FUN_00092140(undefined8* pass, long* param_2)` — `param_1` = pass ctx, `param_2` = array of
9 descriptors: `param_2[0..7]` colour pass-targets, `param_2[8]` depth pass-target,
`param_2[9]` (byte at +0x48) selector. The `+0x1ac/+0x11c/+0x16..18` state caches named in
the contract live in the pass context and are written by the pass functions, not here —
`0x92140` writes `+0x10/+0x11/+0x48+8i/+0xa8` (see §5).

Flow (proven `00092140.c`):
1. Resolve 8 colour pointers `local_78[i]`: descriptor null or `(*(u8*)(d+0x62) | 2) != 3`
   (type must be 1 or 3) → NULL; else `d + 0xe4` (the 8-qword CB descriptor block) or
   `d + 0x124` when `*(byte*)(param_2+9) & 1` (the alternate/target-manager view block).
   Remember the first non-null descriptor (`lVar7`).
2. `lVar1 = param_2[8]` (depth descriptor); `lVar5 = first-non-null(colour, depth)`;
   `uVar2 = FUN_00096f50(lVar5)`, `uVar3 = FUN_00096f60(lVar5)` (width/height from the
   descriptor), then `FUN_00091fc0(pass, 0, 0, w, h)` — emit the viewport/scissor packets
   for those dims.
3. Loop `i = 0..7`: if force-all (bit0 of `pass+0x8`) OR dirty bit `i` of `*(u32*)(pass+0x10)`
   OR `pass[9+i] != lVar10` (byte offset `+0x48+8i`, cached descriptor pointer changed) →
   `FUN_0085cbc0(cb, i, lVar10)` + cache `pass[9+i]` and clear dirty bit i.
4. At `i == 7`: depth block `puVar9 = lVar1 ? lVar1+0x164 : NULL`; if dirty/force or
   `pass[0x15] != puVar9` → `FUN_0086a290(cb, puVar9)` (DB_Z_INFO.. block), and when
   non-null also `FUN_0086e3a0(cb, *puVar9 & 3)` (PA_SU_POLY_OFFSET_DB_FMT_CNTL from the
   depth-format class) + `FUN_00869f50(0, cb)` (DB_DEPTH_CLEAR = 0.0); cache `pass[0x15]`,
   clear depth-dirty `pass+0x11` bit0.
5. Encode pass state into `*(u32*)(pass+0x8)` bits 3-4 from `param_2[9]` (`sel`):
   `sel & 1` set → 0; `sel & 1` clear → `0x10`, overridden to `(sel*2) & 8` when
   `(~sel & 0xc) != 0` — a 2-bit "RT configuration" code; clear the force bit
   (`00092140.c:179-184`).

### 3.2 `0x85cbc0` — SetRenderTarget (one colour slot), 269 B

`FUN_0085cbc0(cb, slot, u64* desc)`:
- NULL → `FUN_0086a1a0(cb, slot, 0)` (unbind: single SET_CONTEXT_REG `0xC0016900`
  reg `0x31c + 0xf*slot` = CB_COLORn_INFO = 0; proven `0086a1a0.c:57-62`).
- Else copy the 8 qwords; when `(desc[2].lo & 0x6000) == 0x2000` (one specific tile-mode
  class) **and** `FUN_0087f0d0(&local)` returns 0 **and** `desc[3].hi != 0`, patch the copy:
  clear bit 10 of `desc[2].lo`, re-pack `desc[2].hi` (`(x & 0x1f) << 5 | x & 0xfffffc1f`),
  re-pack `desc[5].lo` (`(x & 0xffc00000) | (desc[1].lo & 0x3fffff)`), re-pack `desc[0].hi`
  (`(x & 0x7ff) << 0x14 | x & 0x800fffff`), then `uVar2 = FUN_0087eda0(&local)`
  (recompute the aligned surface address from the format words — it reads the
  format/attribute fields of the qword-2 pair) and `FUN_0087efd0(&local, uVar2)` stores it
  back (`0085cbc0.c:31-41`). Exact bit semantics of the three helpers not decoded (gap
  N2-7).
- `FUN_0086a1a0(cb, slot, block)`: when `FUN_0087eda0(block)` says the format words are
  **valid** (`!= 0`), emit 18 dwords: header `0xC00E6900` (SET_CONTEXT_REG, 14 body
  dwords) at reg `0x318 + 0xf*slot` = CB_COLORn_BASE, the 14 dwords from block qwords 0..6
  (the CB_COLORn register run — BASE, INFO at `0x31c` = +4, VIEW, ATTRIB, FMASK …), then a
  trailing 2-dword count-0 marker `{0xC0001000, block[7].hi}` (the same shape as the
  `{0xC0001000, 0x6875000d}` prefix `0x86d5e0` writes). When the format words are
  **invalid** (`== 0`, including a NULL descriptor), emit a single SET_CONTEXT_REG
  `{0xC0016900, 0x31c + 0xf*slot, 0}` — CB_COLORn_INFO = 0, i.e. the slot is *disabled*
  (`0086a1a0.c:32-62`).
  → Vulkan: the colour attachment's `VkImageView` + `VkFormat` (the attachment's `VkImage`
  comes from the BASE address), and the aux/fmask words map to the port's
  `CB_COLORn_ATTRIB_AUX` handling (DCC/FCV).

### 3.3 How render targets are referenced

- **Guest-side identity**: a *pass-target descriptor object* (`param_2[i]`, ≥ 0x1c8 bytes)
  with: type byte `+0x62` (must be 1 or 3 for colour, 2 for depth in `0x94600`),
  flags `+0xf5`/`+0x167`, viewport dims `+0x120`/`+0x194` (two u16s), the colour CB
  descriptor block at `+0xe4` (8 qwords) or the alternate at `+0x124`, and the depth block
  at `+0x164` (12 dwords, DB_Z_INFO..DB_STENCIL_WRITE_BASE). These objects are owned by the
  target manager `M = *(F+0xd98)`: the runners bind **sub-ranges of M** (`V+0x29a0` SetA,
  `V+0x2a40` SetB, `V+0x2590`, `V+0x24a0`, `M+0x31c0..0x3208`, `M+0x2b80..`) — each is an
  array of such descriptors (frame map §0, §9.1, §10.3).
- **GPU-side identity**: the 8-qword CB block is the raw CB_COLORn register set
  (BASE/PITCH/SLICE/VIEW/INFO/ATTRIB/ATTRIB_AUX/FMASK…), whose BASE dwords carry
  `(gpu_va >> 8) & 0xffffff` (the `FUN_0085cbc0` patch writes `addr>>8` into bits 0..23 of
  `local_78`; the port's `AmdGpu::CB_COLORn_BASE` uses the same `Address() << 8` convention);
  the depth block's DB_Z_READ_BASE/DB_Z_WRITE_BASE likewise carry `gpu_va >> 8`. So the PM4
  refers to **guest GPU VAs**, resolved by the port's liverpool/AC pipeline through the
  registered buffer/image table; the *host* binds through the target-manager objects.
- There is **no resource handle table** on this path: the only indirection is the
  `param_2[9]&1` selector between the descriptor's two embedded blocks (+0xe4 vs +0x124),
  which is how the frame map's "Set A/B" aliasing of the same descriptor to different views
  is expressed.

### 3.4 Lifetime / aliasing rules (observable)

1. **Pointer-identity caching**: both `0x92140` (per slot: `pass[9+i] != desc_ptr`) and
   `0x93960` (RT0: `param_1[9] != lVar1`) skip the rebind when the *descriptor pointer* is
   unchanged — so the same target descriptor rebound across passes costs nothing, and a
   *changed* pointer always re-emits even if the contents are equal (no content compare on
   the CB block itself).
2. **Dirty bits**: `pass+0x10` (8 colour bits) and `pass+0x11` bit0 (depth) are cleared as
   each slot is emitted and set by the *callers* (`0x94600` sets `+0x11 |= 1` at line 97);
   bit0 of `pass+0x8` ("force all") bypasses the cache entirely (`00092140.c:150-157`).
3. **Aliasing**: the `param_2[9]&1` (+0xe4 vs +0x124) selector and the two G-buffer sets
   (`V+0x29a0` depth-only vs `V+0x2a40` 7-colour, both over `kRTT_MainDepth`) are the
   aliasing mechanism: the same depth image is bound through different descriptor blocks in
   different passes. The G-buffer flow binds SetA → clears → RenderDepth → binds SetB
   (frame map §10.3) — i.e. a target can be rebound in the same frame with a *different*
   descriptor pointer, which the cache treats as a fresh bind. Resolve/clear ordering is
   explicit in the callers (`0x932a0` clear then binds, EOP+WAIT fences between passes).
4. **Depth state co-emitted**: every depth rebind co-emits DB_DEPTH_CLEAR=0.0 (bind time)
   and the callers restore DB_DEPTH_CLEAR=1.0 after the clear pass (`0x94600:90`), so the
   clear value lives in the DB state, not the target object.
5. **No ownership**: no refcount/free anywhere in `0x92140`/`0x85cbc0`/`0x86a1a0` — the
   descriptor objects outlive the pass; lifetime is the frame graph's job (the port's
   `native_pass.cpp` should treat the target-manager entries as frame-limited aliases and
   place barriers between rebinds, matching the EOP+WAIT fences the passes emit).

### 3.5 Vulkan mapping for `native_pass.cpp`

- Per pass-target descriptor → an attachment description: colour = format from
  CB_COLORn_INFO (via the port's `AmdGpu::CB_COLOR*_INFO.NumberFormat`), depth = from
  DB_Z_INFO format class (`*puVar9 & 3` selects the PA_SU_POLY_OFFSET_DB_FMT_CNTL class →
  16/24/32-bit depth classes).
- `0x92140` = `beginRenderPass` attachment binding (the per-slot cache = "only rebind
  changed attachments"); load-op comes from the *clear* calls (`0x932a0`/`0x93960`'s
  fullscreen rect with blend-off), store-op = store (the guest always writes through).
- `0x85cbc0(cb, slot, NULL)` = attachment unbind (or a 0-masked render pass); the INFO=0
  write is the guest's way of dropping a slot from the target mask (along with
  `CB_TARGET_MASK` 0xf writes in `0x93960:38`).
- The `+0xe4` vs `+0x124` selector = two `VkImageView`s over one `VkImage` (e.g. main vs
  resolve slice) → the API should take **(image, view, format, loadOp, storeOp) per
  attachment plus a depth attachment**, exactly the sub-range descriptors; the viewport
  (`FUN_00091fc0(pass,0,0,w,h)`) is dynamic state.
- `0x86a1a0`'s trailing separate INFO packet = nothing on the Vulkan side (it is the
  format/aux dword of the same attachment).

## 4. Order of operations within one pass (bind vs RT set)

**RT set first, material bind inside the draw objects, always after the RT bind.** All
proven:

- `0x93960` (pass A): `0x93840` (range acquire) → **`0x85cbc0` RT0** (line 24) → blend/
  CB_TARGET_MASK/CB_COLOR_CONTROL/DB_DEPTH_CONTROL → viewport (`0x85de40`) → PA_SU_SC_MODE →
  prim reset (`cb+0xc548=0`) → `0x861600(cb+0x2e8, 0, 0)` (PS header reset, line 94) →
  `0x93690(param_1)` → EOP+WAIT → CB_COLOR_CONTROL mode 1. The fullscreen-rect draw object
  resolves the clear material, stages the VS/ES/GS record, sets prim 0x11, and only then
  calls `FUN_0085fc10(cb+0x2e8)` (`00093690.c:36`) followed by the draw
  (`0x86ac10(cb,3,0)`).
- `0x94600` (pass B): sets `pass+0x14=1`, stage-index table `PTR_DAT_01604428`, enable mask
  `+0x18 = 0x20` →
  `0x92bd0` (clear/quad emitter) → EOP+WAIT → EVENT → CB_COLOR_CONTROL 0 → DB_RENDER_CONTROL
  → DB_RENDER_OVERRIDE → viewport → **depth block `0x86a290` + DB_DEPTH_CLEAR=0.0** (line
  66) → depth-stencil state (`0x91e10`) + SC/bias (`0x91cd0`) → prim reset (`cb+0xc548=0`)
  → `0x861600(cb+0x2e8,0,0)` (line 89) → `DB_DEPTH_CLEAR=1.0` → `0x93690(param_1)` (line 91,
  the bind+draw inside) → restore DB_RENDER_CONTROL/CB_COLOR_CONTROL/DB_RENDER_OVERRIDE.
- Per-draw path (`0x11d3d0`): material resolve → `0xf8c90` (bind records into the pass) →
  `0x123cc0` (instance/index/stencil) → `0x95320` (root table) → `0xae150` (root bind) →
  `0x1222e0` → `0x128d20` → `0x94d00(pass, …)`: prim from `DAT_00f9f2e0[mode]` →
  `0x862510(material,0,0)` (start-vertex SGPRs) → **`0x85fc10(material)`** →
  `0x86b040(cb, …)` (DrawIndexOffset) → `0x862510(material,0,0)` again (restore base after
  the draw's implicit prim restart).
- The MRT runner (`0xc1860`/`0xcc9b0`/`0x118470`) pattern: `0x92140` (RT set) →
  `0x932a0` (clear) → per-group `0x11d3d0` (whose inner bind is §2) → EOP label →
  `0x92140` (next RT set). **`0x93960`/`0x94600` never call `0x92140`**; the RT set happens
  in the *runner* before the pass function is invoked (e.g. `0x118470:53-63` RT set from
  `param_3[3]+0x3a30` via `0x92140`, then `0x93960` twice for the HUD sub-passes).

So the Vulkan order for `native_pass.cpp` is: `vkCmdBeginRenderPass` (from the RT set /
target-manager descriptors) → barrier/state → per draw: `vkCmdBindPipeline` + descriptor
binds (material bind) → `vkCmdDraw*` — with the prim-reset block (`cb+0xc548=0` + VGT reset
+ `0x86d160`) as the pipeline-selection reset between pass phases.

## 5. Pass-context fields set at RT-bind time (offsets cross-checked)

- The **CB_TARGET_MASK cache is `+0x1ac`** (`0x93960:37-39` write, `0x91b50` read —
  pass-map §1.1 already had it right; the "+0xac" in the task brief is a typo for `+0x1ac`).
  The blend-word cache is `+0x8c` (`param_1[0x11]`, `0x93960:31-33`).
- `+0x11c` DB_DEPTH_CONTROL cache — written by `0x93960:49`, read by `0x91e10`.
- `+0x16/17/18` viewport cache — written by `0x93960:60-64` and `0x94600:55-59` (via
  `0x85de40`'s cache update), plus `+0xb4/+0xbc/+0xc4` second pair.
- `0x92140` itself writes: `+0x48+8i` (per-slot colour-target caches), `+0xa8` qword
  (`param_1[0x15]`, depth-target cache), `+0x10` (colour-dirty u32), `+0x11` bit0
  (depth-dirty), `+0x8` bits 3-4 (RT-config code).
- `+0x18` (dword) = the 6-bit stage-enable mask for `0xae150` (`0x11` after pass A's reset
  block, `0x20` after pass B's); `+0x20` (qword) = pointer to the static stage-index table
  (`PTR_DAT_01604408`/`PTR_DAT_01604428`) read by `0x937c0`, `0x94ab0`, `0xae150`.

## 6. Status of the master map's open items

1. `0x95320` return value — **closed** (§2.7): returns the bump-allocation pointer (the
   packed-table base, carried in `rax` across the copy loop); bytes dumped from the eboot.
2. `0x93840` (pass outer setup) — **decoded** (§4, first bullet): it is a `FUN_0086cf40`
   (PM4 opcode 0x58 ACQUIRE_MEM) driver computing the target byte range from the RT
   descriptor (`((w*0x40+0x40)*fmt) << swizzle) + 0xff >> 8`) and emitting a full-surface
   L2/K cache invalidate (`0x38/0x2000000/1` flags) before the pass writes the RT.
3. Camera vtable `+0x2670`, `DAT_00f9f2e0` contents, `cb+0xc548` kind writers beyond `=0`
   (now tracked as gap N2-5), `M+0x2288` writer, `0x90860`'s `S` — unchanged (outside this
   task's scope).
4. AI memory — the `mcp__ai-memory__*` MCP tools are not reachable in this session (server
   `http://127.0.0.1:49374` connection timed out twice; `ai-memory call memory_query`
   CLI fallback also failed), so prior-work grounding came from the on-disk docs only.

## 7. Gap rows added

Deliverable status: the doc is complete; the following open items were recorded in the
register (see `native-render-gaps.md` Phase 2 table).

See `native-render-gaps.md` Phase 2 table: **N2-5** (`material+0xc260` indirect writer =
the draw-kind source), **N2-6** (material staged-constant producers + layout-record u16
table semantics), **N2-7** (CB/DB descriptor patch helpers not bit-decoded). N2-2/N2-3 in
the register belong to parallel Phase 2.1 work (fullscreen pipeline resolution, guest image
base).

## 8. Evidence index

Primary decompiles (all read this session, under `out/re/decomp/`):
`0085/0085fc10.c`, `0085/0085cbc0.c`, `0085/0085f190.c`, `0085/0085f220.c` (head),
`0085/0085f410.c` (head), `0009/00092140.c`, `0009/00093960.c`, `0009/00094600.c`,
`0009/00093690.c`, `0009/000937c0.c`, `0009/00094ab0.c`, `0009/00094d00.c`, `0009/00093840.c`,
`0086/0086d4c0.c`, `0086/0086d5e0.c`, `0086/0086d550.c`, `0086/00862490.c`, `0086/0086c170.c`,
`0086/0086c050.c`, `0086/00861520.c`, `0086/00861260.c`, `0086/00860bb0.c`, `0086/00860c50.c`,
`0086/008612b0.c`, `0086/00861600.c`, `0086/008616f0.c`, `0086/0086c7d0.c`, `0086/008619a0.c`,
`0086/008611a0.c`, `0086/00862510.c`, `0086/008643f0.c`, `0086/0086a1a0.c`, `0086/0086cf40.c`,
`000a/000ae150.c`, `000f/000f8c90.c`, `000f/000f8990.c`, `0014/0014d7b0.c`,
`0012/00123810.c`, `0012/00123cc0.c`, `0001/00012820.c`, `0087/0087eda0.c`, `0087/0087f0d0.c`.
Raw eboot: `out/eboot.elf` (DAT_01290410 at file `0x1294410`; `0x95320` call at file
`0x99360`, epilogue at file `0x99400`). Port cross-checks: `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`
(`sceGnmSetVsShader:1825`, `SetPsShader350:1727`, `SetEsShader:1563`, `SetGsShader:1599`,
`SetHsShader:1627`, `SetLsShader:1657`, `SetCsShaderWithModifier:1409`),
`gpu/shadps4/video_core/amdgpu/pm4_cmds.h:184-206` (SET_DATA header), `liverpool.cpp:1307/1533/2360`
(SetShReg decode), `pm4_opcodes.h:55` (0x58 = AcquireMem).
