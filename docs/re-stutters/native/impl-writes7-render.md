# Writes wave 7, group "render": unannounced writes on the 64 KiB binds (specialist: writes7-render)

Status: static analysis only. Nothing edited, built, run or committed. Sources: Ghidra decompilation
(`out/re/decomp`), `eboot_re.py` func/dis/callers/callees/sigfind, the verify6 log
(`user/pcmem/native-verify6/logs/default-r1-a1.log`), `gpu/sotc/native/verify.cpp`, `gpu/shadps4/video_core/buffer_cache`,
`gpu/shim/bbgpu.cpp`, `src/runtime_memory.c`, and `docs/re-stutters/native/framedata.md` / `verify6-consumed.md`.

## 0. Summary

| rip | function | object written | class | hook proposal |
|---|---|---|---|---|
| 0xd4d97 | sub_d4c50 (vector<u64> range insert), called by sub_d1d40 | per-record lists of CRenderObject pointers (render entities) | (c) CPU-only; shares CPUMemory 64 KiB granule with GPU per-frame blocks | no |
| 0xd3b7f | sub_d3ae0 (deque<int> push + deque of 0xa8 records) | record-index stack of the render-record pool | (c) CPU-only | no |
| 0xf3b026 | sub_f3af80 (float ring, rolling average) | CPU float history ring (`+0x110`, head `+0x120`) | (c) likely; consumer of the average not traced | no |
| 0xa3fb73 | sub_a3fb60 (slot accessor, 29 bytes), called by sub_3deca0 | pointer slot 1 of an 8-slot state table, zeroed on read | (c) CPU-only | no |
| 0x18c3be | sub_18c390 (133 bytes), called by sub_34cad0 | 0x298-byte per-object binding record (`+0x278..+0x290`) | (d) unknown | no (open) |

No row is class (a) or (b): none of the five writes is a field that a V#/T# or command buffer reads through a path I could
find. None gets a hook. The open questions are the 64 KiB bind size itself (section 1) and the readers of record
`+0x278..+0x290` for 0x18c3be (section 6).

## 1. The 64 KiB bind (shared by the five and by many RIPs)

### 1.1 What the bind size is
- The bind size reaching the buffer cache is `size = ClampRangeSize(vbase, vsize)` (`renderer_vulkan/vk_rasterizer.cpp:5590-5599`),
  i.e. `min(vsize, mapped run from vbase)`. `vsize` is `vsharp.GetSize()`. `runtime_memory_clamp`
  (`src/runtime_memory.c:2025`) walks adjacent mapped VMAs, so the clamp is 64 KiB only when the mapped run ends exactly
  64 KiB after `vbase`.
- The CPUMemory BucketAllocator uses **64 KiB granules** (`framedata.md` 1.1, `0x17990(..., 0x10000, 0x10, 0x400)`). A
  V# over a granule-mapped heap therefore clamps to a multiple of 64 KiB. This is consistent with the table: `max_bind`
  is exactly 65536 for the five RIPs and exactly 196608 (3 granules) for 0x18c3be.
- So the 64 KiB value is **most likely a clamp**, not a constant buffer with num_records 0x10000. This is a hypothesis;
  I could not confirm it statically (the V# base and num_records are not logged).

### 1.2 Evidence from the verify6 log
The consumed writes of the five RIPs (and of the other 64 KiB RIPs) land in shared 64 KiB-aligned blocks, the sign of a
bind that covers a whole granule:

| 64 KiB block | rows (per-window sums of `n`, max_bind 65536) |
|---|---|
| 0x1405a00000 | 0xd3b7f 351 (this RIP), 0xd9fd6 230 |
| 0x1362e00000 | 0xd4d97 494 (this RIP), 0xd3a39 (same last address 0x1362e04f40) |
| 0x1245600000 | 0xf3b026 368 (this RIP), `host:bb-probe.exe+0x241e97` (sceGnmDingDong) 495, `ucrtbase+0xee3ec` 360 |
| 0x1108800000 | 0xa3fb73 364 (this RIP alone) |
| 0x13d5cc0000 | 0x18c3be: 3 small binds of 996, max 196608 (not a 64 KiB granule case) |

Each block receives writes from unrelated RIPs, and the bind covers the whole block. The 0x124560 block also receives the
port's own doorbell write and host memcpy, so it is a shared host/guest block. A granule-sized bind over a heap page
explains "consumed" without the GPU reading the written object.

### 1.3 Static search for a real 64 KiB V#
- Constant V# encoder `0x8655d0` has 7 static callers: `0x8be60`, `0x8c280`, `0x928f0`, `0x92bd0`, `0x995b0`, `0x16eb90`,
  and `Material_BindGraphicsShaders_B` (`0x8601b6`, size 0x24 bytes). None of the decompiled callers passes 0x10000 as a
  constant. Only 7 decomp files near the V# encoders contain 0x10000/65536 (`0x92140`, `0x92e80`, `0x94320`, `0xa24b0`,
  `0x117930`, `0x8655d0`, `0x86d210`), and none is a per-frame 64 KiB uniform.
