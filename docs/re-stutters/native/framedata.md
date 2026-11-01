# N0 framedata: per-frame render data, command buffers, descriptors, submit (revision 2)

Read-only RE. Base: main tree `C:\Users\ptr\Documents\sotc-pc` (branch re/pc-native). Decompilation: `out/re/decomp/<first 4 hex digits of the 8-digit address>/<8-digit address>.c`. Addresses are eboot VAs. Labels: **observed** (decompiled code), **inferred**, **unknown**. Not committed.

Revision 2 corrects three errors in revision 1 (marked **[corrected]**) and closes open questions 1, 2, 4, 5, 6, 7, 8 and specifies the counters for 3.

## 0. Corrections to revision 1

- **[corrected] The per-frame block allocator does not use a renderer scope.** `0x8c0d0(param_1, size)` treats `renderer+0x480` (or +0x458) only as a result handle (`*param_1` = block pointer, `param_1[1]` = high bits). The allocation is `0x12820(size, category DAT_02ed1f54 = 1, align 0x10)`. Category 1 is **CPUMemory** (see 1.1), not a frame heap.
- **[corrected] `0x4100000` is not a flip packet.** In `0x90860` line 69, `0x4100000` (65 MiB) and `0xa00000` (10 MiB, line 102) are the budgets of the render-context pools, printed as `Render Context memory CPU/GPU: used / budget` (debug only, flag `DAT_02ed3458`).
- **[corrected] `0x85c580`, `0x85c9b0`, `0x85cb40` are not T# builders.** See 6.

## 1. Memory heaps (Q1, observed)

### 1.1 Category table and the default allocator
- `0x13380` (`GetDefaultAllocator`, called by `0x12820` on first use and by `0x110a0`) creates **CPUMemory** (`0x47b20(…,"CPUMemory",1)`, object `DAT_02dfb500`), stores it in slot 1 of the category table `DAT_02dfaab0[]` (`DAT_02dfaab8` = slot 1), and sets `DAT_02dd9f7c` = its category index. Its BucketAllocator (page `0x10000`, buckets 0x10..0x400) and HeapAllocator are created here.
- `0x110a0` (`Memory_InitAllocators`, 2,739 bytes) creates **GPUMemory** (`0x47b20(…,"GPUMemory",2)`, `0x17070(0xe0)`), registers it at the first free slot (slot 2), and saves its category index in `DAT_02dd9f84`. Its allocator stack is the same shape as CPUMemory (bucket 0x400, heap 0x100000, page -1 via `0x47dd0`). It also creates `Default`, `TFrameAllocators`, `STL Containers`, `BucketAllocator`, `HeapAllocator`, `PageAllocator`.
- **Consequence:** `0x8c0d0` allocates from category 1 = CPUMemory, unless the calling thread has a scope pushed on its TLS stack (1.2). So the instance and skinning blocks written by `0x123410` and `0x11c710` are **CPU-typed memory**, not `RenderSingleFrameGPU`. This explains the reports/03 result that untracking `RenderSingleFrameGPU` did not help.
- `0x8be60` (resource creation, section 6) picks the backing category by flag: bit 0 set gives `DAT_02dd9f80` (CPUMemory index), otherwise `DAT_02dd9f84` (GPUMemory index). The GPU-visible resources (textures, buffers) therefore come from GPUMemory.

### 1.2 TLS scope stack and scope objects
- `0x12300` (`ScopePush(scope, info)`) appends `(scope, info)` to the thread's TLS array at `TLS+0x400` (count) with 0x20-byte entries. `0x12350` pops. Only 12 functions push: `0x917c0` (DCB/CCB, pushes `renderer+0x3c0` and `+0x4a0`), `0x90470` (pushes `+0x3c0`), `0x906d0` (compute context, pushes `+0x3c0`), `0x335d0` (JobSystem init, FrameAllocateJanitor), `0x16e990`, `0x1b2390`, `0x1b32f0`, `0x19aad0` (1 MiB heap scope), `0xa4ab90`, `0xa46640`, `0xa47d60`, `0xa48280` (save/entitlement workers). **None of the render-frame code paths push `RenderSingleFrameGPU` or `RenderSingleFrameCPU`.**
- `0x12820(size, cat)` with a non-empty TLS stack and `cat == 1` allocates from the top scope's vtable `+0xa0` (`0x47e30` for generic scopes). With an empty stack it uses the global slot `DAT_02dfaab0[cat]`.
- `0x47e30` (scope allocate) tries the scope's own sub-allocator at `scope+0xb8` first (vtable `+0xa0`), then the scope's heap array at `scope+0x90` (pairs of allocator and max size, count at `+0x80`). That is how the size classes route to bucket, heap and page.
- `0x475c0` (scope ctor, `FUN_000475c0(obj, name, memtype)`) stores the name and memtype. The scopes created by `0x8c880`: `GameToRenderThread`, `GameToGPU` (5), `CPU_RenderToGPUToGame` (6), `RenderSingleFrameGPU` (`DAT_02ed29c0`, memtype 2), `RenderSingleFrameCPU` (`DAT_02ed2ca0`; the ctor call passes memtype 2, as for the GPU scope, and the memtype in the list is not verified). `0x8c370` initialises them (vtable `+0x40`, `+0x50` with size 0). They are configured, never pushed.
- `renderer+0x3c0` (`RenderContextCPU`, memtype 1) and `renderer+0x4a0` (`RenderContextGPU`, memtype 2) are the scopes for DCB and CCB. The scope names and memtypes come from `0x8f6f0` (renderer ctor).

