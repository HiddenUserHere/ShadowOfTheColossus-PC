# Render-state objects: where the engine gets them (RE, topic R2)

Scope: blend, depth/stencil, raster, colour-target setup, primitive type. Evidence is the Ghidra decompile in
`out/re/decomp/<addr>>>16 as 4 hex>/<addr>.c` (folder `0008/` holds `0x8f1d0`, folder `0086/` holds `0x86a1a0`, etc.),
the first line of each file lists its callers. Addresses are eboot offsets.

## Summary

1. The eboot does **not** call `sceGnmSet*` state functions. There is no `sceGnm` import for blend, depth, raster or
   RT state (`src/import_names.inc` has only `sceGnmSetCsShader`, `SetPsShader`, `SetVsShader`, `SetVgtControl`,
   `SetResourceUserData`, `SetWaveLimitMultipliers` and the submit/flip family). The state writers are Gnmx-style
   inline code linked into the eboot. They build PM4 packets directly into a command-buffer cursor.
2. Those writers are one-register setters (about 80 functions, `0x869350`..`0x86fd60`). Each one writes a 3-dword PM4
   packet (`header, offset, value`) at `[cb+0x10]`, after a free-space check on `[cb+8]` with a flush callback at
   `[cb+0x18]`. The packet is `0xC0016900` for SET_CONTEXT_REG, `0xC0017900` for SET_UCONFIG_REG.
3. There are three layers of state in the engine:
   a. **Init-time fixed state objects.** A small fixed set, built once. Blend words, depth/stencil words, and
      depth-related blocks are written to globals `DAT_03001618`..`DAT_03001658` (`0x5d5410`) and to a context object
      (`0x1671d0`).
   b. **Per-pass apply with a shadow cache.** `0x93960` (1029 bytes) writes the blend, depth, colour-control, target
      mask, primitive and RT registers for a pass. Each write is skipped when the cached value is unchanged and no
      dirty bit is set.
   c. **Per-draw source data.** The RT descriptor comes from the pass object (`pass+0xe4`). The primitive type comes
      from a table indexed by a draw-type enum (`DAT_00f9f2e0`).
4. The full pipeline-key space is **not enumerable from code alone**. The fixed layer is a finite list of a few dozen
   words. The per-pass layer is literal constants inside `0x93960` and its siblings, plus values read from the pass
   and material data. That data is loaded from `.cmat` / `.cslt`, and I did not trace that path (see open items).

## 1. Register writes that reach the pipeline key

Register numbers are dword offsets from the context base `0xA000` for SET_CONTEXT_REG (`0x0C0016900` header), and
from `0xC000` for SET_UCONFIG_REG (`0xC0017900`). Byte address = dword x 4. The port's `gpu/shadps4/video_core/amdgpu/regs.h`
uses the same absolute dword indices (`ContextRegWordOffset = 0xA000`, `UconfigRegWordOffset = 0xC000`).

Names are from the standard GCN/Sea-Islands register map. Rows marked "verified" were checked against the port's
bitfield structs (`regs_color.h`, `regs_depth.h`, `regs_vertex.h`). Others are inferred from the register number and
the bit masks the engine applies.

