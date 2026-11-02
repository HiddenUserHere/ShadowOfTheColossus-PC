# Architecture wave brief (2 Nov): 15 specialists build ARCH-DRAW.md in parallel

You are one of 15 specialists. Read, in this order, before touching anything:
1. CLAUDE.md at the repo root.
2. docs/re-stutters/native/ARCH-DRAW.md (the design: you build one part of it).
3. gpu/shadps4/video_core/sotc_arch/sotc_arch.h (the CONTRACT: the interfaces every part uses).
4. This file: the shared rules, the ownership map, YOUR section.
5. Every function your section names, in full, before you design anything.

## Rules (all specialists)
- Worktree base: `git checkout --detach 838677c`, then
  `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/arch-contract.diff` (adds the contract,
  sotc_arch.cpp with SwitchMode, and ARCH-DRAW.md). Check `git status`.
- Do NOT commit. Do NOT run the game (the coordinator runs it).
- Code against the contract. Do NOT change sotc_arch.h; if it is wrong or missing something, say so in your report
  with the exact change, and code against your proposed change only inside your own files (a local helper).
- Edit ONLY what the ownership map gives you. Anything else: the exact snippet with file:line in your report.
- Your switch (named in your section) is read once through SotcArch::SwitchMode. Unset = today's behaviour bit for
  bit (prove it by pointing at the branches). =2 = verify where the section says so: run new AND old, use OLD,
  count differences. =1 = new only.
