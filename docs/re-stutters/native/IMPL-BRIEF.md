# PC-native engine: implementation brief (N1-N3), rules for every specialist

Read first, in the MAIN tree C:\Users\ptr\Documents\sotc-pc (branch re/pc-native): CLAUDE.md,
docs/re-stutters/native/PLAN.md, the API contract gpu/sotc/native/native.h (code against it exactly; do not
change its signatures: if you need more, add a new function in your own file and report it), and the N0
inventory in docs/re-stutters/native/: heaps.md, allocators.md, loader.md, framedata.md, gpuresults.md, sync.md.

## Rules
- You are the specialist of your area. Go deep; never stop at the first finding. Read the Ghidra decompilation
  (out/re/decomp, out/re/ghidra/names*.txt; callers with `python tools/re/eboot_re.py callers <addr>` and
  `icallers <name>`) before writing a hook; raw disassembly only to confirm patch bytes.
- Work in your worktree, but base your edits on the MAIN tree's current files: copy each file you change from
  the main tree into your worktree first (your worktree may be on an older base), then edit. Edit only the files
  your task owns; give exact snippets for anything else.
- NEVER commit, merge or switch branches. Leave changes uncommitted; the coordinator collects them with git diff.
- Do NOT build with ninja/build.sh and do NOT run the game or tools that start it. Check with a syntax-only
  compile: the file's command from out/gpu/compile_commands.json (main tree), source path pointed at your copy,
  your worktree's gpu/ include dirs first, `-c ... -o ...` replaced by `-fsyntax-only` (run `source msys2-env.sh`
  in bash first). For new .cpp files in gpu/sotc/native/, use the command of gpu/sotc/load_warm.cpp.
- Everything runs only with SOTC_NATIVE=1 or 2 (SotcNative::Mode()); mode 0 must behave exactly as today.
  Mode 2 = verify: also run the old path and count mismatches. Counters go into SotcNative::Report or your
  own report function (give the driver.cpp snippet).
- Hooks: follow the existing hook engine (scripts/hooks.d/*.py records: name, switch, group, mode entry/wrap/
  range, pattern, expected offsets; handlers registered in gpu/sotc/hook_handlers.cpp / RegisterHandler). The
  hook switch of every native hook is SOTC_NATIVE. Check new hook patterns with
  `python tools/re/hooks_install_check.py` (main tree).
- Comments in English, `sotc:` marker on edits inside gpu/shadps4, match the surrounding style.
- Final report: docs/re-stutters/native/impl-<your-id>.md in your worktree (uncommitted) AND returned: files
  changed (file:function), hooks added (name, offset, mode), what each does, the ordering/safety argument,
  counters, verify checks, snippets for files you do not own, what remains.
