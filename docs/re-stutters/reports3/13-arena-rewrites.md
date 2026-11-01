# 13. Who writes into RenderSingleFrameGPU after the draw is recorded

Branch `re/stutter-architecture`. Static analysis only (decompile in `out/re/decomp/`, call index via
`tools/re/eboot_re.py`). Nothing was built or run. Addresses are eboot VAs.

Labels: **verified** (read in the decompile or disassembly), **inferred** (from call structure or
naming, not fully proven), **ruled out** (read, and no write found that matches the pattern),
**open** (needs a run).

Pass 2 read every row that was OPEN in pass 1 (rows 4, 5, 7, 8, 9 and the callers). The method and
its limits are in section 8.

## 1. Summary

- The render frame is built on one render-job thread by `EndFrameViewParams` (0xcefa0), entered
  through `RenderThread::FrameJobEntry` (0xcef90). **Verified.**
- Every submit goes through `RenderThread::SubmitJob` (0xc5560), which copies the pending
  job/command-list entries of the current context into a 0x610-byte record and kicks the submit job
  `FUN_000d07c0`. That job waits on the record's dependency counters (`JobSystem_WaitForCounter` at
  0xd0816) and only then calls `sub_91f80` → `GfxCmdList::Submit` (0xd0876, 0xd08a1). **Verified.**
- **Pass 2 result: no writer in the 19 OPEN functions (and in the three helpers they call that
  allocate: 0x137b90, 0x137fb0, 0x1375e0) writes into an arena block after the PM4 that references
  that block.** Every call follows the same order: allocate a fresh block (0x8c0d0 / 0x12820), fill
  it, then emit the PM4 that references it (SET_SH_REG pointer push 0x85ecd0 / 0x862490, descriptor
  emit 0x861ce0, or dispatch 0x86b9e0 / 0x883870 / 0x92b50). **Verified by decompile order and call-site
  addresses, with the scan in section 8.**
- Object-owned slots are **re-pointed, not rewritten**: `param_2+0xb0` (PrepareMotionBlur, ApplyDOF,
  9710), `param_2+0xb8` (ParticleRenderer 0x1051f0), `param_1+0x220110` (InstanceDynamicData 0x11c710).
  Each call stores a new block pointer before its emit. No store goes through an old pointer. **Ruled out.**
- Two patterns look like "write after dispatch", and both are CPU writes into CPU draw-list or
  context structs, not into arena memory and not into a constant buffer the GPU reads later:
  `*(param_2[7] + 0xa8*idx) = mask` in 0x19b760 and `*(param_3[7] + 0xa0 + …)` in 0x19c7e0, plus the
  context slots in 0x19c1a0. **Ruled out as arena rewrites.**
- Consequence for section 4: the intra-frame "rewrite after submit" explanation for the 43% in-place
  rewrites is **not supported** by these writers. The most likely source is cross-frame reuse of the
  same arena addresses (section 4), which the attribution run must confirm.
- Recommendation (revised, see section 5): the attribution run (section 6) comes before any hook. For
  these writers a per-submit declare of the arena ranges touched since the previous submit is enough,
  because every fill finishes before the emit that references it.

## 2. Frame structure (render thread)

`EndFrameViewParams` (0xcefa0), in the order of its calls (decompile line numbers from
`out/re/decomp/000c/000cefa0.c`):

| Step | Call | Address | Notes |
|---|---|---|---|
| 1 | `EndFrameView_WriteA` | 0x8b7f0 | one descriptor alloc (0x8c0d0) |
| 2 | `UpdateSkinningRenderObjects` | 0xcfdb0 | see below |
| 3 | `BPXPR_PreRender` | 0xca6d0 | calls `InstanceDynamicData_Update` 0x11c710 |
| 4 | `RenderView::MainCamera` | 0xcaed0 | job kicks, 14 waits, 2 submits (189, 200) |
| 4a | `sub_d04e0` (PostProcessing) | 0xd04e0 (site 0xcfa57) | see section 7, row 1 |
| 5 | `ParticleRenderer::BeginFrame` via 0xd0040 | 0x107540 | 3 descriptor allocs |
| 6 | water and particles | 0xda110, 0xe48b0, 0x109970, 0x1038a0 | particle update writes here |
| 7 | `Frame::InsertWaitFlipDone` | 0x118470 | |
| 8 | `RenderThread::SubmitJob` | 0xc5560 (site 0xcfd52) | final submit of the frame |

