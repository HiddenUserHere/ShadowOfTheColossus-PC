# Performance brief 4 (2 Nov): draw thread wave 2, and the BPE heap crash

You are a specialist of the SotC PC port. Read, in this order, before touching anything:
1. CLAUDE.md at the repo root.
2. This whole file (the shared part, YOUR section, the ownership map), and PERF3-BRIEF.md (wave 1, same rules).
3. Every function named in your section, in full, before you design anything.

## Where we are (commit 80a40fc)
Native mode (SOTC_NATIVE=1) renders correctly; the frame is limited by the draw thread `shadPS4:GpuDraw`.
Wave 1 (PERF3) made SOTC_N5_BDA_BUFFERS run (buffers reach shaders by device address through a per-draw block
table) and cut per-bind bookkeeping, but the draw thread did NOT get faster. Profile with everything on
(user/pcmem/profile-gpudraw4.txt; resolved: user/pcmem/profile-gpudraw4-fn.txt; resolve again with
`PATH=/c/msys64/clang64/bin:$PATH python tools/profile_functions.py user/pcmem/profile-gpudraw4.txt
out/gpu/bb-probe.exe 150 --inlines --qualified` from the main tree; llvm-symbolizer must be on PATH):
- idle 17.4% (GetRenderFrame 8.0% = 30 FPS cap, FutexSleep 9.4%), ntdll 25.4%, ucrtbase 9.6%;
- BindResources 35.8%: BindBuffers 16.5% (ObtainBuffer 9.0% inside it), BindTextures 12.9% (FindImage 4.0%);
- ExecuteShaderHLE 3.9%, ReplaceChangedPages 3.8%, CopyGpuResults 3.5%, SynchronizeMemory 2.6%, NativeDrain 2.3%.
The BDA path still calls ObtainBuffer per buffer and writes a block table per draw (~10 MB/s): it added cost.

The switches of the measured configuration (all must stay correct with yours on):
SOTC_NATIVE=1 SOTC_NATIVE_MASKS=prev SOTC_NATIVE_PARTICLES=prev SOTC_PIPE_FAST=1 SOTC_SPEC_MEMO=1
SOTC_CP_PRECISE_WAIT=1 SOTC_GEN_BIND=1 SOTC_FUTEX_WAIT=1 SOTC_N5_BDA_BUFFERS=1 SOTC_TSLOT=1 SOTC_LEAN_SUBMIT=1
SOTC_FLAT_NOTES=1. Ring 8 is the default (SOTC_PC_RING=8): the per-frame arena is imported into Vulkan and the
GPU reads guest memory in place (SotcPcRing::HostServed says whether a range is served that way).

## Rules
Same as PERF3-BRIEF.md "Rules", with these changes:
- Worktree base: `git checkout --detach 80a40fc` (clean).
- Do NOT commit. Do NOT run the game: the coordinator runs it. Make one run enough to answer your open
  questions (one-time LOG_INFO lines, counters).

## Ownership map
| Section | Owns |
|---|---|
| V1 BDA direct addresses | Rasterizer::BindBuffers (vk_rasterizer.cpp, the whole function); vk_bda_table.{h,cpp}; video_core/renderer_vulkan/sotc_guest_memory.{h,cpp} (lookup helpers only) |
| V2 texture binds | Rasterizer::BindTextures, BindTexturesFull and their helpers (vk_rasterizer.cpp); texture_cache/tsharp_slot_cache.{h,cpp}; TextureCache::ProbeTextureIdentity / FindTextureIdentity / UpdateImage / FindTexture (texture_cache.{h,cpp}) |
| B1 BPE crash: guest side | src/probe.c (fault handler, heap guard); NEW docs/re-stutters/native/BPE-CRASH.md (your findings) |
| B2 BPE crash: GPU side | video_core/buffer_cache/bpe_heap_guard.{h,cpp}; video_core/page_manager.cpp (the native stray-fault code only); gpu/sotc/native/native.cpp (stray-fault reporting only) |

## V1. Buffers by direct device address (switch SOTC_BDA_DIRECT)
Goal: for a buffer binding (V#) whose whole range lies in memory the GPU already reads in place (ring 8 imported
runs, SotcPcRing::HostServed / LookupGuestView in sotc_guest_memory), compute the device address by arithmetic
(run device address + (guest address - run base)) and SKIP BufferCache::ObtainBuffer and everything it does.
1. Read BindBuffers in full (both the BDA branches and the classic path), ObtainBuffer's hit path, and
   sotc_guest_memory.cpp (how runs are imported, their device addresses (g_bda), LookupGuestView). List with
   file:line everything ObtainBuffer does that a skipped bind would lose: sync/upload (not needed for in-place
   memory? prove it), GPU-modified marking for written bindings (AddGpuModified: who reads it in native mode?),
   write-extent notes, native copies (NoteWrittenBind), N1 verify, residency/eviction touches, buffer lifetime
   (the run must stay alive until the GPU is done: how are runs retired?).
2. Implement the direct path behind SOTC_BDA_DIRECT=1, only when SOTC_N5_BDA_BUFFERS is on: read-only bindings in
   served runs first; written bindings only if you can show every reader of the marks you skip is satisfied (else
   keep ObtainBuffer for written ones and say why).
