# 01 - Per-frame render context chunks (DCB/CCB)

Read-only RE. Addresses are eboot VAs. Labels: **observed** (decompile line or data read), **inferred**, **unknown**.
Sources: `out/re/decomp/<hi>/<addr>.c`, eboot data via `tools/re/eboot_re.py` Image.read. Prior reports 01-cmdbuf-lifetime and 06-render-frame read first.

## 1. Answer (short)

- **Chunk sizes (observed):** DCB = `0x500000` (5 MiB) by default; CCB = `FUN_0085d770(0x10) = 0x10 * 0x9bc0 = 0x9bc00` = 637,952 B (623 KiB, not the 610 KiB in the brief). Table `0xfa1810` gives DCB sizes by flag index: 0, 1, 2, 3, 4, 5, 6 MiB (index 0 means default 5 MiB).
- **Per frame, fixed:** 2 contexts (FrameStart 0x90470 via 0x90860, and UpdateGPUData 0x118100 via cfdb0). **Conditional:** 1 more (caed0, 3 MiB DCB). **Variable:** 43 of the 48 `RunFunctionAsRenderJob` (0xc12a0) call sites can each request one more context per invocation, multiplied by loops whose bounds are not static.
- **Reclaim (partly observed):** the DCB/CCB scopes (ctx+0x3c0 "RenderContextCPU", ctx+0x4a0 "RenderContextGPU") are freed only by `mem_allocator_free_all` in teardown (0x90050). I found **no per-frame reclaim path for them**. The only per-frame retirement is a 2-slot ring in RenderBackend_Frame (0x90ea0) that waits on a GPU label and frees pending deletes. **Unknown and important:** this does not explain how 5+ MiB per frame is recycled. Needs a runtime counter.
- **Writes after submit:** the job-counter wait before submit (0xd07c0) suggests each chunk is complete before its submit. Nothing in 0xd07c0 writes into chunk memory. **inferred.**

## 2. Chunk creation (observed)

| Step | Address | Evidence |
|---|---|---|
| `PerFrameRenderContexts::RequestNewRenderContext(list, dcbSize, ccbSize, this)` | 0x90470 | DCB size default `0x500000` if arg 0; CCB default `FUN_0085d770(0x10)` if arg 0 |
| Create context object `0x238` bytes; DCB from scope this+0x3c0 via `FUN_00012820(size,1,0x80)` | 0x917c0 | `FUN_00012300(param_4)` pushes scope, `FUN_00012820` allocates from TLS top (category 1), `FUN_00012350` pops |
| CCB from scope this+0x4a0, same pattern | 0x917c0 | second push/pop pair |
| Writer init: DCB writer capped at `0x3ffffc` bytes per region | 0x862690 | `min(words*4, 0x3ffffc)` |
| Refill callbacks are stubs returning 0 (`sub_91790`, `sub_917a0`, `sub_8627f0`) | 0x91790, 0x917a0, 0x8627f0 | `return 0`; set at ctx+200 and ctx+0x2e8 (0x917c0) |
| Scope ctor: "RenderContextCPU" type 1, "RenderContextGPU" type 2, "RenderContextFrameCPU/GPU" (vtable 0x15976c8) | 0x8f6f0 | names and `0x47b20`/`0x475c0` calls |

So there is no continuation chunk. A context is one fixed allocation; overflow returns 0 (observed in 0x862800 main-region branch).

## 3. Question 1: requests per frame

### 3.1 Direct, per frame (observed)

| # | Caller chain | Request | Condition |
|---|---|---|---|
| A | cefa0:311 region -> 0x118440 (FrameStart) -> 0x90860 line 121 -> 0x90470(0,0) | DCB 5 MiB + CCB 623 KiB | unconditional |
| B | cefa0:311 -> cfdb0:71 -> 0x118100 -> 0x90430(0,0) | DCB 5 MiB + CCB 623 KiB | unconditional in cfdb0 |
| C | cefa0:339 -> caed0:188 -> 0x90430(0x300000,0) | DCB 3 MiB + CCB 623 KiB | at most one; only if some entry of list `caed0.param_2+0x1a8..+0x1b0` (stride 0xa8) has `[+0x90] != [+0x98]` (break after first) |

Count per frame from A-C: **2 fixed + 0 or 1 conditional**. The caed0 call from ccdf0 (RenderCubeMapFace) adds more; count unknown.

### 3.2 RunFunctionAsRenderJob (0xc12a0) sites