### 1.3 Physical heap answer (Q1)
- Per-frame instance and skinning blocks (`0x8c0d0`): **CPUMemory**, category 1, no scope push in the frame path. The memory-type of the GPU reads is therefore CPU-typed memory, which the port tracks as guest memory.
- DCB and CCB (`0x917c0`): `RenderContextCPU` (memtype 1) for DCB and `RenderContextGPU` (memtype 2) for CCB. The CCB is GPU-typed.
- Frame scopes `this+0x580` and `+0x670` (`RenderContextFrameCPU/GPU`) rotate each frame through `0x48270` (section 2.2), but they are not pushed by any render code path found.

## 2. Reclaim per frame (Q2)

### 2.1 Deferred-delete machinery (observed)
- The renderer deleter is `renderer+2000` (`*(renderer+0x7d0)`, `RenderDeferredMemoryDeleter`). Its item lists are at `+0x60` (pending) and `+0x68` (end). Generations: `+0x8/+0x9/+0xa` and `+0xc/+0xd/+0xe`.
- `0x8ef20` (`FlushPendingFree(deleter, wait)`): if a job handle at `+0x10` is set, waits (`0x35730`) unless `wait == 0` and it is not set; swaps the two generations; then runs `DeletePendingItems` (`0x8f180`), which calls `0x12af0(item)` (free) for each pending block. **Items are raw block pointers returned by `0x12820`.**
- `0x90ea0` (RenderBackend_Frame) calls `0x8ef20(deleter, 0)` once per frame, after the GPU-label spin (section 4, site 15).
- `0x91010` and `0x910d0` (guarded by the lock at `renderer+0x7c0`) call `0x8ef20` three times, the three-generation flush used at shutdown and by `0x9ac10`. `0x8ed90` and `0x8ee00` do the same at teardown.
- **Producer:** the code that appends a block to the pending list (the deferred `free`) is not identified. Candidates: a free helper that the `0x12af0` callers use, or the `renderer+0x7c0` path. **Unknown.**

### 2.2 Per-frame blocks (`0x8c0d0`)
- `0x123410` and `0x11c710` allocate on every call (`0x8c0d0` always calls `0x12820`) and overwrite the handle. I found no `free` in `0x118100`, `0x123410`, `0x11c710` or `0x8c0d0`. Either the block returns through the deleter (producer unknown, 2.1), or the category-1 heap has a generation reset I did not find, or the blocks leak. **Unresolved, and the leak case is excluded by memory budget only if the deleter producer exists.** Decisive counter in section 7 (item 4).

### 2.3 DCB and CCB contexts
- `0x90860` (SetupFlipState) clears the context list `this+0x18` every frame and calls `0x90470`, which creates a 0x238-byte context object (`0x17070`), the DCB and CCB through `0x917c0`, and appends it to the list (grows the array with `0x17090`, old array freed `0x170c0`).
- The context object's destructor and the free of its DCB and CCB blocks: not identified. `0x90050` (teardown) frees the scopes with `0x47620` (free all) and the deleter with `0x8ed90`. **So per-frame DCB and CCB reclaim is unresolved.** The two-generation deleter with a GPU-label gate (2.1) is the only mechanism that matches a two-frame reuse, and it is the likely path for the context object, but I have not seen the producer.
- Budget check: `renderer` prints `CPU: used/0x4100000` and `GPU: used/0xa00000`. A CCB is 0x9bc00 (623 KiB), so a 10 MiB GPU budget holds about 16 CCBs. The steady state therefore needs reclaim within a few frames. The budget printout (debug flag `DAT_02ed3458`) gives the used and budget numbers, which is the verify-mode check.

### 2.4 `0x47620` (scope free all)
- Walks the scope's allocation list (`+0x48`), frees each block with `0x1b0e0` and resets the list. Callers: `0x90050`, `0x8d620`, `0x8d660`, `0x2556c0` (teardown), `0x335d0`, `0x13e30`, `0x48790`, `0x481d0`, `0x48770`, `0x35820`, `0x15160`, `0x1cf10`, `0x18a10`. The render frame does not call it.

