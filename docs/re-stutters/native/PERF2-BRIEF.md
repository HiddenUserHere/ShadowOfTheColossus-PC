# Performance brief 2 (2 Nov): command processor and draw thread, from the profile

You are a specialist of the SotC PC port. Read, in this order, before touching anything:
1. CLAUDE.md at the repo root.
2. This whole file (the shared part and YOUR section; skim the other sections so you know who owns what).
3. Every function named in your section, in full, before you design anything.

## Measured facts (native mode on the Lab route, profiler SOTC_PROFILE, 2 Nov)
Two threads limit the frame together. Budget at 30 FPS: 33.3 ms.
- Command processor thread `shadPS4:GpuCommandProcessor` ("command parse" ~20-23 ms/frame):
  inclusive ~31% in pipeline lookup: Rasterizer::LookupGraphicsPipeline (16%) and LookupComputePipeline (15%),
  inside them PipelineCache::RefreshGraphicsKey / RefreshGraphicsStages (14.6%) and PipelineCache::GetProgram (15%):
  per draw it rebuilds the key and compares the stage specialization (Shader::StageSpecialization::operator==,
  ForEachSharp, SharpFetch::Fetch, SearchBinaryInfoCached). ~47% waiting: Liverpool::WaitBackWritesSlow (17%),
  yields and condition-variable waits.
- Draw thread `shadPS4:GpuDraw` ("draw busy" ~31-32 ms/frame): Rasterizer::BindResources 38% inclusive
  (BindBuffers 16%, BindTextures 15.5%, BufferCache::ObtainBuffer 10.8%), TexBlockKey::TexBlockKey 3.7% self,
  ucrtbase (memcpy/alloc) 10%, waits on Presenter::GetRenderFrame 8.5% (the 30 FPS cap: not a target).
- The existing pipeline memo SOTC_PIPE_MEMO=2 shows 0 mismatches but hit rates of only 40-47% (stages) and 62-64%
  (compute) with ~100k stage evictions per 5 s; turning it on (=1) changed nothing measurable.
- Already tried and NOT helpful (do not redo): SOTC_N5_BINDLESS (correct, but slower: disables the fast-bind memo);
  the announce drain is only 0.84 ms/frame.
Profile logs: C:/Users/ptr/Documents/sotc-pc/user/pcmem/profile-draw.txt (command processor) and
user/pcmem/profile-gpudraw.txt (draw thread); resolve with
`python tools/profile_functions.py <file> out/gpu/bb-probe.exe 50 --inlines --qualified` from the main tree.

## Rules (all specialists)
- Worktree base: `git checkout --detach b0ba7e2`, then
  `git apply C:/Users/ptr/Documents/sotc-pc/docs/re-stutters/native/data/perf2-base.diff` (current uncommitted work).
- Do NOT commit. Do NOT run the game. Leave changes uncommitted.
- Edit ONLY the functions/files your section lists as yours. Anything else: put the exact snippet in your report.
- Every new behaviour is behind ONE new switch named in your section, read once (static const lambda +
  std::getenv, like the surrounding code). Unset = today's behaviour, bit for bit; prove it by pointing at the
  branches. Value 2 = verify mode: run the new path AND the old path, use the OLD result, count every difference
  (a counter that must read 0 in a run). Value 1 = the new path only.
- Every change gets counters in an existing 5 s report line (hits/misses/ns saved), so one run proves it.
- Correctness first: on any doubt, take the old path and count it. Never assume guest memory did not change.
- Syntax check every touched .cpp: from the worktree, `source msys2-env.sh` (if refused, call
  C:/msys64/clang64/bin/clang++.exe directly), then `clang++ -fsyntax-only -std=gnu++23 <flags of the matching entry
  of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json, with the worktree's gpu/shim and gpu/shadps4 -I
  paths FIRST, third-party paths left in the main tree> <file>`. Paste the command and its output. Never claim a check
  you did not run.
- Style: follow the surrounding code; English comments; `sotc:` marker on every change in gpu/shadps4/...; no dead
  code, no leftover debug output.
- Final report in your last message: design (with file:line evidence), every change (file:function), checks run and
  output, exactly what a run with your switch =2 and =1 must show, risks, unknowns.

