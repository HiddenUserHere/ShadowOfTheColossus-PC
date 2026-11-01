# PC memory model rewrite: handoff (1 Nov 2026)

Plan and status: docs/re-stutters/PC-MEMORY-REWRITE.md (section 8 = switch table). Reports: docs/re-stutters/reports3/
(01-14, draw/). Session transcript: C:\Users\ptr\.claude\projects\C--Users-ptr-Documents-sotc-pc\f1b0221a-10cf-4fa9-af25-38f34ee2e5c8.jsonl

NOTHING IS COMMITTED. Branch re/stutter-architecture, all work is in the working tree.

## How to test
- Build: `source msys2-env.sh; ninja -C out/gpu bb-probe && cp out/gpu/bb-probe.exe out/` (check rc before copying).
- Game runs: `python tools/pcmem_matrix.py --configs <names>` (one round, Lab route user/routes/tests.json; configs in
  SWITCHES at the top of the script: base, ring8, poolfull, cpuonly1, place2, rbahead, drawall, attrib, ...).
- Hooks: `C:/Users/ptr/AppData/Local/Programs/Python/Python312/python tools/re/hooks_install_check.py` (52/53 ok).
- Unit tests: gpu/sotc/tests/*_test.cpp (command in each header, clang64 bin on PATH).

## Measured in game (latest)
- Native pool allocator (SOTC_PC_POOL_NATIVE=1, SOTC_PC_POOL_PATHS=all wired paths): 6454/6454 calls native and
  verified, 0 kills, 28.5 FPS. fresh-reserve/fresh-poolfix never occur in the route. Refill opt-in pending.
- ring8 (SOTC_PC_RING=8, GPU reads the per-frame arena from imported host memory): 28.3 FPS (base 29), write faults
  -35% eboot / -73% host. User sees A FEW WEIRD VERTICES: suspected arena half reused while the GPU still reads it
  (EOP labels written at parse time). Agent was adding a GPU wait at the ring flip (0x8c860).
- ring7 (modes 6/7): boot hang, dropped in favour of ring8.
- CPU_ONLY (SOTC_CLASS_TRACK=1): bind fast path fixed the cost (27.5 FPS), but it publishes only 186 MiB; faults barely drop.
- rb2 vertex corruption: serving stale data is off by default (SOTC_RB_CLASS2_PARTS=0); readbacks 760 -> ~557-584/s.
- Arena writers after submit (SOTC_ARENA_ATTRIB=1, tools/arena_attrib.py): only ~3.6% of arena faults are rewrites
  (mostly memcpy from guest code and the arena allocator's own headers).
- RAM (SOTC_RAM_REPORT=1): working set 5.9 GB, guest resident only ~0.8 GB; ~4.4 GB port/driver not yet split.
- Draw-thread switches (TEX/BUF_BIND_FAST, VK_POOL, PRESENT_UNLOCKED, UD_SWAP, HLE_TABLE_GEN): no measurable gain yet.

## In flight when the session ended (may be partly applied in the tree)
ring8 flip wait + straddle dedupe; native refill enable (SOTC_PC_POOL_REFILL); CPU_ONLY coverage; placement glue
signals + windows + stage 3 fit; texture protect scope + switches review; static/RT class completion; RAM split;
safe ring8 labels (SOTC_PC_RING8_LABELS). Rebuild, check what compiles, then run the matrix.

Done after the summary: texture protect scope in vk_rasterizer.cpp (Draw/DrawIndirect/Dispatch*), deferral only for
dirty images that are uploaded in the same UpdateImage; review in docs/re-stutters/reports3/draw/switches-review.md.
Safe ring8 labels done (SOTC_PC_RING8_LABELS=1, off by default; liverpool.cpp Ring8ScanSubmit lookahead, CP-wait
flush, 500 ms watchdog). Cause of the old hang: nothing polled the deferred label while the CP spun on it. Untested.
ring8 vertex fix done (untested): BufferCache::PcHostFlipWait at each frame flip (needs SOTC_HOOK_FRAME_FLIP=1,
default on in mode 8, SOTC_PC_RING8_FLIPWAIT=0 off) waits for the GPU ticks of the arena half about to be reused;
straddle dedupe keyed per draw-side submit (SOTC_PC_RING8_DEDUPE=0 off). Log: "SOTC PC RING8 flip waits ...".
Static class mode 3 (skip the page scan for armed clean mesh ranges) written in static_class.cpp; NOT wired: the
buffer_cache.cpp hunks (TrySkip after the SYNC_FAST slot block, NoteClean after SynchronizeMemoryFull) are in the
agent's report in the session transcript (search "SOTC_STATIC_CLASS=3"). RT class: no measured gain; VRAM-only RTs and
read-fault readbacks not done. Neither showed an FPS gain in game so far.
Placement glue: signals fixed (own gate SotcPcPlaceGlue::On(), heap context, carves), automatic windows, window fit
FitInWindow written. Stage 3 NOT wired: the pc_pool.h/pc_pool.cpp/pc_pool_native.cpp diff (steps A-F) is in the
agent's report in the transcript (search "WithWindowFit").
BUILD: OK again (ninja rc=0 after the RAM agent finished). RAM report now splits private memory (loader image,
thread stacks, process heaps, mimalloc, staging, caches, pipelines, remainder).
CPU_ONLY: memtype labels fixed (were frozen "unnamed"), SOTC_CLASS_TRACK=1 now applies every pool except TaggedArena
by default (SOTC_CLASS_POOLS=PageAllocator = old behaviour). The faulting memtype granules are GPU-marked, so few
faults are avoidable; the real lever is the granule cap (32768, full at boot; ~1000 faults/s unattributed): raise
kMaxGranules / kGranuleTable (probe_memtype.cpp) and kMaxGranuleEntries (mem_classes.h).
Allocator refill done: SOTC_PC_POOL_REFILL=1 (off by default); dispatch moved to gpu/sotc/pc_pool_native_dispatch.*
(new file: re-run cmake/build.sh so the glob picks it up). Test: SOTC_PC_POOL_NATIVE=1 SOTC_PC_POOL_REFILL=1
SOTC_PC_POOL_PATHS=flex-exact,flex-node,primary-exact,primary-node,fresh-plain. Many new files are UNTRACKED
(gpu/sotc/pc_pool*, pc_ring*, mem_classes*, ...): `git add` them when committing.

Run 20261101-064529: base 28.2 FPS; ring8 21.8 FPS (draw busy 47 ms), flip waits on EVERY flip (avg 8.3 ms, max 49 ms:
the GPU lags ~1 frame), write faults 1870/709 vs 3102/2828, straddles 0; vertices still a bit weird -> the half reuse is
not the (only) cause and the wait costs ~6 FPS (consider SOTC_PC_RING8_FLIPWAIT=0 for the next test);
poolfull 28.9 FPS; cpuonly1 27.3 FPS.
Run 20261101-064930 (ring8, FLIPWAIT=0, LABELS=1): no hang, labels deferred 116-147 of ~23-29k, forced 0; vertices
STILL weird; FPS still 21.8 (draw busy 47) even without the flip wait -> the FPS loss is elsewhere (suspect: the
flip-wait agent made liverpool run OnSubmitStart/PcHostSubmitStart on every mode-8 submit; the 062604 build ran ring8 at
28.3 FPS). Early windows show 1559 straddles and "normal-path CPU marks 9.8 GiB" per 5 s: the 64 MiB binds that start
inside the 32 MiB arena block go through the normal path, so their arena part is a DEVICE COPY uploaded once per
submit (dedupe) while other binds read the live host bytes -> two views of the same bytes; leading suspect for the
remaining weird vertices. Candidate fix: serve a straddling bind from the host view clipped to the block end (the V#
size is likely just a large num_records), or upload its arena part on every bind (no dedupe) and compare.
ring8 still shows a few weird vertices (user, last build). Steps: (1) confirm "SOTC PC RING8 flip waits N" with N>0;
(2) try --env SOTC_PC_RING8_LABELS=1 (GPU-written arena data read by the CPU before the GPU finished, e.g. particle
stats); (3) if still there, cover ReleaseMem / EventWriteEos labels too.

Straddle cause found (1 Nov, later session): the 64 MiB binds are SHADPS4_UNBOUNDED_BUFFER_MB windows of unbounded
V#s (vk_rasterizer.cpp), not game sizes; each marked ~33 MB for upload (16 GB per 5 s, the FPS loss), and memoized
hits skipped PcHostMarkCpu entirely (stale device copy). Fix: BufferCache::PcHostClipLimited clips a limited
read-only bind that starts in an imported run to the run end (SOTC_PC_RING8_CLIP, default on; 0 = old path), and
binds with ArenaRelation 2 are never memoized. Report: "clipped N" in the "SOTC PC RING host" line.
Run 20261101-065448 (FLIPWAIT=0, LABELS=1): base 28.6 FPS p99 67.1 max 136.6; ring8 28.9 FPS p99 34.5 max 36.4,
draw busy 34.7 ms (was 47), straddles 0, CPU marks 0, ~1.2-3.5k clips per 5 s, write faults 2820/829 vs 3651/2849.
(That build printed clips in the "fallbacks" field and fallbacks in "written binds": format string fixed after.)
User confirmed (1 Nov): vertices are clean with the clip. Defaults changed: SOTC_PC_RING8_FLIPWAIT off (=1 enables),
SOTC_PC_RING8_LABELS on in mode 8 (=0 disables). New matrix config ring8pool (ring8 + poolfull).
Run 20261101-065841 (new defaults, one round): base 28.6 FPS p99 50.5 max 159.9, faults 3275/2862; ring8 29.1 / 41.3
/ 67.0, faults 2102/738, clipped ~900/s, straddles 0, fallbacks 0; ring8pool 28.7 / 66.7 / 66.9, faults 1784/658,
pool native 6447/6447 verified, 0 kills. No crashes or warnings. ring8 is still opt-in (SOTC_PC_RING=8).
Loading-screen glitch (user: quick blue/white lines on the loading screen with ring8): captured with
BB_FRAME_DUMP_TRIGGER + SOTC_SHOT_COUNT (scratchpad bisect.sh / flicker.py): rare frames with a full-screen quad
drawn with wrong vertices. Only the runs where an arena block LEFT the ring ("host maintain ... was 4" -> 3) showed
it (2 of 2; 8 runs without a leave were clean; CLIP=0 still showed it). Cause: pages leaving the ring went back to
normal tracking with their unseen CPU writes, so the stale device copy counted as clean. Fix: PcHostApply marks the
pages that leave CPU-modified (log "SOTC PC RING8 left the ring"). Run 20261101-070814 ring8 28.8 FPS p99 42.1 max
50.3, one leave marked. NOT yet confirmed by eye. Note: per-frame PNG saving stalls the loading (route never runs,
base too), so captures are only good for the first ~100 frames. One ring8 run (070721) ended at 11 s with no crash
log; the rerun was fine.

Committed 6ef9c26 (everything up to the loading-glitch fix).
Untested-pieces matrix 20261101-071620 (one round): base 27.8 FPS; cpuonly1 27.1 (publishes 488.6 MiB now, faults
unchanged, 4 bind trips retracked; faults sit in memtype:CPUMemory 1198/s, unpublished: granule cap); place2 28.0
(signals arrive now: 28.7M binds, 138k writes, 33559 carves all "unruled" -> no rules, verify checked 4 blocks);
rbclass1 29.1 / 597 readbacks/s vs rbahead 28.4 / 527 (-12% readbacks, no FPS gain, stats mismatch 0);
poolrefill 28.5, 6453/6453 native verified, 0 kills, but no refill counter line (unknown whether a refill ran);
texbatch 27.6 (deferred ~500 of 3M flush points: no effect); recycle1 retires 0: Glue::OnFlushDone is only called
by the unit tests (needs the PC_POOL_MAP hook, native pool on, and placement stage 3 to be useful).
RAM report fixed: the mimalloc line used mi_process_info (= OS process commit); now mi_stats_get: 678 MiB.
Private remainder ~5.2 GB; new line "SOTC RAM private largest allocations" (top 8 ~2.1 GB: mimalloc 678,
0x36070000 656, 0x7ff0000 256, three 128 MiB, loader 77, 64): the rest is many smaller allocations.

Granule cap raised 32768 -> 131072 (probe_memtype.cpp kMaxGranules/kGranuleTableBits, mem_classes.h
kMaxGranuleEntries, static_assert ties them). Run 20261101-072816 (cpuonly1, SOTC_CLASS_WARMUP_S=5): 90991 granules
claimed, overflow 0. Full fault attribution (write faults/s): memtype:CPUMemory 2415, RenderSingleFrameGPU 1835,
GPUMemory 1559, slot22 288, slot21 23; never-GPU-bound only ~33/s. Conclusion: CPU_ONLY publishing cannot cut
faults (the port protects only GPU-read pages, so a CPU-only page never faults); cpuonly1 costs ~1.8 FPS
(26.4 vs 28.2, draw busy 38.1 vs 35.6) and trips 20 times. Drop CPU_ONLY as a fault lever. The faults are CPU
writes to GPU-read memory: RenderSingleFrameGPU (83.5 MiB, likely the per-frame arena ring8 already serves) and
bound CPUMemory granules (935 bound of 32797): the next lever is serving those from host memory like ring8.
Warm-up is not why granules stay "warming": NoteWarmup runs only from the draw thread's sample feed
(NoteGranuleGpuPages), which never reaches most granules.

Precise flip wait (SOTC_PC_RING8_FLIPWAIT=2, now the default in mode 8; 1 = old drain wait, 0 = off): the
idle frame-end path in liverpool.cpp records (SubmitDone seq, flushed tick) through
BufferCache::PcHostNoteFrameRecorded; at a flip the game thread waits only for the tick of the frames that ended
before the previous boundary (no WaitGpuIdle drain; 200 ms cap). Run 20261101-074045: ring8 27.5 FPS vs base 26.7,
max 84 vs 176 ms, flip wait ~1 us avg. Verify run 074212: ~0.1% of sampled read binds changed before the next
submit (inflated by clipped binds that reach the block end). Vertices with this build: waiting for the user.
Heap serving (SOTC_PC_RING8_HEAPS=CPUMemory, off by default; mem_classes CollectHostServe, pc_ring
SetHostExtras, pc_ring_copy CollectHeapRuns): written and built, not yet run.
Heap serving measured (074556, 074852, 075018): CPUMemory faults -27..-31% (2122-2288 -> 1471-1678/s), host
-35%, but FPS 21.6-23.0 vs ring8probe 25-26 vs ring8 28.6 (draw busy 44-48 ms). The probes alone (memtype +
classes + class track 2) cost 2.6-3.6 FPS. Fixed on the way: refused imports were retried every maintain (5364
log lines), HostMaintain rechecked ~320 runs per submit with an O(n^2) held list. Remaining cost: the run set grows
by 1-3 granules about once a second and every change re-applies the whole ring (ApplyRing over all region
managers, 300+ ranges) plus the full re-import walk; and the binds of the new runs. To make it pay: incremental
ring add (AddRingRange, as mode 5), whole heap regions instead of 64 KiB granules (few imports, no class track),
and a cheaper heap source than the memtype probe. Parked: off by default.

## Next
Fix ring8 vertices, combined matrix (several rounds), long play test, AMD/Intel, enable proven switches by default,
CHANGELOG (Portuguese) and docs, commit. Progress estimate ~74%.
