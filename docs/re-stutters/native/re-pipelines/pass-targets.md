# Render targets per pass: formats, colour count, depth, samples (RE, topic K1)

Read-only static analysis of the EU 1.01 eboot (Ghidra export `out/re/decomp/<hi16>/<addr>.c`, first line of each
file lists callers; string data from `out/re/ghidra/eboot_ghidra.elf`). Addresses are eboot VAs. Builds on
`pass-map.md` (R8), `render-states.md` (R2) and `materials-meshes.md` (R3).

Labels: **proven** (decomp read, address given), **inferred** (reasoning, not checked against game data),
**unknown**. No game data was read; the only runtime data used is the port's own capture
`user/shader_predict/pass_layouts.txt`.

## 1. Answer in short

1. **The pass-slot k is not written by constructors.** Every writer of `pass+0xf4` that I opened is a *pass runner*:
   it sets k, then calls a dispatcher (`FUN_0011d3d0`, `FUN_001a8c60`, ...) for one sub-draw. The runners are
   named by eboot strings (`DepthPrePass`, `GBuffer`, `GBufferAmbientBaked`, `Decal`, `Standard`, `Water`, ...).
   Table in section 2. **proven** (writes and strings); correction to R8 section 2.1, which said "constructors".
2. **A pass draws into a fixed target set of 10 qwords** (8 colour slots, 1 depth owner, 1 flag qword). The set is
   read by `FUN_00092140` (MRT bind) and the depth block by `FUN_0086a290`. The sets live in the render-target
   manager at fixed offsets (`+0x29a0`, `+0x2a40`, `+0x2a88`, `+0x1fb8`, `+0x1ff8`, `+0x2000`, section 3).
   **proven** for the reads; the fill of the set slots was not found (open).
3. **Colour target = 64-byte descriptor** (8 qwords) at target `+0xe4` (alternate copy `+0x124`), written as the 15-dword
   `CB_COLORn` block at `0xA318 + 0xF*n`. Layout in section 4. **proven** (packet layout in `0x86a1a0`).
4. **Depth target = 12-dword block** at target `+0x164`, written as `DB_Z_INFO` (0xA010) ... `DB_STENCIL_WRITE_BASE`
   (0xA015) and `DB_DEPTH_VIEW`/`DB_HTILE`/`DB_DEPTH_INFO`. Section 5. **proven**
5. **Target objects are created once, in `FUN_0009aef0`** from a static list of roughly 90 `"kRTT_*"` names (section 6),
   via `FUN_00097a50` / `FUN_0008d6c0` / `FUN_0008d6f0`. The format of each is an engine format id (0..27) looked up
   in a table `DAT_02ed3680` that `FUN_00099350` writes at run time. **proven** (creation and table code).
6. **The target set is fixed per session** for the sizes and formats: they are constants and two size modes
   (base console and Neo, `sceKernelIsNeoMode`). Which set slot holds which `kRTT` object is decided by code
   (not by data). The trigger of the creation path is `FUN_00533100` -> `FUN_002569d0` -> `FUN_00095a90` ->
   `FUN_0009aef0`; I did not decode `FUN_00533100`, so whether this runs once per session or per area is **unknown**.
7. **`pass_layouts.txt` does correspond to the targets, but it is not the target set.** Each of its 68 rows is the
   pixel-shader export layout (`PsColorBuffer` x 8, `mrtz_mask`, `num_samples`) of one PS program, captured from the
   live draw state, keyed by the pass name of that program. The GBuffer family rows match the `kRTT` formats of
   the GBuffer target group exactly (section 7). Depth formats are not in these rows. **proven (format match),
   inferred (name mapping)**

## 2. Pass runners and their slot k

Runner = function that writes `pass+0xf4` and then calls a dispatcher. `pass` is the `param_1` of the runner.
Scope names are the strings passed to `FUN_0016b8c0` / `FUN_0016bb70` (profiler scope); the `k` column lists the
values written, in code order. Mode values are the 4th argument of `FUN_0011d3d0` (bit index of the pass-flag word
`pass+0xf8`, bits 1, 2, 4, 8, 0x10 -> modes 0..4). **proven** unless marked.