### 2.5 `0x48270` (frame ring rotate)
- Rotates a ring of generation heads at `+0x80` with count `+0xb8` and index `+0xbc`. When a slot is reused, its old head is released. This is the per-frame generation mechanism for the frame scopes (`this+0x580/+0x670`), reached through vtable `+0x20`. Its callers are indirect.

## 3. Bytes per frame (Q3): counters for verify mode

Verify mode must record, per frame, per thread, and per 5-second window:
1. **DCB bytes:** the writer cursor delta per context in `0x86d210` (sum of `(words+padding+3)` per call), split by the caller address (`__builtin_return_address(0)`), and the final DCB size at `0xd07c0` before each `0x91f80` (the `[start, cursor)` range is also the announce range).
2. **CCB bytes:** the same at `0x16f100`.
3. **Per-frame heap bytes:** the sum of `size` argument of `0x8c0d0` per call and call count, by caller (`0x123410`, `0x11c710`, others), plus the category and the TLS-stack depth at entry (`TLS+0x400`).
4. **Heap pressure:** `0x12820` calls per category, sum of sizes, failures (the `0x124f0` fallback path), and `0x47e30` sub-allocator hits per tier (`+0xb8`, heap array tiers).
5. **Deleter:** items appended and freed per `0x8ef20` call (count and bytes via `0x12af0`), and the pending-list length at each `0x90ea0`.
6. **Contexts:** `0x90470` calls per frame, DCB and CCB sizes requested, the context list length after `0x90860`, and the budget line (`Render Context memory CPU/GPU`) when `DAT_02ed3458` is set.
7. **Resources:** `0x8be60` calls per frame with size and flag (CPU or GPU category).
8. **Constant V# writes:** count of `0x861ce0` and `0x861e60` calls and slot writes per stage.
9. **Dispatch:** `0x928f0` calls per frame, with the two buffer sizes and the dispatch width.
10. **Totals:** bytes per frame by class (DCB, CCB, per-frame heap, resources) and per second. The 5-second line is printed as `SOTC FRAMEDATA`.

## 4. Inventory (revision 2)

Classes: **PF** per-frame CPU write, **PERS** persistent CPU write, **GPU-RES** GPU-written result, **SYNC** CPU wait, **DESC** descriptor build, **SUB** submit, **CPU-STATE** CPU-only, no GPU data.

