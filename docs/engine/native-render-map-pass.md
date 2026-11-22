# Structure map: PASS / DRAW-OBJECT / MATERIAL-BIND / RENDER-TARGET of the SOTC RenderJob

All addresses are EU 1.01 eboot VAs (CUSA08809). Decompiles are at `out/re/decomp/<hi16>/<addr>.c`; the first comment line of each file carries the size and the caller list. Tags: **proven** = read in the named decompile line(s); **inferred** = reasoning from proven facts; **unknown** = not established.

## 0. Provenance / conventions

- The pass context (`param_1` of 0x93960 / 0x94600 / 0x92140, and the `*param_1` of every draw object) is the **CRenderContext**. `*param_1` (qword at +0) is the **device/GNM cursor object** (`cb`), 0xcbb8 bytes, per `W1-14-submit.md` §1 (proven). All `FUN_0086xxxx` "state" writers take that `cb` and build PM4 into `cb+0x10`.
- GNM cursor contract (proven, `W1-14-submit.md` §2.1; re-read in every `FUN_0086xxxx` here): `cb+0x08` end, `cb+0x10` write cursor, `cb+0x18` reserve callback, `cb+0x20` callback arg, `cb+0x28` bit0 flag OR-ed into draw/dispatch initiators. Every writer checks `(end-cursor)/4 < N` and calls the callback before writing.
- The **material block** passed to 0x85fc10 is `cb+0x2e8` (`0x93960:36`, `0x94d00:25`, `0xdb620:15`), i.e. it is `ctx`+0x2e8, not the pass context. 0x85fc10's `param_1` = material block base.
- PM4 headers below are the literal dwords the writers store (`FUN_0086xxxx` proven).

## 1. Cross-cutting structures

### 1.1 Pass context (`param_1` of 0x93960/0x94600/0x92140; `*param_1` = cb)

| Offset | Field | Evidence |
|---|---|---|
| `+0x00` | `cb` (device/CRenderContext cursor) | `0x93960:21,24` (`*param_1`); `0x94ec0:13` |
| `+0x08` | qword dirty/force word; bit0 forces all state writes | `0x93960:22,31,37`; `0x94ec0:12` |
| `+0x10` | uint colour-dirty bitmask (8 bits, one per RT) | `0x92140:151,161,184` |
| `+0x11` | byte depth-dirty bit (bit0) | `0x92140:177`; `0x94600:64,70` |
| `+0x0c` | byte blend-dirty bit0 | `0x93960:34`; `0x91b50:14` |
| `+0x0d` | byte state-dirty bits (bit1 depth, bit2 scissor, bit3 stencil-ref, bit0/… ) | `0x93960:40,50,76`; `0x91e10:18,25,43`; `0x91cd0:16,29,46` |
| `+0x14` | dword: draw-kind/prim mode byte written to cb+0xc548 path (0 or 1) | `0x93960:90`; `0x94600:27`; read `0x10add0:16`, `0xdb620:12` |
| `+0x18` | qword: stage-descriptor pointer (PTR_DAT_01604408 / 01604428) | `0x93960:91`; `0x94600:28` |
| `+0x20` | qword: stage-descriptor; dword +0 = VS-stage index, +0x10 = PS-stage index | `0x93690:14`; `0x937c0:8`; `0x94ab0:17`; `0x92140` uses param_2[8] as lVar1 |
| `+0x23` | cached PA_SU_SC_MODE_CNTL (SC mode) | `0x93960:73`; `0x91cd0:13` |
| `+0x24` | cached DB_STENCIL_CONTROL | `0x91e10:22` |
| `+0x36/+0x37`, `+0x1b4/+0x1bc` | cached depth-bias (poly offset) floats | `0x91cd0:21,27,35,44` |
| `+0x39` | cached INDEX_TYPE | `0x93690:31`; `0x94ec0:21`; `0x94c60:27` |
| `+0x3b + i` | per-stage cached shader-record pointer (i = stage descriptor dword) | `0x93690:15`; `0x937c0:9`; `0x92140:153`; `0xae150:13` |
| `+0x11` (qword index 0x11 = byte 0x88) … | blend-word cache slots | `0x93960:31` (cb+0x8c = param_1[0x11]) |
| `+0x1ac` | cached CB_TARGET_MASK | `0x93960:37,39`; `0x91b50:46,48` |
| `+0x11c` | cached DB_DEPTH_CONTROL | `0x93960:47,49`; `0x91e10:15,17` |
| `+0x124/+0x126/+0x127`, `+0x129/+0x12a/+0x12b` | cached STENCILREFMASK front/back (2 u32s) | `0x91e10:30-43`; `0x123810:33-42` |
| `+0x16/+0x17/+0x18` | cached viewport (x,y,w,h) + depth-range floats (0.5 default) | `0x93960:55-66`; `0x94600:50-63` |
| `+0xb4/+0xbc/+0xc4` | alternate viewport cache (x/y second pair + depth-range hi) | `0x93960:56,61,66` |
| `+0x1cc` | cached VGT_PRIMITIVE_TYPE | `0x93690:26`; `0x94d00:13`; `0x94ec0:12`; `0x94bf0:10` |
| `+0xa8` | instance V# descriptor (qword) written by 0x123810 | `0x123810:14` |
| `+0xf4` | dword: pass slot index k (0..13) | `0x123810:30`; `0x11d3d0` via callers |

### 1.2 Device/CRenderContext (`cb`) fields used here

| Offset | Field | Evidence |
|---|---|---|
| `+0x2e8` | material block base (see §1.3) | `0x93960:94`; `0x93690:36` |
| `+0xc090` | per-material GNM sub-cursor (first arg of every SGPR emitter) | `0x85fc10:48,57,72…`; `0x862490:9` |
| `+0xc378` | geometry/vertex-buffer object; its first qword is a cursor for `0x86c5f0` and `0x86bc00` | `0x93960:83`; `0x94ab0:49` |
| `+0xc398` | geometry descriptor A (byte +0x27 = index-count nibble for kind 0) | `0x10add0:30`; `0xdb620:26` |
| `+0xc3a8` | geometry descriptor B (byte +0x19 for kind 0xb0) | `0x10add0:37`; `0xdb620:33` |
| `+0xc3b8` | geometry descriptor C (byte +0x19 for kinds 0x45/0xad/0x145/0x1ad) | `0x10add0:26`; `0xdb620:21` |
| `+0xc548` | draw-kind selector; see §4.1 | `0x10add0:20`; `0xdb620:16`; written `0x93960:81`, `0x94600:76` |
| `+0xc8b0` | dword vertex-cache dirty count (if != 0 → reset) | `0x93960:82`; `0x94600:77` |
| `+0xc8b4` | dword reset to 0 with the cache | `0x93960:85`; `0x94600:80` |
| `+0xc8d4` | dword set to 0xffffffff with the cache | `0x93960:86`; `0x94600:81` |
| `+0xc8c1` | byte used as the dispatch flag arg | `0x92bd0:131` |
| `+0xc5d8` | RT width (dwords) — read via 0x123810's pass side | inferred |

The vertex-cache reset block (identical in 0x93960/0x94600/0x93ef0/0x118470, proven `0x93960:79-93`):
`cb+0xc548 = 0`; if `cb+0xc8b0 != 0` → `FUN_0086c5f0(cb+0xc378)` (emits VGT reset packets), `cb+0xc8b0=0`, `cb+0xc8b4=0`, `cb+0xc8d4=0xffffffff`, `FUN_0000c540[cb+3]=0`; then `FUN_0086d160(cb,0)` (draw-kind UCONFIG write, reg 0x2d5 = 0); `pass+0x14=0`, `pass+0x18=PTR_DAT_01604408`, `pass+0x18`(low)=0x11.

### 1.3 Material block (`ctx+0x2e8`, read by 0x85fc10)

`param_1 = ctx+0x2e8`. Field map (proven, `0085fc10.c`):

