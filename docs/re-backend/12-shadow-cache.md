# 12 - Sun shadow cascade cache (SOTC_SHADOW_CACHE)

Status: **mode 2 (log and verify) only.** Mode 1 (skip the far cascades and restore their constants)
is not implemented, because the constants that the receivers sample are not located well enough to
restore them safely. Mode 0 is the default and changes nothing.

Labels: **observed** (disassembly, pseudocode, repo code, with address), **inferred**, **unknown**.
Evidence: `out/re/decomp/000c/000cc9b0.c`, `000cdbc0.c`, `000ce850.c`, `000cdb00.c`, `0009/00092140.c`,
`0009/000932a0.c`, `000b/000be1f0.c`; `tools/re/eboot_re.py dis 0xcdbc0` (EU 1.01 eboot).
Context: `docs/re-stutters/reports2/14-draw-sources.md` (H2 proposal).

## 1. Observed structure

- **RenderShadows** `0xcc9b0`. Runs once per main view. Before the loop it calls
  `FUN_000932a0(..., param_5 = 2)`, which fills an HTILE-like range (`FUN_00092bd0`) of the sun
  depth target over its slice range (**observed**; the fill value is 0, and what the port does with
  that fill is **unknown**). The port's own stagger comment (vk_rasterizer.cpp, `SkipStaggeredShadow`)
  says skipped slices keep their texels, so this fill does not clear the depth texels in the port
  (**inferred** from that comment, not re-tested).
- **Loop**: five passes, `lVar5 = 0, -1 .. -4`. The cascade index written to the job at `+0x88`
  (`param_1[0x11]`) is `lVar5 + 4`, so the order is 4 (Super Close), 3, 2, 1, 0 (**observed**).
  Each pass runs `RunFunctionAsRenderJob` (`0xc12a0`) with the body `RenderShadowCascade` `0xcdbc0`.
  Index 0 is not gated; the job at index != 4 also queues `IndexBufferCullShadowCascade` (`0xcdb00`).
- **Body** `0xcdbc0` (signature unique, one match at `0xcdbc0`, checked with `eboot_re.py sigfind`):
  - `ctx = job[+0x18]` (param_1[3]); `i = job[+0x88]`.
  - Reads the per-cascade block at **`ctx + 0x2400 + (i + 0x23) * 0x50`**, 80 bytes (observed:
    `lea ecx,[r13+0x23]; lea rcx,[rcx+rcx*4]; shl rcx,4; vmovups [rax+rcx+0x2400]`).
  - The block is 10 qwords: `[0..7]` view-object pointers, `[8]` a pointer, `[9]` a flags word (bit 0
    selects the `+0xe4` or `+0x124` variant in `FUN_00092140`) (**observed** from `FUN_00092140`).
  - Calls `FUN_00092140(job[0], block)`: binds the view objects (their `+0xe4`/`+0x124` constant
    blocks) into the command state (**observed**). It writes no matrix itself.
  - Calls `FUN_000ce850(job)`, the cascade draw body (object passes, XPR masked lists, virtual geometry,
    particle models; see report 14).
- **`FUN_000ce850`** calls `FUN_000be1f0(job, 0)` (SetViewParams, 10 KB) first. That is the most likely
  writer of the cascade's view matrix (**inferred**, not proven). Its other callers are `caed0`,
  `cefa0`, `cd330`, `cd7a0` and others, so it is not only the cascade path.
- **Cascade index to port slice**: observed that the index runs 4..0 in the loop. That the port's
  slices 2 and 3 are indices 2 and 3 is **inferred** (report 14, open question 5). Check with the
  `SOTC_SHADOW_CACHE=2` report and `SOTC_PASS_TRACE` together.
