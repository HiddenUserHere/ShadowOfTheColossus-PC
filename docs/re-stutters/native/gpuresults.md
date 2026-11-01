# N0 inventory: CPU consumers of GPU results (gpuresults), revision 2

Scope: every place the game's CPU reads memory the GPU writes, or waits on a GPU label. Rates come from the
"SOTC READBACK RIPS" blocks in `user/pcmem/*/logs/*.log` (160 blocks; the latest sessions 20261101-084856 to
085630 and qw-recspin / qw-staging are used for the per-RIP rates). Code is read from the decomp in
`out/re/decomp/<bucket>/<offset>.c` and the RIP-to-function map from `tools/re/eboot_re.py func`. Base code is the
main tree (branch re/pc-native). No game run, no code edited.

Readback rates are readbacks/s for each RIP in the top-12 list. Latest run (qw-recspin, 803.5/s total): 0x1a6d60
310/s, 0x101a98 190/s, 0x48ac5 31/s, 0x101a10 27/s, 0x49500 26/s, 0x1a8b38 22/s, 0xf9c40 21/s.
Over all 160 blocks: masks (C1-C4, C13) about 40-45%, particle counts (C5) about 25%, page-shared CPU data
(C7-C11, C14, C8) about 25%, water (C6) about 3%, the rest unattributed.

## Summary of classification (revision 2)

| Class | Sites | Share of readbacks (approx.) |
|---|---|---|
| A. GPU-written result consumed by CPU draw-list build (XPR visibility/decal masks) | C1, C2, C3, C4, C13 (+ chain C12) | 40-45% |
| B. GPU-written result consumed by CPU lifetime policy (particle counts) | C5 | 25% |
| C. GPU-written stat, consumer not identified (water query) | C6 | 3% |
| D. Not a GPU result: CPU-owned records or heap metadata on pages the GPU also writes (page sharing) | C7, C8, C9, C10, C11, C14 (except C14 items below), C15 (cause) | about 25% |
| E. Sync wait on a GPU label, CPU spin | S1 | 0 readbacks, one spin |

Previously "unknown" is now resolved: C8, C9, C10, C11, Profiler::PushScope and most of C14 are class D.
Still open: the consumer of C6, the flag at +0x4c in C13 (whether GPU-written), and +0xb4 readers (C5 record).

## Resolved open questions (revision 2)

1. **Water +0x11c (C6).** The producer is `WaterRendering::ReadQueries` 0xe48b0 (called from `EndFrameViewParams`
   0xcefa0 at 0xcfcec). It issues one PM4 WAIT_REG_MEM through `FUN_0086edc0` (packet header 0xc0053c00 = opcode
   0x3c, a GPU-side wait, no CPU wait), then copies dword +4 of ring slot `(count & 1)` (`FUN_0008c1b0`, ring at
   `param_1+0xc8`, 0x28 stride) into `param_1+0x11c`. Consumer search: 277 decomp files reference offset 0x11c, but
   none reads `param_1+0x11c` of the water object. Found readers are other structs (0x91e10 and 0x93960 are
   state-cache setters that compare their own `+0x11c` field; 0x5b9e0 and 0x53fc0 write other objects). Conclusion:
   the value is a per-frame stat whose consumer is not visible in the decomp; `EndFrameViewParams` does not
   reference 0x11c. **Still open**: a caller might copy the block as a struct (`memcpy` of the 0x118-0x130 range).
   The readback itself is a ring read of a page shared with GPU writes (see C15). Recommended: (3) read the
   fence-complete slot; if the water draw proves not to depend on it, (1) remove the read entirely.

