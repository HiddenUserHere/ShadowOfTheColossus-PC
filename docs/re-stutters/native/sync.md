# N0 sync inventory: every CPU wait on the GPU (revision 2)

Specialist: sync (event flags, equeues, flip, submit gate, labels, readback waits).
Source of truth: Ghidra decompilation in `out/re/decomp/` (main tree), eboot xrefs through `tools/re/eboot_re.py`
(`callers`, `refs`), port code in the main tree (`gpu/shadps4/...`), trace and log evidence from
`user/trace/20261101-085933` and `user/pcmem/*/logs/*.log` (175 files; totals are cross-run sums).
Addresses are eboot virtual addresses. No engine code edited. Nothing committed.

## 0. What changed since revision 1

- Rev 1 said R3 (0x85d690 / 0x85d6f0) was the compute drain. Corrected: those two functions have no static caller and
  no data reference in the eboot (checked: direct calls, RIP-relative refs, 32-bit and 64-bit absolute pointers in
  every PT_LOAD segment; no hit). They are dormant in this build or reached by a computed table. The real compute
  CPU spin is the ring-full retry in the gnmx submit path (section 3.2).
- Rev 1 said "no render-code loop spins on a GPU-written label" (from `reports/02-gpu-cpu-sync.md`). Wrong for the
  XPR cull and the frame-slot fence: five functions spin on GPU-written labels (section 2).
- sceGnmAreSubmitsAllowed has exactly one direct caller (wrapper FUN_00882cf0 via FUN_000c5560, the render submit
  job): it is a job-level predicate, not a spin (section 4).

## 1. Summary

| class | count | sites |
|---|---|---|
| KEEP (pacing) | 4 | P1 RenderFlip equeue wait, P2 flip loop, P3 init flip spin, P4 frame boundary |
| REPLACE by host frame ticket | 4 | R1 DingDong gate, R2 CB submit gate, F1 frame-slot fence (0x90ea0), F2 two-label fence (0x91090) |
| REPLACE by host ring accounting | 3 | R3 ring-full retry loops (0x863af0, 0x863310, 0x8636e0) |
| REMOVE with its consumer (XPR mask, GPU result read) | 4 | X1 0x11dbe0, X2 0x11d7a0, X3 0xceae0, X4 0x11e190 |
| REMOVE with its consumer (guest readback) | 1 | C1 buffer_cache ReadMemory waits |
| REMOVE with its consumer (port CP label spin) | 1 | C2 liverpool WAIT_REG_MEM spin |
| GPU-internal (CP waits, CPU does not block) | 2 | G1 WaitOnAddress, G2 InsertWaitFlipDone |
| Not GPU (CPU job, list, mutex spins; event flags) | many | section 5 |

Measured cost (rev 1, unchanged): readback waits 10104 ms over 11758 waits; port submit gate 689 ms over 15391
calls; RenderFlip equeue waits 242 ms in 6 waits in a 58 ms frame (pacing).

## 2. Game-side CPU polls of GPU-written labels (question 1)

Method: (a) label writers = every caller of the EOP / RELEASE_MEM / WRITE_DATA builders (`FUN_0086eb60` EOP with
event 0x28 or 4, `FUN_0086ec50 / ed40 / edc0 / eec0 / f530 / f5e0` EOP and EOS variants, the `FUN_00884580..884b70`
RELEASE_MEM family; packet headers 0xc0044700, 0xc0054902, 0xc0034800, 0xc0003700, 0xc0053c00 found in 45 decomp
files); (b) spins = multi-line `do { } while (...)` with an empty body across the whole decomp, plus the explicit
yield / usleep spins; (c) each spin's pointer traced back to a writer.

### 2.1 XPR visibility cull labels (GPU result, consumed by the CPU)
- Writer: `FUN_000925c0` (0x925c0: `FUN_0086eb60(ctx, 0x28 EOP, data 1)`), called by the three cull submitters:
  `FUN_000c6bd0` (shadow cull, label `obj[0x1d]+0x40`), `FUN_000c6f70` (main scene cull
  `XPRRunFragmentCullShaderMainScene`, label `obj[0x1d]+0x68`), `FUN_000c5200` (label `obj[0x1d]+0x78`).
  The labels are reset to 0 by `XPRPreRender` (0xca6d0, per the probe notes in `gpu/sotc/probe_xpr.cpp`).