- Counters: a Report function in your file (signature like the contract's Report), and the driver.cpp lines to call
  it as a snippet (driver.cpp ~2000-2090 prints the 5 s lines).
- New .cpp files go in gpu/shadps4/video_core/sotc_arch/ (CMake globs video_core recursively: no CMake edit).
- Syntax check every touched/new .cpp: `C:/msys64/clang64/bin/clang++.exe -fsyntax-only -std=gnu++23 <flags of the
  vk_rasterizer.cpp entry of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json, with the worktree's
  gpu/shim and gpu/shadps4 -I paths FIRST, -include <worktree>/gpu/shim/bb_win_compat.h, third-party paths left in
  the main tree> <file>`; C files: the src/probe.c entry, -std=c11 -Wall -Wextra -Werror. Paste commands and output.
- Style: surrounding code; English comments; `sotc:` marker on changes in gpu/shadps4/...; no dead code.
- Vulkan: use vulkan-hpp as the surrounding code does (VULKAN_HPP_NO_EXCEPTIONS: check results). Every extension or
  feature goes through the accessors of section 15 (below); never assume a feature is present.
- Final report in your last message: what you built (file:function), the proof that unset is unchanged, checks run
  and output, snippets for others, exactly what the coordinator's run must print, risks, unknowns.

## Instance accessors (implemented by specialist 15; everyone else calls them as written here)
`bool Instance::SotcUnifiedLayouts() const`, `bool Instance::SotcDescriptorHeap() const`,
`bool Instance::SotcSparseBuffers() const` (sparseBinding + sparseResidencyBuffer + a sparse-capable queue),
`u32 Instance::SotcSparseQueueFamily() const`, `bool Instance::SotcDeviceGeneratedCommands() const`,
`bool Instance::SotcBufferDeviceAddress() const`. Each is false when the feature is missing or its switch is off.

## Ownership map
| # | Part | Owns |
|---|---|---|
| 1 | A0 op cost | NEW sotc_arch/opcost.cpp; the timing lines at the start/end of Liverpool::ExecuteBackOp, SubmitDraw, SubmitDrawIndirect, SubmitDispatch (liverpool.cpp) |
| 2 | A1 unified layouts | NEW sotc_arch/layouts.cpp; Image::Transit and the layout helpers in texture_cache/image.{h,cpp}; the layout choice in BindTexturesFull's second pass and in BeginRendering's attachment layouts (vk_rasterizer.cpp: only the lines that pick a vk::ImageLayout) |
| 3 | A2 mirror core | NEW sotc_arch/mirror.cpp (all of SotcArch::Mirror and the two C bridge functions) |
| 4 | A2 mirror hooks | src/runtime_memory.c (calls to the weak bridge where the guest maps/unmaps GPU-visible memory) |
| 5 | A2 drain into the mirror | BufferCache::NativeDrain (buffer_cache.cpp, that function only) |
| 6 | A3 recompiler: addresses | shader_recompiler/backend/spirv/spirv_emit_context.{h,cpp}: BdaAddress, LoadBdaBlock, the BDA block hoisting, the push-constant block definition; shader_recompiler/profile.h / runtime_info.h for one new flag |
| 7 | A3 recompiler: formatted buffers | shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp (buffer load/store emitters only) and the formatted-buffer emitters (grep LoadBufferFormat / StoreBufferFormat) |
| 8 | A3 CPU replica + bind skip | NEW sotc_arch/shader_addr.cpp; the SOTC_SHADER_ADDR branch in Rasterizer::BindBuffers (vk_rasterizer.cpp, that branch only) |
| 9 | A4 heap | NEW sotc_arch/heap.cpp; texture_cache/image_view.cpp (ImageView ctor/dtor: index acquire/release); the sampler cache's create/destroy (grep the sampler cache) |
| 10 | A4 table memo | NEW sotc_arch/table_memo.cpp |
| 11 | A4 recompiler: images via heap | shader_recompiler/backend/spirv/emit_spirv_image.cpp and the image/sampler resource definitions in spirv_emit_context.cpp (DefineImagesAndSamplers or its equivalent: NOT the functions specialist 6 owns) |
| 12 | A5 packet + one layout | NEW sotc_arch/packet.cpp; the pipeline layout creation in vk_graphics_pipeline.cpp and vk_compute_pipeline.cpp; Pipeline::BindResources (vk_pipeline_common.cpp) |
| 13 | A6 passes | NEW sotc_arch/passes.cpp; Rasterizer::BeginRendering / EndRendering and the barrier emission helpers in vk_rasterizer.cpp (not the layout-choice lines: specialist 2) |
| 14 | A6 parallel recording | vk_recorder.{h,cpp}, vk_parallel_record.{h,cpp} |
| 15 | Feature probes and fallbacks | vk_instance.{h,cpp} (the accessors above, their probes, enabling the extensions/features only when their switch is set) |

## 1. A0: op cost (SOTC_OPCOST)
Implement SotcArch::OpCost. Per thread and op kind: count, total ns, p50/p99 estimate (a small log2 histogram).
Time with one QueryPerformanceCounter / steady_clock read at the start and end of each op (skip all of it when off).
Report: `SOTC OPCOST draw: N ops/frame, X us/op (p50 Y, p99 Z) by kind ...; cp: ...`. This is the yardstick of
every later phase: make it cheap (<30 ns per op when on) and exact.

## 2. A1: unified image layouts (SOTC_LAYOUT_UNIFIED)
When on: every image is created/used in GENERAL; Image::Transit stops changing layouts (it keeps the memory
dependency the old transition carried: access masks and stages), descriptor and attachment layouts are GENERAL.
With Instance::SotcUnifiedLayouts() the extension's feature is used; without it GENERAL is still correct (say
where it costs). =2: compute both layouts at every decision and count where the old path would have transitioned
but the memory dependency differs (must be 0). Counters: transitions skipped, barriers kept.

