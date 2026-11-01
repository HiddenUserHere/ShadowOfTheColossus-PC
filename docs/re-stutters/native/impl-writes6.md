# Engine CPU-written, GPU-read memory: five open writers (specialist: writes6)

Status: done for the five items, with two corrections to the brief (items 1 and 2 premises). Nothing committed, nothing
built with ninja or build.sh, the game was not run. Decisions are static (Ghidra decompilation in `out/re/decomp`,
`eboot_re.py` callers/dis/sigfind, raw bytes only for the patch sites). Each row says how sure the decision is.

Base: the worktree predated `gpu/sotc/native/` and `gpu/sotc/hooks.h`. Copied from the main tree before editing:
`docs/re-stutters/native/IMPL-BRIEF.md`, `impl-writes3.md`, `impl-writes4.md`, `impl-writes5.md`,
`scripts/hooks.d/native_writes.py`, `gpu/sotc/native/native_hooks.cpp`, `gpu/sotc/native/native.h`,
`gpu/sotc/native/native.cpp`, `gpu/sotc/hooks.h`. Line endings are LF in both trees (kept).

## 0. Summary

| Item | Premise in the brief | Finding | Decision |
|---|---|---|---|
| 1. 0xfffa0 block | CPUMemory V# base, per frame | `+0x178` is the particle **record pool** (stride `+0x74` x count `+0x6c`), allocated at object creation only, not a V# base; the GPU reads the chunk copies at `+0x80`/`+0xa8` | **no announce** (reason in 1.3) |
| 2. 0x92b50 block | arg 4 is the 0x14-byte block, per frame | confirmed: arg 4 is bound as a read-only buffer V# (5 dwords) by 0x928f0; the CPU writes it in 0x11dbe0 before the call; the other 15 call sites were not verified | **new hook** `NATIVE_CULL_DISPATCH_BLK`, Entry, caller span of 0x11dbe0 only |
| 3. 0x117930 | a copy to add a hook after | the memcpy is at 0x11805a; the path is reached only when the job object's `[1]` is 0 (liveness open) | **new hook** `NATIVE_MAT_COPY_STREAM`, Site 0x11805f, counter `strm` |
| 4. 0x112db0 / 0x97a50 | announce the written ranges Persistent | 0x112db0's GPU copy is already announced by `NATIVE_RES_COPY` (the 0x8be60 copy); 0x97a50 ranges are not CPU-written in its body (callers fill them later) | **no new hook**; runtime checks in section 4 |
| 5. 0xfedc0 | find the writers of `+0x28` / `+0x50` | fedc0 is the non-frame twin of 0xffe60 (same two 0x804 buffers, same sizes), no static caller; its ranges are the ones 0xffe60 already announces | **span added** to `kZeroCopyCallers` (`{0xfedc0, 0xfee84}`) |

Corrections to earlier notes:
- `0xceae0` (writes4, writes5) is **not** a second BPXPR builder. It is a 42-byte tail jump: `cmp byte [rcx+0x20],0; jmp 0x11dbe0`
  (`eboot_re.py dis 0xceae0`). The Ghidra file `out/re/decomp/000c/000ceae0.c` shows the 0x11dbe0 body because the FDE covers
  only 42 bytes. Consequence: 0x11dbe0 is the only writer; the twin rows of writes4/5 are one function.
- The 0xfffa0 allocation (`0x1001fa`, `call 0x12820` then `mov [rbx+0x178], rax`) runs only on the cache-miss path of
  `sub_102590` (`FUN_00017070(0x1c8)` then `FUN_000fffa0`, decomp of 0x102590 lines 56-57). It is object creation, not per frame.
  The chain `RenderThread_Kick -> sub_fc470 -> sub_102590` runs per frame; the allocation does not.

## 1. Item 1: 0xfffa0, the particle block at `+0x178`

### 1.1 What the block is
- `0x1001fa`: `call 0x12820(count * stride, CPUMemory, align 4)`; `[obj+0x178] = result` (`0x1001ff`). `stride` = `[obj+0x74]`
  = `[data+8] + 0xd8`; `count` = `[obj+0x6c]` = `[data+0x10]` (decomp 0xfffa0 lines 87-98).
