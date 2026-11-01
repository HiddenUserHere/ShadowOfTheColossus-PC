# 05 - Render context heaps and category tags

Read-only. Addresses are eboot VAs. Pseudocode: `out/re/decomp/<hi16>/<addr>.c`. Labels: **observed**, **inferred**, **unknown**.

## 1. Answer

- **Premise correction (observed, `0x8f6f0` lines 250-259).** The 130 MiB and 20 MiB heaps are not owned by `RenderContextCPU` (+0x3c0) or `RenderContextGPU` (+0x4a0). They are the bump bases of the kind-0 frame allocators nested under them: `RenderContextFrameCPU` (+0x580, 130 MiB) and `RenderContextFrameGPU` (+0x670, 20 MiB). The families are their parents (`0x47dd0`, lines 256-261).
- **The 56 MiB block (ctx+0xa88) is not a linear arena.** It is one of four GPU-family blocks (ctx+0xa80 200 KB, +0xa88 56 MiB, +0xa90 320 KB, +0xa98 400 KB; `0x8f6f0` lines 221-243), read every frame by `0xca150` (called from `BPXPR_PreRender`, `0xca6d0`).
- **Kind correction.** `MaterialParamHeap` and `VirtualMemoryGPU` are kind 2 (`0x8c880:238, 249`), not kind 0 as report 04 says.
- **Category count.** `0x8c370` creates 17 `Renderer`-parented tag records (lines 53-117), not 12.
- **Frame rings.** The two frame allocators are 2-slot bump rings. Vtable slot 4 (`0x48270`) rotates the ring and clears the cursor; slot 20 (`0x48440`) is the bump allocator.
- **Gap.** Callers of those vtable slots are not found, so what writes the rings and whether the GPU reads them is unknown.

## 2. Render-context blocks

Family indices: `0x13380` calls `vtable+0x18(obj,1)` on CPUMemory, which writes `+0x1c = 1` through `0x13d20` (observed). So `DAT_02dd9f80` = 1 is the CPU family (inferred value). `DAT_02dd9f84` is the GPU family index, copied from its `+0x1c` at `0x110a0:66` (value not read).

| Block | Size | `FUN_12820` family | Pool (report 06 rule, inferred) | Held in | Used by |
|---|---|---|---|---|---|
| RC FrameCPU base | 130 MiB | `DAT_02dd9f80` (CPU) | kind 1, Onion | ctx+0x760, passed to `0x481f0` (`0x8f6f0:250-254`) | ring alloc, slot 20 |
| RC FrameGPU base | 20 MiB | `DAT_02dd9f84` (GPU) | kind 2, Garlic | ctx+0x768 (`0x8f6f0:252-255`) | ring alloc, slot 20 |
| GPU block A | 56 MiB | GPU | kind 2, Garlic | ctx+0xa88 (`0x8f6f0:229`) | `0xca150:73` |
| GPU blocks B | 200 KB, 320 KB, 390 KB | GPU | kind 2, Garlic | ctx+0xa80, +0xa90, +0xa98 | `0xca150:72, 78-79` |
| Per-flip chunk | 5 MiB | index 1, not a family (`0x917c0`) | unknown | per-flip context (`0x90470`) | `0x917c0` |

`0x481f0(obj, 2, chunks, base, size)` sets the bump cursor and two chunk slots. `0xca6d0` line 63 copies ctx+0x760 into ctx+0x768 every frame (observed). The FrameGPU object keeps its own base copy, so the allocator is safe, but the slot pair is not a reliable owner record (inferred). The ctx is the object that `0x8f6f0` builds. `0xca150` reads `DAT_02ed33f0+0xa80..0xa98`, so that global is probably the ctx (inferred; `0x8d660` writes it and runs first in `0x8f6f0`).

## 3. Questions

**Q1 (heap users).**
- 130 MiB and 20 MiB: frame ring bases. Rewritten each frame, inferred from the reset at `0x48270`. Writer: unknown (vtable dispatch, no direct caller found).
- 56 MiB and the three smaller blocks: set once in `0x8f6f0`, re-published every frame by `0xca150` into a per-frame descriptor (`lVar3+0xae08..0xae20`). Contents and GPU writes: unknown.
- CPU kind does not mean CPU-only. Onion is GPU-visible on PS4 (general knowledge, not from the repo). Report 04's pool split is placement only.

**Q2 (frame allocators).**
- Alloc: vtable slot 20, `0x48440` (observed in pseudocode): align cursor (+0x98), check chunk end (+0xa8), spill through +0xa0.
- Size: 130 MiB and 20 MiB, two chunks each.
- Reset: slot 4, `0x48270` (observed): rotates to the other chunk, clears +0xa0. Slots 0 and 1 (`0x90420`, `0x91270`) free all via `0x47620`.
- Callers: unknown. GPU reads: unknown. The name suggests CPU writes, unconfirmed.

**Q3 (category tags).** `0x8c370:53-117` creates the records; `0x8c880:12-26` zeroes the ids. Users are direct RIP-relative references (observed, `eboot_re.py refs` over 0x2ed1f68-0x2ed20f0). A record pointer from `0x11bc0` is not stored, so indirect use would not be visible.