## 3. A2: mirror core (SOTC_MIRROR)
Implement SotcArch::Mirror per the contract. Windows: find the guest GPU-visible ranges from the memory runtime's
layout (src/runtime_memory.c: where direct memory and its GPU mappings live; the heaps in user logs are around
0x1000000000-0x1240000000; read the code, do not hardcode a guess: expose the window bounds through one function
the runtime can call, or derive from the first OnMap calls). One sparse VkBuffer per window
(vk::BufferCreateFlagBits::eSparseBinding | eSparseResidency, usage storage + transfer dst + shader device address),
device-local memory in chunks bound per 64 KiB page with vkQueueBindSparse on Instance::SotcSparseQueueFamily(),
signalling a timeline semaphore; ReadyForSubmit makes the next graphics submit wait on it (find how the scheduler
adds wait semaphores). OnMap/OnUnmap may come from any thread: lock-free queue or a short mutex, batched binds.
Unmap releases after the frame fence. Upload records a buffer copy from a staging ring (reuse the scheduler's
staging/stream buffer pattern) into the mirror. Translate/Find are lock-free reads of a page table (one entry per
64 KiB page of each window). VRAM budget: count bound bytes; refuse (and count) above the device budget minus a
margin. Report: windows, bound MiB, binds/s, uploads MiB/s, refusals.

## 4. A2: mirror hooks (SOTC_MIRROR)
In src/runtime_memory.c, find every place the guest maps or unmaps memory the GPU can access (direct memory maps,
flexible maps with GPU protection bits, munmap/release of such ranges, protection changes that add/remove GPU
access). Call `bbgpu_mirror_on_map(addr, size)` / `bbgpu_mirror_on_unmap(addr, size)` there, declared
`__attribute__((weak))` like bbgpu_upload_wait_phys (runtime_memory.c ~739), called only when non-null and when
SOTC_MIRROR is set (read once, C). Never call it with the memory lock held if the GPU side could take a lock that
waits on the memory runtime (check what mirror.cpp will do: OnMap only queues). List every call site with file:line
and the guest API it serves (sceKernelMapDirectMemory, ...).

## 5. A2: drain into the mirror (SOTC_MIRROR)
In BufferCache::NativeDrain: when Mirror::On(), every announced range (Persistent and PerFrame alike, served or not)
is uploaded with Mirror::Upload in addition to today's handling (=1 keeps today's arena path too until phase A3 is
proven: the arenas are still what binds use). Merge adjacent ranges first (the drain already sorts and merges).
=2: additionally sample one uploaded range per drain and compare mirror bytes with the guest bytes after the GPU
copy finished (use the existing frame fence: compare ranges uploaded 3+ frames ago, never wait); count mismatches.

