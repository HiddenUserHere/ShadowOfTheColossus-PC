# N0 framedata: per-frame render data, command buffers, descriptors, submit

Specialist report, read-only RE. Base: main tree `C:\Users\ptr\Documents\sotc-pc` (branch re/pc-native). Decompilation: `out/re/decomp/<first 4 hex digits>/<8-digit address>.c`. Addresses are eboot VAs. Labels: **observed** (decompiled code or prior evidence), **inferred**, **unknown**. Not committed (coordinator rule).

## 1. Answer

- **Per-frame GPU data is built by the CPU in three places.** (a) Instance data: `UpdateGPUData` (0x118100) walks a dirty bitmap and calls per-class writers; class 1 (`InstanceData`, 0x123410) fills a 0x78-byte record per object from a heap block, then emits two V# buffers and a compute dispatch (0x928f0). (b) View/constant data: `SetViewParams` (0xbe1f0) and `EndFrameViewParams` (0xcedf0, 0x8b7f0). (c) Command-buffer embedded data: 0x86d210 bump allocator inside the DCB stream (12 callers). Everything the CPU writes for the GPU is written before the submit that consumes it (job counter waited in 0xd07c0). **observed**.
- **Memory:** the per-frame heap blocks come from the renderer object's scopes (renderer `DAT_02ee3ce0`, offsets +0x480 and +0x458, allocator 0x8c0d0 -> 0x12820). Which physical heap (GPUMemory vs CPU) backs them is **unknown** (see section 6). The DCB/CCB chunks come from scopes +0x3c0/+0x4a0 (`RenderContextCPU/GPU`, see report reports3/01).
- **Descriptors:** V# and T# are encoded from **guest addresses passed in by the caller**. The V# base word is the guest VA itself (`0x865410(addr, stride/format, count)`): there is no GPU handle in the engine, so the port can resolve the guest VA to a host buffer at bind time, and a bindless index can replace the V# words later. Per-stage user-data SGPRs are kept as a CPU shadow (`renderer+0x90`, 0x600 dwords per stage) and copied into the DCB (`0x861ce0`, `0x861e60`). **observed**.
- **Submit:** `0xd07c0` (KickoffSubmitJob) waits CPU job counters, submits each DCB (`0x91f80`), each CCB (`0x16f100`), and only in mode 1 rings the doorbell and calls `sceGnmSubmitDone`. It writes nothing into chunk memory. **observed**.
- **Buffering:** the frame ring is 2 slots (`0x90ea0`, RenderBackend_Frame). It toggles `renderer+0x7b0` in {0,1}, then **spins** until a GPU-written label matches (`*(p+0x770+i*8) == *(p+0x780+i*8)`). That is the only CPU read of a GPU-written value found in the frame path, and it is the one existing GPU wait on the CPU side. It then runs `0x8ef20` (FlushPendingFree). **observed**.
- **Counts:** 2 fixed contexts per frame (FrameStart via 0x90860, UpdateGPUData via 0xcfdb0), +1 conditional 3 MiB context (caed0), and up to ~43 variable context requests from RunFunctionAsRenderJob sites (see reports3/01). Per-frame byte totals: **unknown** (no counter exists).

## 2. Frame flow (observed, decompiled)

Frame body `0xcefa0` (492 lines) in order, with the per-frame data writers marked `[W]`:

1. `0x118440` FrameStart -> `0x90860` SetupFlipState -> `0x90470` RequestNewRenderContext (DCB 5 MiB, CCB 0x9bc00) — context 1.
2. `0xcfdb0` UpdateSkinningRenderObjects:
   - `0xc12a0` job (flag 4) with name "UpdateSkinningRenderObjects", then `0xc5560` (params) — mid-frame submit 1 (inside the job).
   - `0xc12a0` job "UpdateTextureStreamingGPUData", then `0xc5560` — submit 2.
   - `0x118100` UpdateGPUData `[W]` (context 2, via `0x90430`, then `0x92720`).
   - `0x92540` (not inspected).