- **Clear on skipped slices**: the engine clears once, in `RenderShadows` (above), not in the body.
  The body starts a depth pass per cascade through its draw calls. A skipped body therefore starts no
  pass for its slice (**inferred**; matches the port's stagger comment).

## 2. Unknown (blocks mode 1)

1. **Which memory the receivers read for the sun matrix.** The port's reuse notes say the light matrix
   is at `+0x80` of the view constants that `*(VS user data 0..1) + 8` points to (`vk_dlssg.cpp`,
   `vk_shadow_scroll.h`). Whether that object is written by `SetViewParams` in the body, or by an
   earlier cull job (`CullSunShadowCascade`, pointer table `0x159b930`), is **unknown**.
2. **Whether the `+0xe4`/`+0x124` variants are double-buffered per frame** (flags bit 0 at block `[9]`).
   If yes, skipping the body would leave the other copy stale for the next frame. **Unknown.**
3. **Whether the block at `ctx+0x2400+...` persists across frames** or is rebuilt each frame (the
   zeroing of `FUN_0009aef0` writes many `param_1+0x2xxx` slots; it is a reset, not a per-cascade
   writer (**observed**; the writer of the 0x50 blocks is **unknown**).

Without (1) and (2) a skip cannot be paired with a restore that keeps the old texels and the old matrix
together. That is the blink of the port's stagger (report 14, 2.7). So mode 1 is not implemented.

## 3. Design for mode 1 (not implemented)

Schedule (the probe computes it in mode 2): cascade 2 is drawn on odd frames, cascade 3 on frames that
are multiples of 4. No frame draws both. Far shadows of moving casters may then lag up to 3 frames.

- **Skip point**: Entry hook on `0xcdbc0` (the probe's hook). On a skipped frame return
  `HookAction::Return` before the body runs. This skips `FUN_00092140`, `FUN_000ce850` (and so
  `SetViewParams`) for that cascade.
- **What must be restored**: the view constants the receivers read for that cascade, as they were on
  the last drawn frame. The restore needs the addresses from unknowns (1) and (2), which were not found.
  Snapshot them in the drawn frame (after the body) and write them back on the skipped frame.
- **Clear**: the depth texels of the skipped slice are not cleared (no pass starts). The
  `FUN_000932a0` clear before the loop still runs for all slices, so it must be checked against the port
  (unknown; see section 1).
- **Verify (=2 in the next step)**: compare the sun map of a drawn and a skipped frame on the Shrine
  ground (blink test, as in report 14).

## 4. Mode 2 (what is in the build)

Hook `SHADOW_CASCADE_BODY` (`scripts/hooks.d/shadow_cache.py`, entry at `0xcdbc0`, switch
`SOTC_SHADOW_CACHE`). Handler `gpu/sotc/probe_shadow_cache.cpp`. It reads guest memory only
(`SotcGuest::Read`) and always continues.

- Logs the first 12 frames: `SOTC SHADOW CACHE frame F cascade I block ADDR view0 .. flags .. would_skip B (mode M)`
- Report line each 5 s window (from `driver.cpp`):

```
SOTC SHADOW CACHE: mode 2, cascades drawn per frame d0..d4 = a b c d e, skipped s2/s3 = x/y (would skip, not done), restores R = 0 (none done; n would-restore), frames F, block reads failed B
```

`d0..d4` are body runs per frame (mode 2: 1.00 each). `s2`/`s3` are the frames the schedule would
skip. `n` is the would-restore count (= s2 + s3). `B` counts failed reads of the block.
Mode 1 is accepted but prints "not implemented: runs as mode 2".

## 5. Risks

- The Entry detour on the body adds one guest-to-host call per cascade per frame (5 calls). Cheap, but
  the report should be checked for frame time with the switch on (not measured).
- The log uses stderr for 12 frames only, so it does not flood the session log.
- `view0` and `flags` are logged to check unknown (1) and (2) in a real run. They are not used for any
  decision.

## Matrix consumer (static, read-only)

Evidence: `out/re/decomp/000c/000caed0.c`, `000cc9b0.c`, `000cdbc0.c`, `000ce850.c`, `000c7850.c`,
`000ca6d0.c`, `000c6bd0.c`, `000c59d0.c`, `000afcc0.c`, `0009/00092140.c`, `000b/000be1f0.c`,
`0019/0019c1a0.c`, `0085/0085cbc0.c`, `0008/0008c120.c`. Labels as above. "Ctx" is `job[3]` of the cascade job.

### M1. View object layout and SetViewParams

- **Observed:** the per-cascade **view object** is allocated each frame by `FUN_000c7850` with
  `FUN_00017170(0x1228)`. It is stored at `view_state[2] + 0xdd8 + c*8`, where `c` = 0..4 and 4 is
  Super Close (`pfVar[2] = 0` for c == 4). The cascade job reads it as `job[6]` (`cc9b0`:
  `param_4[6] = *(lVar2 + 0xdf8 + lVar5*8)`, the same slot for c = 4).
- **Observed, offsets (float index x 4; overlaps in the decompile make the exact rows partly inferred):**
  - `+0x0c`: 4x4 matrix, copied from view_state `+0x168`, `+0x188`, `+0x1a8`, `+0x1c8` (per cascade stride
    0xf0). `SetViewParams` reads it as `pfVar31+3`.
  - `+0x4c`: second 4x4, from the `+0x1a8`/`+0x1c8` sources (**inferred**).
  - `+0xc4`: 3x4 transform, read by `SetViewParams` as `pfVar31+0x31`. Its sources (`+0x1e8`, `+0x208`,
    `+0x228`, `+0x238`) overlap in the decompile, so the exact rows are **unknown**.
  - `+0xe4` and `+0x124`: two 64-byte constant blocks, 0x40 apart. `FUN_00092140` binds one of the two
    to a user-data slot, chosen by bit 0 of the block flags word (`block[9]`) (observed, `00092140`).
    This is a double buffer. `c7850` writes a sentinel `(0, 0, -10000, 0)` into `+0x104..+0x160` at
    creation (observed). Whether `+0xe4` carries the sun matrix is **inferred** (it is the region copied
    from view_state `+0x228`/`+0x238`).
- **Observed:** `FUN_00092140(cmd, block)` takes an 80-byte block of 10 qwords: `[0..7]` view pointers,
  which it binds to slots 0..7 with `FUN_0085cbc0`; `[8]` a fallback pointer; `[9]` the flags word. It
  writes no matrix.
- **Observed:** `FUN_000be1f0(job, 0)` (SetViewParams) allocates a fresh pool block
  `pfVar27 = FUN_0008c120(ctx + 0x408, &key)` on every call. It writes `pfVar27[0] = view[+0x0c]`, then
  the 4x4 products (`FUN_000ab860`) and the 3x4 at `+0xc4` into the same block, plus rotation terms at
  `pfVar27 + 0x150..0x15b`. It stores the GPU address of that block in `job[0x1c]` (`job+0xe0`) and the
  shared block address in `job[0x13]` (`job+0x98`).
- **Unknown:** which draw reads `pfVar27[0..15]` as the light matrix.

### M2. Who computes the cascade matrices each frame

- **Observed chain:** `caed0` (MainCamera, `0xcaed0`) -> `ccdf0` (`0xccdf0`) and `cefa0` (`0xcefa0`) ->
  `ca6d0` (`0xca6d0`, per-view prepare) -> `c7850` (`0xc7850`, line 93). The order in `ca6d0` is `c72e0`,
  `a1c90`, `c7560`, `c7850`, `c8110`, `c89f0`, `c9160`. `caed0` calls `cc690` and `cc9b0` (`RenderShadows`)
  after its own setup.
- **Observed:** `c7850` shifts last frame's per-cascade state first (`ctx+0x29b0 = ctx+0x29a8`, and the
  `+0x2f90..0x31b0` copies, stride 0x828 per cascade). It keeps one frame of history.
- **Observed:** `c7850` builds each view object from `view_state[4]` (`lVar4 = param_2[4]`): matrices at
  `lVar4 + 0x168 + c*0xf0`, and a cull input at `lVar4 + 0x618 + c*0x128` (the 4th field of the
  `CullSunShadowCascade` job). It only copies. It does not compute the sun direction or the splits.
- **Unknown:** the writer of `view_state[4]` (sun direction, split distances, texel snap constants). It was
  not found. `CascadedShadows` is used only by `RenderShadows` (string `0xfa138d`), and no decompiled
  writer of `[4] + 0x168` was located by offset.
- **Observed:** `FUN_000c6bd0` (`XPRRunFragmentCullShaderShadowScenes`, same frame) calls `FUN_0019c1a0`
  with `view_state[4] + 0x168` and `+0x618` per cascade. It reads the same inputs and does not write them.
- **Inferred:** a new view object is allocated and filled every frame in `c7850`, whether or not the body
  `0xcdbc0` runs. So the resolve binds this frame's matrix even for a skipped cascade, while the depth
  texels stay from an older frame. This is consistent with the blink in section 2.

### M3. Who reads the matrix when sampling

- **Observed:** `LightRendering::ResolveSunShadowJob` (string `0xfa0f15`) is queued in
  `BPRender_DepthAndGBufferPasses` (`0xc59d0`, around line 298). Before that, `FUN_00092140` binds a block at
  `ctx + 0x29a0 .. 0x29f0` (8 view pointers and the flags word, lines 162-171).
- **Observed:** the job body is `0xafcc0` (`ResolveSunShadows`, string `0xfa0484`). It allocates a constant
  block at `view + 0xb0` (`FUN_0008c0d0(param_1 + 0xa30)`) with the screen size, `1/size`, and values from
  `ctx+0x2110` (`+0xc4..+0xdc`). It then binds `job[0x13]` and `job[0x1a]` (`+0xd0`) and calls `FUN_00883870`.
  `0xb0100` is a second variant (`sub_afca0` calls one of them).
- **Inferred:** the sampled light matrix is the 64-byte block at view object `+0xe4` or `+0x124` (bound by
  `FUN_00092140`). The port's note in `vk_shadow_scroll.h` (`+0x80` of the constants behind VS user data
  0..1 + 8) disagrees with this. The disagreement is **unknown**; check which slot the shader reads.
- **Unknown:** the lighting and forward passes were not traced for their slot reads.

### M4. Hook points for mode 1 (not implemented)

1. **Snapshot**: after `0xce850` returns inside `0xcdbc0`, copy `+0xe4` and `+0x124` (2 x 64 bytes) and the
   flags word of cascade `c`, read through `job[6]`. Hook site: `0xcdbc0` exit. Which register holds `job`
   there is **unknown** (check the prologue with `eboot_re.py dis 0xcdbc0`).
2. **Skip**: entry of `0xcdbc0` on skipped frames, as in section 3. This skips `0x92140` and `0xce850` for
   that cascade. `c7850` still builds a new view object, so the new matrix is present regardless.
3. **Restore**: write the snapshot into the **new** view object's `+0xe4` / `+0x124` before the resolve job's
   `FUN_00092140` (around line 297 of `c59d0`), or at the entry of `0xafcc0`. Size: 128 bytes per cascade plus
   the flags word. The GPU reads these through the pointer at draw time, so the write has to land before
   the command list that references it is submitted.
4. **Verify**: if `+0x124` still holds the sentinel `(0, 0, -10000, 0)` after the restore, the restore did
   not reach the bound buffer.

### M5. Summary of unknowns

- Writer of `view_state[4] + 0x168` (sun direction, splits): **unknown**.
- Which of `+0xe4` / `+0x124` the resolve reads at run time (depends on flags bit 0): **unknown**.
- The `ctx + 0x2400 + (c+0x23)*0x50` block (read by `cdbc0`) and the `ctx + 0x29a0` block (read by `c59d0`)
  are different 0x50-byte blocks. Their writers were not found.
- The port's `+0x80` note (`vk_shadow_scroll.h`) is not confirmed.