| Offset | Field | Evidence (line) |
|---|---|---|
| `+0xc260` | dword dirty/flag word. bit0x20 = "advanced shader group pending"; bit0 = "compute/PS group pending" | 28-29, 194 |
| `+0xc24a..0xc256` | per-stage pending bytes (see below) | 53,66,132,197,221,226,248,314,369,383,400,473 |
| `+0xc24b..0xc24e`,`+0xc259..0xc25d` | stage "changed" bytes | 53-54,132,197-198,226,236,369-371,383-384,400 |
| `+0xc264` | ushort accumulator of pending stage bits (OR-ed 8/0x20/4/2) | 55,237,372,385 |
| `+0xc0a8` | VS record pointer (via `FUN_0000c0a8`) | 368 |
| `+0xc0b0` | VS record pointer alt (via `FUN_0000c0b0`) | 370 |
| `+0xc0b8` | GS record pointer | 31 |
| `+0xc0c0` | ES record pointer | 33 |
| `+0xc0c8` | LS record pointer | 195 |
| `+0xc0d0` | HS record pointer | 196 |
| `+0xc0e4..0xc0f8` | per-stage first-SGPR base (0xe4,0xe8,0xec,0xf0,0xf4,0xf8) | 443,516,175,357,208,291 |
| `+0xc0fc`,`+0xc100..0xc118` | per-stage alt SGPR bases | 11,446,112,360,294,66 |
| `+0xc104..0xc118` | alt user-data bases | 519,294 |
| `+0xc108..0xc118` | stage 2 user-data bases | 446 |
| `+0xc130`,`+0xc140`,`+0xc150` | per-stage 64-bit SGPR values | 454,120,302 |
| `+0xc168`,`+0xc170`,`+0xc178` | per-stage dword SGPR values | 374,46,222 |
| `+0xc188`,`+0xc190`,`+0xc198`,`+0xc1a0`,`+0xc1a8`,`+0xc1b0` | per-stage user-data descriptor pointers | 32,401,133,67,315,249 |
| `+0xc180` | stage-0 (VS) user-data descriptor pointer | `0x8616f0:10` |
| `+0xc1b8` | GS secondary user-data descriptor (tessellation mirror) | `0x862490:11` |
| `+0xc5e4` | ES present flag (0 → use embedded record path) | 34 |
| `+0xc5e8` | u32 packed (hi16 * lo16) = texture size for user data | 183-184, 467-468 |
| `+0xc5f4` | VS resource size for `FUN_00880530` | 201,227 |
| `+0xc5f8` | scratch 16 bytes for the LS/HS constant block | 202-219 |
| `+0xc600` | qword staged into the `+0x7890` table | 212 |
| `+0xc608` | int, sign-bit → `FUN_00880480` clamp | 233-234 |
| `+0xc60c` | dword descriptor for `FUN_00880530` | 200 |
| `+0xc612` | byte: LS/HS path present | 197 |
| `+0xc613` | byte: GS tessellation mirror present | `0x861520:29`; `0x862490:10` |
| `+0x88` | byte: "native/SQTT present" → emits `FUN_0086a7f0` | 49,62,223,244,379,396 |
| `+0x78` | qword (SQTT/thread-trace marker buffer) | 105,171,287,353,439,512 |
| `+0x90, +0x1890, +0x3090, +0x4890, +0x6090, +0x7890, +0x9090` | embedded per-stage user-data source arrays | 23,403,135,69,317,251,21 |

Embedded-array → stage mapping (proven from the `import_memcpy` sources): stage0 `+0x90`, stage1 `+0x1890`, stage2 `+0x3090`, stage3 `+0x4890`, stage4 `+0x6090`, stage5 `+0x7890`, stage6 `+0x9090`.

Shader-bind emitters called from 0x85fc10 (all proven by decompile + the `import_sceGnm*` name in the stub):

| Callee | Emits | Packet size | Call site |
|---|---|---|---|
| `0x86c2c0` | `sceGnmSetEsShader` | 0x50 dwords | 48 |
| `0x86cb00` | `sceGnmSetGsShader` | 0x74 | 57 |
| `0x86c760` | `sceGnmSetLsShader` | 0x5c | 222 |
| `0x86c330` | `sceGnmSetHsShader` | 0x78 | 239 |
| `0x86c170` | `sceGnmSetVsShader` | 0x74 | 374 |
| `0x86c250` | `sceGnmUpdateVsShader` | 0x74 | 377 |
| `0x86c050` | `sceGnmSetPsShader350` | 0xa0 | 391 |
| `0x86c110` | `sceGnmUpdatePsShader350` | 0xa0 | 394 |
| `0x86d4c0` | 64-bit SGPR (user-data chunk ptr) | 4 dw | 72,79,92,97,… |
| `0x86d5e0` | generic user-SGPR block | 4+N dw | 109,112,175,… |
| `0x86a7f0` | SQTT/thread-trace marker (0xC0055000) | 7 dw | 50,63,… |
| `0x8619a0` | scattered user-SGPR write (per-slot gather) | — | 219 |
| `0x8643f0` | bump-alloc a user-data buffer in the material heap | — | 68,134,250,316,402,475 |

`0x86d4c0` writes one SET_CONTEXT_REG pair: header `0xC0027600` (or `0xC0027601` for stage 0), reg `param_3 - 0x2c00 + DAT_01290410[stage]`, plus the 64-bit value (`0086d4c0.c:18-22`). `0x86d5e0` writes header `0xC0001000/0xC0007600` + N dwords from a pointer (`0086d5e0.c:18-61`). Both are the user-data (SQ/constant) upload path → **Vulkan: push constants / uniform buffer of the per-draw user-data block**.

### 1.4 Pass descriptor (`param_2` of 0x93960/0x94600)

| Offset | Field | Evidence |
|---|---|---|
| `+0x62` | type byte. 0x93960 needs `(b\|2)==3` (b∈{1,3}); 0x94600 needs `b==2` | `0x93960:17`; `0x94600:18` |
| `+0xf5` | flags; bit0x20 required | `0x93960:19` |
| `+0x167` | flags for 0x94600; bit0x20 required | `0x94600:18` |
| `+0xe4` | colour-target block (8 qwords) fed to `0x85cbc0(cb,0,+0xe4)` | `0x93960:20,24` |
| `+0x124` | alternate colour-target block (used when `param_2[9]&1`) | `0x92140:28` |
| `+0x120` | viewport dims: low16 = width, high16 = height | `0x93960:53-54` |
| `+0x194` | viewport dims for 0x94600 (low16 w, high16 h) | `0x94600:48-49` |
| `+0x164` | depth-target descriptor block (fed to `0x86a290`) | `0x92140:165`; `0x93ef0:29` |
| `+0x184/+0x188` | geometry ranges for 0x94600 clear | `0x94600:20-21` |
| `+0x9` (qword idx) | bit0 selects +0xe4 vs +0x124 | `0x92140:24,39` |

`param_2[8]` (0x92140) is a depth descriptor; `param_2[1..7]` are colour-target descriptors.

## 2. Pass functions

### 2.1 `0x93960` — pass function A, 1029 B

(a) Reads/writes: pass ctx `param_1` fields in §1.1; pass descriptor `param_2` `+0x62,+0xf5,+0xe4,+0x120`. Writes ctx `+0x48+8i` (RT ptr cache), `+0xa8` (depth cache), `+0x8/+0xc/+0xd` dirty bytes, `+0x8c/+0x1ac/+0x11c/+0x23/+0x1cc` state caches, `+0x90`/`+0xb4`/`+0xb8`/`+0xc4` viewport caches, `+0x18` stage descriptor, `+0x14` kind. Device `cb`: `+0xc548`, `+0xc378`, `+0xc8b0/c8b4/c8d4`, and the GNM cursor `+0x10`.

