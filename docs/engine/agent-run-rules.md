# Agent run rules (binding)

Standing rules for running the game in this worktree (user-set 2026-11-20, mirrored in
`sotc-pc/user/agent-briefing-run-and-tools.md`, the canonical briefing):

- **No subagent may ever execute the game** — not `bb-probe.exe`, not `sotc.py`, not the
  bench/play scripts, in any phase, on any authorization. Runtime verification is the main
  session's step. Subagent briefs must state this rule explicitly.
- **Never launch the game without the user's explicit authorization for that run** (this
  binds the main session). A "go" for one run is not a standing authorization; ask again for
  the next run.
- **When authorized, always launch with `skipSplash=1 nointro`** (engine cvars that skip the
  logos, the opening film and the title screen) unless the run specifically targets the
  splash/intro/title screens. Combined example:
  `SOTC_CVARS="skipSplash=1 nointro"` (or `BENCH_CVARS` for the bench scripts).
- Do not `source msys2-env.sh` in the same shell before launching: it clears the
  environment (`SOTC_GAME_DIR` and every `SOTC_*` switch). Export `MSYS2_ROOT` instead.
- **Continuous execution rule (user-set 2026-11-20): when a phase completes, do not stop.**
  Continue straight into the next phases until the whole plan is 100% complete; testing
  happens at the end, not between phases. Each finished agent frees its slot immediately and
  the next task launches without waiting for direction.

