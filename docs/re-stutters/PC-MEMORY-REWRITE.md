# PC memory model: rewriting the game's pool layer (UMA → RAM + VRAM)

Branch `re/stutter-architecture`, 1 Nov 2026. Consolidates reports `reports3/01-06` (wave A, read
only) with the earlier rounds (`reports/03`, `04`; `reports2/06`, `07`, `13`). Supersedes step 6 of
`STUTTER-ARCHITECTURE.md` and extends `docs/sotc/MODELO-MEMORIA-PC.md`. Addresses are eboot VAs for
fingerprint `72c1d4a9…3bb225`; every hook resolves by signature (`docs/HOOK-DESIGN.md`).

Labels: **verified** (checked by the lead in disassembly), **reported** (agent report, not
re-checked), **open** (needs a run).

## 1. Why

The port emulates PS4 unified memory: every page the GPU might read is write-protected, GPU results
come back through read faults, and a fresh 16 MiB region starts all CPU-dirty. The engine gives the
port no way to tell CPU data from GPU data, because its pool layer places allocations by a kind
byte and **spills between pools** (section 2). The plan replaces that layer with `PcPool`, which
knows the owner of every 64 KiB block and places it by memory class.

## 2. The pool layer (spec: `reports3/06-pool-layer-spec.md`)

- Three pool sets: flexible S0 (root, VA only), Onion S1 (root+0xa0, phys from 0x100000), Garlic S2
  (root+0x140, phys from 0x40100000). Reported.
- Order: kind 1 tries flexible → Onion → Garlic; kinds 0 and 2 try Garlic → Onion. Pool type alone
  therefore never identifies CPU or GPU data. Reported.
- VA from a per-set address-ordered free-VA list; phys from a per-set 64 KiB bitmap (bit 1 = free,
  lowest first). Each run is one `sceKernelBatchMap` op 0 (prot 0x33); spill blocks also get op 4.
  Frees of Onion/Garlic blocks unmap and flush before returning. One global spin lock (0x2dfc8a0).
  Reported.
- **`POOL_FIX` (eboot 0x1a321), verified.** Original: when the bitmap scan finds no free bit,
  `jmp 0x1a357` uses r12 = -1 as the bit index and reads `[r9 + (-1>>6)*8]` at 0x1a371 (the Pro-mode
  crash). The patch writes `[r14+0x74] = 0` (cached free count), `eax = 0`, and jumps to 0x1a1f1,
  the free-VA-list path. That entry skips the empty-list guard at 0x1a1e4 (`cmp [r14+0x38], r14+0x28;
  je 0x1a275`, next pool set). If the list is empty, 0x1a205-0x1a215 branch to 0x1a2c0 with rax = 0
  and `mov rcx, [rax+0x58]` reads address 0x58. **Latent crash (bitmap full and free list empty);
  `PcPool` must fall through to the next pool set instead.**
- Open: whether freed Onion/Garlic VA returns to the free list (possible VA leak).

## 3. Allocation entry points

| Entry | Address | Notes |
|---|---|---|
| Block alloc | `mem_allocator_alloc` 0x479a0 | obj name +0x10, kind +0x18; forwards rdx (align) and rcx (kind) |
| Generic dispatcher | 0x12820(size, memtype, align, flags) | memtype = slot id; memtype 1 + TLS scope → scope object |
| Descriptor alloc | 0x8c0d0(desc, size) | **verified:** `0x12820(size, [0x2ed1f54], 0x10, 0)`, RenderSingleFrameGPU; 156 refs |
| GPU buffer wrapper | 0x8be60(usage) | 0x002 → RenderSingleFrameGPU; 0 → GPUMemory |
| Ring flip | 0x136f0 → vtbl+0x20 = 0x1d230 | **verified:** 0x8c860 flips 0x2ed1f54 and 0x2ed1f58 (RenderBackend_Frame 0x90fd2, after the writers) |

Heap-id globals 0x2ed1f48..1f60 are memtype slots: 0x8c880 sets placeholders, `sub_8c370`
overwrites them (reported). Other ring flips: GameFrame_Update 0x25b00b-0x25b03b (five game heaps),
ParticleRenderer::BeginFrame 0x107540, PhysicsJob 0xa3f1c8 (reported).

## 4. Class table