3. Jobs: PreRenderJob, VirtualGeometry, Water, Fur, Particle UpdateSimulation (`0xd0040`).
4. `0xcaed0` main cam: `0xbe1f0` SetViewParams `[W]`, shadows (`0xcc9b0`), depth/G-buffer (`0xc59d0`), two mode-0 submits (0xcb00b, 0xcb0b2).
5. `0xd04e0` PostFX prep, `0xbe1f0` SetViewParams `[W]`.
6. `EndFrameViewParams` `0xcedf0` x3 and `0x8b7f0` `[W]`; `EndFramesWaterAndParticle`: `0xe48b0` WaterReadQueries (PM4 readback `0x86edc0` + `0x8c1b0`), `0x109970`, `0x1038a0`.
7. `0x118470` (`sceGnmInsertWaitFlipDone` into the DCB via `0x91f50`).
8. `0xc5560(Mode=1)` -> `0xd07c0` KickoffSubmitJob (final submit, SubmitDone).
9. `0xcfd6c` -> `0x90ea0` RenderBackend_Frame: ring toggle, GPU label spin, FlushPendingFree.

## 3. Inventory of sites

Classes: **PF** per-frame CPU write, **PERS** persistent CPU write, **GPU-RES** GPU-written result, **SYNC** CPU wait, **DESC** descriptor build, **SUB** submit.

