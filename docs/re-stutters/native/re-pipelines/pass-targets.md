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

## Set fill (K5)

Topic: which kRTT target sits in which slot of each 10-qword render-target set, who writes the sets, and whether
they change per frame. Read-only static analysis of the same eboot export (`out/re/decomp`). Addresses are eboot VAs.
Manager `M` = the object passed as `param_1` to `FUN_0009aef0` (allocated with 0x4d88 bytes in `FUN_00095a90`,
`0x95a90` lines 136-142). Labels as in section 1.

### K5.1 Answer in short

1. **The sets are written by `FUN_000a0a60`** (`0xa0a60`, 1705 bytes), a list of plain 8-byte copies from the manager.
   It is called at the end of the creation function `0x9aef0` (line 2185) and by the mode-change function `0xa1cd0`
   (66 bytes, called by `0x95690`). **proven**
2. **Each set slot is resolved through a two-level table.** `FUN_0009fba0` (`0x9fba0`, 1756 bytes) copies a "primary"
   entry at `M+0x1b00+i` into a "resolved" entry at `M+0x1f80+i` (i = offset - 0x1f80). When the primary is null it
   falls back to the named `kRTT_*` field at `M+i` (K5.3). `0xa0a60` reads the resolved table. **proven** for the code;
   **inferred** that the fallback is what the sets hold in practice (K5.4 lists the one unchecked case).
3. **The sets are not written per frame.** They are rebuilt only when the view mode `M+0x3b70` changes. The mode is
   set by `FUN_00090860` (`0x90860`, lines 193-200) each time it runs: `iVar9 = param_2[9]->+0x4b8`, else `+0x4c0`,
   default 3; `FUN_00095690(M, iVar9)` then maps it (3 -> 0, 1 -> 2, 2 -> 1, others -> 0) and `0xa1cd0` returns at once
   when the mode is unchanged. **proven** (code). **Unknown** what the two input fields mean (main view, shadow or
   reflection). With the fallback rule the three modes resolve to the same named objects, so in practice the sets
   are constant after the first `0x9aef0` run (**inferred**).
4. **Main findings (table in K5.5):**
   - `+0x2a40` (GBuffer family, `0xc1ce0`, `0xc1fc0`, `0xc59d0`): seven colour slots (Albedo_AmbientOcclusion,
     Normal_Roughness, PBRProperties, ExtraPBRProperties, Motion, Ambient0, Ambient1) and depth = kRTT_MainDepth.
   - `+0x29a0` (DepthPrePass, `0xc1860`; also read by `0xcaed0`, `0xc28c0`, `0xc59d0`, `0xc6f70`, `0xc6550`): **no colour
     slot is written**, depth = kRTT_MainDepth. Depth-only set. **proven** (no writes to `+0x29a0..+0x29d8`).
   - K1's label "shadows" for `+0x1fb8`, `+0x1ff8`, `+0x2000` is not supported: these are the depth source of the
     GBuffer and `+0x29a0` sets (`+0x1fb8`) and the Ambient0/Ambient1 sources (`+0x1ff8`, `+0x2000`). See K5.3.
5. **Formats** (K1 table, `DAT_02ed3680`): MainColor id 1 (16_16_16_16 F); Albedo, Normal, PBR, ExtraPBR, Ambient1 id 4
   (32 UINT); Motion id 0x14 (16_16 F); Ambient0 id 0x18 (32_32 UINT); SpecularIBLOcclusion id 0x12 (8 UNORM);
   MainDepth id 9 (32 F, depth kind). **proven** (ids at the `0x9aef0` creation calls; table from K1).
6. **Check against `pass_layouts.txt`**: the GBuffer rows agree on the formats of all seven colour slots (as K1 found).
   Two names are swapped relative to the rows (K5.3). The decal row has no colour export, but `0xc28c0` binds four
   GBuffer colours for `Decal` (K5.5). Both are open.

### K5.2 The sets and their writers

Set bases (`+0x29a0`, `+0x29f0`, `+0x2a40`, `+0x2a90`, and `+0x2900`, `+0x2950`) are zeroed by 32-byte stores in
`0x9aef0` lines 282-302. Set layout (K1 section 3, **proven** for the readers `0x92140` and `0x86a290`):

```c
struct RtSet {            // 80 bytes
  void* slot[8];          // +0x00..+0x38  colour targets (MRT 0..7)
  void* depth;            // +0x40         depth owner (depth block at target+0x164)
  u64   flags;            // +0x48         bit 0: alternate descriptor (+0x124)
};
```

`0x9aef0` never stores into `+0x29xx` or `+0x2axx`; its stores for kRTT objects go to the named table `M+0x8..M+0x2b8`
(K5.6). **proven**

`0xa0a60` writes (**proven**; line numbers in `000a/000a0a60.c`):

| Set base | Slot writes (destination <- source) | Depth | Flags |
|---|---|---|---|
| `+0x29a0` | none | `+0x29e0` <- `+0x1fb8` (line 85) | `+0x29e8`: not written |
| `+0x29f0` | `+0x29f0` <- `+0x1fe0`; `+0x29f8` <- `+0x1fd8`; `+0x2a00` <- `+0x1fe8`; `+0x2a08` <- `+0x1ff0`; `+0x2a10` <- `+0x2078` (lines 88-95) | `+0x2a30` <- `+0x1fb8` (line 96) | `+0x2a38`: not written |
| `+0x2a40` (GBuffer) | `+0x2a40` <- `+0x1fe0`; `+0x2a48` <- `+0x1fd8`; `+0x2a50` <- `+0x1fe8`; `+0x2a58` <- `+0x1ff0`; `+0x2a60` <- `+0x2078`; `+0x2a68` <- `+0x1ff8`; `+0x2a70` <- `+0x2000` (lines 97-103) | `+0x2a80` <- `+0x1fb8` (line 104) | `+0x2a88`: not written (stays 0) |
| `+0x2900` | `+0x2900` <- `+0x1f88`; `+0x2908` <- `+0x2078`; `+0x2910` <- `+0x1fd8`; `+0x2918` <- `+0x1fe8`; `+0x2920` <- `+0x2300`; `+0x2928` <- `+0x1ff8`; `+0x2930` <- `+0x2000` (lines 118-124) | `+0x2940` <- `+0x1fb8` (line 125) | `+0x2948`: not written |
| `+0x2950` | partial: `+0x2950` <- `+0x24f0` (a copy of `+0x24a0`, itself a copy of `+0x1f88`); `+0x2958` <- `+0x2300`; `+0x2960` <- `+0x2078`; then sub-copies at `+0x2968..+0x2988` (lines 134-139) | not written | not written |

The `+0x2950` block is mixed with copies from `+0x24xx` and `+0x25xx` and has no reader found yet.

### K5.3 The two-level table (`0x9fba0`) and the names

`0x9fba0`, decomp `0009/0009fba0.c` lines 20-41 (condensed, **proven**). `k` runs over -0x480..-8 in 8-byte steps,
so i = k + 0x480 runs over 0..0x478:

```c
lVar6 = *(long *)(M + 0x1f80 + k);              // primary, address M+0x1b00+i
if (lVar6 == 0) {
  lVar6 = *(long *)(M + 0x480 + k);             // fallback: named kRTT field at M+i
  if (M->+0x3b70 == 2) lVar7 = *(long *)(M + 0x1b00 + k);   // derived, mode 2 (M+0x1680+i)
  else {
    if (M->+0x3b70 == 1) goto LAB_0009fc20;     // mode 1: fallback only
    lVar7 = *(long *)(M + 0xfc0 + k);           // derived, mode 0 (M+0xb40+i)
  }
  if (lVar7 == 0) goto LAB_0009fc20;
  *(long *)(M + 0x2400 + k) = lVar7;            // resolved = derived
} else {
LAB_0009fc20:
  *(long *)(M + 0x2400 + k) = lVar6;            // resolved = primary or fallback
}
```

So the resolved entry at `M+0x1f80+i` is the first non-null of: primary `M+0x1b00+i`, then derived `M+0xb40+i`
(mode 0) or `M+0x1680+i` (mode 2), then the named kRTT field `M+i`. (Mode 1 skips the derived lookup.) The named
offsets `M+i` for the set slots are the store offsets of K5.6, so the table below gives the kRTT object per slot.

| Set source | i = source - 0x1f80 | Named kRTT object at `M+i` | Format id |
|---|---|---|---|
| `+0x1f88` | 0x08 | `kRTT_MainColor` | 1 |
| `+0x1fb8` | 0x38 | `kRTT_MainDepth` | 9 (depth) |
| `+0x1fd8` | 0x58 | `kRTT_Normal_Roughness` | 4 |
| `+0x1fe0` | 0x60 | `kRTT_Albedo_AmbientOcclusion` | 4 |
| `+0x1fe8` | 0x68 | `kRTT_PBRProperties` | 4 |
| `+0x1ff0` | 0x70 | `kRTT_ExtraPBRProperties` | 4 |
| `+0x1ff8` | 0x78 | `kRTT_Ambient0` | 0x18 |
| `+0x2000` | 0x80 | `kRTT_Ambient1` | 4 |
| `+0x2078` | 0xf8 | `kRTT_Motion` | 0x14 |
| `+0x2300` | 0x380 | `kRTT_SpecularIBLOcclusion` | 0x12 |

The GBuffer set therefore holds Albedo in MRT 0 and Normal in MRT 1. `pass_layouts.txt` lists c0 = Normal and c1 =
Albedo (both 32 UINT, so the rows cannot tell them apart). **Open:** which order the shader really exports.

### K5.4 Caveats

- **Derived and primary tables.** I found no store into `M+0xb40..M+0xfc0` (mode 0 derived) for the slots used; the
  hits in `0003/000335d0.c` are another object (`lVar4`). The primary table `M+0x1b00+i` is written by `0xa0290`
  (line 27 `+0x1b48`; lines 60-231 `+0x1c40..+0x1ce8` and `+0x1ca0..`, `+0x1d70`, `+0x1dc0`, `+0x1e10+8n`,
  `+0x1eb8`, `+0x1f00`, `+0x1f28`). None is a set source except possibly `+0x1e80` (slot `+0x2300`), if the
  `+0x1e10+8n` loop reaches n = 14. **Unchecked.**
- **Order.** The `0x9aef0` tail is `0x9fa90`, `0x9fba0`, `0xa0290`, `0xa0a60` (lines 2183-2185). `0x9fba0` runs before
  `0xa0290`, so the derived writes of `0xa0290` are not seen in the same pass. Mode switches (`0xa1cd0`) run the same
  order, so the resolved table lags one rebuild. **Inferred** from the call order.
- **Conflict with `0xcaed0`.** `0xcaed0` computes `(M+0x29f0 - M+0x29e8) >> 5` (about line 210), i.e. it reads
  `+0x29e8` and `+0x29f0` as the begin and end of a 32-byte-item vector. In the set reading `+0x29f0` is SetD slot 0
  (a pointer) and `+0x29e8` is SetA's flags. Either `0xcaed0` reads a different object at `view+0x29xx`, or one reading
  is wrong. **Open**; check at run time before relying on SetA or SetD.
- **Object identity.** `0xc0da90` (called from `0xb33c80`, not from the manager creation path) initialises
  `+0x1fb0..+0x20f8` of its object. I could not confirm that this object is the `0x4d88` manager. The fallback result
  assumes that the `+0x1b00..+0x2400` fields and the named table belong to the same object, as the copy in `0x9fba0`
  does.

### K5.5 Table: pass runner -> set -> slot -> kRTT -> format

Runners read the set from `view = pass+0x18` (K1, **inferred** to be the manager). Samples are 1 throughout (K1
assumption, not decoded).

| Runner (VA) | Pass scope | Set | Slot | kRTT | Format (id) | Depth |
|---|---|---|---|---|---|---|
| `0xc1860` | DepthPrePass | `+0x29a0` | none (depth-only) | - | - | MainDepth (9, Z32F) |
| `0xc1ce0` (k 2, 3) | GBufferAmbientBaked | `+0x2a40` | 0 | Albedo_AmbientOcclusion | 32 UINT (4) | MainDepth (9) |
| | | | 1 | Normal_Roughness | 32 UINT (4) | |
| | | | 2 | PBRProperties | 32 UINT (4) | |
| | | | 3 | ExtraPBRProperties | 32 UINT (4) | |
| | | | 4 | Motion | 16_16 F (0x14) | |
| | | | 5 | Ambient0 | 32_32 UINT (0x18) | |
| | | | 6 | Ambient1 | 32 UINT (4) | |
| | | | flags `+0x2a88` | 0 (linear descriptor) | | |
| `0xc1fc0` (k 4, 5) | GBufferAmbientDynamic | `+0x2a40` | as above | as above | as above | MainDepth; also binds `+0x1fb8` (depth), `+0x1ff8` (Ambient0), `+0x2000` (Ambient1) through `0x93d70` and `0x93960` (lines 115-117) |
| `0xc28c0` (k 6) | Decal | `+0x29a0` read; explicit binds of `+0x1fd8`, `+0x1fe0`, `+0x1fe8`, `+0x1ff0` (lines 69-72) | GBuffer slots 1, 0, 2, 3 | Normal, Albedo, PBR, ExtraPBR | as above | MainDepth |
| `0xcaed0` | PreDebug, Debug, ForwardDecal | `+0x29a0` (line 207) | none | - | - | MainDepth |
| `0xc59d0` | main scene (k not decoded) | `+0x29a0`, `+0x2a40`, `+0x2a88` (lines 128-134, 268); explicit binds of `+0x1fb8`, `+0x1f88`, `+0x1fe0..+0x1ff0`, `+0x2000` | as GBuffer | as above | as above | MainDepth |
| `0xc6f70` | XPRRunFragmentCullShaderMainScene | `+0x29a0` (line 88) | none | - | - | MainDepth |
| `0xc6550` | (name not read) | `+0x29a0` (line 78) | none | - | - | MainDepth |

Not traced by K5: `0xc2710`, `0xc2fa0`, `0xc3600`, `0xc3820`, `0xc3c70`, `0xc4de0`, `0xc3320`, `0xc3ec0`, `0xc4f80`,
`0xc46c0`, `0xc4a50`, `0xc3a50`. The `+0x2900` set (slot 0 MainColor, slots 1..6 Motion, Normal, PBR, SpecularIBLOcclusion,
Ambient0, Ambient1, depth MainDepth at `+0x2940`) has no reader found. It looks like a forward colour-plus-depth set (**inferred**).

### K5.6 Fixed or per frame, and when

- **Creation.** `0x9aef0` runs once per manager creation (`0x95a90`, from the area path `0x2569d0`, K1 section 6). Its
  tail is `0x9fba0`, `0xa0290`, `0xa0a60` in the default mode.
