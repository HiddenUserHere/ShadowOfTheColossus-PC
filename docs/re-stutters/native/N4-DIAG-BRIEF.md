# N4 diagnostics consolidation: brief

User decision (1 Nov): diagnostics are consolidated into three switches, SOTC_FPS (reports and counters, printed
with the frame rate), SOTC_TRACE (timeline, `.sotctrace`) and SOTC_VERIFY (verify modes). Source list:
docs/re-stutters/reports3/draw/wave3/switch-audit.md, "Consolidation targets" (28 MERGE-INTO-FPS, 46
MERGE-INTO-TRACE, 4 MERGE-INTO-VERIFY) and the full table rows.

Follow docs/re-stutters/native/N4-BRIEF.md (worktree rules, copy files from the main tree first and keep their line
endings, no game, never commit, launcher/docs edits listed for the coordinator).

Rules, per switch, decided from its reader and what it costs:
1. A cheap counter or report line (atomic increments, a 5 s line): it prints when SOTC_FPS is on; its own switch is
   deleted. "Cheap" means no per-op clock read, no allocation, no lock on a hot path, no extra GPU work. If it is not
   cheap, it is not merged (rule 4).
2. A trace probe that only adds spans/events to the timeline recorder: it records when SOTC_TRACE is on; its own
   switch is deleted. Same cost rule: if it adds measurable cost to a trace run that does not need it, keep it as a
   value of SOTC_TRACE_ONLY or a comma list in one existing SOTC_TRACE_* parameter instead of a separate switch.
3. Parameters of the trace or profiler (SOTC_TRACE_DIR, _MS, _PRE_MS, _POST_MS, _MAX, _RING, _ZONE_MIN_US,
   SOTC_PROFILE, _OUT, _SECONDS, _DELAY ...) are configuration of one tool, not separate diagnostics: they stay.
   Report them as KEEP-PARAM.
4. Dumps and heavy tools (shader/texture/command dumps, RenderDoc triggers, menu/set triggers): they write files or
   change timing. They stay as they are and are reported as KEEP-TOOL. Never merge them into SOTC_FPS.
5. Verify modes: SOTC_VERIFY=1 turns on every `=2` verify mode listed (SOTC_BIND_DIFF, SOTC_PC_POOL=2,
   SOTC_PR_VERIFY, SOTC_REUSE_SIZE_CHECK, plus the native SOTC_VERIFY that exists already): each reader also accepts
   SOTC_VERIFY. Keep the old names working only where a test or tools/pcmem_matrix.py preset uses them, and list
   those.
6. Default behaviour with none of the three set must be identical (no new output, no new cost).
7. The launcher: list the rows of launcher/sotc_tuning.py (TUNING, LIVE, migrations) that name a merged switch;
   the coordinator removes them. sotc.py / sotc.ini mentions likewise.

Report docs/re-stutters/native/impl-n4-diag.md: one row per audited name (MERGED-FPS, MERGED-TRACE,
MERGED-VERIFY, KEEP-PARAM, KEEP-TOOL, NOT-MERGED with the reason), files and lines changed, syntax checks, and the
coordinator's list.
