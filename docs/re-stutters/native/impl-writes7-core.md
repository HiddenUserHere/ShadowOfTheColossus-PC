# Writes wave 7, group "core": engine memory and frame helpers (specialist: core)

Status: static decision for the eight RIPs of the group (0x17c4f, 0x17f25, 0x1805f, 0x1806a, 0x184856, 0x917f2).
Method: Ghidra decomp (`out/re/decomp`), `eboot_re.py func/dis/callers`, `verify.cpp` for the metric. No source edited,
nothing built, nothing committed, the game was not run. Heap map from `heaps.md` (sections 1.1, 1.2, 6.9) and
`framedata.md` (sections 1.1, 7).

Result: none of the six RIPs is a GPU-data write that needs an announce hook. Five are CPUMemory bucket-allocator
bookkeeping (class c). One is a CPU object field (class c, with an open allocation question). The small-bind share
does not separate the classes (see 0), so the "consumed" count is weak evidence in this group.

## 0. Two findings about the metric (apply to every row, not only this group)

1. **A RIP is the first write to a protected page, not the writer of the object.** The page tracker reports the
   faulting instruction. A constructor or allocator reports whichever store hits a protected page first (0x917f2 is the
   first store of `CRenderContext::CRenderContext`, the rest of the object is on the same page). The RIP names a page
   event, so the object it names may be only one of several writers on that page.
2. **"Consumed" is address overlap, not ownership.** `NoteGpuConsume` (verify.cpp, around line 300) marks a suspect
   when the uploaded bind range `[addr, addr+size)` overlaps the suspect's written bytes and the suspect is at most 4
   frames old. It does not check whether the bound data was allocated or written after the suspect. CPUMemory granules
   are reused for GPU chunks and CPU objects alike (`heaps.md` 6.9, `framedata.md` 1.1: the per-frame 0x8c0d0 chunks are
   CPUMemory), so a CPU-object write followed within 4 frames by a bind of a new chunk at the same address is counted as
   consumed. The small-bind share is not a discriminator either: a bind of at most 64 KiB is the bind range, which can be
   an over-estimate of the resource (V# sized to a rounded or larger count), so "a read the shader almost surely makes"
   holds for constant buffers read in full, not for every bind.
   Consequence: the consumed column is an upper bound for CPUMemory RIPs. A fix would store an allocation generation per
   block (or the object's publish frame) and skip suspects written before the current owner's allocation. Not done here
   (source edits are out of scope).

## 1. Objects and heap

- CPUMemory (slot 1) holds the BucketAllocator (64 KiB granules, align 0x10, `0x17990(...,0x10000,0x10,0x400)`), the
  HeapAllocator (below 1 MiB) and the PageAllocator (above 1 MiB) (heaps.md 1.2, 6.9).
- Every `FUN_00017070(size)` is `FUN_00012820(size, CPUMemory, align 8, flag 1)` (decomp 0001/00017070.c): all C++
  objects of the engine, including `CRenderContext` (0x238 bytes), come from CPUMemory.
- The BucketAllocator's granule has its bookkeeping in the granule itself: a free-run node at the granule base (count at
  +8) and a footer at granule_end-0x28 (live-block count) and granule_end-0x20 (free-list head). The bucket's block
  space lies between them. This is read from the code of 0x179c0 and 0x17e90, and it matches the verify "last address"
  values (see the rows).

## 2. Per RIP

### 2.1 0x17c4f, sub_179c0 (BucketAllocator allocate, 0x179c0-0x17d14)
- Instruction: `mov [rax+8], rcx` (decomp `plVar7[1] = lVar4`), in the branch where a free run is split and its count
  field is updated. The write is to the free-run node at the head of the run.
- Last address 0x1043000008 = a granule base + 8, so the node is at the granule base.
- Object: CPUMemory BucketAllocator. The function is entered through the heap object's vtable (the wrapper `sub_179b0`
  is the only direct caller).
- Class: **(c)**. The node lies in free space by construction (it is the head of a free run that the allocator is
  splitting). No GPU read of free space is possible at that instant; a consumed suspect here is an overlap (section 0.2).
- Small binds: 745 of 1167 consumed (64%). Not decisive (section 0.2).
- Hook: **no**.

### 2.2 0x17f25, sub_17e90 (BucketAllocator free, 0x17e90-0x180a6)
- Instruction: `dec dword ptr [r12-0x28]` (decomp `piVar2 = uVar10 - 0x28; *piVar2 += -1`). `uVar10` is the granule
  base above the freed block, so the write is the live-block counter in the granule footer.
- Last address 0x10a460ffd8 = granule_end - 0x28 exactly. Matches the footer layout of 1.
- Object: CPUMemory BucketAllocator granule footer. Written on every free into the granule.
- Class: **(c)**. Bookkeeping in the granule that also holds blocks. The footer is never GPU data.
- Small binds: 175 of 657 consumed (27%). Not decisive.
- Hook: **no**.

### 2.3 0x1805f, sub_17e90 (free, freed block becomes a free run)
- Instruction: `mov [r15+8], 1` (decomp `param_2[1] = 1`), with `mov [r15], rax` next to it: the freed block itself gets
  a free-run header (next pointer, count 1) in its first 16 bytes.
- Last address 0x10d6c0c3c8 (not granule-aligned): the freed block's own first bytes.
- Object: the freed CPUMemory block (any BucketAllocator block: C++ objects, strings, CPU-backed GPU buffers of
  `0x8be60` flags 1/5). The GPU-visible per-frame chunks of `0x8c0d0` are not freed one by one (ring reset, `heaps.md`
  7.2), so they do not reach this path.
