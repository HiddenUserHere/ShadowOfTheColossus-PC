# Handoff: native render defaults and native command processor (branch `native/render-vk13`)

Repo: `C:\Users\ptr\Documents\sotc-native` (Windows, Git Bash, MSYS2 CLANG64).
Scope: native GNM boundary, native command processor (op-list executor), virtual labels, event-driven
sleeping, pass recorder. Follows `HANDOFF-N2-48.md` (render backend cutover). The evidence is in
`docs/engine/native-render-gaps.md` rows N2-94 to N2-108. The N2-109 change (default flip) is
row N2-109. This file does not repeat the evidence.

---

## 1. Hard rules

- **No game run without the user's explicit authorization for that run.** A "go" for one run is not
  standing authorization. When authorized, launch only with `skipSplash=1 nointro` (the run scripts
  export it).
- **No subagent runs the game** (`bb-probe.exe`, `sotc.py`, bench or play scripts). Runtime checks
  are the main session's step.
- Never put game files or Sony modules (`.sprx`, `.prx`, `libSceNgs2`, `libSceUlt`) in the repo.
  Packaging refuses them.
- Ghidra hex constants can mislead on PM4 packet headers. Check them against the raw bytes of
  `out/eboot.elf` (file offset = VA + 0x4000) before relying on them.
- Use `rg` or the Grep tool, never `grep -r`. Read ranges with Read `offset`/`limit`, not `sed`.
- Code comments and engine docs in English. User-facing docs (README, CHANGELOG, `docs/sotc/*`) in
  Portuguese.
- Gap table check: `python out/check_pipes.py` must print `bad 0`. Every N2 row has exactly 7 pipes.

## 2. Defaults and switches

**Stated default (code):** the native render is the intended default (N2-109).

| setting | code default | how to turn it off |
| --- | --- | --- |
| `SOTC_RENDER` | unset = `native-pass,native-gnm` (`gpu/sotc/hooks.cpp`, `RenderJobHooks`) | `SOTC_RENDER=off` (or any unknown stage name) installs nothing: emulated path |
| `SOTC_NATIVE_GNM` | the mask parser treats unset as all classes (`ParseMask`, `gpu/native/gnm/gnm_common.cpp:28-34`) | set it to a class list (for example `cb,db`); an empty value turns every class off |
| `SOTC_NATIVE_GNM_SKIP` | unset = no skip | comma list of hex guest VAs whose rows are never installed |

Both the install path (`gnm_class_on`, `gpu/sotc/hooks.cpp`) and the mask parser (`ParseMask`) treat an
unset `SOTC_NATIVE_GNM` as every class (fixed in N2-109), so all 171 hooks with `ib` are on by default.

The seven validated switches are on by default. `"0"` turns one off. Defined in
`gpu/native/native_switches.h`:

| switch | what it does | wave |
| --- | --- | --- |
| `SOTC_REC_BLOCK` | the closure recorder (GpuRecorder) waits on its condition variable without a timeout | W1 |
| `SOTC_BACK_BLOCK` | the draw thread (BackLoop) waits untimed; every producer notifies | W1 |
| `SOTC_CP_EVENTS` | the command processor sleeps on the progress epoch (`Liverpool::NotifyProgress`), 1 ms cap | W1 |
| `SOTC_NATIVE_VLABELS` | ON by default again since N2-115 (`=0` turns it off): a wait whose label has a pending write parks without re-polling until the real write (N2-112); it never passes early | W2 |
| `SOTC_NATIVE_CP` | all-native submissions run on the op-list executor (`liverpool_native_cp.cpp`) | W3 |
| `SOTC_BACK_BATCH` | consecutive draw-thread ops become one back-ring entry | W4 |
| `SOTC_PASS_RECORDER` | GpuDraw records real command buffers per segment; `shadPS4:GpuSubmit` calls QueueSubmit | W5 |

Off, and not a default candidate:

- `SOTC_SUBMIT_IDLE=cp`: releases SubmitDone's lock when the command processor drains (N2-105).
  Run 75 (N2-106): 94.4 Mcycles/flip on the command processor against 24.4 in run 74. **Rejected.**
  Only the value starting `cp` enables it (`native_switches.h`, the check is on `v[0]`/`v[1]`).
