# Chunk, zero-copy and DMA writers: closing the writes4 OPEN items (specialist: writes5)

Status: partial. One code change (span tables in `native_hooks.cpp`, no new hook pattern, so no new
`hooks_install_check` entry). Nothing committed. Nothing built with ninja or build.sh, the game was not run.
Decisions are static (Ghidra decompilation in `out/re/decomp`, call graph from `eboot_re.py`); assembly was used
only for the signatures already in `native_writes.py`. Each row says how sure the decision is.

Base: the worktree predated `gpu/sotc/native/`, so `native.h`, `native.cpp`, `native_hooks.cpp` and
`gpu/sotc/hooks.h` were copied from the main tree first; `scripts/hooks.d/native_writes.py` was copied and left
unchanged. The only edited file is `gpu/sotc/native/native_hooks.cpp` (span tables).

## 1. Item 1: the 0x8c0d0 callers outside kChunkCallers

Scope correction: writes4 counted 68 chunk-out callers among the RSF16 rows. `eboot_re.py callers 8c0d0` gives 153
caller functions in total: 30 are fully inside kChunkCallers, **123 are outside (193 call sites)**. The 0x8c0d0
allocator serves every heap user, not only RSF16, so all 123 were classified.

Method (scratch scripts, not in the repo): for each caller, the 0x8c0d0 result variable and its aliases were
tracked through the decompilation. Rules, in order of strength:
- DIRECT: the chunk (or an alias) is an argument of a GPU consumer: bind `0x85ecd0` / `0x85e5d0`, PM4 writers
  `0x16edc0` / `0x16ee00` / `0x16ee80` / `0x16eb00` / `0x16ed40`, V# encoders `0x865410` / `0x8655d0` / `0x865600` /
  `0x865500`, zero-copy `0x8be60` (flag 0x800), or DMA `0x16eb90`.