- The consumers are record accesses: `ParticleRenderer_ShiftRecords` 0x101720 (memcpy of a record from `pool + r15`, stride
  `[this+0x74]`, into the chunk `[rbp-0x30] + r15`), `0x1008b0` (field `+0xcc` of record i), `0x100ad0` / `0x100ac0` (record
  address `pool + stride*i`), `0x1013f0` (per-frame record update, `+0xd4`), `0x1010a0` (BeginFrame record update). It is the CPU
  record pool; the GPU never sees it through a descriptor (see 1.2).

### 1.2 Is the GPU reading the pool? (no, by the static evidence)
- The pool address is not passed to any V# encoder (`0x865410`, `0x8655d0`, `0x865600`, `0x865500`) or to `0x8be60` as a backing
  (`0xfffa0` lines 144-149 create the descriptors at `+0x80` ... `+0x148` with backing 0).
- The GPU-read copies are the chunks: `0x103ad0` (UpdateSimulation) takes `0x8c0d0(obj+0x80)` and `0x8c0d0(obj+0xa8)` every frame
  and passes them to `ShiftRecords` as the record destination (`param_2`) and the index list (`param_3`). Both are on the
  chunk path (`kChunkCallers` has `0x103ad0`) and `NATIVE_PARTICLE_SHIFT` (0x101817) announces the destination record.
- The float reads of `+0xf8`, `+0x120`, `+0x148` in 0x107540 (constants) go through the chunk path (0x107540 is a chunk caller).

### 1.3 Decision
- No hook. The range is pinned (`[pool, pool + stride*count)`), but the GPU does not read it, so an announce would upload
  bytes nobody reads. Confidence: medium-high (static: no V# encoder takes the pool).
- Open, related: the `0x8be60` calls with flags 1 (`+0xf8`, `+0x120`, `+0x148`) allocate CPUMemory storage with no backing copy
  (`0x8be60` lines 24-28), so these **are** V# bases by construction. Their CPU writers were not located (the storage pointer is at
  `obj+0x108` / `+0x130` / `+0x158`; the only hits are float reads in 0x107540). Runtime check: `SOTC_NATIVE=2` with the
  `NATIVE_DESC_865410` handler logging the base argument; a base equal to `[obj+0x108]` (etc.) that is not in an announced range
  is the unannounced case.

## 2. Item 2: 0x92b50 (BPGnm_DispatchWithLabelWait) and the cull block (0x11dbe0)

### 2.1 Argument layout (verified)
- `0x92b50` takes `rdi` = DCB context, `rsi` = buffer handle (word + high 12 bits), `rdx` = count, `rcx` = block, `r8` = dword count.
  `0x92b50` only calls `0x928f0` (no change of registers) and then the dispatch encoder `0x86d210` / `0x86eb60` / `0x86edc0`.
- `0x928f0` (decomp 0x928f0): `V#a = 0x865410(rsi, 0x4404, rdx)` with attribute `0x865360(0x6d)` (bound by `0x861e60`, the output
  side); `V#b = 0x865410(rcx, 0x4404, r8)` with attribute `0x865360(0x10)` (bound by `0x861ce0`, the input side);
  `0x8655d0(8 bytes {count, stride})`. So the block is a buffer of **r8 elements of format 0x4404, read by the GPU through the user-data
  SGPRs** (framedata row 5). The attribute values are inferred (0x10 input, 0x6d output); the binding order matches the
  output (buffer 0xab98) and the input (block).
- Size: 5 x 4 = 20 bytes = 0x14, equal to the `0x12820(0x14, RSF16)` block of 0x11dbe0 (decomp line 106).

### 2.2 Callers (16 call sites in 14 functions; `eboot_re.py callers 92b50`)
- Only `BPXPR_IndexCullObjects` 0x11dbe0 (site 0x11de8e, count 50000, block `pauVar9`, ndw 5) is verified: the block is written
  by the CPU at 0x11dbe0 lines 109-110 (`*pauVar9 = 0`, `*(pauVar9+4) = 0`) and passed before the call. That is CPU-written,
  GPU-read, per frame (the cull job runs per frame: `sub_cdb00` -> `0x11dbe0`; the frame chain was not traced to the top, so
  PerFrame is safe either way).