- `-DSOTC_UF_SBO=1` (build flag): small-buffer `UniqueFunction` (N2-98). Off, so the pointer-sized
  layout stays (`gpu/shadps4/common/unique_function.h`).
- `SOTC_FPS` (presence check, not `=1`): `std::getenv("SOTC_FPS") != nullptr`
  (`gpu/shadps4/video_core/sotc_stats.h:29`). Setting it turns on the wait and oplist counters. The
  counters are also on with `SOTC_NATIVE_CP` (`NativeWaitCounted`, `liverpool.cpp:160-162`, returns
  `NativeCp() || kFpsLog`). Needed for a wait baseline (N2-100).

Other env vars used in the runs: `SOTC_THREAD_TIMES=1` (per-thread CPU time over the profile window,
`src/probe.c`, with `SOTC_PROFILE_DELAY` default 20 s and `SOTC_PROFILE_SECONDS` default 15 s),
`SOTC_PROFILE=<thread-name part>` with `SOTC_PROFILE_OUT=<file>` (sampler in `src/probe.c`).

## 3. Architecture

**Native GNM boundary (N2-75 to N2-87).** Each `sceGnm*` / `sceGnmDriver` entry in a native class runs
natively. The emitting site writes a native op into the guest PM4 stream as a `SotcNativeOp` NOP
(`gpu/native/native_stream_ops.{h,cpp}`, `EmitIb` at line 263). The command processor's NOP handler runs
it on the draw side. Runs 62 to 75 show PM4 0 on both rings: no guest PM4 packet is parsed in an
all-native run.

**Op-list command processor (N2-89, N2-97, N2-104).** `BuildOpList` scans a buffer and builds the op
list. `RunNativeEntry` runs it without coroutines: CP ops inline, runs of draw-thread ops through
`NativeBackRunIds`, waits (park and resume), WaitCe with a CE cursor, nested IB frames, prefetch over
the shadows. Compute slices split mid-packet carry their tail to the PM4 path through a per-vqid carry.
Running ops at `sceGnmSubmit*` time on the guest thread was rejected (N2-89): it overtakes queued
submissions.

**Virtual labels and the same-submission rule (N2-96, N2-103).** `native_labels.cpp` records pending
label writes (EOP/EOS, WRITE_DATA, ExecReleaseMem, FlipTail). They retire on the draw thread after the
real write. A memory wait asks `Satisfies` first. The game CPU zeroes and reuses label slots at
command-buffer build time, so a pending write from an earlier submission may belong to an older use of
the slot. Matches are therefore restricted to **the same submission** (`Labels::SetSubmission`, set
before every resume). Cross-buffer waits keep polling, woken by events. `lbl_boundary` (entries still
pending at sceGnmSubmitDone) is ~450/s in run 72: the SubmitDone barrier does not retire each frame's
label writes before the next frame. The same-submission rule is what keeps this safe.

**Event-driven sleeping (N2-95, N2-101, N2-102).** `Liverpool::NotifyProgress` (`liverpool.h:479`)
bumps an epoch from WriteFenceValue, ExecReleaseMem, the draw thread after ops, the WRITE_DATA and
EOP/EOS writes, the flip tail, `SignalVoLabel` and the scheduler callbacks. It is seq_cst and paired
with the sleeper. The per-op notify was removed after run 69 (it woke the command processor
constantly).

**Pass recorder (N2-99, N2-104).** Under `SOTC_PASS_RECORDER` the draw scheduler is built without the
closure recorder (`vk_presenter`). It records one command buffer per segment; the `shadPS4:GpuSubmit`
thread (`vk_scheduler_segments.cpp`) takes `SegmentBatch`es on an untimed condition variable and calls
QueueSubmit. Segment cuts: render-target change, compute after rendering, FlushPeriodic past 256 draws.
**Risk (N2-99):** PopPendingOperations runs before the GPU submit in pass mode. There is no
`Scheduler::Sync`.

## 4. Validation runs and numbers

Runs 62 to 75 were the main session's, at the 30 FPS capped scene, 40 s, `skipSplash=1 nointro`. The
numbers below are copied from the gap rows, not re-checked against the logs in this pass. The logs
exist: `out/re/cutover/probe-gnm-NN.log` (for example `probe-gnm-70.log` to `probe-gnm-75.log`).

