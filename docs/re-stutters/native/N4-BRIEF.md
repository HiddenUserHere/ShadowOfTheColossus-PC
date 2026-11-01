# N4 safe deletions: brief for every specialist

Context: docs/re-stutters/native/PLAN.md stage N4 and the switch audit
docs/re-stutters/reports3/draw/wave3/switch-audit.md (full table: class, default, reader file:line, launcher row).
This wave deletes only the classes that do not depend on the native model being verified in game:
DELETE-NO-GAIN, DELETE-BROKEN, DELETE-UNUSED, DELETE-DEAD, DELETE-SUPERSEDED. DELETE-AT-NATIVE is NOT touched
(it waits for SOTC_VERIFY in game).

Follow docs/re-stutters/native/IMPL-BRIEF.md (worktree, copy every file you edit from the main tree
C:\Users\ptr\Documents\sotc-pc first, because your worktree base is old; never commit; syntax-only checks with
`-I<wt>/gpu/shim -I<wt>/gpu/shadps4` in that order and the main tree's out/gpu/compile_commands.json flags; no game
run). You are the specialist of the files you are given: edit ONLY those files.

What "delete a switch" means:
1. Confirm the default from the reader code itself (the audit's defaults marked `inf` are inferred). Confirm that
   nothing sets it: grep the whole main tree (src, gpu, scripts, launcher, tools, tests, packaging, sotc.py, sotc.ini,
   docs) excluding .claude/worktrees. If anything default-on or a launcher row sets it to a non-default value, STOP for
   that switch and report it instead of deleting.
2. Keep exactly the behaviour of the default, delete the reader and the non-default code path, and every helper,
   member, counter and report line that only that path used (follow the callers: never stop at the reader line).
   A `value` switch becomes a named constexpr with the default value, unless the value is then used only once and
   reads clearer inline.
3. No behaviour change in the default configuration. If removing the path would change anything (a side effect in
   the reader, an order of initialisation), keep it and report it.
4. Code comments in English, matching the file's density; keep `bbport:` / `sotc:` markers on vendored edits.
5. Do not edit launcher/sotc_tuning.py, CHANGELOG.md, README.md or docs/sotc/*: list in your report each launcher
   row, LIVE entry and doc line that names a deleted switch, with file:line, so the coordinator applies them.

Report docs/re-stutters/native/impl-n4-<id>.md (uncommitted, in your worktree) and return it: per switch, the
confirmed default, the lines removed (file:line ranges), anything kept and why, the syntax-check result, and the
snippets for files you do not own.