| Runner (eboot VA) | Scope string(s) | k written | Notes |
|---|---|---|---|
| `0xc1860` (`000c/000c1860.c`) | `DepthPrePass` (string at `DAT_02ed3d98`) | 1 (flag 1 and 2), 10 (flag 8), 12 (flag 0x10) | reads the flag word at `pass+0xf8` (bits 1, 2, 4, 8, 0x10); flag 4 writes no k. Target set `view+0x29a0` (section 3) |
| `0xc1ce0` (`000c/000c1ce0.c`) | `RenderGBufferBakedPerVertex`; sub-scope `GBuffer` (`DAT_02ed3db0`, no k write seen); sub-scope `GBufferAmbientBaked` (`DAT_02ed3dc8`) | in the GBufferAmbientBaked scope: 3 (mode 6), then 2 (mode 5) | reads `view+0x2a40` (line 49) and calls `FUN_00092140` (line 59). The k=3 inside the Baked scope is why k is not a name key (section 2, XPR note) |
| `0xc1fc0` (`000c/000c1fc0.c`) | `RenderGBufferBakedPerProbe`; `GBuffer` (`DAT_02ed3de0`); `GBufferAmbientDynamic` (`DAT_02ed3df8`) | 5 (mode 8), 4 (mode 7) inside the GBufferAmbientDynamic scope | uses `view+0x2a40`; then `FUN_00093d70` and two `FUN_00093960` calls on `view+0x1fb8`, `+0x1ff8`, `+0x2000` |
| `0xc3a50` (`000c/000c3a50.c`) | `Depth` (scope), `DepthPrePass` (string `DAT_02ed3e88`) | 1 | second runner for the depth pass |
| `0xc28c0` (`000c/000c28c0.c`) | `Decal` | 6 (two writes) | |
| `0xc2710` (`000c/000c2710.c`) | `RenderFur`, `RenderStrands`, `RenderShells`, `Stats` | 7 | fur; also `0x183d00`, `0x183d10` (same code, folder 0018) |
| `0xc2fa0` (`000c/000c2fa0.c`) | `SkyboxOpaque` | 7 | |
| `0xc3320` (`000c/000c3320.c`) | `SkyboxAlpha` | 8 | |
| `0xc3600` (`000c/000c3600.c`) | `Standard` | 7 | |
| `0xc3820` (`000c/000c3820.c`) | `Water` | 7 or 2 (`(pass+0xfc & 1) == 0 ? 7 : 2`) | |
| `0xc3c70` (`000c/000c3c70.c`) | `Alpha`, `Transparent` | 8 | |
| `0xc3ec0` (`000c/000c3ec0.c`) | `TransparentDepth` | 3 | conditional |
| `0xc4de0` (`000c/000c4de0.c`) | `SpecialScreenFX` | 8 | |
| `0xc4f80` (`000c/000c4f80.c`) | `EditorZFail` | 9 | |
| `0xc46c0` (`000c/000c46c0.c`) | `Debug` | 0xb | |
| `0xc4a50` (`000c/000c4a50.c`) | `DebugCollision`, `Debug` | 7, 10, 0xb | |
| `0xcaed0` (`000c/000caed0.c`, the main-camera builder) | `PreDebug`, `Debug`, `ForwardDecal` | 1 (line 205) and others (not all read) | reads `view+0x29a0` (line 207); the other runners it calls were not traced |
| `0xca6d0` (`000c/000ca6d0.c`) | (called from `0xcefa0`, `0xccdf0` `RenderCubeMapFace`) | 1 | |
| `0xc6f70` (`000c/000c6f70.c`) | `XPRRunFragmentCullShaderMainScene` | 1 | cull dispatch, not a shading pass (inferred) |
| `0xcd330`, `0xce850`, `0xcd7a0` | none (called from `0xcdbc0`, `0xcdcc0` `RenderSunShadowProxy`) | 0 and 0xd (`0xcd330`, `0xce850`); 0 (`0xcd7a0`) | shadow proxy, name inferred |
| `0x11d040`, `0x11d140`, `0x118470` | none | 7 | |
| `0x14c8a0`, `0x14c9a0`, `0x146ea0` | none | 0xb | |
| `0x1809a0` | none | 1 then 6 | |
| `0x1a8c60` (`001a/001a8c60.c`) | none (callers `0x1a8a40`, `0x1a8dd0`) | 1 then 6 | one material item drawn twice (R8 section 2.1) |

Also written by a copy and not a runner: `0xc9e50` copies 8-byte pieces at `+0xdc`..`+0xf4` from a source object
(`pass+0xe4` descriptor and `+0xf4`), called from `0xc95c0` (inferred: pass-object copy).

