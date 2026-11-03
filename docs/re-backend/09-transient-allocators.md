# 09 - Per-frame transient GPU allocators

Read-only static analysis of `out/eboot.elf` (Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`,
`tools/re/eboot_re.py` for callers, disassembly and vtable slots). Nothing built, run or committed.
Addresses are eboot VAs. Per-draw descriptor fill (`ctx+0x3090`) and mesh buffers are left to
docs 06 and 07.

Labels: **observed** (decomp, disassembly or vtable read, address given), **inferred**, **unknown**.

## 1. Answer

- **observed:** the frame has two ring allocators (130 MiB CPU-family, 20 MiB GPU-family) that
  rotate once per frame in `EndFrameViewParams` (`0xcefa0`). Their base blocks are allocated once at
  render-context init (`0x8f6f0`). Their objects carry a readable name (`RenderContextFrameCPU`,
  `RenderContextFrameGPU`), so the port can find them at runtime.
- **observed:** no caller of the ring allocation slot (`0x48440`) was found. The ring rotation
  (`0x48270`) does not wait on the GPU. Whatever waits before reuse is **unknown**. In the port the only
  observed GPU wait per frame is at `sceGnmSubmitDone` (report 06, `GateWait(2)`).
- **observed:** each frame builds a new per-flip render context (`RequestNewRenderContext` `0x90470`,
  wrapper `0x90430`). Its DCB (5 MiB default) and CCB come from family 1 (CPUMemory) through `0x12820`.
  The DCB has no chunk refill: the refill callback at `+0x18` (`0x91790`) returns 0.
- **observed, important:** the GPU writes into command-buffer memory. `0x86d210` embeds data inside the
  DCB as a NOP payload. Fence labels (EVENT_WRITE_EOP targets, WAIT_REG_MEM sources) live in that
  payload (`0x92b50`, `0x92500`, `0x925c0` callers). The DCB is therefore not "CPU-written, GPU-read
  only". Labels in it must be treated as GPU-written.
- **observed:** per-slot data for 8 frame slots: one 1.6 MB GPU block (`ctx+0x7a0`, 8 x 200000 bytes),
  one 0x20-byte GPU block of 8 dwords (`ctx+0x7a8`), and 8 embedded CB labels. The 24-entry slot table
  (`frame+0x40`) that points at them is built in `0x90860` every frame.
- **observed:** the SH user-data bump (`0x8643f0`) takes its memory from a refill callback in its own
  object (`[obj+0]`). The refill target is **unknown** (not read).
- **observed:** the named RenderSingleFrameGPU heap wrapper (`0x8c0d0`) allocates through `0x12820`
  with the slot global `DAT_02ed1f54` as family (inferred value 2). That family is the BucketAllocator
  (`0x1d750`, per-thread buckets). Its per-frame reset `0x1d230` is called only from
  `ParticleRenderer::BeginFrame` (`0x107540`).
- **observed:** generic allocations go through `0x12820` (streaming uses `0x12950`). A runtime hook on
  `0x12820` sees all families, with size, family and alignment as arguments.
- **Answer to "transient, CPU-only?":** the two frame rings and the 56 MiB block are not proven CPU-only
  or GPU-free. Whether the GPU reads or writes their data bodies is **unknown**. The embedded CB labels
  and the per-slot dword labels are GPU-written (observed for EOP targets; see section 4).

## 2. Allocator inventory

### 2.1 Frame rings (two blocks, rotated per frame)

| ID | Object (ctx field) | Size | Family (block base) | Alloc | Reset / rotate | Init | Name (runtime) |
|---|---|---|---|---|---|---|---|
| A | FrameCPU, `ctx+0x580` | 0x8200000 (130 MiB) | 1 = CPUMemory (`0x12820(0x8200000, DAT_02dd9f80)`, `0x8f6f0`, base `ctx+0x760`) | vtable slot 20 = `0x48440` (cursor `+0x98`, spill `+0xa0`, spinlock `+0x70`) | vtable slot 4 = `0x48270`, called from `0x90860` each frame | `0x481f0(obj, 2, chunks=ctx+0x660, base, size)` at `0x8ff7b` | `obj+0x10` = `"RenderContextFrameCPU"`, `obj+0x18` = 0 (kind) |
| B | FrameGPU, `ctx+0x670` | 0x1400000 (20 MiB) | 2 = GPU family (`0x12820(0x1400000, DAT_02dd9f84)`, base `ctx+0x768`) | same `0x48440` | same `0x48270` | `0x481f0(obj, 2, chunks=ctx+0x750, base, size)` at `0x8ffa3` | `obj+0x10` = `"RenderContextFrameGPU"`, `obj+0x18` = 0 |

Object vtable `0x15976c8` (read from eboot): slot 0 `0x90420`, slot 1 `0x91270` (free all via `0x47620`),
slot 4 `0x48270`, slot 20 `0x48440`. Other slots are not analysed.

Ring object layout (**observed** from `0x481f0`, `0x48270`, `0x48440`; offsets are bytes):

| Offset | Field | Set by |
|---|---|---|
| `+0x80` | chunk-marker array (2 entries, `0` = free, nonzero = in use) | `0x481f0` (arg 3) |
| `+0x88` | block base (rotate restarts here) | `0x481f0` (arg 4) |
| `+0x90` | block end = base + size | `0x481f0` |
| `+0x98` | cursor | alloc advances; rotate sets |
| `+0xa0` | spill cursor | rotate clears |
| `+0xa8`, `+0xb0` | slot end | rotate |
| `+0xb8` | slot count (2) | `0x481f0` (arg 2) |
| `+0xbc` | current slot index | rotate |
| `+0xc0` | block size | `0x481f0` (arg 5) |
| `+0xc8`, `+0xd0`, `+0xd8` | bytes used, high-water, alloc count | alloc |

Mechanics of a rotate (`0x48270`, **observed**): clears the marker of the slot being left if it still
equals the cursor; advances the slot index modulo 2; searches for another slot with a nonzero marker.
None found: cursor = block base. Found: cursor = that slot's marker. The new slot's marker is set to its
starting cursor. No GPU value is read and no fence is consulted.

Allocation (`0x48440`, **observed**): spins on the lock at `+0x70` (sleeps `sceKernelUsleep(1)` after
10000 tries), aligns the cursor, checks the slot end, spills through `+0xa0` if needed, and returns the
pointer (0 on failure). It also calls `0x452d0` when `+0x28` is set (thread id, **inferred**).

Per frame (**observed**): `EndFrameViewParams` (`0xcefa0`, call site `0xcf10f`) calls `0x118440`,
which zeroes three 32-byte blocks in the render object and calls `0x90860(DAT_02ed33f0)`. `0x90860`
calls `0x48270` on both rings (`call [rax+0x20]` at `0x90a9b` and `0x90aac`).

Who allocates from A or B (vtable slot 20 on `ctx+0x580` / `ctx+0x670`): **unknown**. No direct caller,
no field-offset reference outside the render context, and no indirect call site found by grep.
Candidates to check: report 05 open items 1 and 2.

Backing block for A and B: `0x12820` (section 3.1). Family 1 = `CPUMemory` object `0x2dfb500`
(`0x13380`, ctor `0x47b20`, name `"CPUMemory"`, kind 1). Family 2 = `BucketAllocator` (`0x2dfb600`,
`0x13380`). Family 2 is `DAT_02dd9f84` (inferred from the family-2 registration order and report 05).

### 2.2 Per-flip render context (DCB + CCB, one per RequestNewRenderContext)

| Item | Address | Evidence |
|---|---|---|
| Wrapper | `0x90430` (53 B) | observed; callers `RenderThread::RunFunctionAsRenderJob` (sites `0xc1471`, `0xc17cc`), `RenderView::MainCamera` (`0xcb001`), `UpdateGPUData_InstanceData` (`0x1182f4`) |
| Creator | `RequestNewRenderContext` `0x90470` (509 B) | observed; default DCB size `0x500000` (5 MiB) when arg is 0; CCB size from `0x85d770(0x10)` when arg is 0 (fn not read) |
| Block allocation | `0x917c0` at `0x91901` (DCB) and `0x9193a` (CCB), both `0x12820(size, 1, 0x80, 0)` | observed: family literal 1 (CPUMemory), 0x80 alignment |
| Command list init | `0x862690` at `0x91960` (350 B) | observed; `0x862690` clamps the DCB stream to `0x3ffffc` bytes, refill `0x862800` |
| Refill, DCB stream | `+0xc8` (`0x91790`, returns 0) | observed; a full DCB does not get a new chunk |
| Refill, second stream | `0x8627f0` (returns 0) | observed |
| Lifetime | Context list `ctx+0x18` is reset each frame in `0x90860` (`*pauVar3 = 0`) | observed |
| Free of the 5 MiB blocks | **unknown** | not in `0x90860` or `0x90470` |

Note: `0x917c0` passes `param_2` (DCB size) and `param_3` (CCB size) as the sizes for `0x12820`. The
CCB size is the `0x85d770(0x10)` result when the caller passes 0, so it is not a fixed constant.
Report 05 said "index 1 is not a family" for this block. That is wrong: the literal 1 is the CPUMemory
family (section 3.1).

### 2.3 Per-frame slot table and embedded labels (built in `0x90860`)

Observed at `0x90860` (after the rings rotate; the frame object is the one `0x90860` receives as `param_2`):

| Item | Address | Detail |
|---|---|---|
| Per-slot GPU data, 8 x 200000 B | `ctx+0x7a0` (1.6 MB, GPU family, `0x17180(1600000, DAT_02dd9f84, 8)` in `0x8f6f0`) | slot k at `+0x7a0 + 200000*k` |
| Per-slot GPU dword, 8 x 4 B | `ctx+0x7a8` (0x20 bytes, `0x17180(0x20, DAT_02dd9f84, 8)`) | slot k at `+0x7a8 + 4k` |
| Per-slot CB label, 8 x 8 B | `0x92500(*ctx_frame, 8, 0)` = embedded in the current DCB (`0x86d210`) | slot k at label block `+8k`; decomp of `0x92500` shows no return value, so the exact result register is **inferred** |
| Frame label block, 39 qwords = 312 B | `0x92500(cmd, 0x27, 1)` embedded in DCB, stored at `frame+0xe8` (`param_2[0x1d]`) | initialised to 1; XPR and CS labels at `+0x40`, `+0x68`, `+0x78` (see 2.4) |
| 24-entry table, 0xc0 B | `0x17170(0xc0)`, stored at `frame+0x40` (`param_2[8]`) | entries `[3k]` = GPU data, `[3k+1]` = GPU dword, `[3k+2]` = CB label |

Table entry `[0x12]` is decompiled as `&UNK_00124f80 + ctx+0x7a0`. This looks like a decompiler artefact.
Treat that slot as **unknown**.

Per-slot semantics (GPU data, GPU dword, CB label) are **unknown**. The naming suggests an 8-slot
frame pipeline with a GPU data block and a fence per slot (**inferred**).

### 2.4 Labels read by the port's XPR path (`0xc6f70`, `0xc5200`, `0xc6bd0`)

| Caller | Label address | Writer |
|---|---|---|
| `0xc6f70` (BPXPR_CullMainScene path) | `frame[0x1d] + 0x68` | `0x925c0` (EOP, data 1) |
| `0xc5200` (BPGnm_WriteCsDoneLabel) | `frame[0x1d] + 0x78` | `0x925c0` |
| `0xc6bd0` (BPXPR_CullShadowScenes) | `frame[0x1d] + 0x40` | `0x925c0` |

`0x925c0` calls `0x86eb60(cmd, 0x28, 0, addr, 1, 1, 0x38, 2)` (EVENT_WRITE_EOP, BOTTOM_OF_PIPE_TS). So
these labels are written by the GPU inside the CB payload. The view object is assumed to be the frame
object that `0x90860` fills (**inferred**). The CPU-side consumer is in report 01 and report 07 (`0x1a6d20`).

## 3. Generic heaps and sub-allocators

### 3.1 Family dispatch `0x12820` (the choke point)

`0x12820(size, family, align, flags)` (292 B), observed:
- family is the low 32 bits of arg 2; family 1 with a thread override uses the thread's stack entry
  (`tls-0x18 + 0x400`); otherwise `DAT_02dfaab0[family]`.
- calls the family object's vtable slot 20 (`+0xa0`) with `(obj, size, align, tmp)`. On failure it
  calls slot 19 (`+0x98`) and, if that returns 0, `0x124f0` (OOM path).
- `0x12950` is the same dispatcher with vtable slot 23 (`+0xb8`); used by the streaming pool
  (`0x96680`, `0x98ae0`).
- `0x17180(size, family, align)` is a one-line wrapper for `0x12820` (used for the 1.6 MB and 0x20-byte
  GPU blocks in `0x8f6f0`, and the 200 KB and 0x3800000 blocks there).

Registered family objects (`0x13380`, observed):

| Index | Object | Ctor | Vtable slot 20 | Notes |
|---|---|---|---|---|
| 1 | CPUMemory `0x2dfb500` | `0x47b20` (name "CPUMemory", kind 1) | `0x47e30` (fallback child list, `+0xb8`, `+0x90` array) | `DAT_02dd9f80` = 1 |
| 2 | BucketAllocator `0x2dfb600` | `0x13990` + `0x17990(0x10000, 0x10, 0x400)` | `0x1d750` (per-thread buckets, 0xb slots) | reported in report 13 as "RSF heap" |
| 3 | HeapAllocator `0x2dfc700` | `0x475c0` (vt `0x1595b70`) | `0x48c80` (index name `BpeHeap_Free`, not checked) | `0x48910(0x9600000)` = 150 MiB |
| 4 | PageAllocator `0x2dfc800` | `0x475c0` (vt `0x1594af8`) | `0x187e0` (not read) | `bpe_PlatformPageAllocator_Init` `0x18aa0` |

### 3.2 Bucket allocator `0x1d750` and reset `0x1d230`

- `0x1d750` (587 B): per-thread bucket index from `0x332b0`; threads 0..10 get a 0x30-byte bucket at
  `+0xa0`. Other threads use the shared list at `+0x2b0`, behind a spinlock at `+0x70`. Refills from
  the backing object at `+0x80` (vtable slot 25 = `+0xc8` to size a chunk, slot 29 = `+0xe8` to allocate).
- `0x1d230` (721 B, vtable `+0x20`, called only from `0x107540` at the `ParticleRenderer::BeginFrame`
  site, reached from `0xd0040`): clears the bucket and chunk lists, updates the high-water mark, advances
  the chunk ring index at `+0x88` (modulo the count in the high dword), and calls backing slot 30
  (`+0xf0`) for the next chunk. This is a per-frame reset of the heap object it receives.
- Which heap object `0x107540` resets, and whether it is the same as the `RenderSingleFrameGPU` slot 2
  used by `0x8c0d0`: **inferred** (report 13 open item 4).

### 3.3 Named heap wrapper `0x8c0d0`

`0x8c0d0(desc, size)` (74 B) calls `0x12820(size, DAT_02ed1f54, 0x10, 0)` and stores ptr and size in
the descriptor. `DAT_02ed1f54` is the slot global of `RenderSingleFrameGPU` (report 05). Callers from the
index include `0x11c710`, `0xbb310`, `0x191a90`, `0x10c8c0` and many more. The family value is inferred
to be 2, so the bucket allocator backs the per-frame GPU descriptors (**inferred**).

### 3.4 Backing direct memory (observed call sites, types inferred)

`bpe_PlatformPageAllocator_Init` (`0x18aa0`-`0x1965e`, 3006 B):

| Step | Address | Arguments (observed) |
|---|---|---|
| Size query | `0x18ade` | `sceKernelGetDirectMemorySize` |
| First allocation | `0x18b6d` | `sceKernelAllocateDirectMemory(0, size, 0x20000, 0x10000, memType=3, &phys)` |
| First mapping | `0x18b9e` | `sceKernelMapDirectMemory(addr, 0x20000, prot=0x33, flags=0x10, phys, align=0x10000)` |
| Other allocate and map pairs | `0x18c5c`/`0x18c8f`, `0x1941b`/`0x1944f`, `0x19513`/`0x1953b` | memory-type argument not read (**unknown**) |

- Memory type 3 is the Garlic (GPU-write-combined) type in the PS4 SDK. This is **inferred** from the
  value, not checked against the SDK header in the repo. The kind 1 / kind 2 split in report 05
  (CPU = Onion-like, GPU = Garlic-like) is consistent with it.
- The allocation sizes and the 150 MiB bucket heap (`0x48910(0x9600000)`) come from the decompile.

## 4. GPU writes into transient memory

| Range | Writer (observed) | Evidence |
|---|---|---|
| Embedded CB labels (8 B each, `0x86d210` in the DCB) | `EVENT_WRITE_EOP` via `0x86eb60` (`0x92b50`, `0x925c0` and callers listed in report 03) | observed (`0x92b50` writes 0, emits EOP to it, then WAIT_REG_MEM `0x86edc0`) |
| Frame label block (312 B, XPR and CS labels at `+0x40`, `+0x68`, `+0x78`) | `0x925c0` (EOP) | observed (`0xc6f70`, `0xc5200`, `0xc6bd0`) |
| Per-slot GPU dword `ctx+0x7a8 + 4k` | unknown | the table entry is GPU family; no writer found |
| Per-slot GPU data `ctx+0x7a0 + 200000k` | unknown | no writer found |
| XPR visibility mask and masks in RSF heap (`0x19b760`, `0x19c1a0`, `0x19c7e0`) | compute dispatch `0x19c0f1` and others (report 13) | observed (report 13 row 5, "GPU-written") |
| Particle records and instance data (`0x101720`, `0x123410`) | CPU writes (report 03, 07), then dispatch (report 04 families) | observed (writes); GPU writes not seen in these functions |
| Frame rings A and B | unknown | no GPU-side writer found; no caller found (section 2.1) |
| 56 MiB block `ctx+0xa88` (and 200 KB, 320 KB, 400 KB blocks) | unknown | published each frame by `0xca150` (report 05) |

Rule for the port (**inferred**): treat the embedded CB labels, the per-slot dword and the frame label
block as GPU-written, and do not skip their write-tracking. The DCB payload that contains them can be
treated as CPU-written once per frame, but the label words cannot.

## 5. Per-frame order (render thread, observed)

1. `EndFrameViewParams` `0xcefa0`. Step 1 `0x8b7f0` (one descriptor alloc).
2. `0x118440` (zeroes three blocks), then `0x90860`:
   - reset ring A and ring B (`0x48270`);
   - build the per-flip context: `0x90470`, two `0x12820` calls (DCB, CCB) from family 1;
   - embed the frame label block (`0x92500(cmd, 0x27, 1)`) and slot labels (`0x92500(cmd, 8, 0)`) in the DCB;
   - fill the 24-entry slot table at `frame+0x40`.
3. Skinning and texture streaming jobs (`0xcfdb0`), then `BPXPR_PreRender` `0xca6d0` (reads ctx+0xa80..0xa98).
4. `RenderView::MainCamera` `0xcaed0`: culling and dispatches, EOP labels into the DCB.
5. Particles `0x107540` (heap reset `0x1d230`, descriptors `0x8c0d0`), water, post.
6. `Frame::InsertWaitFlipDone` `0x118470`, final `SubmitJob` `0xc5560`.
7. Next frame: step 1 again. Ring rotation (step 2) is the only per-frame reuse point observed.

## 6. Command buffer chunks: allocation and recycling

| Question | Answer | Label |
|---|---|---|
| How is a DCB allocated? | One 5 MiB family-1 block per render context (`0x91901`); CCB is a second block (`0x9193a`) | observed |
| Chunk size and count | One block per context, no chunk list. The stream is capped at `0x3ffffc` bytes (`0x862690`) | observed |
| Refill when full | `0x91790` returns 0. Second stream refill returns 0. Overflow is an error, not a new chunk | observed |
| Segment table | `0x862a80` (453 B) records up to 16 segments per stream (`+0x164`..`+0x2e4`); its consumer is not read (submit is the likely user, **inferred**) | observed |
| Reuse | Context list reset each frame (`0x90860`). Block release is not in `0x90860` or `0x90470` | observed (the release path is **unknown**) |
| Who waits before reuse | **unknown**. Port-side: the frame boundary `sceGnmSubmitDone` waits for GPU idle (report 06, report 01) | observed (port) |
| Embedded data (`0x86d210`) | NOP-payload bump in the DCB: header `0xc0001000 | count<<16`, marker `0x68750000 | align<<12`, data follows | observed |
| Garlic or Onion | CPUMemory family 1 (kind 1); not verified against the memory type | inferred |

Port-side fix (report 01) is unaffected by these findings: the DCB range check should include the
embedded label area, which the GPU writes.

## 7. Stable runtime hooks for the port

| Purpose | Hook | Args and fields | Stability |
|---|---|---|---|
| Learn ring A and B base and size | Entry of `0x481f0` at the two render-context sites `0x8ff7b`, `0x8ffa3` (callers listed by `eboot_re.py callers`) | Arguments 1-5 (SysV: rdi ring object, rsi slot count, rdx chunk array, rcx base, r8 size) | Stable: a fixed call inside `0x8f6f0`. Other `0x481f0` callers (`JobSystem_Initialize`, `0xa46640`, `0xa47d60`, `0xa48280`, `0xa4ab90`) are different rings; filter by the name field |
| Confirm ring identity | Read the ring object name: C string at `obj+0x10`, kind at `obj+0x18` | "RenderContextFrameCPU" / "RenderContextFrameGPU", kind 0 | Stable across runs (static string) |
| Per-frame reset | Entry of `0x90860`, or the vtable call on the ring (`0x48270`) | Ring object pointer | Stable |
| Per-flip context (DCB and CCB block) | Return of the two `0x12820` calls in `0x917c0` (sites `0x91901`, `0x9193a`) or entry of `0x90470` | DCB or CCB base and size (args 1 and 2 of `0x12820`) | Stable |
| All family allocations (catch-all) | Entry of `0x12820`, log family, size, align | `ulong` size, family (low 32 bits of arg 2), align (arg 3) | Stable, high volume; sample per frame |
| Tag strings for the render heaps | `0x8c370` tag records (report 05); family name strings "CPUMemory", "BucketAllocator" | static strings | Stable |

Confirm each hook in a run (`=2` log mode) before relying on it. Hook addresses are from the
decompile. The `0x8ff7b`/`0x8ffa3` sites were taken from the caller index and were not re-read in
disassembly here.

## 8. Corrections to earlier notes

| Earlier note | Correction | Source |
|---|---|---|
| Report 05: 5 MiB per-flip block is "index 1, not a family" | Index 1 is the CPUMemory family (literal in `0x917c0`) | `0x917c0` at `0x91901`, `0x9193a` |
| Report 05: frame rings are "kind 1, Onion" and "kind 2, Garlic" | Ring A is family 1 (CPUMemory, kind 1), ring B is family 2 (GPU, kind 2). The Onion and Garlic mapping is inferred | `0x8f6f0`, `0x13380` |
| Report 03 describes the writer refill at `[obj+0x18]` as the chunk path | For the per-flip DCB that callback is `0x91790`, which returns 0. No chunk recycling in that DCB | `0x917c0` (`+0xc8` = `0x91790`) |
| Report 13 treats the write-after-emit set as CPU writes only (pass 2 scan) | The scan did not consider EOP labels in the DCB payload, which the GPU writes | `0x86d210`, `0x92b50`, `0x925c0` |

## 9. Unknowns and next checks

1. Callers of ring slot 20 (`0x48440`) on `ctx+0x580` and `ctx+0x670`. Method: a runtime probe on the
   hook above, counting callers by return address.
2. Release path of the 5 MiB DCB and CCB blocks (CPUMemory family). Method: log `0x12820` free calls or
   the CPUMemory reset (vtable of `0x2dfb500`).
3. Refill callback behind the SH user-data bump `0x8643f0` (`[obj+0]`), and who owns that object.
4. Whether `0x107540` resets the same heap as `RenderSingleFrameGPU` (`0x8c0d0`). Report 13 item 4.
5. Contents and readers of the per-slot GPU data (`ctx+0x7a0`) and dword (`ctx+0x7a8`), and of the 56 MiB
   block `ctx+0xa88`.
6. Memory types of the other three `sceKernelAllocateDirectMemory` sites in the page allocator.
7. `0x92500` return value (the decompile shows none).
8. Map of `0x85d770(0x10)` (default CCB size). Fn not read.

## 10. Summary table (per-frame transient ranges)

| Range | Kind | Who writes | Who reads | Reuse | Wait before reuse | Label |
|---|---|---|---|---|---|---|
| Ring A, 130 MiB (CPUMemory) | bump ring, 2 slots | CPU (alloc, unknown users) | unknown | rotate per frame (`0x48270`) | none in rotate; unknown | observed (mechanics), unknown (users) |
| Ring B, 20 MiB (GPU) | bump ring, 2 slots | CPU (alloc, unknown users) | unknown | rotate per frame | none in rotate; unknown | observed (mechanics), unknown (users) |
| Per-flip DCB and CCB, 5 MiB each | command buffer | CPU (packets), GPU (embedded labels) | CP and GPU | new block per context; release unknown | port: `sceGnmSubmitDone` | observed |
| Embedded labels in DCB | fence words | GPU (EOP) | CPU (XPR path) | per context | WAIT_REG_MEM (GPU) | observed |
| Per-slot GPU data, 8 x 200 KB | frame slot data | unknown | unknown | per slot | unknown | unknown |
| Per-slot GPU dword, 8 x 4 B | fence | unknown | unknown | per slot | unknown | unknown |
| SH user-data bump | dword bump | CPU | GPU (SGPR pointers) | refill, unknown | unknown | observed (bump), unknown (refill) |
| RenderSingleFrameGPU (bucket heap, family 2) | descriptor heap | CPU, GPU (masks, dispatch outputs) | GPU | reset `0x1d230` in `0x107540` | unknown | observed (heap), inferred (reset target) |
