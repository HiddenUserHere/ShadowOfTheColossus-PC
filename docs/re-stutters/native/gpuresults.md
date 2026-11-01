# N0 inventory: CPU consumers of GPU results (gpuresults)

Scope: every place the game's CPU reads memory the GPU writes, or waits on a GPU label, with the readback
rates from the runtime logs. Source: `user/pcmem/*/logs/*.log` "SOTC READBACK RIPS" blocks (160 blocks in the
logs; the latest sessions 20261101-084856 to 085630, qw-recspin and qw-staging are used for the rates),
decomp in `out/re/decomp/<bucket>/<offset>.c` (function containing each RIP found with
`tools/re/eboot_re.py func`). Base code read from the main tree (branch re/pc-native).

Rates are "readbacks/s" per RIP from the top-12 list. Latest run (qw-recspin, 803.5 readbacks/s total):
visibility mask 0x1a6d60 310 + 0x1a6d3a 0 (not in that block), particles 0x101a98 190, 0x101a10 27,
heap 0x48ac5 31 + 0x49500 26 + ...; the summed rates over all 160 blocks put the visibility masks at about 40%
of all readbacks, particle stats at about 25%, heap-metadata pages about 8%, and the remainder spread over
about 40 small RIPs.

## Inventory (consumers)

| # | eboot RIP (rate) | Function (decomp) | GPU result written | CPU use | Class | Recommendation |
|---|---|---|---|---|---|---|
| C1 | 0x1a6d60 (~150-310/s), 0x1a6d3a (~15-75/s) | `BPXPR_ConsumeVisibilityMask` 0x1a6d20 (1386 B), caller `FUN_0019af20` (from `FUN_0011d7a0`, RenderXPR) | Per-set visibility bitmask (64-bit words) written by the GPU through the four mask pointers in the set table (`table + index*0xa8`, `XPRMEM_BUILD_MASKS`) | Walks the set bits and emits the draw/set records (`FUN_0016b850/0016b890/0086b3d0`) only for visible objects; a per-object CPU bit table (`plVar5[4]`, bit `param_3+0x20`) gates the second test | GPU-result, draw-list compaction | (1) make all visible: skip the bit walk and emit the full set, or drive the draw count from an indirect buffer on the GPU; (2) no CPU equivalent (visibility is a GPU result); (3) read the previous completed mask without waiting, only if (1) costs too much GPU time |
| C2 | 0x1a7xxx: 0x1a73f9 (~37/s), 0x1a72a1, 0x1a757a | `BPXPR_ConsumeVisibilityMaskIndirect` 0x1a7290 (1454 B), same caller | Two GPU masks (`param_2` and `param_3`/`uVar6`/`uVar7`) | Same bit walk, second mask path (`FUN_001a7290`); used when `(mode & 0xff00000000)!=0` or `uVar4 != 0/7` with the object flag at `+0x48+0x22` | GPU-result, draw-list compaction | As C1 |
| C3 | 0x1a8a5e (~29/s), 0x1a8b38 (~21-38/s), 0x1a8c83 (~24/s), 0x1a8404, 0x1a757a | `FUN_001a8a40` 0x1a8a40 (534 B, caller `FUN_0019c740`, from `FUN_0011e190` RenderXPRDecals) and `FUN_001a8c60` 0x1a8c60 (358 B) | Bit masks of the same GPU family (decal/visibility masks, `uVar1 = param_1[param_3+0x14]` cursor, `[param_3+0x20]` bound) | Bit-scan of 64-bit words with `LZCOUNT`, produces draw ranges | GPU-result, draw-list compaction | As C1 (same bit-walk pattern; verify the producer in `FUN_0019c740` before patching) |
| C4 | 0x1a8210, 0x1a83f1, 0x1a8a5e (~25-28/s) | `FUN_001a8180` 0x1a8180 (1104 B, caller `FUN_0019b330`) | GPU mask words (`param_2`, `param_3`) | Same bit-walk (sibling of C1 for another set type) | GPU-result, draw-list compaction | As C1 |
| C5 | 0x101a98 (~120-270/s), 0x101a10 (~20-56/s), 0x101ab2 (~20/s), 0x101cdb (~18/s) | `ParticleSystemPool::ProcessStats` 0x1018c0 (1351 B, caller `FUN_00109970`) | Per-system particle counts, a 3-slot ring of arrays dispatched by `sub_928f0` at 0x10197d (`gpu/sotc/particle_prev.cpp` timeline) | Reads `count[system]` (`FUN_0008c1b0` ring pointer, slot (k+1)%3); if `count < 1` it clears the active bit (`pool+0x198`), and after 1000 ms (flag bit 0 of the owner `+0x54`) or 30000 ms it retires the system (`FUN_000fc0f0`, `FUN_00135d90`, log "Killing AutoDestroy ParticleSystem") | GPU-result, lifetime policy | (1) drop the count read: retire systems on the CPU emitter timer (the emitter knows its own duration and spawn state), accept a timing change of up to the retire timeout; (2) CPU count from the sim (emitter spawn/death counts computed on the CPU) if the 30 s rule must stay exact; (3) read slot k-2 without waiting: it is already fence-complete at 30 FPS, so the wait is only the 64 KiB page-sharing fault (see C15). Recommended: (3) first for a safe change, then (1) |
| C6 | 0xe4930 (~21/s) | `WaterRendering::ReadQueries` 0xe48b0 (172 B, caller `FUN_000cefa0`) | Water occlusion query result (`FUN_0086edc0(..., 5, 1)` then copy of `(slot+4)` from the stats slot via `FUN_0008c1b0`) into `param_1+0x11c` | Writes `+0x11c`; consumers of `+0x11c` not yet traced | GPU-result, unknown consumer | Trace `+0x11c` users (open question). If it gates water draws: (1) treat all water as visible; otherwise (3) read the slot with fence age |
| C7 | 0x48ac5 (~19-65/s), 0x49500 (~9-26/s), 0x4944d (~12-15/s), 0x490a0 (~5/s), 0x495fc, 0x4966d, 0x17b02 (~18/s), 0x17edb | `BpeHeap_Walk` 0x48a70, `BpeHeap_Link` 0x493d0, `BpeHeap_Find` 0x49030, `sub_495e0`, `sub_179c0` (0x179c0), `sub_17e90` | NOT a GPU result. Heap free-list nodes (`+0xc0` chunk, `+0x80` list head) live on pages shared with GPU-written allocations | Allocator metadata updates | Allocator page sharing (false sharing) | Not a consumer: move heap metadata to CPU-only pages (allocator specialist, N1/N2). Do not patch the RIPs themselves |
| C8 | 0xf9c40 (~20-48/s), 0xf9c53, 0xf9c70 | `sub_f9c40` 0xf9c40 (76 B, callers `FUN_000fb1e0`, `FUN_000fb2d0`) | Records at `[lVar2+0xc4..0xdc]` (one 32-byte copy per entry of the list at `param_1+0x578`) | Copies 4 qwords per entry into a buffer at `param_1+0x580 + (+0x478 - +0x470)`; the source pages are GPU-written | GPU-result or shared record, class unknown | Open: find who writes `+0xc4..+0xdc` (candidate: a GPU-side transform/bounds write). If GPU-written, (2) compute on the CPU from the transform the CPU wrote, or (1) skip the copy |
| C9 | 0x937fd (~20/s) | `sub_937c0` 0x937c0 (124 B, many callers) | Object descriptor used as a dispatch key (`param_2[1]`, `*param_2+8`) | Dispatch on object type to draw helpers (`0x861520`, `0x860bb0`, `0x861260`) | Probably not a GPU result; readback likely from a GPU-written descriptor page | Open: check whether the descriptor page is GPU-written (allocator class) |
| C10 | 0x92b12 (~18/s) | `sub_92af0` 0x92af0 (87 B, many callers) | Descriptor (`param_2[1]`, `*param_2+8`) | Descriptor bind/update helper | Probably sharing (see C9) | Open |
| C11 | 0x112306 (~21/s) | `sub_1122e0` 0x1122e0 (150 B, caller `FUN_00193810`) | Copies 10 qwords of `param_1` state (`+0x30..+0x218`) into a local block and calls `0x85e8e0` | Pure state copy into the GPU side | Likely state readback, not a result | Open: check `param_1` writers |
| C12 | 0x11d940 (~17/s), 0x11e222 (~11/s) | `BPXPR_RecordMaskedDraws` 0x11d7a0 (1073 B) and `sub_11e190` 0x11e190 (294 B, `RenderXPRDecals`) | Mask set table and the decal list | Calls `FUN_0019c740` / `FUN_0019af20` (C1, C3) | Sync wait + consumer | See S1 (hard spin below). Patch the consumer chain with C1/C3 |
| C13 | 0x19d624 (~19/s), 0x19b435 (~26/s) | `sub_19d410` 0x19d410 (9092 B, callers `FUN_0019d340`, `FUN_000cefa0`), `BPXPR_BuildSetEntry` 0x19b400 (857 B) | Set entries, the GPU mask words used for the set build | Builds set entries from mask bits | GPU-result, probably as C1 | Open: the function is large; a full read is needed before a patch |
| C14 | 0x79ffad (~9/s), 0xd52b0 (~17/s), 0x567f60 (~20/s), 0x251f70 (~19/s), 0x122340/0x12236f (~24/s), 0x135719 (~6/s), 0x16edd4 (~21/s), 0x1d6db0 (~21/s), 0x861570 (~17/s), 0xfa1c3 (~12/s), 0x85e7c5 (~20/s), 0x1010a0/0x1013f0 (particle, ~20/s), 0x103ad0 `ParticleRenderer::UpdateSimulation` (~20/s), 0x16b8c6 `Profiler::PushScope` (~16/s), 0x10124c, 0x1015c4, 0x104635 | `sub_79f3a0`, `sub_d5170`, `sub_567e70`, `sub_251f30`, `sub_122340`, `sub_135690`, `sub_16edc0`, `sub_1d6c90`, `sub_861520`, `sub_fa1a0`, `sub_85e7a0`, `sub_1010a0`, `sub_1013f0`, `ParticleRenderer::UpdateSimulation`, `Profiler::PushScope` | Not read in depth in this pass | Unknown; 8 to 20 readbacks/s each | Unknown | Open. Profiler::PushScope (0x16b8c0) calls `0x86f2a0` when the scope mask matches (likely a GPU timestamp query: check whether it reads a result) |
| C15 | (all particle/stat rates, cross-cutting) | The 64 KiB page of the count arrays is shared with GPU-written buffers | The fault is page-level, not data-level | The stall is a page fault, not a true GPU wait | Allocator placement | Give the count arrays their own pages (allocator, N1) so C5 and C6 cost nothing even before the consumers are changed |