(b) Sequence of GPU effects (proven line by line):
1. `0x93960:18` `FUN_00093840(param_1,param_2,0xffffffff)` — outer pass setup (not decoded here).
2. `:21` `FUN_0086ced0(cb,0x2e)` → EVENT_WRITE 0x46 (`0xC0004600`, `0086ced0.c:17`).
3. `:24` RT set: if RT descriptor changed, `FUN_0085cbc0(cb,0,param_2+0xe4)` → CB_COLOR block.
4. `:28-29` blend word 0 via `0x87c380/0x87c390(...,0)`; if changed `FUN_00869a50(cb,0,0)` → CB_BLEND0_CONTROL (`0xC0016900`, reg 0x1e0, `00869a50.c`).
5. `:38` `FUN_00869990(cb,0xf)` → CB_TARGET_MASK reg 0x8e = 0xF (`00869990.c:17-19`).
6. `:42` `FUN_00869ce0(cb,2,0xcc)` → CB_COLOR_CONTROL reg 0x202 = mode2|rop3 0xCC (`00869ce0.c:17-19`).
7. `:44-45` depth word via `0x87cb90/0x87cba0(...,0,7)/0x87cc00(...,1)`; if changed `FUN_00869d60(cb,word)` → DB_DEPTH_CONTROL reg 0x200 (`00869d60.c:17-19`).
8. `:59` `FUN_0085de40(0.5,0.5,cb,0,0,w,h)` → viewport + scissor + depth-range (see §2.9).
9. `:71` SC-mode word via `0x87c4a0/…c0/…b0/…d0(2,2)`; if changed `FUN_0086e1a0(cb,word)` → PA_SU_SC_MODE_CNTL reg 0x205 (`0086e1a0.c:17-19`).
10. `:79-93` primitive reset block (§1.2): sets `cb+0xc548=0`, resets vertex cache, `FUN_0086d160(cb,0)` (reg 0x2d5), caches prim 0x11.
11. `:94` `FUN_00861600(cb+0x2e8,0,0)` — material PS-header setter (see §2.10).
12. `:95` `FUN_00093690(param_1)` — the fullscreen rect draw object.
13. `:96-99` `FUN_0086d210(cb,4,3)` allocate a 4-dword label slot; `*slot=0`; `FUN_0086eb60(cb,4,0,slot,1,1,0,0)` → EVENT_WRITE_EOP 0x47 (`0xC0044700`, `0086eb60.c:19`); `FUN_0086edc0(cb,slot,0xffffffff,5,1)` → WAIT_REG_MEM 0x3c (`0xC0053c00`, `0086edc0.c:18`).
14. `:100` `FUN_00869ce0(cb,1,0xcc)` — CB_COLOR_CONTROL mode 1.

(c) Vulkan mapping: a render pass that sets RT + depth + raster state, draws one fullscreen triangle/rect, then inserts an EOP label + WAIT_REG_MEM (GPU fence / `vkCmdWriteTimestamp` + `vkCmdWaitEvents` or a pipeline barrier). `cb+0xc548` write = dynamic pipeline selection. Viewport/scissor = dynamic state.

(d) Callers/callees: called by `0x118470` (twice, lines 112-113), `0xcaed0`, `0x23d750`, `0x5d5ec0`, `0x178cb0`, `0xb8c60`, `0xa6640`, `0xc1fc0` (lines 116-117), `0xc1ce0`-family, etc. Callees: `0x93840, 0x86ced0, 0x85cbc0, 0x87c380, 0x87c390, 0x869a50, 0x869990, 0x869ce0, 0x87cb90, 0x87cba0, 0x87cc00, 0x869d60, 0x85de40, 0x87c4a0..d0, 0x86e1a0, 0x86c5f0, 0x86d160, 0x861600, 0x93690, 0x86d210, 0x86eb60, 0x86edc0`.

(e) Size 1029 B (proven, header).

### 2.2 `0x94600` — pass function B, 1005 B

(a) Pass ctx fields as §1.1; descriptor `param_2` `+0x62,+0x167,+0x184,+0x188,+0x194,+0x164`(via puVar1). Writes ctx `+0x14,+0x18,+0x20,+0x8c/+0xa8` (depth cache via param_1[0x15]), `+0x11` byte, `+0x16..+0x18`, `+0x1cc`, `+0x39`.

(b) Sequence:
1. `:18` gate: `param_2+0x62==2` and `param_2+0x167 & 0x20`.
2. `:20-23` read `param_2+0x184/0x188` and `param_2+0x164` (geometry/index ranges); `FUN_00869180()` (index size).
3. `:26-30` set `pass+0x14=1`, `pass+0x18=PTR_DAT_01604428`, `pass+0x18`(low)=0x20 (draw kind 0x20).
4. `:31-32` `FUN_00092bd0(param_1, uVar6*0x100 + (iVar3*uVar7), …, local_40, 1)` — the clear/quad emitter (see §2.11); local_40 = {0xf}.
5. `:33-38` `FUN_0086d210(cb,8,3)`; `*slot=0`; `FUN_0086eb60(cb,0x28,0,slot,1,1,0x38,2)` → EOP label; `FUN_0086edc0(cb,slot,0xffffffff,5,1)` → WAIT.
6. `:39-40` `FUN_0086ced0(cb,0x2c)`; `FUN_00869ce0(cb,0,0xcc)` (CB_COLOR_CONTROL mode 0).
7. `:41-43` SC-mode word via `0x87c620/0x87c650(...,1)`; `FUN_0086dd20(cb,word)` → DB_RENDER_CONTROL reg 0 (`0086dd20.c:17-19`).
8. `:44-46` word via `0x87d070/0x87d0a0(...,1)`; `FUN_0086be10(cb,word)` → DB_RENDER_OVERRIDE reg 3 (`0086be10.c:17-19`).
9. `:48-67` viewport dims from `param_2+0x194`; `FUN_0085de40(0.5,0.5,cb,0,0,w,h)`.
10. `:64-71` depth block: if changed, `FUN_0086a290(cb,param_2+0x164)` (depth target block) + `FUN_0086e3a0(cb,*puVar1 & 3)` (PA_SU_POLY_OFFSET_DB_FMT_CNTL reg 0x2de = `DAT_0128fee0[idx]`) + `FUN_00869f50(0,cb)` → DB_DEPTH_CLEAR reg 0xb = 0.0f (`00869f50.c:17-19`).
11. `:72` `FUN_00091e10(param_1,DAT_02ee3ce0+0x20c,0xff)` — DB depth/stencil state block (see §2.12).
12. `:73` `FUN_00091cd0(param_1,DAT_02ee3ce0+200)` — SC-mode + depth-bias block (see §2.12).
13. `:74-88` primitive reset block, prim 0x11.
14. `:89` `FUN_00861600(cb+0x2e8,0,0)`.
15. `:90` `FUN_00869f50(0x3f800000,cb)` → DB_DEPTH_CLEAR = 1.0f.
16. `:91` `FUN_00093690(param_1)` fullscreen rect draw.
17. `:92-96` DB_RENDER_CONTROL / CB_COLOR_CONTROL again; `pass+0x11 |= 1` (depth dirty).

(c) Vulkan: a clear pass (depth clear via `vkCmdClearDepthStencilImage` / a load-op clear; colour clear via the fullscreen rect), with EOP+WAIT fence. `cb+0xc548=0`.

(d) Callers: `0xbb840`, `0xbae70`. Callees as listed. Size 1005 B.

### 2.9 `0x85de40` — viewport/scissor/depth-range emitter (called by both passes)