## Ownership map (who edits what; everything else is snippets)
| Section | Owns |
|---|---|
| P1 pipeline dirty fast path | vk_pipeline_cache.cpp: GetGraphicsPipelineLookup, RefreshGraphicsKey, RefreshGraphicsKeyRegs, RefreshGraphicsKeyRegsDirty, GetComputePipelineLookup, RefreshComputeKey; vk_pipeline_cache.h members for these |
| P2 specialization memo | vk_pipeline_cache.cpp: GetProgram and RefreshGraphicsStages (its bind_stage lambda); shader_recompiler/specialization.h; vk_pipeline_cache.h members for these |
| P3 precise CP waits | liverpool.cpp: BackPublish, WaitBackWrites, WaitBackWritesSlow, SubmitDraw, SubmitDrawIndirect, SubmitDispatch; liverpool.h members for these. The packet handlers belong to P4: P3 gives them as snippets (one call per handler that passes the write range) |
| P4 CPU-path writes on the CP | liverpool.cpp: the packet handler bodies of WRITE_DATA, DMA_DATA, COPY_DATA, EVENT_WRITE_EOP/EOS, RELEASE_MEM (the parts that decide whether the write goes to the draw thread); liverpool.h members for these |
| P5 futex waits | liverpool.cpp: WaitBackTail, the full-ring wait loop in BackAlloc, the idle wait in BackLoop, every drain_cv / back_cv notify; vk_recorder.cpp: producer/consumer waits |
| P6 descriptor buffer | NEW vk_descriptor_buffer.{h,cpp}; vk_pipeline_common.cpp (Pipeline::BindResources); vk_instance.cpp (feature probe lines); layout creation lines in vk_graphics_pipeline.cpp / vk_compute_pipeline.cpp |
| P7 bind resolution memo | vk_rasterizer.cpp: TexBlockKey, TryTextureBlock and its helpers; buffer_cache.cpp: the ObtainBuffer fast path only |

## P1. Pipeline dirty fast path (switch SOTC_PIPE_FAST, command processor)
Goal: when nothing that feeds a pipeline changed since the previous draw (or dispatch), return the previous
pipeline without rebuilding the key and without visiting the stages.
1. Read GetGraphicsPipelineLookup (vk_pipeline_cache.cpp ~1619) and RefreshGraphicsKey (~2131) completely, and
   SOTC_PIPE_DIRTY (~1938: the register part of the key rebuilt only for dirty registers) and the existing key memo
   (last_graphics_key / last_graphics_pipeline, PipeMemoKeyOn).
2. List EVERY input of the result: the registers the key reads (RefreshGraphicsKeyRegs), the registers and guest
   memory the stages read (program addresses, user data SGPRs, the sharps they point to, the fetch shader), the
   draw_indirect_params, async translation state (async_done_count, translation_pending), the pipeline map size
   (erasures), the profile. Write the list with file:line in the report.
3. Get a per-draw "inputs changed" signal that is cheaper than recomputing: register writes are tracked by
   Liverpool::MarkRegs / WriteRegsMarked (liverpool.h ~772; read how SOTC_PIPE_DIRTY consumes them); guest memory
   read by the stages: you may NOT assume it is unchanged. Use a content check that is cheaper than today's work:
   e.g. keep, for the previous draw, the exact dwords of every sharp / user-data word the stages read (the list
   SharpFetch produces) and compare them with one memcmp each; or get them from P2's memo if P2's switch is on (do
   not depend on it: work standalone).
4. Implement: if the registers feeding the key and the stages are not dirty AND the dwords compare equal AND no
   async install/erase happened AND the params are equal, reuse the previous pipeline (and the previous stage state:
   infos, modules, fetch_shader, bindings, exactly as the slow path would have left them). Same for compute
   (GetComputePipelineLookup / RefreshComputeKey).
5. =2: always run the slow path too and compare pipeline pointer + stage state; count mismatches.
6. Counters (append to the "SOTC pipe memo" line or a new "SOTC pipe fast" line): fast hits, misses by reason
   (regs dirty, sharp changed, async, params), ns spent in lookup per second.

## P2. Stage specialization memo (switch SOTC_SPEC_MEMO, command processor)
Goal: make GetProgram's "find the permutation for these sharps" O(1) for repeated stages, with a much higher hit
rate than SOTC_PIPE_MEMO's stage memo (40-47%, many evictions).
1. Read PipelineCache::GetProgram (~2396), RefreshGraphicsStages (~2192), the existing stage memo (PipeMemoStageMode,
   PipeMemoStats), and shader_recompiler/specialization.h (StageSpecialization, ForEachSharp, operator==).