- **Per frame.** `0xcefa0` calls `0x118440`, which calls `0x90860`, which sets the mode. A mode change reruns `0x9fba0`,
  `0xa0290`, `0xa0a60` through `0xa1cd0`. Otherwise nothing is written. **proven** (call edges). How often the mode changes
  during play is **unknown**; the run check in K5.7 answers it.
- **Named table.** The kRTT fields `M+0x8..M+0x2b8` are written once, at creation (store list below).

Named kRTT stores in `0x9aef0` (offset of the field <- kRTT name; from the first `*(param_1 + X) = plVar25` after each
`"kRTT_*"` call, lines 633-2107). **proven**:

`MainColor +0x8`; `MainDepth +0x38`, `MainDepth_HalfRes +0x40`; `CollisionDepth +0x50`; `Normal_Roughness +0x58`;
`Albedo_AmbientOcclusion +0x60`; `PBRProperties +0x68`; `ExtraPBRProperties +0x70`; `Ambient0 +0x78`; `Ambient1 +0x80`;
`Lighting +0x88`; `RenderTargetDisplay +0x90`; `PostColorHalfRes_A +0x98`, `_B +0xa0`; `DOFAmount +0xa8` and `+0xb0` (two
calls, lines 691 and 705); `DOFResult +0xb8`; `DOFAmountTile_8x8 +0xc0`, `_Field_16x16 +0xc8`, `_Prefilter_16x16 +0xd0`;
`DOFPrefilterNear +0xd8`, `Far +0xe0`; `DOFNearField +0xe8`; `DOFFarField +0xf0`; `Motion +0xf8`; `MotionBlur_Vel +0x100`,
`_MagDepth +0x108`, `_Tile_Horiz +0x110`, `_Tile_Vert +0x118`, `_Tile_Final +0x120`; `TemporalAA_Color_Out_A +0x128`,
`_B +0x130`; `FogMediaCull +0x388`; `FogMedia +0x390`; `FogMediaParticleMediaArray +0x3b0`; `FogScatter0 +0x398`,
`FogScatter1 +0x3a0`; `FogRaymarch +0x3a8`; `SpecularIBL +0x378`; `SpecularIBLOcclusion +0x380`; `SSAO_Accumulation_A +0x1f8`,
`_B +0x200`; `SSAO +0x1f0`; `SSAOTemp +0x208`; `AmbientShadowRG_SSAOB +0x210`; `MainColorCopy +0x218`; `SunShadowResolve +0x190`;
`LightingDiffuse +0x10`; `ParticleColor_UltraLowRes +0x30`, `_HalfRes +0x28`; `ParticleBlocker +0x188`; `PerTileDepth_12x12 +0x360`,
`_Temp +0x368`; `ClusterInfo +0x370`; `LinearDepthMipped +0x308`; `LinearNearDepth +0x358`; `SpotShadowArray +0x1c50`;
`HiResSpotShadowArray +0x1c38`; `CascadedShadow +0x1c98`; `RainBlocker +0x1c80`; `OccluderDepth +0x1d60`,
`OccluderDepthTextureMipped +0x1d68`, `NAdvOccluderDepthTextureMipped +0x2b8`; `Particle_LightmapA +0x1ef8`, `_B +0x1f20`;
`Display +0x1b00`; `PostProcessFinal +0x448`; `FullscreenBlur_0 +0x450`, `_1 +0x458`; `UI_Onscreen +0x460`;
`UI_Offscreen_0 +0x468`, `_1 +0x470`, `UI_Offscreen_Fonts +0x478`.

Note: the store-pairing is by the first store after each call, so a field that is written by a later call or by
another function could be missed. The 10 set sources in K5.3 were checked by hand against these offsets.

### K5.7 Port use and runtime check

- The port can read the sets after `0x9aef0` returns (hook the return), or read the named fields `M+0x8..` directly. The
  set slots resolve to the named objects (K5.3) unless the primary or derived tables are non-null (K5.4).
- Per pass key (`pass+0xf4`, K1 section 2): `k=1` (DepthPrePass) uses `+0x29a0` (depth-only); `k=2..5` use `+0x2a40`;
  `k=6` (Decal) binds GBuffer colour slots 0-3 explicitly, so the set alone does not give its colour formats.
- **Run check** (one run, no change to draws): at the first draw of `DepthPrePass` and of `GBufferAmbientDynamic`, log the
  10 qwords at `M+0x29a0`, `M+0x2a40`, `M+0x2900`; `M+0x3b70`; the resolved entries `M+0x1f80+i` for the i of K5.3; the primary
  entries `M+0x1b00+i`; and the derived entries `M+0xb40+i`, `M+0x1680+i`. Also log `param_2[9]+0x4b8` and `+0x4c0` where
  `0x90860` reads them. This settles the `0xcaed0` conflict (K5.4), the primary and derived question, and how often
  the mode changes.

### K5.8 Open items (K5)

1. Settle the `0xcaed0` reading of `+0x29e8`/`+0x29f0` against the set reading (K5.4).
2. Find the reader of `+0x2900` and `+0x2950`, and check `+0x1e80` against the `0xa0290` loop (K5.4).
3. Name the mode inputs `param_2[9]+0x4b8` and `+0x4c0`, and the meaning of mode values 0, 1, 2 (K5.1).
4. Settle the Albedo-vs-Normal MRT order against the shader export (K5.3), and the decal colour question (K5.5).
5. K1 labelled `+0x1fb8`, `+0x1ff8`, `+0x2000` as shadow targets. This section gives GBuffer depth and Ambient0/1 for
   those offsets. Confirm at run time.

### K5.9 Evidence list (K5)

| Item | Address | File |
|---|---|---|
| Set writer | `0xa0a60` | `out/re/decomp/000a/000a0a60.c` |
| Resolve loop | `0x9fba0` (lines 12-41) | `out/re/decomp/0009/0009fba0.c` |
| Derived and primary writes | `0xa0290` | `out/re/decomp/000a/000a0290.c` |
| Mode change | `0xa1cd0`, `0x95690` | `out/re/decomp/000a/000a1cd0.c`, `out/re/decomp/0009/00095690.c` |
| Mode input | `0x90860` (lines 185-200), `0x118440`, `0xcefa0` | `out/re/decomp/0009/00090860.c`, `out/re/decomp/0011/00118440.c`, `out/re/decomp/000c/000cefa0.c` |
| Creation, zeroing and tail | `0x9aef0` (lines 235, 282-302, 633-2107, 2183-2185) | `out/re/decomp/0009/0009aef0.c` |
| Manager allocation | `0x95a90` (lines 136-149) | `out/re/decomp/0009/00095a90.c` |
| Set readers | `0xc1860`, `0xc1ce0`, `0xc1fc0`, `0xc28c0`, `0xc59d0`, `0xcaed0`, `0xc6f70`, `0xc6550` | `out/re/decomp/000c/` |
| Vector-like reads of `+0x29e8/+0x29f0` | `0xcaed0` (about line 210) | `out/re/decomp/000c/000caed0.c` |
| Initial values of `+0x1fa0..+0x20f8` | `0xc0da90` (called by `0xb33c80`) | `out/re/decomp/00c0/00c0da90.c` |
| Pass layouts | 68 rows | `user/shader_predict/pass_layouts.txt` |

## Open items closed (K6)

Topic: close the open items of "Set fill (K5)" (K5.4 and K5.8). Read-only static analysis of the same eboot export
(`out/re/decomp`). Addresses are eboot VAs. Labels as in section 1: **proven** (decomp read, file and line given),
**inferred**, **unknown**. Object names follow K5: `M` = the 0x4d88-byte manager, `V` = the view a runner reads
(`pass+0x18`), `F` = the renderer object that `0xcefa0` passes as `param_1`, `S` = the object at `param_2[9]` in
`0x90860`, `O` = the video-out owner in the global `DAT_02ed3670` (`M` = `O+0x80`, see K6.3).

### K6.1 Answer in short

| # | Item | Answer | Confidence |
|---|---|---|---|
| 1 | 0xcaed0 conflict | Two objects. Line 207 reads the view `V` as an RtSet from `+0x29a0` (K5 is right). The `+0x29e8`/`+0x29f0` vector read at line 704 is on `F` (`param_1`), not on `V`. `+0x29f0` = `+0x29a0` + 0x50 is the first qword of SetD in `V`. | high (object identity proven by call sites) |
| 2 | Derived and primary tables | Both derived tables are written by `FUN_0009fa90` (0x9fa90), **in Neo mode only**, from `FUN_0009aef0`: mode 0 at `M+0xb40+i`, mode 2 at `M+0x1680+i`. The primary `M+0x1b00+i` is written by `FUN_000a0290` (0xa0290): fixed slots and a loop at `M+0x1e10+8n`. The loop reaches `M+0x1e80` (source `+0x2300`) only if the count at `M+0x2288` is at least 15. The writer of `M+0x2288` was not found. | high (writers), unknown (count) |
| 3 | Object identity of 0xc0da90 | Not `M`. It is a PhysX-style object made by `0xb33c80` (called from `0xb31840`), not the 0x4d88 manager of `0x95a90`. | high |
| 4 | View mode `M+0x3b70` | Raw mode R = `S+0x4b8` if non-zero, else `S+0x4c0` if non-zero, else 3. `M+0x3b70` = map(R): 3->0, 1->2, 2->1, else 0. In base mode the mode has no effect on the sets; in Neo mode it selects sized derived targets (item 2). The meaning of the `S` fields is not decoded. | high (code), low (meaning) |
| 5 | Decal anomaly | The four GBuffer binds do not reach four slots: `FUN_00093960` always binds CB_COLOR0 (slot 0 is a constant in its call), and `FUN_00092140` then clears CB_COLOR0 because SetA has no colour. The decal draw has no colour target, which matches the `decal` layout row. | medium-high |
| 6 | Slot order | Set slot 0 = `kRTT_Albedo_AmbientOcclusion` (`M+0x1fe0`, named `M+0x60`), slot 1 = `kRTT_Normal_Roughness` (`M+0x1fd8`, named `M+0x58`). The layout file cannot decide this; the K1 table entry "c0 = Normal" was an assumption. The order the shader exports is not checked. | high (set order), unknown (shader export) |

### K6.2 Item 1: the 0xcaed0 conflict

Proven facts:
- `FUN_000caed0(long param_1, long param_2, undefined8 *param_3)` (`000c/000caed0.c` line 5), called from
  `FUN_000cefa0` line 339 with `param_1` = `F`, and from `FUN_000ccdf0` line 52.
- Line 206: `lVar11 = param_3[3]` (`V`). Lines 207-216 load ten qwords `V+0x29a0` .. `V+0x29e8` into a local array
  that goes to `FUN_00092140` (line 217). That is the RtSet of K1 section 3: SetA at `+0x29a0`, depth at `+0x29e0`,
  flags at `+0x29e8`.
- Line 704, in the stats block at the end: `(*(param_1+0x29f0) - *(param_1+0x29e8)) >> 5`. The base is `F`. The
  same begin/end pattern appears at `F+0x948/0x950`, `+0x1170/0x1178`, `+0x21c0/0x21c8`, `+0x3210/0x3218`,
  `+0x3a38/0x3a40` and `+0x4260/0x4268` (lines 690-704). It counts 32-byte items; it is not a set.
- `FUN_000c7850` (`000c/000c7850.c`, called from `FUN_000ca6d0` line 93 with the same `F`) writes
  `F+0x29b0 <- F+0x29a8`, `F+0x29d0 <- F+0x29c8`, `F+0x29f0 <- F+0x29e8` for `lVar28` = 0..4, stride 0x828 (lines 56-59).
  This is again `F`.
- Stride: `0x29a0 + 0x50 = 0x29f0`, and SetA is `0x29a0..0x29ef`. So `+0x29f0` is the start of SetD in `V`.
  `0xa0a60` line 88 writes SetD slot 0 from `+0x1fe0` (K5.2).

All readers and writers of `+0x29e8` and `+0x29f0` in the decomp (grep of `000c`, `000a`, `0009`, `000b`, `0011`):
- SetA flags, read as RtSet qword 9 on `V`: `0xc1860:57`, `0xc28c0:82` and `:158`, `0xc59d0:171`, `:185`, `:293`,
  `0xc6550:38`, `0xc6f70:71`, `0xcaed0:216`, `0xadd50:116` (object not identified).
- SetD slot 0 on `V`: read only by `0xa0a60:110` (copy of `+0x29f0` into `+0x2860`). No traced runner reads SetD, so
  SetD has no consumer in the traced set (open).
- `F`'s vector: read `0xcaed0:704`, written `0xc7850:59`.
- Zeroing: `0x9aef0:290` (`+0x29a0` block) and `:293` (`+0x29f0` block).

Answer: the K5 reading (SetA and SetD) is right for `V`. Line 704 is a stats read of `F`. There is no conflict.

### K6.3 Item 3: identity of the 0xc0da90 object, and of V

- `FUN_00c0da90` (`00c0/00c0da90.c`) is called only by `FUN_00b33c80` and `FUN_00b33bf0`. Its body calls
  `FUN_00b30a10`, `FUN_00b2f940`, and a PhysX allocator path with the string
  `"./../../../../PxShared/src/foundation/include\\PsMutex.h"` (line 0x71). It initialises `param+0x1fa0` (16 bytes),
  `+0x1fb0` (mutex pointer), `+0x1fb8` (2 bytes) and `+0x1fc0` (`FUN_00bf8c40(.., 0x4000)`). This is a PhysX mutex
  and foundation setup, not a kRTT target.
- `FUN_00b33c80` (`00b3/00b33c80.c` lines 1-20): sets the vtable `PTR_FUN_015d6c00`, calls
  `FUN_00c0da90(param_1+2, param_2, param_1)` and `FUN_00bc6180(param_1+0x486, ...)`. Its only caller is
  `FUN_00b31840` (`00b3/00b31840.c` line 36). It does not appear in the manager chain.
- The manager `M` is a different allocation. `FUN_00095a90` line 142 does `lVar7 = FUN_00017070(0x4d88)`, calls
  `FUN_0009aef0(lVar7, ...)`, and stores `lVar7` at byte `O+0x80`. The owner parameter of `0x95a90` is `undefined4 *`,
  so `*(long *)(param_1 + 0x20)` is byte offset 0x80 (lines 131-141). The owner is the global `DAT_02ed3670` (lines 54-61).
  Neither `0x95a90` nor `0x9aef0` calls `0xc0da90` or `0xb33c80`.
