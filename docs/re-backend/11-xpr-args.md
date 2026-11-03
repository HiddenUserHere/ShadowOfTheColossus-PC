# 11 - XPR indirect args: who writes them, and can the per-object draws become one multi-draw per group?

Read-only static analysis of `out/eboot.elf` (Ghidra pseudocode in `out/re/decomp/<4 hex>/<8 hex>.c`,
`tools/re/eboot_re.py` where noted). Port sources read for packet semantics. Nothing built, run or edited.
Addresses are eboot virtual addresses.

Labels: **observed** (code, string, import or port source at the address given), **inferred** (reasoning
from observed code), **unknown** (not decided by the files read).

Builds on `docs/re-backend/00-SUMMARY.md`, `01-backend-interface.md`, `docs/re-stutters/reports/01-xpr-cull.md`
and `docs/re-stutters/reports2/04-compute-passes.md`.

## 0. Corrections to earlier reports

- **Observed:** report 01 says `0x1a6d20` calls `sceGnmDrawIndexIndirect` (`0x86b3d0`) "twice per visible
  object" (sites `0x1a716b`, `0x1a71b2`). The decompile shows one call per visible object on every path.
  The two sites are alternative branches on the shader kind `[ctx+0xc548]` (the `LAB_001a71a0` and
  `LAB_001a7149` tails), not a pair.
- **Observed:** `0x1a6d20` is called per **batch entry**, not per set. Its `param_1` is `*(entry+0x18)` (from
  `0x19af20`), and the object index inside it is a bit index (0..count-1) in a 2048-bit mask.
  `0x11d7a0` (RecordMaskedDraws) loops the batch entries of a pass and calls `0x19af20` for each.

## 1. Question 1: who writes the 20-byte args records, and is the CPU visibility bitset from the same pass?

### 1.1 Record layout

- **Observed:** the consumer's per-object draw uses `data_offset = objIdx * 0x14` (`0x1a6d20`, `iVar6 =
  (int)uVar15 * 0x14`). The port's `DrawIndexedIndirectArgs` is `static_assert(sizeof == 0x14)`
  (`gpu/shadps4/video_core/amdgpu/pm4_cmds.h:1122-1129`) and the CP uses it as the stride
  (`liverpool.cpp:4406`). The 20 bytes are the standard indexed indirect record: indexCount, instanceCount,
  startIndex, baseVertex, startInstance (**inferred** from the field order; the port's struct is the
  reference for the CP, `liverpool.cpp:4377-4418`).
- **Observed:** the port's single-draw packet has only `data_offset`, `base_vtx_loc` (dw2, bits 0-15) and
  `start_inst_loc` (dw3, bits 0-15) (`pm4_cmds.h:1131-1143`). The CP fetches baseVertex and startInstance
  from the record and writes them to the vertex and instance user SGPRs. No draw index is written by this
  packet.

### 1.2 Args buffer and its clear

- **Observed, `0x11dbe0` (IndexCullXPRObjects, 1433 bytes):**
  - Args buffer: `FUN_0008be60(local_80, 0x1b, 0x14, 10000, 0x804, [lVar12+0xab98+set*0x50], 0)`. Element
    stride 0x14, 10000 elements, per pass-set slot (`set*0x50` into `[ctx+0xab70]`).
  - A second buffer with 4-byte elements, 0xe00000 count, at `[lVar12+0xaba0+set*0x50]`. `0x11d7a0` binds the
    same size as an index buffer (`FUN_0086bfb0(..., 0xe00000)`, `0x11d7a0` line "FUN_0086bfb0(*puVar9,0xe00000)").
    **Inferred:** this is the culled index buffer.
  - Clear: `FUN_00092b50(cmd, [lVar12+0xab98+set*0x50], 50000, zero20, 5)` after a `"Clear Dispatch Draw
    Buffer"` marker. `50000 = 10000 * 5` dwords, and the source is a 20-byte zeroed block (`pauVar9`, written
    to 0). **Inferred:** this is a GPU fill of the whole 10000-record args buffer with zeros, run every time the
    IndexCull job runs. `0x92b50` is dispatch + EOP + WAIT (report 04 section 2.1).
- **Observed:** after the clear, `0x11dbe0` calls `FUN_0019b330` for each entry of the pass list
  (`param_2+8 .. +0x10`, 16-byte entries).

### 1.3 Who writes non-zero records

