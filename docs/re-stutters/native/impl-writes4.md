# Direct callers of the allocator 0x12820 (specialist: writes4)

Status: partial. One hook added and checked (NATIVE_BUILDXPR_ZC). The classification below is static (decompilation
of `out/re/decomp`, `eboot_re.py callers 12820`), not a per-site dataflow. The open items in section 5 are the
work that remains. Nothing committed. The game was not run, and nothing was built with ninja or build.sh.

## 1. Scope and method

- 226 direct sites (the caller list of `0x12820`), 225 caller functions after grouping (`table2` in the scratchpad,
  one row per caller function). Callers already behind a hooked funnel (0x8c0d0, 0x8be60, 0x8c120, 0x928f0) are
  counted as covered only when their own range is in the hook's caller filter (see section 3).
- Heap per site: the slot argument of the call. Literal `1` = CPUMemory; `DAT_02dd9f80` = CPUMemory; `DAT_02dd9f84`
  = GPUMemory (5); `DAT_02dd9f88` / `DAT_02dd9f8c` = GameSingleFrame (11) / GameDoubleFrame (12); `DAT_02ed1f54` =
  RenderSingleFrameGPU (16); `DAT_02ed1f58` = RenderSingleFrameCPU (17); `DAT_02ed1f50` = CPU_RenderToGPUToGame (15);
  `DAT_02ed1f5c` = MaterialParamHeap (18); `*param_N` / `*puVar5` / `*param_4` = a heap id read from an object (the
  caller's heap: particle slot 22, resource slot 20/5, others).
- GPU consumer in the caller's body: zero-copy or descriptor creators (`0x8be60` with type 0x1b/0x1c and flags 0x800,
  0x804, 0x2, 0x4), V# encoders (`0x865410`, `0x8655d0`, `0x865600`, `0x865500`), DMA (`0x16eb90`) and the command
  emitters (`0x16edc0`, `0x16ee00`, `0x16ee80`, `0x16eb00`, `0x16ed40`) that write PM4 packets.

## 2. Classification (static)