Flags are the 4th argument. Logic (observed, 0xc12a0): `f&2` requests a context of size `table[(f>>8)&7]`. `f&1==0 && f&6!=0` with the inline condition (`[[param_2[2]+8]+0x13]==0`) runs inline and requests a 5 MiB context. Otherwise (job path) `f&4` calls 0x90670, which creates a 0x1af0 object with a CPU and a GPU allocation of sizes `fa17a0[idx]` (128 KiB/32 KiB for idx 0, 32 KiB/8 KiB for idx 1, and so on), but only when `*(param_2[9]+0x15)!=0`.

| Flag | Sites | Context per invocation |
|---|---|---|
| 0x001 (no bit 2/6 path) | 5 (c7560:160, c7850:385, c8110:387, c89f0:355, c9160:188) | none |
| 0x002 | 12 (c5130, c5300, c53d0 x2, c59d0:378, caca0, cadd0, ca6d0 x3, caed0:600, caed0:618) | 5 MiB (idx 0 = default) |
| 0x004 | 15 (c2380, c2460, c2560, c2650, c2750, c6550, c59d0:308, c59d0:362, caed0:183/233/248, cfdb0:52/69, d0040:73/135) | inline: 5 MiB; job path: 0x90670 pair only |
| 0x104 | 1 (d04e0:32) | inline: 5 MiB; job: 0x90670 pair (32 KiB/8 KiB) |
| 0x302 | 6 (c59d0:205/323/339, cc690:100, cc9b0:106/122) | inline: 5 MiB; job: 3 MiB |
| variable | 9 (caed0:263/284, cff50:34, cefa0:328/353, d0040:91/109/151/164) | depends on flag expression |

Total 48 sites. 43 of them can request a DCB per invocation.

### 3.3 Loop multipliers (observed headers, bounds unknown)

- caed0 for-loop over `+0x1a8..+0x1b0` stride `0xa8` (lines 197-): count = list length; **unknown statically**.
- c59d0 do-loop at L342 (sites 205/362/378) and cc9b0 do-loop at L82 (sites 106/122): iteration bounds **unknown** in this pass. Report 06 names suggest cascades 0-4 and spots 0-3, not verified in code.
- ca6d0 do-loop at L101, cc690 do-loop at L63: bounds unknown.
- d0040 for-loop over list at `+0x220` (L114): length **unknown**.
- cefa0 while-loop at L243 (sites 328/353): bound unknown.

**Answer to Q1:** a finite static upper bound is not derivable from the callers alone. Formula: per frame DCB requests <= 2 + 1 (caed0) + sum over the 43 DCB-capable sites of their loop iteration counts (each invocation at most one). Loop-free sites give exactly one each. Loop bounds need the list lengths at runtime. Recommend a counter at 0x90470 per frame.

## 4. Question 2: lifetime

| Object | Reset / free | Evidence |
|---|---|---|
| Context list V (this+0x18) | header zeroed each frame in SetupFlipState | 0x90860 start: `pauVar3 = *(this+0x18)` and self-link writes |
| Frame scopes this+0x580 and +0x670 ("RenderContextFrameCPU/GPU") | ring rotate each frame: slot+0x20 = 0x48270 | 0x90860 vcall `(*(...)+0x20)`; 0x48270 rotates ring at `+0x80` with length `+0xb8`, index `+0xbc` |
| DCB/CCB scopes this+0x3c0 / +0x4a0 | `mem_allocator_free_all` (0x47620) only in 0x90050 (teardown) | 0x90050 lines `FUN_00047620(param_1+0x3c0)`, `(+0x4a0)`; 0x90050 callers: 0x8d620 (destroy), 0x8d660 (replace singleton), 0x2556c0 |
| Typed scope vtable dtor 0x90420 / 0x91270 / 0x8da10 | `free_all` on destruction | 0x90420, 0x91270, 0x8da10, 0x8c360 call 0x47620 |
| Pending delete deferral | RenderBackend_Frame (0x90ea0) once per frame, from EndFrameViewParams 0xcfd6c after the final submit | toggles ring index `+0x7b0` in {0,1}; waits until GPU label `*(+0x770+idx*8)` equals `*(+0x780+idx*8)`; then `FlushPendingFree(deleter,0)` (0x8ef20) swaps generations and runs `DeletePendingItems` (0x8f180 -> free) |
| Label table | 1024 qwords at +0x7e8 | 0x8fe..: label slot pointers appended per frame (up to 0x400) |