`UpdateSkinningRenderObjects` (0xcfdb0), **verified** by disassembly:

- 0xcfe4c `RunFunctionAsRenderJob` (the skinning job, body `LAB_000d06f0`), then 0xcfe56 `SubmitJob`.
- 0xcfef7 `RunFunctionAsRenderJob` (`UpdateTextureStreamingGPUData`, body `LAB_000d0710` = `sub_d0710`),
  then 0xcff01 `SubmitJob`.
- 0xcff13 `UpdateGPUData_InstanceData` (0x118100) runs **on the render thread after both submits**.

`sub_d0710` (21 bytes, **verified**) is the job entry for records: if `param[1] != 0` it calls
`sub_117160` (updateDynamicGPUDataJob), otherwise `sub_117930`.

## 3. Writers table (status after pass 2)

Counts are static call sites (`eboot_re.py` index). Byte volumes come from `PC-MEMORY-REWRITE.md`
section 6. Row numbers are kept from pass 1.

| # | Writer (address) | Heap | Pattern (verified) | Thread / path | Status |
|---|---|---|---|---|---|
| 1 | `InstanceData_WriteStream` 0x123410 | RSF-GPU (desc) | alloc, fill, emit in one call | render job, from `UpdateGPUData_InstanceData` 0x118100 | verified in pass 1, unchanged |
| 2 | `ParticleRenderer::UpdateSimulation` 0x103ad0 | RSF-GPU | alloc, fill, dispatch; `ShiftRecords` 0x101720 fills | render job, via `sub_103aa0` | verified in pass 1 |
| 3 | `sub_1051f0` (ParticleRenderer variant) | RSF-GPU | fresh block per branch; slot `+0xb8` re-pointed | via `sub_103aa0` tail jump | **ruled out** (section 7, row 18) |
| 4 | `sub_b50c0` PrepareMotionBlur | RSF-GPU | 5 fresh blocks, fill → push → dispatch | job worker (inferred), via `sub_b5060` | **ruled out** (row 1) |
| 4 | `sub_b5b10` PrepareMotionBlur (variant) | RSF-GPU | 5 fresh blocks | job worker / render thread | **ruled out** (row 2) |
| 4 | `sub_b6e20` ApplyDOF | RSF-GPU | 5 fresh blocks, memcpy 0x4d0 at alloc | render thread, via `sub_b6590` | **ruled out** (row 3) |
| 4 | `sub_b9710` | RSF-GPU | 5 fresh blocks, fill → push → draw | render thread, from `sub_b9610` | **ruled out** (row 4) |
| 5 | `BPXPR_BuildSetDraws` 0x19b760 | RSF-GPU (mask + 0x160 block) | masks GPU-written; 0x160 block memcpy at alloc | from `BPXPR_CullMainScene` 0xc6f70 | **ruled out** for CPU writes after emit (row 5) |
| 6 | `sub_117160` updateDynamicGPUDataJob | RSF-GPU | job with own counters | render job via `sub_d0710` | verified in pass 1 (covered by record counters) |
| 7 | `InstanceDynamicData_Update` 0x11c710 | RSF-GPU (heap at `param_1+0x220110`) | fill loop, then `BPGnm_DispatchWithLabelWait` | `BPXPR_PreRender` 0xca6d0 | **ruled out** (row 6) |
| 8 | `sub_17ecf0` | RSF-GPU | 3 variants, each fresh blocks, fill → push → dispatch | job worker (inferred), via `sub_b5060` | **ruled out** (row 7) |
| 8 | `sub_17f790` | RSF-GPU | 3 fresh blocks per variant | job worker or render thread | **ruled out** (row 8) |
| 8 | `sub_bb310` | RSF-GPU | 3 blocks, fill → bind → push → dispatch | render thread, from `sub_bae70` | **ruled out** (row 9) |
| 8 | `sub_198bb0` | RSF-GPU | fill blocks, then SLOT, push, dispatch | render thread, from `sub_d0040` | **ruled out** (row 10) |
| 8 | `sub_19c7e0` | RSF-GPU + GPU wraps | mask GPU-written; 0x160 memcpy; CPU list write after dispatch | from `sub_c5200` | **ruled out** (row 11) |
| 8 | `sub_19c1a0` | RSF-GPU + GPU wraps | as 19c7e0; CPU context writes after dispatch | from `sub_c6bd0` | **ruled out** (row 12) |
| 9 | `sub_164a60`, `sub_164f30` | RSF-GPU (5 and 4 descriptors) | fill all descriptor blocks, then `0x861ce0` emit, then draw | from `sub_23b870` / `sub_164e70` | **ruled out** (rows 13, 14) |
| 9 | `sub_1371e0` (+ 137b90, 137fb0, 1375e0) | RSF-GPU | memcpy at alloc, then emitters with their own fresh blocks | from `sub_136b30` | **ruled out** (row 15) |
| 9 | `sub_10cf60`, `sub_10d890` | RSF-GPU | fill → push → dispatch | from `sub_106950` / `sub_106ce0` | **ruled out** (rows 16, 17) |
| 9 | `sub_103aa0` | (wrapper) | tail jump to 0x103ad0 or 0x1051f0 | no direct caller | caller **open** (section 9) |
| 10 | `EndFrameView_WriteA` 0x8b7f0 | RSF-GPU | 1 desc | `EndFrameViewParams` step 1 | not in the open list, not read |
| 11 | `ParticleRenderer::BeginFrame` 0x107540 | RSF-GPU | 3 desc | via `sub_d0040` | not in the open list, not read |
| 12 | `sub_16e990` (constructor) | RSF-GPU | object constructor | `sub_906d0` | not in the open list |
| 13 | GPU buffer creators 0x8be60 with `memcpy` | GPUMemory / RSF | copy at create | mostly not in the frame path | low |