| Owner / data | Class | Basis |
|---|---|---|
| CPUMemory family, GameSingleFrame, GameDoubleFrame, GameToRenderThread, CPU_RenderToGPUToGame, Shadow Heap, physics ring | CPU_ONLY | kind 1; reported |
| Resource blobs (CreateResource 0x147160, heap 1) | CPU_ONLY (staging) | the GPU never reads them; freed after the build (`reports3/03`) |
| Mesh VB/IB copies (0x1264d0 → 0x8be60, GPUMemory), non-streamed texture copies (0x98aa0 → 0x96680), streaming window (0x98ae0, VirtualMemoryGPU) | GPU_STATIC | copied at build, no later patching (`reports3/03`) |
| RenderSingleFrameGPU (0x2ed1f54): view constants 0xbe1f0, instances 0x123410, particles 0x101720, descriptors | GPU_DYNAMIC | 2-slot ring, flipped at 0x8c860 (verified) |
| RenderContextFrameGPU (20 MiB), DCB 5 MiB + CCB 623 KiB per context, GameToGPU, ParticleDoubleFrame, MaterialParamHeap | GPU_DYNAMIC | no writes after submit (`reports3/01`) |
| Render targets (RTAllocator, `render_targets_allocate` 0x9aef0) | GPU_WRITTEN | per area load |
| XPR masks, from RenderSingleFrameCPU (0x2ed1f58) | GPU_WRITTEN + readback | no CPU wait on the label (`reports3/02`) |
| Stats/water rings (accessor 0x8c1b0) | GPU_WRITTEN + readback | `reports2/07` |
| The 4 GPU blocks re-published by XPRPreRender (incl. 56 MiB) | **unknown** | `reports3/05`; tracked as today |
| BPE heap headers/links (0x48a70-0x4a477) | CPU_ONLY metadata | follows its heap's class |

**Conflict to settle at run time:** RenderSingleFrameCPU (0x2ed1f58) is kind 1 per `reports3/04`
and `05`. If the GPU writes the XPR masks there, a "CPU" heap holds GPU-written data. Until the
probe shows its per-call kind and the GPU-written pages in it, it stays tracked (fallback class).

## 5. Open items for the wave B probes

1. Kind and GPU-written bytes per owner (resolves the 0x2ed1f58 conflict).
2. Chunks per frame from 0x90470, and where 5+ MiB per frame of DCB/CCB is recycled (only the
   teardown path `sub_90050` was found).
3. Contents of the four XPRPreRender blocks.
4. Freed VA reuse in the Onion/Garlic sets.

## 6. First measurement (1 Nov, wave B, ~2 min of play)

`SOTC_HOOK_ALLOC_PROBE=1 SOTC_CLASS_TRACK=2 SOTC_FPS=1`. The memtype and XPR probes did not install
(`0x136f0`: indirect branch the detour cannot relocate; `0x19b760`: wrap refused, the function
realigns its stack); both were fixed for the next run.

- **Only 9 block-level owners** hold all 3.6 GiB; every named heap is carved inside them.
- Write faults (~7,300/s), per owner:

| Owner | Live | Write faults/s | Reads/s | GPU pages |
|---|---|---|---|---|
| `Default TaggedArena` kind 2 (per-frame GPU heaps) | 96 MiB | **3,548 (48%)** | 0 | 6,683 |
| `PageAllocator` kind 1 | 324 MiB | 1,685 (23%) | 0 | 0 |
| `BucketAllocator` kind 1 | 134 MiB | 1,238 | 438 | 41 |
| `Default TaggedArena` kind 1 | 16 MiB | 437 | 394 | 8 |
| `HeapAllocator` kind 1 | 303 MiB | 284 | 103 | 180 |
| `BucketAllocator` kind 2 | 10 MiB | 175 | 43 | 87 |
| `HeapAllocator` kind 2 | 1.2 GiB | 5 | 2 | 28,449 |
| `PageAllocator` kind 2 | 1.4 GiB | 0 | 0 | 57,618 |
| `RTAllocator` | 322 MiB | 0 | 0 | 19,928 |

- Readbacks 666/s: `0x101a98` (particle stats ring) 248/s and `0x1a6d60` (XPR masks) 234/s, 72%.
- No pool spills; 54-60% of block allocations reuse a freed base.
- **Consequence:** no kind-1 owner is free of GPU use, so class placement by block owner (old
  phase 2) gains almost nothing. Classes must be decided per heap inside the arenas (the 0x12820
  memtype layer). The static data (2.6 GiB, kind 2) barely faults, as expected.

### Second run (1 Nov, Lab route `user/routes/tests.json`, all probes installed)

`SOTC_LAB=1 SOTC_LAB_ROUTE=user/routes/tests.json` plus the four probes and `SOTC_HOOK_XPR_PROBE=1`.
11 hooks installed; route done (210 frames; timings not comparable, the probes are heavy).