| Register | Ctx idx (dword) | Byte addr | Engine writer (eboot) | Masking / fields seen | Confidence |
|---|---|---|---|---|---|
| CB_BLEND0_CONTROL[0..7] | 0x1E0 + i | 0x28780 | `0x869a50(cb, i, word)` | `word & 0xFFFF1FFF`; color src bits 0-4, comb 5-7, dst 8-12, alpha src 16-20, alpha comb 21-23, alpha dst 24-28, separate-alpha bit 29, enable bit 30 (from `0x87c3b0`/`0x87c3d0`/`0x87c390`/`0x87c400`) | verified (layout vs `regs_color.h` BlendControl) |
| CB_BLEND_RED..ALPHA | 0x105..0x108 | 0x28414 | `0x869ac0(cb, r, g, b, a)` (4 dwords, `0xC0046900`) | raw | inferred |
| CB_COLOR_CONTROL | 0x202 | 0x28808 | `0x869ce0(cb, mode, rop3)` | `(mode & 7) << 4 \| (rop3 & 0xFF) << 16` | verified (ColorControl: mode 4-6, rop3 16-23) |
| CB_TARGET_MASK | 0x8E | 0x28238 | `0x869990(cb, value)` | raw (value 0xF in the per-pass path) | inferred |
| CB_COLORn_BASE (and the 14 dwords after it) | 0x318 + 0xF * n | 0x28C60 + 0x3C * n | `0x86a1a0(cb, n, desc)` (`0xC00E6900`, 15 dwords) | desc = 8 qwords (64 bytes) read from the RT descriptor | inferred from stride; INFO is at 0x31C |
| CB_COLORn_INFO | 0x31C + 0xF * n | 0x28C70 + ... | written inside `0x86a1a0`, sometimes zeroed (`0xC0016900`, value 0) | format/number/swap bits come from the descriptor | inferred |
| DB_DEPTH_CONTROL | 0x200 | 0x28800 | `0x869d60(cb, word)` masked `0xC07007FF`; zero reset in `0x869e80` | stencil_enable bit 0, depth_enable bit 1, depth_write bit 2, depth_func bits 4-6, backface bit 7, stencil func bits | verified (DepthControl in `regs_depth.h`) |
| DB_EQAA | 0x201 | 0x28804 | `0x869e20(cb, word)`, mask `0xF3F7777` | raw | inferred |
| DB_STENCILREFMASK / _BF | 0x10C / 0x10D | 0x28430 / 0x28434 | `0x869b50` / `0x869bb0` (front and back pairs, `0xC0026900`) | raw | inferred |
| DB_STENCIL_CLEAR | 0x00A | 0x28028 | `0x869fb0(cb, v & 0xFF)` | 8-bit | inferred |
| DB_DEPTH_CLEAR | 0x00B | 0x2802C | `0x869f50(cb, float bits)` | raw | inferred |
| DB_RENDER_CONTROL | 0x000 | 0x28000 | `0x86dd20(cb, v & 0x1FFF)` | | verified (first field of `DepthRenderControl` in `regs.h`) |
| DB_RENDER_OVERRIDE | 0x003 | 0x2800C | `0x86be10(cb, v & 0xFC1F1FFF)` | | inferred |
| DB_RENDER_OVERRIDE2 | 0x004 | 0x28010 | `0x86be70(cb, v & 0x60)` | | inferred |
| PA_CL_CLIP_CNTL | 0x204 | 0x28810 | `0x86ded0(cb, word)`, mask `0xF7FE03F` | clip_disable and related bits | inferred |
| PA_SU_SC_MODE_CNTL | 0x205 | 0x28814 | `0x86e1a0(cb, word)`, mask `0x393FFF` | cull / front-face / polygon mode bits | inferred (cull and polygon-mode fields; the port's `PolygonControl` should confirm) |
| PA_CL_VTE_CNTL | 0x206 | 0x28818 | `0x86db40(cb, v & 0xF3F)` | | inferred |
| PA_SC_WINDOW_SCISSOR_TL | 0x080 | 0x28200 | `0x86a120(cb, x, y)` | 16-bit each | inferred |
| VGT_PRIMITIVE_TYPE | UC 0x242 | 0x30908 | `0x86a900(cb, prim)` (`0xC0017900`, value `prim & 0x3F`) | bit 28 (`0x10000000`) is OR-ed in when `FUN_00882ec0()` returns 1 (meaning unknown) | verified index; name inferred |
| VGT_INDEX_TYPE | UC 0x248 | 0x30920 | `0x86fc30(cb, v)` | raw | inferred |

Generic setters (all in the same family, used by the rows above and by other code):

- `0x869350(cb, addr, value)`: one SET_CONTEXT_REG with `addr - 0xA000` (the `param_2 + -0xa000` pattern).
- `0x869370(cb, addr, value, shift)`: the same with a 4-bit field at bit 28.
- `0x8694E0(cb, addr, data, count)`: block of `count` context regs (`0xC0006900 | count<<16`, data is memcpy'd).
- `0x86D080`, `0x86E3A0`: write through lookup tables `DAT_0128FED0`, `DAT_0128FEE0`, indexed by an enum. These
  map a state slot to a register index. Not yet decoded.

### Per-register ID table for the port

The port can use these writer functions as the recognition points. For a reverse mapping at run time, the port can
decode the PM4 stream it already parses (`liverpool.cpp`), so it does not need the eboot. The eboot addresses above
matter only when the port wants to pre-build keys before the first draw.

## 2. Engine-side state objects

### 2a. Fixed init-time objects (finite, built once)

- `0x5d5410` (420 bytes; callers `0x5c64c0`, `0x5b9b40`, `0x5b9b60`). It writes a fixed set of globals:
  - `DAT_030015C8`, `DAT_030015F0`: `FUN_0008be60(&g, 0x1c, 0x50, 1, 2, 0, 0)`, twice (type not identified).
  - `DAT_03001618` / `DAT_03001620` (16 bytes, from `FUN_0008f520` = depth/stencil block) and `DAT_03001628`
    (from `FUN_0008f1d0` = blend word and write mask).
  - Same pattern at `DAT_03001630/38/40`, `DAT_03001648/50/58`.
  The `FUN_0008f520` arguments are constant (`0,1,7,0,0,0,7,0,0,0,0xFF,0xFF`). These are fixed states, not
  data-driven.
- `0x1671d0` (7768 bytes; caller `0x166f50` <- `0x8f6f0` <- `0x2569d0` <- `0x533100`). It fills a context object with
  about 20 fixed packed states:
  - 8-byte words at object `+0x28 .. +0x90` from `FUN_0008f1d0` / `FUN_0008f280`, with literal arguments. Each word is
    `(control_word << 0) | (writemask << 32)`. The control word is built by the bitfield helpers in section 2c.
  - `+0x98 .. +0xB8` and `+0xB0 .. +0xC0` from `FUN_0008f330` (two 3-entry and 7-entry tables, literal arrays on the
    stack).
  - `+0xC8 .. +0x118` from `FUN_0008f410` (floats, `0xBF800000 = -1.0`, raster-like values; the exact meaning is not
    decoded).
  The literal arguments in these calls are the complete list of fixed state words. Count them from the decomp when
  needed (about 20 calls in `0x1671d0` lines 154-230, plus the `0x8f410` calls later).

### 2b. Per-pass apply with shadow cache (`0x93960`)

`0x93960` (1029 bytes; callers include `0xcaed0` = `RenderView::MainCamera`, `0x118470` = `Frame::InsertWaitFlipDone`
path, `0x b9710`, `0x23d750`, and about 10 more) applies one pass state. Its structure:

1. It checks a pass flag byte at `pass+0x62` (`(b | 2) == 3`) and `pass+0xF5 & 0x20`.
2. It sets `FUN_0085cbc0(cb, 0, pass+0xE4)` only when the RT descriptor pointer changes (cache at `param_1[9]`).
3. It builds packed words on the stack with bitfield helpers, then writes each one only when its cached copy differs
   or a dirty bit is set. Cache slots: `+0x11` (blend0), `+0x1AC` (target mask = 0xF), `+0x11C` (depth), `+0x23`
   (`PA_SU_SC_MODE_CNTL`), `+0x1CC` (primitive type). Dirty bits are in `param_1[1]` and `param_1[2]`.
4. Constants in this path (not data-driven, from the decomp):
   - blend word: `0` (blend disabled) through `FUN_0087c390(…,0)`;
   - target mask: `0xF`;
   - colour control: `FUN_00869ce0(cb, 2, 0xCC)` (mode 2, rop3 0xCC = copy);
   - depth word: ZFUNC `7` (ALWAYS), Z_WRITE off, Z_ENABLE on (`0x87cba0(…,0,7)`, `0x87cc00(…,1)`);
   - PA_SU_SC_MODE_CNTL seed `0x240` (`0x87c4a0`), then fields from `0x87c4c0`, `0x87c4b0`, `0x87c4d0(…,2,2)`.
   - viewport seed `0.5` for depth range (`0x85de40`, `0x3F000000`).

Other callers, which use the same writers:

- `0x91b50`, `0x919e0`, `0x91cd0`, `0x91e10`, `0x93ef0`, `0x94600`, `0x93690`, `0x94bf0`, `0x94d00`, `0x94dd0`: the same
  pattern with different pass constants (primitive type, depth, blend). These are the "pass family" callers.
- `0x90470` (`PerFrameRenderContexts::RequestNewRenderContext`) calls `0x86ded0` (PA_CL_CLIP_CNTL).
- `0x85de40` (called by `0x93960`, `0x5b9b...` family) writes viewport and scissor through `0x86d7b0`, `0x86db40`,
  `0x86dd20`.

### 2c. Packed-word builders (the bitfield helpers)

These are the functions the packed words come from. Their bit layouts match the port's headers, so they are useful to
the port as a decoder.

| Function | Layout |
|---|---|
| `0x87c380(p)` | `*p = 0` |
| `0x87c390(p, b)` | bit 30 = b (blend ENABLE) |
| `0x87c400(p, b)` | bit 29 = b (SEPARATE_ALPHA_BLEND) |
| `0x87c3b0(p, comb, src, dst)` | color: src 0-4, comb 5-7, dst 8-12 |
| `0x87c3d0(p, comb, src, dst)` | alpha: src 16-20, comb 21-23, dst 24-28 |
| `0x87cb90(p)` | `*p = 0` (depth) |
| `0x87cba0(p, zwrite, zfunc)` | bit 2 = zwrite; bits 4-6 = zfunc |
| `0x87cc00(p, zen)` | bit 1 = depth enable |
| `0x87c4a0(p)` | `*p = 0x240` (PA_SU_SC_MODE_CNTL seed, inferred) |
| `0x87c4c0(p, b)` | bit 2 = b |
| `0x87c4b0(p, b)` | (not decoded) |
| `0x87c4d0(p, a, b)` | bits 5-7 = a, bits 8-10 = b, + 8 when not (2,2) |

`0x8f1d0(ctx, src, comb, dst, writemask)` returns the 64-bit word: `(writemask << 32) | control`. The enable bit is
`(src | dst) != 0 || comb != 1`.

## 3. Render-target setup per pass

- `0x85cbc0(cb, index, desc)` (269 bytes; callers `0x92140`, `0x93960`) takes a pointer to an 8-qword colour-target
  descriptor. A null descriptor clears the target (`0x86a1a0(cb, index, 0)`). Otherwise it writes the CB_COLORn block at
  `0xA318 + 0xF * index` (14 values from the descriptor).
- The descriptor bits used by `0x85cbc0`: `(desc[2] & 0x6000) == 0x2000` checks a tile/format field, `0x87f0d0` and
  `0x87eda0` decide whether the target is used (`0x87eda0(desc) != 0` picks 0x12 dwords instead of 3 in the size
  check). Format enum decode not done.
- Depth target: the `DB_Z_INFO` / `DB_Z_READ_BASE` / `DB_Z_WRITE_BASE` writers were not isolated in this pass.
- Pass list: `user/shader_predict/pass_layouts.txt` (68 pixel layouts) is a port-side artifact. The eboot has no
  fixed pass table I could identify. The passes come from the `0x93960` family callers, each with its own literal
  constants. Counting the distinct constant sets in those callers would give the enumerable pass list. Not done.

## 4. Primitive type

- Table `DAT_00F9F2E0` (eboot data, index = draw-type enum): used by `0x94bf0`, `0x94d00`, `0x94dd0`. The table
  itself was not dumped.
- Literal primitive values written by `0x86a900`:
  - `0x11` (17 = `RectList` in `regs_vertex.h`): `0x93690`, `0x91b50` family, for full-screen passes.
  - `9` (`PatchPrimitive`) for the tessellation path in `0x94bf0`.
  - Other values come from `DAT_00F9F2E0`.
- The write is skipped when the cached value matches (`+0x1CC`, dirty bit `param_1[1] & 1`).

## 5. Is the key space enumerable at boot?

- **Fixed part (yes).** The init objects in 2a are constant. Their words come from literal arguments, so the port can
  decode them from the eboot and pre-build each one once. That is about 20 blend/depth/raster words plus a few
  dozen depth/stencil blocks.
- **Per-pass part (mostly yes, from code).** The `0x93960` family writes literal constants, so the distinct pass
  states can be listed by reading each caller's constants. The count is the number of distinct (blend word,
  depth word, PA_SU word, colour control, target mask, primitive) tuples in those callers. Not counted yet.
- **Per-material part (not enumerable from code).** Material blend and raster values would come from `.cmat` data.
  I did not trace how `.cmat` is parsed, so this part's size is unknown. It can be enumerated by reading the material
  files at run time, which is the port's existing route for shader resources.

## 6. Run-time use for the port

- Boot: the port can read the fixed words from the eboot image (`out/boot-linked.bin`) at the globals
  `DAT_03001618..DAT_03001658` and the object built by `0x1671d0`. That gives the fixed blend, depth and raster keys
  before the first frame. Read them after `0x5d5410` / `0x1671d0` have run, or emulate their literal arguments.
- Area streaming: the material blocks (`.cmat`) are the source of the rest. When a `.cmat` loads (RES_LOADED hook in
  `gpu/sotc/probe_csdr.cpp`), read its blend, depth and raster fields and add them to the key set.
- Pass family: the constant sets in section 2b are known at boot (they are in the eboot code). The port can pre-build
  one pipeline per distinct (pass, primitive, target format) tuple.
- Shadow cache: the `0x93960` cache slots (`+0x11`, `+0x11C`, `+0x23`, `+0x1CC`) show which register values the engine
  itself considers unique. The port can key on the same values and skip re-deriving them.

## Open items and limits

- The `0x8F330` and `0x8F410` helper arguments were not decoded. Their output words are stored at `0x1671d0` `+0x98..
  +0x118`. The meaning of the float values (`0xBF800000`, etc.) is not known.
- `FUN_0008be60`, `FUN_0086d080`, `FUN_0086e3a0` (enum-to-register tables `DAT_0128FED0`, `DAT_0128FEE0`, and the
  UCONFIG table `DAT_01290050`..) were not decoded. They look like a state-slot-to-register map.
- `DB_Z_INFO`, `DB_Z_READ_BASE` / `DB_Z_WRITE_BASE` writers were not isolated. `CB_COLOR_CONTROL`'s `0xCC` (copy
  ROP3) is from the decomp constant and matches the port's `ColorControl` bit layout.
- The `0x10000000` flag in `VGT_PRIMITIVE_TYPE` (`0x882ec0`) is unexplained.
- I did not check the material parser (`.cmat`), so the per-material part of the key space is open.
Scratch files were not written. Nothing under `src/` or `gpu/` was modified.
