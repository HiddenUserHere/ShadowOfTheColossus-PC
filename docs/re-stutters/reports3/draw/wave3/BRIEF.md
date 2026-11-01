# Wave 3 (stutter attack): common rules for every specialist agent

Project: native Windows port of PS4 Shadow of the Colossus (vendored shadPS4 video core in gpu/shadps4, local
edits marked `sotc:`). Read first: CLAUDE.md, docs/re-stutters/HANDOFF.md (the end: in-game results of 1 Nov),
docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md, and the trace evidence below.

## Evidence (1 Nov, Lab route + free play, ~28 FPS, GPU ~37% busy: CPU-bound)
- Trace session user/trace/20261101-085933/ (40 .sotctrace files, frames over 50 ms; report hitch.md there; read
  with `python tools/trace/sotc_trace.py hitch|top|json <files>`, format in docs/sotc/TRACE-FORMAT.md).
- 106 slow frames: 91 "draw" (shadPS4:GpuDraw over the 33 ms budget; every game thread waits on the flip),
  13 "game-cpu", 2 other.
- Draw-thread spike sources: (1) dispatch-heavy frames, +30 ms in DispatchDirect/dispatch/DrawIndirect;
  (2) readbacks serviced on the draw thread ("rb finish", "command" spans, +8..17 ms in 17 of 40 files);
  (3) area load: one frame re-uploads 139 MiB of CPU-written memory (2606 sync copies; bind buffers / sync
  memory / sync copy), 152 ms.
- Matrix logs with per-5 s reports: user/pcmem/*/logs (newest: 20261101-085630 drawbest vs default).

## Rules
1. You work in your own git worktree (isolation). Commit your work on your branch with clear commits; the
   coordinator merges the branches. Keep edits to the code your task needs; others edit other areas of the same
   files in their own worktrees, so do not reformat or move code you do not change.
2. Do NOT build with ninja/build.sh, do NOT run the game or tools/pcmem_matrix.py. Check compiling with a
   syntax-only run: take the file's command from C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json,
   point the source path at your worktree's copy, add your worktree's gpu/ include dirs before the main tree's
   (`-I<worktree>/gpu/shadps4 -I<worktree>/gpu/shim`), replace `-c ... -o ...` with `-fsyntax-only`.
   MSYS2 CLANG64: `source msys2-env.sh` in bash first.
3. Every behaviour change sits behind a NEW environment switch, OFF by default, read once (do not derive one
   inline-variable flag from another: init order broke kFpsTimers before). Where data could go stale or order
   could change, add a `=2` verify mode that counts mismatches. Add counters to a 5 s report line.
4. You are a specialist: do not stop at the first finding. Map the whole problem, rank causes with evidence
   (trace spans, log counters, code), fix every cause you can, and say what remains and why.
5. Comments in English, `sotc:` marker on edits in gpu/shadps4, match the surrounding style and density.
6. Final report: docs/re-stutters/reports3/draw/wave3/<your-id>.md on your branch AND returned to the
   coordinator: findings ranked with evidence, what you changed (file:function), switches, counters, the
   ordering/safety argument, how to test in game (one-round matrix configs), risks, what is left.