Signature `FUN_0085de40(zdepth_lo, zdepth_hi, cb, x, y, w, h)` (`0085de40.c:3`). Writes (proven):
- `0x86d7b0` reg `0xb4+2i` and `0x10f+6i` = PA_SC_VPORT (viewport) — `0086d7b0.c:31-45`.
- `0x86d950` reg `0xc/0xd` = PA_SC_SCREEN_SCISSOR — `0086d950.c:17-20`.
- `0x86a010` reg `0x8d` = PA_SC_WINDOW_SCISSOR (4×u9 rounded to /4) — `0086a010.c:17-19`.
- `0x86db40` reg `0x206` = PA_CL_VTE_CNTL — `0086db40.c:17-19`.
- `0x86a090` reg `0x2fa` = depth-range (PA_CL_VPORT_Z) — `0086a090.c:18-23`.
→ Vulkan: `VkViewport`, `VkRect2D` scissor, depth range, VTE control = dynamic state.

### 2.10 `0x861600` — material PS-header setter

`FUN_00861600(material, param_2, param_3)`: sets `+0xc251=1`, `+0xc249 |= (lVar2 != param_2)` where lVar2 = `material+0xc0a8` (VS/PS header record), `+0xc259 = (same)`, clears bit1 of `+0xc260`, sets `+0xc266 |= 2`, `+0xc188 = param_3`, `+0xc0a8 = param_2`, `+0xc1c0 = 0` (`00861600.c:12-34`). Called by 0x93960/0x94600/0x93ef0/0x94ab0 with `(0,0)` (reset) or with a record.

## 3. Draw objects

All draw objects take `param_1` = pass context (their `*param_1` = cb). Common preamble: read draw kind from `pass+0x18` byte bits 1-2 (`(*(byte*)(param_1+3) & 6)`) to choose primitive source; call `FUN_0086a900(cb, prim)` (VGT_PRIMITIVE_TYPE via UCONFIG 0x242, `0086a900.c`); call `FUN_00862510(material, a, b)` (per-stage start-vertex SGPR, §3.6); call `FUN_0085fc10(material)`; then the draw wrapper.

### 3.1 `0x93690` — clear / fullscreen rect draw object, 291 B

(a) Reads `param_1[4]` (+0x20) stage descriptor dword +0 (`uVar2`); writes `param_1[0x3b+uVar2]` (+0x1d8+8i) shader-record cache; `param_1+0x1cc`, `param_1+0x39` caches.
(b) 
1. `:10-11` `lVar3 = *(DAT_02ee3ce0+0x8a8)` shader list; `FUN_00095580(lVar3+0x20)` ensure loaded; `FUN_0014d7b0(...)` → record object `plVar1 = *(lVar3+0x60)`.
2. `:14-24` depending on `uVar2` (stage idx): `FUN_00861520 / 0x861260 / 0x860bb0` bind that record's program as PS/GS/ES (`plVar1[1]`, `*plVar1+8`).
3. `:26-28` `FUN_0086a900(cb,0x11)` → VGT_PRIMITIVE_TYPE = 0x11 (RectList) if changed.
4. `:31-33` `FUN_0086bed0(cb,0,2)` → INDEX_TYPE (reg 0x2a) reset.
5. `:36` `FUN_0085fc10(cb+0x2e8)` — material bind.
6. `:37` `FUN_0086ac10(cb,3,0)` → **DrawIndexAuto count=3** (import `sceGnmDrawIndexAuto`; header produced import-side, `W1-14` §3.1). 
(c) Vulkan: `vkCmdDraw(3,1,0,0)` — one fullscreen triangle/rect with the bound engine shader. No vertex buffer needed (procedural fullscreen).
(d) Callers: `0x93960`, `0x93ef0`, `0x94600`. Callees: `0x95580, 0x14d7b0, 0x861520/0x861260/0x860bb0, 0x86a900, 0x86bed0, 0x85fc10, 0x86ac10`. Size 291 B.

### 3.2 `0x94d00` — DrawIndexOffset draw object, 204 B

(a) Signature `(pass, int mode, a3, a4, u32 count_lo, u32 count_hi)` (`00094d00.c:3`). Reads `pass+0x18` byte; writes `pass+0x1cc` cache. `a3/a4` unused (index base comes from the pass/material).
(b) `:11-21` primitive from `DAT_00f9f2e0[mode]` (or 9 for tessellation path) → `0x86a900`; `:24` `0x862510(material,0,0)`; `:25` `0x85fc10`; `:26` `FUN_0086b040(cb,param_5,param_6,0)` → **DrawIndexOffset** (`sceGnmDrawIndexOffset`, import-side packet, `W1-14` §3.1); `:27` `0x862510(material,0,0)`.
(c) Vulkan: `vkCmdDrawIndexed(count, 1, firstIndex=param_5, …)`.
(d) Caller: `0x128d20` (which is called by `0x1222e0` render-job draw entry). Callees: `0x86a900, 0x862510, 0x85fc10, 0x86b040`. Size 204 B.

### 3.3 `0x94dd0` — DrawIndexOffset + instances, 231 B

(a) Signature adds `param_7` = instance count. Same reads/caches as 3.2.
(b) `:22` `FUN_0086c000(cb,param_7)` → IT_NUM_INSTANCES (`0xC0002F00`, `0086c000.c:17-18`); `:25` `0x862510`; `:26` `0x85fc10`; `:27` `0x86b040(cb,param_5,param_6,0)` DrawIndexOffset; `:29` `0x86c000(cb,1)` restore instances=1.
(c) Vulkan: `vkCmdDrawIndexed(count, instances, …)`.
(d) Callers: none in the index (proven, header). Callees: `0x86a900, 0x86c000, 0x862510, 0x85fc10, 0x86b040`. Size 231 B.

### 3.4 `0x94ec0` — DrawIndexAuto draw object, 234 B

(a) Signature `(pass, int mode, u32 a3, u32 count)`; reads `pass+0x18`, writes `pass+0x1cc`, `pass+0x39`.
(b) `:10-20` primitive from `DAT_00f9f2e0[mode]` (or 9); `:21-24` `FUN_0086bed0(cb,0,2)` INDEX_TYPE reset; `:27` `FUN_00862510(material,param_3,0)`; `:28` `0x85fc10`; `:29` `FUN_0086ac10(cb,param_4,0)` → **DrawIndexAuto count=param_4**; `:30` `0x862510(material,0,0)`.
(c) Vulkan: `vkCmdDraw(count,1,0,0)`.
(d) Callers: ~30, incl. `0xb9710, 0xa69d0, 0x1666e0, 0x5d5ec0, 0x164a60, 0x5e3b50, 0x5d1540, 0x166c90, 0xb8c60, 0x118470` (line 203). Callees: `0x86a900, 0x86bed0, 0x862510, 0x85fc10, 0x86ac10`. Size 234 B.

### 3.5 `0x10add0` — DrawIndirect draw object, 264 B

(a) Signature `(pass, uint* args, u32 data_offset)` where `args = {lo, hi}` (`0010add0.c:3`). Reads `pass+0x39` cache, `pass+0x14`.
(b) `:12-15` `FUN_0086bed0(cb,0,2)`; `:16-17` `FUN_0086ad40(cb, pass+0x14, args[0] | (args[1]&0xfff)<<32)` → **SET_BASE** (indirect args base, `0x86ad40`); `:19` `0x85fc10`; `:20-42` draw-kind dispatch (see §4.1) → `FUN_0086b150(cb, data_offset, stage, vtx_sgpr, inst_sgpr, 0)` → **DrawIndirect** (`sceGnmDrawIndirect`); `:44` `FUN_0086c000(cb,1)` instances=1.
(c) Vulkan: `vkCmdDrawIndirect(argsBuffer, offset, 1, stride)`.
(d) Callers: `0x109ca0`, `0x18e3d0`. Callees: `0x86bed0, 0x86ad40, 0x85fc10, 0x86b150, 0x86c000`. Size 264 B.

### 3.6 `0xdb620` — DrawIndexIndirect mesh draw object, 213 B