| Heap (memtype slot) | Bytes per frame | Allocs/s | Main callers |
|---|---|---|---|
| RenderSingleFrameGPU (16) | **13.8-15.1 MiB** | 66-81k | `0x8c0f7` (descriptor alloc 0x8c0d0) ~9-10 MiB; `0x117876` one ~4 MiB block per frame; `0x16ea5f` |
| CPUMemory family (1) | 78-82 MiB avg, 132 max | 8-88k | render-context chunks (scope) and C++ `new` |
| slot 22 (unnamed) | 0.8-1.2 MiB | 2.7-3.6k | `0xffeb0` (particle code range) |
| slot 21 (unnamed, PhysX ring) | 0.25 MiB | 85 | `0xa3fd06` |
| RenderSingleFrameCPU (17) | 0.18-0.26 MiB | 7-9k | `0x19c23d`/`0x19c290` (inside BuildSetDraws 0x19b760), `0xca17b` (XPR table 0xae40) |
| GPUMemory (5) | 0.4-4.7 MiB avg, 156 max | 0.1-1.2k | `0x8bf0b` (0x8be60, resource builds: bursts while streaming) |

- **Conflict settled:** the XPR masks are allocated by BuildSetDraws from RenderSingleFrameCPU
  (slot 17, kind 1). The GPU writes them into a CPU-kind heap, so that heap is GPU_WRITTEN.
- **Command buffers:** 16-20 render contexts per frame (DCB 5 MiB ×~7 and 3 MiB ×~9), from the
  CPU family through the render-context scope. 128-149 frames per 5 s, both rings flip once per
  frame at `sub_8c860`.
- **XPR blocks:** the four pointers at global object `0x123c3c5630` +0xa80..+0xa98 never changed:
  static allocations, not per-frame.
- XPR labels: 17-32% of mask reads happen before the GPU signalled (earlier logs: ~10%).
- The mask probe at BuildSetDraws entry saw only empty entries (the table is cleared each frame);
  the memtype callers answered the question instead.
- **Upload ring size:** about 15 MiB per frame for RenderSingleFrameGPU plus ~1 MiB for slot 22,
  ~450-500 MB/s at 30 FPS.

### Ring shadow (`SOTC_PC_RING=2`, 1 Nov, Lab route)

`gpu/sotc/pc_ring.cpp`. The per-frame arena is 3-4 blocks of 32 MiB (kind-2 Default
TaggedArena). The game submits ~33 times per frame (3,100-5,000 submits per 5 s), and the draw
thread binds each submit's data before the frame boundary at sub_8c860, so the ring's epoch must be
the submit (gnmdriver `PerformSubmit` and the compute doorbell), not the frame.

Per 5 s: 1,800-3,800 pages re-uploaded within one submit; of those, the bytes inside the range being
synchronized changed in 250-510 descriptor binds and 50-180 DMA sweeps, about 2-3 pages per frame
out of ~1,700 uploaded. With a declare-at-submit ring those pages would reach the GPU with older
bytes. It is not known whether the shaders read them: a V# can cover more than the shader uses,
and the bump allocator writes the next job's data right after the submit, inside such a range.
Re-uploading the arena on every bind (correct by construction) is the measured-worse
`SOTC_UNTRACK_AUTO` behaviour.

### Ring apply (`SOTC_PC_RING=1`, 1 Nov, one A/B pair on the Lab route)

Design changed from "never protect" to "unprotect at allocation": the `PC_RING_ALLOC` wrap at
0x12820 declares each 64 KiB of a new arena allocation (once per submit) through
`PageManager::DeclareCpuWrite` before the game writes it. Protection stays on, so a page bound and
rewritten within a submit still faults: no stale bytes are possible. (Never-protecting pages would
have needed protection changes on GPU-bit transitions that the region manager does not make: a page
could stay protected while marked unprotected, and the next CPU write would fault forever. The
arena holds ~6,700 GPU-written pages.)

| Per 5 s window | Default | Ring |
|---|---|---|
| Write faults, eboot code, /s | 2,734 / 2,902 / 3,307 | 1,723 / 2,104 / 2,523 |
| Write faults, host memcpy, /s | 2,721 / 2,797 / 3,032 | 1,059 / 1,443 / 1,418 |
| Write faults in the arena, /s | ~3,500 (earlier runs) | 530-840 |
| Draw thread busy, ms/frame | 35.1 / 33.3 / 33.3 / 35.8 | 37.0 / 36.0 / 33.7 |
| FPS (windows 2-4) | 28.5 / 30.0 / 30.0 | 27.0 / 27.8 / 29.7 |
| DMA sweeps, ms/s | 36.4 / 35.8 / 44.8 | 42.6 / 45.1 / 50.6 |
| Route p99 / max | 49.8 / 67.5 ms | 58.7 / 67.2 ms |