- **Observed, `0x19b330` (205 bytes):** selects a pointer from the object's 0xa8-byte entry
  (`lVar1 + XPR_index*0xa8 + field`, field chosen by a switch on `[param_3+0x44]`), then calls
  `0x1a8180` when `param_2[1] == 0`, else `0x1a85f0`.
- **Observed, `0x1a8180` (1104 bytes) and `0x1a85f0` (1079 bytes):** both loop the **set bits of the mask**
  (`param_2`, the 2048-bit mask, scanned with the same blsi/lzcnt loop as `0x1a6d20`). For each visible bit:
  - write a 0x20-byte record into `param_3+0x18` (fields: `[0]` index count u32 from `plVar4[5]+idx*8`,
    `[8]` = object index `uVar19`, `[0x10]` = running index total `param_3+0x40`, `[0x14]` = record ordinal
    `param_3+0x3c`);
  - write 8-byte pointers into `param_3+0x20` (to `param_5+0x98/0xa8/0xb0/0xd0`);
  - dispatch once for that object: `FUN_0086b9e0(..., (indexCount/3 + 63) >> 6, ...)` in `0x1a8180`, or
    `FUN_00883870(..., same groups, ...)` in `0x1a85f0`.
  - Increment `param_3+0x3c` (count) and add the object's index count to `param_3+0x40`.
- **Inferred:** the 0x20 records are the cull shader's per-object input: object index, index count, and the
  offset where its surviving indices go. The GPU cull shader then writes the object's args record at
  `objIdx * 0x14` in the args buffer and its culled indices into the culled index buffer. The shader was not
  decoded (**unknown**: exact output fields).
- **Observed:** the CPU emitters do not write args records for culled objects. They only iterate visible bits.

### 1.4 Answer to question 1

- **Args are written only for visible objects.** Culled objects keep the zero written by the clear in
  `0x11dbe0`. Their records have indexCount 0 and draw nothing (**inferred**, depends on the cull shader
  writing only visible records and on the clear running in the same frame).
- **The CPU visibility bitset is produced by the same pass.** `0x19c1a0` (called from `0xc6bd0` and `0xc6f70`,
  the GPU fragment cull passes, report 01) writes the mask. The consumer `0x1a6d20` and the emitters
  `0x1a8180` / `0x1a85f0` all read the same mask pointer, chosen by the switch in `0x19af20` / `0x19b330`
  from the entry's 0xa8 block (**observed** that they read the same mask argument; **inferred** that it is the
  pass mask written by `0x19c1a0`).
- **Consequence:** the CPU and the GPU agree on visibility because both use the same mask bits, so the CPU
  already knows the visible set when it records. The cull is GPU-written, CPU-read.
- **Timing:** unknown. Report 01 notes the consumer does not wait on the cull label; the label wait is in
  `0x92b50` and `0x86eec0` inside `0x11dbe0`. The mask read by `0x1a6d20` is not known to be fenced
  (**unknown**).

### 1.5 Membership test: an important difference

- **Observed:** `0x1a6d20` tests membership before it emits a draw:
  `(*(uint*)(plVar5[4] + objIdx*4) >> (param_3[+0x20] & 0x1f)) & 1`. The bit index is the pass set index.
- **Observed:** `0x1a8180` and `0x1a85f0` do not test `plVar5[4]`. They take every mask bit.
- **Consequence:** if a mask bit can be set for an object that is not a member of this set, the emitters
  write a non-zero record and dispatch the cull for it, while the consumer skips the draw. An object that
  the consumer skips still has a non-zero record. This matters for any multi-draw (section 4). Whether the
  mask can ever contain non-members is **unknown**.

## 2. Question 2: are object indices contiguous within a (material, mesh) group?

### 2.1 Layout of the bound arrays (`0x1a6d20`, observed)

`plVar5 = *(entry+0x50)`:

| Field | Use in `0x1a6d20` | Label |
|---|---|---|
| `plVar5[0]` (u16 array) | material **exclusive end** bounds; count = low 16 bits of `entry+0x60` | observed |
| `plVar5[1]` (u16 array) | mesh **exclusive end** bounds; count = high 16 bits of `entry+0x60` | observed |
| `plVar5[2]` (8-byte ptrs) | material object for group `k` at `[k-1]` | observed |
| `plVar5[3]` (0x20-byte records) | mesh record for group `k` at `(k-1)*0x20` (address, index buffer size, user-data offset `u16 +0x18`) | observed |
| `plVar5[4]` (u32 per object) | membership bits per set, indexed by object index | observed |
| `plVar5[5]` (8-byte per object) | index count (`u32 +0`) and a second u32 (`+4`) | observed |