## 4. Where a rewrite can happen

For a rewrite inside one frame, all three must hold: the PM4 that references an allocation is emitted
into record K; the allocation is written after record K was submitted; the allocation is not a fresh
block.

Pass 2 finds the first and second conditions false for every writer read:

- **Fresh blocks.** Every writer allocates before it fills (section 7). Within a frame the RSF heap is a
  bump allocator (`sub_1d750`, reached through vtable +0xa0 from 0x12820; **verified** in the decompile):
  each allocation takes the next free offset of the current block. The only reuse inside a frame is the
  chunk free list walked at the top of `sub_1d750` (`pauVar9+1`). Who fills that list (the deferred
  frees at 0xd08d9, `sub_16f100`) was **not** read. This is the one remaining intra-frame reuse path.
- **Object-owned slots.** `param_2+0xb0`, `param_2+0xb8`, `param_1+0x220110` are written with a fresh
  block pointer before each emit, and nothing reads them to write through. Ruled out.
- **Per-view and per-pass constants.** The per-view blocks (BuildSetDraws masks, PrepareMotionBlur and
  ApplyDOF constants) are allocated per call. Ruled out.
- **Job fills.** Job-path writers (b50c0, 17ecf0, b5b10, 17f790) fill inside the job. The job's
  `RunFunctionAsRenderJob` (0xd0572) adds its counter to the current record before the call
  (0xd0559–0xd0562, **verified**), so the submit fence 0xd0816 waits for it. Upload at the fence covers
  these writes.

**Cross-frame reuse (new, inference).** `sub_1d230` (vtable +0x20, the heap reset) zeroes the chunk
lists of the heap object it receives and re-links their sentinels (**verified** in the decompile). It is
called from `0x107540` (`ParticleRenderer::BeginFrame`). If that heap is the RSF slot that the
2-slot ring reuses, the same arena addresses are written again two frames later. A write fault on a page
uploaded in an earlier frame then looks like an "in-place rewrite" without any intra-frame write. This
would explain the 43% figure by page-level faults and does not need an intra-frame writer. **Open**: the
attribution run must bucket write-fault RIPs by frame distance (same frame vs. previous frame) to
confirm it.

## 5. Options

**(a) Upload at the submit fence, per submit.** Declare the arena ranges written since the previous
submit at the point just before `FUN_000d07c0` waits (0xd0816) or at `SubmitJob` (0xc5560), using
dirty tracking from the write faults. Every writer in pass 2 completes its fills before the emit that
references it and before the submit that covers that emit. The fence therefore sees complete data. This
is **sufficient for the writers read**, provided the arena's cross-frame reuse is handled by re-upload
on reuse (which a per-submit dirty set does by construction).

