# Readbacks: what the game's CPU reads from GPU writes, and how to serve each one

Agent M2, read-only. No source edited, no build, no game run, no git change. Labels: **observed** (log,
code or decomp read in this pass), **inferred** (reasoning), **unknown** (needs a run).

## 0. Method and limits

- **Pre-native logs** (the old page-tracking path): `user/pcmem/*/logs/*.log`, 238 files, 1122 windows of 5 s
  (the `SOTC READBACK RIPS` block). Aggregated with awk. Command in section 7.
- **Native logs** (current default switches from `sotc.ini`: `SOTC_NATIVE=1`, `SOTC_NATIVE_MASKS=prev`,
  `SOTC_NATIVE_PARTICLES=prev`): `user/gcn-run1..4/*.log`, `user/gcn-cold1..5/*.log`, `user/pass1`, `user/area1`
  (the ten logs with a readback line were read: about 35 windows). These are the runs that matter for the next design.
- The `SOTC READBACK RIPS` line counts **copy-path entries** per guest RIP, not blocked time (07-readback-diagnosis.md,
  M10, buffer_cache.cpp:1119 `NoteReadbackRip`). The table is the top 12 only. In the 1122 windows, "not listed at
  other offsets" is 0 every time, so the top-12 list is complete for eboot RIPs.
- **Per-site wait time is not logged.** The only wait times are per-window totals ("SOTC RB guest wait"). Per-site
  cost below is a share of readbacks, multiplied out only where marked as inferred.
- **Correction to the brief:** `eboot+0x49500` does not appear "in every healthy run". It is in 173 of 1122
  pre-native windows (about 19/s when present). `eboot+0x48ac5` is in 607 windows. `0x1013f0` and `0x101720` never
  appear as readback RIPs (section 3.9).

## 1. Headline

1. **Pre-native (pcmem, 1122 windows):** about 426 readbacks/s on average, 478k in total. Each window waited a mean
   of **569 ms** for the GPU, summed over guest threads (638 s in total). Mean wait per waited readback: about 766 us.
   Of the total wait, about 40% is GPU time, about 5% submit, and the rest is not split by the counters (inferred:
   copy and finish work on the CPU side). Source: "SOTC RB guest wait" lines, e.g.
   `user/pcmem/20261101-084856/logs/default-r1-a1.log:589` (4491 readbacks, 1778 ms, GPU 1064 ms).
2. **Native default (gcn-run and gcn-cold logs):** `SOTC READBACK RIPS` is **0.0 readbacks/s** and "SOTC RB guest
   wait" is **0 readbacks** in every window (e.g. `user/gcn-run1/s6.log:537`, `:649`). The guest page-fault readback
   path is effectively gone in the native configuration. The per-window stats for particles and masks come from
   the native counters instead (section 3).
3. **What still waits in the native configuration** (ranked in section 5): (a) the XPR cull label spins, which the
   `prev` switch does not remove (`gpu/sotc/native/native_results.cpp:246-249`), unmeasured; (b) the frame-slot fence
   (`native_sync.cpp:38-50`), a real CPU wait on GPU completion two frames back; (c) "exact" particle count reads that
   fall back when no copy exists (about 48/s); (d) zero-mask fallbacks that draw nothing for a frame (a visual risk,
   not a wait); (e) the port's own command-processor label spin (4 to 15 ms per frame, not a game read).

## 2. Readback sites, ranked by measured share (pre-native, 1122 windows)

Share = summed readbacks/s over all windows divided by 478k (the total). "Rate" is the summed per-window
readbacks/s, so it is a share, not a rate.