## Sync waits (not readbacks, but CPU waits on the GPU)

| # | Site | Function | What it waits on | Class | Recommendation |
|---|---|---|---|---|---|
| S1 | `FUN_0011e190` 0x11e190: `do {} while (*(long*)(param_3[0x1d]+0x78) != 1);` (when `*(param_3[9]+0x16) == 0`) | `sub_11e190` RenderXPRDecals | Busy-wait on a GPU label (value 1) | Sync | Remove the spin: take the last completed value (C12) or make the label a fence the frame already waits on. The else branch uses `FUN_00035590` (wait with timeout 1). Owner: label/sync specialist |

## Counts by class (inventory above)

- GPU-result consumers with a draw-list or lifetime effect: C1, C2, C3, C4, C5, C6, C12, C13 (8 sites; they carry about 75% of the readback rate).
- GPU-result or shared record, class unknown: C8, C9, C10, C11, C14 (partly unknown; about 15%).
- Allocator page sharing, not consumers: C7, C15 (about 10%).
- Sync waits: S1 (1 site).
- Total sites: 15 readback entries plus 1 wait.

## Elimination options per consumer

Option order: (1) drop the consumer and keep the work on the GPU, (2) compute on the CPU from data the CPU already has,
(3) read the last completed value without waiting.

- **Visibility masks (C1-C4).** (1): skip the bit walk and emit the full set, or use an indirect draw whose count is
  written by the GPU. The GPU has about 60% headroom, so the extra draws cost GPU time; measure it on the Lab route
  first. Patch point: wrap `FUN_0019af20` (entry) at the call to `FUN_001a6d20` / `FUN_001a7290` and
  `FUN_0019b330` / `FUN_0019c740` for C3/C4; replace the mask words with all-ones before the walk, or skip the walk.
  Stale-read (3) is possible for one frame, but the bit walk runs on the CPU side and changes the draw list, so it is
  visible as a pop-in of one frame.