- Verdict: the 0xc0da90 object is **not** `M`. Confidence high.
- Identity of `V` (not settled): `V = **(F+0x40 +0xd98)` (`000c/000cefa0.c` lines 147 and 163). Writers of `F+0xd98`
  found: `0x38b40:56`, `0x43fb0:63` (copy from `+0xd88`, so it may be a per-frame alternate), `0x1c6e80:40`.
  No writer ties it to `M` directly. Since `0xa0a60` writes the sets only into `M`, and runners draw with valid targets
  from `V+0x29a0`, `V` must be `M` or a copy of it. **inferred (medium)**. Closed by the runtime check in K6.9.1.

### K6.4 Item 2: the derived and primary tables

Proven writers:
- Mode-0 derived, `M+0xb40+i` (the `M+0xfc0+k` of `0x9fba0`): `FUN_0009fa90` (`0009/0009fa90.c`, loop body lines 13-36).
  The whole body is under `if (sceKernelIsNeoMode() != 0)`. For k from -0x480 in steps of 8 it releases the old entry,
  then stores `M+0xfc0+k = FUN_00096f70(named_i, 0, w, h, p)`, with `(w, h, p)` from the 12-byte record at
  `M+0x480+12n` (`puVar6[-0x2d2..-0x2d0]`, n = (k+0x480)/8). `named_i` = `M+i` (the named kRTT field of K5.6).
- Mode-2 derived, `M+0x1680+i` (the `M+0x1b00+k` of `0x9fba0`): the second store of the same loop body
  (lines 37-46): `M+0x1b00+k = FUN_00096f70(named_i, 0, w, h, p)`, with `(w, h, p)` from the record at `M+0xfc0+12n`.
- The 12-byte records are written by `FUN_0009ae80` (`0009/0009ae80.c` lines 11-14): `M+0xfc0+12*idx` and
  `M+0x480+12*idx`. No direct caller was found (header empty), so the caller is open.
- Clear: `FUN_0009ac10` (`0009/0009ac10.c` lines 9-27) releases and zeroes the named fields, `M+0xfc0+k` and `M+0x1b00+k`.
  It is called from `0x96010` and `0x95a90` (manager replacement).
- The derived tables are built once, at creation, from the tail of `0x9aef0` (K5 tail list). A mode change (`0xa1cd0`)
  does not rebuild them. The earlier K5 statement "no store into M+0xb40..M+0xfc0" was wrong: the store is the
  indexed form `param_1 + 0xfc0 + lVar5`, which a direct-offset grep does not match.
- Primary, `M+0x1b00+i` (the `M+0x1f80+k` of `0x9fba0`): `FUN_000a0290` (`000a/000a0290.c`).
  - Fixed writes: `M+0x1b48` from `FUN_00097520(M+0x1fb8, desc)` (around lines 18-28), `M+0x1ca0` and `M+0x1ca8`
    from `M+0x2118` (around lines 47-62), and others listed in K5.4.
  - Loop (lines 33-45): `count = FUN_000987f0(*(M+0x2288))`, which returns `*(int *)(target+0x1a8)`
    (`0009/000987f0.c`). If `count != 0`, for n = 0..count-1 it stores `M+0x1e10+8n = FUN_00097520(*(M+0x2288), desc(n))`.
  - `M+0x1e80` (primary of source `+0x2300`, i = 0x380) is n = 14, because 0x1e80 - 0x1e10 = 0x70 = 14*8.
    **Written iff count >= 15.** The loop writes views of the target at `M+0x2288`, not views of SpecularIBLOcclusion.
    If the count is at least 15, the `+0x2300` set slot is view 14 of `M+0x2288` (inferred).
  - Writer of `M+0x2288`: not found. Searched `0x2288`, decimal `8840` and `+ 0x2288` in `0009`, `000a`, `000b`,
    `000c`, `0025`. Only reads: `0xa0290:33`, `:39`, and `0xbb190:37`. **unknown**.

Effect on the resolved set (`0x9fba0`, K5.3):
- Base mode (not Neo): the derived entries are never written. Mode 0 and mode 2 both fall back to the named object
  (the allocation zeroing of `FUN_00017070` is not checked). The mode does not change the sets, and the mode-change
  rebuild is content-neutral.
- Neo mode: mode 0 uses derived set A (sizes from `M+0x480`), mode 2 uses derived set B (sizes from `M+0xfc0`),
  and mode 1 uses the named object. The modes differ in target size (inferred: the resolution path).
- Creation value: `0x9aef0` line 459 writes `M+0x3b70 = param_5`, and `0x95a90` line 142 passes `param_5 = 0`.
  So the mode starts at 0.

### K6.5 Item 4: the view mode M+0x3b70

- `0x90860` lines 193-200 (K5): `R = S->+0x4b8`; if `S->+0x4c0 != 0` then R = `S->+0x4c0`; if `S->+0x4b8 != 0`
  then R = `S->+0x4b8`; default 3. Then `FUN_00095690(DAT_02ed3670, R)`. Priority: `+0x4b8` over `+0x4c0` over 3.
  **proven**.
- `S = param_2[9]`, the second argument of `0x90860`. The decomp of `0x118440` (`0011/00118440.c`) shows one parameter and
  calls `FUN_00090860(DAT_02ed33f0)`, so the second argument is passed through from `cefa0` line 159 and is not visible in
  that decomp. Identity of `S` is **unknown**. The same index is used in `0xc7850` (`param_2[9]+0x42c`) on a pass object.
- `0x95690` (`0009/00095690.c` lines 1-15): stores R in `M+0x3c`, maps R to the mode (R=1 -> 2, R=2 -> 1, else 0), and
  calls `FUN_000a1cd0(M, mode)`. It returns early when R is unchanged.
- `0xa1cd0` (`000a/000a1cd0.c`): returns early when `M+0x3b70` is unchanged. Otherwise it bumps `M+0x4d80`, sets
  `M+0x3b70`, and calls `0x9fba0`, `0xa0290`, `0xa0a60`.