Faults fell 40-60%, but the draw thread stays saturated and frames did not improve; the faults run
on the game's threads, not on the draw thread. Costs seen: the 0x12820 wrap (~80k calls/s) and
more DMA sweep uploads (~+6 ms/s) from the pages declared ahead.

Three more alternating pairs (the windows were closed during the save load, so only the first 5 s
window of each was recorded): FPS default / ring 28.1 / 26.2, 27.6 / 26.9, 27.6 / 26.7 (first pair
28.5 / 27.0); eboot write faults 2,433 / 1,676, 2,270 / 1,484, 2,845 / 1,688. In 4 of 4 pairs the
ring costs ~1-1.5 FPS while it removes 30-40% of the eboot's write faults: the faults it removes
are not on the draw thread, which bounds the frame.

**Decision (user, 1 Nov): the ring is kept as part of the UMA → PC migration and optimized later;**
performance alone does not decide whether a migration step stays. It remains behind
`SOTC_PC_RING=1` until the migration is complete and the user's ending test passes; the defaults
change together. Optimizations to do:
- replace the 0x12820 wrap (~80k calls/s on the game threads) with a narrower hook: the descriptor
  allocator 0x8c0d0 and the few direct callers (0x117876, 0x16ea5f), or the arena heap's own
  refill, which runs once per 64 KiB instead of once per allocation;
- declare only the pages the allocation covers instead of whole 64 KiB granules, so the DMA sweeps
  do not upload pages nobody wrote (~+6 ms/s now);
- later, the real ring: arena pages never protected, uploaded once per submit. That needs
  protection changes on GPU-bit transitions in the region manager (section above). ReBAR is not
  required (user question, 1 Nov: older cards): the ring is ~15 MiB per frame (~45 MiB with three
  frames in flight). Paths, best first:
  1. guest memory imported with `VK_EXT_external_memory_host` (stage 1 of `SOTC_PC_MEMORY`
     already imports the backing view in 64 MiB chunks; tested coherent, 5.5 GB/s GPU reads on the
     RTX 2060 SUPER) and copied to VRAM by the GPU on the transfer queue at each submit: no CPU copy
     on the draw thread, no ReBAR (NVIDIA, AMD and Intel expose the extension on Windows);
  2. the classic 256 MiB BAR window (`DEVICE_LOCAL | HOST_VISIBLE`, present without ReBAR since
     Kepler / GCN): fits the ring, but the CPU copies into it;
  3. today's path (protection and faults).
  The earlier "host import slower than the CPU copy" result (`SOTC_HOST_IMPORT`, RTX 3060 Ti) was
  for large bulk uploads; for this ring the gain is the CPU copy leaving the draw thread.

### Parallel wave and matrix (1 Nov, `tools/pcmem_matrix.py`, 8 configs x 2 rounds)

Results in `user/pcmem/20261101-014856/summary.md`. 16 of 17 routes ran (the Lab fix in
`src/probe.c` / `engine_cmd.cpp` worked); the one failure is the known `SOTC_DROP_FAST` crash
(`bad PM4 packet` in a fast-dropped range), with only the observe-only pool probe on.

| Config | Took effect | Readbacks waited/s | FPS | Draw busy ms |
|---|---|---|---|---|
| base | - | 803 | 29.0 | 34.5 |
| rb (`SOTC_RB_CLASS` v2 + fixes) | yes | 565 | 29.1 | 34.4 |
| rt (`SOTC_RT_CLASS=1`) | yes (69k RT tracks skipped / 5 s) | 707 | 28.9 | 34.7 |
| ring (bump site 0x48440) | no: `arena allocs 0/s`, the site misses the arena | 740 | 28.9 | 34.6 |
| static (`SOTC_STATIC_CLASS=1`) | no: 0x8bf0e refused ("branch target inside the displaced span") | 713 | 28.5 | 35.2 |
| pool2 (`SOTC_PC_POOL=2`) | no: 0x19e00 refused (indirect branch) | 808 | 29.0 | 34.5 |
| commit (`SOTC_COMMIT_ON_DEMAND=1`) | unknown: no report line | 816 | 28.9 | 34.7 |
| all | rb + rt | 596 | 29.1 | 34.4 |

No configuration changed the frame: the draw thread bounds it (~34.5 ms of 33). `hooks_check`
checks signatures only; detour refusals appear at load, so an offline install check is needed.
Readback diagnosis: `docs/re-stutters/reports3/07-readback-diagnosis.md` (0x1a72e0 is the indirect
XPR consumer; "readbacks waited" counts the copy path, not blocked time).

### All switches in one run (1 Nov, after the hook fixes and `tools/re/hooks_install_check.py`)

