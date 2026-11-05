# Command path: where it costs, and the design that removes the cost

Agent C1 of the arch-next round. Read-only: no source edits, no builds, no game runs, no git changes. Every
figure below comes from a log or a document already in the repo (file and line given). Nothing was re-measured.
Labels: **measured** (a log line), **documented** (a doc table that cites its own source), **inferred** (reasoning
from the figures above it), **unknown** (not in any artifact). Line numbers are for the tree on branch
`arch/gcn-native` as read on 4 Nov 2026.

## 0. Summary

1. The frame is paced by the draw thread (`shadPS4:GpuDraw`), not by the command processor. In the steady window
   used below the draw thread is busy 31.3 of 34.4 ms per frame. Its cost is almost all **resource resolution**
   (`BindResources`, 14.7 ms) and **dispatches** (11.6 ms). Pure Vulkan command emission is about 1 ms.
2. The command processor (CP) does about 9.6 ms of real work per frame. Its largest part is the **pipeline
   lookup**, and the largest wait is the **back-write wait** on the draw thread (6.9 ms, which is the CP's
   "blocked" time). Its PM4 decode itself is about 3 ms per frame at the heavy-window load.
3. Of the five designs, the two with a ms-scale gain today are not in the list: finishing the GCN-native resource
   stages (`ARCH-GCN-NATIVE.md`, S2-S6) removes about 6 ms of bind work and about 5 ms of dispatch work per frame
   (basis in section 3.1). The command-buffer memo (b) is worth measuring first: the repo has no evidence that
   whole command buffers repeat byte for byte, and the engine rebuilds them each frame. Gnmx-layer HLE (c), faster
   PM4 (a), multi-draw (e) and native passes (d) have small or unmeasured gains on the current critical path.
4. Recommended order: measure the repeat rate of normalized command buffers (no behaviour change), then ARCH-GCN-NATIVE
   S2/S3 (bind), then S5/S6 (dispatch), then a command-replay cache (b) only if the measurement says so. c is for later,
   when the CP becomes the limit at 60 FPS.

---

## 1. Measured cost per frame

### 1.1 Windows used

| Id | Source | Scene and load | Frame rate | Why this window |
|---|---|---|---|---|
| W1 | `user/pass1/pass_probe.log` lines 2844-2864 (5 s window, 147 frames) | 3,413 cmds/frame, 3,518 ops/frame (`SOTC DRAW WORK`, line 2851) | 29.1 FPS (30 FPS cap) | The most complete steady window in the logs: every counter printed, bind cache 88% hits |
| W2 | `docs/re-stutters/native/data/cpucull-perf.log` lines 1303-1313 | 6,684 cmds/frame | 29.4 FPS | Second steady window, different route state |
| W3 | `docs/re-stutters/native/data/cpucull-perf.log` lines 1013-1024 | 9,526 cmds/frame | 22.7 FPS | Heavy window, the CP and draw costs grow with the load |
| D1 | `docs/DRAW-THREAD-7C-DESIGN.md` section 0 and the CP profile table | 15 heavy windows (29 Oct) | 30 FPS cap | Only source that splits the draw thread by phase and the CP by function |
| D2 | `docs/re-stutters/native/ARCH-GCN-NATIVE.md` sections 8.1-8.2 | 2,800-3,700 ops/frame average (4 Nov) | 30 FPS cap | Per-op and per-phase costs, cross-check of W1 |

The 30 FPS cap hides the real ceiling. W1 is 91% draw-thread busy, so the uncapped rate would be about 32 FPS.
Frame time is 1000 / 29.1 = 34.4 ms (inferred from line 2864).

### 1.2 Cost table (ms per frame, W1 unless marked)

| Part | Thread | ms/frame | Basis |
|---|---|---|---|
| Frame time | all | 34.4 | 1000 / 29.1 FPS (line 2864) |
| Draw thread, wall busy | GpuDraw | 31.3 | `threads per frame: draw busy` (line 2850) |
| Draw thread, on-CPU | GpuDraw | 28.7 | `cpu/frame: draw` (line 2852); idle spin 0.7, idle 3.1 (line 2857) |
| - ops `draw` | GpuDraw | 2.9 | `DRAW SPLIT` (line 2857) |
| - ops `indirect` | GpuDraw | 10.2 | line 2857 |
| - ops `dispatch` | GpuDraw | 11.6 | line 2857 |
| - ops `func` (label writes, write data, DMA) | GpuDraw | 5.6 | line 2857 |
| - of which `bind resources` (inside the ops above) | GpuDraw | 14.7 | line 2857: buffers 5.0, textures 3.2, dma 0.3, rest 6.2 (inferred) |
| - draw work per op | GpuDraw | 7.7 us/op (27.24 ms / 3,518 ops) | line 2851 |
| Command processor, on-CPU (includes spin) | GpuCommandProcessor | 26.6 | `cpu/frame: cp` (line 2852) |
| - PM4 parse, wall | GpuCommandProcessor | 16.5 | `cp/frame: parse` (line 2854) |
| - - blocked on the draw thread | GpuCommandProcessor | 7.0 | line 2854; equals `back-write waits 14.2 (6.89 ms)` (line 2848) |
| - - net parse (real CP work) | GpuCommandProcessor | 9.6 | line 2854 |
| - label spin (no-progress rounds, WAIT_REG_MEM) | GpuCommandProcessor | 11.6 | `CP spin` (line 2855), counted in CPU, not in parse |
| Recorder, replay busy | GpuRecorder | 5.5 | `recorder/frame: busy` (line 2856) |
| Recorder, queue submit | GpuRecorder | 0.8 | line 2856 |
| Recorder, on-CPU | GpuRecorder | 33.9 | line 2852; **not a work figure**, see 1.4 |
| Async upload thread, on-CPU | sotc:AsyncUpload | 2.0 | line 2852 |