| run | config | result |
| --- | --- | --- |
| 62 | every class + `ib` (171 hooks), N2-84..86 | PM4 0 both rings, no fault, 30 FPS (N2-87) |
| 63 | every class + `ib`, N2-88/89, `SOTC_PROFILE=GpuCommandProcessor` | every buffer all-native, emit 172676, missing 0 (N2-90) |
| 64 / ref 65 | all native / `SOTC_RENDER=off`, `SOTC_THREAD_TIMES=1` | native PM4 0 vs emulated `pm4_gfx=191204` (N2-91) |
| 66 | all native, `SOTC_PROFILE=GpuRecorder` | GpuRecorder full core is the designed idle spin (N2-92/93) |
| 67 | every class, every N2-94 switch off | regression clean (N2-100) |
| 68 | switches off + `SOTC_FPS=1` | wait baseline: 6156 waits/s, 592863 polls/s, 636 ms blocked/s; GpuRecorder 112.4 Mcycles/flip (N2-101) |
| 69 | + REC_BLOCK, BACK_BLOCK, CP_EVENTS (before the per-op notify fix) | GpuRecorder 112.4 to 15.2; CP polls rose to 867k/s; fixed (N2-101) |
| 70 | the three above, with the fix | polls ~200-224k/s (-64%); GpuRecorder 112.4 to 13.8 (N2-102) |
| 71 | + NATIVE_VLABELS | 72% of waits pass a same-submission pending label; polls ~50k/s (N2-103) |
| 72 | + NATIVE_CP | oplists ~960/s, fallback 0; blocked ~305 ms/s; CP 21.8 vs 18.8 Mcycles/flip (N2-103) |
| 73 | + BACK_BATCH | CP 19.0, GpuDraw 22.1, recorder 13.5 Mcycles/flip (N2-104) |
| 74 | + PASS_RECORDER | closure recorder gone; GpuSubmit 1.7 Mcycles/flip; GPU threads 53.0 vs 54.6 (N2-104) |
| 75 | all 8 N2-94 switches incl. SUBMIT_IDLE=cp | no fault, 30 FPS, oplists 961/0; image OK at 20 s and 35 s; SUBMIT_IDLE=cp harmful (N2-106) |

**N2-107 and N2-108 (attract demo stop and fix).** The attract demo benchmark (fps60, no save template)
stopped at ~28 s in both the all-off run A and the 7-switch run B with
`liverpool.cpp:2681 ProcessCompute: Unreachable code! Invalid PM4 type 0`. The bisect (50 s runs)
found the culprit is class **`ib`** alone. The guest's `IT_INDIRECT_BUFFER` is 4 dwords (`0x884c40`,
`0x86fa00`), but the native IB NOP was 5. The compute ring filled with 4-dword IB packets, and the
5-dword NOP left a 1-4 dword tail at the ring end when the guest refill wrapped. The 40 s probe runs
never wrapped that ring; the attract demo does at ~20-28 s.

**Fix:** `StreamOps::EmitIb` writes exactly 4 dwords. An address above 2^40, or a size of 2^24 dwords or
more, keeps the guest packet. The NOP handlers, the prefetch walker and BuildOpList were updated. After
the fix, `ib` alone and all classes pass 45 s of the attract demo at ~121-126 FPS
(`out/re/cutover/bis-ib-fix.txt`, `bis-all-fix.txt`, both present).

## 5. Known limits and open items

- **Install-path default for `SOTC_NATIVE_GNM`** (section 2): unresolved. Do not describe "all classes on
  by default" as fact until `gnm_class_on` in `hooks.cpp` agrees with `ParseMask`.
- **Uncapped benchmark not done.** All numbers are at the 30 FPS capped scene, where the switches mostly
  cut waits and polls without a thread gain (N2-103). The decision on whether the seven switches stay
  default is the uncapped attract benchmark: A (all off) against B (seven on). **Needs the user's
  per-run authorization.**
- **Image check.** Run 74's image check is the user's. Run 75 was checked by the main session.
- **`SOTC_SUBMIT_IDLE=cp` rejected** (N2-106). A real fix needs frame-boundary retirement of virtual
  labels first (N2-98). Do not re-enable it as a quick experiment.
- **Same-submission rule.** Matches are limited to one submission because the game reuses label slots
  across command-buffer builds (N2-96). The SubmitDone barrier does not retire each frame's writes
  (N2-103). Widening the matching needs a new proof.