**(b) Hook each writer's end.** The writers read have one emit per block, placed right after the fill.
The hook points are the emit sites (table in section 6). Cost is higher than (a) and no longer needed
for intra-frame correctness, but it is the cheapest per-writer precise declare if (a) is not wanted.

**(c) GPU reads the sub-range from host-visible memory.** Import the arena with
`VK_EXT_external_memory_host` (stage 1 of `SOTC_PC_MEMORY` already does this in 64 MiB chunks, tested
coherent, 5.5 GB/s GPU reads on the RTX 2060 SUPER per `PC-MEMORY-REWRITE.md`). The GPU sees the bytes
the CPU has written at execution time, with no copy and no writer list. It stays correct for any
rewrite pattern, including cross-frame reuse. Cost: GPU reads of dynamic data over PCIe (about 15 MiB
per frame).

**Revised recommendation.** Do not implement a per-writer hook yet. Run the attribution in section 6 first.
If the faults are cross-frame (the likely case from section 4), (c) or (a) with re-upload on reuse is
the fix. The object-slot and per-view argument from pass 1 is withdrawn: those writers do not rewrite.

## 6. Hook sites and checks

Fence and submit (for (a), and for any ordering check):

| Site | What | Verified |
|---|---|---|
| 0xc5560 | `RenderThread::SubmitJob`, copies the pending list (`+0xe88`/`+0xe8c`/`+0xe98`) into record, kicks 0xd07c0 | yes |
| 0xd07c0 | submit job body | yes |
| 0xd0816 | `JobSystem_WaitForCounter` loop over record counters (the fence) | yes |
| 0xd0876, 0xd08a1 | `sub_91f80` (→ `GfxCmdList::Submit`) | yes |
| 0xd08d9 | `sub_16f100` (list of deferred frees) | yes |
| 0xd0923 | `sub_91f90` and `sceGnmSubmitDone` (last record) | yes |
| 0xd0559–0xd0562 | counter add for a render job (`RunFunctionAsRenderJob` 0xd0572) | yes |

Declare hooks (emit sites, placed after each fill; the declare goes just before these calls). Addresses
are the call sites from the disassembly:

| Writer | Alloc (0x8c0d0 / 0x12820) | Reference emit (push / dispatch) |
|---|---|---|
| 0xb50c0 (PrepareMotionBlur) | 0xb526e, 0xb551c, 0xb5698, 0xb57fa, 0xb5969 | push 0xb5491, 0xb560f, 0xb578e, 0xb5910, 0xb5a66; dispatch 0xb5503, 0xb568a, 0xb57ef, 0xb595e, 0xb5aad |
| 0xb5b10 | 0xb5cd6, 0xb5f83, 0xb6106, 0xb6275, 0xb63e5 | push 0xb5f11, 0xb6093, 0xb6216, 0xb63a1, 0xb64f8; dispatch 0xb5f6d, 0xb60fb, 0xb626a, 0xb63da, 0xb652a |
| 0xb6e20 (ApplyDOF) | 0xb79fd, 0xb7f99, 0xb83d4, 0xb87bd, 0xb8b2e | push 0xb7a9e, 0xb803e, 0xb8472, 0xb8861, 0xb8bcc; dispatch 0xb7ba8, 0xb80b1 |
| 0xb9710 | 0xb99b9, 0xb9c8f, 0xb9f2f, 0xba1c2, 0xba45b | push 0xb9a5a, 0xb9d48, 0xb9fe8, 0xba269, 0xba511 |
| 0x19b760 (BuildSetDraws) | 0x19b836, 0x19b889, 0x19b8d9, 0x19b929 (masks), 0x19bd55 (0x160) | dispatch 0x19c0f1 (masks written by GPU) |
| 0x11c710 (InstanceDynamicData) | 0x11c76d | `BPGnm_DispatchWithLabelWait` 0x11cf01 |
| 0x17ecf0 | 0x17ef05, 0x17f1e0, 0x17f458 | push 0x17f0c9, 0x17f33c, 0x17f69f; dispatch 0x17f16e, 0x17f3a8, 0x17f70b |
| 0x17f790 | 0x17f9e5, 0x17fcca, 0x17ff70 | push 0x17fbb4, 0x17fe3a, 0x1801b6; dispatch 0x17fc16, 0x17fe90, 0x18020f |
| 0xbb310 | 0xbb3c4, 0xbb540, 0xbb6bc | push 0xbb4cf, 0xbb64b, 0xbb7c7; dispatch 0xbb509, 0xbb685, 0xbb801 |
| 0x198bb0 | 0x199305, 0x199790, 0x199d5d (plus 0x937c0 binds) | push 0x1996ea, 0x199b18, 0x19a11a; dispatch 0x19a183 |
| 0x19c7e0 | 0x19c9e8, 0x19cbd6, 0x19cce4 | push (FUN_000ae150) before 0x19cd9e; dispatch 0x19ce03 |
| 0x19c1a0 | 0x19c238, 0x19c28b, 0x19c435 | dispatch 0x19c6ac, after desc emit 0x19c67b |
| 0x164a60 | 0x164b3f, 0x164b63, 0x164b8b, 0x164bb2, 0x164bdc | desc emit 0x164e26 |
| 0x164f30 | 0x16500f, 0x165033, 0x16505a, 0x165084 | desc emit 0x16529c |
| 0x1371e0 (+137b90/137fb0/1375e0) | 0x1373a2, 0x1373cb, 0x13745a, 0x137483; own: 0x137e16, 0x138009, 0x137845 | desc emit 0x137f05, 0x1380ca; flush 0x137965; dispatch 0x137f66, 0x138192, 0x13798d |
| 0x10cf60 | 0x10d034, 0x10d06e, 0x10d0ca | push 0x10d134; dispatch 0x10d179 |
| 0x10d890 | 0x10d969, 0x10d9a2, 0x10d9fe | push 0x10daa6; dispatch 0x10dadd |
| 0x1051f0 | 0x1056af, 0x1056c0 (top, filled by 0x101720), 0x105a50, 0x106086, 0x1061da | push 0x105b8f, 0x1063ee, 0x1067cf; dispatch 0x105bff |