2. Find out WHY the existing memo misses and evicts (its key, its capacity, what it compares). Write it down with
   evidence.
3. Design: key = (program identity: code address + code hash/generation as GetProgram uses it, hw/sw stage, the
   binding offsets) + the specialization-relevant bits only (what operator== compares, extracted once per sharp, not
   the whole sharp), stored in a hash map with enough capacity for the scene (SotC uses a few thousand stage
   variants; size it from the counters, no unbounded growth: LRU with a clear cap).
4. Make the fingerprint cheap: compute it while SharpFetch reads the sharps (one pass), not in a second pass.
5. =2: compute the old answer too, count mismatches. Counters: lookups, hits, misses, evictions, ns per second.

## P3. Precise command-processor waits (switch SOTC_CP_PRECISE_WAIT, command processor)
Today, before every draw/dispatch, Liverpool::WaitBackWrites (liverpool.h ~766) waits until the draw thread has
executed EVERY earlier op that writes memory (back_write_seq, set in BackPublish when writes_memory), because the
command processor's pipeline lookup and indirect-argument reads look at guest memory those ops may change. This
costs ~17% of the command processor.
1. Read BackPublish (~939), WaitBackWrites/WaitBackWritesSlow (~1001), SubmitDraw (~1454), SubmitDrawIndirect
   (~1481), SubmitDispatch (~1588) and every packet handler that publishes with writes_memory=true (grep
   BackPublish( and follow the callers: DMA_DATA, WRITE_DATA, EVENT_WRITE_EOP/EOS with data, RELEASE_MEM, COPY_DATA,
   DUMP_CONST_RAM, ...).
2. For each memory-writing op, record at publish time the guest range(s) it will write (from the packet: destination
   address and byte count). If the range is unknown, record "unknown" (= everything).
3. For each draw/dispatch, determine the guest ranges the COMMAND PROCESSOR reads before handing the op over: the
   user-data tables and sharps the pipeline lookup reads (P1/P2 read the same: SharpFetch), indirect argument and
   count addresses, anything else (grep reads of guest memory in the Submit* paths and the lookup).
4. Replace the "wait for all earlier writes" by "wait only for the earlier pending writes whose ranges overlap what
   this op reads (or are unknown)". Keep a small ring of pending (seq, range) entries; entries older than back_tail
   are dropped.
5. =2: decide with the precise rule but WAIT the old way; afterwards check (once the overlapping ops ran) that no
   op the precise rule skipped wrote into the read ranges: count violations (must be 0).
6. Counters: waits avoided, waits taken (by reason: overlap / unknown range), ns blocked per second.

## P4. CPU-path memory writes run on the command processor (switch SOTC_CP_DIRECT_WRITES)
Many memory-writing ops are handed to the draw thread only so they run in order with the GPU work, although they
are plain CPU stores into guest memory (WRITE_DATA to memory, DMA_DATA with a CPU path, EOP/EOS/RELEASE_MEM data
writes that only the CPU reads, small fills). Each one sets back_write_seq and makes the next draw wait (P3).
1. Grep every BackPublish(..., writes_memory=true ...) caller and the op it queues. For each, write down: what the op
   does on the draw thread, whether it touches Vulkan (a buffer-cache upload, a GPU copy, an image invalidation, a
   fence/label) or only guest memory, and whether its ORDER against earlier queued GPU work matters (does the write
   depend on a GPU result? e.g. an EOP write after GPU work must wait for that work: it must stay on the draw thread /
   GPU timeline).
2. For the ops that are pure CPU stores of packet-supplied data with no dependency on earlier GPU results, execute
   them on the command processor immediately (same function the draw thread calls today), and publish the op with
   writes_memory=false (or not at all, if nothing remains for the draw thread). If the op must also tell the buffer
   cache that guest memory changed (in native mode: SotcNative announces / InvalidateMemory), keep that notice
   ordered the same way as today (read how the draw thread does it; if it must stay on the draw thread, queue only
   that notice, writes_memory=false).
3. Any op whose correctness you cannot prove this way stays as today; list it with the reason.
4. =2 is not meaningful (the write happens once); instead count per op type: executed on CP, kept on draw thread (by
   reason). Add the counts to an existing 5 s line.

## P5. Futex-style waits between the GPU threads (switch SOTC_FUTEX_WAIT)
Today the waits spin 4096 times then sleep on a condition variable with a 1 ms timeout (WaitBackTail ~857, the
full-ring loop in BackAlloc, BackLoop's idle wait), so a missed or late notify costs up to 1 ms, and the spins burn
CPU the other thread needs.
1. Read every wait and every notify on back_head / back_tail / drain_cv / back_cv (and the same pattern in
   vk_recorder.cpp between the draw thread and the recorder thread). List them with file:line, who waits for what.
2. Replace them, behind the switch, with WaitOnAddress / WakeByAddressSingle|All (Windows, Synchronization.lib; on
   Linux keep the old path: the switch is Windows-only, say so) on the very atomics that carry the condition
   (back_tail, back_head, ring indices): the waiter re-checks the condition in a loop, the writer wakes after it
   publishes. Keep a short bounded spin (measure: ~100-200 iterations of CpuRelax) before sleeping.
3. Make sure no wake can be lost (the waiter compares the current value; the publisher stores then wakes), and that
   shutdown (stop_token) still wakes every waiter.
4. =2 is not meaningful here; instead count: waits, spins resolved, sleeps, average and max sleep time, wakes sent.

## P6. Descriptor buffers (switch SOTC_N5_DESCBUF, draw thread, Vulkan VK_EXT_descriptor_buffer)
Goal: binding a draw's resources costs memcpy of descriptor bytes, not vkUpdateDescriptorSets / push descriptors,
WITHOUT changing the shaders (the layout and binding numbers stay identical, so the fast-bind memo and the shader
cache keep working).
1. Read Pipeline::BindResources (vk_pipeline_common.cpp, the set_writes / push descriptor path), how set_writes are
   produced in vk_rasterizer.cpp (BindBuffers, BindTexturesFull, BindSamplers: read only, not yours), the pipeline
   layout creation (vk_graphics_pipeline.cpp / vk_compute_pipeline.cpp), and how the recorder records the descriptor
   push (vk_recorder: closures).
2. Probe VK_EXT_descriptor_buffer (descriptorBuffer feature, descriptorBufferPushDescriptors if push descriptors are
   used today) in vk_instance.cpp; fall back (switch has no effect, one log line) when missing. Target drivers:
   NVIDIA, AMD, Intel Arc current Windows drivers support it.
3. Create the set layouts with VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT and the pipelines with
   VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT when on (the pipeline cache key must include the switch).
4. A per-frame descriptor ring in host-visible (ReBAR if available, else host-cached) memory with
   SAMPLER/RESOURCE_DESCRIPTOR_BUFFER usage, sized from the counters, recycled by frame fence (no GPU wait beyond the
   existing frame pacing).
5. vkGetDescriptorEXT bytes for each image view / sampler: cache them on the view/sampler object at creation (they
   never change); for buffers (address + range) compute per bind (cheap) or cache per (buffer, offset, size).
6. Per draw: write the set's descriptors at the layout's binding offsets (vkGetDescriptorSetLayoutBindingOffsetEXT,
   queried once per layout) into the ring with memcpy, then vkCmdSetDescriptorBufferOffsetsEXT (bind the buffers once
   per command buffer with vkCmdBindDescriptorBuffersEXT). Skip unchanged sets (same bytes as the previous draw).
7. =2: also build the classic descriptor writes and compare the resources they reference with the bytes' source
   objects (count mismatches); execute the classic path.
8. Counters: draws bound via descriptor buffer, bytes written per frame, sets skipped as unchanged, fallbacks.

## P7. Bind resolution memo (switch SOTC_BIND_RESOLVE_MEMO, draw thread)
Goal: cut BindTextures / TexBlockKey and ObtainBuffer cost for draws that rebind the same tables.
1. Read TexBlockKey and TryTextureBlock (vk_rasterizer.cpp; the texture block memo, ~6107 and the key type), the
   texture cache memo (TextureCache::MemoLookup, MemoGeneration), and BufferCache::ObtainBuffer's existing fast paths
   (bind memo, SOTC_GEN_BIND, sync fast).
2. Measure by reading why TexBlockKey construction costs 3.7% self: what it copies/hashes per draw. Make the key
   cheaper (e.g. hash incrementally from the T# dwords already loaded, avoid copying whole structs, avoid
   re-reading guest memory twice).
3. ObtainBuffer: add a per-draw-thread memo keyed by (address, size, is_written, buffer-cache bind generation, the
   page generation of the range) that returns the previous (buffer, offset) when nothing in the range or the cache
   changed; any doubt = slow path.
4. =2: run both, count mismatches. Counters: hits, misses by reason, ns saved per second.
