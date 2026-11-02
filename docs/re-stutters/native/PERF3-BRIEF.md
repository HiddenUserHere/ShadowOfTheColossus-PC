# Performance brief 3 (2 Nov): the draw thread, wave 1

You are a specialist of the SotC PC port. Read, in this order, before touching anything:
1. CLAUDE.md at the repo root.
2. This whole file (the shared part, YOUR section, and the ownership map).
3. Every function named in your section, in full, before you design anything.

## Where we are
Native mode (SOTC_NATIVE=1) renders correctly. The command processor got lighter in brief 2 (PERF2-BRIEF.md,
commit 27cfa5a). The frame is now limited by the draw thread `shadPS4:GpuDraw`: ~25 ms of real work per frame
(the rest of its 33 ms is the 30 FPS cap). The target is a draw thread under ~12 ms per frame (60 FPS headroom,
no late frames = no animation judder at 30).

Profile of the draw thread (2 Nov, Lab route, 7939 samples over 14 s, the brief-2 switches on). Read it yourself:
`python tools/profile_functions.py C:/Users/ptr/Documents/sotc-pc/user/pcmem/profile-gpudraw3.txt
C:/Users/ptr/Documents/sotc-pc/out/gpu/bb-probe.exe 70 --inlines --qualified` (run from the main tree; the
resolved output is user/pcmem/profile-gpudraw3-fn.txt). Inclusive shares:
- Rasterizer::BindResources 31% (BindBuffers 14.3%, BindTextures 11.4%, BufferCache::ObtainBuffer 8.6%).
- Native-mode bookkeeping per bind: SotcNativeCopies::NoteWrittenBind 3.0%, SotcWriteExtent::NoteMark 1.8% self,
  SotcGen::Sum/Bump, std::map/multimap lookups (__tree lower_bound_multi / tree_min / lower_upper_bound) ~3.5%.
- Texture lookups: TextureCache::FindImage / FindTextureIdentity 4.9%, MemoLookup 1.5%, Image::FindView,
  ImageViewInfo construct + operator==, LeastRecentlyUsedCache::Touch, ForEachImageInRegion.
- Per-submit maintenance: Rasterizer::CopyGpuResults 4.9%, ExecuteShaderHLE 4.5%, ReplaceChangedPages 4.4%,
  BufferCache::SynchronizeMemory(Full) 2.6%, PcRingCopy::OnSubmitStart 2.4%, BufferCache::NativeDrain 2.3%,
  MemoryManager::TryWriteBacking 2.5%.
- ucrtbase.dll (memcpy/malloc/free) 8.8%.
- Waiting (not a target): Presenter::GetRenderFrame 14.6% (frame cap), FutexSleep 9.2% (no work queued).

The end design (wave 2, not yours): buffers by device address (BDA) into the imported guest arena, textures as
permanent bindless slots keyed by T# content, one per-draw packet (user data + slot indices + addresses) in a
ring, resolution moved off the draw thread. Wave 1 builds the parts that design needs and cuts the overhead.

## Rules (all specialists)
- Worktree base: `git checkout --detach 27cfa5a` (clean; nothing to apply).
- Do NOT commit. Do NOT run the game (W1 has a special rule, read it).
- Edit ONLY what the ownership map gives you. Anything else: put the exact snippet in your report.
- Every behaviour change is behind ONE new switch named in your section, read once (static const lambda +
  std::getenv, like the surrounding code). Unset = today's behaviour bit for bit; prove it by pointing at the
  branches. =2 = verify mode where the section says so (run new AND old, use OLD, count differences; the counter
  must read 0 in a run). =1 = new path only.
  Exception: removing work whose result is provably unused with the switch off (a diagnostic that computes and
  discards) may be done without a switch; state the proof (file:line of every reader).
- Counters in a 5 s report line (find how the neighbours print theirs: driver.cpp ~2000-2080 calls Report
  functions; give the driver.cpp lines as a snippet if driver.cpp is not yours).
- Correctness first: on any doubt take the old path and count it. Never assume guest memory did not change.
- Syntax check every touched .cpp: `C:/msys64/clang64/bin/clang++.exe -fsyntax-only -std=gnu++23 <flags of the
  matching entry of C:/Users/ptr/Documents/sotc-pc/out/gpu/compile_commands.json, with the worktree's gpu/shim
  and gpu/shadps4 -I paths FIRST and the worktree's -include bb_win_compat.h, third-party paths left in the main
  tree> <file>`. Paste the command and its output. Never claim a check you did not run.