| # | Eboot site (decomp) | Class | What it writes / reads / waits | Address source | Frequency | Patch point (proposed) | Evidence |
|---|---|---|---|---|---|---|---|
| 1 | `0x118100` UpdateGPUData (0011/00118100.c) | PF | Walks the dirty bitmap at `param_1+0x210070` (4096 bits), dispatches class writers (section 5), `memset(bitmap, 0, 0x8000)`, then `0x193e40`-style counter and `0x92720` (EOP label). | renderer `DAT_02edf140`, bitmap CPU state | once per frame, from `0xcfdb0` | Entry: note bitmap. Tail before `0x92720`: announce instance span. | 0011/00118100.c |
| 2 | `0x123410` InstanceData writer (0012/00123410.c) | PF | Allocates a 0x78-byte block (`0x8c0d0`, CPUMemory), fills it from the instance object (`+0x14..+0x118`), two `0x928f0` calls, writes block state back (`+0xe0`, `+0xd70`). | instance object, `0x12820` cat 1 | per instance class 1 per frame | Allocation point: route to host-shared ring (N1). | 0012/00123410.c |
| 3 | `0x11c710` InstanceDynamicData (0011/0011c710.c, caller `0xca6d0`) | PF | Dynamic instance data, same `0x8c0d0` funnel. 0.8% of write faults. | | per frame | as 2 | reports/03 |
| 4 | `0xbe1f0` SetViewParams (000b/000be1f0.c) | PF | View constant block (0x40-byte view record via `0x122d90`, light records via `0x1936d0`). Callers `0xc6810`, `0xca6d0`, `0xcaed0`, `0xd04e0`. | view object, heap block | 3 to 4 per frame | Allocation to host-shared ring. | 000b/000be1f0.c |
| 5 | `0x928f0` compute with 2 buffers (0009/000928f0.c) | DESC + PF | Two buffer V# (`0x865410(addr, 0x4404, count)` with `0x865360` attrs, and a constant V# `0x8655d0`), user-data writes `0x861ce0` (slot 0x10) and `0x861e60` (slot 0x6d), an 8-byte `0x12820` block (count, stride), then `0x86b9e0` DispatchDirect with `x = (count+63)/64`. Callers: `0x123410`, `0x92b50`, `0x93960`, `0x927d0`, `0x932a0`, `0x92640`, `0x92850`, `0x94320`, `0x95420`, `0x1018c0` (particle stats), `0x18f830`, `0x18a000`, `0x117930`. | guest addresses from args | every call | Announce source ranges; descriptor index (site 6). | 0009/000928f0.c |
| 6 | `0x861ce0` / `0x861e60` user-data SGPR shadow (0086/00861ce0.c, 00861e60.c) | DESC | Writes a 16-byte V# into the CPU shadow `renderer+0x90 + stage*0x600*4` at slot `table[stage] + 0x22` (`0x861ce0`) or `+0x42` (`0x861e60`). Index 16-bit from `renderer+0xc180 + stage*8`. Negative index: immediate `SET_SH_REG` through `0x86d5e0`. Flag `renderer+0xc613` adds a second path `0x884240`. | slot tables from the shader binding | every bind | Single funnel. Bindless: store a 32-bit index. | 0086/00861ce0.c |
| 7 | `0x865410` V# buffer encoder (0086/00865410.c) | DESC | Words 0/1 = guest address (44 bits), word 2 = count, word 3 = format from `0x866b30`. Callers `0x16eb90`, `0x928f0`, `0x85c580`, `0x92bd0`. | guest pointer | per binding | Side record (VA, size, format) at encode time. | decomp |
| 8 | `0x8655d0` constant V# (0086/008655d0.c) | DESC | Words 0/1 = guest address, word 2 = `(size+15)>>4`, word 3 = `0x20077fac`. | guest pointer | per constant | Side record. | decomp |
| 9 | `0x86d210` DCB embedded-data allocator (0086/0086d210.c) | PF | Reserves `(bytes+3)/4 + pad + 3` dwords in the DCB writer. Writes `0xc0001000 | words<<16` (a PM4 type-3 NOP header) and tag `0x68750000 | align<<12`, so the payload is NOP-wrapped and skipped by the CP. Returns the aligned payload. 12 callers. | DCB writer `+0x10` cursor | per embedded block | DCB range `[start, cursor)` announced at submit (site 12). | 0086/0086d210.c |
| 10 | `0x90470` RequestNewRenderContext (0009/00090470.c) | PF / SUB | Context object (0x238 bytes), DCB 0x500000 (default), CCB `0x85d770(0x10)` = 0x9bc00. Initial PM4 state (`0x86f1d0`, `0x869290`, `0x87c7a0..0x87c8c0`, `0x86ded0`, `0x86c840`). Appends to context list. | scopes `+0x3c0`, `+0x4a0` | 2 to 3 per frame, plus variable | Ring-allocate for N1. | 0009/00090470.c |
| 11 | `0x917c0` context ctor (0009/000917c0.c) | PF | Writers, refill callbacks `0x91790`/`0x917a0` return 0 (fixed-size, no continuation). Pushes `+0x3c0` for DCB and `+0x4a0` for CCB. | | with 10 | writer cap `0x3ffffc` (`0x862690`) | 0009/000917c0.c |
| 12 | `0xd07c0` KickoffSubmitJob (000d/000d07c0.c) | SUB + SYNC | CPU waits on job counters (`0x35590` per list entry). `0x91f80` per DCB (`sceGnmSubmitCommandBuffersForWorkload`), `0x16f100` per CCB, and in mode 1 `0x91f90` doorbell and `sceGnmSubmitDone`. | params `0xc5560` | 5 mid + 1 final | Announce point for DCB ranges before `0x91f80`. | 000d/000d07c0.c |
| 13 | `0xc5560` SubmitJob (000c/000c5560.c) | SUB | 0x610-byte params (`0x17170`), DCB vector `+0x210`, CCB vector `+0x410`. | | 6 per frame | none | 000c/000c5560.c |
| 14 | `0xc12a0` RunFunctionAsRenderJob (000c/000c12a0.c, 1461 lines) | PF + SYNC | Flag `f&2`: requests a context (size `table[(f>>8)&7]`). `f&4` without inline: `0x90670` creates an 0x1af0 object with CPU and GPU blocks. Waits `0x35590(param_5)` before inline work. 50 call sites in 26 functions (section 8). | job param block | per invocation | Ring-allocate. | 000c/000c12a0.c |
| 15 | `0x90ea0` RenderBackend_Frame (0009/00090ea0.c) | SYNC (GPU-RES read) | Toggles `+0x7b0` in {0,1}, `0x8d730` (counter at +0x10), then spins while `*(p+0x770+i*8)` != `*(p+0x780+i*8)` (label pointer vs expected value). Then `0x8ef20` FlushPendingFree, `0x8c860`, `0x32430`. | GPU labels | once per frame, after the final submit (`0xcfd6c`) | Keep as the one fence, moved to a frame ticket. | 0009/00090ea0.c |
| 16 | `0x91f80` / `0x91f90` submit thunks | SUB | `sceGnmSubmitCommandBuffersForWorkload` (port `gnmdriver.cpp:2429`) and `sceGnmSubmitDone`. | | | announce here if not in 12 | reports/06 |
| 17 | `0x92720` EOP label (0009/00092720.c) | GPU-RES (write) | 8-byte DCB slot (`0x86d210`), write 0, `0x86eb60(…, 0x28, …)` (EVENT_WRITE_EOP, section 9), then `0x86edc0` or `0x86eec0` (WAIT_REG_MEM, section 9). | DCB | after each UpdateGPUData and at frame end | Keep; it produces the labels 15 polls. | 0009/00092720.c |
| 18 | `0xbe1f0` / `0xcedf0` / `0x8b7f0` EndFrameView writers | PF | Per-frame view constants (`0xcefa0` "EndFrameViewParams"). 1.2% of write faults. | | | as 4 | reports/03 |
| 19 | `0x101720` particle record shift (0010/00101720.c) | PERS | 96-byte shift inside a 192-byte record, plus a 32-bit index. Called from `0x103ad0`, `0x1051f0`. 8.7% of write faults. | particle pool | per update | N1 `NativeWrite`, upload once. | reports/03 |
| 20 | `0x1018c0` ParticleSystemPool::ProcessStats | GPU-RES read | Reads GPU-written stats (`pool+0x188`), 3,699 read faults. Uses the site 5 compute path. | pool | per update | Consumer: particle topic. | reports/03 |
| 21 | `0xe48b0` WaterReadQueries | GPU-RES read | PM4 result packet (`0x86edc0` WAIT_REG_MEM) then CPU read in the same frame. Called from `0xcefa0` line 474. | | once per frame | Consumer: N3. | reports/06 |
| 22 | `0x8c0d0` per-frame block allocator (0008/0008c0d0.c) | PF | `0x12820(size, cat 1, align 0x10)`, stores the pointer in its handle. Callers include `0xa69d0`, `0xbb310`, `0x5de0b0`, `0x1a5490`, `0x11c710`, `0x10c4e0`. | TLS top or CPUMemory | per call | Funnel to replace (N1). | 0008/0008c0d0.c |
| 23 | `0x12820` allocator (0001/00012820.c) | PF | TLS-top scope or global category slot; calls vtable `+0xa0`; failure path `0x124f0`. | | | route categories to host-shared heaps (N1) | 0001/00012820.c |
| 24 | `0x8c880` scope init (0008/0008c880.c) | PF | Creates the named scopes (1.2). | static | init | | 0008/0008c880.c |
| 25 | `0x8f6f0` renderer ctor (0008/0008f6f0.c) | PF | Creates the 0x8b0-byte renderer (`DAT_02ee3ce0`), the scopes `+0x3c0`, `+0x4a0`, `+0x480` (handle), `+0x458` (handle). | singleton | init | | 0008/0008f6f0.c |
| 26 | `0x90860` SetupFlipState (0009/00090860.c) | PF | Context list reset (`+0x18`), frame scope rotation, budget debug print (`0x4100000`, `0xa00000`), `0x90470` (context 1), `0x95650`, `0x95690` (flip-state change), and the `"Scene"` PM4 marker (`0x86f2a0`). | renderer | per frame | Frame boundary. | 0009/00090860.c |
| 27 | `0x95760` / `0x960a0` flip thread, `0x956e0` | SYNC | Flip event queue, `sceVideoOutSubmitFlip` only in `0x956e0` and `0x95760` (mode change). | | | out of topic | reports/06 |
| 28 | `0x118470` (0011/00118470.c) | PF + GPU-RES | Calls `0x8be60` (resource creation) and `0x91f50` (`sceGnmInsertWaitFlipDone(handle, ring slot)`, section 9). | | once per frame | Resource creation here (section 6). | 0011/00118470.c |
| 29 | `0x121ed0` instance class 0 (0012/00121ed0.c, from `0x118100`) | PF | Builds a 0x40-byte transform from view state (`0x121be0`, `0x162f40`), writes to `*(param_1+0x1d8)`, a persistent block whose content is per frame. | persistent block, per-frame content | per frame | Persistent address, NativeWrite each frame. | 0012/00121ed0.c |
| 30 | `0x193e40` instance class 0xc (0019/00193e40.c) | CPU-STATE | `*(p+0x14) = *(p+0x10)` (a frame-history copy). No GPU data. | | | none | 0019/00193e40.c |
| 31 | `0x18aa20` instance class 2 (0018/0018aa20.c) | PF (transform) | Bounding-volume transform into object fields (`+0x5c..+0x88`); when `*(p+0xa8) != 0` builds a 4x4 into a block. Tail not fully read. | | per frame | Read the tail to confirm the block write. | 0018/0018aa20.c |
| 32 | `0x85c580` draw resource builder (0085/0085c580.c) | DESC + PF | Two buffer V#: `0x865410(obj+0xe0, param_2, 0x4402, param_3>>1)`, and when `param_5 != 0` `0x865410(obj+0x104, param_4, 0x4404, param_5>>2)`. Then `0x8839c0` (SH register set with `param_8`/`param_9`), `0x86b750`, `0x86f3f0`, `0x86f490`, `0x86f4e0`, `0x865f50`. Called indirectly. | guest pointers | per draw | Side record at encode. | 0085/0085c580.c |
| 33 | `0x85c9b0` DCB marker (0085/0085c9b0.c) | PF (marker) | Writes `0xddc0ddc0` into an 8-byte DCB slot (`0x86a440`), registers it with the label tracker `obj+0x40` (`0x885320`, `0x885220`), and sets a state field at `obj+0x11e`. | DCB | per context | Keep; these are label slots for the CP. | 0085/0085c9b0.c |
| 34 | `0x85cb40` EOP marker (0085/0085cb40.c) | GPU-RES (write) | 8-byte DCB slot written with `0`, then `0x86eb60(…, 0x28, …, data 0xddceddce)` (EVENT_WRITE_EOP), registered with the label tracker. | DCB | per context end | Keep. | 0085/0085cb40.c |
| 35 | `0x8be60` resource creation (0008/0008be60.c) | PF (create) | Allocates backing (`0x12820(size, cat)`, category from flag bit 0), optional `memcpy` of initial data, then per type: `0x1a` `0x865600`, `0x1b` `0x865500`, `0x1c` (further encoder). Callers include `0x118470`, `0x1671d0`, `0x19c1a0`, `0x5de0b0`. | resource object | at create | Owner of the T# base (section 6). | 0008/0008be60.c |
| 36 | `0x865600` image/buffer word encoder (0086/00865600.c) | DESC | Word 0/1 = `param_2` (base), word 2 = `param_3`, word 3 = `0x2000c004`. Callers `0x8be60`, `0x8c280`. | backing from `0x8be60` | per resource | Side record at create. | 0086/00865600.c |
| 37 | `0x95650` / `0x95690` / `0x91f50` flip helpers | PF / SYNC | `0x95650` calls `sceVideoOutSubmitChangeBufferAttribute` (flip-state, not per frame). `0x95690` flip-mode change. `0x91f50` inserts the flip-done wait for ring slot `DAT_02ed33f0+0x7b0`. | ring index | per frame (`0x91f50`) | Keep. | 0009/00095650.c, 00095690.c, 00091f50.c |