- The other 15 sites pass object fields (`param_1+0xa0`, `+0x30`, `+0x128`, `+0x2a8`, `+0x700`, `+0x300+k*0x28`, ...) or local blocks
  (`local_60`, `local_80`, `local_798`). Their writers were not verified: some may be GPU outputs (the arg-2 buffer is GPU-written by
  the dispatch), so an Entry announce for all callers would clobber GPU results. Not announced.

### 2.3 Hook
- `NATIVE_CULL_DISPATCH_BLK`, group `native_cull`, **Entry** at `0x92b50`, pattern
  `55 48 89 e5 41 56 53 49 89 fe e8 ?? ?? ?? ?? 49 8b 3e be 08` (20 bytes, `sigfind` unique), expected EU offset `0x92b50`.
- Handler `CullDispatchEntry`: caller filter `kCullDispatchCallers = {0x11dbe0, 0x11e181}` (the return address at entry, `CallerOffset`),
  then `AnnounceRange(rcx, rcx + 4 * r8, PerFrame, cull)`. Only the 0x11dbe0 call (site 0x11de8e) passes the filter.
- Ordering: the Entry runs before `0x928f0` and before the displaced prologue; the block bytes are written (109-110) before the
  call; PerFrame data is read at the next submit (after the job waits in `0xd07c0`), so the announce precedes the reads.
- Mode 0: returns before reading anything (`SotcNative::On()` is checked first).

## 3. Item 3: 0x117930 (material copy, second path)

- Function `0x117930` (1982 bytes, the Ghidra name is `CRenderer::UpdateTextureStreamingGPUData` in the name list; its body has the
  `Material Param Update` profiler scope). Lines 281-283 of the decomp: `uVar4 = 0x12820(DAT_02ed2100, RSF16, 0x10)`, `memcpy(uVar4,
  DAT_02ed2108, DAT_02ed2100)`, `0x928f0(param_2, DAT_02ed2110, n, uVar4, n)` (`n = size >> 2`). The `0x928f0` binds the block as a read
  buffer V# (section 2.1), so the GPU reads it.
- The memcpy call is at `0x11805a`; its return is `0x11805f` (`lea rax, [rip+...]`), with `rbx` = the block, `r12` = `0x2ed2100`
  (not reloaded until `0x11806a`), so `[r12]` is the copied size.
- **Liveness:** `0x117930` is called only from `sub_117140` (site 0x11715a), which is called only by `sub_d0710` (site 0xd0720). `sub_d0710`
  is the job body of `UpdateTextureStreamingGPUData` submitted by `UpdateSkinningRenderObjects` (0xcfdb0, job at 0xcfe63), reached from
  `EndFrameViewParams` (0xcf710) in `RenderThread::FrameJobEntry`. So the path is per frame, **if** the job's `[1]` is 0 (the other
  branch goes to 0x117160, NATIVE_MAT_COPY). The job payload was not traced. Open: the counter below answers it.
- Hook: `NATIVE_MAT_COPY_STREAM`, group `native_strm`, **Site** at `0x11805f`, pattern
  `48 8d 05 ?? ?? ?? ?? 41 8b 14 24 4c 8b 64 24 08 48 89 d9 48 8b 30 c1 ea 02 4c 89 e7 41 89 d0 e8 6d a8 f7 ff`
  (`sigfind` unique; the call rel32 `6d a8 f7 ff` is the one to `0x928f0` at `0x11807e`; the NATIVE_MAT_COPY pattern of `0x11788f` has
  `e8 dd 72 05 00` and does not match here).
- Handler `MatCopyStream`: `AnnounceRange(rbx, rbx + u32[r12], PerFrame, strm)`. Same contract as `MatCopy` (0x11788f), its own counter.
- Confidence: high for the placement (checked against the bytes); the frequency is open until the `strm` counter runs.

## 4. Item 4: 0x112db0 (AreaLoad) and 0x97a50 (resource factory)

### 4.1 0x112db0 (`AreaLoad_ShaderListAndAssets` 0x256c70 is its only caller; load path)
- `+0x44022`: `0x8be60(desc, 0x1b, 0x20, 0x40000, flags 2, backing 0)`. Flags 2 with no backing: no storage is allocated
  (`0x8be60` lines 22-42 skip the allocation and the copy), so the V# has base 0. Nothing to announce.