Second and third windows, same fields:

| Part (ms/frame) | W2 (6,684 cmds) | W3 (9,526 cmds) |
|---|---|---|
| Draw busy (wall) | 29.3 (line 1303) | 36.2 (line 1013) |
| CP parse wall / blocked / net | 18.2 / 6.8 / 11.4 (line 1306) | 25.4 / 8.2 / 17.2 (line 1016) |
| CP on-CPU | 25.8 (line 1304) | 35.8 (line 1014) |
| Recorder busy | 6.6 (line 1308) | 8.9 (line 1018) |

Net CP work per command: 2.8 us (W1), 1.7 us (W2), 1.8 us (W3). Net parse grows less than linearly with commands
(commands x2.8 from W1 to W3, net parse x1.8).

### 1.3 Where the CP's time goes (documented, D1)

D1 sampled the CP for 45 s (heavy windows, CP 22-31 ms on-CPU per frame) and gives shares of CP samples:

| Share | What | Converted to ms per frame (CP on-CPU ~27 ms) |
|---|---|---|
| ~29% | spin in WAIT_REG_MEM (labels the draw thread writes) | ~8 |
| ~22% | pipeline lookup: graphics 14.5% (RefreshGraphicsKey/Stages 13%, RefreshFlatBuf 9.5%), compute 7.3% | ~6 |
| ~20% | idle (condition variable, waiting for the game to submit) | ~5 |
| ~12% | PM4 parse self (Process, ProcessCommands, ProcessGraphics/Compute self time) | ~3 |
| ~6% | guest reads for SRT / flat buffer (BufferCache::ReadClean) | ~1.6 |

So the CP's decode of packets is the smallest real item. The pipeline lookup is about twice as large.

### 1.4 The draw thread's phases (documented, D1 and D2)

| Phase (ms/frame) | D1, heavy windows (26.0 draw) | D2, 4 Nov (per frame, = ms/s / 30) |
|---|---|---|
| bind resources | 12.05 (textures 4.57) | 13-15 (textures 2.2-3.5; written buffers ~2.7; clean buffers 1.5-1.8; rest ~6.2) |
| begin rendering | 1.62 | 1.0 |
| render state | 1.15 | 0.9 |
| hle (shader HLE) | 1.11 | 1.4 |
| reset | 0.99 | 0.9 |
| descriptor push | 0.68 | 0.7 |
| vertex / index | 0.42 | 0.5 |
| dynamic state | 0.39 | 0.5 |
| other | 1.86 | 4.5-5.3 |
| **Pure emission** (descriptor push + dynamic state) | **~1.1** | **~1.2** |

D1 reports 20.3 ms of phases against 26.0 ms of draw busy, so about 5.7 ms is unattributed. W1 agrees with D2 on
bind resources (14.7 against 13-15).

### 1.5 Reading of the table

- **The critical path is the draw thread.** The draw thread is busy 31.3 of 34.4 ms. The CP's extra waits are on the
  draw thread (section 2.3), and the recorder waits on nothing (line 2853: `draw waits/frame: gpu 0.00, recorder 0.00`).
- **The CP and the draw thread are coupled in both directions** at every op that reads guest memory after a write.
  `Liverpool::WaitBackWrites` (liverpool.h:797) makes the CP wait until the queued draw ops have run. Those
  waits are W1's 14.2 per frame (6.9 ms). This, not the packet parse, is the CP's main cost.
- **At 60 FPS the budget is 16.7 ms.** W1 needs about half its draw cost removed. ARCH-DRAW's targets (draw 5-9 ms,
  CP below 8 ms) remain the reference (`docs/re-stutters/native/ARCH-DRAW.md` lines 21-22).
- **Per-op costs are 6.3-12.7 us** (D2 section 8.1). Dispatches are 21-27% of the ops and cost 9.7-10.7 us each,
  about 50% more than a draw (D2 section 8.1). W1's dispatch time (11.6 ms) fits about 1,100 dispatches at 10 us.

### 1.6 Caveats on the numbers

1. `cpu/frame` is thread on-CPU time (`SotcStats` role timers, `driver.cpp:2587`). It includes spin. The CP's
   "cp 26.6" is 16.5 parse plus spin, not work.
