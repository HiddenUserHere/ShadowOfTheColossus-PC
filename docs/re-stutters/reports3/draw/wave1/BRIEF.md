# Draw-thread design, wave 1: common rules for every agent

Project: C:\Users\ptr\Documents\sotc-pc (native Windows port of PS4 Shadow of the Colossus; vendored shadPS4
video core in gpu/shadps4, local edits marked `sotc:`). Read first: CLAUDE.md, and
docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md (sections 0-5; your component is named in your task),
docs/re-stutters/HANDOFF.md (latest state).

Rules (several agents work in the same tree at the same time):
1. Edit ONLY the files your task lists as owned. Any change needed in another file: do not make it; put the
   exact snippet (file, the anchor lines to find, the new code) in your report under "Call-site snippets".
2. Do NOT run ninja, build.sh, cmake, the game, or tools/pcmem_matrix.py, and do not copy anything to out/.
   Check your files compile with a syntax-only run using the exact command of the file from
   out/gpu/compile_commands.json, with `-c ... -o ...` replaced by `-fsyntax-only` (for a header, check a .cpp
   that includes it). MSYS2 CLANG64 tools: `source msys2-env.sh` in bash first.
3. Every behaviour change sits behind a NEW environment switch, OFF by default (read once, like the existing
   `static const bool x = [] { getenv... }()` pattern; do NOT derive one inline-variable flag from another:
   that init order broke kFpsTimers). Where data could go stale, add a `=2` verify mode that runs the old
   path too and counts mismatches. Add a counter line to an existing 5 s report or your own report function
   (give me the call-site snippet if the report lives in a file you do not own).
4. Comments in English, `sotc:` marker on edits inside gpu/shadps4, match the surrounding style and comment
   density. No unrelated refactors.
5. Write your final report to docs/re-stutters/reports3/draw/wave1/<your-id>.md AND return it: what you
   changed (file:function), the switch(es) and values, what each counter means, the call-site snippets,
   how to test it in game (which matrix config / env), the risks, and what you could not finish.
   Be explicit about what you verified in code vs assumed.
