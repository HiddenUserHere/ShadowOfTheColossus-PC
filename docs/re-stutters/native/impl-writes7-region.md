# Writes wave 7, group "region": the slot-17 ring (RenderSingleFrameCPU)

Scope: the six RIPs of the region around 0x13ce90_0000 - 0x13ce9b_0000 (verify6, commit faf7711). Static analysis only
(decomp in `out/re/decomp`, `eboot_re.py dis/callers`, the verify6 and MEMTYPE lines of its log). No source edited, nothing
built, the game not run, nothing committed.

## Summary

**The region is the RenderSingleFrameCPU ring (memtype slot 17, global 0x2ed1f58).** It is a per-frame ring of depth 2,
a child heap of arena kind 1 (Onion, 8 MiB data blocks), and it allocates about 0.087 MiB per frame. Four of the six RIPs
write blocks that come straight out of the slot-17 allocation funnel (`0x17170` -> `0x12820` with esi = [0x2ed1f58]). The
other two write fields of CPU job objects. The 917504 and 983040 binds are spans over these blocks, not one object of
about 900 KiB. The identity is strong for the allocation side (MEMTYPE probe), not yet proven for the bind side.

**Should it be a window: yes.** Window B of `allocators.md` section 6 is this ring (arena kind 1 object 0x2dfb2c0, key 17 =
`kMaskKey`, 16 MiB, registered at the 0x1171f site). In a host-shared window every write in this region needs no announce and
no upload, so one window covers all six RIPs, GPU data and bookkeeping alike. verify6 ran with windows off ("windows 0
(0 MiB)"), so this region was not a window in that run.

| rip | function | object | class | hook proposal |
|---|---|---|---|---|
| 0x1d849 | sub_1d750 (lane allocator) | free-tail header of the slot-17 lane, outside any block | (c) CPU bookkeeping | no (window covers it; block announce covers any later owner) |
| 0xceaa9 | sub_ce850 | phase field `+0xf4 = 0xd` of the render-job object (param_1) | (c) probable, object origin not traced | no |
| 0xc7acc | sub_c7850 (BPXPR_PreRender caller) | 0x1228-byte slot-17 block, first float 1.0 and zero stores | (a) per-frame GPU data | yes as fallback only (MEMTYPE_ALLOC post, see 6) |
| 0x1a8ca1 | sub_1a8c60 | phase field `+0xf4 = 1` of the object (rsi) | (c) probable | no |
| 0xc14c6 | RenderThread::RunFunctionAsRenderJob (0xc12a0) | 0x100-byte slot-17 block, zero fill | (a) per-frame GPU data | yes as fallback only (same hook) |
| 0xbbbe4 | sub_bbbe0 (sole caller EndFrameViewParams 0xcefa0, call 0xcfa70) | 8 bytes at +0x80 of the view-params object | (a) probable (object origin not traced) | yes as fallback only (same hook, if the object is a slot-17 block) |

## 1. What the region is

### 1.1 Identity (slot 17)
- Globals: `0x2ed1f58` is RenderSingleFrameCPU (heaps.md 2.4, "runtime slot 17"). `0x2ed1f54` (slot 16) is RenderSingleFrameGPU;
  `0x2ed1f4c` (GameToGPU) and `0x2ed1f50` (CPU_RenderToGPUToGame, slot 15) are different rings. The `RingDepth` enum in
  `gpu/sotc/native/native_draw.h` has GameToGpu = 5 and CpuRenderToGpuToGame = 6, which are heap ids, not these slots. Slot 17
  has no entry in that enum; its depth 2 comes from heaps.md 2.4 ("ring depth 2, CPU base").
- verify6 log, MEMTYPE probe (lines 535 and 761): `slot 17 [RenderSingleFrameCPU]: 1447 calls/s, 2.67 MiB/s, bytes/frame
  avg 0.087 MiB max 0.087 MiB`. Top callers per 5 s: `0xca17b` (6.55 MiB), `0xc7aa8` (3.41 MiB, sub_c7850), `0xc55a5`,
  `0xc9603`, `0xc9868`, `0xc14b4` (2772 calls, RunFunctionAsRenderJob), `0xc7e3d`, `0xc7f81`.
- Creator: `FUN_0008c370` (heaps.md 2.4). Allocations: `0x17170` (a 10-byte wrapper, `call 0x12820`) with esi loaded from
  [0x2ed1f58]. `sub_17170` callers include `sub_c7850`, `RunFunctionAsRenderJob` (two sites: 0xc1325, 0xc14af) and
  `Flip::SetupFlipState` (0x90b30).
- Memory: arena kind 1 (`allocators.md` section 2, row "Default TaggedArena kind 1 ... hosts RenderSingleFrameCPU
  (memtype 17)"), data blocks 8 MiB, headers Onion.
- Per-frame lifetime: the flip `0x1d230` (vtable +0x20 of the child class, called from `0x136f0` for 0x2ed1f58 at `0x8c860`)
  resets the lanes. The lane allocator is `0x1d750` (child heap +0xa0): per-thread lanes at +0xa0 + lane*0x30, one shared lane
  at +0x2b0.

### 1.2 Is it one ~900 KiB object?
No. The 0.087 MiB per frame is the allocation volume; the 896 KiB (917504) and 960 KiB (983040) binds are V# spans that cover
several slot-17 blocks (8 MiB arena blocks, ring depth 2). The addresses in the verify lines (0x13ce910000 to 0x13ce9ac000 and
the 0x13ce97f2d8 / 0x13ce99ff98 ends) fit one or two such blocks. The exact block base was not traced in the logs.

### 1.3 Does the GPU read it?
- verify6 counts the 917504 binds as "consumed": the bytes were written unannounced and the old page path uploaded them
  within 4 frames. "Consumed" means the bind covered the bytes; it does not prove which bytes the shader fetches.
- The V# builder of these binds was not traced (descriptor writers 0x865410 / 0x8655d0 / 0x865600 / 0x865500 not read here).
  The GPU read of the region is therefore probable (the 26 to 65 small binds of 0xbbbe4 and the 170 small binds of 0xc7acc
  point to smaller V#s over the same blocks), not proven.

## 2. Per RIP

### 2.1 0x1d849, sub_1d750 (lane allocator, decomp `out/re/decomp/0001/0001d750.c`)
- Instruction: `mov [rcx+0x10], rbx` at 0x1d849, with `[rcx+8] = [r13+0x18]` and `[rcx] = r13+0x10` at 0x1d84d to 0x1d859.
  This is the path where the current lane block (`[r13]` = base, `[r13+8]` = size) cannot serve the request and its
  leftover (>= 0x18 bytes) is pushed on the lane's retired list. The 0x18-byte header (link, next, size) sits at the start
  of the leftover.
- Class (c). The leftover is not allocated to anyone: no owner's live data is there. The GPU reads only live allocations.
  A later allocation that includes these bytes is written by its owner; bytes it does not write would carry the header
  (the same bytes the game writes, so the value matches the game). The port's block-level announce (section 6) covers
  those bytes whenever they belong to an allocated block at flush time.
- No hook. Consumed count 1235 is the bind-coverage effect, not a GPU read of the header.
- The lane is shared by all child heaps of the class (slot 16 and 17 both use `0x1d750`). The region match tells that these
  retired leftovers are slot-17 blocks.

### 2.2 0xceaa9, sub_ce850 (`out/re/decomp/000c/000ce850.c`)
- Instruction at 0xceaa9: `mov dword [rbx+0xf4], 0xd`, after `call 0x11d7a0` (BPXPR_RecordMaskedDraws). The stored 0 at 0xcea8b
  and 0xd here are a phase tag. `rbx` = `param_1` (callee-saved, the render-job object). Callers: `sub_cdbc0` (0xcdc83),
  `sub_cdcc0` (0xce2f5).
- Class (c), probable. The field is CPU state; the object holds pointers (+0x40 .. +0x88 read by the function), not a
  constant block. The object's origin (a slot-17 block or not) was not traced.
- No hook.

### 2.3 0xc7acc, sub_c7850 (`out/re/decomp/000c/000c7850.c`, 2231 bytes; caller BPXPR_PreRender at 0xca8e5)
- Asm 0xc7aa3 to 0xc7acc: `esi = [0x2ed1f58]`, `edi = 0x1228`, `call 0x17170` (slot-17 block). Then `r13 = block`,
  `mov dword [r13], 0x3f800000` (1.0f), and the zero/initialise stores at `r13+0x640`, `+0x740`, `+0x768` follow.
- Class (a): per-frame data written by the CPU for the frame. verify6: 375 consumed, 170 of them through binds <= 64 KiB
  (small V#s over the block are the GPU read).
- Hook: a block-level announce is the right form (one PerFrame range per block, after its last store, before the frame
  submit). It is the fallback of section 6; the window needs none.

### 2.4 0x1a8ca1, sub_1a8c60 (`out/re/decomp/001a/001a8c60.c`)
- Asm 0x1a8ca1: `mov dword [r13+0xf4], 1`, then `call 0xf8c90` (sub_f8c90). `r13 = rsi` (the object). Callers: `sub_1a8a40`
  (0x1a8bb4) and `sub_1a8dd0` (0x1a8f44).
- Class (c), probable: the same phase-tag pattern as 2.2.
- No hook.

### 2.5 0xc14c6, RenderThread::RunFunctionAsRenderJob (0xc12a0, decomp `out/re/decomp/000c/000c12a0.c`)
- Asm 0xc14a3 to 0xc14c6: `edi = 0x100`, `edx = 8`, `esi = [0x2ed1f58]` (via `lea rax, [0x2ed1f58]`), `call 0x17170`,
  then `vmovups ymmword [r12], ymm0` zeroes the first 32 bytes; further zero stores at `r12+0x20`, `+0x50`.
- Class (a): per-frame block, 358 consumed (31 small binds). The block is a render-job record the GPU reads.
- Hook: same fallback as 2.3. Highest call count of slot 17 (2772 per 5 s).

### 2.6 0xbbbe4, sub_bbbe0 (13 bytes, `out/re/decomp/000b/000bbbe0.c`)
- `vxorps xmm0; vmovlps qword [rdx+0x80], xmm0`: 8 bytes zeroed at `+0x80` of the object in `rdx` (decomp `param_3`).
  Only caller: `sub_cefa0` (EndFrameViewParams), call at 0xcfa70.
- Class (a) probable: the object is the per-frame view block and the GPU reads +0x80 through small binds (26 of 65 in
  verify6). The object's origin (slot 17 or another ring) was not traced; the region match points to slot 17.
- Hook: same fallback as 2.3, valid only if the object is a slot-17 block returned by `0x12820`.

## 3. Why one window fixes all six

Per `native.h`: a host-shared window is imported once as host memory; the GPU reads and writes it in place; CPU writes are
visible without an announce. The per-frame data is in the window, so classes (a) and (c) both stop needing announces.
The window is the only fix that covers the phase fields and the lane headers without per-write hooks.

## 4. Window B: the design and its state

- Design (`allocators.md` section 6, `impl-windows.md`): window B, 16 MiB (2 x 8 MiB), serves "arena kind 1 block for child
  key of RenderSingleFrameCPU". Keyed by memtype 17: `gpu/sotc/pc_pool_windows.h` `kMaskKey = 17`.
- Bind: `SotcPcPool::SetupWindows` (`gpu/sotc/pc_pool.cpp` 2202) binds window B to the arena kind 1 object read from
  0x2dfb2c0 (`kArenaKind1Global`), at the NATIVE_WINDOW_SETUP site 0x1171f (`scripts/hooks.d/native_windows.py`).
- Registration: `impl-windows.md` open item 2 says `RegisterWindow` has no implementation. It is implemented now:
  `gpu/sotc/native/native.cpp:225` (imports the range with `SotcGuestMemory::ImportGuestRange`, refuses overlaps). Open item 2
  is stale.
- Route: `WindowTable().Route(req.obj, req.kind, t_heap_key, req.size, req.align)` in `pc_pool.cpp` 2131. `t_heap_key` is set
  by `SetHeapKey(rsi)` at the 0x12820 entry (MEMTYPE_ALLOC pre, `native_windows_hooks.cpp` 104). So recognition at allocation
  is: 0x12820 entry with memtype 17 and owner = the arena kind 1 object.
- Gate: `WindowsOn()` = `NativeActive() && SotcNativeWindows::ModeOn()`. verify6 was run without windows
  ("SOTC NATIVE mode 2 (5.0 s): windows 0 (0 MiB)"), so these writes went through the normal upload path.
- Known limits (impl-windows item 6): arena kind 1 blocks are carved for other keys too, so window B is placement, not an
  exclusive owner. Harmless for correctness (windows are host-shared), but CPU-only data of other memtypes may sit in it.
- Capacity: the window holds 2 blocks; the ring needs 1 block per frame plus the flip (depth 2).

## 5. Recognition at allocation (for the window)

At `0x12820` entry (MEMTYPE_ALLOC wrap, `scripts/hooks.d/memtype.py`; rdi = size, esi = memtype):
- memtype (esi) == 17 selects the slot-17 ring.
- owner = the arena kind 1 object (0x2dfb2c0 global) selects the arena block the window is bound to.
- Block size: the window routes by exact size (`Route` matches owner, kind, key and size). The 8 MiB arena block is the
  unit; slot-17 requests (0x100, 0x1228, ...) are carved from it by the lane allocator.

## 6. Fallback without a window: block announce per slot-17 allocation

Only if the window cannot be used (for example the import is refused, `impl-windows` item 5):
- Hook: the existing MEMTYPE_ALLOC wrap at 0x12820 (`scripts/hooks.d/memtype.py`, the same address the windows use for
  their pre and post). Pre: save rdi (size) and esi (memtype) in a thread-local. Post: if memtype == 17 and rax != 0,
  `ChunkPush(rax, size)` (`gpu/sotc/native/native_hooks.cpp` 227). `FlushChunks()` runs at each submit (265, 281) and
  announces the queued blocks as PerFrame. Those announces are made after the writers, before the submit, which is the
  required order for class (a).
- No new pattern or switch is needed: the wrap already exists; the handler gets a memtype == 17 branch.
- Cost: one PerFrame range per allocation (about 0.087 MiB per frame), uploaded at the next submit outside a window (native.h,
  "PerFrame outside a window: uploaded at the next submit").
- 0x1d849 and the phase fields (2.2, 2.4) need no entry: they are either outside every allocated block (2.1) or in a block
  that the same announce covers anyway (if the object is a slot-17 block).
- A whole-ring announce per frame is not needed; the per-block announce is the same bytes with fewer ranges to track.

## 7. Open items and tests

1. The V# builder of the 917504 / 983040 binds is not traced. Check descriptor writers 0x865410 / 0x8655d0 / 0x865600 / 0x865500
   and log the base and size of each bind that falls in the slot-17 blocks. This decides how much of the 896 KiB is read.
2. The origin of the objects of 0xceaa9, 0x1a8ca1 and 0xbbbe4 (slot 17 or not) is not traced. Trace the caller chain
   (`sub_cdbc0`, `sub_cdcc0`, `sub_1a8a40`, `sub_1a8dd0`, `sub_cefa0`) to the allocation.
3. Slot 17 is not in the `RingDepth` enum; its depth (2) is from heaps.md 2.4.
4. Runtime test (not run): enable window B (`SOTC_NATIVE=2`, window path on per impl-windows item 7). Expected: the six RIPs
   stop appearing in the VERIFY UNANNOUNCED lines (writes go to host-shared memory), and the image must match the
   windows-off run. This is the decisive check of the window fix.
5. Window B holds only 2 blocks of 8 MiB; if the slot-17 ring needs more than 1 block in flight (depth 2 plus the flip), the
   overflow goes to the normal path and must be counted (`full` in the window report).

## 8. Summary table

| rip | function | object | class | hook proposal |
|---|---|---|---|---|
| 0x1d849 | sub_1d750 | lane free-tail header (slot 17 lane) | (c) | no |
| 0xceaa9 | sub_ce850 | render-job object +0xf4 phase tag | (c) probable | no |
| 0xc7acc | sub_c7850 | 0x1228 slot-17 block (view/state) | (a) | window B; fallback: block announce at 0x12820 post (memtype 17) |
| 0x1a8ca1 | sub_1a8c60 | object +0xf4 phase tag | (c) probable | no |
| 0xc14c6 | RunFunctionAsRenderJob | 0x100 slot-17 block | (a) | window B; fallback: same |
| 0xbbbe4 | sub_bbbe0 (EndFrameViewParams) | view object +0x80 | (a) probable | window B; fallback: same, if the object is slot 17 |