- `+0x44027`: `uVar7 = 0x12820(0x800000, CPUMemory)`, `memset(uVar7, 0, 0x800000)` (CPU write, lines 237-238), then
  `0x8be60(desc, 0x1b, 0x20, 0x40000, flags 4, backing uVar7)`. Flags 4 is not zero-copy, so `0x8be60` allocates a GPUMemory block
  (`0x12820(..., DAT_02dd9f84)`, `0x8be60` line 28) and **copies** the backing into it (`memcpy`, line 41). That copy is the existing
  `NATIVE_RES_COPY` site (0x8bf50, Persistent, the `dest`/size of the copy). The CPU block is freed right after (`0x12af0(uVar7)`), so
  the GPU never reads it. **Covered by `NATIVE_RES_COPY`; no new hook.** Confidence: high (the same copy path as the other 8be60 users).
- The other memsets of 0x112db0 (`param_1+8`, `+0x100a`, `+0x4100e`) write the CPU render-state object, not a GPU-read block (no V# or
  copy takes these addresses in the function).

### 4.2 0x97a50 (resource factory, 21+ callers)
- The creator allocates the storage (`0x12820`, heap by argument) and returns the resource; it does not write the data (decomp
  0x97a50 lines 184-332: allocation, `0x87ec70` / `0x87efd0` / `0x867990` descriptor setup, no memcpy). The data is written by the
  callers (the loader), after the return. An announce at the return would be too early (zero bytes).
- **Video-out** (`VideoOut_OpenSetFlipRate` 0x95a90, the two `DisplayBuffer` creations at decomp lines 104 and 122, then `0x96f70`):
  no CPU write into the display buffers in the function; they are GPU render or scanout targets. Not announced.
- Other callers (`render_targets_allocate` 26 sites, `sub_197050`, `sub_1986a0`, `sub_1ac2d0`, `sub_17cac0` (a chunk caller)): the
  written ranges are the callers' data, not pinned here. **Open.** Runtime check: the `res` counter of `NATIVE_RES_COPY` covers only the
  8be60 copies; a 0x97a50 block written by the loader and read by the GPU shows as `unannounced` in a `SOTC_NATIVE=2` verify run.

## 5. Item 5: 0xfedc0 (two 0x804 buffers at `+0x28` and `+0x50`)

- `0xfedc0` (196 bytes): `0x12820(count * 0x70, *param_2)` into `obj+0x28` via `0x8be60(obj+0x28, 0x1b, 0x70, count, 0x804, backing)`;
  `0x12820(count * 0x24, *param_2)` into `obj+0x50` via `0x8be60(obj+0x50, 0x1b, 4, count * 9, 0x804, backing)`.
- Its twin is `0xffe60` (lines 18-25 of its decomp, under `lVar1 = *(param_1+0x30)`): the same two resources, the same sizes, created
  **every frame** from the particle update. `0xffe60` is in `kZeroCopyCallers`, so `NATIVE_ZC_CREATE` announces both backings at
  `0x8be60` entry (the announce happens before the particle writes; PerFrame data is read at the next submit, so the order is safe).
- `fedc0` has no static caller (`eboot_re.py callers` empty; no RIP-relative reference; no absolute pointer in the ELF). Its live status is
  unknown. Writers of `+0x28` / `+0x50`: not located for `fedc0` (it has no callers to search). The `0xffe60` family writes the
  same backings after its creator call; the writes themselves were not traced in this pass (open).
- Decision: add `{0xfedc0, 0xfee84}` to `kZeroCopyCallers` (`native_hooks.cpp`). Same class as `0xffe60`, PerFrame is a superset when the
  buffers are written once. Confidence: medium. The `zc` counter shows whether it runs.

## 6. Hooks summary

| Name | Group | Offset (EU) | Mode | Handler | Announce | Counter |
|---|---|---|---|---|---|---|
| NATIVE_CULL_DISPATCH_BLK | native_cull | 0x92b50 | entry | CullDispatchEntry | `[rcx, rcx + 4*r8)` PerFrame, only from `0x11dbe0..0x11e181` | `cull` |
| NATIVE_MAT_COPY_STREAM | native_strm | 0x11805f | site | MatCopyStream | `[rbx, rbx + u32[r12])` PerFrame | `strm` |