**The XPR name table.** `FUN_000ca3e0` (`000c/000ca3e0.c`) writes a table of names with two ints each:
`XPR_kSMLT_Depth` (1, 0), `XPR_kSMLT_DepthFadeOut` (4, 0), `XPR_kSMLT_DepthFadeOutInvLODFade` (5, 0),
`XPR_kSMLT_GBufferAmbientBaked` (2, 0), `XPR_kSMLT_GBufferAmbientDynamic` (3, 0), `XPR_kSMLT_Debug` (6, 0),
`XPR_kSMLT_Shadow` (0, 0..7), `XPR_kSMLT_ShadowFadeOut` (7, 0..7). **proven** (table), **the meaning of the two
ints is unresolved**: the first int matches the runner k for Depth (1) and GBufferAmbientBaked (2), but
GBufferAmbientDynamic is 3 here while the runner `0xc1fc0` writes 4 and 5 in that scope, and `0xc1ce0` writes
k=3 inside the GBufferAmbientBaked scope. So this table is not a plain k -> name map; treat it as a name list with
a sub-key, and do not key the port on the scope names alone.

Name note: the table spells `GBufferAmbientBaked`; the resource stem in `pass_layouts.txt` is
`gbufferbakedambient` (word order differs). Map by the stem, not by a case-folded copy of the string.

## 3. The render-target set (10 qwords) and its consumers

`FUN_00092140(ctx, set)` (`0009/00092140.c`, 924 bytes; callers include `0xb9710`, `0xbb840`, `0xcaed0`,
`0xc1ce0`, `0xc1fc0`, `0xc4de0`, `0x118470`, ...). `set` is a pointer to 10 qwords:

```c
struct RtSet {                 // 80 bytes
  void* slot[8];               // +0x00..+0x38  colour target objects (MRT 0..7), or 0
  void* depth;                 // +0x40        depth owner object (depth block at +0x164), or 0
  u64   flags;                 // +0x48        bit 0: use the alternate descriptor (+0x124 instead of +0xe4)
                               //              other bits are read by 0x92140 when it updates a context dirty
                               //              word (not decoded)
};
```

Behaviour (**proven** from the decomp):
- A slot is used only if `slot->type_byte` (`+0x62`) satisfies `(type | 2) == 3`, i.e. type 1 or 3.
- Slot i uses descriptor `slot+0xe4` or `slot+0x124` (flag bit 0), and calls `FUN_0085cbc0(ctx, i, desc)`
  which writes CB_COLORi (section 4). A null slot clears CB_COLORi (`0x86a1a0(cb, i, 0)`, which writes INFO = 0).
- The depth owner's block `depth+0x164` is written by `FUN_0086a290` (section 5), with `FUN_0086e3a0` (writes
  `0x2de`, a table value from `DAT_0128fee0`, inferred to be the depth/AA mode) and `FUN_00869f50`.
- Viewport and scissor size come from the first valid target: `+0x198` (width) and `+0x19c` (height)
  (`FUN_00096f50` / `FUN_00096f60`, **proven**).

**Where the sets sit.** The runners read the set from the render-target manager `view = pass+0x18`
(`param_1[3]`; **inferred**: the same object as the one `FUN_0009aef0` fills, see below):
- `DepthPrePass` `0xc1860`: `view+0x29a0`.
- `GBuffer*` `0xc1ce0`, `0xc1fc0`: `view+0x2a40`.
- `0xcaed0`, `0xc6f70`: `view+0x29a0`; `0xc59d0`: `+0x29a0`, `+0x2a40`, `+0x2a88`.
- Shadow passes: `view+0x1fb8`, `+0x1ff8`, `+0x2000` (`FUN_00093d70`, `FUN_00093960`).

`FUN_0009aef0` (19 KB) starts by zeroing 32-byte blocks in its manager object at `+0x2950`..`+0x2b0c` (a run of
stores of the same zero vector `auVar47`; lines 285-300 of `0009/0009aef0.c` are the ones I read). The sets appear to be 0x50 bytes apart (`+0x29a0`, `+0x29f0`,
**`+0x2a40`**, `+0x2a90`) (**inferred** from the zeroing stride). **Open:** the code that stores target pointers
into these sets was not found; so the mapping from set slot to `kRTT` object is not yet proven.

## 4. Colour target: descriptor and CB_COLORn writes