`SOTC_PC_RING=1 SOTC_STATIC_CLASS=1 SOTC_RT_CLASS=1 SOTC_RB_OWNER=1 SOTC_RB_MATERIALIZE=1
SOTC_RB_CLASS=1 SOTC_PARTICLE_PREV=1 SOTC_PC_POOL=2 SOTC_COMMIT_ON_DEMAND=1` plus
`SOTC_HOOK_ALLOC_PROBE=1 SOTC_HOOK_FRAME_FLIP=1`. 12 hooks installed, route done, no crash.

- Ring (`PC_RING_HEAP`, wrap of the heap allocator 0x1d750): 98k hook calls/s, 68k arena allocs/s,
  15.0 MiB/frame declared (the expected ring size), arena write faults 652/s (was ~3,000-3,500).
- GPU_STATIC: 2,796 mesh ranges (404 MiB) and 12 texture ranges; 42k pages marked clean at
  allocation. RT: 67k render-target tracks skipped per 5 s. Commit on demand: confirmed on.
- Readbacks waited 596/s (base 803). Stats requests: 304 matches in the first window, then
  9-10k "no request" per window: the request bookkeeping breaks after the first ~10 s (bug).
- PcPool model: 852 matches, 2,908 mismatches, 2,578 unmodelled: its pool assumptions need fixing
  before phase 1. Freed Onion/Garlic VA is not returned to the free list (~740 per window), which
  answers open item 4.
- Against base: eboot write faults 3,396 -> 2,538/s, host memcpy faults 2,863 -> 1,400/s;
  FPS 29.0 -> 28.5, p99 45.9 -> 66.7 ms, max 54.8 -> 83.8 ms (one run), draw busy 34.5 -> 35.1 ms,
  DMA sweeps ~40 -> 51.8 ms/s.

### Full ring check, chunks and the corrected pool model (1 Nov, one combined run)

Everything on, with `SOTC_PC_RING=4` (full ring, verify only) and `SOTC_COMMIT_CHUNKS=1`. Route done,
no crash (nothing in the port touched memory after the game freed it).
- **PcPool model:** 6,140 matches, 112 mismatches, 0 unmodelled (was 852 / 2,908 / 2,578), with the
  1 MiB Onion/Garlic alignment, the free-run hit path, the spill rule and the VA -> phys map
  (`PC_POOL_MAP`, wrap of vmem_batch_map 0x1af80). Freed Onion/Garlic VA is consumed and unmapped,
  never returned (710 / 50 per window, as predicted): the game's own VA leak.
- **Chunks:** 9,600 MiB committed from boot and constant. The game maps the whole direct pool in one
  mapping at boot (`0x1040300000`, 8,124 MiB, already seen in FIRST-FRAME-PEAK phase 0b), so commit
  at map time (on demand) or per chunk while mapped saves nothing in this game. Next: open a chunk
  on the first touch of any of its pages (the exception handler), not at map time.
- **Full ring verify:** 16 FPS (draw thread 65 ms): the verify itself compared 28.4 M pages. 28,245
  divergent pages (~376 per frame), but the comparison is per whole page, the same overcount as the
  first shadow (a page holding this submit's bytes and the next one's): restrict it to the bind's
  byte range and sample the binds before deciding on `=3`.
- Write faults 1,600 / 1,635 per s (eboot / host), readbacks 270/s (the verify run's own slowdown
  lowers both; not comparable).

### Verify run and first safe run (1 Nov, `user/pcmem/20261101-032443`, `-032624`)

Verify (`--configs verify`): route done, no crash, no deadlock, no class trip.
- Chunks on first touch (`SOTC_COMMIT_CHUNKS=2`): 6,016-6,528 MiB committed (9,600 with chunks
  opened at map time): ~3 GB saved.
- Ring coverage (`SOTC_PC_RING=4`, byte-range verify, 1 bind in 16): of the bound arena bytes only
  0.6% / 1.6% / 22.9% / 56.7% are covered by an allocation of the last 1 / 4 / 16 / 64 submits;
  43.3% by none (data rewritten in place); 58 MB divergent at bind. The hot-page ring (`=5`) is not
  safe as is; the safe variant is protect-then-copy per submit (K = 1). The ring stays at v1.
- PcPool per path: flex-exact 29/29, flex-node 770/770 (no mismatch: next native candidate; the
  writer must handle the node it takes), primary-node 27 mismatches, fresh-plain 124.
- Heaps: memtype:CPUMemory 6,294 of 6,482 granules (405 MiB) look CPU_ONLY, GPUMemory 4,748 of 5,576;
  not published yet: the rule cannot see "the GPU reads it but nobody rewrites it" (clean protected
  data takes no fault). A bind feed per granule is needed first.