Plus the span `{0xfedc0, 0xfee84}` in `kZeroCopyCallers` (existing hook NATIVE_ZC_CREATE, no new pattern).

Report: the PF line gains ` strm <n> <MiB>` and ` cull <n> <MiB>` after ` view`. Nothing else in `Report()` changed; `driver.cpp` needs no change.

## 7. Checks run

- `hooks_install_check.py` (main tools, `--hooks-dir` = a scratch copy of the main `scripts/hooks.d` with this worktree's `native_writes.py`):
  `87/88 hooks accepted`. The new hooks: `NATIVE_CULL_DISPATCH_BLK 0x92b50 accepted mode=entry displaced=6`,
  `NATIVE_MAT_COPY_STREAM 0x11805f accepted mode=site displaced=7`. The one refusal is the pre-existing `RENDER_JOB_RUN 0xc12a0`
  (unresolved indirect branch, not this agent's). CONFLICT lines: 9, the same 9 as the main tree (pre-existing pairs). Main tree: 85/86.
- `hooks_check.py` (same scratch dir): `88/88 hooks resolve to one site`.
- Syntax: `gpu/sotc/native/native_hooks.cpp` passes `clang++ -fsyntax-only -Wall -Wextra -std=gnu++23` with the flags of
  `out/gpu/compile_commands.json` (the `hook_handlers.cpp` line), worktree `gpu/shim` and `gpu/shadps4` first. No warnings.
- Not run: the build, the game, mode 2 (verify).

## 8. Verify checks (not run)

1. `SOTC_NATIVE=2`, one window in the cull path: `cull` counts should be one per cull dispatch; `unannounced` must not count the block.
   A nonzero `unannounced` inside the 20-byte block would mean the writer is not 0x11dbe0's lines 109-110.
2. `strm` per window: 0 means `0x117930` is dead in this build; a count per frame means the job takes the `[1] == 0` branch.
3. `zc` per window for `0xfedc0`: nonzero means the creator runs; its `unannounced` count shows whether the writers reach outside the range.
4. `NATIVE_DESC_865410` base argument vs the storage pointers of `0xfffa0`'s flag-1 blocks (`obj+0x108`, `+0x130`, `+0x158`): the open item of 1.3.
5. A `0x97a50` (resource factory) block read by the GPU: `unannounced` in a verify run is the signal; the loader's write point is the place to hook.

## 9. What remains (open)

- Item 1: the flag-1 CPU blocks of `0xfffa0` (`+0xf8`, `+0x120`, `+0x148`) are V# bases with no located writer (1.3).
- Item 2: 15 of 16 `0x92b50` call sites are not verified. Each needs its arg-4 writer (object field or local) and whether the GPU writes it.
- Item 3: liveness of `0x117930`: the job payload `[1]` of `UpdateTextureStreamingGPUData` (job at `0xcfe63`) is not traced.
- Item 4: `0x97a50`'s callers (render targets, load paths) have no pinned write ranges.
- Item 5: the callers of `0xfedc0` (indirect) are not found.

## 10. Files

- Changed (uncommitted, worktree `C:\Users\ptr\Documents\sotc-pc\.claude\worktrees\agent-a208bbf1a4d8fd0a0`):
  `scripts/hooks.d/native_writes.py` (2 declarations: NATIVE_CULL_DISPATCH_BLK, NATIVE_MAT_COPY_STREAM),
  `gpu/sotc/native/native_hooks.cpp` (`kZeroCopyCallers` +1 span; `Window` +2 counters; `kCullDispatchCallers`, `CullDispatchEntry`,
  `MatCopyStream`; Report +2 classes; 2 `RegisterHandler` lines),
  `docs/re-stutters/native/impl-writes6.md` (this file).
- Copied from the main tree, unchanged: `gpu/sotc/native/native.h`, `native.cpp`, `gpu/sotc/hooks.h`, the N0-N5 docs above.
- Scratch (not in the repo): the hooks scratch copy and the check outputs in the session scratchpad.
