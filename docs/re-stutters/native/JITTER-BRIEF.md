# Jitter brief (1 Nov): small fast jitter on animated and static objects, mode 1 only

You are a senior specialist of the SotC PC port (read CLAUDE.md at the repo root, then
docs/re-stutters/native/FLICKER-BRIEF.md and FLICKER-BRIEF2.md for the model and history). Find the ROOT CAUSE. Go deep:
do not stop at the first finding; check every claim against code, Ghidra decomp and run logs; report unknowns.

## Rules
- Worktree: `git checkout --detach 1b3d7a8`, `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/flicker-wip4.diff`,
  then copy C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/wip4-new/n1verify.{h,cpp} into gpu/sotc/native/.
- Do NOT commit. Do NOT run the game (the coordinator runs it; write the exact env and counters you need).
- Syntax-check with MSYS2 CLANG64 (`source msys2-env.sh`, clang++ -fsyntax-only, flags of out/gpu/compile_commands.json).
- RE: decomp at C:/Users/ptr/Documents/sotc-pc/out/re/decomp/<bucket>/<offset>.c; `python tools/re/eboot_re.py` from the main tree.
- Clean code, English comments, `sotc:` markers in vendored files, no new user switch, no GPU wait except frame pacing.
- Final report in your last message: root cause with evidence, files changed, checks, the confirming run, unknowns.

## State
The parked-camera grass flicker is FIXED: NATIVE_ZC_CREATE was never installed (NATIVE_RES_COPY held 0x8be60 and the
detour engine refuses a second detour in a function); RES_COPY was removed (guest memcpy is announced anyway) and
new Site/Wrap hooks announce the blocks filled outside that path (NATIVE_UBUF_*, NATIVE_SUNSHADOW_*, NATIVE_UDATA_*,
NATIVE_PALETTE_A3EE50). Parked-camera frame diff now 0.83 = mode 0 (0.86/0.92). The in-mode-1 detector
(SOTC_NATIVE=1 SOTC_VERIFY=n1, gpu/sotc/native/n1verify.cpp) shows 5-25 remaining "bound_miss" per window, mostly
allocator code (sub_17e90 0x17f25/0x17f4f/0x18019..., sub_181258) through large binds.

## The remaining bug (user, free play)
"The horse animation, the cape, the grass, the stairs show a minimal, very fast jitter." ONLY in mode 1 (the user
compared mode 0 at the same place: no jitter). Animated meshes (skinned horse, cloth cape, wind grass) and static
stairs: this looks like per-frame data of one frame used with another frame's data (e.g. motion vectors / TAA
reprojection with previous-frame matrices, skinning palettes one frame early or late, view constants vs object
constants from different frames, data uploaded at the wrong time).

## Mode-1 timing facts to check
- Announces are queued (SotcNative::Announce) and drained by BufferCache::NativeDrain at submit start on the draw
  thread (pc_ring_copy.cpp OnSubmitStart), budget 64 MiB per drain; PerFrame ranges are only page-marked (uploaded at
  the next bind, from guest memory as it is at bind time); ranges over GPU-modified memory are copied byte-exact AT
  DRAIN TIME (NativeExactUpload). The queue is not always empty between drains ("queued 300..1083" in snapshots).
- The draw thread lags the CPU; the engine's per-frame heaps rotate with ring depths 2-5 (native_draw.h RingDepth,
  "reuse fences"). Mode 0 uploaded at bind time too, but CPU writes were seen at the fault.
- NATIVE_ZC_CREATE announces at creation; some builders fill after the call; chunk announces happen at submit.
- Log of a free-play mode-1 session with the fix: C:/Users/ptr/Documents/sotc-pc/user/native-visual8.log; mode 0:
  user/default-visual.log. Detector run: user/shots/n1v2/run.log.