(a) Signature `(pass, uint* args, u32 data_offset)`.
(b) `:12-13` `0x86ad40(cb, pass+0x14, args…)`; `:15` `0x85fc10`; `:16-37` draw-kind dispatch; `:38` `FUN_0086b3d0(cb, data_offset, stage, vtx_sgpr, inst_sgpr, 0)` → **DrawIndexIndirect** (`sceGnmDrawIndexIndirect`); `:40` `0x86c000(cb,1)`.
(c) Vulkan: `vkCmdDrawIndexedIndirect(argsBuffer, offset, 1, stride)`.
(d) Callers: `0x189060, 0xdb390, 0x10b0a0, 0x18d6f0`. Callees: `0x86ad40, 0x85fc10, 0x86b3d0, 0x86c000`. Size 213 B.

### 3.6a `0x862510` — per-stage start-vertex SGPR (shared preamble)

`FUN_00862510(material, a, b)`: by draw kind `material+0xc260` selects a descriptor (`+0xc0c0/+0xc0d0/+0xc0b0`), reads byte `+0x19`, splits into nibbles, and calls `FUN_0086d550(cb+0xc090, stage, nibble, value)` for each nonzero nibble (`00862510.c:12-46`). `0x86d550` writes one SET_SH_REG (`DAT_01290410[stage]` reg). → Vulkan: push constant (start vertex / base instance SGPR).

### 3.7 Draw-kind selector (§4.1) — used by 0x10add0 and 0xdb620

`iVar2 = *(int*)(cb+0xc548)` (proven `0010add0.c:20`, `000db620.c:16`):
| kind | stage (`uVar5`) | nibble source |
|---|---|---|
| 0 | 2 | `*(byte*)(*(cb+0xc398)+0x27)` |
| 0x45, 0xad | 6 | `*(byte*)(*(cb+0xc3b8)+0x19)` |
| 0xb0 | 4 | `*(byte*)(*(cb+0xc3a8)+0x19)` |
| 0x145, 0x1ad | 6 | `*(byte*)(*(cb+0xc3b8)+0x19)` |
| other | — | no draw (jump to epilogue) |
Nibble: `low = bVar1 & 0xf` = vertex SGPR offset; `high = bVar1>>4` = instance SGPR offset (`0010add0.c:42`, `000db620.c:38`). The same kind values gate `FUN_0086d160` (reg 0x2d5, `0086d160.c:12-24`) and `FUN_00862510`.

## 4. Render-target / material / table binders

### 4.1 `0x92140` — render-target set binder, 924 B

(a) `param_1` = pass ctx (undefined8*), `param_2` = array of 8 RT descriptors + `[8]` depth descriptor + `param_2[9]` byte selector. Writes `param_1+0x48+8i` (RT ptr cache), `param_1+0xa8` (depth cache), `param_1+0x10` (colour-dirty mask), `param_1+0x8` (state word bits 3-4), `param_1+0x11` (depth dirty bit).
(b) 
1. `:20-135` resolve each of the 8 RT pointers `local_78[0..7]`: validate `+0x62` type, then `= desc+0xe4` or `desc+0x124` by `param_2[9]&1`.
2. `:136-147` `FUN_00091fc0(param_1,0,0,w,h)` — viewport/scissor dims from depth desc.
3. `:149-194` loop i=0..7: if dirty bit set or cached ptr changed → `FUN_0085cbc0(cb, i, desc)` (CB_COLOR block, §4.2), cache ptr, clear bit i of `+0x10`; at i==7 also depth: `FUN_0086a290(cb, depth+0x164)`, `FUN_0086e3a0(cb,*depth&3)`, `FUN_00869f50(0,cb)`.
4. `:180-184` encode pass state: `param_1+0x8` bits 3-4 = 0/0x10/0x8; clear depth-dirty bit.
(c) Vulkan: bind the render pass's colour/depth attachments; dynamic viewport/scissor. `0x86a1a0` writes CB_COLORn_BASE..INFO → the Vulkan colour attachment / framebuffer.
(d) Callers: `0xbb840, 0xb9710, 0xa69d0, 0x23d750, 0x5b7f90, 0xcaed0, 0x5d5ec0, 0xc4de0, 0x118470` (line 63), `0xc1ce0`, `0xc1fc0`, `0x1818e0`, … Callees: `0x96f50, 0x96f60, 0x91fc0, 0x85cbc0, 0x86a290, 0x86e3a0, 0x869f50`. Size 924 B.

### 4.2 `0x85cbc0` — SetRenderTarget, 269 B

(a) `param_1` = cb, `param_2` = colour slot index, `param_3` = 8-qword descriptor (or NULL).
(b) `:20` NULL → `FUN_0086a1a0(cb,slot,0)` (clear). Else copies the 8 qwords, optionally patches the tile/format words via `0x87f0d0`/`0x87eda0`/`0x87efd0`, then `FUN_0086a1a0(cb,slot,&local)`.
(c) `0x86a1a0` emits SET_CONTEXT_REG block header `0xC00E6900` (15 dwords) starting at reg `0x318+0xf*slot` (CB_COLORn_BASE..), plus a second packet `0xC0001000`/`0xC0016900` reg 0x31C = CB_COLORn_INFO (`0086a1a0.c:34-54`). → Vulkan: the colour attachment (image view + format) of the render pass / framebuffer.
(d) Callers: `0x92140`, `0x93960`. Callees: `0x86a1a0, 0x87f0d0, 0x87eda0, 0x87efd0`. Size 269 B.

### 4.3 `0x85fc10` — Material_BindGraphicsShaders_B, 3994 B

(a) `param_1 = ctx+0x2e8` (material block, §1.3). 
(b) Two groups (proven):
- Group A (`+0xc260 & 0x20`, lines 29-193): ES record `+0xc0c0` → `0x86c2c0` (SetEsShader) with user data at `+0xc1a0` (copy from `+0x6090`); GS record `+0xc0b8` → `0x86cb00` (SetGsShader) with user data `+0xc1a0`… ; LS/HS via `+0xc0c8/+0xc0d0`; stage-3 (`+0xc198` from `+0x4890`), stage-2 (`+0xc190`? actually `+0xc198`/`+0x4890`), stage-4 (`+0xc1a0`/`+0x6090`), stage-5 (`+0xc1a8`/`+0x7890`), stage-6 (`+0xc1b0`/`+0x9090`), each via `0x8643f0` (alloc) + `0x86d4c0`/`0x86d5e0` (user SGPRs).
- Group B (`+0xc260 & 1`, lines 194-367): LS/HS path (`+0xc1a8`/`+0xc1b0`, `FUN_0086c760`/`FUN_0086c330`), stage-5 (`+0xc1a8`/`+0x7890`), stage-6 (`+0xc1b0`/`+0x9090`), with SQTT marker `0x86a7f0` when `+0x88`.
- Group C (lines 368-527): VS record `+0xc0a8` → `0x86c170` (SetVsShader) / `0x86c250` (UpdateVsShader); PS record via `+0xc0b0`? → `0x86c050` (SetPsShader350) / `0x86c110` (UpdatePsShader350); stage-1 (`+0xc188`/`+0x1890`), stage-2 (`+0xc190`/`+0x3090`).
Each block ends by clearing the pending byte (`+0xc24a..0xc256`, `+0xc264` bits) (lines 188-192, 363-366, 522-527).
(c) Vulkan: bind the pipeline (VS/PS/GS/ES/LS/HS modules) and upload the per-stage user-data block (push constants / UBO). `0x86c2c0` etc. → `vkCmdBindPipeline` / pipeline cache keyed by shader records; `0x86d5e0` → push constant / UBO writes.
(d) Callers (≥13): `0x1a8c60, 0x1338d0, 0x94dd0, 0xdb620, 0x133550, 0x10add0, 0x133a50, 0x94d00, 0x133e20, 0x133bd0, 0x93690, 0x133750, 0x94ec0`. Callees as listed. Size 3994 B.

### 4.4 `0x95320` — root-table builder, 252 B