- Spins (value must become 1; the GPU sets it with the EOP above). All four have the same shape:
  `if (*(char*)(obj[9]+0x16) == 0) do {} while (*label != 1); else FUN_00035590(DAT_02ea4718, label, 1, 0, 1);`
  - X1 `FUN_0011dbe0` @ 0x11dbe0 (IndexCullXPRObjects, decomp lines 72-74): label `*(obj[7]+0xab70+idx*0x50)`, then
    reads the mask.
  - X2 `FUN_0011d7a0` @ 0x11d7a0 (1066 bytes, lines 36-38 and 108-110; 6 callers: FUN_000c1860, 000ce850, 000cd330,
    000c1ce0, 000c1fc0, 000c46c0): label `*(obj[7]+0xa800+idx*0x28)`.
  - X3 `sub_ceae0` @ 0xceae0 (decomp lines 77-79): label `*(obj[7]+0xab70+idx*0x50)`.
  - X4 `FUN_0011e190` @ 0x11e190 (RenderXPRDecals, 294 bytes, lines 17-19, caller FUN_00181420): label `obj[0x1d]+0x78`,
    then `FUN_0019c740` per entry (the draw side of the mask).
- Mask consumers the labels release: `FUN_001a6d20` (0x1a6d20, XPR_CONSUME_MASK, reads the bitset the GPU wrote),
  `FUN_001a7290` (indirect). The `obj[7]+0xab70` / `+0xa800` arrays are inferred to hold the `+0x40 / +0x68 / +0x78`
  cull labels (the writers take struct fields; the readers go through arrays of pointers). Not proven for X1-X3;
  one SOTC_HOOK_XPR_PROBE session matches consumers to labels and settles it.
- Class: GPU result read. REMOVE with the consumer (plan rule 1: disable the occlusion round trip; treat all as
  visible, or keep the culling on the GPU with indirect draws). Patch: byte-patch each spin to skip, and both branches
  (the `obj[9]+0x16` choice selects the job-wait variant). The mask consumer 0x1a6d20 then reads nothing from the
  GPU. If a one-frame-old mask is kept instead, the port supplies the bitset of the last completed frame with no wait.

### 2.2 Frame-slot fence (GPU frame completion, double buffered)
- Writer: `FUN_00090e30` @ 0x90e30 (caller FUN_00118470): `FUN_0086eb60(cmd, event 4 (bottom of pipe), label =
  *(obj+0x770+slot*8), data = the 32-bit frame counter obj[0x10], 0x38, 0)`, then stores the counter in the CPU copy
  `obj+0x780+slot*8`.
- F1 `FUN_00090ea0` @ 0x90ea0 (RenderBackend frame, caller FUN_000cefa0, 351 bytes; decomp lines 28-30): slot is 0/1,
  alternating; if `*label(slot) != obj[0x780+slot]`, spin until equal (no yield, no sleep). The following
  `FUN_0008ef20` (deferred-free flush) waits on the job counter, not on a GPU label.
- F2 `FUN_00091090` @ 0x91090 (47 bytes, caller FUN_000961a0; lines 6-9): two empty spins, `**(obj+0x770) ==
  obj[0x780]` and `**(obj+0x778) == obj[0x788]`. The second label (0x778 / 0x788) has no writer in the decomp that I
  could find (open question 2).
- Class: resource reuse of the per-frame slot, which is GPU frame pacing at two frames of lag. REPLACE (section 6.2).

### 2.3 Other CPU spins checked, not GPU
- `0x38b40` (the only other explicit yield spin): job helper, CPU-only (not analysed further).
- `FUN_00035590` (JobSystem::WaitForCounter, spin then usleep) is a generic `*ptr == value` wait. 226 decomp files call
  it or usleep. Callers that pass a GPU label are GPU waits; the XPR sites above are the ones confirmed. A pointer audit
  of all 226 callers is open (question 1).
- `*(byte*)(node+0x19) != 0` spins (0x217ea0, 0xa68760, 0xa56d80, 0xa52b80, 0xac6cb0, 0xac6a60, 0xac67c0, 0xac6570):
  CPU list / node hand-off between threads. No GPU label on the path shown by the decomp. Not GPU.