Object count is `u16 entry+0x64`.

### 2.2 Are groups contiguous?

- **Observed:** the material scan and the mesh scan in `0x1a6d20` only move forward (`uVar19`, `uVar18`
  never reset inside the bit loop). They stop at the first bound with `objIdx < bound`. That scan is only
  correct when the bound arrays are ascending, so a material group and a mesh group are each an interval of
  object indices, `[bound[k-1], bound[k])`. The same holds in `0x1a8180` and `0x1a85f0`.
- **Inferred:** the intersection of a material interval and a mesh interval is also an interval. So a
  (material, mesh) group is a contiguous index range `[max(start), min(end))`, inside one batch entry.
- **Unknown:** how `plVar5[0]`, `plVar5[1]` and the object order are built. The builder is `0x19b760`
  (BuildSetDraws, 2610 bytes, called from `0xc6f70`) and `0x19b400` (called from `0xc6810`). Their
  sorting key was not decoded. **Do not assume the index order is sorted by material or mesh without
  checking those two builders.** The forward-only scan requires it, but the scan does not prove it.
- **Observed, `0xc6810` (XPRRenderInstancePreShaders):** the object XPR index `[obj+0x148]` is a slot number
  picked from a 2048-bit occupancy bitmap by `FUN_00035930`. So indices are slots assigned per frame, not
  inherently sorted. The sort, if any, happens in the bound builders.

### 2.3 What a group looks like in the loop

- Material change (group change block, `if (uVar20 != uVar19)`): `FUN_0016b890(4 or 8)` and
  `FUN_0016b850(material+0x38, ...)`, `FUN_000f8990(material, cmd)`.
- Mesh change (`if (uVar7 != uVar18)`): `FUN_0016b850(mesh.addr)`, `FUN_000ae150(...)`, then index base
  `0x86bf50` (`C0012600`, INDEX_BASE) and index size `0x86bfb0` (`C0001300`, INDEX_BUFFER_SIZE) when the
  cached value differs, `0x86bed0` (index type, `C0002A00`-family) when it differs.
- These are group-level only: none of them runs per object.

## 3. Question 3: what varies per object inside a group?

### 3.1 Per-object work in `0x1a6d20`

For each visible bit the consumer runs, in order:

1. `FUN_0085fc10(ctx+0x2e8)`: the material state flush (`Material_BindGraphicsShaders_B`, 3994 bytes).
2. Read `iVar4 = [ctx+0xc548]` (shader kind) and `bVar2` from the material (`[material+0x27]` or `[+0x19]`).
3. One `FUN_0086b3d0(ctx, objIdx*0x14, stage, vsSlot = bVar2 & 0xf, isSlot = bVar2 >> 4, flags = 0)`.

Nothing else is written per object (**observed**; the group-change branches are the only other writes).

### 3.2 Does the flush write anything per object?

- **Observed:** every write in `0x85fc10` is gated by a dirty byte or bit that the function clears at the end:
  bit `0x20` of `[ctx+0x2e8+0xc260]` and byte `+0xc24b` (cleared at the end, `*(0xc24b)=0`, `(0xc25b)=0`),
  `+0xc24c`, `+0xc254`, `+0xc253` (cleared), `+0xc251`, `+0xc249`, `+0xc24a`, `+0xc252`, and bit `1` for the
  second block with `+0xc612`, `+0xc24e`, `+0xc24d` (see the tail of the decompile). The writes go to PM4
  through `0x86c2c0`, `0x86cb00/0x86cb60`, `0x86d4c0`, `0x86d5e0`, `0x86a7f0`, `0x8619a0`, `0x861ce0`,
  `0x861e60`, `0x86c760`, and `0x8643f0` (bump allocation).
- **Inferred:** nothing in the consumer loop sets those dirty bits, so after the first flush of a group the
  calls are no-ops. This is the reason the flush can be moved out of the per-object loop. Verifying it needs a
  runtime count of PM4 words per flush, which this task did not do (**unknown** until measured).
- **Unknown:** `iVar4` (`[ctx+0xc548]`) is read per object. Whether the flush or another path changes it
  inside a group was not checked.

### 3.3 What the shader gets per object

- **Observed:** the per-object packet passes `data_offset = objIdx*0x14`. The CP reads baseVertex and
  startInstance from the record and writes them to the user SGPRs given by the packet's slots
  (`liverpool.cpp:4377-4418`, `pm4_cmds.h:1131-1143`).