(a) `FUN_00095320(u32 mask, undefined8* src, u32 tag)`. `mask` = low 10 bits (bits 0..9; bit 9 = 0x200, bit 8 = 0x100, bit 7 = 0x80 tested as sign).
(b) `:9-13` popcount of mask (SWAR), `FUN_00012820(popcount*8, tag, 8, 0)` bump-alloc; `:14-52` copy `src[0..9]` in ascending bit order for each set bit. (Decompile declares void; the return is unproven — see gap.)
(c) Vulkan: build a descriptor-set-like table of N qwords from the masked `ctx+0x90..` window; the 10 elements are the per-draw binding tables (V# table, view constants, object block, material buffer, baked-ambient, sampler table, …). Caller `0x11d3d0` passes `ctx+0x12` (= `ctx+0x90`) as `src` and `DAT_02ed1f54` as tag.
(d) Callers: `0x94fb0, 0x11d5b0, 0x11d3d0, 0x1051f0, 0x1809a0, 0x103ad0, 0x5b7630, 0xdb390, 0x10b0a0, 0x1823e0`. Callee: `0x12820`. Size 252 B.

### 4.5 `0xae150` — user-data table binder, 441 B

(a) `FUN_000ae150(binder, u64 table_ptr)` where `binder = *pass` (the renderer/ctx). Reads `binder+0x18` flag word (bits 1..0x20); writes nothing to the binder.
(b) For each flag bit i (0..5), if `binder[binder[4][i] + 0x3b]` (per-stage cached object, §1.1) is non-null and its type byte `+0x19 == 2`, call `FUN_00862490(*binder+0x2e8, slot, &table_ptr, 2)` (`000ae150.c:12-58`).
(c) `0x862490` reads `material+0xc180+slot*8` → a ushort `+0x10` and calls `0x86d5e0(cb+0xc090, slot, lo, &table, hi)` (`00862490.c:8-9`). → Vulkan: write the per-draw user-data (root table) into the bound user-data block / push constants, one slot per shader stage.
(d) Callers: `0x1a8c60, 0x11d5b0, 0x198bb0, 0x11d3d0, 0xadd50, 0x1a6d20, 0x1809a0, 0x1a7290, 0x19c7e0, 0xdb390, 0x10b0a0`. Callees: `0x862490`. Size 441 B.

### 4.6 `0x123810` / `0x123cc0` — instance/object block writers

(a) `FUN_00123810(renderable, pass, int index, char flag, uint* inst)` (`00123810.c:3`).
(b) `:14` `pass[0x15]` (= `pass+0xa8`) = `inst[0] | (inst[1]&0xfff)<<32` — the instance V# descriptor. `:15-16` if `*(byte*)(renderable+0x10)&0x10` → `FUN_000ab310(*(renderable[0xb]+0x78), pass)`. `:18-19` `lVar2 = *(renderable+0xf0)`, `lVar6 = index*0x58`, `FUN_00193580(lVar2+lVar6, pass)` (scope bind). `:21-45` if VS-program flag clear: `FUN_00095580(lVar2+0x20+lVar6)`, `FUN_0014d7b0(...)`; then if flag → `FUN_001122c0(*(lVar4+0x70), pass)` else `FUN_00094c60(*pass, *(lVar2+0x50+lVar6)+0x28)` (index-buffer bind, `00094c60.c`: `0x86bf50` INDEX_BASE + `0x86bfb0` INDEX_BUFFER_SIZE + `0x86bed0` INDEX_TYPE). `:30-44` if `pass+0xf4` in {2,3,4,5} → compute a STENCILREFMASK word and if changed `FUN_00869bb0(cb, front, back)` (reg 0x10c/0x10d) and cache `pass+0x124/0x126/0x127/0x129/0x12a/0x12b`.
(c) Vulkan: bind the index buffer + instance buffer (V#) and set stencil reference.
(d) `0x123cc0` is a thunk: `FUN_00123810()` (`00123cc0.c:6`). Callers: `0x123810` ← `0x1239c0, 0x123cc0`; `0x123cc0` ← `0x11d5b0, 0x1af700, 0x11d3d0, 0x18d6f0`. Sizes 390 B / 44 B.

## 5. Frame / submit / sync

### 5.1 `0x90ea0` — RenderBackend_Frame (per-frame reset/fence), 351 B

(a) `param_1` = frame object; `param_2` = render backend. Fields: `+0x7b0` int frame parity (toggles 0/1 via `:18-20`), `+0x18` = `param_1+0x20 + parity*0x1d0` (frame slot, `:21`), `+0x770+parity*8` / `+0x780+parity*8` = frame-fence label begin/end (`:27-30`), `+0x7c0` = lock (`:32-41`), `+2000` (0x7d0) = arg to `0x8ef20`, `+4` = elapsed µs (`:49`). `param_2+0x18` read.
(b) `:17` `FUN_000323d0(local_40)` timestamp; `:23-25` `FUN_00096160/0x96190(DAT_02ed3670)` (videoout) + `FUN_000a1a80(param_2+0x18, …)` (frame-view-params swap); `:26` `FUN_0008d730(param_1)` (increments `+0x10`); `:27-31` spin-wait until the frame-slot label equals the written value; `:32-61` spin-lock `+0x7c0`, `FUN_0008ef20(param_1[0x7d0],0)` (deferred-memory delete/flush), `FUN_0008c860()` (free the two temp arenas), record elapsed.
(c) Vulkan: per-frame fence / frame-slot timeline semaphore; the label spin is the frame-pacing fence (lag 2).
(d) Caller: `0xcefa0` (RenderFrame). Callees: `0x323d0, 0x96160, 0x96190, 0xa1a80, 0x8d730, 0x8ef20, 0x8c860, 0x32430`. Size 351 B.

### 5.2 `0xc5560` — RenderThread::SubmitJob, 1128 B

(a) `param_2` = source (job/render-context array): `+0x10` DCB context list, `+0x48` = control object (byte `+0x14` selects inline vs queued). `param_3` = flip flag.
(b) `:58` `FUN_00017170(0x610, DAT_02ed1f58, 8)` alloc a 0x610-byte job (`piVar7`). Layout (proven): `piVar7[0]=param_3` (flip), `piVar7[1]=*(DAT_02ed33f0+0x10)` (workload), `piVar7[2]=*(DAT_02ed33f0+0x7b0)` (frame idx); `piVar7+0xc` (0x30) DCB ctx array, `piVar7+0x84` (0x210) DCB array begin, `piVar7+0x86` (0x218) end, `piVar7+0x104` (0x410) CCB begin, `piVar7+0x106` (0x418) CCB end. `:72-98` copy new render contexts from `param_2+0xe88/0xe8c/0xe98`. `:99-134` copy the DCB/CCB arrays from `DAT_02ed33f0+0x18`. `:135-140` if `*(char*)(param_2+0x48+0x14)==0` → `FUN_000d07c0(piVar7)` inline; else queue via `FUN_00035480(DAT_02ea4718,&job,1,0,…,0xffffffff)`.
(c) Vulkan: package one frame's command buffers into a submit job; the queue path = job-system submit.
(d) Callers: `0xc59d0, 0xcaed0, 0xcfdb0, 0xcefa0, 0xc53d0`. Callees: `0x17170, 0xd07c0, 0x35480, 0x34b10`. Size 1128 B.

### 5.3 `0xd07c0` — KickoffSubmitJob, 394 B

(a) `param_1` = job. Fields: `param_1[2]/[3]` (0x10/0x18) = DCB ctx list; `param_1[0x42]/[0x43]` (0x210/0x218) DCB array; `param_1[0x82]/[0x83]` (0x410/0x418) CCB array; `*param_1` flip flag; `param_1[1]` workload.
(b) `:19-30` for each DCB ctx `FUN_00035590(DAT_02ea4718, ctx, 0,0,1)` (wait for the job's memory). `:31-47` for each DCB array entry `FUN_00091f80(ctx, workload)` → `0x8636e0` `sceGnmSubmitCommandBuffersForWorkload`; the last when `*param_1==1` uses `FUN_00091f90` → `0x863aa0`/`0x863af0` `sceGnmSubmitAndFlipCommandBuffers`. `:48-56` CCB array → `FUN_0016f100` (async ACE submit). `:57-63` if flip → `import_sceGnmSubmitDone`.
(c) Vulkan: `vkQueueSubmit` (graphics) + `vkQueueSubmit`/present for the flip + `vkQueueWaitIdle` for submit-done.
(d) Caller: `0xc5560`. Callees: `0x35590, 0x91f80, 0x91f90, 0x16f100, 0x323d0`. Size 394 B.

### 5.4 `0x118470` — Frame::InsertWaitFlipDone (frame tail, VBlank scope), 2095 B

(a) `param_1` = renderer; `param_2` = mode byte; `param_3` = pass/render-context array. Fields: `param_3[3]` render ctx (with `+0x23c8`, `+0x23e0` sub-passes, `+0x3a30` RT block, `+0x2b80` RT block); `param_3[9]` (`+0x4c8` brightness, `+0x4cc`), `param_3[0x16]/[0x1a]`; `param_1+0x220290/0x220298` (HUD list), `+0x2202b0/0x2202b8`, `+0x2202f0/0x2202f8`, `+0x220378` (float accumulator), `+0x8050` (render-object count), `+0x220350/0x220354` (ring index).
(b) Sequence (proven): `:53-63` RT set from `param_3[3]+0x3a30` via `FUN_00092140`; `:64` `FUN_000932a0` (target clear); `:67` scope "R::Hud"; `:89` scope "VBlank" + `:100-101` `FUN_0016bb70(…,"VBlank")` + `FUN_00091f50(*param_3)` → `0x86f700` `sceGnmInsertWaitFlipDone` (`0086f700.c:17`) — the flip-wait emit; `:104` scope "R::HudResolve"; `:106-113` `FUN_00093960(*param_3, lVar6)` and `(*param_3, lVar8)` for the two HUD sub-passes; `:114-140` RT set from `param_3[3]+0x2b80` and material reset; `:141-152` bind HUD shader records (`0x937c0`, `0x94ab0`) and state (`0x919e0`, `0x91cd0`, `0x91e10`); `:153-154` optional `FUN_0008be60` clear; `:156-202` fill the HUD user-data block, `FUN_00094ec0(pass,1,0,4)` draw; `:203` scope "R::AfterHud"; `:228` `FUN_00090da0(DAT_02ed33f0, param_3)` (frame-slot EOP → `0x90e30`); `:248-251` `FUN_00095e70` (gamma) + `FUN_00090e30` (EOP); `:252` ring index step.
(c) Vulkan: the frame tail — HUD pass, flip wait (VBlank = `vkWaitSemaphores`/present wait), gamma, frame-slot timestamp.
(d) Caller: `0xcefa0`. Callees: `0x92140, 0x932a0, 0x16b8c0/16b900/16bb70/16bc50, 0x91f50, 0x93960, 0x937c0, 0x94ab0, 0x919e0, 0x91cd0, 0x91e10, 0x8be60, 0x8c0d0, 0x94ec0, 0x862490, 0x90da0, 0x95e70, 0x90e30, 0x323d0, 0x32430`. Size 2095 B.

### 5.5 `0x956e0` — Flip::SubmitAndWaitVblank, 117 B

(a) `param_1` = flip object. Fields: `+0x68` (videoout handle), `+0x70` (flip event), `+0x78` (thread/handle).
(b) `:8` `import_sceVideoOutSubmitFlip(+0x68, 0xffffffff, 1, 0)`; `:9-13` loop `IsFlipPending` + `WaitVblank`; `:14` `TriggerUserEvent`; `:15-19` `FUN_0003b8c0(+0x78)` (join thread) + virtual dtor; `:20` `DeleteFlipEvent`.
(c) Vulkan: `vkQueuePresentKHR` + `vkAcquireNextImageKHR`/`vkWaitForFences` on the present fence.
(d) Callers: `0x96010, 0x95690`. Callees: `0x3b8c0`. Size 117 B.

### 5.6 `0x1469d0` — ResourceFactory::WaitForAllCmdsToFinish, 362 B

(a) `param_1` = factory. Fields: `+0x68` and `+0x70` mutexes, `+0x92` "running" byte, `+0x91` flag, `+0x98`/`+0xb8` condvars.
(b) `:25,32` lock both mutexes; `:33-51` while `FUN_00147f40` returns non-zero: set `+0x91=1`, `FUN_0003b450(+0x98)` (wait), unlock, `FUN_0003b4a0(+0xb8)`, lock, `FUN_001480f0`, retest. `:52-53` unlock both.
(c) Vulkan: CPU drain of all in-flight GPU work (`vkDeviceWaitIdle` / a fence set).
(d) Callers: `0x1478d0, 0x146770, 0x25ddd0, 0x3c5a00, 0x25c080`. Callees: `0x147f40, 0x3b450, 0x3b4a0, 0x1480f0`. Size 362 B.

### 5.7 `0x35480` — JobSystem_KickJobArray, 261 B

(a) `param_1` = job system (`+0x8` = queue object with `+0xb80` mutex, `+0xac0` free-list, `+0xb60` slot array). `param_3` = count, `param_6` = priority, `param_7` = timeout (-1 = default `+0x3c`).
(b) `:18` lock `queue+0xb80`; `:23` `FUN_00035db0(queue+0xac0)` grab a slot (stride 0x9b8); `:30-31` store `param_6` at `slot+0x9a0`, `param_7` at `slot+0x998`; `:35` `FUN_00034e60(param_1, slot, param_2, param_3)` (copy job descriptors); `:36` `FUN_00035250(param_1, slot, param_4)` (dispatch/notify).
(c) Vulkan: dispatch a host job (command-buffer recording) to a worker pool.
(d) Callers: many (`0x14b800, 0xa67480, 0xa3f140, 0x563ad0, 0xa433d0, 0x8ef20, …`). Callees: `0x35db0, 0x34e60, 0x35250`. Size 261 B.

### 5.8 `0x35730` — JobSystem_WaitAndFreeCounter, 40 B

(a) `param_1` = job system, `param_2` = pointer to a counter/handle.
(b) `:7` `FUN_00035590(param_1, *param_2, param_3, param_4, param_5)` (wait until the job completes); `:8` `FUN_00034bd0(param_1, param_2)` (release the slot: clears the bit in the bitmap at `queue+0xc60`, `00034bd0.c:52-55`).
(c) Vulkan: wait on and release a submit/timeline slot.
(d) Callers: many. Size 40 B.

## 6. GPU command emitters used by the above (exact packets, proven)

| Function | Header / reg | Effect | Vulkan |
|---|---|---|---|
| `0x86ced0` | `0xC0004600` + table[val] | EVENT_WRITE 0x46 | event / marker |
| `0x869a50` | `0xC0016900`, reg `0x1e0+i` | CB_BLEND i control | blend state |
| `0x869990` | `0xC0016900`, reg `0x8e` | CB_TARGET_MASK | color write mask |
| `0x869ce0` | `0xC0016900`, reg `0x202` | CB_COLOR_CONTROL | blend/logic op |
| `0x869d60` | `0xC0016900`, reg `0x200` | DB_DEPTH_CONTROL | depth test/write |
| `0x869dc0` | `0xC0016900`, reg `0x10b` | DB_STENCIL_CONTROL | stencil op |
| `0x869bb0` | `0xC0026900`, reg `0x10c/0x10d` | DB_STENCILREFMASK | stencil ref |
| `0x869f50` | `0xC0016900`, reg `0xb` | DB_DEPTH_CLEAR | depth clear value |
| `0x86dd20` | `0xC0016900`, reg `0x0` | DB_RENDER_CONTROL | depth render ctl |
| `0x86be10` | `0xC0016900`, reg `0x3` | DB_RENDER_OVERRIDE | depth override |
| `0x86e1a0` | `0xC0016900`, reg `0x205` | PA_SU_SC_MODE_CNTL | cull/poly mode |
| `0x86e3a0` | `0xC0016900`, reg `0x2de` | PA_SU_POLY_OFFSET_DB_FMT_CNTL | depth-bias fmt |
| `0x86e410` | `0xC0026900`, reg `0x2e0` | PA_SU_POLY_OFFSET (scale/offset) | depth bias |
| `0x86e480` | `0xC0026900`, reg `0x2e2` | PA_SU_POLY_OFFSET (back) | depth bias |
| `0x86a290` | `0xC0016900`/`0xC0086900`, regs `0xf/2/5/0x2af` | depth-target block | depth attachment |
| `0x86a1a0` | `0xC00E6900`, reg `0x318+0xf*n` | CB_COLORn block (15 dw) | colour attachment |
| `0x86a900` | `0xC0017900`, reg `0x242` | VGT_PRIMITIVE_TYPE | `VkPrimitiveTopology` |
| `0x86bed0` | `0xC0002a00`, reg `0x2a` | INDEX_TYPE | `VkIndexType` |
| `0x86bf50` | `0xC0012600` | INDEX_BASE | index buffer addr |
| `0x86c000` | `0xC0002f00` | IT_NUM_INSTANCES | instance count |
| `0x86d160` | `0xC0016900`, reg `0x2d5` | draw-kind | pipeline select |
| `0x86c5f0` | `0xC0016900` reg 0x290=0; `0xC0016900` reg 0x2e5=0 | VGT prim-reset + PA_SC reset | primitive restart reset |
| `0x86d7b0` | `0xC0026900`/`0xC0066900`, regs `0xb4+2i`, `0x10f+6i` | PA_SC_VPORT (viewport) | `VkViewport` |
| `0x86d950` | `0xC0026900`, reg `0xc/0xd` | PA_SC_SCREEN_SCISSOR | `VkRect2D` |
| `0x86a010` | `0xC0016900`, reg `0x8d` | PA_SC_WINDOW_SCISSOR | `VkRect2D` |
| `0x86db40` | `0xC0016900`, reg `0x206` | PA_CL_VTE_CNTL | (no direct Vk) |
| `0x86a090` | `0xC0046900`, reg `0x2fa` | PA_CL_VPORT_Z (depth range) | viewport min/max depth |
| `0x86d4c0` | `0xC0027600`, reg `-0x2c00+DAT_01290410[stage]` | 64-bit user SGPR | push constant / UBO |
| `0x86d5e0` | `0xC0001000`+`0xC0007600`, reg `-0x2c00+table[stage]` | N-dword user SGPR | push constant / UBO |
| `0x86d210` | `0xC0001000`+`0x6875xxxx` | allocate an inline data slot, return its VA | — (host buffer) |
| `0x86eb60` | `0xC0044700` | EVENT_WRITE_EOP 0x47 (label write) | timeline semaphore / `vkCmdWriteTimestamp` |
| `0x86edc0` | `0xC0053c00` | WAIT_REG_MEM 0x3c | `vkCmdWaitEvents` / barrier |
| `0x86ac10` | (import) `sceGnmDrawIndexAuto` | DRAW_INDEX_AUTO 0x2d | `vkCmdDraw` |
| `0x86b040` | (import) `sceGnmDrawIndexOffset` | DRAW_INDEX_OFFSET_2 | `vkCmdDrawIndexed` |
| `0x86b150` | (import) `sceGnmDrawIndirect` | DRAW_INDIRECT 0x24 | `vkCmdDrawIndirect` |
| `0x86b3d0` | (import) `sceGnmDrawIndexIndirect` | DRAW_INDEX_INDIRECT 0x25 | `vkCmdDrawIndexedIndirect` |
| `0x86b9e0` | (import) `sceGnmDispatchDirect` | DISPATCH_DIRECT 0x15 | `vkCmdDispatch` |
| `0x86f700` | (import) `sceGnmInsertWaitFlipDone` | wait-flip-done marker | present wait |

## 7. Render-job draw chain (caller/callee, proven)

```
0x11d3d0 (per-group draw loop, 464 B)          callers: 0xc1860, 0xc2fa0, 0xc4de0, 0xc4f80, 0xc3320, 0xc3ec0, 0xce850, 0xcd330, 0xc1ce0, 0xc3820, 0xc3a50, 0xc1fc0
  per 0x20-byte draw record:
    R = 0x14d7b0(record[1])                     (lazy material resolve)
    if R changed: 0x16b850(R+0x38,pass,4); 0xf8c90(R,pass)
    0x123cc0(record[2], pass, record+0x14, record[5]!=0)   -> 0x123810 (instance block)
    T = 0x95320(u32(R+0x468), pass+0x90, DAT_02ed1f54)      (root table)
    0xae150(pass[0], T)                                     (user-data bind)
    0x1222e0(record[1], pass)                               (draw)
  -> 0x1222e0 (84 B): if record+0x28==0 -> 0x189060(...) else 0x128d20(record+0x20, pass)
       -> 0x128d20 (37 B): 0x94d00(*pass, record+0x38, record+0x40, record+0x48)   [DrawIndexOffset]
```
- `0x94d00` → `0x86b040` (DrawIndexOffset). `0x94dd0` → `0x86b040` + `0x86c000`.
- The material bind `0xf8c90` → `0xf8990` (per-slot resolver, `pass-map.md` §3) sets `pass+0xb0/+0xb8` and binds records `rec0`/`rec4` via `0x937c0`/`0x94ab0`.
- `0x937c0` (124 B) binds a VS/PS/GS/ES record by `pass[4]+0` (stage idx 2/6/4 → `0x861520`/`0x861260`/`0x860bb0`).
- `0x94ab0` (300 B) binds by `pass[4]+0x10` via `0x861600`, and when `record[1]+0x38 != 0` builds `SPI_PS_INPUT_CNTL` words and calls `0x86bc00` (SET_CONTEXT_REG block reg 0x191+n).

## 8. Gaps I could not close

1. **AI memory not queried** — the `mcp__ai-memory__*` tools are not in the subagent's tool set, so prior work came only from the on-disk docs.
2. **0x95320 return value** — the decompile is `void`; `R-08` says `0x11d3d0` consumes a returned pointer. The table address is found by content, not by `rax`. Unresolved (matches `R-08` open item 5).
3. **`FUN_00093840` (0x93960:18)** — the pass outer setup called before everything else; not decoded here.
4. **Exact EVENT_WRITE payload semantics of `0x86ced0`** — header `0xC0004600` is proven; whether it is an EVENT_WRITE (0x46) with the table value as control, or another type-3 packet, is inferred.
5. **`DAT_00f9f2e0` contents** — the primitive table indexed by draw mode; the table bytes were not dumped (matches `render-states.md`).
6. **Draw-kind values 0/0x45/0xad/0xb0/0x145/0x1ad** — the selection logic is proven, but which pass/draw sets each value on `cb+0xc548` beyond the `=0` writes in the pass functions is not traced.
7. **`0x93690` shader-list `DAT_02ee3ce0+0x8a8` identity** — proven as the engine shader list, identity (`.cslt`) inferred per `pass-map.md` §5.
8. **Stage-index ↔ shader-type mapping** — the `0x86d4c0/d5e0` stage number (0..6) is proven as a field; the VS/PS/GS/ES/LS/HS labelling is inferred from the setter functions (`0x86c170` = VS, `0x86c050` = PS, `0x86c2c0` = ES, `0x86cb00` = GS, `0x86c760` = LS, `0x86c330` = HS) and the `import_sceGnm*` names.
9. **0x118470 `param_1` list strides** — the HUD-list loop strides (0x28 and 0x20) are proven; the list element type is not named.
