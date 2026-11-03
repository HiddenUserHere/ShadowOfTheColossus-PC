# 06 - Draw-time descriptors: who fills the user-data arrays, and when they are copied

Read-only static analysis of `out/eboot.elf` via Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`
and `tools/re/eboot_re.py` (callers, stage table bytes). No build, no game run, no edits outside this file.
Addresses are eboot VAs. Labels: **observed** (decomp line or byte, with address), **inferred**, **unknown**.

## 0. Frame of reference (read first)

Earlier notes wrote "CRenderContext+0x2e8" and "ctx+0x3090". The callers prove a different base:

- **observed**: `FUN_000917c0` (ctor, `0x917c0`) sets `*this = cursor` (a 0xcbb8-byte object, `FUN_000170d0`).
  It then writes `*(cursor + 0x2e8) = &LAB_000917a0` (`return 0`). Setter callers pass `*param_N + 0x2e8`,
  where `*param_N` is that cursor object (e.g. `FUN_000de9d0` line 33: `FUN_00861ce0(*param_2 + 0x2e8, ...)`).
  `FUN_00862690` calls `FUN_0085ed20(param_1 + 0x5d, ...)`, i.e. cursor + 0x2e8 (`0x5d` qwords).
- Therefore, in this document: **S = cursor + 0x2e8** (the "material state" block). Every "S+off" below is
  `cursor + 0x2e8 + off`. The brief's `ctx+0x3090` is **S+0x3090 = cursor+0x3378**. The stage-array
  copy in `0x85f410` reads `param_1 + 0x3090` with `param_1 = S`.
- The cursor object (`cursor`) is the PM4 command buffer (writer cursor at +0x10, end at +0x08, refill callback
  at +0x18). The wrappers `0x86d4c0`, `0x86d5e0`, `0x86c170` etc. take it as their first argument.

## 1. Answer

- **Setters (observed):** the engine calls per-stage setters `0x8619a0`, `0x861b40`, `0x861ce0`, `0x861e60`,
  `0x861fe0`, `0x862160`, `0x8622e0` (and `0x8611a0` for VS only) with `(S, stage, first_slot, count, data)`.
  Each copies the 16- or 32-byte data into the stage's dword array `S + 0x90 + stage*0x1800`, at the
  dword offset given by a slot map in the stage's layout object. The setter only sets a dirty byte.
- **Shader binds (observed):** `0x8616f0` (CS), `0x861600` (PS), `0x860c50` / `0x860ea0` / `0x861520` /
  `0x861730` (VS) store the shader record and layout pointer per stage and set dirty bits.
- **Per draw (observed):** `0x862c50` (draw flush) always calls `0x85f410(S)`. That function copies the dirty
  stage array (count from the layout) into a bump chunk (`0x8643f0`) and writes the chunk address into SGPR
  pairs via `0x86d4c0` (SET_SH_REG, 3 body dwords). Clean stages are not copied and not re-written.
- **Compute (observed):** `0x85f220(S)` is the CS equivalent (stage 0). Its callers include `0xde9d0` and
  `0xa8c50`, which call `0x86b9e0` (DispatchDirect) straight after.
- **Constant data:** per-draw records are bump-allocated in the cursor's own PM4 stream by `0x883f80`
  (`0x862c50` writes 0x54, 0x34 or 0x20 bytes at cursor+0x40, pointer at cursor+0xc8b8). The consumer of
  `cursor+0xc8b8` was **not found**. A software V# builder (`0x16eb90`) exists and is called from `0x117160`.
- **CTexture T#:** read from `CTexture+0xc4` (32 bytes) in `0x133550` and `0xa8c50`. It is copied, then
  two flag bit-fields are rewritten by `0x8829e0`, then the copy goes into the stage map. **observed**
- **Stability (inferred):** the T# words are stable per texture and bind site, except the base-address dword
  (rewritten on residency) and the flag bits. The copy itself is not conditional on content change: any
  setter call marks the stage dirty, so identical words are re-copied.

## 2. Stage numbering and layout objects

| Item | Value | Evidence |
|---|---|---|
| Stage 0 = CS, stage 1 = PS, stage 2 = VS | SH base table at VA `0x1290410` (file off `0x1294410`): `0x2e40, 0x2c0c, 0x2c4c, 0x2c8c, ...` | **observed** bytes; values match GFX7 `COMPUTE_USER_DATA_0`, `SPI_SHADER_USER_DATA_PS_0`, `SPI_SHADER_USER_DATA_VS_0`. Table read by `0x86d4c0` (`DAT_01290410[stage*2]`) and `0x86d5e0` |
| Dword array per stage | `S+0x90 + stage*0x1800`: CS `S+0x90`, PS `S+0x1890`, VS `S+0x3090`; 0x600 dwords each | **observed** `0x8619a0` (`param_1+0x90+(param_2*0x600)*4`), `0x85f220`, `0x85f410` |
| Layout pointer per stage | CS `S+0xc180`, PS `S+0xc188`, VS `S+0xc190`; mirror layout `S+0xc1b8` | **observed** `0x8616f0`, `0x861600`, `0x860c50` |
| Shader record per stage | CS `S+0xc0a0`, PS `S+0xc0a8`, VS `S+0xc0b0`; mirror `S+0xc0d8`; current material `S+0xc0b8` | **observed** `0x8616f0`, `0x861600`, `0x860c50`, `0x861880` |
| Cursor pointer used by writers | `S+0xc090` (zeroed in `0x85ed20`) | **observed** use; writer at run time **unknown** |
| Bump allocator state | `S+0x00` refill callback, `S+0x20` cur, `S+0x28` cur2, `S+0x30` end; ring of chunk starts `S+0x38 + i*8` and ends `S+0x58 + i*8`, count `S+0x80`, index `S+0x84` | **observed** `0x8643f0`, `0x8643b0` |
| Refill callback value | `S+0x00` = cursor `+0x2e8` = `&LAB_000917a0` = `return 0` | **observed** `0x917c0` line 56 and `0x917a0`. Consequence (NULL copy target on chunk exhaustion) **inferred** |
| Ring rotation | `0x8643b0` (per-frame, from `0x85f100`, called by `0x862900`) | **observed** call graph. Ring length per frame **unknown** |

### 2.1 Layout object fields (observed in `0x85f410`, VS block with layout `L = S+0xc190`)

| Field | Use |
|---|---|
| `L+0x00` (u16) | dword count copied from the stage array (`import_memcpy(chunk, S+0x3090, count*4)`) |
| `L+0x07` (u8) | SGPR slot (0..15, 0xff none) receiving the chunk base address |
| `L+0x08` lo / `L+0x09` | lo byte of u16 `L+0x08` = slot for `base + u16[L+0x16]*4`; `L+0x09` = slot for `base + u16[L+0x1a]*4`. If the lo byte is `0xff`, the hi byte (`L+0x09`) is used for the first write instead, and the second is skipped |
| `L+0x0a` lo / `L+0x0b` | slots for `base + u16[L+0x1c]*4` and `base + u16[L+0x1e]*4` |
| `L+0x0c` lo / `L+0x0d` | slot for `S+0x78` (8-byte pointer); `L+0x0d` slot gets an inline 1-dword write of `S+0xc0e8` (`0x86d5e0`) |
| `L+0x0e` | inline 1-dword write of `S+0xc108` (`0x86d5e0`) |
| `L+0x04` / `L+0x05` / `L+0x06` / `L+0x14` | slots for `S+0xc130` (8 bytes), `base + u16[L+0x18]*4`, `base + u16[L+0x20]*4`, `S+0xc5d0` (8 bytes) |
| `L+0x13` | returned to the draw (`*param_7`, `*param_6=1`); meaning **unknown** |
| `L+0x15` | `0x86d550` write, or `*param_8` when the caller supplies it; meaning **unknown** |
| `L+0x12` | mirror block only: returned as `*param_4`; meaning **unknown** |

The same field map is used for PS (`L = S+0xc188`, with `S+0xc0e4`, `S+0xc104`, `S+0xc0fc`, `S+0xc11c`
for the inline 1-dword writes; `0x86d4c0` stage 1). The mirror block (`L = S+0xc1b8`) reuses it with the
`0x8841d0` / `0x884240` / `0x884360` writers on cursor `S+0xc098`. **observed** (decomp of `0x85f410`).

### 2.2 Slot map inside the stage layout (observed in the setters)

Each setter reads a u16 table inside the layout at a fixed byte offset. Entry `E` for slot `i`:
`0xffff` = unused; `E < 0x8000` = dword offset `(E & 0x3fff)` into the stage array; `E & 0x8000` = direct
user-data write (`0x86d5e0`, SH register `E & 0x3fff`).

| Setter | Stage arg | Slot table at layout + | Entry size | Observed data (sample caller) |
|---|---|---|---|---|
| `0x8619a0` | any | `0x82` | 16 B | `0xde9d0`: `R[0x10]+0x68` (16 B); `0xa8c50`: 16 B built from the pointer returned by `0x8c0d0(obj+0xc8)` and `obj+0xd0` (the caller's 16 B is copied into that allocation) |
| `0x861b40` | any | `0xaa` | 16 B | not sampled |
| `0x861ce0` | any | `0x22` | 16 B | `0xde9d0`: `R+0x130` (count 1), then `R+0x130` + `R+0x2e8` (count 2) |
| `0x861e60` | any | `0x42` | 16 B | `0xde9d0`: `R+0x338` (count 1) |
| `0x861fe0` | any | `0x22` | 32 B (T#) | `0x133550` (slot 7): `CTexture+0xc4..0xdc`, passed through `0x8829e0(…, 0x10)` |
| `0x862160` | any | `0x42` | 32 B (T#) | `0xa8c50`: `CTexture+0xc4..0xdc` through `0x8829e0(…, 0x6d)` |
| `0x8622e0` | any | `0x62` | 16 B (inferred: direct path size 4) | not sampled |
| `0x862490` | any | `0x10` (lo byte = slot, hi byte = count) | direct | not an S-array setter: writes the block straight to PM4 via `0x86d5e0`; callers include `0xaf610`, `0xae150`, `0xb9710` |
| `0x8611a0` | VS only | `0xca` (u16 per index, `L+0xca+i*2`) | 16 B | **no caller** found in decomp or in the call index (see 7) |

Data-source notes:
- Table `0x22` is read by 16 B and 32 B setters with overlapping slot numbers (0..3 with `0x861ce0`, 5..7
  with `0x861fe0`). The element size is therefore chosen by the caller, not by the table. **observed**
- `0x861ce0`, `0x861e60`, `0x8619a0` in `0xde9d0` are called in a sequence with stage `R[4]+0x14` (1 or 2),
  then `0x85f220` (CS flush) and `0x86b9e0` (dispatch). **observed**

### 2.3 Dirty bits (all observed)

| Byte | Set by | Cleared by | Meaning |
|---|---|---|---|
| `S+0xc250 + stage` (`c250` CS, `c251` PS, `c252` VS) | every setter above (unconditional `= 1`) | `0x85f410` end (`c251`, `c252`, plus `c24f, c249, c24a, c257, c259, c25a`, the shader and mirror flags), `0x85f220` (`c248`, `c250`) | stage user data dirty |
| `S+0xc257` | setters on the mirror branch (`stage == 2` and `S+0xc613`), and `0x861880` | `0x85f410` end | mirror stream dirty; forces VS copy |
| `S+0xc613` | VS bind: `L_VS+0x14 != 0xff` (`0x860c50`, `0x860ea0`, `0x861520`) | not cleared | mirror enabled (semantics **unknown**) |
| `S+0xc248` / `c249` / `c24a` | shader setter when the new shader pointer differs from `S+0xc0a0/a8/b0` | `0x85f410`/`0x85f220` end | shader changed |
| `S+0xc25a` / `c259` / `c25b` | shader setter when the new 3-dword variant hash equals the old one and state bit `c260 & 4 / 2 / 8` is set | same | choose `UpdateVs`/`UpdatePs` instead of `SetVs`/`SetPs` (**observed** choice) |
| `S+0xc24f` | `0x861730` / `0x861880` when mirror shader pointer changes | `0x85f410` end | mirror shader changed |

## 3. Per-draw flow (observed unless marked)

1. **Bind-time (setters, any time before the draw).** Engine code (e.g. `0xa8c50`, `0x133550`, `0xde9d0`)
   builds a 16 or 32-byte blob, calls a setter, which writes `S+0x90 + stage*0x1800 + off*4` and sets the
   dirty byte. Nothing is written to the GPU here (except the direct-entry branch, which writes PM4 at once
   via `0x86d5e0`).
2. **Shader bind (any time before the draw).** `0x860c50` / `0x860ea0` (VS), `0x861600` (PS), `0x8616f0` (CS)
   store the shader record and layout pointer. VS chooses a layout inside `S` (`S+0xc340`, `S+0xc268`,
   `S+0xc418`, `S+0xc4f0`) from a variant byte in the shader record (`rec+0x3` index into `rec+0x38`). **observed**
3. **Draw flush `0x862c50(cursor, draw_desc, ...)`** (called from an unresolved site; caller not found):
   - Writes a draw-state record into the cursor's embedded-data pool (`0x883f80(cursor+0x40, 0x54|0x34|0x20, 2)`),
     copying `cursor+0xe0..0x11c` and `draw_desc+0x4..0x18`, and stores it at `cursor+0xc8b8` (size at `+0xc8c4`).
     **observed**; the reader of `cursor+0xc8b8` is **unknown**.
   - Calls `0x85f410(S, &out...)`, then `0x86b6f0` if the output prim value changed, then the draw emitter
     (not read in full).
4. **`0x85f410(S, ...)` order (observed):**
   a. mirror shader (`c24f`): `0x883e60`, `0x883730` on cursor `S+0xc098`.
   b. VS shader (`c24a`): `0x86c170` (SetVs) or `0x86c250` (UpdateVs) with `rec(S+0xc0b0)+8`, then `0x86a7f0`
      (address `rec[+8] << 8`, size `rec[+0] & 0x7fffff`).
   c. PS shader (`c249`): `0x86c050` (SetPs) or `0x86c110` (UpdatePs), same address write for `rec(S+0xc0a8)`.
   d. VS user data (`c252 | c257`): `chunk = 0x8643f0(S, count)`, `memcpy(chunk, S+0x3090, count*4)`, then the
      SGPR writes in 2.1 (`0x86d4c0`, `0x86d5e0`, `0x8841d0`... for the mirror).
   e. PS user data (`c251`): same with `S+0x1890` and layout `S+0xc188` (no mirror copy).
   f. All dirty bytes cleared.
5. **Compute (observed):** `0x85f220(S)`: if `c248` then `0x86c7d0` (SetCsWithModifier) with `S+0xc0a0+8`;
   if `c250` then copy `S+0x90` (count from `S+0xc180`) and the same SGPR pattern with `0x86d4c0(stage 0)`.

Reuse rule (observed): a stage is copied and its SGPR pairs are re-written only on a draw where its dirty byte
is set. On other draws, the previous SGPR values stay in effect. Inferred: this relies on the GPU keeping the
last user-data values within the command buffer, and on the chunk not being recycled while in use.

## 4. Constant buffers and per-draw data

| Question | Finding | Label |
|---|---|---|
| Per-draw allocator | `0x883f80(obj, bytes, align_log2)`: bump in the cursor's own PM4 stream. Writes a header `0xC0001000` (NOP-type) and tag `0x68750000 | align<<12`, returns the payload. The `obj` is `cursor+0x40` (draw record), `obj+0x10` (in `0x16eb90`), or a caller-given object | **observed** |
| Generic copy helper | `0x85e3c0(obj, src, dwords, align)`: `0x883f80` + `memcpy` | **observed** |
| Ring vs bump | Stage arrays use the per-frame chunk ring (`S+0x38/0x58`, `0x8643b0`). Draw-state and embedded constants use the PM4 stream chunk (`0x883f80`) | **observed** allocators; the per-frame relation of the two is **inferred** |
| Constant V# built in software | `0x16eb90` (422 bytes) writes a descriptor via `0x865410(buf, ptr, 0x4404, size)`, a flag word via `0x865360(buf, 0x10 or 0x6d)` (same pattern as `0x8829e0`), stores it in a 3-entry table at `obj+0x50` (`0x85e8e0`, `0x85e9a0`, `0x85e820`), and writes an 8-byte payload (count, stride) with `0x883f80(obj+0x10, 8, 2)`. Called from `0x117160` (CComputeContext per the earlier doc) and `0x191690` | **observed** calls; the name and bit meaning of `0x4404` and the `obj+0x50` table are **inferred** |
| Consumer of `cursor+0xc8b8` (draw record) | No reader found in decomp (only the two writes in `0x862c50`) | **unknown** |
| Persistent constant-buffer object | Not found. Constants reach the GPU through user data written in 3, plus the embedded V# path above | **inferred** |

## 5. CTexture T# path (observed)

Caller `0x133550` (slot 7, stage `R[4]+0x10`) and `0xa8c50` (stage `R[4]+0x14`):

| Step | Address | Effect |
|---|---|---|
| Copy T# | `0x133550` lines 30-35, `0xa8c50` lines 40-43 | `local = *(CTexture+0xc4), +0xcc, +0xd4, +0xdc` (32 bytes, the T#) |
| Flag rewrite | `0x8829e0(&T#, flags)` with flags `0x10` in `0x133550`, `0x6d` in `0xa8c50` | Exact bit moves (observed): dword1 bits 6-7 = `flags[1:0]`; dword1 bits 30-31 = `flags[3:2]`; dword3 bit 26 = `flags[4]`; the old bits at those positions are cleared, all other bits kept |
| Store | `0x861fe0` (T# at slot 7) or `0x862160` (T# after `0xa8c50` flags) | dword copy into `S+0x90+stage*0x1800+off*4` |
| Base address | `0x881e60` writes the base into T# dword0 on residency (from `0x12820`/`0x12950` placement, `0x96680` build) | **observed** (from `02-resource-objects.md`, not re-read) |

Field table of the T# for one bind (observed bit placement; semantics **unknown** except dword0 base):

| Word | Source | Changes |
|---|---|---|
| dword0 | `CTexture+0xc4` (base>>8 from residency `0x881e60`) | residency only |
| dword1 | `CTexture+0xc8`, with bits 6-7 and 30-31 set per bind by `0x8829e0` | per bind site |
| dwords 2..7 | `CTexture+0xcc..0xdc` | constant per texture |
| dword3 bit 26 | `0x8829e0` per bind | per bind site |

## 6. Are copied tables stable across frames? (inferred)

- **T# from CTexture:** same words each frame for a given texture and bind site, except dword0 after a
  residency change (`0x881e60`). Stable, so a per-draw copy re-writes identical data.
- **16-byte blobs from render state (`R+0x130`, `R+0x2e8`, `R+0x338`, `R[0x10]+0x68`, `obj+0xc8`):** these
  come from per-view and per-pass structs. Whether they change per frame is **unknown** (their writers were not
  read). Expect camera-like data to change, material-like data to stay.
- **Content vs copy:** the copy is not gated on content. `0x8619a0`-family setters always set the dirty byte, so
  a stage is re-copied whenever any setter touched it, even with identical words. The bump address changes
  on each copy (chunk ring), so the SGPR values change on each copy.
- **Only the bump address changes** for an unchanged stage that is not re-dirtied. Inferred: unchanged stages do
  not change their SGPRs at all (see Reuse rule in 3).

## 7. Open items and unknowns

1. **Owner of `0x8611a0` (VS 16-byte map at layout `+0xca`).** Not called from any decompiled function, and
   absent from the call index. Probably reached through a table, a tail call, or a call Ghidra did not resolve.
   **unknown**. (The earlier note "caller `0xca8e5`" was not confirmed: in `out/re/decomp`, the only files
   that mention `0x8611a0` are its own file and the caller list header of `0x86d5e0`.)
2. **Caller of `0x862c50`** (draw flush): index shows no direct caller. **unknown**.
3. **Writer of `cursor+0xc090` (S+0xc090)** used as the cursor for all writers. Zeroed at init (`0x85ed20`);
   the runtime writer was not found. **unknown**.
4. **Semantics of the mirror stream** (cursor `S+0xc098`, layout `S+0xc1b8`, enabled by `L_VS+0x14`).
   **unknown**.
5. **Who builds the layout tables** (`L` objects at `S+0xc340`, `S+0xc268`, ... and the caller-supplied ones
   in `0x861520`/`0x861600`/`0x8616f0`). The layout content for a given shader is **unknown**. The record
   format `rec+0x38 + variant*4` is **observed** but its fields are only partly read.
6. **Ring length and refill**: `S+0x80` count is set elsewhere. If a chunk fills, the callback `0x917a0` returns 0
   and `0x8643f0` returns NULL, which would crash the copy. **inferred**; not checked against runtime.
7. **`0x861730` label**: Ghidra reports size 1 (a stub label), but the body is read and matches the VS bind
   behaviour. Treat as a real function with a bad size. **observed**.
8. **`0x8622e0` and `0x861b40` bodies** were only partly read (the direct branch and first lines).
9. **`0x8829e0` bit semantics** (what dword1 bits 6-7, 30-31 and dword3 bit 26 mean in hardware) are
   **unknown**. Only the bit moves are **observed**.
10. **Draw emitter after `0x85f410`** in `0x862c50` (the call to `0x86b3d0` / `0x86b9e0` family) was not read in full.

## 8. Corrections to earlier notes

- The base of the state block is the cursor object (`*CRenderContext + 0x2e8`), not `CRenderContext + 0x2e8`.
  The brief's `ctx+0x3090` is `S+0x3090` = `cursor+0x3378`.
- The setter family is wider than the single `0x8611a0` path: seven stage setters (`0x8619a0`, `0x861b40`,
  `0x861ce0`, `0x861e60`, `0x861fe0`, `0x862160`, `0x8622e0`) feed the same stage arrays, and `0x862490`
  writes direct entries.
- The stage index is 0 = CS, 1 = PS, 2 = VS (from the SH base table bytes).
