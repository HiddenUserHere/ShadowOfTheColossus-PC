# Flicker brief 2 (1 Nov): find the real cause, mode 1 versus mode 0

You are a senior specialist of the SotC PC port (native Windows port of PS4 Shadow of the Colossus; read CLAUDE.md at
the repo root first, then docs/re-stutters/native/FLICKER-BRIEF.md for the model and the history). The user wants
the ROOT CAUSE, not more symptom tests. Go deep for real: do not stop at the first finding; when a lead dies, take
the next one; check every claim against the code, the Ghidra decomp and the run logs; report what stays unknown.

## Rules
- Worktree: `git checkout --detach 1b3d7a8`, then
  `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/flicker-wip2.diff` (all current work).
- Do NOT commit. Do NOT run the game (the coordinator runs it, one instance at a time; if you need a run, write the
  exact env/counters you need in your report). Syntax-check C/C++ with MSYS2 CLANG64 (`source msys2-env.sh`,
  clang++ -fsyntax-only with the flags of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json).
- RE: Ghidra decomp first (C:/Users/ptr/Documents/sotc-pc/out/re/decomp/<bucket>/<offset>.c), names in
  out/re/ghidra/names*.txt, `python tools/re/eboot_re.py func|dis|callers|sig|sigfind` from the main tree.
- Code: clean, English comments, `sotc:` markers in vendored shadPS4 files. No new user switch. No GPU wait except
  frame pacing, no added input latency, no notable extra RAM/VRAM.
- Final report in your last message: root cause(s) with evidence (file:line, decomp offsets, log lines), every file
  changed, check results, the exact game run that would confirm it, unknowns.

## Proven facts (measurements of this wave, Lab route user/routes/visual.json, camera parked at 18 stops,
## max frame-to-frame mean abs diff, mean over stops; screenshots and run.log in user/shots/<run>/)
- mode 0 (reference): 0.86 / 0.92. mode 1 with all fixes: 1.41 (fl_w9). The extra difference is in the GRASS near
  the camera (tufts change frame to frame, user/shots/w9_diff.jpg). Trees move with wind equally in both.
- mode 1 with `SOTC_N4_TRACK=1` (TEMP: TracksGuestPages() forced true inside mode 1: page protection, write faults,
  readbacks, fault-driven invalidations all back, everything else of mode 1 unchanged): 0.84 = reference. So the
  cause is in what TracksGuestPages() gates or what page faults did in mode 0.
- Masks are NOT the parked-camera cause: serving every mask as the OR of the last two copies changes nothing (1.43);
  zero masks are now ~0.
- Byte oracle (SOTC VERIFY BYTES, mode 2, buffer binds up to 64 MiB and images, ~1.8 GB compared per window): only
  hundreds of stale CPU-written bytes per window, 0 in images. CAVEAT: it only sees CPU writes that FAULT in mode 2
  and only data the old path uploads. It does NOT see host-side writes the port itself makes into guest memory, nor
  the CPU consuming GPU results, nor mode-1-only mechanics (NativeDrain timing, exact uploads, image reuse rules).
- Tracking on but readbacks off (TEMP SOTC_N4_NORB) could not be measured: the game hung in loading waiting on
  PS4SyncEvent (the game needs GPU-written data read by the CPU during loading).
- Found and fixed just now (coordinator): Rasterizer::FillBuffer / CopyBuffer CPU paths (vk_rasterizer.cpp ~7400) write
  guest memory on the host (std::fill / memcpy for the game's DMA) without telling the buffer cache in mode 1; mode 0
  learned it through the write fault. Not yet measured. There may be more host writes like this.

## User evidence
- Free play: grass flickers "with other textures" (maybe shadows); some places invisible; first frames fine, the
  bugs start after a few seconds.
- NEW: when turning the camera fast, about 3 frames show the depth bug, then it clears. That is the XPR visibility
  masks served from the previous completed frame (objects entering the view stay culled until a copy of a frame that
  saw them completes; GPU lag adds frames). Separate from the parked-camera grass flicker.
