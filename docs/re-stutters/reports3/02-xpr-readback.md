# 02 - XPR visibility mask readback: CPU wait, mask age, buffers, cvar, hook site

Read-only. Addresses are eboot VAs (`out/re/decomp/<hi>/<addr>.c`, `tools/re/eboot_re.py`). Labels: **observed** (disassembly or decompiled pseudocode), **inferred**, **unknown**.

## 1. Answers

| # | Question | Answer | Label |
|---|---|---|---|
| 1 | CPU wait between cull EOP and mask read? | **No GPU-label wait.** The only CPU wait on the chain is a job-counter wait in `sub_c12a0` (0xc12a0) on `[ctx+0xe28]`, which waits for CPU jobs, not the GPU. | observed |
| 1b | Which frame's mask is read? | The consumer records in the same frame, before the GPU runs that frame's cull. It reads the bytes last written at that address by an earlier GPU execution. Which frame that is depends on address reuse, which is unknown. | inferred |
| 2 | Mask buffer allocator | `sub_12820` with family handle `DAT_02ed1f58`, GPU family (kind 2, inferred), 16-byte aligned. Size `4 * ceil(n/32)` bytes per buffer, n = object count (max 256 B at 2048). Not ping-pong by frame; two slots chosen by config byte. | observed / inferred |
| 3 | `forceUseLastIndexBufferCullingResults` reader | Cvar object at `0x2eaa920`, value byte at `+0x68`. No code reads the value directly. The reader is reached through the cvar registry, and I did not find the branch. | observed (object), unknown (branch) |
| 4 | Best hook site | Guest entry `0x1a6d20` with a mask-pointer swap (`rsi`), served from a port copy taken at the cull EOP (`liverpool.cpp:3763`). The port side already has `TryServePrevious` (`buffer_cache.cpp:2607`). | inferred |

Report 01 open question 1 is settled: the game does not wait on the label. The conflict between reports 01 and 07 is resolved in the same way. What the CHANGELOG calls "the game stalls on the read" is the port's own fault-and-copy wait, not a game wait.

## 2. Evidence

### 2.1 Chain from the frame to the consumer

| Step | Address | Evidence (observed) |
|---|---|---|
| Frame job body calls XPRPreRender | `sub_cefa0` (0xcefa0, call at decomp line 335), `sub_ccdf0` | caller list of `sub_ca6d0` |
| XPRPreRender: rotate slot, clear labels | `sub_ca6d0` 0xca6d0; `*(ctx+0xe8) = *(ctx+0xe0)` (decomp line 49); clears `slot+0x40`, `slot+0x68` (0xca99b, 0xca9a3) | decomp |
| Queue cull job `BPXPR_RunCullPasses` | `sub_c6670`, `c12a0(..., flags 2, dep = [ctx+0xe28])` | decomp, `ca6d0` near line 191 |
| Shadow cull, EOP to `slot+0x40` | `sub_c6bd0`, `sub_925c0(.., [ctx+0xe8]+0x40)` at 0xc6f2f (decomp line 83) | observed |
| Main cull, EOP to `slot+0x68`, then `BuildSetDraws` | `sub_c6f70`, EOP at line 120, `sub_19b760` at `0xc722c` | observed |
| Queue RenderDepth job | `sub_c59d0`, `c12a0(..., flags 0x302, dep = [ctx+0xe28])` (lines 205-206) | observed |
| Job runs per-draw recorder | `sub_c1860` -> `BPXPR_RecordMaskedDraws` `0x11d7a0` -> `BPXPR_SelectMaskSource` `0x19af20` (call at 0x11d949) | caller walk |
| Mask read | `BPXPR_ConsumeVisibilityMask` `0x1a6d20`, read at `0x1a6d60` (site 0x19b0d8 via `0x19af20`) | observed |
| Indirect variant | `BPXPR_ConsumeVisibilityMaskIndirect` `0x1a7290` (site 0x19b0c4), taken when `[cfg+0x22]` is set | observed |