2. The recorder's on-CPU figure (33.9) cannot be work: its busy figure is 5.5 (`vk_recorder.cpp:2603`). The consumer
   spins while idle unless `SOTC_WAIT_NOSPIN=1` (`vk_recorder.cpp:123-131`, default off). **Inferred:** about 28 ms
   of the recorder's CPU is idle spin. Measure with `SOTC_WAIT_NOSPIN=1` before using that figure.
3. `pass_probe.log` line 2848 and line 2854 give 6.89 and 6.95 ms for the same thing (back-write waits and CP blocked
   time). **Inferred** to be the same quantity.
4. The `user/prof-*.txt` files are raw samples (`bb-probe.exe+0x... count count`). They were not resolved with
   `tools/profile_functions.py` in this round, so no table here comes from them. D1 and D2 are the resolved sources.
5. D1 is from 29 Oct. Its CP function names (liverpool.cpp:2766, :3846) are out of date. Current spin accounting is at
   `liverpool.cpp:3716` (Process) and `driver.cpp:2620`.

---

## 2. The pipeline as it is

### 2.1 Threads and queues

```
Game thread (eboot code)
  writes PM4 into the DCB/CCB each frame (framedata.md section 2.3; setters 0x869350..0x86fd60, render-states.md)
        |  submit (~6 per frame: framedata.md table row 13)
        v
GpuCommandProcessor (Liverpool::Process, liverpool.cpp:3716)
  coroutines per queue (ProcessGraphics:4192, ProcessCompute:5415), packet switch, register writes
  (WriteRegsMarked:654), op capture (CaptureBackOp:1579), pipeline lookup, label/EOP handling
        |  BackOp ring (BackAlloc:1081, BackPublish:1170)
        v
GpuDraw (Liverpool::BackLoop, liverpool.cpp:680) -> ExecuteBackOp (1696)
  Rasterizer::Draw (vk_rasterizer.cpp:3092) / DrawIndirect (3309) / DispatchDirect (3770) / DispatchIndirect (3986)
  -> BindResources (4671), BindCacheSync (5916), TryBindCache (6171), CommitBindCache (6430)
  -> producer side of the recorder: every Vulkan call is copied into a 256 KiB chunk (vk_recorder.h:68-84)
        |  chunk stream
        v
GpuRecorder (vk_recorder.cpp:2440) replays the stream into real VkCommandBuffers; queue submit
  (optional) GpuPrep0/1 threads (Liverpool::PrepLoop, 1839), off by default (SOTC_PARALLEL_RECORD)
  (optional) pass secondaries (SOTC_PASS_WORKERS), inactive in W1 (line 2857, passsec 0.00)
sotc:AsyncUpload threads: uploads and the native drain
```

### 2.2 What runs in parallel and what is serial, per op

| Stage | Where | Parallel with the next op? | Notes |
|---|---|---|---|
| Register replay and op capture | CP, liverpool.cpp:654, 1579 | Yes, with the draw thread | The register block is copied into the op. Changed blocks only (`reg_same`, OpStat) |
| Pipeline lookup | CP, inside ProcessGraphics | Serial in the CP | 22% of CP samples (D1). Reads SRT and flat buffers: needs `WaitBackWrites` (liverpool.h:797) |
| Packet decode and register switch | CP, ProcessGraphics:4192 | Serial in the CP | ~3 ms/frame (D1) |
| Label wait (WAIT_REG_MEM) | CP, Process | Serial; the CP stops until the draw thread writes the label | 11.6 ms spin/frame (line 2855), 2-3 waits per frame (D1, CP profile reading) |
| Prepared draw slot (PrepLoop, TakePrepared:1946) | prep threads, if SOTC_PARALLEL_RECORD=1 | Yes, when on | Off in the user's config (sotc.ini does not set it). Measured: no gain (D1 section 0, "no measurable gain") |
| Resource resolution (BindBuffers, BindTextures, FindImage, ObtainBuffer, Transit) | GpuDraw, BindResources:4671 | No: draw thread only | Cache mutation (ARCH-PARALLEL-DRAW.md table). 46% of draw busy (D1 table) |
| Bind cache lookup | GpuDraw, TryBindCache:6171 | No | 85-91% hits in W1/W2-class windows (line 2844: 147,978/168,136, 88.0%) |
| Vulkan command emission (wrappers) | GpuDraw, scheduler | Serial per op, cheap | About 1 ms/frame (D1) |
| Replay of recorded commands | GpuRecorder, vk_recorder.cpp | Yes, one consumer | 5.5 ms busy (line 2856). Spins while idle by default (section 1.6) |
| Pass secondaries | GpuRecorder workers | Yes, when on | Off |
| Dispatch resource binding | GpuDraw, DispatchDirect:3770 | No | No prepared resources for dispatches (ARCH-PARALLEL-DRAW.md, "Gap"); SOTC_GCN_COMMIT (S6) prepares them |

### 2.3 The coupling that limits the pipeline