- Style: follow the surrounding code; English comments; `sotc:` marker on every change in gpu/shadps4/...; no dead
  code, no leftover debug output.
- Final report in your last message: findings with file:line evidence, every change (file:function), checks run
  and output, exactly what a run with your switch must print, risks, unknowns.

## Ownership map
| Section | Owns (functions / files) |
|---|---|
| W1 BDA crash | vk_bda_table.{h,cpp}; the SOTC_N5_BDA / SOTC_N5_BDA_BUFFERS branches inside Rasterizer::BindBuffers (vk_rasterizer.cpp) and nothing else in that function; shader_recompiler BDA emission (grep BdaLoad / bda in shader_recompiler/backend/spirv and ir); src/probe.c ONLY to add a terminate / fail-fast diagnostic |
| W2 T# slot cache | vk_bindless.{h,cpp}; NEW file video_core/texture_cache/tsharp_slot_cache.{h,cpp}; the bindless branch of Rasterizer::BindTexturesFull (vk_rasterizer.cpp) |
| W3 flat native bookkeeping | gpu/sotc/native/native_copies.*; video_core/sotc_write_extent.*; video_core/sotc_generation.h; the per-bind calls into them inside BufferCache::ObtainBuffer (buffer_cache.cpp: only those call lines) |
| W4 per-submit maintenance | Rasterizer::CopyGpuResults, ReplaceChangedPages (find its file), ExecuteShaderHLE (vk_shader_hle.cpp), BufferCache::SynchronizeMemory / SynchronizeMemoryFull / NativeDrain, PcRingCopy::OnSubmitStart (pc_ring_copy.cpp) |

## W1. Make the BDA path work (switches exist: SOTC_N5_BDA=1, SOTC_N5_BDA_BUFFERS=1|2)
Facts: with SOTC_NATIVE=1 and SOTC_N5_BDA_BUFFERS=1 the process dies with 0xc0000409 (fail-fast) right after
the first "SOTC FPS CAP" line, before the first frame, with NO stack (crash log:
user/crashes/2026-11-02_01-56-31_0xc0000409.log; full output user/p3-bda.txt). 0xc0000409 bypasses the
unhandled-exception filter (src/probe.c:1708): it is __fastfail / abort / std::terminate (an uncaught C++
exception, e.g. vk::SystemError from vulkan-hpp, std::bad_alloc, a libc++ hardening assert) or /GS.
SOTC_N5_BDA=1 (the older variant) dies the same way. An unmerged earlier attempt added a SPIR-V BdaLoad guard
and per-stage logs: worktree C:/Users/ptr/Documents/sotc-pc/.claude/worktrees/agent-af4da99caeb89411a (read its
diff with `git -C <that path> diff`, take what helps).
1. Make the crash visible: in src/probe.c add, at startup, std::set_terminate (print the current exception's
   type and what() when there is one: rethrow inside try/catch for std::exception and vk::Error / vk::SystemError),
   a SIGABRT handler, and _set_invalid_parameter_handler, each printing a line and a stack with the existing stack
   printer (see how unhandled_fault prints "#0 guest offset", and how host frames are printed), then flushing.
   Keep it unconditional (it only runs when the process is already dying). This is the one probe.c change.
2. Read the whole BDA path: how the table is built (vk_bda_table.cpp), which buffers get device addresses, how
   their memory is allocated or imported. Known Vulkan rules to check one by one, with file:line:
   - a buffer whose address is queried needs VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
   - its memory must be allocated with VkMemoryAllocateFlagsInfo{VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT}. This
     includes memory IMPORTED with VK_EXT_external_memory_host (ring 8 imports the guest arena: find the
     vkAllocateMemory with VkImportMemoryHostPointerInfoEXT and check the flags);
   - the device must enable bufferDeviceAddress (Vulkan12Features); check vk_instance.cpp;
   - SPIR-V needs PhysicalStorageBuffer64 capability + addressing model, and aligned loads (Aligned operand);
   - vulkan-hpp calls throw on VK_ERROR_*: find every call in the BDA path that can throw on the first frame.
3. Fix what is wrong. If you find several candidates, fix all of them; say which one you believe caused the crash.
4. Special rule: you cannot run the game. Make your change so ONE run by the coordinator gives the answer: the
   terminate handler prints the cause; add one LOG_INFO per BDA table build step on the first frame only.