- Writers of the settings fields (`S+0x4b8`, `S+0x4c0`), all found in the decomp:
  - `0x195370` (`0019/00195370.c` lines 203-210). The same function holds the string
    `"[VirtualGeometry] Too many model placements in model group %s"` (line 184). Its callers are `0x194140` and `0x195010`.
  - `0xe0db0` (`000e/000e0db0.c` lines 102-105): copies its `param_2+0x80`, `+0x88`, `+0x98` into `param_1+0x4b4`,
    `+0x4bc`, `+0x4c0`. Called by `0x195370`.
  - `0xd0950` (`000d/000d0950.c` line 86): zeroes `+0x4b8`. Called by `0xbd170` and `0x5b7630`.
  - Not `S`: `0x9aef0` lines 925-926 and 1572-1573 write `M+0x4b4..0x4c0` (the manager's own fields); `0xf6c90` and
    `0xf8270` write material fields.
- Strings near `0x90860`: `"Render Context memory CPU/GPU: %s / %s"` (lines 74, 107) and `"Scene"` (line 166). No mode name.
- Meaning: **unknown**. Inferred from the Neo-only derived sets: modes 0 and 2 are two sized variants of the same targets,
  mode 1 is the full named size, so the raw value is probably a Neo resolution choice. HDR, photo mode, cinematic and
  Neo itself are not supported by any evidence found.
- Frequency: `0x90860` runs once per `cefa0` main-camera build (`0x118440`). The mode changes only when R changes.
  How often R changes during play: **unknown** (K6.9.5).

### K6.6 Item 5: the decal anomaly

Proven path:
- `0xc28c0` (`000c/000c28c0.c`) calls `FUN_00093960(ctx, V+0x1fd8)`, `(V+0x1fe0)`, `(V+0x1fe8)`, `(V+0x1ff0)` (lines
  69-72, K5 numbering). Each call takes one target as argument. There is no slot argument.
- `0x93960` (`0009/00093960.c`):
  1. It exits unless the target type byte `+0x62` is 1 or 3.
  2. `FUN_00093840(ctx, target, -1)` sets size and scissor from the target (`+0xf0`, `+0xec`, `+0xf8`).
  3. Only if the target flag `+0xf5 & 0x20` is set: `FUN_0085cbc0(*param_1, 0, target+0xe4)` (index **0**), and
     `param_1[9] = target+0xe4`. So CB_COLOR0 is the only colour slot written. The last colour call wins
     (`V+0x1ff0`, ExtraPBR).
- The four binds therefore do not write slots 1, 0, 2, 3. **K5.5 is wrong on this point.**
- `0xc28c0` then copies SetA (`V+0x29a0..0x29e8`, depth only, no colour) and calls `FUN_00092140(ctx, SetA)`
  (after line 82).
- `0x92140` (`0009/00092140.c` lines 150-159): for each slot i with a null entry, it writes CB_COLORi with descriptor 0
  (`FUN_0085cbc0(*param_1, i, 0)`) when the dirty bit `param_1+2 bit i` is set or `param_1[i+9] != 0`. The cache
  `param_1[9]` for slot 0 was set to the decal's CB_COLOR0 descriptor by `0x93960`, so CB_COLOR0 is cleared.
- Net result: after the decal setup CB_COLOR0..7 are all cleared. The decal draw is depth-only at the target level.
  That matches the decal layout row, which has no colour export.

Caveats:
- Inferred: both calls use the same `lVar6` (`*ctx` cache) as `param_1`, so the cache is shared.
- If bit 0x20 of the target flag is clear, `0x93960` writes no colour at all, and the result is the same.
- `user/shader_predict/pass_layouts.txt` has one `decal` row. Its 64 colour bytes are all zero, and the tail is
  `00010000`. `predict_table.tsv` lists 164 `decal` programs, 109 of them pixel (footer type 0). Only one was captured.
  Other decal pixel programs may export colour. The anomaly is therefore partly a sampling question (K6.9.6).

### K6.7 Item 6: the slot order

- `0xa0a60` (`000a/000a0a60.c`): line 89 `uVar2 = *(M+0x1fd8)`; line 97 `*(V+0x2a40) = *(M+0x1fe0)` (Albedo_AmbientOcclusion,
  named `M+0x60`); line 98 `*(V+0x2a48) = uVar2` (Normal_Roughness, named `M+0x58`). Then `+0x2a50` = PBR (`+0x1fe8`),
  `+0x2a58` = ExtraPBR (`+0x1ff0`), `+0x2a60` = Motion (`+0x2078`), `+0x2a68` = Ambient0 (`+0x1ff8`), `+0x2a70` = Ambient1 (`+0x2000`).
- `0x92140` binds set index i to CB_COLOR i (line 159 `FUN_0085cbc0(*param_1, uVar6, lVar10)`, with `uVar6` the slot index).
- Answer (proven for the set): MRT0 = Albedo_AmbientOcclusion, MRT1 = Normal_Roughness. K5 is right.
- Not settled: the order in which the GBuffer pixel shader exports MRT0 and MRT1. `pass_layouts.txt` gives c0 and c1 the
  same format (32 UINT), so the file cannot decide. The K1 table (c0 = Normal) was an assignment, not evidence.
  The decal bind order (Normal, Albedo, PBR, ExtraPBR) is not a slot order (K6.6).

### K6.8 Corrections to K5

1. K5.4 "conflict with 0xcaed0": resolved. Line 704 reads `F`, not `V` (K6.2).
2. K5.4 "derived and primary": the derived tables are written by `0x9fa90` (Neo only). The `+0x1e80` loop write is
   confirmed and is conditional on count >= 15 (K6.4).
3. K5.4 "object identity": the 0xc0da90 object is not `M` (K6.3).
4. K5.5 decal "slots 1, 0, 2, 3": wrong. All four binds target CB_COLOR0, and the state is cleared by `0x92140` (K6.6).
5. K5.1 item 3 "the three modes resolve to the same named objects": true only in base (non-Neo) mode. In Neo mode modes 0
   and 2 use derived sized targets, so the sets are not constant across mode changes there (K6.4).
6. K5.1 item 4: the priority and mapping in K5 are correct. The object is `S = param_2[9]`, which is neither `M` nor `O` (K6.5).
7. K5.3 (GBuffer MRT order) stands and is now confirmed in the code (K6.7).

### K6.9 Runtime log that closes what remains (one run, no draw change)

1. At the first `cefa0` call, log `F = param_1`, `V = **(F+0x40 -> +0xd98)`, and `M = *(DAT_02ed3670 + 0x80)`.
   Check `V == M`. Closes K6.3.
2. Log the result of `sceKernelIsNeoMode()` on the port. It decides whether the derived tables exist (K6.4).
3. Log `M+0x2288` (pointer) and `*(int *)(*(M+0x2288)+0x1a8)` (count) at the first frame, and `M+0x1e80`.
   Closes the `+0x2300` question. If `M+0x2288` is zero, a store watch on `M+0x2288` finds its writer.
4. In Neo mode, at `0x9fba0` entry, dump `M+0xb40+i` and `M+0x1680+i` for the set sources
   i = 0x08, 0x38, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0xf8, 0x380 (sizes and pointers).
5. At `0x90860`, log S, `S+0x4b8`, `S+0x4c0` and R on each call. At `0xa1cd0`, log each change (old, new, frame).
   Closes the frequency question and the S identity.
6. At the first `decal` draw (k = 6), log CB_COLOR0..7 INFO (`0x31C + 0xF*n`) after `0x92140`. Expect cleared.
   Log the decal pixel-program hash. Covers the 109-program question.
7. At `0x92140` entry for the GBuffer set, log slot 0 and slot 1 pointers and compare with `M+0x60` and `M+0x58`.
   The MRT0 export order is checked by the shader's export instruction for target 0 (static, on the captured pixel program).

### K6.10 Evidence list (K6)

| Item | Address | File and line |
|---|---|---|
| View RtSet read, F vector read | `0xcaed0` lines 5, 207-216, 339 (call), 704 | `out/re/decomp/000c/000caed0.c`, `000cefa0.c` |
| F SetD-like write | `0xc7850` lines 56-59; caller `0xca6d0` line 93 | `out/re/decomp/000c/000c7850.c`, `000ca6d0.c` |
| SetD copy into `+0x2860` | `0xa0a60` line 110 | `out/re/decomp/000a/000a0a60.c` |
| Zeroing of SetA and SetD | `0x9aef0` lines 290, 293 | `out/re/decomp/0009/0009aef0.c` |
| V chain | `0xcefa0` lines 147, 163 | `out/re/decomp/000c/000cefa0.c` |
| F+0xd98 writers | `0x38b40:56`, `0x43fb0:63`, `0x1c6e80:40` | `out/re/decomp/0003/`, `0004/`, `001c/` |
| 0xc0da90 (PhysX) | `0xc0da90` lines 1-40 | `out/re/decomp/00c0/00c0da90.c` |
| 0xb33c80 and caller | `0xb33c80` lines 1-20; `0xb31840` line 36 | `out/re/decomp/00b3/` |
| Manager allocation and owner | `0x95a90` lines 4, 40-62, 131-142 | `out/re/decomp/0009/00095a90.c` |
| Derived writers (Neo) | `0x9fa90` loop body lines 13-46 | `out/re/decomp/0009/0009fa90.c` |
| Derived records | `0x9ae80` lines 11-14 | `out/re/decomp/0009/0009ae80.c` |
| Derived clear | `0x9ac10` lines 9-27 | `out/re/decomp/0009/0009ac10.c` |
| Resolver | `0x9fba0` lines 20-41 (K5) | `out/re/decomp/0009/0009fba0.c` |
| Primary writer and count | `0xa0290` lines 25-45 | `out/re/decomp/000a/000a0290.c` |
| Count helper | `0x987f0` | `out/re/decomp/0009/000987f0.c` |
| Other reader of `+0x2288` | `0xbb190` line 37 | `out/re/decomp/000b/000bb190.c` |
| Mode computation | `0x90860` lines 193-200 (K5) | `out/re/decomp/0009/00090860.c` |
| Mode map and change | `0x95690` lines 1-15; `0xa1cd0` lines 1-15 | `out/re/decomp/0009/00095690.c`, `000a/000a1cd0.c` |
| Mode init | `0x9aef0` line 459 | `out/re/decomp/0009/0009aef0.c` |
| Settings writers | `0x195370` lines 184, 203-210; `0xe0db0` lines 102-105; `0xd0950` line 86 | `out/re/decomp/0019/`, `000e/`, `000d/` |
| Decal binds | `0xc28c0` lines 69-72, after 82 | `out/re/decomp/000c/000c28c0.c` |
| Colour bind (CB_COLOR0 only) | `0x93960` (call `FUN_0085cbc0(*param_1,0,lVar1)`), `0x93840` | `out/re/decomp/0009/00093960.c`, `00093840.c` |
| MRT bind and clear | `0x92140` lines 150-159 | `out/re/decomp/0009/00092140.c` |
| GBuffer set order | `0xa0a60` lines 89, 97-98 | `out/re/decomp/000a/000a0a60.c` |
| Decal layout row | 1 `decal` row, colour bytes zero, tail `00010000` | `user/shader_predict/pass_layouts.txt`, `predict_table.tsv` (164 decal rows, 109 pixel) |

## Export order (T2)

Topic: which render target each MRT export (`mrt<i>`) of a GBuffer pixel shader writes, and whether the exported
value matches the target's meaning. Checked on the translated SPIR-V in the port's pre-pack
(`user/cache/CUSA08809_v13272bcc/sotc_pretranslate.pack`) and on the translator and Vulkan code. Script:
`tools/re/gbuffer_exports.py` (stdlib, spirv-dis). Labels as in section 1. The GCN export instructions are not in the
pack and were not decoded; the GCN side rests on the translator code (proven) and the SPIR-V (proven for the
translated module).

### T2.1 Answer in short

1. **Export i goes to slot i.** The translator maps `EXP mrt<i>` to `IR::RenderTarget<i>` with index i, and the SPIR-V
   output is declared with `Location i`. Vulkan attachment i is `CB_COLOR i`. **proven** (code, section T2.4).
2. **Slot 0 = Albedo_AmbientOcclusion, slot 1 = Normal_Roughness.** This matches K5.3 and K6.7 (MRT0 Albedo, MRT1
   Normal). **proven** for the content (section T2.2): loc 0 is gamma-encoded colour with a linear byte, loc 1 is an
   octahedral normal with a linear byte.
3. **The K1 table entry "c0 = Normal, c1 = Albedo" (section 7 of this file) is wrong.** The layout rows are indexed by
   the same MRT index, so row c0 is Albedo_AmbientOcclusion and row c1 is Normal_Roughness. Both are 32 UINT, so the
   file itself cannot tell them apart; the content can.
4. The anchors hold: loc 4 is 16_16 float (Motion, 2 components) and loc 5 is 32_32 UINT (Ambient0, 2 components).

### T2.2 Export order table

Sample: the 1077 modules of pass `gbufferbakedambient` (all have the same export signature), plus the
`gbufferdynamicambient_motion` and `gbufferbakedambient_motion` modules checked by hand. Decoration evidence: module
`00000000000f9518` (`OpDecorate %64 Location 0` ... `%71 Location 6`, names `frag_color0..6`).

| Location | Output name | Declared / stored | Content (from the export expression) | Target (K5/K6 name) | Evidence |
|---|---|---|---|---|---|
| 0 | frag_color0 | u32 (declared u32x4), comp 0 | four 8-bit unorm fields. Bytes 0..2: `255 * pow(x, 1/2.2)` (`Exp2(0.454545438 * Log2(x))`) of a thresholded lerp of texture layers. Byte 3: linear `255 * clamp(v)`, no gamma. | Albedo_AmbientOcclusion (id 4) | gamma pow only on the colour bytes; no normal term; several albedo-like samples |
| 1 | frag_color1 | u32 (declared u32x4), comp 0 | two 12-bit unorm fields (bits 0..11 and 12..23) of `4095 * clamp(0.5 * v + 0.5)`, where v is an octahedral normal component (sign select `select(0 <= x, 1, -1) * -FAbs(...)`, division by an L1 sum of `FAbs`, `InverseSqrt` of squared sums). Byte 3: linear `255 * clamp(...)` (roughness, inferred). | Normal_Roughness (id 4) | normalisation (`InverseSqrt`, L1 fold, sign select) on interpolated tangent-frame inputs; 12+12 bit encode of [-1, 1] |
| 2 | frag_color2 | u32, comp 0 | byte 3: linear clamp of a per-vertex input (`in2[v][3]` via `BaryCoordKHR`, meaning unknown); bits 12..14 and 20..23 are constant flag bits (`0x7000`, `0xF00000`). | PBRProperties (id 4) | no texture, no normal term; constant flag bits (inferred: a material/flag word) |
| 3 | frag_color3 | u32, comp 0 | bytes 0..1: linear 8-bit values from per-vertex inputs `in1[v][0..1]`; bytes 2..3: constant 0. | ExtraPBRProperties (id 4) | no texture; two zero fields (inferred: two small material values) |
| 4 | frag_color4 | f32, comps 0 and 1 | Motion. `x = FragCoord.x * buf[50] - 0.5 * buf[36] - (0.5 / w) * (...) - 0.5`; `y` uses dwords 51, 37 and `FragCoord.y`. Difference of two screen positions, one built from the pixel position and srt_flatbuf rows (inferred reprojection). | Motion (id 0x14, 16_16 F) | 2 components, FragCoord-based, two matrix row groups in `srt_flatbuf` |
| 5 | frag_color5 | u32, comps 0 and 1 | Ambient0, two words. Each word ORs three fields shifted by 0, 9 and 18 with `Exp2`, `Log2`, `Floor` in the cone (shared-exponent, RGB9E5-like, inferred). | Ambient0 (id 0x18, 32_32 UINT) | 2 components, 9-bit shifts, exponent ops |
| 6 | frag_color6 | u32, comp 0 | constant bits `0x00C00000` (bits 22..23) plus an 8-bit field at 24 from `select` and shifts (meaning unknown). | Ambient1 (id 4, 32 UINT) | constant bits, no texture |

The declared type is `u32x4` / `f32x4` because the translator sizes the output from its component count, but only the
listed components are stored. **proven** (declared types and stores in the disassembly).

Consistency with the layout file: the `gbufferbakedambient` rows of `pass_layouts.txt` are c0..c3 32 UINT, c4 16_16
float, c5 32_32 UINT, c6 32 UINT. All 1077 baked-ambient modules have exactly this output signature. **proven** for
the 1077 modules.

### T2.3 Packing shape (32 UINT targets)

The 32-bit targets are built from `OpBitFieldInsert` chains (8-bit fields at 0, 8, 16, 24 for loc 0 and loc 3; a 12+12+8
split for loc 1) and from shift/or chains (loc 5). Each 8-bit field is `u32(select(isnan(x), 0, clamp(round(255 * x), 0, 255)))`.
**proven** (disassembly, `out/re/gbuffer_exports/<sha>.dis`). The per-module field lists are printed by the script
(`--report FILE`).

### T2.4 The mechanism: why Location i equals slot i

1. `gpu/shadps4/shader_recompiler/frontend/translate/export.cpp:50-65` (`Translator::ExportRenderTarget`):
   `color_buffer_idx = exp.target - RenderTarget0` (the MRT index). The only exception is dual-source blending, which
   folds MRT1 into index 0 and is not used by the GBuffer. The layout is read as
   `runtime_info.hw.fs.color_buffers[color_buffer_idx]`, so the layout slot is the MRT index too.
   `export_format == Zero` or `exp.en == 0` drops the export; the index is never renumbered. `info.mrt_mask` records
   the MRT.
2. `gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp`, `DefineOutputs` (Fragment case, around lines
   860-875): for each `RenderTarget<i>` with stores, `DefineOutput(type, i)` gives `Location i` and the name
   `frag_color<i>`. The type comes from `color_buffers[i].num_format`. Verified in the disassembly.
3. `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2962-2963`: `num_color_attachments =
   bit_width(mrt_mask)`, and `vk_graphics_pipeline.cpp:350-362` builds `color_formats[i]` from `color_buffers[i]`.
   The attachments are indexed by CB index, with no location remap. `vk_pipeline_cache.cpp:2836` clears the entries
   whose `mrt_mask` bit is clear. `CB_SHADER_MASK` only sets blend and write-mask state
   (`vk_graphics_pipeline.cpp:462-472`); it does not renumber.
4. Consequence: an MRT with no export has no `frag_color<i>` in the module, but the index does not shift. Some gbuffer
   modules have no `frag_color` outputs at all: their body is branch-only with no stores (module `000fe4af79a92841`,
   which also carries the diagnostic string `sotc salt 41`). These are export-dropped variants, and the export set
   depends on the layout, so the module is only comparable within one layout. **proven** for the module, **inferred**
   for the cause.

### T2.5 Caveats and open items

1. The GCN `EXP` target numbers were not decoded (the GCN code is in the game dump, not in the pack). Step 1 of T2.4 is
   translator code, not a byte-level check.
2. "Albedo" and "Normal" are assignments from the expressions (gamma colour, octahedral normal), not from a debug name.
   Roughness, metal and the flag bits of loc 2, loc 3 and loc 6 are inferred. Confidence: high for loc 0 and loc 1
   (the decisive shapes), medium for the 8-bit linear fields.
3. The motion formula (loc 4) is read from the expression and the srt rows (36, 37, 50, 51 in the baked module). It is
   not checked against the engine's matrices at run time.
4. The full run over all 4937 gbuffer modules (`python tools/re/gbuffer_exports.py --sample 0`) was started in the
   background but had not finished when this section was written. The signature counts above come from the 1077
   baked-ambient modules and the hand-checked motion samples only.
5. Run check (one run, no draw change): at `0x92140` for the GBuffer set, compare set slot 0 and slot 1 pointers with
   `M+0x60` and `M+0x58` (K6.9 item 7), and read the pass's `pass_layouts` row. This closes the GCN side without
   decoding it.

### T2.6 Evidence list (T2)

| Item | Where |
|---|---|
| Script | `tools/re/gbuffer_exports.py` (new, read-only) |
| Module examples | `out/re/gbuffer_exports/` (`.dis` cache): `00000000000f9518` (locations 0..6), `2a65df2e317793aa` (baked, expressions), `000fe4af79a92841` (no outputs) |
| Translator export mapping | `gpu/shadps4/shader_recompiler/frontend/translate/export.cpp:50-65`, `:150-200` |
| Output declaration | `gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp`, `DefineOutputs` (Fragment) |
| Vulkan attachments | `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2836, 2962-2963`; `vk_graphics_pipeline.cpp:350-362, 462-472` |
| Layout rows | `user/shader_predict/pass_layouts.txt` (`gbufferbakedambient` rows) |

## PS4 Pro / Neo (T3)

Topic: the Neo (PS4 Pro) side of the render targets, and what the port actually runs as. Read-only static analysis of
the same EU 1.01 eboot export (`out/re/decomp`). Addresses are eboot VAs. Names `M` (manager), `V` (view), `S` (settings
object of `0x90860`) follow K5 and K6. Labels: **proven** (decomp or port source read, file and line given),
**inferred**, **unknown**. Run data: `user/last_run.log` and `last_run.1..4.log` (the five current runs, all base mode).
Scratch table (script output, not a deliverable): `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/t3/kRTT_table.tsv`.

### T3.1 Answer in short

| # | Question | Answer | Label |
|---|---|---|---|
| 1 | What does the port report to `sceKernelIsNeoMode`? | 0 by default; 1 only with `SOTC_NEO=1`. One source, `runtime_sotc_neo()`. | proven |
| 2 | Is Neo on in the runs we have? | No. The five current `last_run*.log` print `sceKernelIsNeoMode -> 0 (SOTC_NEO)` and `RTAllocator (GPU): +1 allocs (322.50 MiB)`. No log under `user/` has `-> 1`. | proven |
| 3 | Which `.csdr` files load? | `_shipping.csdr`. The game command line has `+shaderTypeToLoad=Shipping`; with that cvar value Neo changes no file name. | proven (code and log) |
| 4 | What do Neo targets look like? | Same formats, 1x samples, larger sizes. Named main scene 2560x1440, half 1280x720, quarter 640x360, display 3840x2160. Derived sets (view modes 0 and 2): 1920x1080 and 1600x900. | proven (code); mode meaning inferred |
| 5 | Checkerboard, 2x MSAA, ID buffers? | None created. Every target creation call passes sample argument 0. | proven (code) |
| 6 | Does Neo add pipeline-key variants? | No from sizes (the key has no size field). Formats and samples are unchanged. Neo gets its own shader cache folder (`<serial>_neo`), so its prebuild runs again. | proven (key); inferred (no new variants) |
| 7 | What is the view mode under Neo? | Mode 0 at boot. The default raw value 3 gives mode 0 (1080p derived set). The Cinematic preset (raw 2) gives the named 1440p set. The preset mapping is inferred. | proven (code); preset mapping inferred |

### T3.2 What the port reports (switches)

Port side (all **proven**):
- `src/runtime_sotc.c:45-55`: `runtime_sotc_neo()` reads env `SOTC_NEO` (`1` = on). `is_neo_mode()` prints
  `Runtime: sceKernelIsNeoMode -> %d (SOTC_NEO)` and returns the value. Table entry at `src/runtime_sotc.c:472`.
- `gpu/shim/bbgpu.cpp:383`: the GPU library's `sceKernelIsNeoMode` returns the same value.
- `src/runtime_memory.c:28-34`: with Neo the direct-memory size is 5056 + 640 MiB.
- `gpu/shadps4/video_core/cache_storage.cpp:169-172`: the shader cache folder gets suffix `_neo` when Neo is on.
- `gpu/shadps4/video_core/texture_cache/sampler.cpp:28-33`: the DLSS mip bias assumes the game's own sizes in Neo.
- `gpu/shim/bbport_overlay.cpp:659-664`: in Neo with `SOTC_MAX_IN_PRO=0`, texture quality Maximum is blocked.
- `scripts/sotc_modes.py:303-311`: Neo forces the texture pool reserve to 0 (with the page allocator fix).
- `launcher/sotc_launcher_win.py:167` default `'neo': False`; `:682` `SOTC_NEO = 1` only when the output is not wider
  than 16:9 (`:683-684` writes a note otherwise); `:709-712` render-size override only when not Neo.
  `user/launcher_settings.json` has `"neo": false`.
- `scripts/sotc_modes.py` RENDER_SITES keep the Pro values next to the PS4 ones (comment at lines 77-78): the render
  size patch only rewrites the PS4 branch.

Game side (**proven**): the import `sceKernelIsNeoMode` (wrapper `0xf8fc40`) is called by `0x3f4e0` (`return == 1`),
`0x882ec0`, `0x882f10`, `0x882fb0`, `0x883010`, `0x883080`, `0x95a90`, `0x9aef0`, `0x9fa90`, `0xa1110`. The
`0x88xxxx` helpers latch the value once (`DAT_03294f20`, guard `DAT_03294f28`):
- `0x882ec0` (68 bytes): 1 on Neo, 0 base. Used by `0x97a50` (lines 107, 115, 173) and by many others.
- `0x882f10` (93 bytes): 911000000 on Neo, 800000000 base (unit and meaning unknown).
- `0x882fb0`: 16 on Neo, 8 base. `0x883010`: 2 on Neo, 1 base. `0x883080`: 4 on Neo, 2 base. Meaning unknown.

### T3.3 Which shader files: the `.csdr` suffix (`0xb0cc50`, `0xb0cb80`)

Proven code, `out/re/decomp/00b0/00b0cc50.c` lines 49-58:

```c
iVar4 = *DAT_03305d80;                  // cvar shaderTypeToLoad (EShaderTypeToLoad)
if (iVar4 == 3) iVar4 = 1 + 2*neo;      // PS4Neo_Shipping: 1 base (_shipping), 3 Neo (_neoshipping)
else if (iVar4 == 2) iVar4 = 2*neo;     // PS4Neo_Debug:    0 base (none),      2 Neo (_neo)
FUN_00b0cb80(out, name, iVar4);         // suffix = DAT_015d5820[iVar4]
// cvar 0 (Debug) and 1 (Shipping): index unchanged, Neo has no effect
```

`neo` is `FUN_0003f4e0()` (`0x3f4e0` = `sceKernelIsNeoMode() == 1`). Suffix strings (`shader-collections.md` 2b):
1 `_shipping.csdr`, 2 `_neo.csdr`, 3 `_neoshipping.csdr`.

Consequences:
- **proven**: Neo changes the loaded shader files only if the cvar is 2 or 3.
- **proven (log)**: the command line of every current run has `+shaderTypeToLoad=Shipping` (cvar 1). So the port loads
  `_shipping.csdr` with Neo on or off, and pretranslate (which predicts from the files loaded) needs no change for Neo.
- **inferred**: the cvar value comes from the game's own command line string. No file in `scripts/`, `src/`, `gpu/` or
  `out/cmdline.bin` sets it (grep).
- **proven (tools)**: `tools/csdr/predict_table.py:39` strips `(_[0-9a-f]+)?_shipping\.csdr$` and
  `tools/csdr/pack_csdr.py:37` keeps only `_shipping.csdr`. A `_neoshipping.csdr` file would keep `_neoshipping` in its
  pass name and would not be packed. This must change before any test with cvar 3.

### T3.4 Neo render targets

#### T3.4.1 Sizes at creation

Size table built in `0x95a90` (`local_a8`, 32 bytes, passed to `0x9aef0` at line 142) and unpacked in `0x9aef0` lines
462-469 (`iVar38`..`iVar6`). **proven**:

| Pair (variables in `0x9aef0`) | Base | Neo | Used by (kRTT calls, `0x9aef0` line) |
|---|---|---|---|
| pair0 `iVar38`, `iVar42` (table word 0x640 x 0x384) | 1600x900 | 1600x900 | UI_Offscreen_0 (2064), _1 (2078), _Fonts (2092) |
| pair1 `iVar2`, `iVar1` (0x780 x 0x438) | 1920x1080 | 1920x1080 | Display (633); source of the mode-0 derived records |
| pair2 `iVar3`, `iVar4` (`uVar9` x `local_94`) | 1920x1080 | 2560x1440 | MainColor (847), MainDepth (891), GBuffer family (1299-1385), Lighting (1635), PostProcessFinal (869), DOF (691-2000), and others |
| pair3 `iVar5`, `iVar6` (`uVar8` x `uVar4`) | 1920x1080 | 3840x2160 | RenderTargetDisplay (934), UI_Onscreen (2107) |

- Neo selection in `0x95a90`: `uVar4 = 0x870` and `uVar8 = 0xf00` (2160 and 3840) on Neo (lines 89, 93); `uVar9 = 0xa00`
  and `local_94 = 0x5a0` (2560 and 1440) on Neo (lines 127, 132). Base values are 0x438 and 0x780 throughout.
- Half and quarter tiers (`0x9aef0`, **proven**): `iVar33 = iVar3/2`, `iVar23 = iVar4/2` (lines 814-816); quarter
  `iVar34 = iVar3/4`, `iVar31 = iVar4/4` (lines 1150-1152). Neo main 2560x1440 / 1280x720 / 640x360; base 1920x1080 /
  960x540 / 480x270.
- **DisplayBuffer** (`0x95a90` lines 104 and 122): `FUN_00097a50(uVar8, uVar4, 1,1,1, 0x19, 3, 0, "DisplayBuffer")`.
  **3840x2160 on Neo**, 1920x1080 base. Two buffers (`param_1+6`, `+8`). Their 1080p view is `FUN_00096f70(.., 0x780, 0x438, 1)`
  (lines 106, 124) in both modes.
- **Heap** (`0x9aef0` lines 486-491): RTAllocator backing block `0x14280000` (322.5 MiB) base and `0x23d10000`
  (573.06 MiB, binary) on Neo. The log value 322.50 MiB is the base one. `docs/re-stutters/native/heaps.md:190` says
  "600 MiB", which is decimal.

#### T3.4.2 Samples, formats, descriptor

- **Samples: 1x everywhere (proven code).** `0x97a50` (2834 bytes) takes an eighth argument `p8`. On the depth path it
  is used as `uVar6 = p8` when `< 3` (lines 232-236) and written into the depth descriptor (`local_90`). Every kRTT
  creation call passes `p8 = 0` (grep of `0x9aef0`). `pass_layouts.txt` agrees: 48 of 68 rows have `num_samples = 1`
  (K1 section 7). No MSAA, 2x targets or checkerboard pairs are created at boot.
- **Formats: unchanged by Neo (proven copy, inferred for all records).** `0x96f70` (called from `0x9fa90` line 25 and
  `0x95a90` line 106) copies the descriptor (`+0xe4..+0x11c`) from its source object (K1 section 6). A derived target
  therefore has the format of its named object.
- **Neo bit in the colour descriptor (proven code, meaning unknown).** `0x97a50` lines 107, 115 and 173: `FUN_00882ec0()`
  (the Neo flag) goes into the descriptor for kinds 1 and 3 (`local_98`, then `FUN_0087dac0`), and two more bits come
  from the format-table flag word (`bVar25`, `bVar26`, lines 99-108). `docs/re-stutters/native/arch-next/engine-gnm-layer.md`
  calls `0x882ec0` the Neo gate for flag `0x10000000`. The meaning in the descriptor is **unknown**; an alt tile mode is
  possible (inferred). The old CHANGELOG entry at `CHANGELOG.md:1294` records a crash in Pro mode with `alt_tile` buffers.

#### T3.4.3 Derived sets per view mode (Neo only)

Writers (**proven**): `0x9fa90` (264 bytes; the whole body is under `if (sceKernelIsNeoMode() != 0)`, line 13), called
from the tail of `0x9aef0` (line 2182) before `0x9fba0` (2183) and `0xa0a60` (2185). The size records are written inline
by `0x9aef0` for each kRTT call. Example, MainColor (`0x9aef0` lines 847-853): mode-2 record `M+0xfcc..0xfd4` =
(pair0 w, pair0 h, 1); mode-0 record `M+0x48c..0x494` = (pair1 w, pair1 h, 1). Block layout: mode-0 records at
`M+0x480+12n`, mode-2 records at `M+0xfc0+12n`. Derived objects: mode 0 at `M+0xb40+i`, mode 2 at `M+0x1680+i`, with
`i` the named offset (K6.4). The derived objects are created once, in `0x9aef0`; a mode change does not rebuild them.
`0x9ae80` (111 bytes, a helper that writes the same records) has no caller in the decomp; the inline writes make it
unnecessary for this path (**unknown** whether it is dead code).

Derived sizes (from the record writes; script output checked by hand for MainColor and MainDepth):

| Class | kRTT (named offset) | Named, mode 1 (Neo) | Derived mode 0 (default) | Derived mode 2 | Base, all modes |
|---|---|---|---|---|---|
| main | MainColor (+0x8), MainDepth (+0x38), Normal_Roughness (+0x58), Albedo_AmbientOcclusion (+0x60), PBRProperties (+0x68), ExtraPBRProperties (+0x70), Ambient0 (+0x78), Ambient1 (+0x80), Lighting (+0x88), CollisionDepth (+0x50), Motion (+0xf8), SpecularIBL (+0x378), PostProcessFinal (+0x448), DOFResult (+0xb8) | 2560x1440 | 1920x1080 | 1600x900 | 1920x1080 |
| half | MainDepth_HalfRes (+0x40), SSAO (+0x1f0), SSAO_Accumulation_A/B (+0x1f8, +0x200), SSAOTemp (+0x208), AmbientShadowRG_SSAOB (+0x210), LinearDepthMipped (+0x308), LinearNearDepth (+0x358), MainColorCopy (+0x218), PostColorHalfRes_A/B (+0x98, +0xa0), FullscreenBlur_0/1 (+0x450, +0x458), MotionBlur_Vel/MagDepth (+0x100, +0x108), AlphaDofAmount_HalfRes (+0x20), ParticleColor_HalfRes (+0x28) | 1280x720 | 960x540 | 800x450 | 960x540 |
| quarter | ParticleColor_UltraLowRes (+0x30), ParticleBlocker (+0x188), MotionBlur_Tile_Horiz/Vert/Final (+0x110, +0x118, +0x120) | 640x360 | 480x270 | 400x225 | 480x270 |
| display | RenderTargetDisplay (+0x90) | 3840x2160 | 1920x1080 | 1600x900 | 1920x1080 |
| display | UI_Onscreen (+0x460) | 3840x2160 | 1920x1080 | 1920x1080 | 1920x1080 |
| display | Display (+0x1b00) | 1920x1080 | none | none | 1920x1080 |

Not resolved: the derived records of UI_Offscreen_0/1/Fonts (the script output mixes tiers for them). Their named size is
1600x900 in both modes.

Set consequences (**proven** for the order, **inferred** for the result):
- The sets of K5/K6 keep their membership. The object bound to a slot is the first non-null of: primary `M+0x1b00+i`,
  then (Neo, mode 0 or 2 only) the derived object, then the named object (`0x9fba0` lines 20-41, K5.3, K6.4).
- Mode 0 (default, raw R = 3): derived A, 1080p main targets. Mode 1 (raw R = 2): named, 1440p main targets and 4K
  display. Mode 2 (raw R = 1): derived B, 900p main targets.
- Base mode: no derived objects exist (`0x9fa90` is gated), so every mode resolves to the named 1080p objects. The
  mode changes nothing except the rebuild of the same sets.

#### T3.5 View mode `M+0x3b70` under Neo

- Raw R (`0x90860`, lines 193-200; K6.5): `S+0x4b8` if non-zero, else `S+0x4c0` if non-zero, else 3. Passed to
  `0x95690` (line 200). **proven**.
- `0x95690` (lines 8-16): returns when R is unchanged; stores R at `M+0x3c`; mode = 2 for R = 1, 1 for R = 2, else 0;
  calls `0xa1cd0(M, mode)`. **proven**.
- Boot value: `0x95a90` line 142 passes `param_5 = 0`, and `0x9aef0` line 459 stores it in `M+0x3b70`: mode 0 at
  creation. **proven**.
- Writers of the raw value: `0xe0db0` (lines 102-105) copies `param_2+0x80`, `+0x88`, `+0x98` to `S+0x4b4`, `+0x4bc`,
  `+0x4c0`; `0x195370` calls it (lines 203-210). Writers of `S+0x4b8`: only the zeroing in `0xd0950` (line 86); no
  non-zero writer found. **inferred**: the raw R is the saved graphics preset (options field `+0x98`), or 3 if it is 0.
  The identity of `S` and of the options struct is **unknown**.
- Preset names: the engine has three preset suffixes for its cvars: `_BasePS4`, `_Perf`, `_Cinematic`
  (`docs/re-stutters/reports2/14-draw-sources.md` section 2.6; strings `0xf9cba6`..`0xf9cbfe`). The port already sets
  `progSamplerAnisoLevel_BasePS4/_Perf/_Cinematic` (`scripts/sotc_modes.py:73-74`). The game's menu option is
  "Cinematográfico" or "Desempenho" (`CHANGELOG.md:1247-1249`; `docs/sotc/README-SotC.md:56-58`). The 60 FPS mode uses
  the game's Desempenho mode (`CHANGELOG.md:1032`).
- **Inferred preset mapping** (not proven; check in T3.8):
  - raw 2 = mode 1 = named 1440p = `_Cinematic` (matches the comment in `src/runtime_sotc.c:45-46`, "Cinematic (1440p)");
  - raw 1 = mode 2 = 900p derived = `_Perf`;
  - raw 3 (default) = mode 0 = 1080p derived = `_BasePS4`, the PS4 size. This fits the CHANGELOG remark that Pro only
    unlocks the menu and the player must pick Cinematic or Desempenho (`CHANGELOG.md:1247-1249`).
- Frequency of R changes during play: **unknown** (K6.9.5).
- Menu strings: none found in the decomp (the labels are probably localized data or built by concatenation). **unknown**.

### T3.6 What this means for the port

1. **Today: base.** The port loads `_shipping.csdr`, sizes everything at 1920x1080 (1600x900 for UI offscreen), uses a
   322.5 MiB RTAllocator and creates no derived sets.
2. **Neo (`SOTC_NEO=1`).** The Pro sizes exist in mode 1 (Cinematic, inferred). The default mode 0 renders the main scene
   at 1920x1080 through derived targets. So `SOTC_NEO=1` alone does not give 1440p; the preset must also be Cinematic
   (inferred). The 4K display buffers and the 573 MiB heap are made whatever the preset.
3. **Pipeline key: no size field.** `GraphicsPipelineKey` (`gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h:38-57`)
   holds the stage hashes, `num_color_attachments`, `color_buffers` (formats), blend and write masks, `num_samples`,
   `depth_samples`, `color_samples`, z and stencil formats and the primitive type. The render-pass part of the hash
   (`vk_graphics_pipeline.cpp:550-556`) adds colour formats, colour samples and depth/stencil formats. Viewport and scissor
   are not in it. So a Neo target with the same format and 1x samples uses the same pipeline.
4. **Extra variants for Neo: none from sizes, formats or samples.** The Neo variants that exist are:
   - (a) a separate shader cache folder `<serial>_neo` (`cache_storage.cpp:169-172`): the pipeline prebuild must run once
     more for Neo; base prebuilt pipelines are not reused.
   - (b) with `shaderTypeToLoad = 3` (not used today), the `_neoshipping.csdr` binaries are loaded and the predict and
     pack tools miss them (T3.3).
   - (c) the Neo colour-descriptor bit (T3.4.2) changes the target descriptor. If it changes the tile mode, the port's
     colour decode changes too. **inferred**; it is not a pipeline-key field.
5. Pretranslate needs no change while the cvar is 1: it predicts from the files loaded.

### T3.7 Open items

1. The raw preset field: which options field feeds `S+0x4c0` (`+0x98`), what `S` is, and the writer of a non-zero
   `S+0x4b8` (none found).
2. The preset mapping (raw value to `_BasePS4`, `_Perf`, `_Cinematic`) and the menu strings. Check by run (T3.8).
3. Meaning of the Neo constants: `0x882f10` (911000000 / 800000000), `0x882fb0` (16 / 8), `0x883010` (2 / 1),
   `0x883080` (4 / 2), and the colour-descriptor bit set by `0x97a50` from `0x882ec0`.
4. The derived records of UI_Offscreen_0/1/Fonts (not resolved).
5. Whether the `shaderTypeToLoad` value 1 comes from the eboot default or from the game's options (no writer found in the
   port).