- **SBO behind `-DSOTC_UF_SBO=1`** (N2-98): built, off. Measure before any default change.
- **Emitters `0x884a60`, `0x884b70`, `0x8848e0`, `0x8849a0`** are reached only through the table at
  `0x39c27f0`; their callers and queues are unresolved (N2-96).
- **Parallel recording and the game job-worker spin are out of scope.** The four job workers (~88
  Mcycles/flip) are unchanged by every switch.
- **PassRecorder risk** (N2-99): PopPendingOperations runs before the GPU submit. No `Scheduler::Sync`.
- **Older open steps** (`0xffffffff` descriptor sentinel, colour-strip work): `HANDOFF-N2-48.md`
  section 6.

## 6. Tools

- **Run script:** `bash out/re/run_native_probe.sh <log-name> [VAR=value ...]` writes
  `out/re/cutover/<log-name>.log`. It exports `SOTC_CVARS="skipSplash=1 nointro"`, `BB_VBLANK_HZ=120`,
  `BB_DMEM_MB=9152` (lower values reproduce the 1.09 GiB OOM) and `BB_RUN_SECONDS` (default 150).
  Per-run overrides apply last. A first boot may stop at guest `0xa5d064`; the script retries once.
- **Build:** `ninja -C out/gpu-phase3 bb-probe.exe` (the directory exists; the target name was not
  re-checked). `gpu/CMakeLists.txt:159` globs `native/*.cpp native/gnm/*.cpp` with `CONFIGURE_DEPENDS`,
  so a new file under `gpu/native/` is picked up without a manual cmake re-run. If a stale object is
  suspected in `src/runtime*.c` (globbed at line 288), touch the file. Not verified this pass.
- **Fault ring:** set `BB_FAULT_RING=<file>` (`src/probe.c:2044`). The file is a mapped ring of 8192
  entries (`fault_entry` in `src/probe.c` near line 1426): nine little-endian u64 per entry, in order
  `tick, tid, code, addr, rip, rsp, region, below, outcome`. `outcome` is the handler path << 8, with
  bit 0 set if execution continued. The write counter is not in the file, so sort non-zero entries by
  `tick`. No reader script is in the repo; read it with a short `struct.unpack('<9Q', ...)` loop. The
  handler path is set at `fault_path` (near line 1448).
- **Thread times:** `SOTC_THREAD_TIMES=1` (`src/probe.c`, `thread_times`), over the window set by
  `SOTC_PROFILE_DELAY` and `SOTC_PROFILE_SECONDS`.
- **Profiler:** `SOTC_PROFILE=<thread-name part>` with `SOTC_PROFILE_OUT=<file>`, then
  `tools/profile_functions.py <file> out/gpu/bb-probe.exe 60 --inlines --qualified`.
- **Screenshots:** `BB_FRAME_DUMP_TRIGGER=<file>` (`gpu/shim/bbgpu.cpp`). When the file appears it is
  removed and the next frame is dumped to PNG under `user/screenshots`, not `BB_DUMP_DIR`. Convert raw
  dumps with `tools/raw2png.py`. Run 75 used dumps at 20 s and 35 s (copies in
  `out/re/cutover/shots-probe-gnm-75/`).
- **Gap table:** `python out/check_pipes.py` must print `bad 0`.
- **Benchmarks:** `tools/sotc_bench_attract.sh` and `tools/sotc_bench_shrine.sh` (the shrine one needs
  `out/save-template/CUSA08809` or `CUSA08034`). Both launch the game: main session only, with per-run
  authorization.
- **REA** (only for a reverse-engineering question): `out/re/eboot_pe_fde.exe` is open in the server.
  Read-only use (`procedure_pseudo_code`, `xrefs`, `read_bytes`, `procedure_assembly`). No
  `set_comment`, `set_address_name` or `close_binary` without a reason.

## 7. Working method

- Every new behaviour goes behind a switch, off until validated. N2-109 flipped the seven validated ones
  on. Changing a default again needs a run table in the gap register.
- Register each run as an N2 row (exactly 7 pipes) in `docs/engine/native-render-gaps.md`, then run
  `python out/check_pipes.py`.
- Verify PM4 constants against the raw bytes of `out/eboot.elf`, not Ghidra's hex output.