| # | Eboot site (decomp) | Class | What it writes / reads / waits | Address source | Frequency | Patch point (proposed) | Evidence |
|---|---|---|---|---|---|---|---|
| 1 | `0x118100` UpdateGPUData (0011/00118100.c) | PF | Walks dirty bitmap at `param_1+0x210070` (4096 bits), per-class dispatch: class 0 `0x121ed0`, class 1 `0x123410` (InstanceData), class 2 `0x18aa20`, class 0xc `0x193e40`. Then `memset(bitmap,0,0x8000)`, emits 0x92720 (8-byte DCB word + PM4 at `0x86eb60`/`0x86edc0`, label write, unknown opcode). | Renderer object `DAT_02edf140`, bitmap is CPU state | once per frame from 0xcfdb0 | Entry `0x118100`: note bitmap range; end (before `0x92720` at the tail): announce the instance span written this frame. | decomp 0011/00118100.c |
| 2 | `0x123410` InstanceData writer (0012/00123410.c) | PF | Allocates 0x78-byte block via `0x8c0d0(renderer+0x480)` and fills it from instance object fields `+0x14..+0x118`. Then two `0x928f0` calls (V# pairs, see 5) and writes back `+0xe0` block state (CPU feedback, no GPU read). | instance object + allocator `0x12820` (TLS category 1) | per visible instance class each frame | Replace the allocation (`0x8c0d0` call at `0x123410` entry) with a host-shared ring; announce block span at `0x118100` tail. | decomp 0012/00123410.c |
| 3 | `0x11c710` InstanceDynamicData_Update (0011/0011c710.c, caller `0xca6d0`) | PF | Dynamic instance data; same allocator `0x8c0d0`. Share of write faults 0.8% (report 03). | | per frame, XPR pre-render | same as 2 | reports/03 |
| 4 | `0xbe1f0` SetViewParams (000b/000be1f0.c, 1345 lines) | PF | Per-view constant block (view/proj/light params). Callers `0xc6810`, `0xca6d0`, `0xcaed0`, `0xd04e0`. Writes via `0x122d90` (fills 0x40-byte view record; `0x162a50` matrix ops) and `0x1936d0` per light. | view object `param_1`, heap blocks | 3-4 times per frame | Patch the block allocator in SetViewParams to a host-shared ring; no upload if the ring is host-visible. | reports/03 (1.0% faults), decomp 000b/000be1f0.c |
| 5 | `0x928f0` ComputeDispatchWithBuffers (0009/000928f0.c, 511 lines) | DESC+PF | Builds two buffer V# (`0x865410(addr,0x4404,count)`, `0x865360` attrs, one constant V# `0x8655d0`), writes them into user-data via `0x861ce0` (slot 0x10) and `0x861e60` (slot 0x6d), allocates 8 bytes via `0x12820` with (count, stride), then `0x86b9e0` DispatchDirect with `x=(count+63)/64`. Callers: 0x123410, 0x92b50, 0x93960, 0x927d0, 0x932a0, 0x92640, 0x92850, 0x94320, 0x95420, 0x1018c0 (particle stats), 0x18f830, 0x18a000, 0x117930. | guest addresses in args (`param_4`, `param_2`) | every call above | This is the compute "instance copy": its V# base is the guest VA of the source block. Patch point: the two `0x865410` calls (announce source ranges) and `0x861ce0/0x861e60` (write a descriptor index instead, see 6). | decomp 0009/000928f0.c |
| 6 | `0x861ce0` / `0x861e60` user-data SGPR shadow (0086/00861ce0.c, 00861e60.c) | DESC | Writes a 16-byte V# into the CPU shadow `renderer+0x90 + stage*0x600*4` at slot `table[stage]+0x22` (VS/CS) or `+0x42` (PS), 16-bit slot indices from `renderer+0xc180 + stage*8`. Negative slot index: immediate SET_SH_REG via `0x86d5e0`. Flag `renderer+0xc613` triggers a second path `0x884240`. | slot tables from the shader binding | every draw/dispatch that binds a resource | Single funnel for all V# writes in the engine. Bindless: store a 32-bit index in the shadow instead of 4 dwords. | decomp 0086/00861ce0.c |
| 7 | `0x865410` V# buffer encoder (0086/00865410.c) | DESC | `param_1[0..3]` from guest address (`param_2`, 44 bits), stride/format from `0x866b30` (size class), count `param_4`. Callers: 0x16eb90, 0x928f0, 0x85c580, 0x92bd0. | guest pointer | per binding | Patch to record (guest VA, size, format) in a side table keyed by the V# (no change to the bits). | decomp |
| 8 | `0x8655d0` constant V# (0086/008655d0.c) | DESC | Word0/1 = guest address, word2 = `(size+15)>>4`, word3 = `0x20077fac` (fixed format). Callers 0x16eb90, 0x928f0, 0x995b0, 0x8be60, 0x85fc10, 0x92bd0, 0x8c280. | guest pointer | per constant buffer | Same as 7. | decomp |
| 9 | `0x86d210` DCB embedded-data bump allocator (0086/0086d210.c) | PF | Reserves `(bytes+3)/4 + pad + 3` dwords in the DCB writer, writes PM4 type-3 NOP-type header `0xc0001000 | words<<16` and tag `0x68750000 | align<<12`. 12 callers (0x92720, 0x92b50, 0x93960, 0x927d0, 0x932a0, 0x92640, 0x92850, 0x94320, 0x95420, 0x85c9b0, 0x85cb40, 0x85dc50). | DCB writer `+0x10` cursor, `+0x8` end | per embedded constant/data block | The DCB range `[start, cursor)` at submit time is the single announce point for everything in the DCB (see 12). | decomp |
| 10 | `0x90470` RequestNewRenderContext (0009/00090470.c) | PF/SUB | Allocates 0x238-byte context (`0x917c0`) with DCB (default 0x500000) and CCB (`0x85d770(0x10)` = 0x9bc00) from scopes `this+0x3c0`/`+0x4a0`. Initializes the context PM4 state (`0x86f1d0` 0x38 dwords, `0x869290`, `0x87c7a0..0x87c8c0`, `0x86ded0`, `0x86c840`). | scope allocator `0x47b20`/`0x12820` | 2-3 per frame, plus 0-43 variable | Announce DCB/CCB allocation to the native pool (N1); see report reports3/01 for reclaim (unknown). | decomp |
| 11 | `0x917c0` context ctor (0009/000917c0.c) | PF | Sets DCB/CCB writers, refill callbacks `0x91790`/`0x917a0` return 0 (no continuation chunk, fixed-size). Copies state from renderer `0x13c..0x154`. | | with 10 | writers capped at 0x3ffffc bytes (`0x862690`) | decomp |
| 12 | `0xd07c0` KickoffSubmitJob (000d/000d07c0.c) | SUB+SYNC | CPU waits on job counters (`0x35590` per list entry), then `0x91f80` per DCB (`sceGnmSubmitCommandBuffersForWorkload`), `0x16f100` per CCB, and in mode 1 `0x91f90` doorbell and `sceGnmSubmitDone`. | params block `0xc5560` (0x610 bytes) | 5 mid-frame + 1 final | Announce point: before `0x91f80`, announce each DCB `[start, cursor)`. Writes nothing into chunks. | decomp |
| 13 | `0xc5560` SubmitJob (000c/000c5560.c) | SUB | Allocates 0x610-byte params (`0x17170`), DCB vector at +0x210, CCB vector at +0x410. | | 6 per frame | none | decomp |
| 14 | `0xc12a0` RunFunctionAsRenderJob (000c/000c12a0.c, 1461 lines) | PF+SYNC | Flag `f&2`: requests a context (size `table[(f>>8)&7]`). Flag `f&4` without inline: `0x90670` creates a 0x1af0 object with CPU/GPU allocations. Waits `0x35590(param_5)` before inline work. 48 call sites (report reports3/01). | job param block | 48 sites, 43 can request DCB | Wrap the context request: ring-allocate instead of scope allocate. | decomp; reports3/01 |
| 15 | `0x90ea0` RenderBackend_Frame (0009/00090ea0.c) | SYNC (GPU-RES read) | Toggles ring index `+0x7b0`, sets `+0x18` to slot ring, `0x8d730`, then **spins** `while (*plVar3 != *(p+0x780+i*8))` on the GPU-written label `*(p+0x770+i*8)`. Then `0x8ef20` FlushPendingFree, `0x8c860`, `0x32430`. Called once per frame from 0xcfd6c after the final submit. | GPU label pointers | once per frame | Keep as the frame-pacing fence (the one allowed wait), but move it to a frame ticket/fence API; delete the pending-free dependency on it once allocations are native. | decomp; reports3/01 |
| 16 | `0x91f80` / `0x91f90` (submit thunks) | SUB | Calls `sceGnmSubmitCommandBuffersForWorkload` (port `gnmdriver.cpp:2429`) and `sceGnmSubmitDone`. | | | port-side: announce ranges here if not done in 12 | reports/06 |
| 17 | `0x92720` label / EOP writer (0009/00092720.c) | GPU-RES (write) | Allocates 8 bytes in DCB (`0x86d210(...,8,3)`), writes 0, then `0x86eb60(0x28,...)` (release-mem-like, unknown opcode) and `0x86edc0`/`0x86eec0` (write-data / wait variant selected by arg). | DCB | after each UpdateGPUData and at frame end | Patch: keep as the GPU label, but it is the source of the label `0x90ea0` reads. | decomp |
| 18 | `0xbe1f0` / `0xcedf0` / `0x8b7f0` EndFrameView* writers | PF | Per-frame view constants. `0xcefa0` names them "EndFrameViewParams". 1.2% faults. | | | same as 4 | reports/03 |
| 19 | `0x101720` particle record shift (0010/00101720.c) | PF (persistent CPU data) | 96-byte shift inside 192-byte particle record plus 32-bit index. Called from `0x103ad0`, `0x1051f0` (ParticleRenderer::UpdateSimulation). 8.7% of write faults. | particle pool (persistent) | per particle update | Persistent data: upload-once/write-announce (N1 NativeWrite), not per-frame ring. | reports/03 |
| 20 | `0x1018c0` ParticleSystemPool::ProcessStats (via `0x928f0`) | GPU-RES read | Writes stats (`pool+0x188`), 3,699 read faults (CPU reads GPU-written stats). Uses the compute path of site 5. | pool | per update | Consumer: see particle-stats topic; the dispatch part is this topic's site 5. | reports/03 |
| 21 | `0xe48b0` WaterReadQueries (`0x86edc0` + `0x8c1b0`) | GPU-RES read | PM4 emits a GPU result packet, then the CPU reads the result in the same frame. | | once per frame | Consumer: remove or read without waiting (N3). | reports/06 |
| 22 | `0x8c0d0` per-frame block allocator (0008/0008c0d0.c) | PF | `FUN_00012820(size, category DAT_02ed1f54 = 1, align 0x10, 0)`, returns guest pointer, stores its high bits in `param_1[1]`. Used by sites 2, 3 and many others (callers 0xa69d0, 0xbb310, 0x5de0b0, ... 0x1a5490). | scope object | every per-frame block | The single allocation funnel to replace with a host-shared ring for N1. | decomp |
| 23 | `0x12820` allocator (0001/00012820.c) | PF | TLS-top scope allocator: category 1 uses the thread-local scope `*(fs:-0x18)+0x400` ring, else the global `DAT_02dfaab0[category]`; calls vtable `+0xa0` on the scope. | TLS / global scope | every allocation | Patch point for a native per-frame ring (N1): categories routed to host-shared heaps. | decomp |
| 24 | `0x8c880` scope init (0008/0008c880.c) | PF | Creates named scopes `GameToRenderThread`, `GameToGPU`, `CPU_RenderToGPUToGame`, `RenderSingleFrameGPU` (`DAT_02ed29c0`), `RenderSingleFrameCPU` (`DAT_02ed2ca0`). | static | init | Names for the per-frame heaps (see section 6). | decomp |
| 25 | `0x8f6f0` renderer singleton ctor (0008/0008f6f0.c) | PF | Creates the 0x8b0-byte renderer object (`0x166f50`) and assigns `DAT_02ee3ce0` (renderer). Its `+0x3c0/+0x4a0` (DCB/CCB scopes) and `+0x480`/`+0x458` (per-frame block scopes) are used by sites 2, 10. | singleton | init | Shows where per-frame heaps are rooted. | decomp |
| 26 | `0x90860` SetupFlipState (0009/00090860.c) | PF | Resets context list `this+0x18`, rotates frame scopes `this+0x580/0x670` (slot+0x20 = `0x48270`), calls `0x90470` (context 1). | renderer | per frame | Frame boundary. | reports3/01 |
| 27 | `0x95760`/`0x960a0` flip thread, `0x956e0` flip mode | SYNC | Flip event queue (`sceVideoOutAddFlipEvent`). Per-frame flip is not identified (see reports/06 Q2). | | | out of this topic | reports/06 |

Counts by class (N0 framedata, this report, 27 rows): **PF** (per-frame CPU write) 12, **PERS** (persistent CPU write) 1 (site 19), **DESC** (descriptor build) 4 (sites 5, 6, 7, 8), **GPU-RES** (GPU-written read or write) 3 (sites 15, 17, 20-21 counted once each for read side), **SUB** (submit) 3 (12, 13, 16), **SYNC** (CPU waits on GPU or jobs) 3 (14, 15, 26-27). Rows overlap by design (a site can be PF and DESC).

## 4. Memory classes and buffering

- **Per-frame instance/view data:** allocated per call from the renderer scopes (`+0x480` and friends), written by the CPU, read by GPU compute (site 5) or by draws through V#. No explicit fence per block; blocks are freed by the scope's frame rotation or by `free_all` at teardown. **Reuse rule: unknown.** I did not find a per-frame reset of scope `+0x480`. Same gap as reports3/01 (DCB/CCB reclaim).
- **DCB/CCB chunks:** fixed-size per context, no continuation (refill returns 0), reclaimed only at teardown (0x90050) according to reports3/01; retirement is the 2-slot ring in site 15, which waits on the GPU label of frame N-2. That two-frame ring is the double-buffer signal.
- **Triple buffering:** not present in this code path. Two frames is the depth (`+0x7b0` in {0,1}).

## 5. V# and T# construction

- **Buffer V#:** `0x865410(out, guest_addr, fmt_word, count)`. Word0/1 = guest address (lo, hi & 0xfff), word2 = count, word3 = format/dst_sel from `fmt_word`. Address is the caller's pointer (instance block address, mesh vertex address, etc.). **observed**.
- **Constant V#:** `0x8655d0(out, guest_addr, size)`: word2 = `(size+15)>>4`, word3 = `0x20077fac`. **observed**.
- **Binding:** `0x861ce0` / `0x861e60` copy 16-byte V# into the stage shadow at a slot given by `renderer+0xc180[stage]`. **observed**. Shader-side slot tables come from the shader binding (not traced).
- **Texture T#:** `0x85c580` and `0x85c9b0`/`0x85cb40` reference V#-like encoders; not decompiled in this pass. **unknown** (open question 6).
- **Where the address of each GPU resource comes from:** guest pointers passed by the caller (instance/view blocks from site 22, vertex/texture pointers from the resource objects). The engine never keeps a host or GPU handle in the V#. So the port can map guest VA -> resource at bind time (as now), and a native pool can hand out VAs in host-shared windows (N1).

## 6. Open questions (ranked, with how to settle)

1. **Which physical heap backs scope `renderer+0x480`/`+0x458` and the `RenderSingleFrameGPU/CPU` scopes (`DAT_02ed29c0`, `DAT_02ed2ca0`).** Settle: runtime trace of `0x12820` category 1 return addresses and the vtable `+0xa0` object's memory type (`GPUMemory` set up in `0x110a0`, report reports/03).
2. **Reclaim of per-frame blocks** (scope `+0x480`, DCB/CCB scopes). No per-frame reset found. Settle: log scope cursor at `0x8c0d0` entry per frame and `free_all` calls; the sub-allocator at scope+0xb8 is still unidentified.
3. **Bytes per frame** through `0x86d210` and `0x8c0d0`. Settle: a counter in `0x86d210` (writer cursor delta) and `0x8c0d0` (size argument sum) per frame.
4. **Opcodes of `0x86eb60`, `0x86edc0`, `0x86eec0`, `0x86d5e0`** (used by label writes and SET_SH_REG). Settle: decode their PM4 type-3 opcode field (`0x86edc0` writes 6 dwords, per reports/06).
5. **Per-frame flip packet** (still open from reports/06 Q2).
6. **Texture T# builder** (`0x85c580`, `0x85c9b0`, `0x85cb40`): decompile next.
7. **`0x11c710` and `0x121ed0`, `0x193e40`, `0x18aa20`** (other instance classes): decompile next; they may write persistent data.
8. **Loop bounds** of the 48 job sites (reports3/01 Q2).

## 7. Proposed PC-native mechanism and patch points

Per-frame data (sites 1-5, 22-23):
- Route the per-frame categories (`0x12820` category 1, and the renderer scope allocator `0x8c0d0`) to a **host-shared ring**, one per frame slot (2 slots, matching site 15). The ring is imported once (N1 host-shared region), so CPU writes are visible to the GPU with no upload and no fault.
- At the end of `0x118100` (UpdateGPUData, before the `0x92720` label write), announce the written span `[ring_start_of_frame, cursor)` (for `NativeWrite` bookkeeping; no copy needed if the ring is host-visible). Frame-slot reuse waits on the same label as site 15 (N-2).
- `0x928f0` (site 5): the two source V# bases are guest VAs inside the ring; no change needed except that the port resolves them to ring offsets directly.

Descriptors (sites 6-8):
- Keep the V# encoder; add a side record `(guest VA, size, fmt)` at `0x865410`/`0x8655d0` so the port never reads the V# words back to find the resource (bind becomes a lookup).
- For bindless (N5): at `0x861ce0`/`0x861e60`, write a 32-bit descriptor index into the stage shadow instead of 16 bytes. This is one patch point for all engine bindings.

Command buffers (sites 9-13, 16):
- The DCB is the GPU-visible command memory. Announce its written range at submit: in `0xd07c0` before each `0x91f80`, `[writer_start, writer_cursor)` (the DCB writer struct holds both). This covers all embedded data written via `0x86d210` (site 9), so site 9 needs no per-call hook.
- Writers are capped at 0x3ffffc bytes (`0x862690`), and refill returns 0 (fixed size). A native DCB that is host-shared at allocation keeps the same API.

Persistent data (site 19 and others):
- Use N1 `NativeWrite(addr, size)` at the write point (particle record shift `0x101720`) and upload once; do not put persistent data in the per-frame ring.

Submit and fences (sites 12, 15, 16):
- Keep `0xd07c0` as the single submit point; the CPU job-counter waits before it are CPU-side and fine.
- Replace the GPU label spin in `0x90ea0` (site 15) with a frame ticket: the only allowed CPU wait. Once the per-frame heaps are ring-native, `0x8ef20` FlushPendingFree no longer needs to wait for GPU labels for them.

## 8. Every place the CPU reads back what it gave the GPU (or GPU-written memory)

1. `0x90ea0` (site 15): spin on GPU label. Frame pacing, keep as the fence (move to a ticket).
2. `0x1018c0` ProcessStats (site 20): reads GPU-written particle stats; 3,699 read faults (reports/03). Consumer of the compute dispatch in site 5.
3. `0xe48b0` WaterReadQueries (site 21): reads a GPU result in-frame (reports/06).
4. The port's own `GateWait` (`gnmdriver.cpp:171`), not engine code: waits for GPU idle at submit (mode 0/1). Relevant to N3.
5. Not a readback, but worth noting: `0x123410` writes block state back into the instance object (`+0xe0` / `+0xd70` frame counter). That is CPU-side state, no GPU read.

No other CPU read of GPU-written memory was found in the frame path, but the job bodies behind `0xc12a0` (48 sites) were not all read, so this list is complete for the frame body only.

## 9. Port-side facts used

- `sceGnmSubmitCommandBuffersForWorkload` (`gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2429`) and `sceGnmSubmitDone` (`:2475`) with `GateWait` (`:171`) and `SOTC_CP_AHEAD` (reports/06). Not changed here.
- Write faults on the port side (reports/03) concentrate on the 1 MiB heap pieces that hold these blocks; the per-frame writers' destination is what the ring must replace.

## 10. Summary

- Per-frame GPU data is built by `UpdateGPUData` (0x118100) and its class writers (0x123410 instance, 0x11c710 dynamic), by SetViewParams (0xbe1f0) and EndFrameView (0xcedf0, 0x8b7f0), and inside the DCB by 0x86d210. All write through the single allocator funnel 0x8c0d0 -> 0x12820, and all consume V# from guest addresses.
- The descriptor funnel is 0x861ce0 / 0x861e60 (stage shadow). The V# encoders are 0x865410 and 0x8655d0.
- Submit is 0xd07c0; the only CPU read of GPU-written memory in the frame path is the 2-frame label spin in 0x90ea0, plus the particle stats and water query readbacks.
- Unknown and gating the design: which physical heap the per-frame scopes use, and how per-frame blocks are reclaimed. Both need a runtime counter, not more static reading.