Safe run (`--configs safe`, with `SOTC_PC_RING=1 SOTC_PC_POOL_NATIVE=1`): route done, no crash.
- **Native PcPool: 27 allocations performed by the port, 27 verified, 0 kills.**
- Committed 6.8-7.2 GB; GPU_STATIC 46k pages clean at allocation; RT 61k tracks skipped / 5 s; ring v1
  arena faults 611/s.
- `SOTC_RB_CLASS=2` served 0: all 1,413 refused "too old" (the masks are read two frames after their
  write; the bound is 2 frames / 150 ms): mask windows need a 4-frame bound.
- Against base (matrix 20261101-014856): FPS 29.0 -> 26.7, p99 45.9 -> 83.9 ms, max 54.8 -> 100.7 ms,
  readbacks waited 803 -> 532/s, write faults 3,396 / 2,863 -> 2,068 / 1,354 per s, draw busy
  34.5 -> 37.7 ms. The overhead fell, the frame got worse; one or more pieces cost more than they
  save (suspects: the first-touch handler's write lock, the 90k/s ring hook, GPU_STATIC protecting
  from game threads). Next: one matrix run per piece to find it.

## 7. Phases (reordered after the measurement)

1. **Upload ring for the kind-2 per-frame heaps** (`SOTC_PC_RING=1`; old phase 3): built,
   measured and kept (section 6): faults fall 40-60%; it costs ~1-1.5 FPS for now, to be optimized.
2. **Explicit readbacks** (next: these run on the draw thread) at `0x8c1b0` (stats ring) and `0x1a6d20` (XPR) (old phase 5): 72% of
   readbacks.
3. **Per-heap classes** at the 0x12820 layer for the mixed kind-1 heaps (old phase 2, redesigned).
4. **`PcPool`** (old phase 1): replacing the pool layer, needed only to place per-heap classes in
   their own regions and commit memory on demand; it keeps the `POOL_FIX` guard (section 2).
5. GPU_STATIC tracked from allocation (old phase 4).

Each phase is off by default, has a `=2` verify mode, and is gated on benchmarks plus the user's
ending test.

## 8. Status and switches

State on 1 Nov 2026, after the matrix lots in `user/pcmem/` (`20261101-014856`, `-032443` verify,
`-032624` safe, `-032958` per-piece, `-035657`, `-035931`, `-040243`). `-035053` and `-035215` did not
finish a route. Status labels:

- **verified**: the port checks it against the game and no divergence or trip showed in the measured runs.
- **experimental**: works in the measured runs; not yet proven safe for the migration.
- **diagnostic**: counts or checks only; changes no data. A diagnostic can show a problem (ring=2, ring=4).
- **unsafe**: known to corrupt data or break the frame, or not checked.

Every switch is off by default. Lot numbers refer to the `user/pcmem/<lot>/summary.md` files. Base FPS
in lot `-032958` ranged 26.2 to 28.1 across its two rounds; differences smaller than that are noise.