| Category | Id global | Users (observed) | Heap |
|---|---|---|---|
| Renderer, RendererBackend | 0x2ed1f68, 0x2ed1f80 | none | - |
| XPR | 0x2ed1f98 | none | - |
| Shaders | 0x2ed1fb0 | `0xb0d3e0:21` (passes address, lea) | unknown |
| Render Targets | 0x2ed1fc8 | `0x9aa10:29`, `0x9aef0:502` (descriptor to `0x452f0` with RT allocator +0x28, size 0x10000) | RT family, kind 2 |
| Textures | 0x2ed1fe0 | none | - |
| CodeGen Textures | 0x2ed1ff8 | none | - |
| Fur Textures | 0x2ed2010 | `0x1ac2d0:39` (descriptor, `&DAT_02dd9f84`), factory `0x97a50` "InteractiveOffset" | GPU (Garlic) |
| Water Textures | 0x2ed2028 | `0x197050:155`, GPU index; `0x8d6c0` "WaterDisplacement", `0x97a50` "WaterSim_*" | GPU (Garlic) |
| LightProbe Textures | 0x2ed2040 | `0x134790:44`, `0x134880:97`, GPU index; `0x97a50` "ProbeLUT" | GPU (Garlic) |
| Models, Models Skinning | 0x2ed2058, 0x2ed2070 | none | - |
| Particles | 0x2ed2088 | `0x10ec60:128`, GPU index; `0x97a50` | GPU (Garlic) |
| Lighting, VirtGeometry | 0x2ed20a0, 0x2ed20b8 | none | - |
| Fur (unnamed) | 0x2ed20d0 | none | - |
| Tessellation | 0x2ed20e8 | none | - |

`0x97a50` is the factory of the `kRTT_*` render targets (report 13), so the GPU-indexed descriptors are GPU-kind allocations (inferred).

**Named heaps.** Created in `0x8c880`, bound in `0x8c370` via `0x1cfb0`, which copies the arena kind into the heap. Slot globals are the values from `0x120d0`.

| Heap | Kind | Arena, slot | Slot global | Users (observed) |
|---|---|---|---|---|
| GameToRenderThread | 1 | CPU, 3 | 0x2ed1f48 | `0x8bd68` |
| GameToGPU | 2 | GPU, 5 | 0x2ed1f4c | none |
| CPU_RenderToGPUToGame | 1 | CPU, 6 | 0x2ed1f50 | `0xafdd0`, `0xb020d` (16-byte) |
| RenderSingleFrameGPU | 2 | GPU, 2 | 0x2ed1f54 | at least 60 functions in the first 80 grep hits, mostly 8-40 byte allocs (`0xb50c0`, `0xb5b10`, `0xb9710`, `0xb6e20`) |
| RenderSingleFrameCPU | 1 | CPU, 2 | 0x2ed1f58 | render-job path `0xc1312`, `0xc148e`, `0xc557b`, `0xc72e0`-`0xcdcc0` |
| XPRTempData | 0 | not bound | none | none found |
| MaterialParamHeap | 2 | 4 MiB CPU-family backing (`0x8c370:41`) | 0x2ed1f5c | none |
| VirtualMemoryGPU | 2 | 4 MiB GPU block at 0x2ed2110 (`0x8c370:48`), reader unknown | 0x2ed1f60 | `0x96a99`, `0x98bd9`, `0x98cc7` |

Report 06 says index 2 exists only on the kind-2 arena. Observed: index 2 is on both arenas (RenderSingleFrameGPU and RenderSingleFrameCPU).

**Q4 (proposed classes).**

| Heap or tag | Class | Confidence and evidence |
|---|---|---|
| RC FrameCPU 130 MiB ring | CPU_ONLY staging; upload to GPU_DYNAMIC if the GPU reads it | low; 2-slot ring, CPU kind |
| RC FrameGPU 20 MiB ring | GPU_DYNAMIC (per-frame upload ring) | low; GPU name, ring, GPU reads unknown |
| GPU blocks 56 MiB and three smaller | unknown (GPU_STATIC or GPU_WRITTEN) | unknown; contents not identified |
| RenderSingleFrameGPU | GPU_DYNAMIC (per-frame ring replacing the small-object heap) | medium-low; many per-frame small allocs |
| RenderSingleFrameCPU | CPU_ONLY | medium; kind 1, render-job path |
| MaterialParamHeap | GPU_DYNAMIC | low; kind 2, CPU-kind backing |
| Render Targets | GPU_WRITTEN (VRAM) | high; `kRTT_*` in `0x9aef0` |
| Water sim, Particles, ProbeLUT | GPU_WRITTEN for sim state; GPU_STATIC for LUTs | low; names only |
| Textures, Models, VirtGeometry, Tessellation, Lighting, Renderer | unknown | zero direct references |

## 4. Generic allocator (affects report 06)

`0x17070` calls `FUN_12820(size, 1, 8, 1)`. Index 1 is the CPU family (section 2), unless a thread-local override is set (`0x12820` lines 17-21). Generic allocations from many callers therefore probably land in the kind-1 family (Onion), not Garlic (inferred). Report 06's kind-0 rule holds only for kind-0 objects. Verify with the `ALLOC_BLOCK` probe.

## 5. Next steps (no behaviour change)

1. Find the callers of frame-ring vtable slots 4 and 20 (`0x15976c8`). This decides Q2.
2. Check the consumer of `0xca150`'s descriptor (`lVar3+0xae18`) to see whether the GPU reads the 56 MiB block.
3. Find the consumer of the 4 MiB GPU block at 0x2ed2110.
4. Check whether the Models, Textures, VirtGeometry, Tessellation and Lighting tags reach the allocator through a generic `uint` argument (`0x452f0`).
5. Log `0x12820` (size, index), the `0x48440` return (chunk, cursor), and the `0x48270` call rate.