- The compute path `0x928f0` builds 0x4404 buffers with count from the caller. Its sizes are not constant.
- Conclusion: no evidence of a per-frame 64 KiB uniform buffer for these five RIPs.

### 1.4 Runtime check (not run here; the rules forbid running the game from this task)
At `vk_rasterizer.cpp:5590-5599`, log `vbase`, `vsize`, `size` and `mapped` for every bind with `size == 65536` and
`size == 196608`, with the rip of the consuming draw. If `vsize > size`, the 64 KiB is a clamp (section 1.1). If
`vsize == 65536` with a 64 KiB-aligned `vbase`, it is a real 64 KiB V# and the RIP reads are real. This decides the 64 KiB
question for every RIP of this kind, not only the five.

## 2. 0xd4d97: sub_d4c50, render-entity lists (class c)

### 2.1 Function and object
- `sub_d4c50` (652 bytes) is a `std::vector<u64>::_M_range_insert` (the decomp shows the memmove-and-append path). The
  store at `0xd4d97` (`mov [r8], rax`) is the copy loop of the **append-at-end** case (`pos == end`).
- Single static caller: `sub_d1d40` (site 0xd213b, the fourth of five range inserts; this call: dest `this+0x88`, sources `src+0x70` /
  `src+0x78`). `sub_d1d40` copies five list pairs from a source record into the destination record. Its decomp has two
  Bluepoint assert strings: `pEntity->GetType() == CRenderObject::kType_XPRStaticModelSection` and
  `pEntity->GetType() == CRenderObject::kType_RenderEntity`. So the elements are `CRenderObject*` pointers.
- `sub_d1d40` is called once per record by `sub_d2520` (loop over the 0xa8-byte records of `this+0x1a8`, from the index on
  top of the int stack, section 3). `sub_d2520` is called by `sub_2512a0` (4 sites) and `sub_2737a0` (1 site);
  `sub_2737a0` is called by `sub_24a7a0`, which is reached from `sub_24a0f0` (1128 bytes, called by `237630`, `274040`,
  `24a580`). The frame position of that chain was not traced to the top; the lists are rebuilt per visibility pass.

### 2.2 Allocator
- Vector storage comes from `operator new` = `FUN_00017070(n)` = `FUN_00012820(n, category 1, align 8, 1)`: **CPUMemory**
  (`framedata.md` 1.1). So the pointer arrays share the CPUMemory granules with the per-frame GPU-read blocks of `0x8c0d0`.

