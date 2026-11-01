# PC-native engine: N0 reverse-engineering inventory, rules for every specialist

Goal of the whole effort (plan approved 1 Nov 2026): stop emulating the PS4's unified memory. The game (Bluepoint's
Shadow of the Colossus engine, eboot of CUSA08809 EU 1.01 / CUSA08034 US 1.00, same build) will be patched so it
behaves like a native PC engine: it announces the GPU-visible data it writes (explicit uploads, or writes into
host-shared memory the GPU reads in place), the CPU never reads GPU memory, and no CPU thread waits for the GPU except
the frame-pacing fence. Then the port deletes its page tracking, fault readbacks and region compares.

N0 is the inventory that makes this possible: every CPU->GPU write path, every GPU->CPU read path, every CPU wait on
the GPU, with the exact patch point for each.

## Sources (in priority order)
1. The Ghidra decompilation we already have: per-function C in `out/re/decomp/<bucket>/<offset>.c` (the file name is
   the function's eboot offset in hex), names in `out/re/ghidra/names.txt` / `names2.txt` and `tools/re/names.txt`,
   index `out/re/index.pkl.gz`, signatures and helpers in `tools/re/eboot_re.py`. Read C, not assembly.
2. What the port already knows: hook definitions and their notes in `scripts/hooks.d/*.py` (allocator probes,
   memtype heaps, XPR masks, particle stats, frame flip, pc_pool, static class), the probe code in `gpu/sotc/probe_*.cpp`,
   `pc_pool*.cpp` (the game's block allocator, fully modelled), `rb_class.cpp`, `particle_prev.cpp`, `static_class.cpp`,
   docs in `docs/re-stutters/**` (reports2/, reports3/, PC-MEMORY-REWRITE.md, STUTTER-ARCHITECTURE*, FIRST-FRAME-PEAK.md).
3. Runtime evidence in logs: `user/pcmem/*/logs/*.log` (write faults by eboot RIP, "SOTC READBACK RIPS", memtype
   heap names and fault counts, "SOTC CLASS pools"), traces in `user/trace/20261101-085933`.
4. Raw disassembly (REA tools, objdump of the eboot) ONLY to confirm the exact bytes of a patch site.

## Rules
- You are the specialist of your area. Go deep: never stop at the first finding. Enumerate every function of your
  area (callers and callees, vtables, job-system entry points), cross-check each against the runtime evidence, and
  list what remains unknown with what would resolve it.
- Do not edit engine/port code (C/C++). You work in your own worktree; write only your report
  `docs/re-stutters/native/<your-id>.md` (and helper scripts under `tools/re/` if useful), commit them on your branch.
- Do NOT run the game or tools that start it.
- For every site give: eboot offset and decomp function name, what it writes/reads/waits on (address source, size,
  frequency per frame if known), the memory class (per-frame / persistent / GPU-result / sync), the proposed patch
  point (hook entry/wrap/return, or a byte patch) and what the patch would call (announce write (addr,size), write into
  host-shared region, drop the consumer, read without waiting), and the evidence (decomp lines, log counters).
- Return: the inventory table, the count of sites by class, open questions, and your branch name.
