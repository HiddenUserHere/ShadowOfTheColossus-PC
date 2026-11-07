# Pipeline key from game data (RE summary, 7 Nov 2026)

Can every Vulkan graphics pipeline be built before the first draw, without first seeing the draw? This page
answers that for each part of the key. It summarizes read-only RE passes over the decompiles in
`out/re/decomp/`. Addresses are eboot offsets.

Short answer: almost the whole key comes from fixed data:

- the material's shader pair;
- a finite blend set built at boot;
- render targets and primitive per pass;
- resource formats, fixed per program in about 98% of cases.

Depth state and vertex input are dynamic state, so they are not in the key. What data cannot give is which
texture or buffer goes into which shader slot, but the key only needs the formats, and those are stable per
program.

## Key parts and sources

| Key part | Source | Known ahead of time |
|---|---|---|
| VS/PS pair | `.cmat` slot records + `.csdr` `OrbShdr` footers | yes |
| Blend + write mask | material class/kind + pass slot, one of the boot-time words below | yes |
| Depth/stencil | dynamic state; family words via `0x91e10` | not in key |
| Vertex formats, strides, divisors | `VK_EXT_vertex_input_dynamic_state`, applied per draw from VGT registers | not in key |
| Per-attribute step class | fetch shader VADDR byte (`fetch_shader.cpp:123`), fixed per program | per program |
| Indirect-draw SGPR offsets | shader record byte (+0x27 VS, +0x19 ES/LS), not decoded | recorded per program |
| Raster variant | material +0x45c (0..2) selects ctx+0x210/+0x21c/+0x228 in `0xf8990` | yes |
| Resource specialization (image type, formats, buffer stride/format) | per program; 98% of programs show one hint (`sotc-pipelines.user.tsv`) | per program, recorded |
| RT formats, prim type, logic op, CB_COLOR_CONTROL | pass family functions (`0x93960`, `0x93ef0`, `0x94600`, `0x93690`, `0x94bf0`, `0x94d00`, `0x94dd0`, `0x94ec0`) | per pass |
| CB_BLEND5..7 | never written by the game; port zeroes them, key writes only `cb < num_color_attachments` | constant 0 |

## Material header (`0xf6c90`)

- The header is positional dwords with no string keys.
- Read order: +0x454; +0x458 kind; +0x464 flags; +0x460 class; a local mask word; then F, giving
  `+0x45c = F > 2 ? 2 : 2 - F`.
- Kind 3 forces class 0 and F = 2.
- +0x560 is a copy of a 64-bit word `(target_mask << 32) | CB_BLEND_CONTROL`. It is copied from the renderer
  object `DAT_02ee3ce0`, which `0x1671d0` builds once at boot; it is not a pointer into that object. The class
  selects which word is copied.

## Per-draw selection (`0xf8990`)

`k` is the material slot at draw record +0xf4 (0..13). The selection runs in this order:

1. If `k == 11`, use +0x48 (additive).
2. Else if class is 7 or 8, use +0x28 (off), or +0x30 when `k == 8`.
3. Else if kind is 7, use the per-MRT array at +0xb0 via `0x91b50`.
4. Otherwise use material +0x560.

`0x919e0` writes the same low dword to CB_BLEND0..4 and the high dword to CB_TARGET_MASK.

### Slot `k` per pass runner

| `k` | Pass runner |
|---|---|
| 1, 10, 12 | depth prepass |
| 2–5 | G-buffer |
| 6 | decal |
| 7 | skybox opaque, fur |
| 8 | alpha, transparent |
| 9 | editor Z-fail |
| 11 | debug |

### Render mode (0x11d3d0 4th argument)

This is the render "mode", not `k`. It indexes a 44-entry name table at VA 0x1597ea0:

| Mode | Name |
|---|---|
| 0–4 | Depth_* |
| 5–8 | GBuffer_Ambient_{Baked,Dynamic}[_Motion] |
| 9–13 | SkirtDecal_* |
| 14 | Standard_Normal |
| 15–23 | Standard_Sky* |
| 24–32 | Alpha_Sky* |
| 33 | Alpha_BeforeWater |
| 34 | Water |
| 35 | Alpha_Normal |
| 36 | Alpha_Normal_Depth |
| 37 | DepthPrepassAlpha_Normal |
| 38 | SpecialScreenEffect |
| 39 | SpecialCloakEffect |
| 40 | Alpha_AfterParticles |
| 41 | EditorZFail |
| 42 | Debug |
| 43 | CollisionDebug |

## The complete blend set (`0x1671d0`)

### Packing

| Helper | Fields |
|---|---|
| `0x87c3b0` | colour src bits 0–4, combine 5–7, dst 8–12 |
| `0x87c3d0` | alpha src 16–20, combine 21–23, dst 24–28 |
| `0x87c400` | bit 29 separate alpha (always set) |
| `0x87c390` | bit 30 enable |

- Combine values: 0 ADD, 1 SUB, 2 MIN, 3 MAX, 4 REVSUB.
- `0x8f1d0(combine, src, dst, mask)` sets alpha equal to colour. Enable is `(dst | combine) != 0 || src != 1`.
- `0x8f280` takes separate alpha arguments and is always enabled.

### Boot-time words

| Offset | Word | Meaning | Used by |
|---|---|---|---|
| +0x28 | 0x20010001 | off | class 0/default; class 7/8 |
| +0x30 | 0x65040504 | SrcA / 1−SrcA add | class 1, 3, 8; class 6 with flag 0x40; class 7/8 at k=8 |
| +0x38 | 0x64010401 | One / SrcA | |
| +0x40 | 0x60070100 | keep dst colour; alpha separate | |
| +0x48 | 0x61010101 | One / One | class 2; k=11 |
| +0x50 | 0x65010501 | One / 1−SrcA | |
| +0x58 | 0x64010401 | One / SrcA | class 1 with kind 6 |
| +0x60 | 0x61000100 | Zero / One | class 5 |
| +0x68 | 0x61040104 | SrcA / One | |
| +0x70 | 0x61240124 | SrcA sub One | class 4 |
| +0x78 | 0x65040504, mask 0x77777777 | alpha blend, no alpha write | |
| +0x80 | 0x61610161, mask 0x77777777 | MAX, no alpha write | |
| +0x88 | 0x61610161 | MAX | |
| +0x90 | 0x60010101 | One add One colour, alpha One/Zero | |

- That is 11 distinct words. Target masks are 0xffffffff and 0x77777777.
- Class 6 without flag 0x40 builds `0x8f1d0(0, 4, 5, mask)` per material: 0x65040504 with a variable mask.
- The `0x8f1d0` calls in `0x5d5410` are not decoded.

### Per-MRT arrays (`0x8f330`)

`0x8f330` builds two arrays. Entry `i` is `min(i, count-1)`; each array holds 5 blend words plus a mask.

- +0xb0, used by kind 7: RT0, RT1 and RT4 use 0x65040504 (alpha); RT2 and RT3 use 0x20010001 (off); mask
  0xffffffff. There is a single instance.
- +0x98, used by light pass `0xaf610`: RT0 and RT1 use 0x60010101; RT2–RT4 are off.
- `0x91b50` writes CB_BLEND0..4 from the array and CB_TARGET_MASK from its sixth word.

## Open points

- The semantic names of class, kind, flag 0x40 and +0x45c are not in the binary. A runtime dump of `.cmat`
  header dwords would give them.
- The mapping from mode to pass family (render-target layout per mode).
- The `0x8f1d0` calls in `0x5d5410`.
- The generic register writers `0x869350` / `0x869370` are reached only indirectly. A CB_BLEND5..7 writer
  through them is unlikely but not excluded.

## Consequence for the port

These parts are all small sets that can be built at the start loading screen:

- the graphics pipeline library fragment-output part: blend word × render-target layout;
- the vertex-input part;
- the per-program vertex and fragment shader libraries.

A pipeline the manifest has never seen then becomes a fast link at draw time instead of a dropped draw.
