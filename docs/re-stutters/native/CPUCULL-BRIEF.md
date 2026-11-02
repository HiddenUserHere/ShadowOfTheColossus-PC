# CPU cull brief (1 Nov): compute the XPR visibility masks on the CPU (option B)

You are a specialist of the SotC PC port (read CLAUDE.md at the repo root first; context in
docs/re-stutters/native/FLICKER-BRIEF2.md and JITTER-BRIEF.md). Go deep: never stop at the first finding; check every
claim against the decomp and the shader; report what stays unknown.

## Why
In native mode the CPU consumers of the XPR visibility masks get the previous completed frame's GPU mask
(gpu/sotc/native/native_masks.cpp Serve, SOTC_NATIVE_MASKS=prev), because waiting for this frame's cull is a CPU wait
on the GPU (forbidden). Proven by the user: with the real current-frame masks the jitter of the horse, the cape, the
grass and the stairs almost disappears; with the previous frame's mask the wrong LOD/fade instance is enabled for a
frame, and objects entering the view on a fast camera turn stay culled ~3 frames. Decision (user): compute the
current frame's masks on the CPU, conservatively (no Hi-Z occlusion), and serve those.

## Rules
- Worktree: `git checkout --detach 1b3d7a8`, `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/cpucull-base.diff`,
  then copy C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/cpucull-new/* into gpu/sotc/native/.
- The interface is gpu/sotc/native/cpu_cull.h. Implement ONLY your own file(s) as assigned; do not change the
  header's signatures (if you must, say exactly why in the report). New .cpp files under gpu/sotc/native/ are picked
  up by the CMake glob.
- Do NOT commit. Do NOT run the game. Syntax-check with MSYS2 CLANG64 (`source msys2-env.sh`, clang++ -fsyntax-only
  -std=gnu++23 with the flags of an entry of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json).
- Guest memory reads: use the existing helpers (SotcGuest / GuestU32 / GuestU64 patterns in gpu/sotc/native/*.cpp,
  runtime_memory_backing_pointer for mapped checks). Never fault on a bad pointer: validate before reading.
- Performance: these run on the game's job thread at the producer site, up to 65535 objects per set, thousands of
  sets per second (produced ~8500 masks/s). Keep it tight (no allocation per call, SIMD-friendly loops, early outs).
- Clean code, English comments, no new user switch, no GPU wait.
- Final report in your last message: what the shader/decomp does (with evidence), what you implemented, what is
  exact vs conservative vs not reproduced, checks, unknowns.

## Material
- Cull shaders decompiled to GLSL: docs/re-stutters/native/data/cull/2e0f474b.glsl (main scene: 4 RW masks, Hi-Z),
  4defcf7f.glsl (shadow: 2 masks, no Hi-Z). SPIR-V in C:/Users/ptr/Documents/sotc-pc/user/cache/CUSA08809/*.spv
  (spirv-cross / spirv-dis are in C:/msys64/clang64/bin) for other shaders (the LOD pre-shader, decals).
- Earlier reverse of the main cull (a previous specialist; verify it): cull block built by BPXPR_BuildSetDraws
  0x19b760 on its stack at [rsp+0x1a0], 0x160 bytes, live at the produce site 0x19c0f6: +0x00 u32 n, +0x04 u32 mode
  mask, +0x08 u32 plane count, +0x10 planes (16 bytes each, up to 16; inside when n.p + w >= 0 after the sign flip
  with DAT_00fad510), +0x110/+0x114 Hi-Z size, +0x118 u32 class mask, +0x120 4x4 view-projection (Hi-Z only).
  Per-object 64-byte records at *(set+0x30) = owner+0x80: rec[0..11] 3x4 rows (columns c0, c1, c2, translation
  rec[3], rec[7], rec[11]: an oriented box), rec[12] model index, rec[13] LOD, rec[14] mode bits, rec[15] class
  bits. Per-model tables owner+0x58 (32 B/model) and owner+0xd0 (8 B/model) written by the GPU pre-shader 0x19b400
  each frame. Mask 0 = visible; masks 1..3 split mask 0 by the model's fade state (1 fully faded in, 2/3 fading).
  A CPU frustum implementation by that specialist exists in
  C:/Users/ptr/Documents/sotc-pc/.claude/worktrees/agent-a8879033bf8d91777/gpu/sotc/native/native_masks.cpp
  (CpuCull, ApplyCpuVisible): reuse what is right.
- Producer hooks and serving: gpu/sotc/native/native_masks.cpp (OnProduce / OnProduceShadow / OnProduceDecal,
  Serve), scripts/hooks.d/native_results.py; copy ring gpu/sotc/native/native_copies.{h,cpp}.
- Verify: SOTC_NATIVE=2 (tracking on) lets the real GPU mask be compared with a served mask (Serve's Verify block).