- `*(int*)(obj+8) != 0` (0xb30bd0), `*piVar1 != 0` (0xc3ebb0, 0xc3eb70, 0xc3eb20): mutex / refcount hand-off. Not GPU.
- `while (-1 < x)` loops (0x130ee0, 0x12fd00): bit scans, not waits.

## 3. Compute and command paths

### 3.1 Submit gate (port code, not game code)
- R1 `sceGnmDingDong` (gnmdriver.cpp:537): `GateWait(1)`. With the shipped default `SOTC_CP_AHEAD=0` this is
  `WaitGpuIdle()`: it waits for `submission_lock == 0`, which `sceGnmSubmitDone` sets when the GPU was busy and the
  GpuIdle IRQ clears. Game callers of the doorbell: `FUN_0085d570` (129 bytes), `FUN_0085d380` (73 bytes, the one the
  gnmx submit uses), and the dormant `FUN_0085d690` / `FUN_0085d6f0`.
- R2 `WaitSubmitGate` (gnmdriver.cpp:232, `GateWait(0)`): the command-buffer submit. The same `WaitGpuIdle` at default.
- P4 `sceGnmSubmitDone` (gnmdriver.cpp:2607, `GateWait(2)`): the frame boundary; `PushFrameMark` in mode 2.

### 3.2 Ring-full retry (the real compute CPU spin) -> R3
- `FUN_0085d380` returns `0x80000001` (-0x7fffffff) when the compute ring has no room. It calls `FUN_0085d3d0`, which
  compares the ring write pointer with `*param_2`, the ring read pointer that the command processor writes: a GPU
  result read. The gnmx submit then spins with no pause and no sleep until room appears:
  - `FUN_00863af0` @ 0x863af0, lines 148-158 (`lwgfxcontext.cpp:0x2c8 internalSubmitAndFlip`, message
    "ComputeQueue for dispatch draw is full; waiting for space...").
  - `FUN_00863310` @ 0x863310, lines 116-126 (`lwgfxcontext.cpp:0x1ff submit`).
  - `FUN_008636e0` @ 0x8636e0, lines 117-127 (`lwgfxcontext.cpp:0x25b submit`).
- Port state: `liverpool.cpp:5235` `SubmitAsc`: with `SOTC_ASC_RPTR=2` the ring dwords are copied at the doorbell and the
  guest read pointer is handed back at once. `AscRptrMode()` defaults to 0 (liverpool.cpp:5286), so the shipped build
  still lets the guest see a full ring.
- Class: resource reuse (ring slot). REPLACE by host ring accounting: make `SOTC_ASC_RPTR=2` the only behaviour (the
  port needs it anyway). `FUN_0085d3d0` then never reports full and the three retry loops never run. Belt and braces:
  byte-patch the three `do/while` loops to skip. Verify: the "ComputeQueue ... full" message must not appear in a log.
- R3b `FUN_0085d690` / `FUN_0085d6f0` (sentinel spin `**(int**)(obj+0x10) != 0` plus `scePthreadYield`): no static
  caller (section 0). If ever reached, the same fix applies: with mode 2 the ring is free at the doorbell.

### 3.3 Render submit job
- `FUN_000c5560` (RenderThread::SubmitJob, 1128 bytes) is the render submit job. Callers: RenderCullXPR,
  BPRender_DepthAndGBufferPasses, RenderView::MainCamera, EndFrameViewParams, UpdateSkinningRenderObjects.

## 4. sceGnmAreSubmitsAllowed (question 4)

- Direct callers (eboot_re `callers`): only `FUN_00882cf0`, the 16-byte wrapper. Its only caller is `FUN_000c5560` at
  0xc5913, which stores the wrapper as the invoker (+0x18) of a local functor (`local_90`, `pcStack_78 = FUN_00882cf0`).
  The functor goes to the job system (`FUN_00035480`, job create / kick) and is invoked through `sub_d1b10` (0xd1b10:
  `(*(code**)(p+0x18))()`). So the check is a job-level readiness predicate: the render submit job runs when
  `submission_lock == 0`.
- Indirect references: none found. The eboot has no absolute or RIP-relative pointer to 0x882cf0 besides the import
  stub; the predicate is reached only through the closure above.