6. The caller of `0x9ae80`, and whether it is dead code.

### T3.8 One-run log spec (what stays open)

Run with `SOTC_NEO=1`, no draw change, existing logging or RES_LOADED-style hooks only. Start at the Shrine with the
default preset, then switch the menu preset to Cinematic, then to Perf, 10 s each.

- At `0x95a90` return: the two DisplayBuffer objects' width and height (object `+0x198`, `+0x19c`, K1). Expect 3840x2160.
- At `0x9aef0` return, with `M = *(DAT_02ed3670 + 0x80)`: width and height of the named objects at `M+0x8, +0x38, +0x58,
  +0x60, +0x78, +0xf8, +0x378`, and of the derived objects at `M+0xb40+i` and `M+0x1680+i` (i = the named offset). Expect
  named 2560x1440, derived mode 0 1920x1080, derived mode 2 1600x900.
- At `0x90860`: `S`, `S+0x4b8`, `S+0x4c0` and R on each call (log changes only). Map each preset to R.
- At `0xa1cd0` entry: old and new `M+0x3b70`, with the frame number.
- At the first call of `0x882ec0`: its return value (expect 1). For one colour target, the descriptor flag word that
  `0x97a50` writes at line 173.
- At `0xb0cc50` line 49: the value of `*DAT_03305d80` (expect 1), and the names of the `.csdr` files the loader opens.
- Pass log (existing `SOTC PASS` line): at the first `GBufferAmbientDynamic` draw, the size of the bound colour object
  (set `+0x2a40`, slot 0).