- The CP may read guest memory (SRT, sharp tables, pipeline inputs) only after the draw thread has run the ops that may
  write it. `WaitBackWrites` (liverpool.h:797) enforces that: 14.2 waits per frame in W1 (line 2848).
- Every label the game waits on is written by the draw thread after the op before it. The CP therefore stalls at
  each label (11.6 ms per frame in W1; the 2-3 label waits per frame are from D1's CP profile reading).
- Together these make the CP and the draw thread a lockstep pair, not two independent stages. Speeding up the CP alone
  does not change the frame time while the draw thread is the longer side.

### 2.4 Switches in use (sotc.ini, read as the user's default)

| Switch | Value | Effect here |
|---|---|---|
| SOTC_NATIVE | 1 | Native mode: guest memory announced, no page protection for the native path (vk_rasterizer.cpp:11143) |
| SOTC_PC_RING | 8 | Host arena binds, ring copy queued at submit start (liverpool.cpp:4204-4222) |
| SOTC_DRAW_COALESCE | 1 | Runs of DRAW_INDEX_INDIRECT in one command buffer become one op (liverpool.cpp:4616; liverpool.h:777). **No measurement in the docs** |
| SOTC_BIND_CACHE, _RANGES, _XTICK, _FRAME, _CONTENT | 1 | Bind cache, 85-91% hits (line 2844) |
| SOTC_PARALLEL_RECORD | not set (off) | Prep threads off. Measured no gain (D1) |
| SOTC_PR_RESOLVE, SOTC_PR_KEYS | not set / 0 | Prepared resources off |
| SOTC_PASSES | not set (off) | Barrier decision rule only (passes.h), not device-generated commands |
| SOTC_REG_SAME | not set (off) | Skips register writes that do not change the value. Measured in a bundle with two others: no gain (CHANGELOG.md:303-305) |
| SOTC_GCN_COMMIT | not set (off) | Dispatch prep lane (S6), needs SOTC_PARALLEL_RECORD=1 |

---

## 3. The five designs

### 3.a Faster PM4: parsing, register-block skip, draw coalescing

**What exists.** The packet parse is a switch per packet with a memcpy per register block (`WriteRegsMarked`,
liverpool.cpp:654; SetContextReg case 4388). Coalescing of indirect draws exists (`SOTC_DRAW_COALESCE`, liverpool.cpp:4616,
liverpool.h:777). `SOTC_REG_SAME` skips unchanged writes (liverpool.cpp:654-672) and is off.

**Cost today.** PM4 decode self time is ~3 ms per frame (D1). Net CP work is 9.6 ms (W1). The rest is pipeline lookup
(~6 ms), read of guest memory (~1.6 ms) and the back-write waits.

**Gain.**
- SIMD or parallel parse: at most ~3 ms of CP time per frame, and not on the critical path now (the CP waits 7 ms on
  the draw thread). **Frame gain today: about 0.** It starts to matter when the draw thread is near 16 ms, because
  then the CP's ~10 ms of work plus its label waits becomes the longer side (inferred).
- Parallel parse with a register pre-pass: the prep threads already replay register chunks (liverpool.cpp:1839). The
  measured result was no gain (D1 section 0). Not worth repeating without a new reason.
- Register-block skip: `SOTC_REG_SAME` was measured with two other switches as one bundle, and showed no gain in
  CHANGELOG.md:303-305 (21.5 ms vs 19.2 ms, the bundle being the slower). Unmeasured alone.
- Draw coalescing and multi-draw: `SOTC_MDI_STATS` measured ~3,000 indirect draws per frame, 85-90% of them in runs of
  one, and 3-12% mergeable (CHANGELOG.md:308-311). Indirect draws are 42-46% of the ops at 6.3-7.3 us (D2 section
  8.1). **Merge gain: 0.3-1.2 ms per frame** (3-12% of ~10 ms). The ceiling is low because each draw is cheap; the
  cost is the binding, not the draw.
- Pipeline-key memo in the CP (inferred target): the refresh of the graphics key and flat buffer is about 6 ms per frame
  (D1). A memo keyed by the register-block hash the CP already computes could cut it. Gain unknown; the CP is not on the
  critical path, so the frame gain is 0 until the draw thread drops.

**Risk.** Low. Verification with the =2 pattern (compute both, compare, count) is cheap.

**Verification.** `CheckPrepared`-style compare of the key and of the parsed state (the same method as ARCH-PARALLEL-DRAW.md's
"Verification" section). Gate: zero mismatches on the attract and Shrine scenes, and CP net parse down in the report.

### 3.b Command-buffer memoization (cache translated work per IB hash)

**What the repo shows about the IBs.**
- The game does not keep static IBs. It creates new render contexts each frame: `0x90860` (SetupFlipState) clears the
  context list and `0x90470` creates a DCB and a CCB every frame (framedata.md section 2.3, decompile
  `out/re/decomp/0009/00090470.c`). PC-MEMORY-REWRITE.md:122 counts 16-20 contexts per frame (DCB 5 MiB and 3 MiB).
  So the bytes are written each frame by the engine's own setters. Repeats would have to be emergent.
- The memory is recycled by a deleter gated on GPU labels. Its producer is not identified (framedata.md section 2.1 and
  open question 2). Recycling may give a two-frame address period. Address equality is therefore not evidence of
  content equality.
- Repeats that are measured are at the op level, not the IB level: the op key repeats for 82-87% of ops (ARCH-GCN-NATIVE.md
  section 2), and the bind cache hits 85-91% of its lookups with 88% of them seen in the previous frame
  (W1 line 2844: `xtick hits 145,567`). Those hits removed only about 3% of the cost per op, because the work around the
  bind (transitions, barriers, syncs) still happens (ARCH-GCN-NATIVE.md section 2).
- **Unknown:** how many whole frames or IBs are byte-identical after removing per-frame addresses. No artifact has this.

**Cost today that a memo could remove.** On an identical frame, the CP parse (9.6 ms net) and the draw thread work
(27 ms) would both be skipped. The emission that remains is about 1 ms. Upper bound on a full replay: about 10 ms
of CP and emission, plus the bind and dispatch work if the replay also skips the resolution (about 27 ms in total).

**Gain basis, realistic.** Near term (per-op memo already in place): 0-2 ms, because the existing bind cache skips
the cheap part. After ARCH-GCN-NATIVE removes per-frame addresses from the recorded state (BDA/bindless), a replay could
skip most of the 27 ms on the identical frames. The gain is proportional to the share of identical frames, which is
unknown.

**Risks.**
- Correctness of replay: the replayed commands must read the same data. Dynamic data (constants, per-frame heaps, the
  ring-8 arena) is at different addresses or with different values; a replay must take those through push data or
  buffers that are rebound. Memory tracking that would detect a change is off in native mode
  (vk_rasterizer.cpp:11143).
- Timing: the game's allocator depends on GPU completion. `SOTC_CEILING=sync` (skipping memory sync) breaks the game because
  its command-buffer allocator returns null (CHANGELOG.md:285-286). A replay that changes when labels or EOPs fire could
  do the same.
- Memory: stored command buffers add RAM/VRAM. The target is no extra memory (ARCH-DRAW.md section "Target").

**Verification.**
- Step 0 (measurement, no behaviour change): hash each submitted DCB segment at the submit announce (the point of
  `0xd07c0` in framedata.md). Hash it twice: raw bytes, and bytes with addresses inside the per-frame heap windows
  replaced by a placeholder. Count identical frames and IBs. Gate for a memo: at least 50% of frames identical after
  normalization, in the heavy windows.
- If the gate passes: `=2` compares the replayed recorder stream (the producer's closures, vk_recorder.h:68-84) with the
  fresh one, command by command, and counts differences; plus frame hashes at fixed frame numbers against the PM4 path.

### 3.c HLE at the Gnmx layer: intercept the setters 0x869350..0x86fd60, draw functions 0x86b040 and 0x86ac10, shader binders 0x85fc10

**What it is.** The eboot's inline state writers (about 80 functions, 0x869350..0x86fd60) each write one 3-dword PM4
packet (render-states.md, summary item 2). The draw functions 0x86b040 and 0x86ac10 and the shader binder 0x85fc10 sit
above them (materials-meshes.md, lines 151-152; slot-stages.md, line 132). The port already has an engine-hook mechanism
(101 declared hooks, `hooks.bin`, printed at session start, `user/p3-prof4.txt`) and native hooks in
`gpu/sotc/native/native_hooks.cpp`.

**Important: these functions run on the game thread, not on the GPU threads.** The gain on the GPU threads is only the
CP's decode of those packets.

**Cost today.** PM4 decode self is ~3 ms per frame, and the setter packets are a part of it (D1). Game-thread cost of
the setters is unmeasured.

**Gain.**
- On the GPU threads: at most ~1-2 ms per frame (half of the decode self time, assuming setters are about half the
  packets). Not on the critical path now.
- On the game thread: unknown. It matters only if the game thread is near its own limit, which no artifact shows.
- As an enabler: typed state from the hook would let the pipeline key and the bind keys be built without
  decoding (D1 shows the lookup is the biggest CP item). That is the real value, and it depends on design 3.a or ARCH-GCN-NATIVE.

**Risks.**
- The setters carry the engine's own buffer contract: a free-space check on `[cb+8]` with a flush callback at `[cb+0x18]`
  (render-states.md, summary item 2). An HLE must keep the flush and the cursor the engine expects, or the engine's
  own command buffer breaks.
- Some labels and EOPs are inside the same packet stream. Skipping packets changes the label timing the CP relies on.
- Byte-exact PM4 may still be needed by the side that consumes it (the native announce and the ring-8 scan
  `Ring8ScanSubmit`, liverpool.cpp:234).
- Each setter has to be mapped (about 80), and the mapping has to be proved by decompile.

**Verification.** `=2`: keep the PM4 write and the hook, compare the hook's typed state with the decoded `Regs` at each
draw (the same `CheckPrepared` method), count differences. Gate: zero differences in the attract and Shrine scenes.

### 3.d Higher-level HLE: the passes and draw lists (0x11d7a0 and the pass runners)

**What it is.** Replace the engine's per-pass draw loop (`BPXPR_RecordMaskedDraws` at 0x11d7a0, pixel-duplicates.md line
201; pass runners and target sets in pass-targets.md section 1) with native Vulkan passes.

**Cost today.** Pass emission is cheap (~1 ms per frame, section 1.4). The cost is the binding of each draw, not the
pass structure. A native pass that still binds per draw keeps most of the cost. The gain comes only from fewer
resolutions: with bindless and BDA, per-draw resolution goes away (ARCH-DRAW.md). Then 3,000 draws at 1-2 us would be
3-6 ms per frame, against about 27 ms now.

**Gain and risk.** Potentially the largest long-term gain (up to ~20 ms per frame), but unmeasured, and it requires a
re-implementation of the material and draw-list logic (`.cmat`/`.cslt`, not traced; render-states.md open items).
Every pass would need pixel verification. Not for now.

### 3.e GPU-driven submission: device-generated commands and multi-draw indirect

**What the code does.**
- `SOTC_PASSES` is a barrier decision rule (`video_core/sotc_arch/passes.h`), not device-generated commands.
- `VK_EXT_device_generated_commands` is enabled as a device feature when `SOTC_PASSES` is set and the extension exists
  (vk_instance.cpp:973-981, 1256-1257, 1401-1407). **No call records `vkCmdExecuteGeneratedCommandsEXT`**
  (searched `gpu/shadps4` and `gpu/shim`). So DGC is enabled but unused.
- Multi-draw runs exist for indirect draws, behind `SOTC_DRAW_MDI` (vk_rasterizer.cpp:2640-2712,
  vk_instance.cpp:1187: `multiDraw`).

**Gain.** As in 3.a: 0.3-1.2 ms per frame for merging. DGC would reduce only the emission, which is ~1 ms per frame.

**Risk.** DGC needs the extension on each vendor (NVIDIA, AMD, Intel). Multi-draw needs the same state for every merged
draw; the existing measurement says 3-12% can merge.

**Recommendation.** Do not start DGC. Keep `SOTC_DRAW_COALESCE`. Consider `SOTC_DRAW_MDI=1` only with a measured gain
on the heavy windows.

---

## 4. Recommended path

| Step | Change | Expected gain (ms per frame, W1 basis) | Basis | Verification |
|---|---|---|---|---|
| 0 | Measurement only: (1) hash of each submitted DCB, raw and normalized; count identical frames and IBs. (2) Run the heavy windows with `SOTC_WAIT_NOSPIN=1` for an honest recorder CPU. (3) Profile the CP pipeline lookup against parse with the existing `SOTC_PROFILE` and `tools/profile_functions.py` | 0 (decides steps 3 and 4) | Sections 1.5, 3.b | Counts in the 5 s report. No frame change |
| 1 | ARCH-GCN-NATIVE S2 (buffers by address) and S3 (textures by table), on the existing switches | ~6 (buffer phases 130-150 ms/s down 80% = ~3.8; texture phase 67-106 ms/s down 80% = ~2.3) | D2 section 8.2 divided by 30 FPS, and its success criteria (section 8.4) | `=2` verify with zero differences, the bind phase in the report |
| 2 | ARCH-GCN-NATIVE S5 and S6 (dispatch commit lane, `SOTC_GCN_COMMIT`) | ~5 (target: dispatch cost halved; dispatches are 11.6 ms in W1) | D2 section 8.1 (9.7-10.7 us per dispatch), W1 line 2857 | `SOTC_GCN_COMMIT=2` counts; output identical at fixed frames |
| 3 | Command-replay cache (3.b) for static segments, only if step 0 shows at least 50% identical normalized frames | 0-2 now; up to ~10-20 once steps 1-2 are in and identical frames are common (unknown share) | Section 3.b | `=2` replay compare, frame hashes at fixed frames |
| 4 | Gnmx-layer HLE (3.c), for the setters on the hot path, only if the CP is the limit at 60 FPS | ~1-2 on the CP; game-thread gain unknown | Section 3.c | `=2` typed-state compare |
| - | Not now: passes (3.d), DGC (3.e), SIMD or parallel parse (3.a, ~0 frame gain until the CP limits) | ~0 now | Sections 3.a, 3.d, 3.e | - |

Expected total after steps 1-2: about 11 ms per frame, so the draw thread goes from ~31 ms to ~20 ms. That is still
above 16.7 ms. The rest has to come from the draw-side per-op work that is not yet attacked: `render state` (0.9),
`reset` (0.9), `begin rendering` (1.0), `hle` (1.4) (D2 section 8.2), and the unattributed "other" (4.5-5.3). Those
are the next targets after step 2, and they are small items each. **This is an estimate, not a measurement.** Steps 1
and 2 have their own gates in ARCH-GCN-NATIVE.md, and no step has run in the game yet (its section 10).

---

## 5. Risks, in one place

- Wrong basis: W1 is one window at a 30 FPS cap. The cap hides the ceiling. Re-measure at 60 FPS
  (`sotc_modes.py` patches) before judging the CP limit.
- Stale citations: `ARCH-PARALLEL-DRAW.md` cites `PrepareDraw` at vk_rasterizer.cpp:8454; the current line is 11377.
  `PrepLoop` and `TakePrepared` are at liverpool.cpp:1839 and 1946 (cited as 1716 and 1804). `DRAW-THREAD-7C-DESIGN.md`
  cites liverpool.cpp:2766 and :3846, which are out of date. Update these before anyone uses them.
- The recorder's CPU figure is inflated by spin (1.6). Do not use it as work until measured with `SOTC_WAIT_NOSPIN=1`.
- The memo (b) and HLE (c) both depend on the engine's own buffer and label behaviour, which is partly unknown
  (framedata.md open questions 1-2).
- The GCN-native stages are implemented but not run in the game (ARCH-GCN-NATIVE.md section 10). Their gains here are
  the design's targets, not results.

## 6. Open questions

1. Are whole IBs identical across frames once per-frame addresses are normalized? (Step 0 answers it.)
2. What fraction of the CP's ~6 ms pipeline lookup is the key refresh (`RefreshGraphicsKey`, `RefreshFlatBuf`) and what
   fraction is the flat-buffer copy? A resolved profile of W1 would answer it.
3. Does the game thread come close to its own limit? No artifact measures it. It decides whether 3.c matters for the
   frame time.
4. The shares of W2 and W3 in the profile table (D1 was one window type). Not measured.

## 7. Sources

- Logs: `user/pass1/pass_probe.log` (lines 2844-2864), `docs/re-stutters/native/data/cpucull-perf.log` (lines 1013-1024,
  1303-1313).
- Documents: `docs/DRAW-THREAD-7C-DESIGN.md` (section 0 and the CP profile), `docs/re-stutters/native/ARCH-GCN-NATIVE.md`
  (sections 2, 8, 10), `docs/re-stutters/native/ARCH-PARALLEL-DRAW.md`, `docs/re-stutters/native/ARCH-DRAW.md`,
  `docs/re-stutters/native/framedata.md`, `docs/re-stutters/PC-MEMORY-REWRITE.md` (line 122), `CHANGELOG.md` (lines 283-286,
  303-311), `docs/re-stutters/native/re-pipelines/render-states.md`, `pass-targets.md`, `materials-meshes.md`,
  `slot-stages.md`, `pixel-duplicates.md`.
- Code: `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `liverpool.h`, `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`,
  `vk_recorder.cpp`, `vk_recorder.h`, `vk_instance.cpp`, `gpu/shadps4/core/libraries/videoout/driver.cpp`,
  `gpu/shadps4/video_core/sotc_arch/passes.h`, `sotc.ini`.
- Companion: `docs/re-stutters/native/arch-next/engine-gnm-layer.md` (agent C2) did not exist when this was written. Its
  setter map should be checked against section 3.c.

## Results of the CPU-budget plan, C0-C1 (5 Nov 2026, branch arch/nonblocking-frame)

New 5 s line (with `SOTC_FPS`): `SOTC CPU cores (5 s): process X of N hardware threads; cp .. draw .. rec .. ...; rest
(pipeline workers, game, driver)` (`driver.cpp`, `GetProcessTimes` and the `SotcStats` CPU roles).

Warm Lab route, same port cache (salt 73), P1 on, one round each (`user/c1/*.log`):

| config | process cores (two windows) | recorder cores | route p50 / p99 / max |
|---|---|---|---|
| `SOTC_PIPE_PREDICT=1` (the user's sotc.ini until now) | 10.62, 10.65 | 0.99 | 50.6 / 125.7 / 143.7 ms |
| base (prediction off) | 5.21, 5.49 | 0.99 | 33.4 / 66.7 / 76.1 ms |
| `SOTC_REC_SPIN_US=0` | 5.88, 5.34 | 0.99 | 33.3 / 58.9 / 66.9 ms |
| `SOTC_FUTEX_WAIT=1` | 5.36, 4.86 | 0.53, 0.48 | 33.4 / 66.4 / 74.7 ms |
| `SOTC_CP_IDLE_US=50` | 5.54, 5.08 | 0.99 | 33.4 / 50.4 / 66.4 ms |
| `SOTC_WAIT_NOSPIN=1` | 5.40, 4.93 | 0.55, 0.51 | 33.4 / 66.7 / 92.7 ms |
| `SOTC_FPS_LEAN=1` | 6.00, 5.42 | 0.99 | 33.4 / 58.2 / 92.1 ms |
| `SOTC_NB_WORKER_CAP=1` | 5.84, 5.02 | 0.99 | 33.4 / 58.3 / 83.9 ms |

The ~99% CPU the user saw was the material prediction left on in sotc.ini: it doubles the process's CPU (10.6 of 16
threads) and halves the frame rate. With it off the process uses about 5 of 16 threads warm; sotc.ini now has
`SOTC_PIPE_PREDICT=0`. Of the spin switches, the futex and no-spin waits halve the recorder's CPU (its spin), about
half a core, with no change beyond one round's noise in the route; `SOTC_REC_SPIN_US=0` did not change the recorder.
The worker cap has nothing to cap warm (workers idle); it needs the cold run.

### C1.3 worker cap, cold, and C2 (`user/c2/*.log`, one round each)

| config | process cores | route p50 / p99 / max | boot preparation | CP blocked on draw |
|---|---|---|---|---|
| cold, no cap (salt 87) | 11.65, 7.89, 8.34 | 33.4 / 143.2 / 375.9 ms | 64.2 s | 8.06 ms/frame |
| cold, `SOTC_NB_WORKER_CAP=1` (salt 88) | 8.47, 9.01 | 33.5 / 93.2 / 102.3 ms | 75.7 s | 6.10 |
| warm, `SOTC_CP_PRECISE_WAIT=2` | 6.01, 5.41 | 33.4 / 58.7 / 100.5 ms | - | 7.28 (avoided 240/s, verified 240/s, violations 0) |
| warm, `SOTC_CP_PRECISE_WAIT=1` | 5.80, 5.65 | 33.4 / 67.3 / 84.7 ms | - | 4.15 (avoided 1,887/s, violations 0 in =2) |
| warm, `SOTC_CP_LABEL_SKIP=1` | 5.80, 5.30 | 33.3 / 58.1 / 59.2 ms | - | 19.25 (4,515 waits/s) |

- Worker cap: the cold route improved in this round but the boot preparation took 11 s longer (the pretranslation
  queued before the hold starts is capped; earlier cold holds ranged 59.5-78.8 s, so not conclusive). Off.
- Precise wait: 0 counted violations in verify mode, and in mode 1 the CP's blocked time falls from ~7 to 4.15 ms per
  frame. Rejected anyway: the user saw the game's depth broken with `SOTC_CP_PRECISE_WAIT=2` (5 Nov). Verify mode
  uses the lookup made without the full wait and only compares a digest (`CpDigest`), so a read outside the checked
  set (`CpCollectReads`: stage code, fetch shader, sharp tables) goes uncounted; the visible fault says such reads
  exist. Not used as a base for C3.
- Label skip: the CP waits ~15x more often for the draw thread (19.25 ms/frame blocked). Rejected.

### C3 matrix (warm Lab route, salt 73, P1 on, without precise wait; `user/c3/c3b_*.log`, one round each)

| switch | route p99 / max, frames > 50 ms | draw cpu / cp cpu / bind (ms per frame, two windows) | process cores | verify |
|---|---|---|---|---|
| none (base) | 58.6 / 83.4, 8 | 34.8 / 33.7 / 18.2; 34.7 / 32.2 / 18.3 | 5.92, 5.70 | - |
| `SOTC_DEDUP_PUSH=1` | 59.0 / 75.2, 9 | 38.6 / 36.3 / 20.1; 31.4 / 29.9 / 16.5 | 5.85, 5.55 | - |
| `SOTC_SYNC_FAST=1` | 58.4 / 75.4, 8 | 37.2 / 35.0 / 19.7; 33.1 / 31.2 / 17.6 | 5.80, 5.55 | - |
| `SOTC_FASTBIND_WIDE=1` | 76.4 / 100.8, 13 | 37.7 / 36.0 / 19.7; 35.3 / 32.7 / 18.8 | 5.75, 5.67 | - |
| `SOTC_PIPE_FAST=1` | 84.2 / 127.2, 27 | 42.9 / 40.0 / 22.9; 30.5 / 29.2 / 16.0 | 5.78, 5.39 | graphics hits 35%, compute 0% |
| `SOTC_SPEC_MEMO=2` | 58.7 / 67.1, 9 | 36.3 / 34.5 / 18.8; 33.0 / 31.5 / 17.5 | 5.69, 5.48 | hits 100%, mismatches 0 |
| `SOTC_SPEC_MEMO=1` | 50.1 / 58.3, 3 | 34.5 / 33.0 / 18.1; 30.4 / 29.1 / 15.8 | 5.86, 5.27 | hits 100% |
| `SOTC_PARALLEL_RECORD=1` | 50.1 / 58.3, 3 | 35.6 / 33.4 / 18.4; 32.0 / 30.4 / 16.7 | 6.78, 6.14 | - |

At the 30 FPS cap no switch moves the per-frame thread times beyond the spread between two windows of the same run
(draw cpu 30-39 ms in every row). `SOTC_PIPE_FAST` and `SOTC_FASTBIND_WIDE` made the route worse; `SOTC_SPEC_MEMO=1`
and `SOTC_PARALLEL_RECORD=1` gave the best route numbers of the round (the latter costs ~0.8 core more). After the
precise-wait finding (0 counted violations, broken depth on screen), verify counters are not enough: anything kept
needs the user's visual check. Not yet run: the uncapped Shrine scene, where a gain in per-frame cost shows as FPS.