Writers (for the attribution run):

| Site | What |
|---|---|
| 0x12345e, 0x123694 | descriptor allocs in `InstanceData_WriteStream`; fill follows |
| 0x103f62, 0x103f74, 0x104309, 0x104966 | descriptor allocs in `ParticleRenderer::UpdateSimulation` |
| 0x118203 | `UpdateGPUData_InstanceData` call site of `InstanceData_WriteStream` |
| 0xcff13 | `UpdateGPUData_InstanceData` call inside `UpdateSkinningRenderObjects` |
| 0x117876 | ~4 MiB per-frame block (from PC-MEMORY-REWRITE section 6), inside `sub_117160` |

Attribution check (open, needs a run, not done here): in a verify run, log the RIP of each write fault
on RSF pages together with the frame index of the page's last upload. Bucket by frame distance (0 = same
frame, 1 = previous, 2 = two frames back). Map RIPs with `eboot_re.py func`. A fault in the same frame
on a page uploaded in that frame is an intra-frame rewrite and would contradict section 1. Faults that
come from frame distance 2 are the cross-frame reuse of section 4.

## 7. Per-function evidence (pass 2)

Decompile line numbers refer to `out/re/decomp/<bucket>/<addr>.c`. Store windows are given as
address windows between an alloc and its emit, because each individual store was not matched to a
register. Verdicts use the decompile order and call-site addresses.

1. **`sub_b50c0` (PrepareMotionBlur), ruled out.** Thread: job worker (inferred: `sub_b5060` calls it
   at 0xb507c, and `sub_b5060` is the body of the job kicked at 0xd0572; `sub_b5060` has no direct
   caller). Five blocks, each with its own alloc: 0xb526e, 0xb551c, 0xb5698, 0xb57fa, 0xb5969. Fill
   window of block 1: 0xb5339–0xb53be (the `0x8829e0` descriptor-word calls), before the record at
   0xb5443 and the push at 0xb5491. Slot `param_2+0xb0` is set to the new block at decomp 124, 163,
   201, 240 (before each push). Dispatches at 0xb5503, 0xb568a, 0xb57ef, 0xb595e, 0xb5aad. No store to a
   block after its dispatch (the next alloc is at the next address window).
2. **`sub_b5b10` (PrepareMotionBlur variant), ruled out.** Five blocks at 0xb5cd6, 0xb5f83, 0xb6106,
   0xb6275, 0xb63e5. Each: fill, record, push (0xb5f11, …, 0xb64f8), dispatch (0xb5f6d, …, 0xb652a).
   The `if (lVar7 == 0)` split in the instance loop gives two mutually exclusive blocks for one slot
   (decomp 386 and 446), so each iteration has one fresh block.