| Rank | eboot RIP | Function (decomp) | Share | Class | GPU data read | Freshness needed |
|---|---|---|---|---|---|---|
| 1 | 0x101a98 (+0x101a10, 0x101ab2, 0x101cdb, 0x101b25) | `ParticleSystemPool::ProcessStats` 0x1018c0; count array via 0x8c1b0 | 23.3% (family 28.8%) | B | per-system particle count from the count dispatch (`sub_928f0`, 0x10197d) | 1-2 calls old is enough (3-slot ring, section 3.1) |
| 2 | 0x1a6d60 (+0x1a6d3a) | `BPXPR_ConsumeVisibilityMask` 0x1a6d20 | 22.5% + 2.3% | A | per-object visibility bits of the main-scene cull | the game spins for this frame's cull label (section 3.2) |
| 3 | 0x1a72e0 (+0x1a73f9, 0x1a72a1, 0x1a757a) | `BPXPR_ConsumeVisibilityMaskIndirect` 0x1a7290 | 9.4% + 0.1% | A | second visibility mask (indirect path) | same as 2 |
| 4 | 0x48ac5, 0x49500, 0x4944d, 0x490a0, 0x17b02, 0x17edb | `BpeHeap_Walk` 0x48a70, `BpeHeap_Link` 0x493d0, `BpeHeap_Find` 0x49030 | about 5% | D | none (CPU heap metadata on GPU-written pages) | now (CPU's own data) |
| 5 | 0xf9c40 (+0xf9c53, 0xf9c70) | `sub_f9c40` 0xf9c40, callers `FUN_000fb1e0`, `FUN_000fb2d0` | 3.7% | D | none (CPU record copied to upload staging) | now |
| 6 | 0x1a8b38, 0x1a8a5e, 0x1a8c83 | `FUN_001a8a40` 0x1a8a40 (decal mask, C3) | about 4.8% | A | decal visibility mask | same as 2 |
| 7 | 0x1a8210, 0x1a83f1 | `FUN_001a8180` 0x1a8180 (C4) | about 3.5% | A | sibling mask words | same as 2 |
| 8 | 0x937fd, 0x92b12, 0x112306 | `sub_937c0`, `sub_92af0`, `sub_1122e0` | 2.2% | D | none found | now |
| 9 | 0x19d624 (+0x19b435) | `sub_19d410` (flag at object +0x4c), `BPXPR_BuildSetEntry` 0x19b400 | about 1.4% | A or D (open) | possibly a GPU-written enable flag | unknown |
| 10 | 0xe4930 | `WaterRendering::ReadQueries` 0xe48b0 | about 0.2% (51 windows) | C | GPU occlusion result (ring slot dword +4) | any; consumer not found |
| - | 0x1013f0, 0x101720 | particle walk and compacting copy | **0 windows** | ? | particle state (section 3.9) | unknown |

Classes (from `docs/re-stutters/native/gpuresults.md`): A = GPU visibility result read by the CPU draw-list build;
B = GPU particle count read by the CPU lifetime policy; C = GPU stat, consumer not found; D = not a GPU result,
CPU data on pages the GPU also writes (page-sharing faults).

Approximate split, pre-native (inferred, equal wait per readback): particles about 29% of the 638 s, masks (A) about
45%, page sharing (D) about 11%. The mask share is the largest single cost.

## 3. Per-site notes

### 3.1 Particle counts (rank 1): latency-tolerant, already served
- **Game side (observed):** `ProcessStats` 0x1018c0 reads `count[u]` from a 3-slot ring entry at 0x101a98
  (`docs/re-stutters/native/gpuresults.md`, section "Particles (C5)"). The entry is written by the count dispatch
  `sub_928f0` (0x10197d). The read happens two calls after the request (particle_prev.cpp:262-271, cited in
  07-readback-diagnosis.md, M4). The game uses the count for a lifetime decision only: retire a system after 1 s
  without particles, log at 30 s (`gpuresults.md`). **Inferred:** the game tolerates a count one or two calls old.
- **Why the port waited:** the copy is recorded in the tick being recorded, so a fault in the same tick forces a submit
  (07-readback-diagnosis.md M3). The read is latency-tolerant; the wait came from the port's copy timing, not from the game.
- **Native serving (observed):** `SOTC_NATIVE_PARTICLES=prev` serves the last completed copy
  (`gpu/sotc/native/native_particles.h:1-10`, `gpu/sotc/native/native_particles.cpp:11-18`). Native line
  `user/gcn-run1/s6.log:767`: 175.3 calls/s, 641 served same-call, 4 served older, **242 exact reads** (fallback when
  no complete copy). That is about 48 exact reads/s still on the GPU array (**observed**). Verify counts are 0
  mismatches (`s6.log:767`), but only in mode 2 (nothing served).
- **Risk:** retire timing (a system with a count of zero is retired a little later or earlier by at most a copy
  age); the readers of `+0xb4` are not isolated (impl-particles.md, row d). Low visible risk.
- **Serving strategy:** (a) latency-tolerant, already implemented. The 48/s exact fallbacks should be served from
  the k-2 copy too, or proven safe to read without a fence (see 3.10).

### 3.2 XPR main-scene visibility (rank 2): the game waits for this frame's cull
- **Game side (observed, `docs/re-stutters/native/sync.md` 2.1):** the cull writer `FUN_000925c0` sets a label with an
  EOP (data 1). Four spins read it: X1 0x11dbe0 (`IndexCullXPRObjects`), X2 0x11d7a0 (6 callers), X3 0xceae0,
  X4 0x11e190 (`RenderXPRDecals`). The spin is `do {} while (*label != 1)` on one branch of `obj[9]+0x16`; the other
  branch waits on a job counter. **Which branch is taken in the shipped game is unknown.**
- **Consumers:** `BPXPR_ConsumeVisibilityMask` 0x1a6d20 and `...Indirect` 0x1a7290 (`docs/re-stutters/native/gpuresults.md`
  C1, C2; `docs/re-stutters/reports3/07-readback-diagnosis.md` section 3).
- **Freshness:** by the game's own code, this frame's cull. The port's current answer is the previous completed
  copy (`gpu/sotc/native/native_masks.h:5-16`, "the previous frame's mask in practice").
- **Native serving (observed):** `SOTC_NATIVE_MASKS=prev` serves the last completed copy for consumers C1, C2, C3
  (`gpu/sotc/native/native_results.cpp:5-22`, 175-184). Native line `user/gcn-run1/s6.log:761`: 975 consumer calls/s,
  4110 served older, 0 served same frame, and **822 zero-mask fallbacks** ("no copy, frame"). The fallback writes a
  zero mask, so the objects draw nothing for that frame (`native_masks.cpp:700-727`). `native_masks.cpp:17` records
  the same symptom on 1 Nov ("grass flicker and objects invisible for a frame, about 800 zero masks per 5 s").
- **The spins are not removed in this mode.** `native_results.cpp:246-249`: "the spin is only removed with
  SOTC_NATIVE_MASKS=all". The hook log says the spin hooks are "off" (`s6.log`, NATIVE_SPIN_* lines). So the game
  still blocks on each cull label if the spin branch is taken. The wait is invisible to the readback counters.
  **Unknown:** how often the spin branch runs and how long it blocks (section 6, item 1).
- **Serving strategy:**
  - (c) **GPU compaction, Design A in gpuresults.md.** The cull shader's output stays on the GPU; a compute pass writes
    indirect count draws per state group. The CPU never reads the mask and the spin is not needed. This is the only
    option that removes the dependency. It is a rework of the draw emission (per group), not a one-line change.
  - Interim (a): keep `prev`, but never serve a zero mask. Serve the last mask of the same key even when old
    (the code already does this for `g_last_masks`; the zero path runs only when no last mask exists). Measure the 822/5 s.
  - The all-visible fallback (`SOTC_NATIVE_MASKS=all`) removes the spins too, but draws every candidate object:
    expensive, as noted in `gpuresults.md` (Fallback section). Use only as a measurement tool.
- **Risk:** Design A: a new compute and a changed draw path. Interim (a): popping when the cull changes faster than one
  frame (visible as one-frame-late culling). Zero masks: visible objects vanish for a frame. Shader cache: none (no
  shader recompile for the compute in Design A, per gpuresults.md).

### 3.3 XPR indirect mask (rank 3): same as 3.2
- `0x1a72e0` is the first qword scan of the indirect consumer (`07-readback-diagnosis.md` section 3.1). Two masks
  in the indirect path (`rsi`, `rdx`); `native_results.cpp:177-181` serves both when `Prev()` is on.

### 3.4 Decal and C4 masks (ranks 6, 7)
- Decal C3 (`FUN_001a8a40`) is served by `prev` (`native_results.cpp:175-180`, kDecal).
- **C4 (`FUN_001a8180`, 0x1a8210 and 0x1a83f1) is not served** (`native_results.h`/`native_masks.h`: "C4 is not served yet";
  `native_results.cpp:14-16`). In native runs C4 does not show as a readback (0 RIPs), so it is not waiting through the
  fault path. **Inferred risk:** if C4 reads GPU-written memory without a fence in native mode, the value it reads is
  whatever the GPU has written so far (a race), and the port does not count it. Check (section 6, item 4).

### 3.5 Page-sharing readbacks (ranks 4, 5, 8): no GPU data
- `BpeHeap_*` (0x48ac5, 0x49500, ...), `sub_f9c40` (0xf9c40), `sub_937c0`, `sub_92af0`, `sub_1122e0`: they read CPU-owned
  records (free lists, staging, descriptors) that sit on 64 KiB pages the GPU also writes
  (`gpuresults.md`, C7-C11, class D; `07-readback-diagnosis.md` M1). The readback is a page fault, not a GPU result.
- **Serving:** no wait is needed. Remove the cause: allocator placement, so CPU metadata never shares a page with a
  GPU-written buffer (`gpuresults.md`, C15, N1). This is a memory-layout change, not a sync change.
- **Native status (observed):** these RIPs are absent from all native logs (0 RIPs). The native page path (SOTC_PC_RING=8,
  host arena) appears to remove the fault (`sotc.ini`; `user/gcn-run1/s6.log:2-6` header). **Inferred**: class D is
  already gone in the native configuration. Confirm with one run that keeps the heap RIPs.

### 3.6 C13 flag at object +0x4c (rank 9)
- `sub_19d410` reads a one-byte enable flag (`cmp byte ptr [r15+0x4c], 0`, RIP 0x19d624) per object before building
  the set entry. The writer of the flag was not found (gpuresults.md, resolved question 7). If the writer is a CPU
  path, it is class D; if a GPU pass, class A. **Unknown.**

### 3.7 Water query (rank 10)
- `WaterRendering::ReadQueries` 0xe48b0 issues a GPU-side `WAIT_REG_MEM` (0xc0053c00, `FUN_0086edc0`) and copies a
  ring dword into `param_1+0x11c`. The consumer of `+0x11c` was not found (gpuresults.md, resolved question 1).
- **Serving:** (c) drop the read if the value is unused; it costs 0.2%. Verify with the struct copy check in
  gpuresults.md (a `memcpy` of 0x118-0x130 could read it).

### 3.8 Particle distance-fade walk (0x1013f0) and compacting copy (0x101720): not seen
- Decomp: `FUN_001013f0` walks the live bitset, reads particle positions and writes a fade at `particle+0xd4`
  (particles-engine.md 2c, particles-data.md section 1). `FUN_00101720` copies live records (particles-engine.md 2c).
- **Observed:** neither address appears as a readback RIP in any of the 1122 pre-native windows, and no top-12
  list had an unlisted offset. So either the particle state pages are not GPU-tracked at that point, or the walk
  reads CPU-written data. Both are unknown. **Do not assume the walk is a readback** until a probe says so.
- **If it is a real readback** (particle state written by the GPU compute, read by the walk): the walk is a candidate for
  (c), the GPU computes the fade in the particle update shader (the fade needs only the camera position, a
  constant per frame). That removes the CPU walk. This is the only design here that changes game-visible CPU work.

### 3.9 Particle walk notes for the design
- `ParticleSystemPool::ProcessStats` 0x1018c0 retires a system when its count is below 1 for 1 s. The CPU-timer
  variant (`gpuresults.md`, option 1) removes the count read but changes retire timing by the lifetime difference.

### 3.10 Exact reads left in native mode (3.1 fallback, 3.4 C4)
- Native `NATIVE_PARTICLES` still reads the GPU array exactly when there is no complete copy (242 per 5 s in
  `s6.log:767`). Those reads do not show as RIPs, so the port does not wait on them (**observed**: 0 RIPs). Whether the
  value read is fenced is **unknown** (inferred: it is read without a fence, so it can be one frame old, or a racing
  value if the GPU is writing it). A verify run (SOTC_NATIVE=2) compares the copy with the exact read; it does not
  check the fence.

## 4. GPU-written labels and fences the CPU polls

| # | Label (where) | Writer (GPU side) | CPU poll | Purpose | Native status | Needs exact? |
|---|---|---|---|---|---|---|
| L1 | XPR cull labels obj+0x40 (shadow), +0x68 (main), +0x78 (decal); arrays `obj[7]+0xab70`, `+0xa800` | EOP data 1, `FUN_000925c0` (0x925c0), from the three cull submitters `FUN_000c6bd0`, `FUN_000c6f70`, `FUN_000c5200` | spins X1 0x11dbe0, X2 0x11d7a0, X3 0xceae0, X4 0x11e190 (`sync.md` 2.1) | this frame's cull is done | spins NOT removed with `prev` (`native_results.cpp:246-249`); labels reset by `XPRPreRender` (0xca6d0) and set by the port (`native_masks.cpp` header) | game's code says yes; port serves last frame |
| L2 | Frame-slot label obj+0x770 + slot*8 (compare value obj+0x780 + slot*8) | EOP event 4 (bottom of pipe), `FUN_00090e30` (0x90e30) | spin at 0x90ea0 / compare 0x90f47 | DCB/frame-ring reuse, two frames back | replaced by `FrameWait(closed-1)`: `native_sync.cpp:38-50`; hook hits 4-32/s pre, post 0 (`s6.log:431`, `:687`) | yes, as a pacing fence (lag 2) |
| L3 | Second label obj+0x778 / +0x788 (`FUN_00091090`, 0x91090) | no writer found (`sync.md` open 2) | two empty spins | unknown | not hooked | unknown |
| L4 | Ring-full retry (compute ring): read pointer written by CP, polled in 0x863af0, 0x863310, 0x8636e0 | CP read pointer | spin without pause | ring slot reuse | `SOTC_ASC_RPTR` and `SOTC_CP_AHEAD` are unset in `sotc.ini`, so the defaults apply (`liverpool.cpp:363-370`, "both off by default") | yes (ring slot) |
| L5 | Submit gate, `sceGnmDingDong` GateWait(1), `WaitSubmitGate` GateWait(0), `sceGnmSubmitDone` GateWait(2) | port-side submit gate (`gnmdriver.cpp:232, 537, 2607`) | `WaitGpuIdle()` at the default `SOTC_CP_AHEAD=0` (`sync.md` 3.1) | submit pacing | **not measured** in the logs (no counter); `sync.md` 3.1 says it is `WaitGpuIdle` | yes (pacing) |
| L6 | Particle counter wait, `sceGnmComputeWaitOnAddress` (`FUN_00884d40`) | GPU (the count pass) | none on the CPU: the port emits `WAIT_REG_MEM` into the PM4 stream (`gnmdriver.cpp:423-447`) | orders spawn, prep and process passes on the GPU | GPU-side only | GPU ordering, not a CPU wait |
| L7 | Water `WAIT_REG_MEM` (`FUN_0086edc0`, packet 0xc0053c00) | GPU | none on the CPU | GPU-side wait before a ring copy | GPU-side only | n/a |
| L8 | CP label spin (port internal): CP thread waits while draw-thread ops or EOP labels are still queued (`liverpool.cpp:3531-3545`, `liverpool.h:1306-1316`) | the port's own draw-thread ops and EOP-tick labels | the CP thread (`SOTC CP spin`) | port command processor ordering | present in native runs: 4.3-14.6 ms/frame (`s6.log:392`, `:637`); 99.5% of pre-native CP rounds are label rounds | not a game wait, but it is the largest CPU wait left in native |
| L9 | NATIVE_UPLOADS frame waits (port-internal): 3520 waits, average 0.77 ms, max 125.56 ms | GPU frame completion | port upload path | upload reuse | `s6.log:494` | port's own |

Notes:
- L1 and L2 are the game's own GPU-label waits. L8 and L9 are port-internal. L5 is a port gate that replaces a game
  gate (`sync.md` section 6.2 plan).
- `sceGnmComputeWaitOnAddress` (L6) is not a CPU spin: the import writes a WAIT_REG_MEM packet
  (`gnmdriver.cpp:423-447`). The particle counter wait is GPU-side (`particles-engine.md` 2d.2).

## 5. Serving strategy per site

| Site | Freshness | Strategy | Expected gain | Risk | Verify |
|---|---|---|---|---|---|
| Particle counts (rank 1) | 1-2 calls old | (a) async copy, serve k-2 (done, `prev`). Next: serve the 48/s exact fallbacks from the k-2 copy, or prove the exact read is fenced | removes the last 48/s exact reads | retire timing (one call) | `SOTC_NATIVE=2`: copy vs exact count |
| XPR masks C1-C3 (ranks 2, 3, 6) | this frame by game code; 1 frame old in practice | (c) Design A (GPU compaction, no CPU read, no spin). Interim (a): never serve a zero mask | removes the largest wait (about 45% pre-native) and the zero-mask flicker | visible culling lag (interim); compute and draw change (Design A) | `SOTC_NATIVE=2` plus visual A/B; count zero masks |
| C4 mask (rank 7) | same | (a) serve like C1-C3, then (c) | fences the unfenced read (3.4) | same as above | verify counter for C4 |
| Heap and page sharing (ranks 4, 5, 8) | now (CPU data) | no sync change. Allocator placement (C15) | removes page-fault cost (already 0 in native) | allocator change, memory use | native RIP count stays 0 |
| C13 flag (rank 9) | unknown | find the writer first; if CPU, nothing to do | 1.4% pre-native | none | writer search |
| Water (rank 10) | any | (c) drop the read if unused | 0.2% | low | struct copy check |
| Particle walk 0x1013f0/0x101720 | unknown | probe first (3.8). If a real readback: (c) GPU computes the fade | unknown | changes game-visible CPU work | probe: RIP count with the walk on |
| Frame-slot fence (L2) | two frames back | keep as a ticket fence (lag 2); do not remove | none (it is pacing) | removing it lets the CPU overwrite a frame still in flight | `FrameWait` count; `NATIVE uploads frame waits` |
| XPR spins (L1) | game's own | remove with Design A or with `SOTC_NATIVE_MASKS=all` (measurement only) | removes the spin wait | all-visible draws cost GPU time | spin-branch counter (section 6) |
| CP label spin (L8) | port internal | not a readback problem; reduce by moving the draw-thread label work off the CP path | 4-15 ms/frame CPU (native) | ordering bugs | `SOTC CP spin` label share |

## 6. Open items (need a run, in this order)

1. **Spin branch counter for L1.** Count, per window, how often X1-X4 enter their `do {} while (*label != 1)` loop and
   how long they block. The verify hook exists only under `masks=all` (`native_results.cpp:246-249`). Add a counter
   that does not change behaviour. Without it, the largest possible native wait is unmeasured.
2. **Zero-mask fallback.** Measure the 822/5 s zero masks by key. Test: never serve zero when any earlier mask of the
   same key exists (the code already keeps `g_last_masks`; check why the zero path runs).
3. **Exact particle reads (about 48/s).** Check whether these reads are fenced (the frame ticket). If not, serve from
   the k-2 copy.
4. **C4 and the native race.** Confirm whether C4 (0x1a8210, 0x1a83f1) reads GPU memory in native mode and whether
   it sees the current frame's bytes.
5. **0x1013f0 / 0x101720.** Run with the heap-page log on and check whether the particle state pages are GPU-tracked.
   If they are not, the walk is CPU-only and out of scope.
6. **C13 flag +0x4c writer and sub_567e70 keys (qsort).** Still open (gpuresults.md, open questions 2, 3).
7. **Per-site wait time.** Add a per-RIP wait accumulator (the copy path already knows the waiting readback). Right now
   the costs in section 2 are shares, not measured milliseconds.

## 7. Reproduce the aggregates

```
cd user/pcmem
find . -name '*.log' -path '*logs*' -print0 | xargs -0 awk '
/SOTC READBACK RIPS:/ {inb=1; next}
inb && /^  eboot\+0x/ { s[$1]+=$2; next }
{ inb=0 }
END { for (o in s) printf "%s %.0f\n", o, s[o] }' | sort -k2 -rn
find . -name '*.log' -path '*logs*' -print0 | xargs -0 grep -h -o "SOTC RB guest wait: .*"
```
Native check: `grep -h "SOTC READBACK RIPS" user/gcn-run*/*.log` (all 0.0 in the runs read).

## 8. Summary for the design

- The guest readback problem is mostly solved in the native configuration (0 faults per second in the logs), but
  the fix moved the risk into **serving stale or zero data** (masks, particles) and into **game-side label spins
  that are still in place** (L1). Neither is visible in the readback counters.
- The next design should be: (1) count the L1 spin entries and waits; (2) remove the zero-mask fallback; (3) Design A
  for the masks, which removes the dependency; (4) keep the frame ticket fence (L2) as pacing.