Pass criteria: the three presets map to the three size classes (main 1920x1080, 1600x900, 2560x1440). If a preset does
not change the main size, T3.5 is wrong. If `shaderTypeToLoad` reads 3 or 2 with Neo on, T3.3 is wrong.

### T3.9 Evidence list (T3)

| Item | Address | File and line |
|---|---|---|
| Neo switch, port | `runtime_sotc_neo`, `is_neo_mode`, table entry | `src/runtime_sotc.c:45-55, 472` |
| Neo, GPU library | `sceKernelIsNeoMode` | `gpu/shim/bbgpu.cpp:383` |
| Neo cache folder | `_neo` suffix | `gpu/shadps4/video_core/cache_storage.cpp:169-172` |
| Neo launcher | default and SOTC_NEO | `launcher/sotc_launcher_win.py:167, 682-684, 709-712` |
| Neo import wrapper, helper | `0xf8fc40`, `0x3f4e0` | `out/re/decomp/00f8/00f8fc40.c`, `0003/0003f4e0.c` |
| Latched Neo helpers | `0x882ec0`, `0x882f10`, `0x882fb0`, `0x883010`, `0x883080` | `out/re/decomp/0088/` |
| csdr suffix choice | `0xb0cc50` (lines 49-58), `0xb0cb80` | `out/re/decomp/00b0/00b0cc50.c`, `00b0cb80.c` |
| csdr port tools | regex | `tools/csdr/predict_table.py:39`, `tools/csdr/pack_csdr.py:37` |
| Creation, Neo sizes | `0x95a90` (lines 74-142) | `out/re/decomp/0009/00095a90.c` |
| Creation, size pairs, derived inline writes | `0x9aef0` (lines 459, 462-469, 486-491, 633-2107, 2182-2185) | `out/re/decomp/0009/0009aef0.c` |
| Derived writer (Neo only) | `0x9fa90` (line 13) | `out/re/decomp/0009/0009fa90.c` |
| Records helper, no caller | `0x9ae80` | `out/re/decomp/0009/0009ae80.c` |
| Colour descriptor with Neo bit | `0x97a50` (lines 99-108, 107, 115, 173, 232-236) | `out/re/decomp/0009/00097a50.c` |
| View mode | `0x90860` (193-200), `0x95690` (8-16), `0xa1cd0` | `out/re/decomp/0009/00090860.c`, `00095690.c`, `out/re/decomp/000a/000a1cd0.c` |
| Raw R writers | `0xe0db0` (102-105), `0x195370` (203-210), `0xd0950` (86) | `out/re/decomp/000e/000e0db0.c`, `0019/00195370.c`, `000d/000d0950.c` |
| Pipeline key | `GraphicsPipelineKey`, render-pass hash | `gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h:38-57`, `vk_graphics_pipeline.cpp:550-556` |
| Preset suffixes | cvar variants | `docs/re-stutters/reports2/14-draw-sources.md` 2.6; `scripts/sotc_modes.py:73-74` |
| Menu and mode notes | Pro and Desempenho | `CHANGELOG.md:1032, 1247-1249, 1294`; `docs/sotc/README-SotC.md:56-58` |
| Heap note | 600 MiB (decimal) | `docs/re-stutters/native/heaps.md:190` |
| Run logs | `sceKernelIsNeoMode -> 0`, RTAllocator 322.50 MiB, `+shaderTypeToLoad=Shipping` | `user/last_run.log`, `last_run.1..4.log` |


## Neo bit and presets (T10)

Topic: closes the two open items of T3 (T3.7 items 1-3, T3.8). Read-only static analysis of the EU 1.01 eboot export
(`out/re/decomp`) and the port source. Addresses are eboot VAs. Labels as in section 1: **proven** (decomp or source read,
address or file:line given), **inferred**, **unknown**. Nothing was run. The `S`, `M`, `V` names follow K5/K6 and T3.

### T10.1 Answer in short