3. **`sub_b6e20` (ApplyDOF), ruled out.** Render thread, via `sub_b6590` (call at 0xb66a0). Five allocs:
   0xb79fd, 0xb7f99, 0xb83d4, 0xb87bd, 0xb8b2e. Block 1 is filled by `import_memcpy(…, 0x4d0)` at alloc
   (decomp 476) before its record (0xb7a44) and push (0xb7a9e), then dispatch 0xb7ba8. Blocks 3-5 are
   consumed by graphics draws after their push, not by a dispatch. Object slot `param_2[0x16]` (=
   `+0xb0`) is set to each new block before its push (decomp 479, 651, 799, 947, 1078). The loop at
   decomp 647 runs twice (`iVar17 != 2`) and allocates per iteration. The CPU stores at decomp 320-333
   go to the render context (`lVar23`), not to a block.
4. **`sub_b9710`, ruled out.** Render thread, from `sub_b9610` (call at 0xb96c3). Five allocs: 0xb99b9,
   0xb9c8f, 0xb9f2f, 0xba1c2, 0xba45b. Each: fill (decomp 104-107, 175-184, 245-254, 317-321), SLOT, record,
   push (0xb9a5a, …, 0xba511), then bind and draw. The `lVar8+0xc548` stores (decomp 212-217, 287-292)
   were checked and are context writes (the variable is re-assigned to the context pointer first).
5. **`sub_19b760` (BuildSetDraws), ruled out for CPU writes after the emit.** Four mask blocks
   (0x12820 at 0x19b836, 0x19b889, 0x19b8d9, 0x19b929), each wrapped by 0x8be60 at 0x19b86b, 0x19b8be,
   0x19b90e, 0x19b963 (usage 0x805). No CPU store targets the mask memory (inferred: GPU-written by the
   dispatch at 0x19c0f1). One 0x160 block at 0x19bd55, `import_memcpy` of `local_480` at alloc, wrapped
   at 0x19bda5, bound at 0x19be10. The CPU write `*(param_2[7] + 0xa8·idx) = local_4e0` comes after the
   dispatch (decomp line after 0x19c0f1 call). It targets a CPU draw list, not arena memory.
6. **`sub_11c710` (InstanceDynamicData_Update), ruled out.** The allocation writes the new block address
   into the heap descriptor at `param_1+0x220110` (alloc 0x11c76d). The fill loop writes 0x20-byte
   instance records at decomp 330-336, inside the `do … while (plVar13 != plVar6)` loop. The emit
   `BPGnm_DispatchWithLabelWait` (sub_92b50) at decomp 349 (site 0x11cf01) reads the descriptor
   (`+0x220110`, `+0x220114`). No store to the block after 0x11cf01. The "persistent slot" is the
   heap descriptor, re-pointed each call.
7. **`sub_17ecf0`, ruled out.** Job path (tail jump from `sub_b5060` at 0xb5094). Three variants inside a
   loop over an object table (`lVar18 = 0x436`), each: alloc (0x17ef05, 0x17f1e0, 0x17f458), fill
   (decomp 92-123, 164-185, 224-266), record (0x17f07e, 0x17f2ee, 0x17f651), push (0x17f0c9, 0x17f33c,
   0x17f69f), flush, dispatch (0x17f16e, 0x17f3a8, 0x17f70b). The fill window of each variant ends at its
   record.
8. **`sub_17f790`, ruled out.** Job path (`sub_b5060` jmp at 0xb50b9) and render path (`sub_b6590` call at
   0xb6685). Three blocks: alloc 0x17f9e5, 0x17fcca, 0x17ff70; fill (decomp 92-123, 163-184, 227-269);
   record 0x17fb5a, 0x17fddf, 0x18015c; push 0x17fbb4, 0x17fe3a, 0x1801b6; dispatch 0x17fc16, 0x17fe90,
   0x18020f.
9. **`sub_bb310`, ruled out.** Render thread, from `sub_bae70` (called from `BPRender_DepthAndGBufferPasses`
   at 0xc611a). Three blocks: alloc 0xbb3c4, 0xbb540, 0xbb6bc; fill decomp 42-54, 84-96, 126-138; SLOT
   (decomp 61, 103, 145); record 0xbb481, 0xbb5fd, 0xbb779; push 0xbb4cf, 0xbb64b, 0xbb7c7; dispatch
   0xbb509, 0xbb685, 0xbb801.
