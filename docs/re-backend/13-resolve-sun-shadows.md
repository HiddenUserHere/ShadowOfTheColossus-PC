# 13 - ResolveSunShadows: what it binds, and whether it reads the main depth

Read-only static analysis. Sources: Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`, `tools/re/eboot_re.py refs`
(index in `out/re/index.pkl.gz`), and notes 04 (section 8) and 06. No build, no game run, no commits.
Addresses are eboot VAs. Labels: **observed** (decompile line or address), **inferred**, **unknown**.

## 1. Verdict

**(A), provisional: the resolve most likely reads a full-resolution texture that the pre-pass fills, so pre-pass removal is
unsafe as things stand.** The strongest evidence is indirect. The resolve's full-screen input texture is a CTexture
read from `ctx+0x2110` (observed), and I could not name that texture (unknown). The "A" rests on the inferred rule that
a full-resolution input available before the G-buffer must come from the pre-pass or the previous frame. If the input
is proven to be the previous frame's depth or a shadow map, the verdict flips to (B).

## 2. The two resolve bodies

- `0xafcc0` (`ResolveSunShadows`, 1085 bytes) and its twin `0xb0100` (1133 bytes) are the same routine with different
  packet helpers (observed: `0xafcc0` calls `0x884d40` / `0x883870`; `0xb0100` calls `0x86edc0` / `0x86b9e0`).
- Queued from `LightRendering::ResolveSunShadowJob` (name string, `0xc59d0` line 350; job function pointer
  `&LAB_000afca0`). Queue call `FUN_000c12a0(..., 4, 0)` in `0xc59d0`, after the pre-pass queue
  (`0xc1860`, flags `0x302`) and the direct call `0xa6640` (FogParticles), and before the G-buffer queues
  `0xc1ce0` and `0xc1fc0` (flags `0x302`). **observed** (decompile order). The file names are `000c59d0.c`, and the queue
  call is at `0xc5f47` per note 04.
- The job is called with `param_1 = *(BPRender+0x58)` (LightRendering), `param_2` = the pass object, `param_3` = the
  command context (`0xc59d0` line 350 region). **observed** for the calls; the job-argument layout is **inferred**.
- Refs: `eboot_re.py refs 0xafca0` shows only its thunk (`sub_afca0` at `0xafca9`). The function pointer is stored by
  `0xc59d0`, so the direct-caller list is empty. **observed**

## 3. What the resolve binds

### 3.1 Its own body calls no texture setter (observed)

`0xafcc0` and `0xb0100` do not call `0x861fe0`, `0x862160`, `0x8619a0`, `0x861b40`, `0x861ce0`, `0x861e60`,
`0x8622e0` or `0x862490` directly. Its binds are of three kinds:

| Kind | Call (observed) | Data |
|---|---|---|
| Descriptor-table pointers | `0x884d40` (`0xafcc0` l.19), `0x86edc0` (`0xb0100`), `0x16ee00` (`0xafcc0`), `0x92540` (`0xb0100`) | `(pass+0xe8)+0xd0` and `(pass+0xe8)+0xd8`, passed as GPU addresses (user data / 0x38- or 0x10-byte packets). The content of those tables is written elsewhere (**unknown**). |
| Constant chunk | `0x8c0d0(ctx+0xa30)` gives the block, written as 1/w, 1/h, the T# copy and a flag word | see 3.2 |
| Compute program | `0x16edc0(ctx, *(...)+0x60)` | shader record from `DAT_02ee3ce0+0x8a8 -> +0x68 -> +0x210 + byte(pass+0x48+0xec)*0x10` (first dispatch); `+0x230` (second dispatch). **observed** |

So the texture slots of the resolve are not set in its body. They come from the pass tables at `pass+0xe8` (which are
filled outside, **unknown** where) and from the inline T# below.

### 3.2 The full-screen input is the CTexture at `*(pass+0x18)+0x2110` (observed)

- `0xafcc0` l.43 and l.55 (`0xb0100` l.40, l.52): `lVar3 = *(*(pass+0x18) + 0x2110)`. This CTexture is the resolve's
  source. Call it **T_in**.
- `FUN_00096f50(T_in)` and `FUN_00096f60(T_in)` give `iVar4` (width) and `iVar5` (height). They set the 1/w, 1/h
  floats of the constant chunk (`pfVar8[0..3]`) and both dispatch sizes: `((w+7)>>3, (h+7)>>3, 1)` through
  `0x883870` / `0x86b9e0`. So T_in has the screen size, not a square shadow-map size. **observed**
- The 32-byte T# at `T_in+0xc4..+0xdc` is copied into the constant chunk at offset `+0x30` and passed through
  `0x8829e0(&T#, 0x6d)` (flag rewrite). This is the same T# read that note 06 documents for `0xa8c50` and `0x133550`.
  **observed**
- The same `*(ctx+0x2110)` pointer and its T# (`+0xc4`) are used by the lighting job `0xaf610`
  (`DirectionalLight/AmbientLight`, l.50, l.187-190, bind `0x862490` l.224). So T_in is also a lighting input. **observed**

### 3.3 Which texture T_in is (unknown)

- Writers of offset `0x2110` as a pointer: **none found** in the decompile (literal `0x2110` search across `out/re/decomp`).
  The only write is `sub_ec2b30` (`0xec2b30` l.14), a 4-byte store into a different struct. The pointer is therefore
  probably written by a struct copy or a block copy (**inferred**), and I did not trace it.
- `kRTT_MainDepth` is created in `render_targets_allocate` (`0x9aef0`, l.891, `FUN_0008d6f0(..., 1, 0, 9, "kRTT_MainDepth")`)
  and stored in the RT manager at `+0x38` (l.899-903). **observed**. Not linked to `ctx+0x2110` by any statement found.
- `kRTT_MainDepth_HalfRes` (l.967) and `kRTT_CollisionDepth` (l.1649) are also depth-class RTs. **observed**. Note 06
  shows the T# path only, not the target's identity.

## 4. Is the main depth one of the resolve's inputs?

- **Inferred (main argument):** T_in is screen-sized (section 3.2) and the resolve runs before the G-buffer (section 2).
  At that point, a full-resolution texture of this frame can only be the pre-pass output (main depth, `kRTT_MainDepth`
  per 0x9aef0). The alternatives are the previous frame's depth or a square shadow map, and both fail the screen-size
  condition or the timing. Result: T_in = main depth is the most likely reading.
- **Observed counter-evidence:** none found. T_in is not one of the square shadow maps (`kRTT_CascadedShadow`,
  `kRTT_HiResSpotShadowArray` have their own dims and are not reached in the resolve).
- **Unknown (blocks confirmation):** the RT identity of T_in. Confirm by finding the copy into `ctx+0x2110` (or by
  comparing its base address with the `kRTT_MainDepth` CTexture at RT manager `+0x38` at run time).

## 5. What consumes the resolve's output

- **Output objects (observed):** `pass+0xb0` = the constant chunk built by the resolve (`0xafcc0` l.66 `*(pass+0xb0)=pfVar8`).
  An 0x18-byte struct `{pass+0x98, pass+0xb0, pass+0xd0}` (l.69-72) is added to a list at `ctx+0x50` (`0x85ecd0` or
  `0x85e5d0`, conditional on the type byte at `ctx+0x1ae0`). The other output is `pass+0xd0` (also in the struct).
  The texture behind `pass+0x98` / `pass+0xd0` is **unknown**.
- **Consumer (inferred):** the list at `ctx+0x50` is drained by the lighting chain in `0xc59d0` after the G-buffer
  queues: `0xaea10` (direct call, which reaches `0xaf610`), `0xb0570`, and the `0x1a2800` list writes at the
  `LAB_000c60d7` section. Lighting reads the output after the G-buffer is queued. **inferred** (no direct
  statement in the decompile shows which texture the lighting reads).
- **Before the G-buffer ends?** The resolve's dispatches are queued (`0xc12a0`), so their execution order against the
  G-buffer jobs (`0xc1ce0`, `0xc1fc0`) depends on how `0xc12a0` orders the queue. **unknown** (flags `4` for the
  resolve vs `0x302` for the others; not decoded).

## 6. Re-queue after the G-buffer?

- The queue call is `0xc59d0` at `0xc5f47` (between the pre-pass and the G-buffer). Moving it after the G-buffer queue
  (after `0xc1fc0`) is a code-level change in `BPRender_DepthAndGBufferPasses`. It is possible only if the G-buffer
  draws do not read the resolve output. **inferred** (the G-buffer is geometry; nothing observed reads the output there).
- It does not solve the pre-pass problem by itself. If the resolve moves after the G-buffer and still reads T_in = main
  depth, it reads the depth that the G-buffer itself writes (note 04, section 4: G-buffer draws have depth writes).
  That is valid only if the G-buffer covers every depth the pre-pass would have written. Section 6 of note 04 lists the
  objects that would be lost (alpha-tested lists, pre-pass-only occluders, particles, grass, coplanar geometry).
- So the move is a second change, needed only if the resolve turns out to read the main depth.

## 7. Unknowns that would change the verdict

1. Who writes `ctx+0x2110` (section 3.3). Decides whether T_in is main depth.
2. The T# format of the texture behind `pass+0x98` / `pass+0xd0` and where it is read (section 5).
3. The contents of the descriptor tables at `pass+0xe8+0xd0` and `+0xd8` (section 3.1). These could bind more
   depth-class textures than T_in.
4. Job execution order for flags `4` vs `0x302` in `0xc12a0` (section 5).

## 8. Next steps (not done here)

- Trace the writer of `ctx+0x2110` in the frame setup (the copy that puts the RT pointer in the render context), then
  compare the address with `kRTT_MainDepth` at RT manager `+0x38`.
- Dump the `pass+0xe8` tables at run time (one frame, read-only) to confirm the bound T# base addresses.
- Only then decide whether note 04's removal needs the resolve moved (section 6).