- **Observed:** the consumer loop has no per-object user-data write other than the flush (section 3.2).
- **Conclusion:** the only per-object variation the shader can see is the 20-byte record (index count,
  instance count, start index, base vertex, start instance). The record is GPU-written for visible objects and
  zero for the rest (section 1.4).

## 4. Question 4: can one multi-draw cover a group's index range, and is the result image-identical?

### 4.1 Available packets

| Packet | Engine import | Wrapper | Callers in eboot | Port |
|---|---|---|---|---|
| `sceGnmDrawIndexIndirect` (NID `ED9-Fjr8Ta4`) | yes (1 hit in eboot) | `0x86b3d0` | 3 | `gnmdriver.cpp:759` |
| `sceGnmDrawIndexIndirectCountMulti` (NID `thbPcG7E7qk`) | yes (1 hit) | `0x86b470` | **0** (no direct caller) | `gnmdriver.cpp:786` |
| `sceGnmDrawIndexIndirectMulti` (NID `5q95ravnueg`) | **no** (0 hits in `out/eboot.elf`) | none | none | `gnmdriver.cpp:824` |
| `sceGnmDrawIndexMultiInstanced` (NID `jHdPvIzlpKc`) | yes (1 hit) | `0x86aeb0`/`0x86af30` | 0 | not checked |

- **Observed:** the eboot does not import `sceGnmDrawIndexIndirectMulti`. Using it would need a new import
  entry, which is not a simple call site.
