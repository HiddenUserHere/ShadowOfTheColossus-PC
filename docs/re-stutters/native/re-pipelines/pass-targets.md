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
