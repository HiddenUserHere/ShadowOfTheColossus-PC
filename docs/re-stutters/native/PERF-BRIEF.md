# Performance brief (2 Nov): get the draw thread well under 33 ms in native mode

You are a specialist of the SotC PC port. Read, in this order, before touching anything:
1. CLAUDE.md at the repo root (build, conventions, architecture).
2. This whole file.
3. The files of your own section below.

## The problem (measured, user/native-visual16.log of the main tree)
In native mode (`SOTC_NATIVE=1 SOTC_NATIVE_MASKS=prev SOTC_NATIVE_PARTICLES=prev`, plus the default
`SOTC_PC_RING=8` that sotc.py sets), running in the open field:
- "SOTC threads per frame: draw busy 33.99 ms, command parse 24.03 ms" (budget at 30 FPS: 33.3 ms).
- "late 25..52 of ~140" frames per 5 s window. Every late frame is shown twice: a video of the user shows one
  repeated frame every 3 frames ("the animation has fewer frames").
- "SOTC NATIVE TIMING drains 2369 entries 1033750 2936550 KiB" per 5 s: ~1 million announce entries per 5 s
  are processed ON THE DRAW THREAD by BufferCache::NativeDrain (each marks pages CPU-modified and asks the texture
  cache to invalidate), ~6900 per frame. Most come from guest memcpy/memset/memmove (src/runtime.c announces every
  guest copy, "gmem"), chunk announces ("chunk"), zero-copy buffers ("zc"), file reads.
- `SOTC_PC_RING=8` (default) already imports the per-frame arena (Default TaggedArena kind-2 blocks, 32-64 MiB) so
  the GPU reads it in place (BufferCache::PcHostBind, gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp
  HostMaintain). Announces inside those imported blocks are still queued and drained: wasted work.
The goal of this wave: cut the draw thread's per-frame CPU so frames stop missing the 33.3 ms slot, with the
modern techniques the user chose: (1) zero-copy shared memory for per-frame data, (2) bindless resources and
buffer device addresses, (3) reuse of recorded command buffers, (4) parallel translation (behind a flag).

## Rules (all specialists)
- Worktree base: `git checkout --detach b0ba7e2` (the branch tip). Nothing else to apply.
- Do NOT commit. Do NOT run the game (the coordinator runs it, one instance at a time). Leave your changes
  uncommitted in your worktree.
- Only edit the files listed under "You own". If you need a change in another file, put the exact snippet in your
  final report instead (the coordinator applies it). Exception: new files you create.
- Syntax-check every C++ file you touch:
  `source msys2-env.sh` then
  `clang++ -fsyntax-only -std=gnu++23 <the -I/-D flags of the matching entry in C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json> <file>`.
  The compile database has entries for gpu/shadps4/... and gpu/sotc/...; if your file has none, use the flags of
  gpu/sotc/hooks.cpp. Third-party include paths stay pointing into C:/Users/ptr/Documents/sotc-pc (the main tree).
  Report the exact command and its result. If a check cannot run, say so; never claim a check you did not run.