3. The per-draw block table: today it is rebuilt and uploaded per draw. Reuse the previous table when the slots
   are identical (there is a reuse counter: read why it misses), and write only changed entries.
4. Attribute the rest of BindBuffers' 16.5% (7.5% outside ObtainBuffer) with the profile and cut what you can
   prove useless.
5. =2: compute the direct address AND run ObtainBuffer; compare the device address (buffer address + offset) and
   size; count mismatches; use the old answer.
6. Counters: direct binds, fallbacks by reason (not served, written, straddles runs, ...), mismatches, tables
   reused/written, bytes per second.

## V2. Texture binds that hit (switch: keep SOTC_TSLOT, add values if needed)
Measured with SOTC_TSLOT=2: "tex lookups 796973 hits 213806" (27% hits), mismatches 0.
1. Find out why 73% miss: add a miss-reason counter (no memo entry, key differs (which field), epoch moved,
   dirty, warm pending, GenSumImage moved, not a plain binding (num_bindings, written, mip fallback, ...)).
   Make the coordinator's run print it.
2. Fix the dominant reasons where it is correct (e.g. a key that includes per-draw noise, a capacity problem,
   an epoch bumped too broadly, plain-binding conditions that exclude common cases).
3. The second pass (FindTexture = UpdateImage + FindView) runs for every bind. Skip it when nothing about the
   image changed since its last bind in this frame (use the image's generation / dirty flags / a per-image
   "updated in tick N" stamp) and cache the view per (image, view info) where it is provably stable.
4. =2: run the old path too, compare image id, view handle, layout; count mismatches; use old.
5. Counters: hits, misses by reason, second-pass skips, mismatches.

## B1. BPE heap crash, guest side (no switch needed for diagnostics; any behaviour fix behind SOTC_BPE_FIX)
Facts: about 1 run in 4, 11-33 s after start, the game faults reading at guest offset 0x49047 (also 0x490a0,
older runs 0x9427f). Crash logs: user/crashes/*.log (grep "Crash at"); a recent full one is in
user/crashes/2026-11-02_02-20-34_0x3.log and user/crashes/2026-11-02_01-52-34_0x3.log with register dumps and
guest stacks (#0 0x48d70, #1 0x47eed, #2 0x128ff, ...). The fault address is always ~0x10dc000000-0x110c000000
(above 4 GiB, a guest heap mapping) and VirtualQuery afterwards says it is COMMITTED, PAGE_READWRITE, MEM_MAPPED
(page_manager.cpp:1553-1560 prints it: "SOTC NATIVE stray read state"). A read of committed RW memory should not
fault: explain it (unmapped and remapped between the fault and the query? a placeholder split? a different
address than the one printed? read the fault handler in src/probe.c and the native stray-read path).
The heap allocator (eboot+0x48xxx-0x49xxx, free at 0x495e0) is known to be corrupted by GPU writes on stale
descriptors (comment at src/probe.c ~920, video_core/buffer_cache/bpe_heap_guard.cpp).
1. Disassemble the guest code around 0x48d70-0x490b0 and its callers 0x47eed, 0x128ff (out/boot-linked.bin is
   the linked image the loader maps; prepare.py / link_modules.py tell its layout; use the REA tools or
   llvm-objdump on a raw dump: say which). What does 0x49047 read (which field of which heap structure), and
   where does the bad pointer come from (registers in the crash logs: r8 = 0x10fe06e530 in one).
2. Decide: is the pointer garbage (heap metadata corrupted: by whom?) or valid memory that the port made
   inaccessible (mapping change, decommit, protection by a tracker that native mode should not use)?
3. Write the evidence to docs/re-stutters/native/BPE-CRASH.md (English). If the fix is on the CPU side (mapping,
   protection, fault handler), implement it behind SOTC_BPE_FIX=1; if it needs one more run to decide, add the
   exact diagnostics (one line per fault: the page's state from BEFORE the fault if you can keep it, mapping
   history of that range) and say what to look for.

## B2. BPE heap crash, GPU side (same switch SOTC_BPE_FIX for a fix)
Same facts as B1. Hypothesis to prove or kill: in native mode with ring 8 the GPU writes guest memory in place
(imported host memory), so GPU writes of dispatches with stale descriptors land directly in freed heap blocks;
the BPE heap guard (bpe_heap_guard.cpp) was built for the readback path and may not protect anything in native
mode.
1. Read bpe_heap_guard.cpp: what it protects, when it runs, and whether native mode (no readbacks, in-place GPU
   writes, SOTC_N5_BDA_BUFFERS) reaches it at all. List every GPU write path into guest memory in native mode
   (storage buffer writes through served runs, BDA stores, CopyGpuResults, fills, DMA).
2. Find whether a written binding can cover heap memory the game already freed (written V# ranges vs the heap's
   free list: the guard knows the heap object and its metadata layout).
3. Implement, behind SOTC_BPE_FIX=1, the narrowest protection that keeps heap metadata intact in native mode
   (e.g. clamp or redirect written bindings that overlap a freed block's header, or restore the metadata words
   after the GPU work as the guard does after readbacks), plus a counter of hits. =2: detect and count only.
4. Add diagnostics so one run proves or kills the hypothesis: per 5 s, written bindings overlapping freed heap
   blocks (count, size), and on a crash the last such overlap near the fault address.
