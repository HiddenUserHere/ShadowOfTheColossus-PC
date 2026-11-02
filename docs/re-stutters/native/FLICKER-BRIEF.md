# Flicker brief (1 Nov): native mode without page tracking flickers

You are a specialist of the SotC PC port (native Windows port of PS4 Shadow of the Colossus; read CLAUDE.md at the
repo root first). Plan: docs/re-stutters/native/PLAN.md. Goal of this wave: find EVERY cause of the remaining visual
bugs of `SOTC_NATIVE=1` (page tracking off) and fix them for good. Go deep: never stop at the first finding; check
each hypothesis against the code, the Ghidra decomp and the run evidence below, and report what stays unknown.

## Rules
- Your worktree starts on commit 1b3d7a8. First `git checkout --detach 1b3d7a8`, then apply the uncommitted work of
  the main tree: `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/flicker-wip.diff`
  (it holds every change described under "State" below). Work on top of it.
- Do NOT commit. Do NOT run the game (only the coordinator runs it; one instance at a time). Leave changes
  uncommitted in your worktree; the coordinator takes `git diff` and measures.
- Syntax-check C/C++ with MSYS2 CLANG64 (`source msys2-env.sh`, clang++ -fsyntax-only with the flags of
  C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json, third-party includes from the main tree).
- RE: Ghidra decomp first (C:/Users/ptr/Documents/sotc-pc/out/re/decomp/<bucket>/<offset>.c, 8 hex digits, bucket =
  first 4), names in out/re/ghidra/names*.txt; `python tools/re/eboot_re.py func|dis|callers|sig|sigfind` (run from
  C:/Users/ptr/Documents/sotc-pc). Disassembly only to confirm patch bytes.
- Code: clean, English comments, `sotc:` markers in vendored shadPS4 files, follow the surrounding style. No new
  user switch. Constraints: no GPU wait except frame pacing, no extra RAM/VRAM of note, no added input latency.
- Final report in your last message (not a file): findings with evidence, every file changed, check results,
  what a game run should show (counters), unknowns.

## The model (what mode 1 does)
- `SOTC_NATIVE=1`: no guest page is protected; the buffer cache learns CPU writes only through
  `SotcNative::Announce` (gpu/sotc/native/native.{h,cpp}); `BufferCache::NativeDrain` (draw thread, at submit start,
  pc_ring_copy.cpp OnSubmitStart) marks each announced range CPU-modified (page granular; uploaded at the next
  bind) or, over GPU-modified memory, copies exactly the announced bytes (`NativeExactUpload`). GPU-written bytes are
  never read back to guest memory (`ReadMemory`, `DownloadMemory`, `DownloadImageMemory` skip in mode 1).
- Announce sources: eboot Site hooks (scripts/hooks.d/native_writes.py, gpu/sotc/native/native_hooks.cpp: DCB/CCB
  submits, per-frame heap chunks, descriptors, materials, ...), every guest memcpy/memset/memmove (src/runtime.c,
  PerFrame), file reads (src/runtime_file.c), ACE ring dwords.
- GPU->CPU results: XPR visibility masks and particle counts are served from a GPU copy ring
  (gpu/sotc/native/native_copies.{h,cpp}, native_masks.cpp, native_particles.cpp): the consumer gets the newest
  COMPLETE copy (frame done) of the same key, usually the previous frame's. Cull label spins end at record time.
- Mode 2 (`SOTC_NATIVE=2 SOTC_VERIFY=1`) keeps page tracking and runs oracles: unannounced writes the GPU took
  ("SOTC VERIFY CONSUMED", per eboot RIP), CPU reads of GPU-written memory, mask bits that differ from the real mask.
  Mode 0 is the old UMA emulation (the reference: no flicker).

## State (fixes already in the diff)
1. Every guest memcpy/memset/memmove announced (the Shrine facade was missing without it).
2. File reads announced. 3. Byte-exact upload over GPU-modified memory (a page mark uploaded stale guest bytes over
   GPU results). 4. Announces never invalidate GPU-modified images (else the facade breaks and black rectangles
   appear). 5. Masks: when no complete copy of the last 3 frames exists, the last mask served for that key is served
   again instead of a zero mask.

## Remaining bugs and evidence
- User (free play, own save): grass flickers with other textures (maybe shadows), some places invisible for moments.
- Measurement: Lab route user/routes/visual.json, camera parked 2.5 s at 18 stops, 6 consecutive frames per stop
  (C:/Users/ptr/Documents/sotc-pc/user/shots/<run>/, PNG, run.log). Max frame-to-frame mean abs diff per stop, mean
  over stops: mode 0 reference 0.86 / 0.92 (fl_ref, fl_ref2); mode 1 1.51 (fl_nat); mode 1 without byte-exact
  uploads 1.29 (fl_noexact); mode 1 without served particles still high (fl_nopart). The difference is in the grass
  near the camera: tufts appear/disappear between consecutive frames with a parked camera
  (user/shots/fl_nat_diff.jpg).
- Mask counters (mode 1, run.log "SOTC NATIVE_MASKS"): "zero masks (no complete copy)" ~820-850 per 5 s window right
  after load, ~38 later with fix 5 ("last" 81). In an earlier tracking-on session it was 0.
- Verify mode leftovers (user/pcmem/native-verify14, -15 logs): unannounced consumed writes are now mostly allocator
  metadata (sub_179c0 0x17c4f, BpeHeap 0x48b1a..0x4a3e5) and CPU object fields (0x1a9314, 0xbbbe4); CPU reads of
  GPU-written memory: 0x101a98 (particles, served), 0x17b02, 0x135719, 0xf9c70, BpeHeap (CPU data on GPU pages).
- Intermittent (2 of ~12 runs): read access violation in BpeHeap code (0x490a0 / 0x49047) logged as "stray read
  fault (no tracker protection)" in mode 1.