- Hooks (only if you add one): scripts/hooks.d/*.py, pattern unique (`python tools/re/eboot_re.py sigfind`, run
  from C:/Users/ptr/Documents/sotc-pc), displaced bytes without RIP-relative or rel32 operands and not a branch
  target, and NEVER in a function that already has another hook or CPU patch (the detour engine silently refuses
  the second one: grep scripts/hooks.d and gpu/sotc for the function's address range first). Run
  `python tools/re/hooks_install_check.py --eboot C:/Users/ptr/Documents/sotc-pc/out/eboot.elf`.
- Code style: follow the surrounding code. English comments, `sotc:` marker on every change in vendored files
  (gpu/shadps4/...). No dead code, no leftover debug prints (counters go in the existing 5 s report lines). Every
  new switch is read once (static const lambda with std::getenv, as the surrounding code does).
- Correctness first: a faster path that can hand the GPU different bytes or a different resource than today is a
  bug. When unsure, fall back to today's path and count it.
- Every change needs a counter in a 5 s report line that proves it ran and what it saved (calls, bytes, ms).
- Final report in your last message (no file): what you changed and why (file:line), the exact checks you ran and
  their output, what a game run must show (the report lines and expected values), risks, unknowns.

## Where things are (verified 2 Nov)
- Announce API: gpu/sotc/native/native.{h,cpp} (SotcNative::Announce, the pending queue g_pending, overflow,
  TakeUpload), native_draw.h; drain: BufferCache::NativeDrain in gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp
  (called from PcRingCopy::OnSubmitStart in pc_ring_copy.cpp at every submit start, draw thread). Kinds: Persistent
  (uploaded via DrainRange), PerFrame (MarkRegionAsCpuModified + texture_cache.InvalidateMemory(.., keep_gpu_written)),
  over GPU-modified memory NativeExactUpload. Announce sources: gpu/sotc/native/native_hooks.cpp (hooks, chunk list
  FlushChunks, re-announce of rewritten blocks AnnounceTracked/RecheckTracked), src/runtime.c (guest copies),
  src/runtime_file.c (file reads).
- Ring 8 / host import: gpu/sotc/pc_ring.{h,cpp} (modes, block registry, OnBlock), pc_ring_copy.cpp (HostMaintain,
  the imported set "applied"), BufferCache::PcHostApply / PcHostBind (buffer_cache.cpp/.h), the import itself
  SotcGuestMemory::ImportGuestRange (gpu/shadps4/video_core/renderer_vulkan/sotc_guest_memory.cpp). Heaps and their
  allocators: docs/re-stutters/native/heaps.md, allocators.md, framedata.md; per-frame ring depths in
  gpu/sotc/native/native_draw.h (RingDepth). Allocation probes: gpu/sotc/probe_alloc.cpp, probe_memtype.cpp.
- Bindless: SOTC_N5_BINDLESS (images; gpu/shadps4/video_core/renderer_vulkan/vk_bindless.{h,cpp},
  shader_recompiler/backend/spirv/spirv_emit_context.cpp, emit_spirv_image.cpp, vk_instance.cpp feature probe),
  reports docs/re-stutters/native/impl-n5-bindless.md and impl-n5-bindless2.md (written, syntax-checked, NEVER run
  in the game). SOTC_BINDLESS_SAMP (samplers, older, used). SOTC_BDA: vk_bda_table.{h,cpp}.
- Recording: the draw thread translates PM4 ops into closures (CommandRecorder, vk_recorder.{h,cpp}); a recorder
  thread replays them into Vulkan command buffers; SOTC_PASS_SEC records a dynamic-rendering pass on one worker
  into a secondary (vk_parallel_record.{h,cpp}); SOTC_N5_PARALLEL / SOTC_N5_GRAPH are measurement modes; reports
  docs/re-stutters/native/impl-n5-parallel.md, impl-n5-graph.md. Measured 1 Nov: the recorder (~9.5 ms/frame) is
  not the bottleneck; the draw thread (~34 ms) is: per-op binding (buffer lookup, sync, descriptor writes),
  command parsing (~24 ms "command parse").
- Per-op costs and existing memos: vk_rasterizer.cpp (BindBuffers, BindTextures, bind cache SOTC_BIND_CACHE,
  RsMemo, pipeline key "pipe dirty", flat buffer), liverpool.cpp (command processor: ProcessGraphics,
  ExecuteBackOp), sotc_stats.h (switches and counters).

## Sections (one specialist each)

### 1a. Announce fast path and the cost of the drain (zero-copy part 1)
You own: gpu/sotc/native/native.{h,cpp}, gpu/sotc/native/native_draw.h, the NativeDrain function in
buffer_cache.cpp (and only that function plus its declaration in buffer_cache.h), and a NEW lock-free query in
gpu/sotc/pc_ring.{h,cpp}: `bool SotcPcRing::HostServed(uint64_t addr, uint64_t size)` = true when [addr, addr+size)
lies entirely in a block that ring 8 imported and serves in place (keep a lock-free snapshot, e.g. a small sorted
array published with an atomic pointer/generation, updated where the imported set changes in pc_ring_copy.cpp:
put that one-line call as a snippet in your report if pc_ring_copy.cpp is not yours to edit... it is yours for
that one call only).
Do:
1. Add a draw-thread timer around NativeDrain (ms per frame, entries, pages marked, texture invalidations) to the
   "SOTC NATIVE TIMING" line. This is the number that matters.
2. In SotcNative::Announce: drop (count, do not queue) PerFrame announces that HostServed() says are served in
   place: the GPU reads guest memory there, nothing to mark or upload. Persistent announces in served blocks are
   dropped too (same reason). Think about the exact-upload path (GPU-modified ranges inside a served block: ring 8
   never GPU-tracks them, so nothing to do) and say so with evidence.
3. Coalesce: merge a new PerFrame announce with the newest pending entry of the same kind when they overlap or touch
   (the queue already merges on overflow: make merging the normal path, bounded so one entry never exceeds 64 MiB),
   and dedupe pages already marked in this submit (pc_host_marked exists for ring 8: reuse the idea).
4. In NativeDrain: skip texture_cache.InvalidateMemory when no image is registered in the range (the texture cache
   already has a fast empty-page check; make sure it is used without taking its mutex), and batch
   MarkRegionAsCpuModified over merged ranges.
Expected run result: drain ms/frame and entries/s drop by an order of magnitude; "late" frames drop.

### 1b. Import every per-frame heap (zero-copy part 2)
You own: gpu/sotc/pc_ring.cpp (except the HostServed query of 1a: do not touch it), gpu/shadps4/video_core/buffer_cache/pc_ring_copy.cpp
(except the one HostServed snapshot call), gpu/sotc/probe_alloc.cpp, gpu/sotc/probe_memtype.cpp.
Ring 8 imports only the kind-2 TaggedArena blocks. Map, from docs/re-stutters/native/heaps.md, allocators.md,
framedata.md and the decomp, every OTHER heap whose blocks the CPU rewrites every frame and the GPU reads: the
memtype-17 blocks of RenderSingleFrameCPU (NoteFrameBlock), the 0x12820 allocator categories DAT_02ed1f50 /
DAT_02ed1f54 (zero-copy backings and bind blocks, rewritten per frame: see the NATIVE_ZC_CREATE / NATIVE_UBUF_* /
NATIVE_SUNSHADOW_* comments in gpu/sotc/native/native_hooks.cpp), the chunk allocator 0x8c0d0's backing heap, the
GameToGPU / ParticleDoubleFrame rings. For each: block discovery (which allocator probe sees the blocks, their
lifetime, ring depth), whether importing it is safe (GPU writes into it? read-back by the CPU? freed and reused?),
and implement importing the safe ones the same way ring 8 imports the arena (same HostMaintain/PcHostApply
machinery, same flip-wait/reuse-fence rules: native_draw.h RingDepth and the RING8 flip wait), so binds there are
served in place and 1a's HostServed() drops their announces. Count imported blocks/bytes and served binds per heap
in the "SOTC PC RING host" line. RAM: importing must not copy or allocate (the user does not accept more RAM/VRAM).

### 2a. Bindless images: make SOTC_N5_BINDLESS correct and default-ready
You own: gpu/shadps4/video_core/renderer_vulkan/vk_bindless.{h,cpp}, gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_image.cpp,
the image parts of spirv_emit_context.cpp (TextureDefinition / DefineImagesAndSamplers / LoadImage only), the image
bind path of vk_rasterizer.cpp (BindTexturesFull and the bindless heap branch only), vk_instance.cpp (feature probe
lines only).
The code exists (impl-n5-bindless.md, impl-n5-bindless2.md) but was never built into a game run. Do a full
correctness review against the reports and the code: SPIR-V validity (types, capabilities, NonUniform decoration
on the index when the slot can differ per invocation, OpAccessChain into runtime arrays, update-after-bind flags),
host/shader binding numbering equality, slot lifetime (a view freed while a command buffer that uses its slot is in
flight), layout per slot, descriptor heap capacity and eviction, push-constant size on NVIDIA/AMD/Intel Arc
(maxPushConstantsSize), and the pipeline cache key (a pipeline built with bindless must not be reused without it:
the shader cache must be invalidated or keyed). Fix every defect. Add a verify mode (SOTC_N5_BINDLESS=2) that
also writes the classic descriptors and counts slot/view mismatches. Add the per-frame cost counters (descriptor
writes avoided per frame). Report exactly what a run needs to show to make it the default in native mode.

### 2b. Buffers through device addresses (no per-draw buffer descriptors)
You own: gpu/shadps4/video_core/renderer_vulkan/vk_bda_table.{h,cpp}, the buffer parts of the SPIR-V backend
(emit_spirv_*.cpp buffer load/store, the buffer parts of spirv_emit_context.cpp: DefineBuffers and the buffer
definitions only), the buffer bind path of vk_rasterizer.cpp (BindBuffers and the buffer descriptor writes only).
Read what SOTC_BDA does today (vk_bda_table.cpp, its users) and design + implement the step that removes per-draw
buffer descriptor writes: shaders read a V#'s guest buffer through a 64-bit device address (base address of the
arena buffer holding it + offset, from a GPU-visible table or push constants indexed by the buffer slot), with
bounds handled exactly like the descriptor path (the V# num_records / stride clamp: robustness must stay
identical, out-of-bounds reads return 0). Behind SOTC_N5_BDA_BUFFERS=1 (new switch, default off) and =2 verify.
Requires bufferDeviceAddress (core in 1.2): probe it, fall back when missing. Count descriptor writes avoided per
frame. The pipeline cache key must include the mode. Coordinate by contract with 2a: you do not touch image code,
2a does not touch buffer code; spirv_emit_context.cpp is shared, keep your edits inside buffer functions.

### 3. Reuse of recorded command buffers (SOTC_N5_REPLAY, default off)
You own: NEW files gpu/shadps4/video_core/renderer_vulkan/vk_replay.{h,cpp}, plus minimal call sites (list them
as snippets in your report for liverpool.cpp / vk_rasterizer.cpp / vk_recorder.cpp; do not edit those files).
SotC submits nearly the same DCBs every frame. Design and implement a memo that skips the translation of a run of
PM4 ops when it is provably identical to a previous frame's run: key = hash of the PM4 dwords of the run AND of
every input the translation reads (register state at the start of the run, the guest bytes of every user-data /
V# / T# table it dereferences, the identity and version of every buffer and image it binds: buffer cache arena +
offset + generation, texture cache image id + version, pipeline). On a hit, replay the recorded closures (the
CommandRecorder stream) instead of re-translating; on any doubt, miss. SOTC_N5_REPLAY=2: translate anyway and
compare the produced closure stream with the memo (count mismatches; must be 0 before =1 can be trusted). Report
the hit rate you expect, where the key inputs come from (file:line), and every input you could not cover.

### 4. Parallel translation (SOTC_N5_PARALLEL_TRANSLATE, default off; the user wants it behind a flag)
You own: gpu/shadps4/video_core/renderer_vulkan/vk_parallel_record.{h,cpp}, vk_recorder.{h,cpp} (only new code
paths behind the flag), new files if needed; snippets for liverpool.cpp.
Today one draw thread translates the whole frame (24 ms "command parse" + binding). Design and implement
translating independent parts of the frame on several worker threads behind the new flag: the natural unit is a
render pass / compute group between barriers (impl-n5-graph.md has the pass and hazard model, impl-n5-parallel.md
the state map). Whatever runs on a worker must not touch the shared caches unsafely (buffer cache, texture cache,
page tracker are single-threaded today: either take their locks, or resolve every lookup on the draw thread first
and let workers only record). Keep submission order identical. With the flag off, nothing changes (prove it). Add
a verify mode (=2) that translates on the workers but executes the serial result and compares. Report the expected
speedup and its limits.