- Class: a job-level wait on the gate, not a game spin. Port: `gnmdriver.cpp:398` returns `submission_lock == 0`.
  REPLACE together with R1 / R2: the predicate becomes `FramesRecorded() >= FramesMarked() - 1` (section 6.2).

## 5. Event flags and job waits (question 3)

- "Game Render" waits: `EventFlag_WaitInfinite` from the render-thread gates (rev 1: `sub_bd6d0` family, timed wrappers
  `sub_bcf90 / bd010 / bd0a0 / bd110 / bdf20` around `FUN_0003b4a0` / `FUN_0003b4e0`). Their setter is `FUN_0003b450`
  (13 bytes; callers include `FUN_00534690`, `FUN_00147160`, `FUN_00a52b80`, `FUN_00146770`, `FUN_0003b8c0`,
  `FUN_00038550`, `FUN_00a59bd0`, `FUN_00147490`). The render gate is set by the simulation side (`FUN_00534690`),
  after its CPU work. The render job reaches the GPU only through the submit gates (3.1) and `sceGnmSubmitDone`. So
  these waits block on the GPU only transitively, through R1 / R2 / P4.
- "Resource Loading" waits: `FUN_00998630` (0x998630, 271 bytes) and `FUN_00993220` (0x993220), flag at
  `DAT_03295290+0x98`. Setters: `FUN_00992b00`, `FUN_00992ef0`, `FUN_00992fd0` (called by `FUN_0098fd10`) and
  `FUN_0098dd60` (called by `FUN_00991c70`). File-streaming and job-queue producers; no GPU output on their path.
- "SoundSystemSndx": `FUN_00949f80` (audio); setters `FUN_00949fe0`, `FUN_009536d0` (callers `FUN_009ac390`,
  `FUN_00997700`, `FUN_00997970`). Audio only.
- Job worker flags (`obj+0xc0`): `FUN_009b2ac0` (wait), `FUN_009b2e90` (setter, no direct caller), `FUN_009b2fb0`
  (setter, callers `FUN_009b2f80`, `FUN_009b3540`).
- Not found: a producer that waits on GPU output and then sets one of these flags. Method: the setters are the only
  `sceKernelSetEventFlag` callers in the twelve decomp files that contain one; none of them calls a submit or a label
  builder.

## 6. Replacement design

### 6.1 Frame ticket API (what exists, what the port signals, when)
- `Liverpool::PushFrameMark()` (liverpool.cpp:5299), called by `sceGnmSubmitDone` in mode 2 (frame N = the Nth
  SubmitDone). Sets `frames_marked = N` and records the per-queue enqueue counts at that instant.
- `Liverpool::ServiceFrameMarks()` (liverpool.cpp:5322), command processor thread: publishes the mark once every queue
  has completed the tasks counted in it.
- `frames_recorded` (liverpool.cpp:5352): stored by the draw thread when it reaches the ticket op, after it ran every op
  of the frame and read their guest memory.
- API: `Liverpool::FramesMarked()`, `FramesRecorded()` (liverpool.h:163-172), `Liverpool::WaitFramesRecorded(target)`
  (liverpool.cpp:5362; blocks on `frames_cv`).

### 6.2 Replacements
- R1 + R2 + P4 + section 4 predicate: one gate on every submit path:
  `Liverpool::WaitFramesRecorded(FramesMarked() - 1)` before the submit (lag 1), which is the mode 2 gate with
  `SOTC_FRAME_AHEAD` semantics. Dependency: `SotcLabels::Honest()` must be true (already required by
  `FrameAheadActive`). Remove `GateWait(0)` and `GateWait(1)` and the `WaitGpuIdle` / `submission_lock` path
  (gnmdriver.cpp:97-131, 232-245, 282, 398-401, 537, 2614-2620). Signal: `frames_recorded`, nothing else.
- F1 `FUN_00090ea0` slot fence: replace the spin at decomp lines 28-30 with a host call
  `sotc_native_wait_frame(counter)`, where `counter` is the value the EOP at 0x90e30 writes (`obj+0x780+slot*8`). The
  host maps that counter to the frame number and calls `Liverpool::WaitFramesRecorded(frame)`; the port already sees
  the EOP data at the label write. Keep the CPU copy store at 0x90e30. Signal: `frames_recorded`.