**Descriptor** (64 bytes = 8 qwords; `0x85cbc0` / `0x86a1a0`, **proven** for the packet; field names from the
port's `gpu/shadps4/video_core/amdgpu/regs.h` struct `ColorBuffer`, which has 15 dwords, stride `0xF`):

```c
struct ColorDesc {               // target+0xe4 (or +0x124), 64 bytes
  u32 base;        // d0.lo  CB_COLORn_BASE   (reg 0x318 + 15*n)   addr >> 8
  u32 pitch;       // d0.hi  CB_COLORn_PITCH  (0x319)              tile_max bits 0..10
  u32 slice;       // d1.lo  CB_COLORn_SLICE  (0x31A)
  u32 view;        // d1.hi  CB_COLORn_VIEW   (0x31B)              slice_start 0..10, slice_max 13..23
  u32 info;        // d2.lo  CB_COLORn_INFO   (0x31C)              endian 0..1, format 2..6, linear_general 7,
                   //                                               number_type 8..10, comp_swap 11..12,
                   //                                               fast_clear 13, compression 14, blend_clamp 15,
                   //                                               blend_bypass 16, simple_float 17, round_mode 18,
                   //                                               cmask_is_linear 19 (regs_color.h)
  u32 attrib;      // d2.hi  CB_COLORn_ATTRIB (0x31D)              tile_mode 0..4, fmask_tile_mode 5..9,
                   //                                               fmask_bank_height 10..11, num_samples_log2 12..14,
                   //                                               num_fragments_log2 15..16, force_dst_alpha_1 17
  u32 dcc_ctl;     // d3.lo  0x31E (named pad0 in the port; DCC control, inferred)
  u32 cmask_base;  // d3.hi  0x31F
  u32 cmask_slice; // d4.lo  0x320
  u32 fmask_base;  // d4.hi  0x321
  u32 fmask_slice; // d5.lo  0x322
  u32 clear_word0; // d5.hi  0x323
  u32 clear_word1; // d6.lo  0x324
  u32 pad;         // d6.hi  0x325 (not in the packet's register list of the port)
  u32 unused14;    // d7.lo  (not written)
  u32 nop_payload;// d7.hi  copied into a trailing 2-dword 0xC0001000 record (opcode 0x10, inferred NOP)
};
```

**Packets** (`0x86a1a0`, `out/re/decomp/0086/0086a1a0.c`):
- Enabled target (`FUN_0087eda0(desc) != 0`, inferred: nonzero masked base): one SET_CONTEXT_REG packet
  `0xC00E6900` (header, offset, 14 values = 16 dwords) at register `0x318 + 0xF*n`, then a 2-dword `0xC0001000`
  record carrying `d7.hi`. Total 18 dwords.
- Disabled target: `0xC0016900` writing `0` to `CB_COLORn_INFO` (3 dwords).

**Validity and format helpers** (**proven** by decode, meaning partly inferred):
- `FUN_0087eda0` (`0087/0087eda0.c`) takes the descriptor and returns its base dword with low bits masked by the
  tile and format alignment (`FUN_00866710`, `FUN_00885f70`); `0x86a1a0` and `0x85cbc0` use a nonzero result as
  "target bound". `FUN_0087dea0` (`0087/0087dea0.c`) writes the format word into the descriptor; it range-checks
  `(w & 0xff) - 1 < 0x16`, so the low byte of the format word is an AMD data format (inferred from the check).
- `FUN_0087f0d0` / `FUN_0087efd0` do the same masking for the FMASK slot (dword 9 of the descriptor, inferred).

**Sample count.** For colour, `NumSamples = 1 << num_fragments_log2` (ATTRIB bits 15..16, `regs_color.h`
`NumSamples()`). For depth, `1 << z_info.num_samples` (Z_INFO bits 2..3). **proven** (port headers). The values for
each target are not decoded statically (inferred: 1x everywhere, see section 7).

## 5. Depth target: block and DB writes

Block at `target+0x164` (**proven** from `0x86a290`, `0x96f70`, `0x97520`). The packet writer `FUN_0086a290` (307 bytes,
callers `0x92140`, `0x93ef0`, `0x94600`) reads it as `undefined8 *P`:

| Byte offset from P | Dword | Register (dword index / VA) | Meaning (port `regs.h`, **proven** for the names given) |
|---|---|---|---|
| 0x00 | 0 | DB_Z_INFO `0x10` (VA 0x28040) | `z_info.format` bits 0..1 (`Z16`=1, `Z32Float`=3), `num_samples` bits 2..3 |
| 0x04 | 1 | DB_STENCIL_INFO `0x11` (0x28044) | `stencil_info.format` bit 0 (Stencil8=1) |
| 0x08 | 2 | DB_Z_READ_BASE `0x12` (0x28048) | `z_read_base` (addr >> 8) |
| 0x0c | 3 | DB_STENCIL_READ_BASE `0x13` (0x2804c) | |
| 0x10 | 4 | DB_Z_WRITE_BASE `0x14` (0x28050) | |
| 0x14 | 5 | DB_STENCIL_WRITE_BASE `0x15` (0x28054) | |
| 0x18 | 6 | DB_DEPTH_SIZE `0x16` (0x28058) | pitch and height tile max |
| 0x1c | 7 | DB_DEPTH_SLICE `0x17` (0x2805c) | |
| 0x20 | 8 | DB_DEPTH_VIEW `0x02` (0x28008) | written by a separate `0xC0016900` |
| 0x24 | 9 | DB_HTILE_DATA_BASE `0x05` (0x28014) | |
| 0x28 | 10 | reg `0x2AF` (VA 0x28abc) | name not in the port headers (unknown) |
| 0x2c | 11 | DB_DEPTH_INFO `0x0F` (VA 0x2803c) | the port's `regs.h` has this as padding; name from the engine packet (inferred from the register number) |
| 0x30 | 12 | (NOP payload, `0xC0001000`) | copy of dword 6 (inferred) |

The register indices are checked against `regs.h`: `depth_render_control` at `0xA000`, `depth_view` `+2`,
`depth_render_override` `+3`, `depth_htile_data_base` `+5`, `depth_clear` `+0xB`, `depth_buffer` at `+0x10`
(DepthBuffer struct: z_info, stencil_info, z_read_base, stencil_read_base, z_write_base, stencil_write_base,
depth_size, depth_slice). **proven** (struct order matches the packet).

**No depth** (`FUN_00868710` and `FUN_00868ca0` both zero): the same function writes `DB_Z_INFO = 0` and
`DB_STENCIL_INFO = 0` (`0xC0016900` x2, **proven**).

**Depth block builders** (where the 12 dwords are filled): `FUN_00096f70` (1447 bytes, callers `0x9fa90`,
`0x95a90`) copies `+0xe4` to `+0x124`, applies the format with `FUN_0087dea0`, and reads/writes the depth block at
`+0x164` (**proven** for the copy and the `+0x164` accesses; the depth-fill details are inferred). `FUN_00097520`
(`0x97520`) writes `+0x164` through `FUN_00882420` and `FUN_00867990` (**inferred**: the depth path is taken when the
target type byte is 1 or 3). `FUN_0087dea0` stores the format into `+0x124` / `+0xe4`.

## 6. Creation: how the target objects are built, and when

Chain (**proven** call edges):
`0x533100` (3961 bytes, one caller of `0x2569d0`; trigger not decoded) -> `FUN_002569d0`
(`AreaLoad_ShaderListAndAssets` per R3, 12626 bytes) -> `FUN_00095a90` (987 bytes; line 633 of `0x2569d0`) ->
`FUN_0009aef0` (19353 bytes, the list of `kRTT_*` objects).

**Sizes.** `0x95a90` builds a size table on the stack and passes it to `0x9aef0`:
- `1600 x 900` (`0x640 x 0x384`) and `1920 x 1080` (`0x780 x 0x438`) pairs; plus `sceKernelIsNeoMode()` selects
  `2560 x 1440` (`0xa00 x 0x5a0`) for the Neo pair, else `1920 x 1080`. **proven** (constants and the call);
  which pair each `kRTT` uses is **inferred** from the `FUN_0009aef0` size arguments.
- The display buffer is created with `FUN_00097a50(w, h, 1,1,1, 0x19, 3, 0, "DisplayBuffer", ...)` in `0x95a90`
  (line 104-122 of `0009/00095a90.c`), and `0x96f70(display, 0, 0x780, 0x438, 1)`.

**Creation calls.** `FUN_00097a50(w, h, p3, p4, p5, p6, p7, p8, name, out)` (2834 bytes):
- `p6` = engine format id (0..27), indexes the table `DAT_02ed3680` (`0x97a50` lines 149-150,
  `uVar20 = tab[p6*2]`, `iVar11 = tab[p6*2 + srgb]`, where `srgb` is `puVar22[1]`, an sRGB selector by its use
  at object `+0x64` (inferred)).
- `p7` = kind: 1 colour (`FUN_0008d6c0` passes 1), 2 depth (`FUN_0008d6f0` passes 2), 0 other (direct calls).
- `p8` = 0 in all calls I read (samples or tile kind, unknown); `p5` = 1 in all `FUN_0008d6c0` / `FUN_0008d6f0`
  calls (mip count or layer count, inferred).
- Output object: 0x1c0 bytes, vtable `PTR_FUN_015977d0`, size at `+0x198`/`+0x19c`, desc at `+0xe4`,
  block `+0x164` (see `0x97520`). **proven** for the field writes.

**Format table `DAT_02ed3680`.** Written at run time by `FUN_00099350` (`0009/00099350.c`, 288 bytes); the eboot
file has zeros there (bss), so the table must be read from that initializer, not from the file. Each entry is 8
bytes: dword 0 = linear variant, dword 1 = sRGB variant. Entries used by the `kRTT_*` list (decoded by me; the
low byte is the AMD `DataFormat`, bits 8..11 the `NumberFormat`, the rest unresolved):

| id (p6) | Linear (dword 0) | sRGB (dword 1) | Used by (`0x9aef0` lines) |
|---|---|---|---|
| 0 | 8_8_8_8 UNORM | 8_8_8_8 SRGB | Display, SSAO_Accumulation_A/B, AlphaDofAmount, SSAO, SunShadowResolve, UI_Offscreen_0/1/Fonts, UI_Onscreen |
| 1 | 16_16_16_16 FLOAT | same | MainColor, PostProcessFinal, RenderTargetDisplay, TemporalAA_Color_Out_A/B, FogScatter0/1, FogRaymarch, FogMedia, Lighting, LightingDiffuse, PostColorHalfRes_A/B, DOFPrefilterNear/Far, DOFNearField, DOFFarField, DOFResult, FullscreenBlur_0/1 |
| 3 | 32 FLOAT | same | LinearDepthMipped, LinearNearDepth, OccluderDepthTextureMipped, NAdvOccluderDepthTextureMipped |
| 4 | 32 UINT | same | Normal_Roughness, Albedo_AmbientOcclusion, PBRProperties, ExtraPBRProperties, Ambient1, MotionBlur_Vel/MagDepth/Tile_*, FogMediaCull |
| 5 | 16 FLOAT | same | PerTileDepth_12x12 (and _Temp) |
| 9 | 32 FLOAT | same | MainDepth, MainDepth_HalfRes, CollisionDepth, OccluderDepth, SpotShadowArray, CascadedShadow (depth kind, p7 = 2) |
| 10 | 16 FLOAT | same | RainBlocker, ParticleBlocker (depth kind) |
| 0x12 (18) | 8 UNORM | | SpecularIBLOcclusion |
| 0x13 (19) | 2_10_10_10 UNORM | | AmbientShadowRG_SSAOB |
| 0x14 (20) | 16_16 FLOAT | | Motion |
| 0x15 (21) | 10_11_11 FLOAT | | Particle_LightmapA/B, MainColorCopy, SpecularIBL |
| 0x18 (24) | 32_32 UINT | | Ambient0, ClusterInfo |
| 0x1a (26) | 8_8 UNORM | | DOFAmount, DOFAmountTile_8x8, DOFAmountTile_Field_16x16 |

Other ids in the table (2, 6, 7, 8, 11..17, 22, 23, 25, 27) are not used by the `kRTT` names I read. **proven**
for the used rows; the NumberFormat of entry 25 (`0xe`) and the bits 12..23 are unresolved.

**Descriptor fill for a target.** `FUN_00097520` (1319 bytes, `0009/00097520.c`) and `FUN_00096f70` build the
descriptor: when the template flag `local_58 & 4` is clear, they copy `+0xe4..+0x11c` from a source object
(`param_1+0xe4`..), then apply the format word with `FUN_0087dea0(desc, tab[p6*2 + srgb])` when
`local_58 & 1`, and pack pitch and slice bits into `+0x78` with `local_58 & 10`. **proven** for the copies and the
format call; the template object (`param_1`) is the `kRTT` spec built inside `0x9aef0`; the template values are not
decoded (open).

## 7. Compare with the port's pixel layouts (`user/shader_predict/pass_layouts.txt`)

**How the file is made** (`gpu/sotc/predict_probe.cpp`, **proven**): `OnCachedProgram` (line ~750) and the
pipeline-capture path (line ~555) call `AddLayout(pass, LayoutOf(spec))`. `LayoutOf` copies
`spec.runtime_info.hw.fs` (`color_buffers[8]`, `mrtz_mask`, `num_samples`) of a fragment program. The pass name comes
from `predict_table.tsv` by the program's hash. So each row is: **one fragment program's export layout as seen at
the draw**, not the bound target set. The struct `PsColorBuffer` (`runtime_info.h:269`) is 8 bytes: `data_format`
(6 bits), `num_format` (4), `num_conversion` (3), `export_format` (4), `blend_self_scale` (1), then the swizzle.
`FsLayout` is 68 bytes (header `FSLAYOUT 68`).

**Decoded** (my script over the file; 68 rows, 49 pass names): 48 rows have `num_samples = 1`, 20 rows have 0.
33 rows have no colour target; 20 of those have `num_samples = 0` (fields not set, or no export; unknown).

Rows that match the GBuffer target group (`gbufferbakedambient`, `gbufferdynamicambient`,
`gbufferbakedambient_motion`, `gbufferdynamicambient_motion`, `psfurrender`):

| colour slot | layout (DataFormat / NumberFormat / export) | candidate target (section 6 table) |
|---|---|---|
| c0 | 32 UINT / R32 | Normal_Roughness (id 4) |
| c1 | 32 UINT / R32 | Albedo_AmbientOcclusion (id 4) |
| c2 | 32 UINT / R32 | PBRProperties (id 4) |
| c3 | 32 UINT / R32 | ExtraPBRProperties (id 4) |
| c4 | 16_16 FLOAT / ABGR_FP16 | Motion (id 0x14) |
| c5 | 32_32 UINT / GR32 | Ambient0 (id 0x18) |
| c6 | 32 UINT / R32 | Ambient1 (id 4) |

This is an exact format match for seven slots (**proven** format, **inferred** slot names). `mrtz_mask = 0` and
the depth is not in the row.

Other rows (examples): `standard` c0 16_16_16_16 F, c1 16_16 F, c2/c3 32 UINT, c4 8 UNORM, c5 32_32 UINT, c6 32 UINT;
`alpha` c0 16_16_16_16 F, c1 16_16 F, c2 8_8_8_8 UNORM (blend targets); `main_ps` c0 16_16_16_16 F, c1 8_8_8_8 UNORM;
`fog_ps` eight 16_16_16_16 F targets; depth-only rows (`depth`, `depthfadeout`, `depthfadeoutinvlodfade`,
`shadow`, `shadowfadeout`, `decal`) have no colour target. `mrtz_mask = 1` appears in the `ps_main` rows and
`ps_main_depthfromalpha` (the shader exports depth; inferred from the mask name).

**Answer to item 4.** The layouts agree with the target formats of the same pass family, so they are a fair
sample of the real colour targets. They do **not** give the target set itself: (a) they list what the shader
exports, not what is bound (a bound slot the shader does not write is absent); (b) the depth format and the
depth block are not in them; (c) the `num_samples` field is 1 or 0, and 0 is not explained by the port (inferred:
unset). A pass with several shaders has several rows (union over programs).

## 8. Is the target set fixed for the game, and enumerable at boot?

- **Sizes and formats: fixed by code and by one runtime mode.** The `kRTT` list and the format table are constant
  code and data built at run time by `0x99350`. The only runtime input seen is the Neo test (`sceKernelIsNeoMode`),
  which changes two sizes. **proven** (constants), **inferred** (the set is the same for the whole session).
- **Set membership per pass: fixed in code, but the fill was not found.** The 10-qword sets are zeroed in
  `0x9aef0`, and the runners read them from `view+0x29a0` etc. Who fills them, and at what time (boot or area
  load), is **open**.
- **Re-creation on resolution change:** not seen. `0x95a90` replaces the manager global `DAT_02ed3670` when it is
  called again with a new object (the old one is freed by `FUN_00096010` / `FUN_000170b0`), so a second call would
  rebuild the set. The caller `0x533100` is the open part.
- **Enumerable at boot (inferred):** yes, in principle: the list of `kRTT_*` objects is fixed, and each pass runner
  has fixed k and fixed set offsets. The port could build, per pass, (colour formats[8], depth block, samples) from
  the format table and the sets once the sets are filled. It cannot read depth formats from the layouts file.

## 9. Port use (what the port could read at run time)

- Colour formats per pass: the port already gets them from draws (`LayoutOf`). The static route is:
  read the 10-qword set at `view+0x29a0` / `+0x2a40` after `0x9aef0` has run, then each slot's descriptor at
  `slot+0xe4` (INFO bits 2..6 and 8..10 give the format and number type), with sRGB from bit 0 of the flag qword.
- Depth per pass: read `depth+0x164` (dword 0 bits 0..1 = Z format, bits 2..3 = samples), once the set is filled.
- Enumerable at boot: only after `0x9aef0`. That runs in the `0x533100` path, so the port could hook the
  `FUN_0009aef0` return (`out/re/decomp/0009/0009aef0.c` return) and dump the sets, then key on `(pass, k)`.
- Do not key on `pass_layouts.txt` alone: it has no depth format and no set membership.

## 10. Open items

1. Find the code that stores target pointers into the sets (`view+0x29a0`, `+0x2a40`, ...). Likely in `0x9aef0`
   after its `kRTT` calls, or in the runner setup; not decoded.
2. Decode `0x533100` and `0x2569d0` triggers (per session or per area).
3. Confirm DB_DEPTH_INFO (0xF) name and value, and the register `0x2AF`.
4. Decode the `DAT_02ed3680` bits 12..23 and entry 25 NumberFormat `0xe`.
5. Decode `p8` of `FUN_00097a50` (samples or tile kind). Samples are assumed 1.
6. Map `FUN_00096f70`, `FUN_00097520` `local_58` flags to the sRGB and depth-kind decisions.
7. Runtime check (one run): log `pass+0xf4`, the 10-qword set, `slot+0xe4` INFO/ATTRIB and `depth+0x164` at the
   first draw of `DepthPrePass` and `GBufferAmbientDynamic`.

## 11. Evidence list

| Item | Address | File |
|---|---|---|
| Pass runners (k writes) | `0xc1860`, `0xc1ce0`, `0xc1fc0`, `0xc3a50`, `0xc28c0`, `0xc2710`, `0xc2fa0`, `0xc3320`, `0xc3600`, `0xc3820`, `0xc3c70`, `0xc3ec0`, `0xc4de0`, `0xc4f80`, `0xc46c0`, `0xc4a50`, `0xcaed0`, `0x1a8c60`, `0x11d3d0` (dispatch) | `out/re/decomp/000c/`, `001a/001a8c60.c`, `0011/0011d3d0.c` |
| XPR name table | `0xca3e0` | `out/re/decomp/000c/000ca3e0.c` |
| MRT bind from set | `0x92140` | `out/re/decomp/0009/00092140.c` |
| Per-pass bind (CB_COLOR0 and depth) | `0x93960`, `0x93840` | `out/re/decomp/0009/00093960.c`, `00093840.c` |
| CB_COLORn writer | `0x85cbc0`, `0x86a1a0` | `out/re/decomp/0085/0085cbc0.c`, `0086/0086a1a0.c` |
| Depth block writer | `0x86a290`, `0x86e3a0` | `out/re/decomp/0086/0086a290.c`, `0086e3a0.c` |
| Descriptor predicates | `0x87eda0`, `0x87f0d0`, `0x87efd0`, `0x87dea0`, `0x87eb10` | `out/re/decomp/0087/` |
| Depth validity | `0x868710`, `0x868ca0` | `out/re/decomp/0086/00868710.c` |
| Target builders | `0x96f70`, `0x97520`, `0x97a50` | `out/re/decomp/0009/` |
| Target creation list | `0x9aef0` (kRTT_* calls, lines 633-2107), `0x95a90` | `out/re/decomp/0009/0009aef0.c`, `00095a90.c` |
| Format table initializer | `0x99350` | `out/re/decomp/0009/00099350.c` |
| Area path caller | `0x2569d0` -> `0x95a90`; `0x533100` -> `0x2569d0` | `out/re/decomp/0025/002569d0.c`, `0053/00533100.c` |
| Port register layout | `ColorBuffer`, `DepthBuffer`, `AaConfig` | `gpu/shadps4/video_core/amdgpu/regs.h`, `regs_color.h`, `regs_depth.h`, `regs_primitive.h` |
| Port layout capture | `FsLayout`, `LayoutOf`, `AddLayout`, `OnCachedProgram` | `gpu/sotc/predict_probe.cpp`, `gpu/shadps4/shader_recompiler/runtime_info.h:269` |
| Layout data | 68 rows | `user/shader_predict/pass_layouts.txt` |