10. **`sub_198bb0`, ruled out.** Render thread, from `sub_d0040` (call at 0xcf83a). Blocks: `lVar22` (alloc
    0x199305, decomp 256; fills 257-413; SLOT 414), `plVar24` record (0x1996ea), `lVar22` again (alloc
    0x199790, decomp 433; fills 434-606; SLOT 606), `piVar25` (decomp 622, fills 633-637), `lVar20` (alloc
    0x199d5d, decomp 671; fills 677-825; SLOT 827), record 0x19a11a (decomp 837), dispatch 0x19a183
    (decomp 846). All stores to each block precede its SLOT and its record. The `FUN_000937c0` binds at
    decomp 251 and 428 come before the alloc of the block they bind next.
11. **`sub_19c7e0`, ruled out.** Mask 0x12820 at 0x19c9e8, wrap 0x19ca1f (GPU-written, see row 5). 0x160 block at
    0x19cbd6, `import_memcpy` of `local_1e8` at decomp 194, SLOT `param_3[0x16]` at decomp 195. Two
    wrappers (0x19cc3b, 0x19cc74) take CPU addresses only. The 0x50 block at 0x19cce4 is filled at decomp
    219-230 (`puVar8[…]` stores, including a duplicate store of `puVar8[6]`, all before the record at
    0x19cd9e). Dispatch at 0x19ce03. The write `*(param_3[7] + 0xa0 + …) = uVar7` (decomp 253) is after
    the dispatch and targets a CPU list.
12. **`sub_19c1a0`, ruled out.** Render path via `sub_c6bd0` (`BPXPR_CullShadowScenes`). Masks at
    0x19c238 and 0x19c28b (GPU wraps 0x19c273, 0x19c2cd), 0x160 block at 0x19c435 (memcpy at decomp 123),
    wrap 0x19c483, bind 0x19c512, desc emit 0x19c67b, dispatch 0x19c6ac. Decomp 175-176 write
    `lVar9+0x20/+0x60` after the dispatch: per-cascade slots in a context object, not arena memory.
13. **`sub_164a60`, ruled out.** Five descriptor allocs (0x164b3f, 0x164b63, 0x164b8b, 0x164bb2,
    0x164bdc). Fills at decomp 53-122 (all before the call to `sub_163d10`). The descriptor emit
    `sub_861ce0` (0x164e26) copies the V# values from the heap objects into PM4. Draw `sub_94ec0` after.
    No store after the emit.
14. **`sub_164f30`, ruled out.** Four allocs (0x16500f, 0x165033, 0x16505a, 0x165084), same order,
    descriptor emit 0x16529c.
15. **`sub_1371e0` (+ `sub_137b90`, `sub_137fb0`, `sub_1375e0`), ruled out.** Two branches on
    `param_2[1]`. Each allocates a 0x80 block (0x1373a2 or 0x13745a) and an 0xa8 block (0x1373cb or
    0x137483), copies with `import_memcpy` at alloc (decomp 91-96, 106-111), then calls an emitter
    (137b90 + 137fb0, or 1375e0 + 1379e0). The emitters allocate their own blocks (0x137e16, 0x138009,
    0x137845), fill them, and emit (desc 0x137f05, 0x1380ca; flush 0x137965; dispatch 0x137f66,
    0x138192, 0x13798d). The emitters receive heap objects (`param_1+0x80`, `param_1+0xa8`), not
    block pointers, so their writes stay on their own blocks.
16. **`sub_10cf60`, ruled out.** Blocks: lVar7 (alloc 0x10d034, store decomp 39), piVar5 (alloc 0x10d06e,
    stores 42-54, SLOT 55), puVar6 (record 0x10d0ca, store decomp 57). Push 0x10d134 (decomp 65), flush
    0x10d159, dispatch 0x10d179 (decomp 72). No store after 57 to any block.
17. **`sub_10d890`, ruled out.** Blocks: lVar7 (0x10d969), piVar5 (0x10d9a2), puVar6 (0x10d9fe). Push
    0x10daa6, dispatch 0x10dadd. Stores at decomp 38, 41-53, 56, all before the push.
