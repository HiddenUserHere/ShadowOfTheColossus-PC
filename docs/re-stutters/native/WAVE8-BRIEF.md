# Wave 8 brief (1 Nov): remaining readbacks, N4 core, N5

You are a specialist of the SotC PC port (native Windows port of PS4 Shadow of the Colossus; read CLAUDE.md at the
repo root first). Plan: docs/re-stutters/native/PLAN.md. Goal: PC-native, no UMA emulation (no page tracking, no
fault readbacks, no CPU wait on the GPU except frame pacing), modern render architecture. State and decisions:
docs/re-stutters/native/impl-*.md, serve1.md, impl-gaps-round2.md, impl-writes7.md, verify1.md.

## Rules
- Your worktree may start on an old commit: first `git checkout --detach a8b3ea2` (branch re/pc-native tip).
- Do NOT commit, do NOT run the game. Leave your changes uncommitted in the worktree.
- Build: the worktree has no out/; syntax-check C/C++ with MSYS2 CLANG64 (`source msys2-env.sh`, clang++
  -fsyntax-only with the flags of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json, third-party
  includes from the main tree). Python: `python -m unittest discover -s tests` (2 known test_packaged_vulkan
  failures allowed); hooks: `python tools/re/hooks_install_check.py --eboot C:/Users/ptr/Documents/sotc-pc/out/eboot.elf`.
- RE: Ghidra decomp first (C:/Users/ptr/Documents/sotc-pc/out/re/decomp/<bucket>/<offset>.c, 8 hex digits, bucket =
  first 4), names in out/re/ghidra/names*.txt; `python tools/re/eboot_re.py func|dis|callers|sig|sigfind` (run from the
  main tree if your worktree lacks out/). Disassembly only to confirm patch bytes.
- Go deep: never stop at the first finding; report what stays unknown.
- Code: clean, English comments, `sotc:` markers in vendored shadPS4 files, follow the surrounding style. No new
  switch unless asked; existing native switches: SOTC_NATIVE (0/1/2), SOTC_NATIVE_MASKS=prev, SOTC_NATIVE_PARTICLES=prev,
  SOTC_VERIFY. User constraints: 30 FPS locked built for 60; NVIDIA/AMD/Intel Arc; no extra RAM/VRAM; no added input
  latency; GPU->CPU consumers use "last completed value, no wait" (approved for masks and particles).
- Final report in your last message (not a file): findings, every file changed, check results, what a game run
  should show, unknowns.

## Latest game data (free play 6.5 min, SOTC_NATIVE=1 masks+particles prev, log C:/Users/ptr/Documents/sotc-pc/user/native-visual.log)
Readbacks ~400-690/s. Top RIPs: 0xf9c40 98/s, 0x1a8a5e 61/s, 0x101a10 24/s, 0x112306 24/s, 0x19b435 24/s,
0x1a8c83 24/s, 0x101ab2 24/s, 0x1a8b38 13/s. Write faults ~6k/s (page tracking still on).
