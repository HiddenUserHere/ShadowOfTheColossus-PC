# N5: modern render architecture, brief for every specialist

Context: docs/re-stutters/native/PLAN.md section N5 and docs/re-stutters/reports3/draw/FINAL-DESIGN.md. With
SOTC_NATIVE the memory model is native (windows read in place, announced uploads, frame ticket: gpu/sotc/native/).
N5 rebuilds the execution side so a frame of ~6,500 ops costs a few ms of CPU spread over workers (30 FPS locked,
60 FPS ready). Targets: NVIDIA, AMD, Intel Arc on current Windows drivers; Vulkan 1.3 baseline; shader cache
rebuild accepted; no extra RAM/VRAM beyond what replaces existing allocations; no added input latency.

Follow docs/re-stutters/native/IMPL-BRIEF.md (worktree, copy files from the main tree first, never commit,
syntax-only checks, no game, Ghidra decomp when engine behaviour matters) with these additions:
- Everything new runs with SOTC_NATIVE on plus your feature switch (SOTC_N5_<NAME>=1, =2 verify where state can
  diverge); mode 0 and SOTC_NATIVE without your switch behave exactly as today.
- Probe every Vulkan feature at device creation (vk_instance.cpp) and fall back when one is missing on the
  running GPU; log the fallback once.
- Measure-oriented counters in a 5 s report line; give driver.cpp snippets.
- Your report impl-n5-<id>.md in your worktree (uncommitted) AND returned, with snippets for files you do not own.