| Switch | Values | What it does | Measured effect (1 Nov) | Status | Files |
|---|---|---|---|---|---|
| `SOTC_PC_RING` | 0 off (default); 1 apply; 2 shadow; 3 full ring; 4 full ring + verify; 5 hot-page ring | 1: arena pages of each new allocation are declared CPU-written once per submit, protection kept. 2: copies the first upload per submit and compares later binds, changes nothing. 3: records extents per submit, no protection. 4: 3 plus a byte-range verify of 1 bind in 16. 5: 4 plus protection on non-hot pages | 1: eboot write faults 2,964 to 1,936/s (-35%), host copies 2,634 to 1,007/s (-62%), FPS 27.2 to 23.4 in `-032958` (rounds 26.4 and 20.3, draw thread up to 120 ms); earlier pairs cost 1 to 1.5 FPS. 2: 2-3 divergent pages per frame. 4: 58 MB divergent at bind, 43% of bound arena bytes in no extent of the last 64 submits (`-032443`, 23.1 FPS) | 1 experimental; 2 diagnostic; 3 unsafe (no check, not measured alone); 4 diagnostic (its verify shows the ring is not safe as is); 5 unsafe | `gpu/sotc/pc_ring.cpp`, `gpu/sotc/pc_ring.h`, `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp` (submit epoch), `gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp`, `gpu/shadps4/video_core/page_manager.cpp`, `scripts/hooks.d/pcring.py` |
| `SOTC_STATIC_CLASS` | 0 off; 1 apply; 2 count | GPU_STATIC resource-build allocations (meshes, textures) are marked clean at allocation and not CPU-tracked. 2 only counts | 1: 2.4-3.2k mesh ranges (311-451 MiB) and 36-84k pages clean per 5 s window; FPS 28.3 against base 27.2 (within noise). 2: no trips | experimental | `gpu/sotc/static_class.cpp`, `gpu/sotc/static_class.h`, `scripts/hooks.d/staticclass.py`, `gpu/shadps4/video_core/buffer_cache/memory_tracker.h` |
| `SOTC_RT_CLASS` | 0 off; 1 apply; 2 count | render targets in RTAllocator blocks are not downloaded to guest memory and not CPU-tracked. 2 only counts | 1: 61-67k track skips per 5 s, 0 downloads; FPS 27.8 against base 27.2 (within noise) | experimental | `gpu/sotc/rt_class.cpp`, `gpu/sotc/rt_class.h`, `gpu/shadps4/video_core/texture_cache/texture_cache.cpp` |
| `SOTC_RB_CLASS` | 0 off; 1 copy ahead of CPU reads; 2 serve from the last copy | 1: copies the XPR visibility masks and particle stats as soon as the GPU has finished writing them, before the CPU reads them. 2: serves a guest read of a mask window from the last applied copy. Needs `SOTC_RB_OWNER=1`, `SOTC_RB_MATERIALIZE=1`, `SOTC_HOOK_ALLOC_PROBE=1`, `SOTC_PARTICLE_PREV=1` | 1: mask copies applied 195 of 213 per 5 s; readbacks waited 593 to 568/s against the heuristic (`-040243`, about 4%). 2: 17-34 reads served per 5 s, most refused as "not covered"; readbacks 545/s (`-032958`); vertex explosions seen in its runs | 1 experimental; 2 unsafe (vertex explosions; fix in progress) | `gpu/sotc/rb_class.cpp`, `gpu/sotc/rb_class.h`, `gpu/sotc/particle_prev.cpp`, `gpu/sotc/probe_xprmem.cpp`, `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` |
| `SOTC_PC_POOL` | 0 off; 2 verify | observes the game's block allocator (wrapper 0x19ae0) and its map flushes (0x1af80), predicts each allocation with the `PcPool` model and compares. Changes nothing | 6,022 of 6,173 allocations match (about 98%), 151 mismatches. By path: flex-exact 29/29, flex-node 770/770, primary-node 2,541/2,568, fresh-plain 2,682/2,806. Freed Onion/Garlic VA is never returned (the game's own leak, confirmed) | verified | `gpu/sotc/pc_pool.cpp`, `gpu/sotc/pc_pool.h`, `gpu/sotc/tests/pc_pool_test.cpp`, `scripts/hooks.d/pcpool.py` |
| `SOTC_PC_POOL_NATIVE` | 0 off; 1 on | the port performs the game's exact free-run hit allocation itself (`ApplyExactHit`). Every other call falls back to the game | 27 native allocations, 27 verified, 0 kills, 0 refused (`-032624`; `-032958` poolnative mean FPS 27.2, the base value). About 5,900 calls per window are ineligible and fall back | experimental (exact hits only) | `gpu/sotc/pc_pool_native.cpp`, `gpu/sotc/pc_pool_native.h`, `gpu/sotc/pc_pool.cpp`, `gpu/sotc/tests/pc_pool_native_test.cpp` |
| `SOTC_COMMIT_ON_DEMAND` | 0 off; 1 on | the backing section is reserved (SEC_RESERVE) and committed on demand | confirmed on in the all-switches run; committed size and FPS not measured alone | experimental | `src/runtime_memory.c`, `gpu/shadps4/core/libraries/videoout/driver.cpp` (report) |
| `SOTC_COMMIT_CHUNKS` | 0 off; 1 commit at map time; 2 commit on first touch | 1: chunks committed when the game maps them. 2: a chunk is committed on the first access to any of its pages (fault hook) | 1: 9,600 MiB committed from boot, unchanged (the game maps the whole direct pool at boot, so it saves nothing). 2: 6,016-6,528 MiB in verify (about 3 GB less), 6,336-6,720 MiB in lots, 7,168 MiB in the safe lot. No crash or deadlock. FPS not measured alone | experimental (verify clean) | `src/runtime_memory.c`, `src/probe.c` (fault hooks), `gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp` |
| `SOTC_STALE_CHECK` | unset or 0 off; N checks 1 bind in N; "on" = 16 | hashes each uploaded page (XXH3) and rehashes it at a later bind; reports a page that changed without a tracked write | 0 stale pages in the valid lots `-035657` and `-035931` (up to about 509k pages compared per session). Lot `-035053` printed "stale page" lines during a run that stalled (not investigated) | diagnostic | `gpu/sotc/stale_check.cpp`, `gpu/sotc/stale_check.h`, `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` |
| `SOTC_CLASS_TRACK` | 0 off; 1 apply; 2 shadow | per-pool CPU-only ranges from the allocator probe. 2 counts GPU-marked pages inside them. 1 publishes them as unprotected (pools in `SOTC_CLASS_POOLS`, default `PageAllocator`) | 2: 0 trips in every lot that ran it. 1: not measured in a matrix lot | 2 diagnostic; 1 experimental (not measured) | `gpu/sotc/mem_classes.cpp`, `gpu/sotc/mem_classes.h`, `gpu/sotc/probe_alloc.cpp`, `gpu/shadps4/video_core/buffer_cache/region_manager.h` |
| `SOTC_MEMTYPE_CLASSES` (with `SOTC_HOOK_MEMTYPE_PROBE=1` and `SOTC_CLASS_TRACK`) | 0 off; 1 on | per-heap classes of 64 KiB granules from the memtype probe, named `memtype:<heap>` | lot `-032443`: CPUMemory 6,294 of 6,482 granules (405 MiB) look CPU_ONLY; GPUMemory 4,748 of 5,576. Nothing is published yet (0 MiB) | diagnostic | `gpu/sotc/probe_memtype.cpp`, `gpu/sotc/mem_classes.cpp`, `scripts/hooks.d/memtype.py` |
| `SOTC_HOOK_ALLOC_PROBE`, `SOTC_HOOK_MEMTYPE_PROBE`, `SOTC_HOOK_XPR_PROBE`, `SOTC_HOOK_FRAME_FLIP` | 0 off; 1 on | observe-only probes: allocator blocks, per-heap allocations, XPR mask allocations and consumers, frame flips. Needed by the switches above | cost: the allocator probe alone shows no clear FPS change in `-032958` (28.0 against 27.2); the memtype probe conflicts with `SOTC_PC_RING` on the same hook site | diagnostic | `gpu/sotc/probe_alloc.cpp`, `gpu/sotc/probe_memtype.cpp`, `gpu/sotc/probe_xprmem.cpp`, `scripts/hooks.d/alloc.py`, `scripts/hooks.d/memtype.py`, `scripts/hooks.d/xprmem.py` |