5. Report: the cause (or the top candidates), the fix, and what the coordinator must look for in the run output.

## W2. T#-content slot cache for bindless textures (switch SOTC_TSLOT)
Goal: resolving a texture binding = hash of the T# (and S#) dwords -> a permanent bindless slot index, with no
FindImage / FindView / ImageViewInfo work on a hit.
1. Read vk_bindless.{h,cpp} (SOTC_N5_BINDLESS: how slots are allocated, freed and written today), the bindless
   branch of BindTexturesFull, and the texture cache path a T# goes through today (FindTextureIdentity, FindImage,
   Image::FindView, MemoLookup, MemoGeneration). Write down, with file:line, everything that can make a cached
   (T# -> view) answer wrong: image destroyed/replaced, image contents re-uploaded, view recreated, layout
   change, the image's generation counters.
2. Build TsharpSlotCache (new file): key = the exact T# dwords (8) + the sampler dwords (4) when a combined slot
   is needed + the view-relevant flags the bind passes (storage vs sampled, NeedsDepth...). Value = slot index +
   ImageId + the generation(s) that validate it. Open addressing, fixed capacity (start 16384, power of two),
   no allocation on the hot path, LRU or clock eviction that frees the slot through vk_bindless.
3. On a hit with valid generations: return the slot; NO texture cache call. On a miss: the old path, then store.
   Any image destruction / replacement must invalidate (generation bump is enough if the check covers it).
4. Hook it into the bindless branch of BindTexturesFull only (SOTC_N5_BINDLESS=1 must be on for SOTC_TSLOT to do
   anything; say so in a log line).
5. =2: on a hit also run the old resolution and compare (image id, view handle, slot); count mismatches; use old.
6. Counters: lookups, hits, misses by reason, evictions, mismatches, ns spent per second.

## W3. Flat native bookkeeping (switch SOTC_FLAT_NOTES)
Goal: the per-bind native notes cost ~6-8% of the draw thread through std::map/multimap and repeated work.
1. Read SotcNativeCopies::NoteWrittenBind, SotcWriteExtent::NoteMark, SotcGen::Sum / Bump / BumpGranule and every
   caller (grep). For each: what it records, who reads it and when, and the container it uses. Use the profile
   (--inlines) to attribute the __tree samples to their callers.
2. Replace the ordered-map lookups on the hot path with flat structures: page-granular (4 KiB or 64 KiB) bitmaps /
   generation arrays indexed by (addr - base) >> shift for the arena the port already reserves (the guest range
   is below a known bound: find it), or a small open-addressing hash. Batch: if several binds of one draw note
   the same page run, note it once per draw.
3. Readers must get exactly the same answers. =2: keep both structures, answer from the old one, compare every
   query; count mismatches.
4. Counters: notes per second, ns per second (old vs new), mismatches.

## W4. Per-submit maintenance (switch SOTC_LEAN_SUBMIT)
Goal: in native mode (guest memory read in place) the per-submit work should be close to zero. Today it is
~20% of the draw thread (CopyGpuResults 4.9%, ExecuteShaderHLE 4.5%, ReplaceChangedPages 4.4%,
SynchronizeMemory 2.6%, PcRingCopy::OnSubmitStart 2.4%, NativeDrain 2.3%, TryWriteBacking 2.5%).
1. Read each function in full and measure from the profile which inner parts carry the samples (--inlines).
   For each: what it copies or scans, how many bytes / ranges per frame (find existing counters in the 5 s
   lines; logs of earlier runs are in user/pcmem/*/logs and user/p2-on-lab.txt), and WHY it still runs in
   native mode.
2. Cut what is provably not needed in native mode (SotcNative::Enabled(), TracksGuestPages() false, ring 8
   serving the range in place: SotcPcRing::HostServed). Make what remains incremental (dirty lists instead of
   scans, skip unchanged ranges, one pass instead of several).
3. ExecuteShaderHLE: find which HLE shaders run per frame and what they cost; if one of them does CPU work a GPU
   pass could do (or redoes work every frame), say so with evidence; implement only what you can prove safe.
4. =2 where a result can be compared (bytes copied, ranges found): compute both, use old, count differences.
5. Counters: per function, calls/s, ns/s, bytes/s before and after.