- Class: **(c)** for the write. The block is dead after the free; the game frees a GPU resource only after the deleter's
  label gate (`framedata.md` 2.1), so the header is not read by a correct draw. A consumed suspect means a stale or
  reused V#, which is the overlap case of section 0.2.
- Small binds: 90 of 419 (21%). Not decisive.
- Hook: **no**.

### 2.4 0x1806a, sub_17e90 (free, link into the free list)
- Instruction: `mov [rdx], r15` (decomp `*puVar8 = param_2`): the predecessor's next pointer is set to the freed block.
  `rdx` is a free-run node (last address 0x10d6c051c0 is not a granule footer, which would end in 0x...0020/0x...0028).
- Class: **(c)**. Free-list link inside free space.
- Small binds: 85 of 400 (21%). Not decisive.
- Hook: **no**.

### 2.5 0x184856, sub_184830 (`FUN_00184830`, render helper, 0x184830-0x1848c4)
- Instruction: `mov [r12+0x578], rax`. `rax` = the result of `FUN_00092500(*param_2, 9, 1)`, which takes an embedded
  DCB block (`FUN_0086d210`, tag 3, 9 dwords of value 1) and returns its address. The address is stored in the object
  `r12 = param_1` as the EOP label target that `FUN_00092540` (`0x86eb60` EVENT_WRITE_EOP) later uses.
- `r12` is `*(param_1+0x90)` of the callers `FUN_000d0040` (line 123, under `lVar6+0xe0/0xe8` checks) and `FUN_000c2560`
  (line 21). Both run per frame.
- The stored pointer is written to a CPU object every frame. The GPU reads the DCB block (the label), not the object
  field. The DCB block is GPU-read and announced at submit (`framedata.md` site 12), so no hook is needed for it here.
- Class: **(c)** for the store (CPU object field, the GPU never reads the object as data, unless a V# covers the object).
- **(d) open:** where `*(param_1+0x90)` is allocated. If it sits inside a per-frame chunk that a V# covers, the chunk's
  writer is the owner to announce, not this store. Missing: the creator of the object at `param_1+0x90` (not traced in
  this pass; a walk of the `d0040` renderer's constructor would answer it).
- Small binds: 375 of 375 consumed (all 64 KiB). This is the strongest pattern in the group, and it is compatible with
  a 64 KiB bind over the object's granule (section 0.2). It is not proof of GPU data.
- Hook: **no** (until the allocation of the object is known).

### 2.6 0x917f2, sub_917c0 (`CRenderContext::CRenderContext`, context ctor, 0x917c0-0x919b4)
- Instruction: `mov [rbx+8], 0` (rbx = the context object, 0x238 bytes from `FUN_00017070` = CPUMemory). The first store
  of the constructor, which is where the first-page fault lands.
- Last address 0x124bd70008 = granule base + 8. The object sits at a granule base, which fits a fresh BucketAllocator
  block after the granule was reused.
- The same constructor allocates the DCB (`0x12820(param_2, 1, 0x80)`, default 5 MiB) and the CCB (`param_3`, 0x9bc00)
  from CPUMemory as well (section `FUN_00012820(param_3,1,0x80,0)`). Those are large (page or heap allocator, not
  bucket) and are the GPU-read command buffers, announced at submit (`framedata.md` site 12). The write at 0x917f2 is not
  to them.
- Object lifecycle: created every frame by `FUN_00090470` (`RequestNewRenderContext`), the list is cleared each frame in
  `SetupFlipState` (`framedata.md` 2.3). The object is therefore recycled every frame, which is the reuse pattern of
  section 0.2.
- Class: **(c)**. CPU-only C++ object. The GPU-read DCB and CCB are separate allocations and are covered by the submit
  announce.
- Small binds: 379 of 502 consumed (75%); largest 131072. Not decisive.
- Hook: **no**.

## 3. Summary

| rip | function | object | class | hook proposal |
|---|---|---|---|---|
| 0x17c4f | sub_179c0 (BucketAllocator allocate) | free-run node at a CPUMemory granule base | (c) free space | no |
| 0x17f25 | sub_17e90 (BucketAllocator free) | granule footer live counter (granule_end-0x28) | (c) allocator bookkeeping | no |
| 0x1805f | sub_17e90 (free) | freed CPUMemory block header (first 16 bytes) | (c) dead block | no |
| 0x1806a | sub_17e90 (free) | free-list predecessor link | (c) free space | no |
| 0x184856 | sub_184830 | CPU object field `+0x578` (EOP label address), object `*(param_1+0x90)` | (c), open (d) on the object's allocation | no |
| 0x917f2 | sub_917c0 (CRenderContext ctor) | context object, CPUMemory, recycled per frame | (c) CPU object | no |

## 4. Open items

1. The creator of the object at `*(param_1+0x90)` of `FUN_000d0040` / `FUN_000c2560` (0x184856). If it is inside a
   V#-covered chunk, the chunk writer needs the hook, not this store.
2. The verify metric: an ownership check in `NoteGpuConsume` (allocation generation per block) would make the consumed
   column exact for CPUMemory. Until then, read the consumed numbers of CPUMemory RIPs as an upper bound.
3. Whether the bucket granule's footer and node stores can be exempted by a range filter in the page tracker (they are
   CPU bookkeeping in every CPUMemory granule), which would remove most of the unannounced count of this group without
   any hook. Needs the page tracker's range list (`page_manager.cpp`, not read here).