- `SOTC_PC_RING=6` (protect then copy): superseded by mode 8 (host arena binds). Not maintained; kept
  only for comparison.
- `SOTC_PC_RING=7` (protect then copy, verify): superseded by mode 8. Its verify is replaced by
  `SOTC_PC_RING8_VERIFY=1` (sampled bind hashes at bind and at the next submit start).
- `SOTC_PC_RING=8` (host arena binds, the ring mode under development): the arena blocks are imported
  and served in place, and GPU writes into them need an ordering rule. An EOP label is deferred to the
  GPU when a served write came since the previous label, or an earlier deferred label is outstanding
  (`liverpool.cpp`, EventWriteEop; design note in `gpu/sotc/pc_ring.h`). ReleaseMem and EventWriteEos
  labels are not covered yet.

**Tools.** `tools/pcmem_matrix.py` runs named configurations (`SWITCHES` in the file) in interleaved
rounds on the Lab test route and writes `summary.md` after each run. A run counts only when its log has
`SOTC LAB ROUTE done:`. `tools/re/hooks_install_check.py` predicts offline whether the detour engine will
refuse a hook, using the same rules as `gpu/sotc/cpu_patches.cpp`. Refusals used to appear only at load.

**Known problems.**

- A 10 s freeze was seen once with `SOTC_PC_RING=1`. Not reproduced, not explained.
- `SOTC_RB_CLASS=2` produced vertex explosions. Keep it off.
- Lots `-035053` and `-035215` (with `SOTC_STALE_CHECK=16`) ended without a route exit, with frames of
  600-800 ms; the cause is not known.
- Lots `-035657` and `-035931` ran at about 13 FPS, including the base configuration. Do not compare them
  with the 27-28 FPS lots.
- `SOTC_DROP_FAST` (not part of this model) still has the crash recorded in the CHANGELOG (1 in 4 sessions
  in its first test). It stays off by default.
- No switch changes the frame today: the draw thread is at its limit (about 34.5 ms per frame against a
  33 ms target).

**Lab fix (1 Nov).** At boot the port claims a 64 KiB block at the end of its 256 MiB reservation below
2 GiB (`bb_low_block`, `src/probe.c`), before anything else. The Lab's live commands and free camera use
that block; a later scan for free space below 2 GiB had failed in about half of the sessions
(`docs/sotc/LAB-FAILURE-REPORT.md`). The camera no longer fails.