2. **Writer of +0xc4..+0xe3 read by sub_f9c40 (C8).** Callers `FUN_000fb1e0` and `FUN_000fb2d0` walk a 64K-bit
   set (`param_1+0x8`, 1024 qwords) and map each bit through the pointer table `param_1+0x2008` to an object record.
   `sub_f9c40` copies 32 bytes (4 qwords from `+0xc4`) of each record into the destination `param_1+0x580 +
   (+0x478 - +0x470)`, a staging array for instance data. The direction is CPU record to GPU-visible upload, not
   a GPU result. The records are written by the CPU (game code) and the readback comes from the page they share
   with GPU-written buffers. **Class D.** Patch: none needed for the consumer; allocator placement (C15).

3. **Profiler::PushScope 0x16b8c0 (was C14).** Its callee `FUN_0086f2a0` is `import_sceGnmInsertPushMarker`
   (a command-buffer write of a debug marker, with `strlen`), not a query. The readback RIP 0x16b8c6 is the read of
   `param_3[9]+0x28` (the state flag word) on the way in. **Class D, not a GPU query.**

4. **Descriptor pages C9, C10.** `sub_937c0` and `sub_92af0` read `param_2[1]` and `*param_2+8` (a descriptor
   record) and dispatch by its type (`0x861520`, `0x860bb0`, `0x861260`) or copy it into a per-set slot
   (`0x8616f0`, `0x8618e0`). They write CPU-side descriptors, and no GPU write to these records appears in the decomp.
   **Class D** (inferred from the access pattern and the page-sharing evidence; a direct GPU writer was not found).

5. **C11 `sub_1122e0`.** Copies 10 qwords of `param_1` state (`+0x30..+0x218`) into a local block and calls
   `0x85e8e0(param_2+0x50, 0, 5, &block)`, a constant or state upload. **Class D.**

6. **C14 items.** `sub_1010a0`, `sub_1013f0`, `ParticleRenderer::UpdateSimulation` 0x103ad0 (5.9 KB) are particle
   pool bookkeeping on the same records as C5 (their readbacks are page sharing with the count arrays). `sub_d5170`
   and `sub_251f30` are `memmove` copies. `sub_79f3a0` (3.9 KB, 20 calls to `sub_a60860`) is a lookup helper with no
   GPU call. `sub_567e70` calls `qsort` (5 call sites in its body); its key source is not yet confirmed, so it stays
   **open** (if the keys are GPU-written, it is a class A/B consumer). `sub_122340` calls the state helpers
   `sub_125a00`, `sub_14d7b0`, `sub_95580` (class D). `sub_861520` and `sub_85e7a0` are small dispatch and
   constant helpers (class D). `sub_fa1a0` calls the owner-table helpers `0x14c4a0`-`0x14d880` (class D).
   `sub_135690`, `sub_16edc0`, `sub_1d6c90`, `sub_79f3a0`, `sub_d5170`: class D (no GPU call found).