- VIA: the chunk is passed to a callee that reaches one of those consumers within two calls.
- HANDLE: the 0x8c0d0 block is stored into the handle that `0x8be60` creates in the same function (the block is the
  resource's storage).
- SLOT: the chunk is stored into an object slot (`param_N + off`) and the same function calls a bind or PM4 writer.
  Most constant-block builders (view, instance and sun-shadow families) do this at `+0xb0`; the bind record of
  `sub_afcc0` copies `*(param_2+0xb0)` into `puVar6[1]` and binds it (`0xafcc0` line 80-85), so `+0xb0` is a
  GPU-read slot.
- SLOT-family (inferred): a `+0xb0` store without a consumer in its own body. Included because the slot is the
  bind source of its family (above). Weaker than the other rows.

Result: **79 functions added** to `kChunkCallers` (comment table in `native_hooks.cpp`, one line each with the rule):
- 42 by DIRECT, VIA, HANDLE or SLOT with a consumer in the body (for example `sub_a78a0`, `sub_b50c0`, `sub_10c8c0`,
  `sub_1925a0`, `sub_1ad940`, `sub_5de0b0` (DIRECT ZC), `sub_5e3b50` (HANDLE), `Frame::InsertWaitFlipDone` 0x118470).
- 37 by SLOT-family (`+0xb0` store, consumer outside the body): the functions 0xa20b0, 0xa69d0, 0xa8f80, 0xa92a0,
  0xa98a0, 0xa9e10, 0xaea10, 0xaf610, 0xb0100, 0xb0570, 0xb5b10, 0xb6760, 0xb8c60, 0xb9210, 0xba870, 0xbae70, 0xbb310,
  0xbbbf0, 0xd79c0, 0xe6350, 0xe84b0, 0xe8e80, 0xe9670, 0x1097d0, 0x10c4e0, 0x10d890, 0x10df30, 0x1642d0, 0x17cac0,
  0x17e000, 0x17e9c0, 0x17f790, 0x1802e0, 0x18a590, 0x1a28c0, 0x1a42e0, 0x5d5820 (exact list in the `native_hooks.cpp`
  comment, which uses the same addresses).

Not added: 44 functions.
- 36 with no escape: the chunk is written by the function and never passed to a consumer within two calls (CPU-only,
  for example `sub_10cdb0`, `sub_1239c0`, `sub_1646c0`, `sub_1907f0`, `sub_5d5ec0`). Decision: CPU-only.
- 8 with other slots (`+0x90`, `+0x98`, `+0xa8`, `+0xb8`, `+0x168`, `+0x890`): `sub_a6f60`, `sub_a88a0` (`+0x168`),
  `sub_add50` (`+0xa8`), `sub_ae310` (`+0x890`, `+0x90`), `sub_db390` (`+0x90`, `+0xa8`), `ParticleRenderer::UpdateSimulation`
  0x1051f0 (`+0xb8`), `sub_10b0a0` (`+0x90`, `+0xa8`), `sub_1899f0` (`+0x98`). Open: no consumer identified; the
  `+0x98` slot of `sub_1893e0` is included, so `sub_1899f0` is the same family and is the first candidate to add.

Confidence: DIRECT and HANDLE are direct evidence. VIA is a two-call reachability, not a dataflow. SLOT with a
body consumer is a strong inference. SLOT-family is inference from the slot number. The open check is the bind
record: a counter on the `0x85ecd0` entry that compares the record word0 with the chunk range would confirm the
37 SLOT-family rows in one window. Cost: each added row adds its chunk bytes to the per-frame upload; the chunk
class MiB in `SOTC NATIVE WRITES PF ... chunk` shows the change.

## 2. Item 2: zero-copy creators outside kZeroCopyCallers

| Creator | Callers (index) | Writers vs creator | Decision |
|---|---|---|---|
| 0xafcc0 (ResolveSunShadows, 0x10-byte backing) | sub_afca0 (0xafca9); sub_afca0 itself has no direct caller in the index | writes at lines 51-53 precede the 0x804 creator at line 54 | **added** `{0xafcc0, 0xb0100}` (PerFrame at entry). Per-frame status is inferred from the pass name (ResolveSunShadows, the same pass as 0xb0100) and is not confirmed by a caller; PerFrame is safe either way. |
| 0xb0100 (twin, 0x10-byte backing) | sub_afca0 (0xafcb1) | writes at lines 44-50 precede the creator at line 51 | **added** `{0xb0100, 0xb056e}`. |
| 0x1af960 (two 0x804 creators, n*0x80 and n*0x20) | none in index (indirect) | the loops at lines 24-30 (`FUN_00122fc0` 0x80 per entry, `+0x18+idx*0x20 = 1.0f`) precede both creators (lines 35-39) | **added** `{0x1af960, 0x1afc33}`. Frequency unknown (indirect), PerFrame is safe either way. |
| 0x11dbe0 BPXPR_IndexCullObjects, and its twin 0xceae0 | 0xceae0 calls 0x11dbe0 (BPXPR_IndexCullJob, sub_cdb00) | the two 0x804 backings (`0xab98` count 10000 x 0x14, `0xaba0` 0xe00000) are GPU outputs of the cull dispatch (`0x92b50`) and the clear; no CPU writer | **not added**: GPU-written. See the gap below. |
| 0xfedc0 (two 0x804 creators, 0x70 and 0x24 per element) | none in index (indirect) | the body only allocates (`0x12820`) and creates; no CPU write into the backings in the body | **open** (writers not found). Likely the particle buffers that 0xffe60 fills per frame; if so, the 0xffe60 entry announce already covers the frame writes. Check: count `0x8be60` entries at 0xfedc0 per window, and trace the writer of `param_1+0x28` / `+0x50`. |
| 0xceae0 | none in index | twin of 0x11dbe0 | see 0x11dbe0. |
| 0xfffa0 | sub_102590 (0x1027cd) | not a 0x804 creator (descriptor, flags 2 and 4, see item 3) | item 3. |

Gap found (not hooked): in `BPXPR_IndexCullObjects` (0x11dbe0, line 107-111), the 0x14-byte block `pauVar9`
(`0x12820(0x14, RSF16)`) is written by the CPU (lines 109-110) and passed to the dispatch `0x92b50` as its
user-data block (argument 4), per frame (the cull runs every frame). It is an RSF16 block, not a chunk and not a
zero-copy backing, so no existing hook covers it. Candidate: an Entry hook on `0x92b50` (BPGnm_DispatchWithLabelWait,
inferred name) announcing argument 4 (14 bytes) PerFrame. Not implemented: `0x92b50` has 7 direct callers and the
argument layout (and whether the GPU reads the block at all) was not checked against the dispatch encoder.
Check: the verify mode counts unannounced writes; a nonzero count for the block would confirm it.

## 3. Item 3: 0xfffa0 (site 0x1001fa, the particle V# bases)

- Per frame. Chain from the index: `GameFrame_Update` (site 0x25b04f) calls `RenderThread_Kick` (0xbcefa), which
  calls `sub_fc470`, which calls `sub_102590` (0xfc6bb), which calls `0xfffa0` (0x1027cd).
- The V# bases are a CPUMemory block (`0x12820(count * [param+0x6c], 1, 4, 0)`, writes4 5.2). The CPU writes it every
  frame through the particle update, so the block needs a PerFrame announce.
- Not implemented: the hook point needs the argument mapping of `0xfffa0` (the block address and count are not in
  the signature) and the writer's range. Next step: decompile `0xfffa0` and `sub_102590` for the block argument and
  add a PerFrame Entry hook at `0xfffa0`, or a Site after the writer of the block.

## 4. Item 4: 0x117930 (live copy or alias of 0x117160?)

- Not an alias. `0x117930` is a separate 1982-byte function (`out/re/decomp/0011/00117930.c`). Its material block is
  `0x12820(DAT_02ed2100, RSF16, 0x10)` (line 281), filled by `memcpy(uVar4, DAT_02ed2108, DAT_02ed2100)` (line 282)
  from the same source global as the 0x117160 copy, then sent to the GPU by `FUN_000928f0(param_2, DAT_02ed2110,
  size >> 2, uVar4, size >> 2)` (line 283).
- **Correction to writes4:** the DMA of this path is `0x928f0` (the hooked funnel), not `0x16eb90` at line 282.
- Liveness: its only direct caller is `sub_117140` (site 0x11715a), which is called only by `sub_d0710` (0xd0720);
  `sub_d0710` has no direct callers in the index (indirect entry). Live status not decided.
- Not hooked. If it is live, add a Site hook after the memcpy call at line 282 (before the `0x928f0` call), PerFrame,
  the same pattern as NATIVE_MAT_COPY (`rbx`-style destination and a size word), with a signature checked by
  `eboot_re.py sig`. Check: a counter at the entry of `0x117930` per window (one hit per frame means live).

## 5. Item 5: 0x97a50 (7 sites) and 0x112db0 (one-time CPU writes)

- `0x97a50` (resource creator, `*puVar5` heap): direct callers `sub_8d6c0` (0x8d6e2), `sub_8d6f0` (0x8d712),
  `VideoOut_OpenSetFlipRate` (0x95c86, 0x95d29), `sub_96ce0` (4 sites), `sub_96f20` (0x96f3e). These are setup and
  flip-rate init paths, not the frame loop: **Persistent**. Not hooked here (the written ranges are the resource
  blocks, which need the per-caller block list).
- `0x112db0` (CPU1 0x800000 block; descriptor flags 2 and 4 at `param_1+0x44022`, `+0x44027`): the only caller is
  `AreaLoad_ShaderListAndAssets` (site 0x256c70), a level-load path: **Persistent**, announced once at the return of
  `AreaLoad_ShaderListAndAssets`' call (one Site after 0x256c70). Not hooked: the descriptor ranges need the offsets of
  the flag-2 and flag-4 descriptors, which were not extracted.
- The memsets `0x1264d0` and `0x1abba0` were not checked in this pass (open).

## 6. Item 6: the RunFunctionAsRenderJob DMA 0x16efa0 -> 0x16eb90

- `0x16eb90` is not a plain copy: it builds two V# descriptors from its (base, size) arguments
  (`0x865410` for both, `0x8655d0` for the size word) and binds them (`0x85e8e0`, `0x85e9a0`, `0x85e820`), then runs
  the DMA (`0x883870`). **Both the source and the destination are GPU-read as descriptors.**
- Callers of `0x16efa0`: `sub_da220` (4 sites, sources `this+0xb8`, `+0x130`, `+0x158`, `+0x180`, V# words in the
  object) and `sub_1a4b70` (2 sites, `this+0x1d8+k` and `this+0x200+k`).
- For `sub_1a4b70` the first source is the instance block at `this+0x1d8`, the block that `NATIVE_INST_121ED0`
  already announces PerFrame (writes3). That source is covered if the 0x121ed0 write precedes the DMA in the frame
  (it does: the instance writers run before the render job). The `+0x200` source was not identified.
- For `sub_da220` the source writer was not found. The destination (`param_4`) is a GPU block: a CPU announce of it
  would upload a stale copy and clobber GPU results.
- **Decision: no hook at 0x16eb90.** It would announce GPU-written destinations and GPU-side sources. A safe hook
  needs a source-heap filter, which the decompilation does not expose (the heap id is not in the V# words). The
  `sub_da220` sources are open.

## 7. Verify checks (not run)

1. `SOTC_NATIVE=2`, one attract window: the `chunk` class MiB with the 79 added rows, and the `unannounced` count in
   the chunk-out range. An `unannounced` count in an added range would show a missing consumer.
2. A `0x85ecd0` entry counter comparing the bind record word0 with the chunk range: confirms the 37 SLOT-family rows.
3. Entry counters at `0xfedc0`, `0x117930`, `0xfffa0` (per window): frequency for items 2, 3 and 4.
4. `0x92b50` argument 4: whether the GPU reads the 14-byte block (the dispatch encoder).
5. `hooks_install_check.py` was not needed: no pattern was added or changed (`native_writes.py` is unchanged).

## 8. Files

- Changed (uncommitted, worktree `C:\Users\ptr\Documents\sotc-pc\.claude\worktrees\agent-a78d969177e05560a`):
  `gpu/sotc/native/native_hooks.cpp` (`kZeroCopyCallers`: 3 spans; `kChunkCallers`: 79 spans, comment table),
  `docs/re-stutters/native/impl-writes5.md` (this file).
- Copied from the main tree, unchanged: `gpu/sotc/native/native.h`, `native.cpp`, `gpu/sotc/hooks.h`,
  `scripts/hooks.d/native_writes.py`.
- Syntax: `native_hooks.cpp` passes `clang++ -fsyntax-only -Wall -Wextra -std=gnu++23` with the project include list
  of `out/gpu/compile_commands.json` (worktree `gpu/shim` and `gpu/shadps4` first), no warnings.
- Scratch (not in the repo): the classification scripts in the session scratchpad.