Counts by class (rows overlap by design): **PF** 17, **PERS** 1 (site 19), **DESC** 6 (sites 5, 6, 7, 8, 32, 36), **GPU-RES** 4 (sites 15, 17, 20, 21, 34 count as write or read), **SUB** 3 (12, 13, 16), **SYNC** 4 (14, 15, 27, 37), **CPU-STATE** 1 (site 30).

## 5. Instance class dispatch (Q7)

`0x118100` dispatches on the `uint32` at `class+8` (switch): case 0 → `0x121ed0` (site 29), case 1 → `0x123410` (site 2), case 2 → `0x18aa20` (site 31), case 0xc → `0x193e40` (site 30). No other classes in the switch.
- **0x121ed0:** per frame, writes into `*(p+0x1d8)`, a persistent block.
- **0x123410:** per frame, fresh CPUMemory block per call (site 2).
- **0x18aa20:** per frame, CPU transform; GPU block write only if `*(p+0xa8)` is set.
- **0x193e40:** CPU state only.
Note: `0x118100` is called once per frame from `0xcfdb0`, so the dispatch list is per frame.

## 6. T# builders and texture ownership (Q6)

- **There is no T# builder at `0x85c580`, `0x85c9b0`, `0x85cb40`.** `0x85c580` is the draw resource builder (site 32): two buffer V#. `0x85c9b0` is a DCB marker (site 33). `0x85cb40` is an EOP marker (site 34).
- **Texture and buffer V# are built by the resource object, `0x8be60`** (site 35). It allocates the backing store with `0x12820` (category from flag bit 0: CPUMemory if set, GPUMemory otherwise), stores the pointer in `resource+4`, and then encodes the descriptor with `0x865600` (case `0x1a`), `0x865500` (`0x1b`) or the `0x1c` encoder. The descriptor's base is therefore the **backing pointer owned by the resource object**.
- **Owner:** the resource object passed as `param_1` to `0x8be60`. Its base is `resource+4`; its size is `param_4 * param_3`; its type is `param_2`; its memory class is in the flag `param_5`.
- **Bindless attach point:** store the bindless index in the resource object (a field next to `+4`), and have `0x865600`/`0x865500` write the index in the descriptor (or keep the V# and map the guest VA). The creator call sites (`0x118470`, `0x1671d0`, `0x19c1a0`, `0x5de0b0`, `0x189320`, `0x368a70`, `0x19b760`, `0xfffa0`, `0x5d1540`, `0x5e2020`, `0x18c480`, `0x1848d0`) are the places where each resource is created.
- Open: the `0x1c` encoder and the full set of `0x8be60` callers (only 12 listed in the decomp header). The index-to-owner mapping for textures that come from the file loader (`0x8c280` also calls `0x865600`) needs its own pass.

## 7. Open questions (after revision 2)

1. **[closed] Physical heap of per-frame blocks:** CPUMemory (category 1), see 1.3.
2. **[open] Reclaim of per-frame blocks and DCB/CCB contexts:** the deleter (2.1) frees raw blocks after a two-generation GPU-label gate; its producer is not identified. Counter: at `0x12af0`, log the caller address for each free, and the caller of the append (`0x8ef20`'s list insertion). Decide by a steady-state run: if CPUMemory use grows per frame, the blocks leak.
3. **[open] Bytes per frame:** section 3 counters.
4. **[closed] PM4 opcodes:** see 9.
5. **[partly closed] Per-frame flip:** the engine never calls `sceVideoOutSubmitFlip` per frame. It calls only `sceGnmInsertWaitFlipDone` (`0x91f50`, ring slot `renderer+0x7b0`) and the EOP events (`0x86eb60`). So the flip request is not in the engine's code. Candidates: an EOP with INT_SEL (`0x86eb60` param 8), or a flip done by the port at `sceGnmSubmitDone`. Decisive: log every `sceVideoOut*` and `sceGnm*Flip*` import per frame in verify mode.
6. **[closed] T# builder:** see 6 (owner is the resource object).
7. **[closed] Instance classes:** see 5.
8. **[closed] RunFunctionAsRenderJob bodies:** see 8.
9. **[open] Remaining:** `0x86a440` (the `0xddc0ddc0` writer in site 33), the `0x86f3f0`/`0x86b750`/`0x865f50` calls in site 32, the `0x1c` encoder, `0x8be60` callers (12 of many).

## 8. RunFunctionAsRenderJob call sites (Q8)

50 call sites in the decomp (grep `FUN_000c12a0(`), in these functions: `000d04e0`, `000d0040` (x6), `000cff50`, `000cfdb0` (x2), `000cefa0` (x2), `000cc9b0` (x2), `000cc690`, `000caed0` (x6), `000cadd0`, `000caca0`, `000ca6d0` (x3), `000c9160`, `000c89f0`, `000c8110`, `000c7850`, `000c7560`, `000c6550`, `000c59d0` (x5), `000c53d0` (x2), `000c5300`, `000c5130`, `000c2750`, `000c2650`, `000c2560`, `000c2460`, `000c2380`.

Job bodies: each call passes a name block and a job function that runs in a job-system worker. I read the call sites and the callers' bodies (grep for every GPU-touching helper in the 26 functions):

**GPU-side waits (WAIT_REG_MEM written into the DCB by the job; the CPU does not block, the CP stalls on the label).** These are the in-stream dependencies on a label that an earlier pass wrote:
- `0x86edc0(…, mask -1, function 5, ref 1)` (wait label >= 1): `000c59d0` lines 265, 266, 345 (3 sites in the G-buffer job); `000caed0` lines 193, 270, 402, 475, 513, 628 (6 sites in the main-camera job); `000ca6d0` lines 104, 105 (2 sites in the XPR job).
- `0x86eec0(…, mask -1, ref 1)`: none of the 26 functions in the job list except through `000cefa0` (called by the frame body; see below).
- Outside the 26 functions, same subsystem: `000c2710` (2 sites), `000c6f70` (1), `000ceae0` (1), `000da110` (1), `000da560` (4), `000db230` (1, `0x86eec0`). These are GPU-side waits too and are not in the 48 call-site list.

**CPU waits on the GPU, or GPU readbacks inside a job:** none found in the 26 functions (grep for `0x8c1b0`, `0x90ea0`, `0x86eec0`/`0x86edc0` used as CPU spins, `sceKernelWait`, `sceGnmWait`, `sceVideoOutSubmit`). The CPU-side job waits (`0x35590`, `0x35730`) wait on other CPU jobs.

**CPU readbacks in the frame body itself** (`000cefa0`, not inside a job): `0xe48b0` WaterReadQueries (line 474) and `0x90ea0` (line 483, label spin). These are the only two.

Caveat: the job function bodies are referenced through the name blocks; I checked the 26 caller functions by grep, not each job body individually. A full body read is needed to be complete for the non-GPU work, but the GPU-touching helpers are the ones listed above.

## 9. PM4 opcodes of the writers (Q4, observed from the decomp)

- **`0x86eb60` (EVENT_WRITE_EOP):** header `0xc0044700` (opcode `0x47`, 5 body dwords). Dw1 = event type (`param_2 & 0x3f`) | `param_7 & 0x3f << 12` (event index) | `param_3 & 0x10 << 23` (cache policy) | `(param_8 & 3) << 25` (INT_SEL). Dw2 = address low, `param_4 & ~7 or ~3`. Dw3 = `param_5 << 29` (DATA_SEL: 1 = 32-bit write, 2 = 64-bit) | high address | `param_3 & 1 << 16`. Dw4/5 = data (`param_6`). Used by `0x92720` (label, `0x28`), `0x85cb40` (`0xddceddce`).
- **`0x86edc0` (WAIT_REG_MEM, generic):** header `0xc0053c00` (opcode `0x3c`, 6 body dwords), 7 dwords total with a trailing `10` (poll interval). Dw1 = function (`param_4 & 7`) | memory space bit `0x10` when address > 0xffff. Dw2/3 = poll address. Dw4 = reference (`param_5`). Dw5 = mask (`param_3`). Used by `0x92720` (function 5 = greater-or-equal, ref 1, mask -1) and by the job sites above.
- **`0x86eec0` (WAIT_REG_MEM, fixed function):** same header, Dw1 = `0x105` (function 5, memory space, engine bit `0x100` = PFP) | memory space bit. Reference = `param_4`, mask = `param_3`. Used by `0x92720` (when the label argument is non-zero) and the job sites above.
- **`0x86d5e0` (SET_SH_REG):** the payload is preceded by NOP-type header `0xc0001000` and tag `0x6875000d`. The SET_SH_REG header is `0xc0007600 | count<<16 | (param_2==0)*2`, register = `param_3 - 0x2c00 + table[param_2]` (SH base `0x2c00`, per-stage offset table at `0x1290410`), body = `param_5` dwords from `param_4`. Used by the user-data shadow flush (`0x861ce0`, `0x861e60`).
- Embedded payloads (`0x86d210`) use the same NOP-wrapping (`0xc0001000`, tag `0x68750000`), so the CP skips them and the shaders read them through a V#.

The port's PM4 parser must treat these tags as NOP payloads (already the case for the CP path, to confirm in `liverpool.cpp`).

## 10. Proposed PC-native mechanism (updated)

- **Per-frame heap:** `0x12820` category 1 (CPUMemory) is the funnel for `0x8c0d0`. Move the per-frame blocks into a host-shared ring per frame slot (2 slots, matching site 15). The ring is CPU-typed in the game's view, so the port can import it as host-visible. The announce is the end of `0x118100`.
- **Persistent per-frame addresses:** site 29 writes to `*(p+0x1d8)`. Keep the block persistent and announce each frame (N1 `NativeWrite`).
- **DCB:** `0xd07c0` announces `[start, cursor)` before each `0x91f80`. The embedded blocks are NOP-wrapped, so the announce covers them.
- **Descriptors:** side records at `0x865410`, `0x8655d0`, `0x865600`, `0x865500`. Bindless: the index lives in the resource object (`resource+4` neighbour), written by `0x865600`/`0x865500`; for the user-data shadow, a 32-bit index at `0x861ce0`/`0x861e60`.
- **Fences:** the only CPU wait is site 15 (move to a frame ticket). The GPU-side WAIT_REG_MEM sites stay in the stream (section 8); they are CP stalls, not CPU waits.
- **Reclaim:** replace the deleter's raw free (`0x12af0` in `0x8f180`) with the ring generation; once the producer is known (open question 2).

## 11. Summary

- Per-frame GPU data: `0x118100` (dispatcher) with classes 0 (`0x121ed0`), 1 (`0x123410`), 2 (`0x18aa20`); view constants (`0xbe1f0`, `0xcedf0`); DCB-embedded data (`0x86d210`). The funnel is `0x8c0d0` → `0x12820` category 1 = **CPUMemory**, not a frame heap.
- Descriptors: buffer V# `0x865410`, constant V# `0x8655d0`, image/buffer words `0x865600`, backing owned by the resource object `0x8be60`; user-data shadow `0x861ce0`/`0x861e60`. No T# builder exists at `0x85c580`, `0x85c9b0`, `0x85cb40`.
- Reclaim: two-generation deleter (`0x8ef20`, `0x8f180` → `0x12af0`) gated by one GPU-label spin per frame (`0x90ea0`). The producer of deferred frees and the context-object free are not identified.
- CPU readbacks: `0x90ea0` label spin and `0xe48b0` (water query) in the frame body; none inside the 50 job sites.
- Open: section 7 (reclaim producer, flip request, bytes per frame).