7. **C13 `sub_19d410` (9092 B) and the object flag at +0x4c.** The readback RIP 0x19d624 is
   `cmp byte ptr [r15+0x4c], 0` inside the loop over a pointer list (`[r13]`, end at `[rbp-0x7e0]`). It reads a
   one-byte enable flag of each object before building its set entry. Callers: `FUN_0019d340` and `FUN_000cefa0`
   (`EndFrameViewParams`). The flag is CPU-owned in the code seen; whether a GPU pass writes it is **open** (search
   for writers of `+0x4c` in the object's class). Until it is known, the class is **D with a GPU-write caveat**.

8. **FUN_0019c740 (decal mask producer chain).** It calls `FUN_001a8a40(owner = param_1+0x20, mask = [set + 0xa0 + idx*0xa8], param_3, param_2)`.
   The mask is the fifth pointer of the set entry (`+0xa0`), i.e., the GPU decal visibility mask. It is consumed
   by `RenderXPRDecals` (`sub_11e190`), which calls it once per decal object pair from `param_2+8..+0x10`. **Class A.**

9. **XPR consumer call sites (exact).** In `FUN_0019af20` (0x19af20): `call 0x1a7290` at **0x19b0c4** (rel32 bytes at
   0x19b0c5) for the indirect variant, `call 0x1a6d20` at **0x19b0d8** (rel32 at 0x19b0d9) for the direct one. Before
   the direct call: `mov rdi,[rbx+0x18]` (0x19b0ba/0x19b0cb), `mov rsi,rdx` (0x19b0cf, the mask, 3 bytes), `mov rdx,r14`,
   `mov rcx,r15`. Before the indirect call: `mov rdi,[rbx+0x18]`, `mov rcx,r14`, `mov r8,r15` (the second mask is in
   `rdx` set at the table read, and in `rsi` by the callee's own prologue path).
   **Mask pointer origin:** `uVar6` (rdx) is `*(table + idx*0xa8 + k)` with `k` from the switch on `param_3+0x20`
   (0, 8, 0x10 or 0x18 bytes), and `uVar7` the same for `param_3+0x44`. So the GPU mask lives at the four table
   slots written by `BPXPR_BuildSetDraws` (0x19b760, hook `XPRMEM_BUILD_MASKS`). The decal mask is `+0xa0`.

10. **Element counts.** The set table entry is 0xa8 bytes. `*(ushort*)(param_1+0x64)` is the object count of the
    set (`uVar17`); the mask holds `(uVar17+63)/64` qwords. The per-object static table `plVar5[4]` is one bit per
    object (CPU). The draw records are `plVar5[2]` (object sub-records, 0x38 bytes used) and `plVar5[3]` (state
    groups, 0x20 bytes each, index `uVar18` = group, `uVar19` = object range). The state group changes whenever the
    emitted range changes (`FUN_0016b890` close, `FUN_0016b850` open with the group's state word at `+0x20*(g-1)`).

## Inventory (consumers)

| # | eboot RIP (rate) | Function (decomp) | GPU result written | CPU use | Class | Recommendation |
|---|---|---|---|---|---|---|
| C1 | 0x1a6d60 (150-310/s), 0x1a6d3a (15-75/s) | `BPXPR_ConsumeVisibilityMask` 0x1a6d20 (1386 B), caller `FUN_0019af20` (0x19b0d8) | per-set visibility bitmask (64-bit words), written by a GPU pass at the set table slot `+0x0/+0x8/+0x10/+0x18` | scans set bits; for each visible object (and each state-group change) emits a draw record through `FUN_0016b850/0016b890/0086b3d0/000f8990` | A | section "Masks" below |
| C2 | 0x1a73f9 (~37/s), 0x1a72a1, 0x1a757a | `BPXPR_ConsumeVisibilityMaskIndirect` 0x1a7290 (1454 B), caller `FUN_0019af20` (0x19b0c4) | two GPU masks (`rsi` = `uVar7`, `rdx` = `uVar6`) | same scan with a second mask; selected when `(mode & 0xff00000000)` is 0 is false or the object flag `+0x48+0x22` is set | A | as C1 |
| C3 | 0x1a8a5e (~29/s), 0x1a8b38 (21-38/s), 0x1a8c83 (~24/s), 0x1a8404 | `FUN_001a8a40` 0x1a8a40 (534 B), caller `FUN_0019c740` (decal, `+0xa0` mask) from `RenderXPRDecals` | decal visibility mask (GPU) | bit scan of 64-bit words, emits decal draw ranges | A | as C1, for decals |
| C4 | 0x1a8210, 0x1a83f1 (~25-28/s) | `FUN_001a8180` 0x1a8180 (1104 B), caller `FUN_0019b330` | GPU mask words (`param_2`, `param_3`) | sibling bit scan for another set type | A | as C1 |
| C5 | 0x101a98 (120-270/s), 0x101a10 (20-56/s), 0x101ab2 (~20/s), 0x101cdb (~18/s) | `ParticleSystemPool::ProcessStats` 0x1018c0 (1351 B), caller `FUN_00109970` | per-system particle counts, 3-slot ring dispatched by `sub_928f0` at 0x10197d | count `c`: stored in the system record `+0xb4` (`FUN_000fc2d0`); if `c<1`, after 1000 ms (owner `+0x54` bit 0) or flag `+200 & 4`, the system is retired (active bit cleared, release bit set by `FUN_000fc0f0`, `FUN_00135d90` callback); if `c>=1` the pool array `+0x188[u]` takes the time, and after 30000 ms of AutoDestroy the log "Killing AutoDestroy ParticleSystem" fires once and flag `+0x60 |= 4` | B | "Particles" below |
| C6 | 0xe4930 (~21/s) | `WaterRendering::ReadQueries` 0xe48b0 (172 B), caller `EndFrameViewParams` 0xcfcec | water occlusion result (ring slot dword +4) | written to `param_1+0x11c`; consumer not found | C | 3 or 1, see resolved Q1 |
| C7 | 0x48ac5 (19-65/s), 0x49500 (9-26/s), 0x4944d, 0x490a0, 0x495fc, 0x4966d, 0x17b02 (~18/s), 0x17edb | `BpeHeap_Walk` 0x48a70, `BpeHeap_Link` 0x493d0, `BpeHeap_Find` 0x49030, `sub_495e0`, `sub_179c0`, `sub_17e90` | not a GPU result | heap free-list node updates on pages the GPU also writes | D | allocator page placement (C15) |
| C8 | 0xf9c40 (20-48/s), 0xf9c53, 0xf9c70 | `sub_f9c40` 0xf9c40 (76 B), callers `FUN_000fb1e0` and `FUN_000fb2d0` | none (CPU record) | copies 32 bytes per dirty object into the upload staging array `param_1+0x580` | D | allocator page placement (C15); no consumer patch |
| C9 | 0x937fd (~20/s) | `sub_937c0` 0x937c0 (124 B) | none found | dispatch by descriptor type | D (inferred) | C15 |
| C10 | 0x92b12 (~18/s) | `sub_92af0` 0x92af0 (87 B) | none found | copies a descriptor into the set slot `*(param_1[4]+0x14)` | D (inferred) | C15 |
| C11 | 0x112306 (~21/s) | `sub_1122e0` 0x1122e0 (150 B), caller `FUN_00193810` | none | state block upload to `0x85e8e0` | D | C15 |
| C12 | 0x11d940 (~17/s), 0x11e222 (~11/s) | `BPXPR_RecordMaskedDraws` 0x11d7a0 (1073 B), `RenderXPRDecals` `sub_11e190` (294 B) | (chain, not a reader) | calls C1/C3 chain; S1 spin in the decal path | chain | see S1 and the masks section |
| C13 | 0x19d624 (~19/s), 0x19b435 (~26/s) | `sub_19d410` 0x19d410 (9092 B), `BPXPR_BuildSetEntry` 0x19b400 (857 B) | possibly the GPU enable flag at object `+0x4c` (open) | builds set entries | A or D (open, see Q7) | read the writer of `+0x4c` |
| C14 | see resolved Q6 | class D or open items | | | D (mostly) | |
| C15 | (cross-cutting) | 64 KiB pages shared by CPU records or count arrays and GPU-written buffers | | a page fault, not a true GPU wait | root cause | allocator placement (N1) removes classes B and D readbacks |

## Sync waits

| # | Site | Function | What it waits on | Class | Recommendation |
|---|---|---|---|---|---|
| S1 | `FUN_0011e190` 0x11e190: `do {} while (*(long*)(param_3[0x1d]+0x78) != 1);` when `*(param_3[9]+0x16) == 0` | `sub_11e190` RenderXPRDecals | busy-wait on a GPU label (value 1) | E | the label is a decal-done flag; remove the spin and read the last completed value, or keep the flag as the frame fence. The else branch calls `FUN_00035590(..., wait=1)`. Owner: label/sync specialist |

## Masks (C1-C4, C13): GPU-driven replacement and the all-visible fallback

### What the GPU already has, and what the CPU adds
- The GPU has the mask (the visibility pass writes the set's 64-bit words).
- The CPU already has the candidate draw list as static tables per set: `plVar5[2]` object sub-records (0x38 bytes),
  `plVar5[3]` state groups (0x20 bytes, with the state word and the draw-buffer offset `*(ushort)(+0x18)*4 + param_1+0x38`).
- The CPU decides only which candidates to emit; it does no per-object work beyond the bit scan and the state-group
  transitions. The per-object static bit (`plVar5[4]`, one bit per object, bit `param_3+0x20`) is also CPU-side.

### Design A (preferred): GPU compaction into indirect count draws
- **Per set and per state group**, a compute pass (one dispatch per frame for all sets) reads the GPU mask words and
  the static per-object bit table (uploaded once), and writes:
  - an args buffer: one `DrawIndexIndirect` (5 dwords) per candidate object, compacted to the front of each group; and
  - one count dword per state group.
  - The compaction is a per-group prefix sum over the bits (64-bit popcount + one scan).
- **Emission:** instead of the bit walk, the CPU emits, for each state group, one
  `sceGnmDrawIndexIndirectCountMulti` (PM4 opcode 0x9d, `DrawIndexIndirectCountMulti`) with the group's args region and
  count dword. These are already implemented: `liverpool.cpp:3870` (`DrawIndexIndirectCountMulti`) and
  `vk_rasterizer.cpp:2204` (`vkCmdDrawIndexedIndirectCount`); the game's own `sceGnmDrawIndexIndirectCountMulti`
  (gnmdriver.cpp:763) is the entry point the port already exposes.
- **Why groups, not one draw:** a state group changes render state (the `FUN_0086bf50/bfb0/bed0` register writes
  in the consumer), which a multi-draw cannot change between draws. Groups keep the state exactly as the CPU emits
  it today, so the number of multi-draws per set equals the number of state groups the CPU already walks (static).
- **Patch points:**
  - Replace the two direct call sites in `FUN_0019af20`: `E8 rel32` at **0x19b0c4** (indirect path) and **0x19b0d8**
    (direct path). Each 5-byte call is redirected to a native `sotc_mask_emit_groups(set, mask, ...)` stub (rel32
    recomputed). Nothing else in `FUN_0019af20` changes.
  - Or hook the callee entries 0x1a6d20 and 0x1a7290 (mode `entry`, like `XPRMEM_BUILD_MASKS`), and skip the bit scan in the stub.
  - Native side: the compute pass in `gpu/sotc/` (after the mask write, before the first draw of the set), the group
    table built once at set build (`BPXPR_BuildSetEntry` 0x19b400 already walks the groups).
- **Ordering:** the compute must run after the GPU mask write of the same frame and before the draw; the existing
  barrier for the set table covers this (same queue, one barrier). No CPU wait.
- **Shader cache:** no shader recompile is needed (one new compute).

### Expected extra GPU work
- Candidates C and visible V per set are not in the logs. Work added by the design:
  - the compute: about `N_objects/64` words per set, negligible;
  - the extra draws: `(C - V)` draws per group, each an indirect args entry with count 0. In Vulkan a zero-count
    indexed draw costs a command-processor slot and a vertex-fetch setup; order of magnitude 0.5-2 us per empty draw
    on these GPUs (**estimate**, to measure).
  - Design A emits **no** extra draws: the count dword removes them (the multi-draw count is read by the GPU), so
    the extra cost is only the command-processor work of the count read, which is one per group.
- So the expected extra GPU time is about `groups_per_frame * 2 us` plus the compute, not `(C-V)` draws. For the
  heavy frame (about 6,500 draws, of which the XPR share is unknown; report 14 gives two masked lists per cascade
  for indices 0-3), the number of groups is in the tens to low hundreds per frame (**estimate**, to measure with the
  state-group counter).
- Measurement without a game run (later): count, in the existing bit walk, the popcount of the mask words and the
  number of candidate records per set (a read-only counter; the walk already runs). That gives `V` and `C`.

### Fallback: all visible (no GPU work change, more draws)
- **Patch:** make the walk see every bit set. The walk's inputs are the mask pointers. Two options:
  1. **Entry hook** on `BPXPR_ConsumeVisibilityMask` 0x1a6d20 (and `...Indirect` 0x1a7290 for both masks): replace the
     mask pointer argument (rsi, and rdx for the indirect variant) with the address of a process-wide all-ones
     buffer of `(count+63)/64` qwords (prefilled `0xffffffffffffffff`, sized for the largest set; ~8 KiB at 65536 objects).
     Prologue bytes at the hook must be moved to the stub (the port's `entry` mode already handles this for XPRMEM).
  2. **Call-site patch** at 0x19b0cf (`mov rsi, rdx`, 3 bytes): not enough room for `mov rsi, imm64` (10 bytes), so use
     option 1 instead.
- **Cost:** every candidate object is emitted; the GPU draws the invisible ones. Expected extra GPU work = `(C - V)`
  full draws per frame. This is the number to measure (see above); it is bounded by the total draws of the set.
- **Stability:** the walk keeps the static per-object bit (CPU side), so object groups that are statically disabled
  still skip; only the GPU visibility is dropped.

### Recommendation for the masks
1. Measure `C` and `V` first (read-only counters in the existing walk; cheap, no output change).
2. If `(C - V)` draws cost less than the GPU budget (the GPU is about 37% busy, so headroom is large), the all-visible
   fallback is the first step (one entry hook, no shader change).
3. Design A is the target: it removes the readback and keeps the GPU cost at `groups` count reads. It needs one
   compute and the set's group table, which is the N5 "indirect count" direction.

## Particles (C5): CPU-timer retirement (option 1), precise design

### Current logic (observed, `ProcessStats` 0x1018c0)
For each active system `u` (bit set in pool bitmap `pool+0x198`, bounded by `pool+0x1a8`):
- `c = count[u]` from the GPU ring slot (`FUN_0008c1b0`, slot `(k+1)%3`, read at 0x101a98).
- `FUN_000fc2d0(sysrec(u), c)` stores `c` at the system record `+0xb4` (the record stride is 0x100 in `*pool`).
- If `c < 1`:
  - if owner `O = FUN_0014d7b0(pool+0x28)` has `O+0x54` bit 0 (AutoDestroy) **and** `now - sysrec+0xc0 > 1000` (ms), **or** `sysrec+200 & 4`:
    clear the active bit, `pool+0x1b8 -= 1`, release the record (`FUN_000fc0f0` sets the release bit at `pool+0x808 + ...`),
    and call `FUN_00135d90(*(pool+0x1c0)+0x178)` when `sysrec+0xcc != -1`.
- If `c >= 1`:
  - `pool+0x188[u] = now`;
  - if AutoDestroy and `now - sysrec+0xc0 > 30000`: once per pool, log "Killing AutoDestroy ParticleSystem after %.0f seconds"
    and set `pool+0x60 |= 4`; then `sysrec+200 |= 0x14 or 8`; `sysrec+0xc4 = now`.
- `now` is `param_3 + 0x104f8 + O.frameIndex * 0x18` (the frame time table, one entry per frame slot).

What the count is used for: (a) the retire decision, (b) the copy at `sysrec+0xb4`. **Open:** the readers of
`sysrec+0xb4` (a grep over `out/re/decomp` gives hundreds of generic `+0xb4` hits, so the reader was not isolated;
the port's `particle_prev.cpp` observe mode is the handle to check it at runtime). If a renderer skips systems with
`+0xb4 == 0`, the replacement must keep it at `>= 1` while the system is alive.

### Design (option 1): CPU timer instead of the count
- **Liveness stamp.** Replace `c` with a CPU estimate `alive(u)`: the system counts as having particles when the
  game's emitter is in a state that can spawn, or when the last spawn is younger than the longest particle lifetime.
  - Needed inputs (not yet read): the emitter's stop state (the `+0x54` flags of the owner, and the system's own
    spawn flag); the longest particle lifetime of the system's emitter descriptor (the `desc+0x8` / `+0x4`
    family is the candidate; confirm by reading `FUN_000fc2d0`'s callers and the emitter setup).
  - Rule: `alive(u) = spawning(u) || (now - lastSpawn(u)) < maxLife(u) + 1000 ms`. `lastSpawn` is stamped when
    `spawning(u)` is true; `spawning` comes from the game's own stop/start state.
  - The 1000 ms AutoDestroy grace and the 30000 ms log remain exactly as today, driven by `now - lastAlive`.
- **Retire.** The retire branch stays as it is, with `c < 1` replaced by `!alive(u)`.
- **`+0xb4` copy.** Write `alive(u) ? max(1, previous) : 0` so readers see a non-zero count while alive (the value
  is only a "has particles" flag if the readers use it that way; to confirm).
- **Read removal.** Skip the ring read at 0x1019af and the array read at 0x101a98; keep `pool+0x188[u] = now`.
- **Dispatch.** The count-pass dispatch `sub_928f0` at 0x10197d is only needed for the removed read. Skipping it
  (a 5-byte call) saves a small compute. Keep it until the readers of `+0xb4` are known, since the dispatch also
  fills the count arrays that other code may read.

### Patch points (option 1)
- `ProcessStats` 0x1018c0: replace the `iVar4 = *(int*)(lVar8 + uVar16*4)` read at 0x101a98 with a call
  `sotc_particle_alive(sys)` returning the CPU estimate; remove the ring read at 0x1019af (`FUN_0008c1b0` call);
  keep the retire and log branches. 0x10197d (dispatch) is optional (see above).
- Hook form: wrap-replace of the read site, which is a single `mov` from the array; the stub gets `(pool, u)`.

### Timing change
- Today a system with an empty count is retired 1 s after it last had particles (AutoDestroy) and logged at 30 s.
- With the CPU timer it is retired `maxLife + 1 s` after the last spawn. For systems with short lifetimes the
  change is at most the lifetime difference; for long-lived emitters the retire time moves later. Expected visible
  effect: none for normal play (the system is empty when it retires); check the Shrine and an emitter-heavy area.

### Recommendation for particles
- Ship option (3) first (read slot k-2 without waiting). It is fence-complete at 30 FPS, so it does not wait; this
  is safe without new state, and the page fix (C15) removes the fault cost.
- Then option (1), once the emitter stop state and `maxLife` are read and the readers of `+0xb4` are confirmed.

## Open questions (remaining)

1. Readers of `sysrec+0xb4` (particle count copy) and of the water `+0x11c` stat (resolved only partly: the value is
   written, no reader isolated).
2. Whether the writer of object `+0x4c` (read by `sub_19d410`, RIP 0x19d624) is a GPU pass.
3. The key source of `sub_567e70` (`qsort`): CPU-computed or GPU-written.
4. Emitter stop state and the longest particle lifetime per system (inputs of the CPU timer).
5. The actual candidate and visible counts per set (`C`, `V`): needs read-only counters in a game run (not authorized yet).
6. The label family behind S1 (`FUN_00035590`, `FUN_001122c0`): a separate sync pass.

## Caveats

- Rates come from the top-12 lists of the logs (not exact totals); they vary between runs (0x1a6d60: 155 to 310/s).
- Class D (page sharing) is inferred from the readback pattern (a CPU record read, no GPU writer seen in the decomp,
  and the page-sharing evidence in `gpu/sotc/particle_prev.cpp`); it is not proven per site.
- Costs of the GPU replacement are estimates, to be measured.
- No code was edited, no game was run, nothing was committed.