### 2.2 CPU waits on the chain

| Address | What it waits on | Relevance |
|---|---|---|
| `sub_c12a0` (0xc12a0), the non-immediate branch: `if (param_5 != 0) FUN_00035590(DAT_02ea4718, param_5, 0, 0, 1)` | `[ctx+0xe28]`, a job counter (WaitForCounter: spin, then `usleep(1)`, report 02) | Runs before the RenderDepth job (c1860) is queued. Waits for CPU job completion, not a GPU label |
| `sub_ccfd0` (0xccfd0:59) and `sub_caed0` (0xcaed0:267, 631) | `[obj+0xe28]` via 0x35730 / 0x35590 | Other frame sync points on the same counter |
| `sub_86edc0` WAIT_REG_MEM emitters on `[ctx+0xe8]+N` | Offsets +0x8, +0x10, +0x18, +0x30, +0x80, +0x88, +0x98, +0xa8, +0xb0, +0xb8, +0xd0, +0xe0, +0xf0, +0x100 ... | Not +0x40 or +0x68 (see 2.3) |

### 2.3 The mask labels are never waited on

- **observed:** the only writes to `[ctx+0xe8]+0x40` and `+0x68` are the EOP calls in `sub_c6bd0` (0xc6f2f) and `sub_c6f70` (0xc7264), plus the clears in `sub_ca6d0`. A search for `WAIT_REG_MEM` emitters over the decompiled tree found no wait with offset 0x40 or 0x68 on the frame slot. The two hits with `+0x40` (`sub_c2710` 0xc2710, `sub_183d00` / `183d10`) use base `+0x578`, a different object.
- **observed:** the consumer `0x1a6d20` does not read any label. The probe in `scripts/hooks.d/xpr.py` (`XPR_CONSUME_MASK`) is observe-only and has no recorded result in CHANGELOG (`grep` finds none).
- **inferred:** the game relies on frame-level pacing (report 02 handshake), not a per-label wait. Unverified; it is the basis of reading 1b.

### 2.4 Mask buffers (question 2)

- **observed:** `sub_19b760` (BuildSetDraws, 0x19b760) selects the slot by config byte: `plVar10 = [ctx+0x21e8]`, or `[ctx+0x2238]` when `[cfg+0x1c] != 0`, where `cfg = set[9]` (decomp lines 134-136). The selection is by config, not by frame parity. The same selection appears in `sub_11dbe0` (0x11dbe0, lines 115-117), `sub_19c7e0`, and `sub_ceae0`.
- **observed:** four buffers per call, each `FUN_00012820(4 * ((n+31)>>5), DAT_02ed1f58, 16, 0)`, where `n = u16 [object+0x64]` (`uVar1 = *(ushort*)(object+100)` in the decomp, the same count the consumer reads at `+0x64`). Max 2048 bits gives 256 B per buffer.
- **observed:** `DAT_02ed1f58` is set in `sub_8c370` (0x8c370) from object `0x2ed2ca0`, constructed with kind argument `2`. `sub_12820` indexes the family table `0x2dfaab0` by this value. Family 1 is `CPUMemory` (`sub_13380`) and family 2 is `GPUMemory` (report 04, `sub_110a0` at 0x111e8). **inferred:** the mask heap is the GPU family, kind 2 (Garlic, per report 04). The name of its heap object is not resolved.
- **observed:** `XPRPreRender` allocates a fresh 0xae40-byte per-frame table each frame (`sub_ca150`, 0xca150, `FUN_00017170(0xae40, DAT_02ed1f58, 8)`, memset to 0). The per-set entries (0xa8 bytes, index = object XPR index) live in that table at `[ctx+0x38]`.
- **unknown:** whether the four bitmap addresses are recycled across frames (heap ring behaviour), and the writer of `[ctx+0x21e8]` / `[ctx+0x2238]` (no direct store found; `sub_a0290` reads them).
- **Not double-buffered by frame.** The two slots are per config (normal / sun shadow), not per frame.