| # | Question | Answer | Label |
|---|---|---|---|
| 1 | What does `0x882ec0` return? | A latched copy of `sceKernelIsNeoMode() == 1`: 1 on Neo, 0 base. Guarded once by `__cxa_guard` (guard byte `DAT_03294f28`, value `DAT_03294f20`). | proven |
| 2 | Which descriptor field does the Neo flag change? | Colour targets (kinds 1 and 3, every kRTT colour target made with p5 = 1): the flag goes into spec field `[6]`, and `0x87dac0` puts it in **CB_COLORn_INFO bit 31**, the port's `Color0Info.alt_tile_mode` (`regs_color.h:172`). It also writes dword 6 of the descriptor (`CB_COLORn_DCC_CONTROL`, 0x31E) = 8. | proven (code and field names); the 0x31E meaning inferred |
| 3 | Is it `ALT_TILE_MODE`? | Yes for the colour path. The port already reads that bit: `ImageInfo(ColorBuffer)` sets `alt_tile = sceKernelIsNeoMode() && info.alt_tile_mode` (`image_info.cpp:90`). | proven |
| 4 | Does the port mis-detile or mis-key a Neo colour target? | Mis-key: no. The bit is part of the image identity (`texture_cache.cpp:638`, `:922`), not of the pipeline key. Mis-detile: possible, not proven. The colour `ImageInfo` leaves `bank_swizzle = 0` and the detiler never reads `alt_tile`, while the texture (T#) path computes a non-zero bank swizzle for the same bit (`resource.h:281`). Runtime check in T10.8. | proven (code); risk inferred |
| 5 | Depth (`DB_Z_INFO`) alt bit? | None in the port (`regs_depth.h` has no alt field). The Neo effect on depth goes through the tile-query arguments and tile mode (T10.4), inferred. | proven (no field); effect inferred |
| 6 | Source of raw R (T3) | Not the options object as T3 wrote. `S+0x4c0` is copied by `0xe0db0` from its second argument `+0x98`, and that argument is a per-model-group source record built in `0x195370`. The user's graphics cell is a separate object (`DAT_032454b8 + 0x60`). The link between the two is open. | proven (copy); link unknown |
| 7 | Values of R and presets | The graphics cell takes 3 (base, forced by `0x70a6d0`), and 2 or 1 on Neo. R takes the same value set (1, 2, 3; 0 = unset, read as 3). Which of 1 and 2 is Cinematic is not proven. T3's mapping (raw 2 = Cinematic 1440p, raw 1 = Perf 900p, raw 3 = BasePS4 1080p) stays inferred; the launcher menu texts fit it. | proven (values); names inferred |
| 8 | Does `sotc_modes.py` force a preset? | No. It writes `progSamplerAnisoLevel*` (all four suffixes, `af16`) and `TargetFramesPerSeconds=1` (`fps60game`). It does not write the graphics cell, setting `0x25`, `S+0x4b8` or `S+0x4c0`. | proven |

### T10.2 `0x882ec0`: the Neo gate

`out/re/decomp/0088/00882ec0.c` (68 bytes):

```c
if (DAT_03294f28 == 0 && __cxa_guard_acquire(&DAT_03294f28)) {
    DAT_03294f20 = (sceKernelIsNeoMode() != 0);
    __cxa_guard_release(&DAT_03294f28);
}
return DAT_03294f20;   // 0 or 1
```

- Latched once per process. The port's value is fixed at first call (`src/runtime_sotc.c:45-55`), so a mid-run change of
  `SOTC_NEO` has no effect. proven.
- Callers include `0x87dac0`, `0x97a50`, `0x866fa0`, `0x866f50`, `0x87da70`, `0x882f10`-`0x883080` and many others (file header).
- The sibling helpers in the same latch give constants (T3.2): `0x882f10` 911000000 on Neo, 800000000 base; `0x882fb0`
  16 / 8; `0x883010` 2 / 1; `0x883080` 4 / 2. Inferred: `0x882f10` is a clock in Hz (the public PS4 Pro GPU clock is
  911 MHz, base 800 MHz). The other three are not identified.

### T10.3 Colour target: where the Neo flag goes (kinds 1 and 3)

`0x97a50` (`out/re/decomp/0009/00097a50.c`), colour block `LAB_00098014` (kind `p7` in {1, 3}, entered when `p5 == 1`):

```c
FUN_0087da70(&spec);                     // defaults: spec[6] = FUN_00882ec0(), spec[7..9] = 0  (0x87da70, proven)
spec[0] = p1; spec[1] = p2;              // width, height
spec[2] = 0;
spec[3] = uVar10;                        // p4, or p4*6 when uVar19 == 1 (cube-like)
spec[4] = iVar11;                        // format: DAT_02ed3680[p6*2 + srgb]
spec[5] = local_7c[0];                   // tile mode index
spec[6] = FUN_00882ec0();                // line 173: the Neo flag
spec[7] = 0; spec[8] = 0;                // 1x samples (T3.4.2)
spec[9] = (spec[9] & ~3) | ((uVar4 >> 16) & 0xff);   // line 176: low 2 bits from the kRTT flag word
FUN_0087dac0(desc = target+0xe4, &spec);  // colour descriptor builder, line 178
```

Spec layout as the callee reads it (`0x87dac0`, `0x87da70`; proven from the field uses, names inferred):

```c
struct ColorSpec {         // 40 bytes, dword index in brackets
  u32 width;               // [0]  1..0x4000 (checked)
  u32 height;              // [1]  1..0x4000 (checked)
  u32 zero;                // [2]  0 (checked: <= 0x4000)
  u32 slice_max_arg;       // [3]  -> VIEW.slice_max (bits 13..23), scaled by 0x2000
  u32 format;              // [4]  low byte = AMD DataFormat (checked 1..0x16), -> INFO.format
  u32 tile_mode;           // [5]  -> ATTRIB.tile_mode_index (bits 0..4)
  u32 neo;                 // [6]  == 1 on Neo -> INFO bit 31 (alt_tile_mode); dword 6 = 8
  u32 samples_log2;        // [7]  -> ATTRIB bits 12..14 (num_samples_log2); 0 here
  u32 fragments_log2;      // [8]  -> ATTRIB bits 15..16; 0 here
  u32 flags;               // [9]  bit 0: width/height aligned to 8; bits 0..1 set here from the kRTT flag word;
                           //      bit 4 (Neo DCC branch) never set on this path
};
```

What `0x87dac0` (`out/re/decomp/0087/0087dac0.c`, 982 bytes) writes into the descriptor (proven, fields by address):

| Descriptor dword (byte) | Field (port `ColorBuffer`) | Written when | Value |
|---|---|---|---|
| 4 (0x10) | `info` (CB_COLORn_INFO, 0x31C) | tile query `0x885f90` returns 0 | `(spec[6] == 1) << 31 \| (old & 0x7fffffff)`: bit 31 = Neo |
| 3 (0x0c) | `view` (0x31B) | after the checks | `spec[3] * 0x2000`, masked to bits 13..23 |
| 6 (0x18) | `pad0` (0x31E) | `spec[6] == 1` (else branch) | `8` |
| 6 (0x18) | (same) | Neo and `spec[9] & 0x10`: not reached on colour targets | DCC bits (`uVar6`, `uVar7 * 4`) |

Points that follow from the code:
- The Neo branch of the first test (`Neo && spec[6] == 0`, which bails when `spec[9] & 0x13`) is never taken by the colour
  path, because it always passes `spec[6] = Neo`. proven (decomp lines 167-178).
- `spec[9] & 0x10` is never set on the colour path: `spec[9]` starts at 0 (`0x87da70`) and only bits 0..1 are written from
  the kRTT flag word. So the Neo DCC branch is dead for colour targets. proven for the code; the kRTT flag word was not
  decoded, so this holds for every colour call that goes through `0x97a50`.
- So a Neo colour target gets INFO bit 31 = 1 and dword 6 = 8; a base target gets 0 and 0. proven for the code paths.
  Inferred: `0x87dea0` (format write), `0x87dfe0` and `0x87e150` do not overwrite dword 6 (not read).

The other `0x97a50` branches (`LAB_0009819c`, `0x8806d0` / `0x8806f0`, kinds not 1/3 or `p5 != 1`) build their own specs
(`spec[9] = FUN_00882ec0()` at lines 302 and 319). Their descriptors were not traced: unknown whether they change a tile or
alt bit.

### T10.4 Depth target: Neo inputs (kind 2)

`0x97a50` depth path (`p7 == 2`, `p5 == 1`). The spec is built by `0x866f50` (76 bytes) and then rewritten at lines 235-247:

- `spec[6]` = the tile mode `local_7c[0]`. Not the Neo flag: the `0x866f50` default (`Neo * 2`) is overwritten. proven.
- `spec[7]` = `FUN_00882ec0()` (lines 242, 245). proven.
- `spec[8]` = `p8` if below 3, else 0 (samples; always 0 at the kRTT calls, T3.4.2). proven.
- `spec[9]` = `((bVar25 & bVar27) << 1) | (uVar4 >> 24)`. `bVar25` is Neo AND the template gate (line 107), `bVar27 = (p6 != 10)`.
  proven for the expression; the template byte meaning is unknown.

Consumers, `0x866fa0` (`0086/00866fa0.c`, 1148 bytes):

- Gate (lines 4-6): bail if `Neo && spec[7] == 0 && (spec[9] & 1) != 0`. On Neo `spec[7] = 1`, so the gate passes. proven.
- `if (spec[7] == 1 && (~spec[9] & 3) == 0) tile = 4;` (uVar4 forced to 4 when `spec[9] & 3 == 3`). On base `spec[7] = 0`, so
  this never fires. proven (code). This is the only Neo-dependent tile choice on the depth side.
- `uStack_b4 = (spec[7] == 1) * 0x2000 + 2`, passed to `0x885f90` (the tile query). The meaning of `0x2000` is inferred.
- The depth block at `target+0x164` gets `DB_Z_INFO` dword 0 = `(spec[8] & 3) << 2 | (spec[4] & 3)` (T3 and K1 table).
- `0x97a50` lines ~296-300: when `bVar25 & bVar27`, the target dword at `+0xe0` gets `plVar12[0x31]` (a stencil or HTILE base
  copied from `+0x188`) and byte `+0xde` gets bit 0x20. Meaning inferred (a Neo-only copy of an HTILE-like base).

Port side: `DepthBuffer` (`regs_depth.h`) has no alt bit, so the depth Neo path reaches the port only through the tile mode the
game writes to the `DB_*` registers (and the HTILE base), not through a flag. Inferred. A grep of `gpu/shadps4/video_core` for
`neo`/`alt` finds only colour and texture uses.

### T10.5 Register mapping and the port

Port layout (`gpu/shadps4/video_core/amdgpu/regs_color.h:151-174`). The bit positions were summed to check: endian 0-1,
format 2-6, linear_general 7, number_type 8-10, comp_swap 11-12, fast_clear 13, compression 14, blend_clamp 15,
blend_bypass 16, simple_float 17, round_mode 18, cmask_is_linear 19, blend_opt_dont_rd_dst 20-22,
blend_opt_discard_pixel 23-25, fmask_compression_disable_ci 26, fmask_compress_1frag_only 27, dcc_enable 28,
cmask_addr_type 29-30, alt_tile_mode 31. That is 32 bits, so bit 31 is `alt_tile_mode`.

Port handling of the bit (all proven by reading):

| Place | What it does | Effect on a Neo colour target |
|---|---|---|
| `texture_cache/image_info.cpp:90` | colour `ImageInfo`: `alt_tile = sceKernelIsNeoMode() && buffer.info.alt_tile_mode` | Set to 1 (game bit 1 on Neo). |
| `image_info.cpp:76-91` (ctor) | `guest_size = GetColorSliceSize() * layers`, mip 0 from that size; no `UpdateSize()` call in this ctor | The alt bit does not change the colour layout here (size comes from the game's slice field). |
| `texture_cache/texture_cache.cpp:638`, `:922` | image identity (hash and match) and the overlap rule: a different `alt_tile` at the same address is an overlap by tiling and frees or merges the older image | A T# read with a different `alt_tile` of the same surface does not match the colour image. |
| `texture_cache/image_info.cpp:194`, `amdgpu/resource.h:183` | T# `alt_tile = Neo && image.alt_tile_mode` | Same bit for textures. A match depends on the game setting the T# bit like the CB bit (not checked). |
| `amdgpu/resource.h:281` `Image::GetBankSwizzle()` | non-zero only for a T# with alt tile and macro tiling | T# path only. |
| `image_info.cpp:233` `RebaseToMip` | refuses alt-tile images | Not used for colour (single mip). |
| `texture_cache/tile_manager.cpp:203`, `:294` | the detiler uses `info.bank_swizzle` and the mip layout; `alt_tile` is not read | A colour `ImageInfo` has `bank_swizzle = 0`, so a detile of a Neo colour target uses base geometry and no swizzle. |
| `texture_cache/tile.h:292-293` `GetMacroTileExtents(..., alt)` | alt macro tile extents, used by `ImageSizeMacroTiled` (`image_info.cpp:306`) | Used when mip sizes are computed by `UpdateSize()` (T# and buffers). Not on the colour ctor path. |

Verdict (inferred from the table; to be checked at run time in T10.8):
- Pipelines: no effect. `GraphicsPipelineKey` has no size, tile or alt field (T3.6 item 3).
- Image identity: consistent only if the game's T# bit equals the CB bit. On Neo, colour targets are `alt = 1`; a later
  texture read of the same surface is `alt` from its own T# bit. If the two differ, the overlap rule (`texture_cache.cpp:922`)
  creates a second image or frees one.
- Detile on read-back: a risk. A colour target that is later read through the guest path (upload or download through
  `DetileImage`) uses `bank_swizzle = 0` and base macro-tile geometry, while the T# view of the same memory computes a swizzle
  and alt extents. Neither is checked against the GPU's own write layout. The port does not show a Neo colour target being
  detiled today; the risk is for any guest round trip.

### T10.6 Presets: the graphics cell, the menu, and the values

Proven chain (addresses in the decomp file names):

1. The settings store: `FUN_007dbae0(id)` reads and `FUN_007dbab0(id, value)` writes a numeric setting. `0x70a6d0` calls
   `FUN_007dbab0(0x25, param_1)` first, so setting 0x25 is the Pro mode toggle `t` (inferred from that call).
2. `0x70a6d0` (`out/re/decomp/0070/0070a6d0.c`, 725 bytes), with `t` its argument:
   - Not Neo (`FUN_0003f4e0() == 0`): if the graphics cell (`**(int **)(DAT_032454b8 + 0x60)`) is not 3, set it to 3
     (`FUN_0006a3a0`); then `FUN_0025e870` with 3; the mode is 3.
   - Neo, `t == 0`: the cell becomes 2 (unless already 2); mode 2.
   - Neo, `t != 0`: the cell becomes 1 (unless already 1); mode 1.
   - Then `FUN_0005bb40(&DAT_02ea8ee8, mode)` (`0x5bb40`, 105 bytes): mode 1 uses `DAT_02eb30f0` / `DAT_02ebe598`, mode 2
     uses `DAT_02eb2f40` / `DAT_02ebe3e8`, anything else (3) uses `DAT_02eb3018` / `DAT_02ebe4c0`, and writes `+0x1e8` and
     `+0x49c`. proven (code). The three records are a per-mode table; their values were not read.
3. Callers that set `t` (menu and load): the menu case `0x3d` (`0x559a10:127`, `0x558b20:385`, argument `bVar3 ^ 1` or
   `param_2[1]`); `0x70a9b0:60` (`*(byte *)(DAT_02ed3670 + 0x3a) ^ 1`, after writing the cell through `FUN_000698c0`);
   `0x70ac50:40` (`FUN_007dbae0(0x25)` read back, after writing the cell with `(iVar3 != 0) + 1`). The sign of `t` against the
   cell differs between `0x70ac50` and `0x70a6d0` as I read them (`0x70ac50` writes 1 when `iVar3 == 0`, which `0x70a6d0`
   maps to mode 1 only when `t != 0`); unknown which value of `t` is the menu's "Cinematic" item.
4. The cell's value set is therefore {3 base; 1, 2 on Neo}. The launcher's menu text (`launcher/sotc_launcher_win.py:1353-1355`,
   "Options > Presentation: Cinematic (1440p) or Performance") names the two Neo modes, which are the two non-default values.

Where R comes from (T3 correction):

- `0x90860` (`0009/00090860.c` lines 193-200): `R = S->+0x4b8`, else `S->+0x4c0`, else 3, with `S = param_2[9]`. proven.
- `0xe0db0` (`000e/000e0db0.c` lines 102-105) copies `param_2+0x88` to `+0x4b0` and `+0x4bc`, `param_2+0x80` to `+0x4a8` and
  `+0x4b4`, and `param_2+0x98` to `+0x4c0` (`+0xa0` to `+0x4c8`, `+0xac` to `+0x4d4`). proven.
- The caller `0x195370` (`0019/00195370.c` line 171) passes `&local_620` as `param_2`. `0x196260` (line 93) fills that local
  as a vector (begin, end, capacity) of the model-group source records at `lVar6 + 0x60`, where `lVar6 = FUN_0014d7b0(param_2 + 0x28)`.
  So `+0x98` is a field of a model-group source record (from loaded data), not the options object that T3 named. The
  decomp's typing of `local_620` is inconsistent (it is a vector, yet `0xe0db0` reads fields 0x20..0xb0 of it), so the exact
  source is open.
- The record fields `+0x4b8` and `+0x4c0` of the 0x530-byte group record are then set from the locals `uStack_b0` and
  `uStack_a8` (`0x195370` lines 203-204), which have no visible assignment in the decomp. Unknown (a decompile artefact, or a
  write through an alias).
- No non-zero writer of `S+0x4b8` was found (T3). `0xd0950` line 86 only zeroes it.

So R is read per camera build from a model-group record, and its value set is {1, 2, 3}. The link from the user's graphics cell
(values 1-3) to that record is unknown. The cell is the only place where the game writes 1/2/3 from a user choice, which makes
it the likely source, but no code path was found that copies it into the group record.

Cvar suffixes:

- `RegisterXprCvars` (`0x6efe0`, 112 KB; the decompile timed out, and `out/re/decomp/0006/0006efe0.c` is a stub). The suffix
  strings are referenced only from it: `progSamplerAnisoLevel_Cinematic` `0xf9b86c`, `_BasePS4` `0xf9b88c`, `_Perf` `0xf9b8aa`;
  `r_virtualGeometryNumShadowCascades_Cinematic` `0xf9cba6`, `_BasePS4` `0xf9cbd3`, `_Perf` `0xf9cbfe`
  (`tools/re/eboot_re.py str`). proven (string references).
- The lookup that appends the suffix at read time is not located. `0x6efe0` is the registration; the choice of variant at read
  time is inside code that was not decompiled. T3's "name suffix from the preset" is therefore unverified.
- The port's launcher writes all four variants of the grass cascade count (`launcher/sotc_launcher_win.py:649-651`, comment:
  "the game takes the one of its graphics mode"). That fits an engine choice by mode, not a port choice.

Port (`scripts/sotc_modes.py`, `src/runtime_sotc.c`):

- No preset is forced. `CVAR_MODES` writes `TargetFramesPerSeconds=1` (`fps60game`, "until the options code runs") and
  `progSamplerAnisoLevel{,_BasePS4,_Perf,_Cinematic}=16` plus `linearSamplerAnisoLevel=16` (`af16`). The 60 FPS code patches
  (`0xb0a7a6`, `0x89a47f`, `0x89a499`) are not in `0x70a6d0`. proven.
- The graphics cell, setting `0x25`, `S+0x4b8` and `S+0x4c0` are not written by the port (grep of `scripts/`, `gpu/sotc/`,
  `src/`, `launcher/`). proven.
- `SOTC_NEO=1` makes the game take the Neo branch of `0x70a6d0` on its own (cell 2 or 1 by `t`). The port does not choose.

Preset mapping, status:

| Raw value | Named size (`0x95690`) | Presumed preset | Status |
|---|---|---|---|
| 3 (default, base) | derived mode 0, 1080p main | `_BasePS4` | 3 is proven as the base and default value (`0x70a6d0`, `0x5bb40`, `0x90860`); the name is inferred. |
| 2 | named mode 1, 1440p main (4K display) | `_Cinematic` | value proven as one of the two Neo values; name inferred (matches the launcher text "Cinematic 1440p"). |
| 1 | derived mode 2, 900p main | `_Perf` | value proven as the other Neo value; name inferred ("Performance", 60 FPS). |

T3's mapping is kept as inferred and not confirmed: the value set is right, but the names depend on the menu toggle `t` and on
how R is fed from the cell, and neither is proven. The "options +0x98" source in T3 is corrected (R source above).

### T10.7 Corrections to T3

1. T3.4.2 "meaning unknown; an alt tile mode is possible". Now proven: CB_COLORn_INFO bit 31 = `alt_tile_mode`, set from the
   Neo flag through spec `[6]` (T10.3). The "two more bits from the format-table flag word" are not from the table:
   `bVar25` and `bVar26` come from the kRTT spec template bytes and act on the depth path only (T10.4).
2. T3.5 "raw R is the saved graphics preset (options field +0x98)". Not supported. `+0x98` is a model-group source field
   (T10.6). The graphics cell is a separate object (`DAT_032454b8 + 0x60`), and the link is open.
3. T3.5 preset mapping: values confirmed as a set, names still inferred (T10.6 table).
4. T3.1 row 7, "the Cinematic preset (raw 2) gives the named 1440p set": the size part is proven (`0x95690` maps 2 to mode 1,
   named 1440p); the preset part is inferred.
5. T3.7 item 3, meaning of the Neo constants: `0x882f10` 911000000 / 800000000 is probably the GPU clock in Hz (inferred).

### T10.8 Refined one-run log spec (exact hook points and bytes)

Run once with `SOTC_NEO=1`, no draw change. Hooks at the VAs below (entry unless a return is named). Log only changes for the
variable ones. Start at the Shrine, switch the menu toggle to each option, 10 s each (as T3.8).

Colour descriptor (settles T10.3):
1. `0x882ec0` return: `eax`, and the byte at `0x3294f20` (`DAT_03294f20`). Expect 1 with `SOTC_NEO=1`.
2. `0x87dac0` entry: dump 40 bytes at `param_2` (the spec, 10 dwords). Expect `spec[6] == 1`, `spec[7] == 0`, `spec[8] == 0`
   for kind 1/3 calls. Also dump `spec[9]`; expect 0..3 (bit 4 clear).
3. `0x86a1a0` entry (writes CB_COLORn with the `0xC00E6900` packet): dump 8 dwords of the descriptor argument. Expect dword 2
   (INFO) bit 31 = 1 and dword 6 (0x31E) = 8 for every Neo colour target. Base run: both 0.
4. `0x86a1a0` packet (18 dwords): confirm the register offsets 0x318 + 15n, and dwords 2 and 6 at packet offsets +8 and +24.
5. Port side (existing log, no hook): the colour `ImageInfo` constructor (`image_info.cpp:90`) with `alt_tile` and
   `tile_mode`, and the existing `Alt tile:` lines of the overlap log (`texture_cache.cpp:955-1025`, `:984`). Expect
   `alt tile: true` for Neo colour targets. An `Unresolvable image overlap` line after a Neo run means the T# and CB bits differ.

Depth (settles T10.4):
6. `0x866fa0` entry: dump 40 bytes at `param_2`. Expect `spec[7] == 1` on Neo (0 base). Count how often `spec[9] & 3 == 3`
   occurs (the forced tile 4).
7. `0x866f50` entry: dump the spec it creates; expect `spec[4] == 3` (Z32F) or 1 (Z16).

Presets (settles T10.6):
8. `0x70a6d0` entry: log `param_1` (the setting 0x25 value), and the cell `*(int **)(DAT_032454b8 + 0x60)` before and after
   (expect 3 on base, 2 or 1 on Neo).
9. `0x5bb40` entry: `param_2` (1, 2 or 3) and the frame number.
10. `0x90860` entry: `S = param_2[9]` (pointer); dwords at `S+0x4b4`, `S+0x4b8`, `S+0x4bc`, `S+0x4c0`; the value of `iVar9`
    (R); and the graphics cell. Log changes only. This tests the link: if R follows the cell, the link exists; if R stays
    at 3 while the cell is 1 or 2, the link is broken and the T10.6 source stands.
11. `0x195370` after the `memcpy` at line 193 (the 0x480-byte copy into the group record): dump the group name (record `+0x480`)
    and the dwords at `+0x4b8` and `+0x4c0` of each record. This settles the `uStack_b0` / `uStack_a8` question and shows the
    source of R directly.
12. `0xa1cd0` entry: old and new `M+0x3b70`, with the frame number (T3.8).
13. Preset names without decoding `0x6efe0`: set three distinct values through `engine_cvars` (for example
    `r_virtualGeometryNumShadowCascades_BasePS4=1`, `_Perf=2`, `_Cinematic=4`, others unchanged) and log which value the
    cascade count takes in each menu preset. The value that appears names the preset. This is a cvar-only test (no code
    change).

Pass criteria: (a) colour targets on Neo have INFO bit 31 = 1 and dword 6 = 8, base has 0 and 0; (b) the cell takes 3 on
base and 1 or 2 on Neo; (c) R tracks the cell, or the link is shown broken; (d) the cvar test gives one name per value. If (a)
fails, the Neo flag is not in `[6]` and T10.3 is wrong. If (c) shows R stays at 3, the presets do not reach the group record,
and the mapping needs another source.

### T10.9 Open items

1. The link from the graphics cell (`DAT_032454b8 + 0x60`) to R (`S+0x4c0`): not found. T10.8 items 10-11.
2. The source of the model-group field `+0x98` (`0x195370` `local_620`, `0x196260`): decomp typing unresolved.
3. `uStack_b0` / `uStack_a8` of `0x195370`: no visible writer.
4. The name of each preset value (`_Cinematic`, `_Perf`, `_BasePS4` to 1, 2, 3): T10.8 item 13 gives it at run time.
5. The meaning of `t` (setting `0x25`) against the menu item, and the sign used in `0x70ac50`.
6. The depth-side Neo effects: the forced tile 4 (`spec[9] & 3 == 3`), the `0x2000` query bit, the `+0xe0` / `+0xde` copy.
7. Bits 0..1 of the kRTT flag word (`uVar4 >> 16`) and the byte `uVar4 >> 24` of each kRTT spec: not decoded, so the set of
   colour targets with a non-zero `spec[9]` is unknown.
8. The `0x882f10`, `0x882fb0`, `0x883010`, `0x883080` constants (T10.2): only the first is guessed.
9. The colour `ImageInfo` has `bank_swizzle = 0`; whether the GPU applies a swizzle on Neo CB writes is unknown (T10.5).
10. `0x9ae80` and the `0x8806f0` branch (T3.7 item 6 and the T10.3 last paragraph): not traced.

### T10.10 Evidence list (T10)

| Item | Address | File and line |
|---|---|---|
| Neo latch, Neo flag | `0x882ec0` (guard `DAT_03294f28`, value `DAT_03294f20`) | `out/re/decomp/0088/00882ec0.c` |
| Neo constants | `0x882f10`, `0x882fb0`, `0x883010`, `0x883080` | `out/re/decomp/0088/00882f10.c`, `00882fb0.c`, `00883010.c`, `00883080.c` |
| Neo import wrapper | `0x3f4e0` (`== 1`) | `out/re/decomp/0003/0003f4e0.c` |
| Colour target builder, Neo flag at spec `[6]` | `0x97a50` lines 99-108, 107, 115, 167-178 | `out/re/decomp/0009/00097a50.c` |
| Depth spec and rewrite | `0x97a50` lines 235-247, 296-300 | `out/re/decomp/0009/00097a50.c` |
| Colour spec default | `0x87da70` | `out/re/decomp/0087/0087da70.c` |
| Colour descriptor writer (INFO bit 31, dword 6) | `0x87dac0` | `out/re/decomp/0087/0087dac0.c` |
| Depth spec default and builder | `0x866f50`, `0x866fa0` | `out/re/decomp/0086/00866f50.c`, `00866fa0.c` |
| Graphics mode, cell writes (1/2/3) | `0x70a6d0` | `out/re/decomp/0070/0070a6d0.c` |
| Mode table | `0x5bb40` | `out/re/decomp/0005/0005bb40.c` |
| Menu and load callers of `0x70a6d0` | `0x559a10` (lines 127, 202), `0x558b20` (385), `0x70a9b0` (60), `0x70ac50` (40) | `out/re/decomp/0055/00559a10.c`, `00558b20.c`; `0070/0070a9b0.c`, `0070ac50.c` |
| View mode R | `0x90860` (193-200), `0x95690` | `out/re/decomp/0009/00090860.c`, `00095690.c` |
| R copy and source | `0xe0db0` (102-105), `0x195370` (171, 193, 203-210), `0x196260` (93) | `out/re/decomp/000e/000e0db0.c`, `0019/00195370.c`, `0019/00196260.c` |
| Suffixed cvar registration | `0x6efe0` (timed out); strings `0xf9b86c`, `0xf9b88c`, `0xf9b8aa`, `0xf9cba6`, `0xf9cbd3`, `0xf9cbfe` | `out/re/decomp/0006/0006efe0.c` (stub); `tools/re/eboot_re.py str` |
| Port alt-tile field | `Color0Info.alt_tile_mode` (bit 31) | `gpu/shadps4/video_core/amdgpu/regs_color.h:151-174` |
| Port colour ImageInfo | `alt_tile` from the bit | `gpu/shadps4/video_core/texture_cache/image_info.cpp:76-90` |
| Port T# alt and swizzle | `alt_tile` from the T# bit; `GetBankSwizzle` | `image_info.cpp:194`; `gpu/shadps4/video_core/amdgpu/resource.h:183, 281` |
| Port alt identity and overlap | `texture_cache.cpp:638, 922, 955-1025` | `gpu/shadps4/video_core/texture_cache/texture_cache.cpp` |
| Port detiler | `bank_swizzle` only | `gpu/shadps4/video_core/texture_cache/tile_manager.cpp:203, 294` |
| Port depth registers (no alt) | `DepthBuffer` | `gpu/shadps4/video_core/amdgpu/regs_depth.h` |
| Port presets and cvars | `af16`, `fps60game` | `scripts/sotc_modes.py:59-74` |
| Launcher preset text and cascade variants | menu and cvars | `launcher/sotc_launcher_win.py:649-652, 1353-1355` |
| Neo runtime switch | `runtime_sotc_neo` | `src/runtime_sotc.c:45-55` |

## Run results (RA, 4 Nov 2026)

Source: `user/runGJ/re_probe.log` (same run as texture-binding.md, `SOTC_RE_PROBE=1`, cold cache). Eboot offset =
absolute address - 0x18040000. Topic `pass-*` is capped at 300 lines per topic; pass-flip (5 lines), pass-mode (0),
pass-table (1) and pass-view (1) are far below the cap. decal-cb logged 38 arms and 262 draws.

**Key finding for this section:** the probe computes the manager as `M = *(*(image+0x2ed3670)+0x80)`. In this run that
value is 0 in every line that uses it (`pass-view`, `pass-flip`, all 300 `pass-mrt`). The object the game itself passes
as the manager is non-null: `pass-table M=0x78322817712` is the `rdi` of 0x9fba0. The decompile agrees (inferred, see
item 1): `DAT_02ed3670` is the manager object itself, not a pointer to a sub-object. So the `+0x80` dereference in the
probe (and the `M = *(DAT_02ed3670 + 0x80)` of K6.3 / K6.9 item 1) is wrong, and every "M+..." value logged in this run
was read at the wrong base.

### RA.P1 Item results, K6.9

1. First `cefa0` call, `V == M`: **REFUTED.** `pass-view first F=0x123c6d4e60 F40=0x123c6baa68 V=0x100dc05700 M=0x0
   V==M no`. Because M was read at the wrong base, the decision uses the correct manager O = `0x78322817712`
   (value of `DAT_02ed3670`, from `pass-table`): V != O, and V != `*(O+0x80)` (which is 0 in this run). So V is another
   object, not the manager. Decompile support (inferred): `0x95a90` stores its allocated object in `DAT_02ed3670`
   (`00095a90.c` lines 54-61); `0x90860` passes `DAT_02ed3670` itself to `0x95690` (line 200), which passes it to
   `0xa1cd0` (`000a1cd0.c`, reads `+0x3b70`) and to `0x9fba0`.
2. `sceKernelIsNeoMode()` result: **CONFIRMED neo = 0** (`pass-table ... neo=0`, `pass-flip first-call neo=0`). The
   port runs as base PS4, so the derived (Neo) tables are not expected to exist.
3. `M+0x2288` pointer, count at `+0x1a8`, `M+0x1e80`: **UNKNOWN.** Read at the wrong base (`M2288=0x0 count1a8=0
   M1e80=0x0`). Needs a re-run with O (`0x78322817712` in this run) as the base.
4. Derived tables at 0x9fba0 (Neo only): **CONFIRMED that they are empty on base PS4.** At the correct object
   (`pass-table M=0x78322817712`), all ten sources read `0x0/0x0`, e.g. `[08]=0x0/0x0 [38]=0x0/0x0 ... [380]=0x0/0x0`.
5. `0x90860` (S, S+0x4b8, S+0x4c0, R) and the mode change at `0xa1cd0`: **PARTIAL.**
   - R is `0x123c3c5630` in all four logged calls (calls 1, 3, 4 and 176). S4b8 = 0 and S4c0 = 3 in all four.
     Frequency of R changes: **UNKNOWN** (only four distinct S were logged; R did not change in them).
   - Mode: no `pass-mode` line. RE_PASS_MODE (0xa1cd0) never fired. Inferred: `0x95690` returns early when the mode is
     unchanged (`00095690.c` lines 7-8), and `0xa1cd0` is called only from there, so **no mode change happened in this
     run**. The new value was not observed.
6. Decal draws, CB_COLORn INFO after the decal MRT bind: **REFUTED as a general statement.** K6 predicted that colour
   target 0 is unbound for decal draws. Over 38 arms (262 draws logged, the first 8 after each arm):
   - `bound` (bit 0 = target 0 enabled): `0x00` in 78 draws, `0x01` in 100, `0xff` in 84. So target 0 is enabled in
     184 draws and unbound in 78.
   - `cb0` INFO is `0x00000000` in 47 draws (the unbound ones and some others) and non-zero in the rest (for example
     `0x00042730`, `0x00050410`, `0x0000a028`).
   - Evidence: `decal-cb draw=1 arm=2 bound=00 cb0=00000000` (unbound); `decal-cb draw=13 arm=4 bound=01 cb0=00042730`
     (bound).
   - Caveat: the logged draws are the first eight after each arm; they are not checked to be decal (k = 6) draws. The
     decal pixel-program hash was not logged: **UNKNOWN** for the 109-program question.
7. `0x92140` MRT slots against `M+0x60` and `M+0x58`: **UNKNOWN.** The comparison used the wrong M (`eq0=0 eq1=0`
   everywhere, not evidence). The slot values themselves: slot 0 is pointer-like (200 distinct values in 300 rows;
   `slot0=0x124bac0280`). Slot 1 is never a pointer in this run: 300 of 300 read as sign-extended values such as
   `0xffffffff00000000` (173 rows), `0xffff80e000000000` (63), `0xffff90ff00000000` (32), so the hypothesis "slot 1 =
   Normal" is not supported by what is stored at `P[1]`. The shader export order (MRT0 = Albedo?) was not checked.

### RA.P2 Verdict summary (pass-targets.md, K6.9)

| Item | Verdict |
|---|---|
| 1. `V == M` | REFUTED (V is another object; the M formula is wrong) |
| 2. Neo flag | CONFIRMED neo = 0 |
| 3. `M+0x2288`, count, `M+0x1e80` | UNKNOWN (wrong base in the probe) |
| 4. Derived tables (Neo) | CONFIRMED empty on base PS4 |
| 5. R, S, mode change | PARTIAL: R constant (4 calls), no mode change (inferred), change rate UNKNOWN |
| 6. Decal CB INFO | REFUTED as stated (cb0 bound in 184 of 262 draws); pixel-program hash UNKNOWN |
| 7. `0x92140` slots | UNKNOWN (wrong base); P[1] is not a pointer in this run |
