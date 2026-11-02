# ARCH-BINDSET: event-driven resource binding for the draw thread

## Problem

The GpuDraw thread is the 60 FPS bottleneck. Profile (14 s of gameplay, every new path on:
SOTC_HEAP, N5_BINDLESS, BINDLESS_SAMP, LAYOUT_UNIFIED, ADDR_TABLE, SHADER_ADDR, HEAP_VS,
HEAP_COMPOSITE_OP, TEX_BLOCK, GEN_BIND, SYNC_FAST):

- 7,450 ops/frame at 4.1-4.2 us/op, about 31 ms/frame (budget for 60 FPS: 16.7 ms).
- Rasterizer::BindResources 40.5% inclusive: BindBuffers 16.7, ObtainBuffer 7.4, BindTextures 7.1,
  CopyGpuResults/NoteWrittenBind 6.8, TableMemo::Resolve 6.6, FindImage 3.9,
  FindTextureIdentity 3.7, ReplaceChangedPages 3.7, PrepareRenderState 3.3, BeginRendering 3.1,
  Pipeline::BindResources 2.5, ResetBindings 2.5, WriteScopeGuard 2.4.
- ntdll 17% (locks, waits), ucrtbase 9% (malloc, memcpy).

The cost is spread over dozens of 1-7% items. The root cause is the model: every op rediscovers
its resources (decode V#/T#/S#, look them up, check and sync memory, transition, write
descriptors, record writes), although the game draws nearly the same thing every frame.

## Idea

Invert the model: per-op pull becomes per-event push. The draw thread becomes a thin commit loop:

    for each op: pipeline (memoized) -> BindSet (one lookup) -> dynamic-state delta -> push -> draw

Target: 0.5-0.8 us/op, 4-6 ms/frame on one thread.

## Parts

1. BindSet. The key is a hash of the op's raw resource words (user data, plus the T#/S#/V#
   words the stages read from the flat buffer). The value is an immutable BindSet: the GPU
   address of the texture/sampler index table, static flags of the shaders (writes memory or
   not), precomputed write ranges, and the list of images it references. A hit is one lookup
   plus a push of a pointer, with no per-hit validation.
2. Invalidation by event. A reverse index (image -> BindSets) kills the BindSets of an image
   when it is evicted or recreated, when a view is released, or when the CPU writes its memory.
   A dead BindSet is resolved again once. Buffers need none: the page table resolves them on
   the GPU.
3. Freshness by event. CPU writes mark images dirty (existing page tracking). Dirty images are
   uploaded in a batch once per submit, not checked per op (replaces per-op SynchronizeMemory,
   ObtainBuffer and UpdateImage).
4. No per-op descriptors. Textures and samplers always go through the heap (fragment, vertex,
   compute; remaining refusals such as UiLinearRule get heap-side handling). All buffers go
   through the address table: vertex buffers by vertex pulling, index buffers read in the shader
   (non-indexed draw), indirect arguments served from the ring-8 arena VkBuffer. One global
   descriptor set per command buffer.
5. Coarse barriers. Every image stays in GENERAL (SOTC_LAYOUT_UNIFIED). A barrier is emitted at
   render pass boundaries and after an op whose shader writes memory (a static per-shader bit),
   as a global memory barrier.

## Measurements (2 Nov, test route, all heap/address switches on)

Op phases (SOTC_OP_PHASES=1, ms per second of play at ~30 FPS): bind_resources 463 (buffers clean 83,
written 20, textures 103, the rest unattributed ~257), other 77, begin_rendering 73, hle 36,
render_state 35, descriptor_push 31, reset 27, dynamic_state 22, vertex_index 17. bind_resources is
~60% of op time.

Repeat rate of an op key (SOTC_OPSET_STATS=1, 4 windows of 5 s, ~180k ops/s):

| Key | Hit |
|---|---|
| A: pipeline + user data + flat buffer + render targets | 40-62% |
| C1: A without the base words of shader-addressed V#s | A + 1% |
| C2: C1 without the user data | 49-74% |
| D: decoded descriptors only (T#, S#, V# with the addressed base zeroed) + pipeline + targets | 95.5-98% |

What changes from frame to frame is constant data the shader reads from the flat buffer, not the
bindings. The op record (OpSet) is keyed by D; the flat buffer is copied per op (cheap) and not part
of the key. Estimate: 97% hits at ~0.5 us and misses at ~5 us gives ~0.65 us/op, ~5 ms/frame.

## OpSet (supersedes stage 1's texture-only BindSet)

Key D. Value: everything BindResources produces except the per-op data copy: buffer bindings and
descriptor writes, heap index tables, packet words that are not ring addresses, written ranges,
static barrier needs, and the lists of images and buffer ranges for the reverse index. Invalidation by
events: image events (stage 1 hooks), buffer placement (buffer cache OnPlace/OnRemove), CPU-dirty pages
of non-arena buffers (sync on hit only for those), pipeline destruction.

## Stages (each behind a switch, each with a verify mode)

| # | Stage | Expected cut |
|---|---|---|
| 1 | BindSet with reverse-index invalidation (SOTC_BINDSET=1, =2 verify) | TableMemo per-hit work, most of BindTextures and CopyGpuResults |
| 2 | Batched event-driven uploads | ObtainBuffer, SynchronizeMemory |
| 3 | Vertex, index and indirect arguments by address | the rest of BindBuffers |
| 4 | Barriers from static write bits; drop per-op write records (WriteScope, NoteWrittenBind, CollectMap) | |
| 5 | Hashes computed on the prep threads (ARCH-PARALLEL-DRAW.md) | the draw thread keeps only the commit |

## Verification

Each stage has =2: the old path runs beside the new one and the results are compared and
counted (same pattern as SOTC_HEAP_GPU_VERIFY, SOTC_SHADER_ADDR=2, SOTC_TEX_BLOCK=2,
SOTC_GEN_BIND=2). A stage becomes default only with 0 mismatches on the test route
(user/routes/tests.json) and a correct image.

## Risks

- Freshness of textures (stage 2): correctness today comes from the per-op check. Mitigation: the
  verify mode, and event sources audited one by one (CPU page faults, DMA, GPU writes).
- Hash collisions on the BindSet key: the key stores the full words, compared on a hit.
- Shader changes for stage 3 invalidate the shader cache (rebuilt on first launch).