### 2.3 GPU consumer and class
- The elements are CPU object pointers (CRenderObject). The GPU never reads them; no V#/T# encoder takes these vectors.
- Class **(c)**: CPU-only bookkeeping that shares a 64 KiB granule with GPU data. No announce needed once tracking is gone.
- Confidence: medium-high for "not GPU-read" (no V# path); the frame position of the chain is not traced.

## 3. 0xd3b7f: sub_d3ae0, record-index stack (class c)

### 3.1 Function and object
- `sub_d3ae0` (650 bytes): `this` holds a `std::deque<int>` at `+0x188/+0x190/+0x198` (4-byte nodes, map grow via
  `sub_d7310`) and a `std::deque` of 0xa8-byte records at `+0x1a8` (`+0x1a0` allocator, `+0x1c0` count, stride 0xa8
  via the inverse constant `0xcf3cf3cf3cf3cf3d`). The store at `0xd3b7f` (`mov [rax], ecx`) is the push of the 4-byte value
  `count` (the record index) into the int deque.
- Callers (10 sites): `sub_d40a0`, `sub_112db0`, `sub_1c70c0`, `sub_250930`, `sub_250d20`, `sub_2512a0` (3 sites),
  `sub_252190` (2), `sub_271570`, `sub_3ab240`, `sub_536050`. The newest index is read back by `sub_d2520`
  (`*(this+0x190) - 4`) to select the record for `sub_d1d40` (section 2).

### 3.2 Allocator and consumer
- Deque nodes come from `sub_d7310` (`operator new` path, CPUMemory). No readers of the int deque outside the CPU record
  walk were found. `sub_d2520` reads only the newest index.
- Class **(c)**: CPU index stack of the record pool. The writes of this RIP are a pool push, not GPU data.

## 4. 0xf3b026: sub_f3af80, CPU float history ring (class c, consumer open)

### 4.1 Function and object
- `sub_f3af80` (394 bytes). The store at `0xf3b026` (`vmovss [rax+rdx*4], xmm0`) writes a float into the ring at `[this+0x110]`
  at index `[this+0x120]` (head, wrapped with `div ecx` by the capacity `[this+0x118]` low dword; high bit masked with
  `0x7fffffff`). It then sums the ring (32-way unrolled float loop) and returns the average (the caller of the return value
  was not identified).
- The Ghidra signature is misleading (`param_1` is the float in xmm0; `param_2`/`rdi` is the object). Read from the
  disassembly.
- Callers: none static (indirect calls). The ring storage allocation was not traced.

### 4.2 Consumer and class
- The ring holds CPU float samples; the GPU never reads it (no V# path found). The average's consumer is not traced.
- Class **(c)** (CPU-only history), confidence medium. If the average becomes a constant, the class changes to (a); the
  next step is the caller that consumes the return value of `sub_f3af80`.

## 5. 0xa3fb73: sub_a3fb60, pointer slot take (class c)

### 5.1 Function and object
- `sub_a3fb60` (29 bytes) reads a 4-byte tag `[this+0x48+4*i]` into `*out` and an 8-byte pointer `[this+8+8*i]` as its
  return value. When `i == 1` it also zeroes the pointer `[this+0x10]` (the store at `0xa3fb73`: `mov qword [rdi+rcx*8+8], 0`).
  This is a "take" of slot 1 (ownership transfer).
- Single static caller: `sub_3deca0` (site 0x3ded39, loop `i = 0..7`). `sub_3deca0` gets the table as `sub_a3f980(this)`
  (`this+0x28`) and writes the 8 pairs through `sub_369410` (`lVar2+0xf0+8*k` pointer, `+0xe0+k` byte) and `sub_369420`
  (node from the job-node pool `sub_14d7b0`, under the lock `sub_95580`).

### 5.2 Allocator and consumer
- The state table is a field of the object at `this` of `sub_3deca0` (its allocator was not traced). The node pool
  `sub_14d7b0` is a CPU pool (static `DAT_02ee3b00`). Nothing reads the zeroed slot on the GPU side.
- Class **(c)**: a CPU null store into a state table; the 64 KiB granule is the object's own CPUMemory page (this RIP is the
  only writer in its block 0x1108800000, which fits a bind over one object's page).

## 6. 0x18c3be: sub_18c390, per-object binding record (class d, unknown)

### 6.1 Function and object
- `sub_18c390` (133 bytes), called by `sub_34cad0` (site 0x34ccac, the only static caller; `sub_34cad0` has no static
  caller and is reached indirectly). Arguments: `rdi` = object `[r15]` (the record of `sub_34cad0`'s `param_1+0xa0`
  chain), `rsi` = 12-byte value (8 bytes + 4 bytes), `rdx` = 8-byte value (transform through `sub_158c10` with
  `{1.0f, 0}`), `rcx` = 8-byte value.
- Stores: `+0x278` (8 bytes), `+0x280` (4 bytes; the store at `0x18c3be` is `mov dword [rbx+0x280], eax`), `+0x284`
  (8 bytes, the transform), `+0x28c` (4 bytes, `esi`), `+0x290` (8 bytes, `*rcx`).
- The same object family has the stage slot table at `+0x770` (`sub_18c420`, called from `sub_34cad0` and `sub_34ccd0`)
  and flags at `+0x208` (`sub_18c2b0`). That table is a resource-pointer table, so this object is a per-draw binding
  object. The record `+0x278..+0x290` sits beside it.

### 6.2 Consumer and class
- The bind sizes are 3 granules (196608) with only 3 small binds of 996, so this RIP does not match the single-granule
  pattern of section 1. The consumer is not known: no reader of `+0x278..+0x290` was found in the decompilation I
  checked (not searched by displacement across all functions).
- Class **(d)**, unknown. Next step: search the decompilation for loads of `[obj+0x278]`, `[obj+0x284]`, `[obj+0x28c]`,
  `[obj+0x290]` and for V# encoders fed from the object (`0x865410`, `0x8655d0`). If a V# is built over this record, the
  class becomes (a) and a hook on `sub_18c390` (Entry or Site after 0x18c3be, covering the 0x298-byte record) is the
  proposal. Until then: no hook.

## 7. What stays unknown
- The 64 KiB bind is most likely a clamp (section 1); the runtime check in section 1.4 decides it. This affects the
  classification of all five, not only one.
- The frame position of `sub_24a0f0` (for 0xd4d97) and of `sub_34cad0` (for 0x18c3be) was not traced to the top.
- The consumer of the float average (0xf3b026) and the readers of the binding record (0x18c3be) are open.
- The allocator of the objects of 0xa3fb73 (`this` of `sub_3deca0`) was not traced.

## 8. Summary table (hook proposal)

| rip | function | object | class | hook proposal |
|---|---|---|---|---|
| 0xd4d97 | sub_d4c50 | CRenderObject pointer lists per record (CPUMemory) | c | no |
| 0xd3b7f | sub_d3ae0 | record-index int deque (CPUMemory) | c | no |
| 0xf3b026 | sub_f3af80 | CPU float ring (average) | c (consumer open) | no |
| 0xa3fb73 | sub_a3fb60 | pointer slot 1 of an 8-slot state table | c | no |
| 0x18c3be | sub_18c390 | 0x298-byte binding record, `+0x278..+0x290` | d | no (open) |