- F2 `FUN_00091090` (second label 0x778 / 0x788): the same API once its writer is known (open question 2).
- R3 ring-full: `SOTC_ASC_RPTR=2` made the only behaviour (`liverpool.cpp:5235`, default at 5286). Signal: the host copy
  at the doorbell (`SubmitAsc`, the read pointer update near liverpool.cpp:5065 / 5259). No wait is replaced: the spin
  never starts. Belt: byte-patch the three `do/while` loops (0x863af0 lines 156-158, 0x863310 lines 124-126,
  0x8636e0 lines 125-127).
- X1-X4 XPR: REMOVE with the consumer (section 2.1). No replacement wait. Patch points: the four spins (0x11dbe0 lines
  73-74, 0x11d7a0 lines 37-38 and 109-110, 0xceae0 lines 78-79, 0x11e190 lines 18-19) plus their job-wait `else`
  branches, and the mask consumers 0x1a6d20 and 0x1a7290 (XPR specialist).
- C1 readbacks and C2 CP label spin: REMOVE with their consumers (rev 1, section 3). A readback that must stay waits on
  the ticket of the frame that wrote the bytes (`WaitFramesRecorded(mark)`), never on a GPU idle.

### 6.3 Checks before a wait is deleted
- No CPU read of GPU-written memory remains behind a removed wait (the N3 rule).
- `frames_recorded` is published after the draw thread ran every op of the frame, so a wait on it covers resource reuse
  for anything the frame's ops read. Guest writes after the frame are the risk noted at gnmdriver.cpp:117-127; the XPR
  removal (X) takes away the game's label fences for those, so each removal needs the visual check.

## 7. Patch summary (for N2-N4)

| id | site | class | action | signal / API |
|---|---|---|---|---|
| P1 | 0x960a0 equeue loop | pacing | keep | flip event |
| P2 | 0x956e0 flip loop | pacing | keep | vblank |
| P3 | 0x95760 decomp lines 52-56 | pacing (init only) | keep; optionally vblank wait | vblank |
| P4 | sceGnmSubmitDone | frame boundary | keep, as ticket | PushFrameMark |
| R1 | sceGnmDingDong GateWait(1) | REPLACE | ticket gate, lag 1 | frames_recorded |
| R2 | WaitSubmitGate GateWait(0) | REPLACE | ticket gate, lag 1 | frames_recorded |
| F1 | 0x90ea0 lines 28-30 | REPLACE | host wait on slot frame | frames_recorded |
| F2 | 0x91090 lines 6-9 | REPLACE | host wait (writer open) | frames_recorded |
| R3 | 0x863af0:156-158, 0x863310:124-126, 0x8636e0:125-127 | REPLACE | SOTC_ASC_RPTR=2 only | host copy at doorbell |
| R3b | 0x85d690 / 0x85d6f0 spin | dormant | drop the spin if reached | host copy at doorbell |
| SP | predicate sceGnmAreSubmitsAllowed (FUN_00882cf0) | REPLACE | predicate = ticket | frames_recorded |
| X1-X4 | 0x11dbe0, 0x11d7a0, 0xceae0, 0x11e190 | REMOVE with consumer | skip the spins, no mask | none |
| C1 | buffer_cache.cpp:1262-1300 | REMOVE with consumer | delete with the readbacks | none |
| C2 | liverpool.cpp:4420-4440, 4898-4950 | REMOVE with consumer | delete with the labels | none |
| G1, G2 | 0x884d40, 0x86f700 | GPU-internal | keep | CP |

## 8. Open questions
1. Pointer audit of the 226 decomp callers of `FUN_00035590` / usleep: which pass a GPU-written label (only the XPR
   sites are confirmed). Method: for each caller, check whether the pointer is a field written by a section 2 writer.
   Verify X1-X4 labels with one SOTC_HOOK_XPR_PROBE session (already wired in `scripts/hooks.d/xpr.py`).
2. Writer of the second frame label (`obj+0x778`, F2) and its consumer set. Start from `FUN_000961a0`.
3. Whether R3b (0x85d690 / 0x85d6f0) is reached through a computed table. A runtime counter in a verify session
   answers it (no static pointer exists).
4. The frame-number mapping for F1: the EOP data (32-bit counter) and the port's `frames_marked` count the same frames
   only if each SubmitDone pairs with one counter increment in the game. Check in one trace.
5. The GPU share of C1 remains an estimate (rev 1 open question 5).