| Class | Meaning | Sites (functions) |
|---|---|---|
| CPU-1 | CPUMemory (literal 1, or the CPU heap of a dynamic id), no GPU consumer in the body | about 90 functions: string and object allocations, the `0x10xxx`-`0x17xxx` family, `0x11xxxx` setup, `0x3xxxx`-`0x5xxxx` helpers. No announce. |
| NODE | RenderSingleFrameGPU (16) small objects (0x10 to 0x28 bytes), no GPU consumer in the body | about 25 functions (`0xa1e10`, `0xab430`, `0xb50c0`... in the table only when they also call 0x8c0d0 they are CHUNK). Render-thread objects: not GPU-read by static evidence. |
| CHUNK-in | calls `0x8c0d0` and the function is in the `kChunkCallers` filter (native_hooks.cpp) | 13 functions. Announced PerFrame through NATIVE_CHUNK (existing). |
| CHUNK-out | calls `0x8c0d0`, function NOT in `kChunkCallers` | **68 functions, open** (section 5.1). |
| ZC-in | `0x8be60` zero-copy (flags 0x800/0x804) and the caller is in `kZeroCopyCallers` | `0xffe60`, `0x19b760`, `0x19c1a0`, `0x19c7e0`, `0x94fb0`, `0x198bb0`. Announced PerFrame (existing). |
| ZC-out | `0x8be60` zero-copy or descriptor from a block of this function, caller NOT in the filter | `0xfedc0` (0x804, `*param_2`), `0xafcc0` and `0xb0100` (0x804, 0x10 elem, RSF16 / CPU_RenderToGPUToGame), `0x1af960` (0x804, GPU5), `0x11dbe0` / `0xceae0` (0x804, RSF16 0x14 and 0x800000), `0xfffa0` (descriptor flags 2 and 4 on a CPUMemory block). **Open** (section 5.2). |
| BUILDXPR | `0x1b32f0` zero-copy V# backing (0x800, type 0x1b, elem 0xb0) | 1 function. **Hooked now** (section 4). |
| GPU-heap | GPUMemory (5) or a resource heap (20/22) with no GPU consumer in the body; the pointer leaves the function | `0x1af480`, `0x1afca0`, `0x1b0440`, `0x97a50` (7 sites, heap by argument), `0x96680` (texture, hooked by TEX_COPY/MIP hooks), `0xa3fbc0`, `0xa40970`, `0xb0ced0`, `0x8f6f0` (render init). **Open** (section 5.3). |
| DYN | heap id read from an object (`*param_2`, `*puVar5`, `*param_4`, `param_3`) | `0xfedc0`, `0xffe60`, `0x97a50`, `0x96680`, `0x16e990` (GPU block), `0x95320`, `0x917c0`. Resolved per row where the table says so. |
| SETUP | render init and descriptor-table creators (CPU1/GPU5 backing, GPU read later through a V#) | `0x8f6f0`, `0x8c370`, `0xf5cd0` (MaterialParam, hooked MATPARAM), `0x112db0` (build-time memset, see 5.4). |
| COVERED-OTHER | block copied by an existing hook | `0x117160` (material copy, NATIVE_MAT_COPY), `0x96680` (NATIVE_TEX_COPY, NATIVE_MIP_COPY), `0x1b32f0` (now NATIVE_BUILDXPR_ZC). |

Counts (functions, from the generated table): 114 CPU-only or no-consumer rows, 93 RSF16 rows of which 83 call 0x8c0d0
(13 in the chunk filter, 68 out), 15 rows with a zero-copy creator. Named callers whose decomp file the index does not
hold (`CRenderer::UpdateTextureStreamingGPUData`, `Frame::InsertWaitFlipDone`, `BPXPR_IndexCullObjects`,
`BPXPR_BuildSetDraws`, `TextureManager::RegisterCvars`, the `ResourceFactory*` family, `ParticleRenderer_TranslucentHelper`)
were not classified row by row: their sites are inside the functions listed above (e.g. `0x19b836` in `0x19b760`
family, `0x14723c` in the resource loader). This is a gap in the classification, not a finding.

## 3. Existing announces that these sites rely on

- NATIVE_CHUNK (0x8c0d0 wrap): only the callers in `kChunkCallers`. The 68 CHUNK-out functions are not announced.
- NATIVE_ZC_CREATE (0x8be60 entry): only the callers in `kZeroCopyCallers` (`0xffe60`, `0x19b760`, `0x19c1a0`,
  `0x19c7e0`, `0x94fb0`, `0x198bb0`). Other zero-copy creators are not announced.
- NATIVE_MAT_COPY (0x117160), NATIVE_TEX_COPY / TEX_TAIL / MIP_COPY (0x96680, 0x98ae0 path), NATIVE_VIEW_BLOCK (0x8c120),
  NATIVE_INST_18AA20 (0x18aa20 entry, PerFrame, also called from `0x1b32f0`).

## 4. Hook added and checked

### NATIVE_BUILDXPR_ZC (site 0x1b3f97, Persistent)

- Function: BuildXPR `0x1b32f0`, a model-section build. Its loop creates one zero-copy buffer V# per instance
  (`0x8be60(&local_90, 0x1b, 0xb0, 1, 0x800, lVar25, 0)`, flags 0x800), with `lVar25 = object+0x18`, advanced by 0xb0
  per instance (`0x1b32f0` line 397), and writes the 0xb0 bytes after each call (the writes follow the call, so an
  announce at the creator's entry would be early, and the caller is not in `kZeroCopyCallers`). `object+0x20` =
  `object+0x18 + count * 0xb0`.
- Hook: a **site** at the only return, `mov rax, r12` at 0x1b3f97 (`r12` is the returned object). The signature is the
  tool's `sig 1b3f97` output (28 bytes, unique). Handler `BuildXprExit` (native_hooks.cpp) reads `object = regs.r12` and
  announces `[object+0x18, object+0x20)` with `Persistent` (the bytes are final at the return).
- Why not Wrap: the first version used mode `wrap` at the prologue. `hooks_install_check.py` refused it: "Wrap: stack
  operand at an unknown stack depth" (the prologue realigns rsp with `and rsp, -32`). Site mode accepts it (displaced 7
  bytes: `mov rax,r12` and `lea rsp,[rbp-0x28]`).
- Per frame or not: the function is reached only from `0x19aad0` (StaticModelSection::Init), which `0x271b10` runs as a
  job (its `local_118 = FUN_00271b10` entry), which `0x271570` creates, from `0x24a0f0` (called by `0x237630` and
  `0x274040`). That is a section-creation chain, not the frame loop. Evidence level: static, from the call graph. The
  top of the chain was not reached (the `callers` walk stops at `0x24a0f0`). Runtime confirmation: count `NATIVE_BUILDXPR_ZC`
  per window with the verify run (section 6). If it runs per frame, Persistent is wrong: the verify mode would report
  the per-frame rewrites as unannounced writes.
- Verified: `hooks_install_check.py --hooks-dir <worktree>/scripts/hooks.d`: `NATIVE_BUILDXPR_ZC 0x1b3f97 accepted mode=site
  displaced=7`; 47/48 accepted; the one refusal is the pre-existing `RENDER_JOB_RUN 0xc12a0` (unresolved indirect branch).
  No CONFLICT line. `hooks_check.py`: 48/48 resolve to one site. Syntax: `native_hooks.cpp` passes `clang++ -fsyntax-only
  -Wall -Wextra` with the project flags, no warnings.

## 5. What remains (open, in order of expected size)

### 5.1 CHUNK-out: 68 functions calling 0x8c0d0 outside the chunk filter
RSF16 chunks from functions such as `0xa1e10`, `0xa75b0`-`0xa9e10`, `0xb0570`-`0xbbbf0`, `0xd79c0`-`0xe9670`,
`0x10bfd0`-`0x10df30`, `0x17cac0`-`0x18a590`, `0x1a28c0`-`0x1a5000`, `0x5d5820`, `0x5d5ec0` (full list: section 2).
Whether their chunks are GPU-read (V#/T# or DCB data) needs a per-function read: memcpy or stores into the chunk, and
whether the chunk pointer reaches a descriptor or a DCB. The earlier walk (heaps.md 7.5) counted the 27 memcpy callers
only. Largest open item by sites.

### 5.2 ZC-out: zero-copy creators outside `kZeroCopyCallers`
- `0xfedc0` (sites 0xfedeb, 0xfee3a): 0x804 creators on `*param_2` blocks (particle sizes 0x70 and 0x24 per element).
  Likely the same class as `0xffe60`, which is announced PerFrame. Needs: the caller of `0xfedc0` (per frame?) and
  whether the writers run after the call. Candidate: add `{0xfedc0, 0xfee6x}` to kZeroCopyCallers only after that check.
- `0xafcc0` (0xafde5, 0xaff35) and `0xb0100` (0xb0222, 0xb0395): 0x804 creators on 0x10-byte RSF16 / CPU_RenderToGPUToGame
  objects, count 1. Small GPU-read buffers. Writers not read.
- `0x1af960` (sites 0x1af9a1-0x1afb16): 0x804 creators on GPU5 blocks of `(param_2[1]-*param_2) * k` bytes. Writers not read.
- `0x11dbe0` / `0xceae0` (0x804 creators with count 10000 and 0xe00000): the backings come from `0x12820` in the same
  function (RSF16 0x14, and a CPU1/`*(lVar12+0xab98+lVar8)` block). Writers not read. `0xceae0` is the twin of `0x11dbe0`.
- `0xfffa0` (site 0x1001fa): the V# bases of the particle descriptors (flags 2 and 4, no storage of their own) are a
  CPUMemory block (`0x12820(iVar16 * [param+0x6c], 1, 4, 0)`). The GPU reads a CPUMemory block: heaps.md 2.1 says that
  such a block must be host-shared, and writes are the particle simulation (per frame?). This one is the clearest case of
  a CPU heap block that the GPU reads.

### 5.3 GPU-heap pointers that leave the function
- `0x97a50` (resource creator, 7 sites, `*puVar5` heap, 21 callers): persistent resource data (meshes, textures) at level
  load. The block pointers are stored in the resource object (`plVar12[0x14]`, `[0x16]`, `lVar16`, `lVar17`, `lVar18`). A
  writer for each block is needed (`0x87ec70`, `0x87efd0`, `0x867990` are the data helpers, not read). Persistent.
- `0x16e990` (the object of RunFunctionAsRenderJob's f&4 path, `0x906d0`): two blocks, CPU1 (site 0x16ea19, size `param_2`)
  and RenderSingleFrameGPU (site 0x16ea5a, size `param_3`). Answer to the brief's item 4: the **GPU block is not written
  by the CPU in the creation path** (`0x906d0`, `0x16e990`, `0x85d0c0` and `0x85e510` only set pointers and sizes). The
  job writes its commands through the stream helpers (`0x865ea0`, `0x16edc0`), and a GPU DMA (`0x16efa0` ->
  `0x16eb90`, copy of a V# source into the destination) fills the GPU block. Consequence: the CPU block is the likely DMA
  source (CPU-written, GPU-read, per frame), and the GPU block is GPU-written. Not verified which of the two `0x16efa0`
  sources is the object block. Open: the right place for an announce is the DMA call (`0x16eb90` entry, source range
  `[param_2, param_2 + param_3 * 4)`, PerFrame, one hook for all DMA sources), but the DMA also copies GPU-side sources
  (MatParam block, VirtualMemoryGPU), so the funnel hook needs a source filter first.
- `0x1af480`, `0x1afca0`, `0x1b0440` (GPU5, no consumer in the body): pointers returned to the callers of `0x1af480` and
  `0x1b0440`. Not followed.
- `0x117930`: a second 4 MiB RSF16 material block (`0x12820(DAT_02ed2100, DAT_02ed1f54, 0x10, 0)`, then `0x16eb90` at line
  282, the same copy/DMA shape as `0x117160`), but it is not in the callers index (its only caller in the index is not
  listed). It calls `0x928f0` (hooked funnel, not announced for this path). Open: confirm whether `0x117930` is a live
  function or an alias of `0x117160`; if live, its copy needs the MAT_COPY pattern with its own rel32 (the DMA call
  `e8 ... 16eb90`).

### 5.4 Build-time CPU writes into CPUMemory blocks
`0x112db0` (CPU1 0x800000 block, descriptors with flags 2 and 4 at `param_1+0x44022`, `+0x44027`) and the memsets of
the descriptor clears (writes3, `0x1264d0`, `0x112db0`, `0x1abba0`) are one-time writes into a block the GPU reads through
a V#. Not announced. If the GPU reads them, Persistent announces are needed at build end (one hook at a funnel return).
Open.

### 5.5 Not verified at all
- Mode 2 (verify) run: not done. Expected new counters: `view`/`inst`/`zc` for the BuildXPR range; a nonzero
  `unannounced` on `0x1b32f0` would mean the Persistent class is wrong for it.
- The `RENDER_JOB_RUN` refusal (0xc12a0) is outside this agent's hooks; its owner must resolve the indirect branch.

## 6. Verify checks to run (not run here)

1. `SOTC_NATIVE=2`, one attract window: `unannounced` must not count the `0x1b32f0` range. Count how many times
   `NATIVE_BUILDXPR_ZC` fires per window: 0 per window after the level load means load-time (as the call graph says).
2. Confirm the per-frame absence of `0x19aad0` with a runtime counter at its entry (one hit per model section, none on the
   frame loop).
3. For 5.1 and 5.2: the verify mode counts the unannounced writes into CHUNK-out blocks. A list of functions with writes
   is the input for the next filter update.

## 7. Files

- Changed (uncommitted, worktree `C:\Users\ptr\Documents\sotc-pc\.claude\worktrees\agent-aaa32b4b2e56d1321`):
  `scripts/hooks.d/native_writes.py` (1 declaration: NATIVE_BUILDXPR_ZC, site 0x1b3f97),
  `gpu/sotc/native/native_hooks.cpp` (`BuildXprExit`, `RegisterHandler("NATIVE_BUILDXPR_ZC", ...)`),
  `docs/re-stutters/native/impl-writes4.md` (this file).
- Base copies (identical to the main tree, copied before editing): `gpu/sotc/native/native.h`, `native.cpp`,
  `scripts/hooks.d/native_writes.py` (replaced by the edited copy).
- Not changed: the main tree, `driver.cpp`, `hook_handlers.cpp`.
- Scratch (not part of the repo): the generated per-function table `table2.txt` and the site list `sites.txt` in the
  session scratchpad.