### 2.5 The cvar (question 3)

- **observed:** `RegisterXprCvars` (0x6efe0) registers the name at `0xf9a971` in `0x70282`-`0x702e7`. Its object is at `0x2eaa920` (ctor `sub_20af0`). Default byte is read from `0x2ea8f0a` (`[r14+0x22]`, r14 = `0x2ea8ee8`) at `0x7033a`, and stored at object `+0x68` (`0x70344`).
- **observed:** no code references `0x2ea8f0a`. The object `0x2eaa920` is referenced only by `sub_55160` (0x55a8f), which copies a 0x30-byte descriptor through `sub_26db0`. That is the cvar registry (reflection), not a reader.
- **unknown:** the read site and the branch. The only config-byte branch I found is in `0x19af20`: `[cfg+0x22]` picks `0x1a6d20` or `0x1a7290`, and `[cfg+0x22]` is not tied to this cvar. Runtime test: set `forceUseLastIndexBufferCullingResults=1` and count calls to `0x11dbe0` and `0x19b760` per frame.

### 2.6 Hook site (question 4)

Guest candidates, all with unique signatures (`eboot_re.py sig`):

| Site | Signature | Use |
|---|---|---|
| `0x1a6d20` BPXPR_ConsumeVisibilityMask, entry | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f9` (20 B, unique) | Swap `rsi` (mask pointer) for a port-owned copy of the same bitmap. Args: `rdi` = set record, `rsi` = mask, `rdx` = draw record, `rcx` = ctx |
| `0x19af20` BPXPR_SelectMaskSource, entry | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 25 ?? ?? ?? ?? 48 89 fb 49 89 d6 49 89` (32 B, unique) | Earlier point; mask is computed here, so a swap can use the entry's `+0x0/+0x8/+0x10/+0x18` field |
| `0x925c0` BPGnm_WriteCsDoneLabel | `55 48 89 e5 48 8b 3f 48 89 f0 be 28` (12 B, unique) | Label writer; use to tag which buffer a fence covers |

Port side (the copy at the fence and the served copy):

- **copy at fence:** `Liverpool::ProcessGraphics`, `case PM4ItOpcode::EventWriteEop` (`gpu/shadps4/video_core/amdgpu/liverpool.cpp:3763`, the EOP path at 3820-3830). Enqueue a GPU-to-host copy of the mask window when the label is `slot+0x40` or `slot+0x68`.
- **serve the last completed copy:** `BufferCache::BeginAsyncReadbackImpl(AsyncReadback& readback)` (`buffer_cache.cpp:1291`, reads `readback.device_addr`, `readback.size`) and `BufferCache::TryServePrevious(AsyncReadback& readback)` (`buffer_cache.cpp:2607`). `TryServePrevious` already implements a previous-copy serve (`SOTC_RB_PREV`), with no recorded measurement.

**Recommendation:** the guest hook at `0x1a6d20` is the explicit readback point. It needs no port change beyond serving the mask address from the last completed copy at the EOP fence.

## 3. Corrections

- Report 01 open question 1 is settled (no game wait on the label). The "draw list depends on mask bits at record time" claim holds.
- `out/re/decomp` is populated now (report 02 said it was empty).

## 4. Next measurement (no code change)

1. At `0x1a6d20` entry, log `rsi` (mask address) and `[ctx+0xe8]`. Compare `rsi` with the bitmap address that `0x19b760` allocated in the same frame. Equal addresses mean the mask is the same frame's buffer (not yet GPU-written); different addresses give the recycle distance.
2. Count `0x1a6d20` calls per frame with `forceUseLastIndexBufferCullingResults` 0 and 1 (the cvar branch).
3. Run `XPR_CONSUME_MASK` (`SOTC_HOOK_XPR_PROBE=1`, `scripts/hooks.d/xpr.py`) and record the before/after counts.