## 6. A3: recompiler, addresses (SOTC_SHADER_ADDR) - READ THE RECOMPILER ADDENDUM BELOW
In spirv_emit_context.cpp, when the profile flag `shader_addr` is set (add it to Shader::Profile in profile.h, set
by the pipeline cache when SotcArch::ShaderAddr::On(); it must be part of the shader cache key: find how other
profile flags reach the key and do the same), BdaAddress takes the block address and size from the buffer's V#
words in the flat buffer (the same words the classic path reads for that buffer: find where buffers' sharps sit in
the flat buffer, Info::RefreshFlatBuf) and the window bases from the push packet (SotcArch::DrawPacket fields
mirror_guest_base/mirror_device_base/arena_*), choosing the arena window when the address is inside it.
device address = window_device_base + (vsharp_base - window_guest_base). Size = what the classic descriptor would
cover (num_records * stride rules of AmdGpu::Buffer::GetSize, including stride 0 and swizzled cases: copy the
exact rule, do not simplify). Keep BdaLoad/BdaStore/atomics unchanged (they take BdaAddress's result).
The push-constant block: add the DrawPacket fields at an offset that does not collide with the existing push data
(read how push constants are laid out today, Shader::PushData, and document the offsets).

## 7. A3: recompiler, formatted buffers (SOTC_SHADER_ADDR) - READ THE RECOMPILER ADDENDUM BELOW
Find every buffer access that is NOT a plain dword load/store through BdaAddress today: formatted (typed) loads and
stores (texel buffers, data/number format conversion), and list them with file:line. For the ones whose conversion
the recompiler already emits in SPIR-V on raw dwords, route them through BdaAddress under the flag. For the ones that
need a texel-buffer descriptor (hardware format conversion), keep the descriptor and report how many shaders/ops of
the Lab route need it (the coordinator can count with a counter you add: a static per-pipeline flag
"needs_texel_descriptor" and its count in a report line).

## 8. A3: CPU replica and bind skip (SOTC_SHADER_ADDR)
Implement ShaderAddr::Expected (the exact formula of section 6, on the CPU, from Mirror::Windows and the arena
window: find the arena window bounds in sotc_guest_memory / pc_ring). In Rasterizer::BindBuffers add the branch:
when ShaderAddr::On() and the pipeline was built with shader_addr, buffers addressed by the shader are NOT bound
(no ObtainBuffer, no descriptor, no BDA table); only the remaining texel-descriptor buffers keep today's path.
=2: run today's path, and compare Expected(base,size) with the device address of the buffer ObtainBuffer returned
(buffer device address + offset) for ranges where both exist; count mismatches; count ranges Expected cannot answer
(not resident) - those are bugs in the mirror coverage and must be listed by address in the report line (first 8).

## 9. A4: heap (SOTC_HEAP)
Implement SotcArch::Heap. With Instance::SotcDescriptorHeap(): VK_EXT_descriptor_heap (read the extension's header
in C:/msys64/clang64/include/vulkan/vulkan_core.h: types, functions; implement exactly what it specifies). Without:
one descriptor-indexing set with large arrays of sampled images, storage images and samplers
(UPDATE_AFTER_BIND, PARTIALLY_BOUND), the classic bindless pattern (vk_bindless.cpp already does part of it:
reuse its pieces). Indices are permanent per ImageView / sampler, acquired in the ImageView constructor (and the
sampler cache) when On(), released in the destructor after the frame fence. Capacity from device limits; refusals
counted. Report: live views, live samplers, capacity, refusals.

## 10. A4: table memo (SOTC_HEAP)
Implement SotcArch::TableMemo. Key: hash of the table dwords + stage hash; open addressing, fixed capacity, LRU.
A hit returns the index table from this frame's ring (if the entry was made in an earlier frame, copy its indices
into this frame's ring: cheap, no resolution). A miss resolves each T#/S# through the texture cache exactly as
BindTexturesFull's first pass would (call the same functions; read that code; you may call but not edit it) and Heap
indices, writes them into the ring. Validity: the images' generations (texture cache MemoGeneration / image uid)
stored with the entry, checked on a hit. =2: resolve both ways and compare indices; count mismatches. The CP call
site (after the flat buffer refresh in the pipeline lookup) is a snippet.

## 11. A4: recompiler, images via heap (SOTC_HEAP) - READ THE RECOMPILER ADDENDUM BELOW
Under a profile flag `heap_images` (same rules as section 6's flag): images and samplers are not separate bindings;
the shader loads its heap index from the index table (push packet field index_table_address, entry i = the image's
position in the stage's image list, samplers after images: document the exact layout so specialist 10 writes the
same) and indexes the global arrays (descriptor indexing fallback) or the heap (descriptor_heap). Non-uniform
indices need NonUniform decorations. Keep every image instruction's semantics identical (gather, fetch, lod,
offsets, depth compare, storage formats): only the way the descriptor is obtained changes.

## 12. A5: packet and one layout (SOTC_PACKET)
Implement SotcArch::Packet. When On() (requires SOTC_SHADER_ADDR and SOTC_HEAP pipelines): pipelines use one
shared pipeline layout (the heap set or descriptor-heap binding + push constants including DrawPacket at the offset
specialist 6 documents; read their section and use the same offset: put it in one constexpr in packet.cpp and
report it). Pipeline::BindResources pushes the packet (flat buffer address: find where the CP's flat buffer for the
op is written and how its device address can be known; if it is not in GPU-visible memory today, say exactly what
must change) and nothing else. Pipelines not built with the flags keep today's layout (mixed frames must work).

## 13. A6: passes (SOTC_PASSES)
Implement SotcArch::Passes. When On(): the barriers that today are emitted per binding are replaced by one global
memory barrier at pass boundaries where a later op reads what an earlier op of the frame wrote (written ranges come
from the shaders' storage-buffer/storage-image writes and render targets). Start conservative: one full
memory barrier between consecutive ops when the earlier op wrote anything the later could read (by range overlap,
or "unknown" = barrier). Count barriers emitted vs today's. =2: emit today's barriers and count where the pass rule
would have omitted one that today's code needed (the comparison is on the dependency, not the API call).

## 14. A6: parallel recording (SOTC_PASS_RECORD)
Read vk_recorder.cpp and vk_parallel_record.cpp (SOTC_N5_PARALLEL / SOTC_N5_PARALLEL_TRANSLATE census: why the
measured ceiling was 1.15x). With the packet path (SOTC_PACKET) an op's recording has no shared cache state. Build
recording of consecutive packet ops on worker threads into secondary command buffers (or per-worker primaries
stitched in order), keeping submission order and the existing synchronization. Ops not on the packet path stay on
the draw thread. Counters: ops recorded on workers, ms per frame per worker, stitch cost.

## 15. Feature probes and fallbacks (switch: none of its own; it enables what the other switches need)
In vk_instance.{h,cpp} implement the accessors listed above: probe the extension and its feature struct
(unifiedImageLayouts, descriptor heap, sparse binding/residency for buffers + a queue family with sparse binding,
device generated commands, buffer device address), enable each at device creation ONLY when the switch that needs
it is set (SOTC_LAYOUT_UNIFIED, SOTC_HEAP, SOTC_MIRROR, SOTC_PACKET), log one line per feature (present/used/
missing), and return false otherwise. Unset switches = the device is created exactly as today. Also write the
vendor matrix (NVIDIA, AMD, Intel Arc: which of these exist on current Windows drivers, with sources you can cite
from the header comments or say "unknown") into your report.

## RECOMPILER ADDENDUM (specialists 6, 7 and 11): be cautious
The shader recompiler translates every GCN shader of the game; one wrong instruction corrupts rendering everywhere
and invalidates the shader cache. Rules on top of the shared ones:
- Change NOTHING on the path without your profile flag. Every edit is inside `if (profile.<flag>)` (or the flag's
  equivalent in the emit context). Show, for each edited function, the exact unchanged branch.
- Do not refactor, rename, reorder or "clean up" existing emitter code. Add new code next to it.
- Read the whole function you change and every caller of it before editing. Read how the existing BDA path
  (SOTC_BDA: BdaLoad, BdaStore, BdaAddress, the guarded atomics in emit_spirv_atomic.cpp) is built and follow it
  exactly: same types, same capability handling, same alignment operands, same out-of-range rule (loads return
  zero, stores and atomics are skipped).
- SPIR-V validity: every new Id has the right type; pointer types for PhysicalStorageBuffer need Aligned memory
  operands; OpPhi incoming blocks must be the real predecessors; no new capability without checking it is declared
  (emit_spirv.cpp). If unsure about a SPIR-V rule, say so in the report instead of guessing.
- Integer widths: guest addresses are 64-bit; V# fields are 32-bit words; do every address computation in u64 and
  every size in u32/u64 exactly as the CPU code does (AmdGpu::Buffer).
- Keep the shader cache key honest: a new flag must change the key (else old cached SPIR-V is reused with the new
  layout).
- Write a self-check you can run without the game: a small standalone harness or, at minimum, a table in your
  report that walks 3 concrete V# examples (stride 0, stride 16 with num_records, a swizzled/indexed case) through
  the CPU formula and the SPIR-V you emit, instruction by instruction.
- If anything in your section cannot be done safely, stop at the safe part and report exactly what is left.