- **Observed:** `sceGnmDrawIndexIndirectCountMulti` is imported and its wrapper `0x86b470` (154 bytes) exists,
  with no direct caller in the eboot. It takes `(cmd, 0x10, data_offset, max_count, count_addr u64,
  stage, vsSlot, isSlot, flags)` and writes `count_addr != 0 ? 1 : 0` into `<<30` and `max_count` plus
  `sizeof(args)` = 0x14 as the stride. With `count_addr = 0` the CP draws `max_count` records
  (`gnmdriver.cpp:786-822`; the CP's count path was not read).
- **Observed, stage index:** the consumer's stage argument is 2, 4 or 6. The port's
  `indirect_sgpr_offsets` table (`gnmdriver.cpp:72`) is `[0,0,0x4c,0,0xcc,0,0x14c]`, which matches VS, ES and
  LS user-data bases. **Inferred:** 2 = VS, 4 = ES, 6 = LS, so all three are accepted by the CountMulti
  stage check (`shader_stage == Vs || Es || Ls`, `gnmdriver.cpp:790`). The port's `ShaderStages` enum was not
  located, so this is **inferred**.
- **Observed, Neo gate:** `sceGnmDrawIndexIndirectCountMulti` runs only when
  `!sceKernelIsNeoMode() || !UseNeoCompatSequences` (`gnmdriver.cpp:790`). Whether SotC runs in that mode was
  not checked (**unknown**).

### 4.2 Proposed patch

Per (material, mesh) group, in place of the per-object packets:

- Group interval: `[start, end)` with `end = min(material bound, mesh bound, count)`, inside the batch.
- One `CountMulti(cmd, 0x10, start*0x14, end-start, 0, stage, vsSlot, isSlot, flags)` with the same stage and
  slot values as today.
- One `FUN_0085fc10` per group (or per batch) instead of per object (section 3.2, inferred no-op).
- Keep the record order ascending, so the rasterization order is the same as today.
- Keep the mask scan, but skip the bits inside the group after emitting it.

### 4.3 Is it image-identical?

**Conditionally, not proven.** It is identical only if all of these hold:

1. **Zero records for non-drawn objects in the interval.** Culled objects keep the zero from the clear (section
   1.2-1.4). Their draws are no-ops. Condition: the clear (`0x11dbe0`) runs in the same frame as the consumer,
   and the cull writes nothing for them. Risk: the `forceUseLastIndexBufferCullingResults` cvar
   (`0xf9a971`, report 04 section 3) skips the cull and reuses last frame's records. That would draw
   stale records for objects that are culled this frame (**unknown**, must check the cvar's gate).
2. **Mask bits are a subset of membership.** The emitters `0x1a8180` / `0x1a85f0` have no membership test
   (section 1.5). An object that is in the mask but not in the set would get a record and be drawn by the
   multi-draw, but not by the consumer. Condition: mask ⊆ membership for every set (**unknown**; check at
   runtime or find the mask writer's test in `0x19c1a0`).
3. **Per-draw state comes only from the record.** The single-draw and CountMulti packets write only baseVertex
   and startInstance, from the record (section 3.3, `pm4_cmds.h` and `liverpool.cpp`). Per-object flush writes
   are no-ops (section 3.2, inferred). So an in-group multi-draw has the same SGPR inputs per draw.
4. **Index buffer constant over the group.** The mesh is bound per mesh group (`0x86bf50`/`0x86bfb0`), which
   the patch keeps. The index base in the records must be relative to that index buffer (**unknown**, the mesh
   record's address field `+0x8/+0xc` vs the culled buffer; the consumer binds the mesh's own buffer, so the
   records are presumably relative to it).
5. **Stage and mode.** CountMulti must be accepted for stages 2/4/6 (**inferred**) and the Neo gate must be
   satisfied (**unknown**).

### 4.4 Gain

- **Unknown.** The count of visible objects per frame is ~3,980 draws (report 04 table, 57% of draws). The
  number of (material, mesh) groups per frame was not counted. Per-object draws would collapse to one per group
  and one per visible-object gap, so the gain depends on the group count. A runtime counter is needed.

## 5. Patch points (summary)

| # | Address | What | Kind |
|---|---|---|---|
| P1 | `0x1a6d20`, group-change blocks (material and mesh) | compute group end from `plVar5[0]`, `plVar5[1]` and count | engine patch |
| P2 | `0x1a6d20`, shader-kind tails `LAB_001a71a0` (stages 2/4) and `LAB_001a7149` (stage 6) | replace `FUN_0086b3d0(ctx, objIdx*0x14, ...)` with one `CountMulti` (wrapper `0x86b470`) | engine patch |
| P3 | `0x1a6d20`, `FUN_0085fc10(ctx+0x2e8)` in the per-object loop | move to per group (inferred no-op) | engine patch |
| P4 | `0x1a6d20`, mask bit loop | skip the bits covered by the emitted group | engine patch |
| P5 | `0x1a8180`, `0x1a85f0` | add the membership test `plVar5[4]` (or verify mask ⊆ membership) | engine patch, only if the unknown in 4.3(2) fails |
| P6 | `0x11dbe0` clear (`0x92b50`, 50000 dwords) | must run whenever the consumer runs; check the `forceUseLast...` gate | check, no patch |
| P7 | new import | `sceGnmDrawIndexIndirectMulti` is not imported; use CountMulti instead | not needed |

## 6. Unknowns to settle (runtime, not done here)

1. Count non-member mask bits per set (P5 / 4.3(2)). Compare `mask & ~membership`.
2. Verify the `forceUseLastIndexBufferCullingResults` path: does it skip the clear and the cull?
3. Count (material, mesh) groups per frame in the consumer, to size the gain.
4. Check how `plVar5[0]`, `plVar5[1]` and object order are built in `0x19b760` and `0x19b400`.
5. Check the cull shader's output for culled objects (must write zero or nothing).
6. Check the port's CountMulti path with `count_addr = 0` and the Neo gate.
7. Check whether `iVar4 = [ctx+0xc548]` changes inside a group.

## 7. Files read

- `docs/re-backend/00-SUMMARY.md`, `docs/re-backend/01-backend-interface.md`,
  `docs/re-stutters/reports/01-xpr-cull.md`, `docs/re-stutters/reports2/04-compute-passes.md`.
- Decompiles: `out/re/decomp/001a/001a6d20.c`, `0086/0086b3d0.c`, `0086/0086ad40.c`, `0086/0086bf50.c`,
  `0086/0086bfb0.c`, `0086/0086b470.c`, `0085/0085fc10.c` (head and tail), `0011/0011dbe0.c`,
  `0011/0011d7a0.c`, `0019/0019af20.c`, `0019/0019b330.c`, `0019/0019b400.c`, `0019/0019b760.c` (head and
  grep), `001a/001a8180.c`, `001a/001a85f0.c`, `001a/001a7290.c` (head), `000c/000c6810.c` (head),
  `000c/000c6bd0.c` (head), `000c/000c6f70.c` (not read in full), `0019/0019c1a0.c` (calls only).
- Port: `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp` (lines 72, 729-840, 3283-3287),
  `gpu/shadps4/video_core/amdgpu/pm4_cmds.h` (1093-1170), `gpu/shadps4/video_core/amdgpu/liverpool.cpp`
  (1601-1602, 4360-4470), `src/import_names.inc` (385-390).
- Import check: `out/eboot.elf` byte search for the three NIDs above.