- **observed:** the GPU-label wait in 0x90ea0 is a two-frame ring. It waits for the GPU to finish the frame that last used the slot (N-2). That is the double-buffering signal, not a per-chunk one.
- **observed:** `FrameAllocateJanitor` (0xf973a1) is the job system's own scope. JobSystem::Initialize (0x335d0, string at 0x33674) creates it with 0x475c0 and pushes it. The four named users (0xa46640, 0xa47d60, 0xa48280, 0xa4ab90) are save/entitlement workers that reference the same scope name. **No renderer reference.** The render contexts are not in the janitor.
- **unknown (main gap):** I found no path that returns individual context DCB/CCB blocks to the scope pool per frame. The sub-allocator at scope+0xb8 (tried first by 0x47e30, the typed scope alloc at vtable slot 20) is not identified, nor is its reset. At 5 MiB per frame, a reclaim must exist somewhere. Candidates: that sub-allocator being a ring reset by the frame ring, or the scope being recreated. **Not resolved.**

## 5. Question 3: DCB bump allocator 0x86d210

- **observed:** `FUN_0086d210(writer, bytes, alignExp)` reserves `(bytes+3)/4` + padding + 3 dwords, writes header `0xc0001000 | (words<<16)` (PM4 type-3, opcode 0x10 = NOP-type payload) and `(alignExp<<12) | 0x68750000` (tag), and returns the aligned payload pointer. This is an **embedded data block** in the DCB stream.
- **observed callers (12):** 0x92720, 0x92b50, 0x93960, 0x927d0, 0x932a0, 0x92640, 0x92850, 0x94320, 0x95420, 0x85c9b0, 0x85cb40, 0x85dc50.
- **inferred:** these are constant buffers, shader constants and user data written into the DCB. Labels are not visible in this allocator.
- **unknown:** bytes per frame. Derivable only by a counter on `writer.cur` deltas per context, or by summing the call sites' size arguments with their loop counts.

## 6. Question 4: write timing around submit

- **observed:** 0xc5560 builds the submit params (DCB vector at +0x210, CCB vector at +0x410). 0xd07c0 first waits on the job counters listed in the params (`FUN_00035590`), then for each DCB calls `FUN_00091f80` (submit), then for each CCB `FUN_0016f100`, then (mode 1 only) the doorbell and `sceGnmSubmitDone`.
- **observed:** the render job that fills a context is itself waited on (`FUN_00035590(param_5)` in 0xc12a0 before inline work). So the CPU writes to a chunk complete before the submit that consumes it.
- **observed:** 0xd07c0 writes nothing into chunk memory.
- **inferred:** each job with flag 0x2/0x302 owns a fresh context, so no context is written after its own submit. Post-submit writes into a reused chunk would require reuse, which I did not find (see Q2).

## 7. Open questions (ranked)

1. How DCB/CCB scope memory is reclaimed per frame (sub-allocator at scope+0xb8, its reset). Settle by logging scope+0xb8 cursor and `free_all` calls per frame.
2. Loop bounds for caed0 list (+0x1a8), c59d0/cc9b0/ca6d0/cc690/d0040/cefa0 loops. Settle with a runtime count of 0xc12a0 invocations per frame by flag.
3. Bytes per frame through 0x86d210 (embedded data) and whether the 0x3ffffc writer cap is ever hit.
4. Caller of 0xc2380..0xc2750 and whether the inline flag `+0x13` is set at runtime (decides 5 MiB vs job path).

## 8. Ranked summary

1. Chunk sizes: DCB 5 MiB default, CCB 623 KiB (0x9bc00), table 0-6 MiB.
2. Fixed per frame: 2 contexts (FrameStart, UpdateGPUData), plus caed0 3 MiB (conditional).
3. Variable: 43 of 48 RunFunctionAsRenderJob sites, times loop counts (bounds unknown).
4. No per-frame reclaim found for DCB/CCB scopes; teardown only (0x90050).
5. Per-frame retirement is a 2-slot ring with a GPU label wait in 0x90ea0 (double buffering).
6. Frame scopes 0x580/0x670 ring-rotate each frame (0x48270).
7. FrameAllocateJanitor is the job system and save workers, not the renderer.
8. Context chunks are fixed size: refill callbacks return 0, so no continuation chunk.
9. No writes after submit found; submit waits on job counters first.
10. Unknown: reclaim mechanism and per-frame byte totals. Needs a runtime counter at 0x90470 and 0x86d210.

Stray files: during this pass a helper script was written to `$HOME/rjsites.py` and removed; a stray empty `/sites.py` may exist at the drive root (created by an unset `$TMPDIR`), and the removal of it was blocked by a safety check. Nothing else in the repo was changed.