18. **`sub_1051f0` (ParticleRenderer::UpdateSimulation variant), ruled out for after-emit writes.** Reached
    by the tail jump at 0x103abc from `sub_103aa0`. Two top-of-function allocs (0x1056af, 0x1056c0,
    decomp 141-142) feed `sub_101720` (ParticleRenderer_ShiftRecords), which `memcpy`s records into
    them at 0x1017c2 and compacts their index list. No emit in this function references those two
    blocks (the variable is reused as a scalar at decomp 227 onward). Their consumer is outside this
    function (see section 9). Per branch, one block: block A (alloc 0x105a50; stores decomp 223-244; SLOT
    `*(param_2+0xb8)` at 248; record 0x105b19; push 0x105b8f; dispatch 0x105bff; no store to A after
    244). Blocks B (0x106086, stores 388-445) and C (0x1061da, stores 446-500) are the two arms of one
    `if`, so one of them runs per iteration; both set the slot at decomp 502/505 and push at 0x1063ee
    (decomp 513). Slot `+0xb8` is re-pointed each iteration, never written through.
19. **`sub_103aa0` (wrapper), no direct caller.** Two tail jumps: 0x103ab4 to `sub_103ad0` and 0x103abc to
    `sub_1051f0`. The function address appears as a 32- and 64-bit value in `out/boot-linked.bin`
    (offsets 0x15d07c0 and 0x2be61d0), so it is in a pointer table. Owner not identified.

**Job path check.** `sub_d04e0` (called from `EndFrameViewParams` at 0xcfa57) calls
`sub_b5050` (returns a flag, `[rsi+0x48]+0x15`). If the flag is set, `RunFunctionAsRenderJob` (0xd0572)
kicks the job whose body (index 0x104, inferred to be `sub_b5060`) runs `b50c0`, `b5b10`, `17ecf0` or
`17f790`. Then `sub_d04e0` calls `sub_b9610` on the calling thread at 0xd057e unconditionally, which
calls `sub_b6590` (0xb96b3; `b5b10`, `17f790`, `b6e20` inside), and `sub_b9710` (0xb96c3). Whether the
job and the render-thread path can run in the same frame, and whether they share a ring, was **not** read.

## 8. Method and limits

- The scan is at decompile level. For each open function (plus the three allocating emitters), every
  alloc result variable was tracked from its alloc to its next plain re-assignment. A store is flagged
  when it writes through that variable after a reference emit (`sub_85ecd0`, `sub_862490`, `sub_86b9e0`,
  `sub_883870`, `sub_85f220`, `sub_85e5d0`, `sub_884240`) that followed the alloc. A `memcpy` into the
  block counts as a fill. Zero flags in all 22 functions. The flag rule was checked on the known
  false positive `lVar8` in 0xb9710 (re-assigned to the context pointer) and then corrected.
- The scan can miss a write done through a second variable or through a callee given the block
  pointer. The callees that receive block pointers were read: `sub_101720` (in 1051f0, runs before any
  emit), and the three emitters (they receive heap objects, not blocks).
- Store windows in section 7 are address windows between call sites, not per-instruction register
  traces. Register-level confirmation for each store was not done.
- Nothing was built or run. `out/re/decomp/` and `eboot_re.py` were used as they are.

## 9. Open items

1. **Who consumes the two top-of-function blocks of `sub_1051f0`** (0x1056af, 0x1056c0, filled by
   `sub_101720`). Their heap descriptors are at `lVar26+0x80` and `+0xa8`. Find the PM4 that reads
   those descriptors (likely the particle draw or compute, 0x1038a0 or 0x109970).
2. **Caller of `sub_b5060`** (the job body, if index 0x104 is correct) and the table entry for job
   0x104. Confirm that the job and the `sub_b6590` path do not write the same ring in one frame.
3. **Caller of `sub_103aa0`.** The pointer is in the linked image at 0x15d07c0 / 0x2be61d0. Map that
   offset to a runtime owner (vtable or job table).
4. **Frame distance of the RSF reset.** Confirm which heap `sub_1d230` resets when called from
   `0x107540`, and whether that heap is the 2-slot RSF ring of `PC-MEMORY-REWRITE.md`.
5. **Free path of `sub_1d750`.** Read the free function and `sub_16f100` (deferred frees, 0xd08d9). A
   chunk freed after a submit and reused in the same frame is the only intra-frame reuse left.
6. **Attribution run** (section 6). This decides between section 4's cross-frame explanation and an
   intra-frame writer that the static pass did not find.