- **Particle stats (C5, C6).** (1) retire systems on the CPU emitter timer: no count read; the only visible change is
  the time at which an empty system is retired (currently 1 s or 30 s after it goes empty). (3) read slot k-2 without
  waiting, which is fence-complete at 30 FPS: no visual change. Patch point: `ProcessStats` 0x1019af / 0x101a98
  (replace the array read with the CPU-side count or the fence-safe slot). Recommendation: first (3) with the page
  fix (C15), then (1) to remove the readback entirely.
- **RenderXPRDecals spin (S1).** Remove the spin, treat as (3).
- **Heap (C7), descriptor and state (C9-C11).** Not consumers of GPU results. Allocator page placement (C15).
- **Unknown (C8, C13, C14).** No recommendation until the consumers are read.

## Open questions

1. Consumers of `param_1+0x11c` (water, C6): does the water draw depend on it?
2. Who writes `+0xc4..+0xdc` of the list entries read by `sub_f9c40` (C8)? GPU-written or CPU-written?
3. Full read of `sub_19d410` (C13, 9 KB) and of `FUN_0019c740` (producer of the decal masks, C3).
4. `Profiler::PushScope` 0x16b8c0 and its callee `0x86f2a0`: is it a GPU query read?
5. Descriptor pages (C9, C10): are they GPU-written (page sharing) or truly GPU results?
6. `FUN_001122c0` and `FUN_00035590` (waits used by RenderXPRDecals) and the label wait family: a separate sync pass is needed (not covered here).

## Patch-point summary

| Option | Sites | Patch point |
|---|---|---|
| Make all visible / indirect | C1, C2, C3, C4, C13 | Wrap `FUN_0019af20` (0x19af20) and `FUN_0019c740` / `FUN_0019b330`, replace the mask words or skip the walk |
| CPU count from sim / CPU retire timer | C5 | `ParticleSystemPool::ProcessStats` 0x1018c0, branch at 0x101a98 |
| Read without waiting | C5, C6, S1 | Slot k-2 in `FUN_0008c1b0` use; S1 spin at 0x11e190 |
| Allocator page placement | C7, C9, C10, C15 | Allocator (not a consumer patch) |

## Caveats

- Rates are from the top-12 lists of the logs (not exact totals); a RIP's rate varies from run to run (e.g. 0x1a6d60 from 155 to 310/s).
- Only about 14 functions were read in full. The other rows come from the function names and call graph; they are marked "open".
- No code was edited. Nothing was run in the game.
