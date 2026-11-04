# GCN-NATIVE decisions (S1-S5)

Decision log for the implementation of stages S1-S5 of [ARCH-GCN-NATIVE.md](ARCH-GCN-NATIVE.md). Each entry says
what was decided and why. Contract headers: `gpu/shadps4/shader_recompiler/gcn_layout.h` (GPU-visible layouts and
the hash) and `gpu/shadps4/video_core/sotc_arch/gcn_native.h` (host API). Everything is off unless its switch is set;
nothing here was run in the game yet (the user asked for no game runs until told).

## D1. Build on the ARCH-DRAW / ARCH-BINDSET parts, do not start over

**Decision.** The GCN stages are new modes layered on parts that already exist: `SotcArch::AddrTable` (the guest page
table, 16 KiB pages, OnPlace/OnRemove events, clean bits), `SOTC_SHADER_ADDR` table mode (profile.shader_addr 3/4:
shaders translate every access through the page table), `SOTC_HEAP` (bindless arrays, heap slots per view),
`SOTC_GENERIC_BUFFERS=2` (V# format decoded in the shader), `SOTC_LAYOUT_UNIFIED` and `Info::writes_memory`.

**Why.** Those parts already carry the hard pieces (sparse arena placement events, per-page translation in SPIR-V,
heap slot lifetime, in-shader format decode) and their verify modes. What they lack is exactly what S0 measured as
the cost: per-op CPU work that remains around them (Touch per bound buffer, TableMemo::Resolve per heap stage,
per-op sync and transitions). The new stages remove that per-op work; they do not add a second page table or heap.

## D2. One upstream page table is not reused for guest buffers

**Decision.** The upstream `bda_pagetable_buffer` (64 KiB arena blocks, DMA path with FaultBuffer) stays as it is;
the GCN stages use `AddrTable` (16 KiB guest pages, 64 GiB window).

**Why.** The upstream table only covers the buffer cache arenas, not the ring-8 imported runs, and its block size is
wrong where a ring-8 run boundary falls inside a 64 KiB block (addr_table.h explains it). AddrTable covers both.

## D3. S1: the table is enabled by SOTC_GCN_MEM on its own, and ring-8 imports become events

**Decision.** `AddrTable::On()` is true when `SOTC_GCN_MEM` is on, even without `SOTC_SHADER_ADDR`. Ring-8 imported
runs announce themselves (`Mem::OnImport` / `OnRelease` from sotc_guest_memory.cpp) instead of being entered by the
first Touch. Verify (`=2`) compares the table with ObtainBuffer's answer on every classic buffer binding.

**Why.** S1 has to be verifiable before any shader reads the table: with classic shaders, every binding the old path
makes is a free test of the table. Entering imported runs at import time is what makes the table event-driven; Touch
remains only as the residency path for pages the table has never seen.

## D4. S2: residency checked on the CPU with a bit test, the GPU miss ring only as a safety net

**Decision.** Per op, per addressed buffer, the CPU tests the table's placed bits (`Mem::Placed`, lock-free) and does
nothing more when they are set and S4 owns freshness. Only an unplaced range takes `AddrTable::Touch`. Shaders that
still find an entry 0 append a page miss to the miss ring; the host makes that page resident at the next submit.

**Why.** A pure GPU-fault model (miss -> zeros for one frame -> CPU maps it) is not acceptable for buffers: a vertex
buffer read as zeros drops a mesh for a frame, and a compute shader writing to an unmapped page loses its results.
The CPU already holds every V# (it builds the flat buffer), so a residency test costs two loads per page run and
never shows a wrong frame. The miss ring stays as the counter that proves the CPU test is complete (it should read 0
in steady play) and as the recovery path when it is not.

## D5. S2 extends shader_addr table mode instead of a new buffer path

**Decision.** `Profile::gcn_buf` is only set on top of `shader_addr` 3/4. It widens the set of shader-addressed
buffers to formatted buffers whose loads decode the V# format at run time (`SOTC_GENERIC_BUFFERS=2`), so they lose
their texel-buffer descriptors too. Written buffers already take the table path in mode 3.

**Why.** One translation routine (TableAddresses) for every guest buffer keeps the generated code and the verify path
in one place. Formatted buffers without the generic decode keep their descriptors (their format is fixed per
pipeline key, and the classic path is correct for them).

## D6. The globals buffer address travels in DrawPacket::arena_guest_base

**Decision.** One host-visible buffer (`Shader::Gcn` layout: header, miss ring, seen bitmap, T# table) whose device
address is pushed in the packet word `arena_guest_base` (push offset 88) for gcn pipelines.

**Why.** The push block is exactly 128 bytes (Vulkan's guaranteed minimum) and every word is taken. The arena words
are always 0 today (the arena window was never enabled in table mode: the page table covers the arena), so the word
is free in table-mode pipelines. A device address cannot be baked into the shader: the shader cache outlives the
session and the address changes between sessions.

## D7. S3: a GPU hash table T# -> heap slot, keyed by the 8 T# dwords and a view class

**Decision.** Open addressing, 64 Ki entries of 48 bytes (3 MiB host-visible), linear probing up to 16 probes, full
key compare in the shader. The key is the raw T# plus a view class = (is_written, is_depth, is_array, bindless key);
samplers use the same table with the S# and a reserved class. The hash is murmur3_32, identical in C++ and SPIR-V.

**Why.** `ImageViewInfo` depends only on the 32-byte T# and those three flags (image_view.cpp), and the bindless key
picks the heap array, so (T#, class) identifies the view exactly. Raw words avoid any decode in the shader. S0
measured 640-1,039 distinct T# per frame and 8 S#: 64 Ki entries keep the load factor near 1-2%, so probes end at
the first or second entry. Host-visible memory lets the host write entries with no upload command and makes the
mapping the CPU replica for verify.

## D8. S3: the CPU inserts on first sight, the GPU miss path is the fallback

**Decision.** While `TableMemo::Resolve` still runs for a heap stage (verify, or ops the table missed), every slot it
resolves is inserted into the table (`Tex::NoteResolved`). A T# the GPU misses samples the null slot of its key and
appends an image miss; the host resolves it at the next submit through the same texture cache path and inserts it.

**Why.** ARCH-GCN-NATIVE section 7 lists the visible first-use fallback as a risk; inserting on CPU first sight removes
it for everything the classic path has already met, and the miss ring covers the rest at the cost of one frame of
the null texture, counted (gate: under 5 per frame in steady play).

## D9. S3: entries die with their heap slot

**Decision.** When a heap slot is released or evicted (vk_bindless.cpp), every table entry naming it becomes a
tombstone (`Tex::OnSlotReleased`, reverse map slot -> entries). The slot itself is reused only after the frame fence,
as today.

**Why.** The table must never point at a slot that now holds another view. Tombstones are written when the slot is
released or evicted, before it can be rewritten (the fence deferral of Release, the IsFree test of Evict). A draw
already recorded but not yet executed then reads the tombstone and samples the null slot for that frame (one miss,
resolved at the next submit): a visible frame at worst, never a wrong view or a freed descriptor.

## D10. S4: freshness is one batch per submit at submit start

**Decision.** At each submit start (where `PcRingCopy::OnSubmitStart` runs) the draw thread drains the miss ring, then
synchronizes every placed table page whose clean bit is clear and refreshes every table-referenced image whose memory
the CPU changed. Per-op sync for table-addressed buffers is skipped when S4 is on (not in verify).

**Why.** PS4 semantics: the game writes what a command buffer reads before it submits it. The clean bits already
record every CPU write (memory tracker and region manager hooks), so the batch sees exactly the pages that changed.
Verify keeps the per-op path and counts stale data it still finds (writes after submit start through label waits are
the known risk).

## D11. S5: barriers from static write bits, transitions dropped only when layouts are unified

**Decision.** With layouts unified active, image transitions between shader-access layouts are dropped (every image
is GENERAL). One global memory barrier follows an op whose shaders write memory (recorded lazily before the next op,
outside render passes), and one at each render pass end.

**Why.** Per-image transitions per op are S0's "draw extras". A global barrier is what a native GCN driver emits
between dependent dispatches; its cost is one command per writer instead of per image per op. Without the unified
layouts feature the old transitions are required by Vulkan, so the stage is refused there.

## D12. Ownership, verification and shader cache

- Every stage has `=2` verify; a stage becomes default only with 0 differences on the test route, a correct image and
  a measured gain (ARCH-GCN-NATIVE 8.4).
- New profile fields (`gcn_buf`, `gcn_tex`) grow `Shader::Profile` from 68 to 72 bytes (there was no tail padding);
  the cache check reads 68-byte profiles of older caches with both off, and the shader binary version is bumped so
  caches of earlier builds are rebuilt (CHANGELOG note when a stage becomes default).
- Implementation agents worked on disjoint files; integration (vk_rasterizer.cpp, pipelines) by one owner.

## Open items (to measure when game runs are allowed)

- GPU time of table translation on the heavy views (PCIe reads of imported memory, Intel BDA speed).
- Miss counts per frame in steady play (target 0 page misses, under 5 image misses).
- Whether S4's submit-start point misses CPU writes made after submission (verify counter).

## Implementation decisions (agents, 4 Nov)

- **Globals buffer allocation** (gcn_globals.cpp): raw Vulkan allocation (as shader_addr.cpp), not a
  VideoCore::Buffer, because the miss ring and the T# table need HOST_COHERENT memory, which VideoCore::MemoryType does
  not guarantee. DEVICE_LOCAL|HOST_VISIBLE (ReBAR) is preferred. Lives for the process, like table_memo.cpp's ring.
- **Miss drain** reads stamp, body, stamp again (seqlock): a slot reused mid-read (the ring lapped the reader) stops
  the drain; overflows count lost records.
- **S2 fast path stamps the use** (`BufferCache::NoteBindUse`), as AddrTable::Touch's fast path does: without it the
  idle eviction could release a block a shader reads every frame, and D4 forbids the frame of zeros that follows.
- **Written buffers always take Touch** in S2: the GPU-modified marking that later written binds rely on is made by
  ObtainBuffer(is_written) inside it.
- **S2 verify** can only see placement changes made by Touch (Translate before and after), not sync-only uploads.
- **S4 sequence protocol**: the batch reads `AddrTable::CleanSequence()` before syncing a run and marks it clean only
  under that sequence (`MarkClean(addr, size, seq)`), so a CPU write that lands during the sync is not hidden by a
  late clean mark.
- **S4 syncs through ObtainBuffer, not Touch**: the table entries already name the backing (every move is an OnPlace
  event), so the batch only uploads; Touch would rewrite entries. Runs are cut at 64 MiB.
- **S4 refreshes only CpuDirty images** that the T# table references (`TextureCache::SotcRefreshCpuDirty`, which
  takes the reentrant cache lock); MaybeCpuDirty images stay with the bind path.
- **S4 known gap (by design, D10)**: a CPU write that lands after the batch and before an op is seen at the next
  submit when per-op sync is skipped. Verify mode counts it.
- **S2 formatted candidates**: a formatted guest buffer is shader-addressed when it is read-only and
  SOTC_GENERIC_BUFFERS >= 2 (its formatted loads are lowered to raw loads with a run-time decode); the V# format is
  not part of the rule, which only uses what the compiled shader fixes. Written formatted buffers keep descriptors (the
  generic decode covers loads only). The Info cache bit (`shader_addr_buffers`) stays profile-free; the profile filter
  (gcn_buf and table mode) is applied in ShaderAddrBuffer / ShaderAddrBufferAt, so with gcn off the output is unchanged.
- **S2 page misses** are recorded only in TableAddresses, which every table load, store and verify path goes through.
  Records are plain stores with the stamp last; the host reads them after the submit fence.
- **S5 drop gate is narrower than D11** (image.cpp `GcnDropTransition`, in `Image::GetBarriers`, the barrier builder
  Runtime::Transit uses): a transition is dropped only for a shader read whose previous access was a shader read
  (no shader write, transfer or attachment access on either side) and not to the feedback-loop layout. Why: the
  global barrier orders writer -> next op only, and inside an open render pass it waits for the pass end, so a read
  after a write in the same pass, a write after a read (WAR) and copies (not ops) still need their own barriers. A
  later contract step (AfterOp with reads, BeforeOp told whether the next op writes) can widen it.
- **S5 pass-end barrier lives in `Scheduler::EndRendering`** (after endRendering, only when a pass was open), which
  covers every caller including the implicit ends in BeginRendering, instead of wrapping nine call sites.
- **S5 verify** counts would-drop transitions and would-record barriers without recording.
- **S1 table with GCN_MEM alone**: `AddrTable::On()` = (SOTC_ADDR_TABLE and SOTC_SHADER_ADDR) or GCN_MEM. Init failures
  of a GCN_MEM-only run refuse the stage (log line) instead of exiting. Side effects to know: the mirror is off in
  table mode, the packet's mirror device word is the table base, and SOTC_SHADER_ADDR=1 with GCN_MEM resolves to
  table mode 3 (GCN implies the table).
- **S1 ring-8 runs enter the table at import** (`sotc_guest_memory.cpp ImportRun`: OnRelease of the replaced run,
  OnImport of the new one), whole 16 KiB pages only: edge pages of a run that does not start or end on a page stay
  with Touch. Ring-8 runs have no VideoCore::Buffer, so their owner id is 0 and `AddrTable::Backing` refuses them
  (ObtainRead falls back to ObtainBuffer). An OnPlace with device address 0 acts as OnRemove.
- **S1 clean protocol** (contract extension in addr_table.h, used by S4): `CleanSequence()` before a sync and
  `MarkClean(addr, size, sequence)` after it; MarkClean refuses when the clean state changed in between, an entry is
  not uploaded yet, or a page is unplaced. A bare MarkClean(addr, size) would hide a CPU write made during the sync.
- **S1 stale scan is lock-free**: `ForEachStalePlaced` scans the atomic placed/clean bit words, 64 pages per word,
  skipping zero words and merging runs across words.
- **S3 sampler key** (contract extension): a sampler entry is keyed by its S# words and a sampler class that carries
  is_depth, the post op and, for DisableAnisoIfSingleLod, the run-time fact it reads from the paired T# (computed in
  the shader). The host rebuilds the sampler from (words, class) alone, so depth samplers and post-op samplers can be
  table entries instead of colliding with plain ones.
- **S3 image identity by events**: the table subscribes to the BindSet image events (bindset_events.h) whether or
  not SOTC_BINDSET is armed. Every cause except a CPU write (when S4 refreshes in place) tombstones the entries of that
  image, so a T# that the texture cache now resolves to another image (a new registration over it, a rebind, a
  depth association) misses once and resolves again. `NoteResolved` carries the ImageId for that reverse map and for
  S4's refresh list.
- **Integration: GCN submit start** is its own hook in liverpool.cpp (outermost gfx and compute submits), next to
  PcRingCopy::OnSubmitStart but not gated on the ring mode, so S2-S4 work without SOTC_PC_RING=8.
- **Integration: the cache migrates 68-byte profiles** (before gcn_buf/gcn_tex) with both off, so a user's shader
  cache survives this build when every GCN switch is off.
- **Integration: packet globals word** is written for gcn_buf != 0 or gcn_tex != 0 (verify pipelines too); arena_size
  stays 0. Heap stages memcpy the whole DrawPacket, so offset 88 reaches them as well.
- **Integration: TableMemo::Resolve is kept** for heap stages until the recompiler side confirms which stages no
  longer read the index table (correctness first); the texture phase gain of S3 waits for that skip.
- **Open (S5)**: async compute dispatches record BeforeOp/AfterOp in whichever command buffer is current; to check
  when the async path is on.
- **S3 push layout**: heap stages keep their byte layout; with gcn_tex == 1 the heap push struct declares one more
  member, the globals word at byte 88 (DrawPacket::arena_guest_base, already copied whole into heap pushes), so the
  later members change index but not offset. gcn_tex 0 and 2 compile exactly as before.
- **S3 probes are hoisted**: every image and sampler binding of a heap stage is probed once at the entry block (an
  OpLoopMerge loop of at most 16 probes, uniform per draw since it depends only on sharps). An invalid sharp takes the
  null slot without a probe or a miss record. Miss = slot 0 (the null view of every key, the default sampler).
- **S3 keeps TableMemo::Resolve by default** (staged): SOTC_GCN_TEX=1 makes the shaders read the GPU table while
  Resolve still runs per op, prefills the table (NoteResolved) and keeps the classic per-image work. That proves the
  table (correct image, misses near 0) before the CPU work goes. SOTC_GCN_TEX_SKIP_RESOLVE=1 (with SOTC_GCN_BARRIER=1)
  skips Resolve for measurement. Why not by default: Resolve's binds also carry the cases the classic bind handles
  itself (an image that is a render target of the same draw, the feedback loop declaration; a sampled stencil image
  whose transition depends on the depth registers; a pending rebind; composite ops). Those must become events (the
  render target set is known at BeginRendering, stencil associations are already BindSet events) before the skip can
  be the default.
- **Review fixes (R1)**: OnPlace/OnRemove clear the clean bits again after changing entries (with S4 on), which the
  clean protocol needs; the S4 batch checks that the table names the backing ObtainBuffer answered before marking a
  piece clean; a GCN_MEM + SOTC_SHADER_ADDR run whose table cannot be created exits (as the table's own switches do)
  instead of leaving shaders on a mirror window that does not exist. This narrows D3's "refuse instead of exit" to
  GCN_MEM alone.
- **S4 skips imported pieces**: a stale piece inside one ring-8 imported run (`SotcPcRing::HostServed`) is read by
  the GPU in place, so it is marked clean with no ObtainBuffer (which would build arena blocks over it and stamp them
  all used). Counted as "in place" in the SYNC line.
- **Known, not fixed here**: ring-8 ImportRun never frees retired runs (pre-existing leak, grows with re-imports); the
  batch's ObtainBuffer can race a guest unmap of a stale range (the bind path's Touch has the same exposure);
  ForEachStalePlaced scans 2 x 65,536 atomic words per submit (a summary bitmap would cut it).
- **Review fixes (R2, R3)**: the probe loop is a proper structured loop (OpLoopMerge followed directly by the exit
  branch). Bindings the key cannot express (r128, atomic, descriptor arrays, mip fallback, constant mip, image post
  op, and storage images) are not probed: `shader_recompiler/gcn_rules.h` (TableImage / TableStage) is the one rule the
  shader, the CPU prefill (table_memo.cpp) and the Resolve skip all use; such bindings keep the per-draw index table.
  The table publishes an entry with an x86 store fence before its state word (the mapping may be write-combined).
  Under SOTC_HEAP_UI_LINEAR=1 with another render size the table is refused (the point-to-bilinear rewrite is per op).
  Live table slots are stamped in the bindless LRU once per submit (`Tex::StampLiveSlots`).
- **Probed descriptor indices are NonUniform**: the host inserts and tombstones entries while earlier draws may still
  run, so two invocations of one draw can read different slots (miss vs hit). The index is decorated NonUniform
  (spec-correct; on uniform data the cost is one iteration of the driver's waterfall loop). The device feature
  (sampled-image non-uniform indexing) is already required by the heap.
- **Review fixes (R4)**: a writer's pending global barrier ends an open render pass before the next op (deferring it
  to the pass end let the next draw read unordered data); BeforeOp runs before BeginRendering; dropped transitions
  merge their access into the tracked state instead of replacing it; the packet's globals word follows the pipeline's
  own profile (not the live switch); the shader binary version reads the switch mode, not ProfileValue (no refusal at
  static init); the Resolve skip requires TableStage of every heap stage.
- **Init order**: the page table and the globals buffer are created in the PipelineCache constructor, before the
  profile reads ProfileValue (they were created later in the rasterizer constructor, so SOTC_GCN_TEX was refused at
  every start). Both Inits are idempotent.
- **S5 expectation (measured by reading, to confirm in game)**: the layouts-unified stage (A1) already drops plain
  read-to-read image transitions, so S5's own drop rarely fires; S5's value is the global writer barrier, and its cost
  is a pass end after each writer inside a pass. SOTC_GCN_TEX_SKIP_RESOLVE leaves first-use images without the
  classic transition and the decline cases: measurement only.
- **Offline SPIR-V validation**: `tests/test_gcn_native.cpp` (target `gcn-native-test`, with a minimal GCN encoder in
  `tests/gcn_encode.h`) checks the hash against an independent murmur3, the GPU layouts, and compiles synthetic GCN
  programs through the recompiler: a compute copy with shader_addr 3 + gcn_buf 1 (miss path present), shader_addr 3,
  shader_addr 0, and a heap stage with gcn_tex 0 and 1. All five pass `spirv-val --target-env vulkan1.3`. It found
  one bug, fixed: a heap stage with no flat buffer cannot load its sharps at run time, so it keeps the index table
  (`gcn_rules.h TableReadable`). The ninja target does not link yet because `tests/test_gpu_runtime_stubs.cpp` lacks
  stubs for runtime symbols libbbgpu now references (runtime_memory_*, bb_setjmp, ...); it was linked by hand with
  unresolved symbols allowed. Not covered: fragment stages with implicit LOD, anything on a GPU.

## How to try it (when game runs are allowed)

One stage at a time, verify first, on the test route (user/routes/tests.json), one run per configuration:

| Step | Switches | Look at |
|---|---|---|
| S1 verify | `SOTC_GCN_MEM=2` | `SOTC GCN MEM` line: verify mismatch 0 |
| S1 + S4 verify | `SOTC_GCN_MEM=1 SOTC_GCN_SYNC=2` | `SOTC GCN SYNC`: stale 0 |
| S2 | add `SOTC_SHADER_ADDR=1 SOTC_GCN_BUF=1` (needs `SOTC_GENERIC_BUFFERS=2`) | `SOTC GCN BUF` fast/slow, `GLOBALS` page misses ~0, image |
| S3 verify | `SOTC_HEAP=1` (+ its requirements) `SOTC_GCN_TEX=2` | `SOTC GCN TEX` verify mismatch 0 |
| S3 | `SOTC_GCN_TEX=1` | image misses < 5/frame in steady play, image |
| S5 | `SOTC_GCN_BARRIER=2`, then `=1` | would-drop counts, then image and per-op cost |
| S3 skip (measurement) | `SOTC_GCN_TEX_SKIP_RESOLVE=1` with TEX=1 and BARRIER=1 | texture phase cost; not a correctness mode |
| S6 verify | `SOTC_PARALLEL_RECORD=1 SOTC_GCN_COMMIT=2` (with S1-S5 on) | `SOTC GCN COMMIT`: verify mismatch 0, eligible % |
| S6 | `SOTC_GCN_COMMIT=1` | per-op cost (metrics.csv draw_busy_ms / draw_ops), dispatch cost |

The 5 s report prints one `SOTC GCN ...` line per stage that is on; the startup log prints the decision of each stage
(`SOTC GCN: mem=... buf=... tex=... sync=... barrier=...`) and a `refused: <reason>` line for any stage refused.

# S6 (thin draw thread)

## D13. S6 builds on SOTC_PARALLEL_RECORD; what is already off the draw thread stays there

**Decision.** S6 (`SOTC_GCN_COMMIT`) needs `SOTC_PARALLEL_RECORD=1` and adds only what the prep threads do not do yet.
Already off the draw thread before S6: the pipeline lookup (command processor, `op.pipeline`), and for draws the push
data, dynamic state values, bind hashes, resolved resources (SOTC_PR_RESOLVE) and heap memo keys (SOTC_PR_KEYS); the
render state memo (SOTC_RS_MEMO) and the pending-operations fast path (SOTC_POP_FAST) already exist.

**Why.** Re-doing them would duplicate verified machinery. What S0 measured as still on the draw thread is: every
dispatch's preparation (dispatches are 30-38% of ops and the costliest, 10-12.7 us), and the bind caches' own cost
(the frame bind cache lookup 8.1% and BindResources' prologue 8.2%), which buys little once S2-S4 make the binds
themselves cheap.

## D14. S6a: dispatches are prepared like draws

**Decision.** The GpuPrep threads fill the op's PreparedDraw slot for dispatches too (`Rasterizer::PrepareDispatch`,
from the prep thread's replayed registers and the stage's flat copy; the compute program registers `op.cs` are only
the dispatch size and HLE inputs, which stay on the draw thread), and the draw thread hands it to
DispatchDirect / DispatchIndirect -> BindResources. Mode 2 computes and compares, never uses (the SOTC_VERIFY pattern).

**Why.** The slot, the publish protocol (ready_seq, map_epoch) and the verify already exist for draws; dispatches only
needed their compute-specific inputs.

## D15. S6b: a commit lane for fully GCN ops instead of more caching

**Decision.** An op whose pipeline is commit-eligible (every guest buffer table-addressed with S4 freshness, every
texture and sampler from the T# table with the Resolve skip, S5 barriers) skips the fast bind, the bind cache and the
frame bind cache, lookup and store, and binds directly. Eligibility is a pure function of the pipeline, cached on it.

**Why.** The caches exist to avoid per-op resource resolution; for an eligible op that resolution is already a bit test
per buffer and a GPU table probe per texture, so the cache lookup (hashing the op, validating the entry) costs about as
much as what it saves. Mode 2 keeps the caches and only counts eligible ops, which also measures S2-S5's coverage: the
share of ops that can go through the lane is the share the GCN model fully covers.

Implementation notes (S6 agents):
- **Commit eligibility is a whitelist**: of the port's special buffers only the flat buffer, clip planes and LOD
  feedback are allowed (GDS, the BDA page table / fault buffer, shared memory and the BDA block table make a pipeline
  ineligible); a stage with DMA reads, sharp tables or fmasks is ineligible; any stage with images or samplers must be
  a gcn_tex == 1 heap stage whose bindings are all in the table, with the Resolve skip. When unsure, not eligible: the
  mode 2 counters show the coverage, and the list can widen from measured reasons.
- **Commit needs SOTC_PARALLEL_RECORD=1** only for the dispatch part; when the machine has too few threads for prep
  workers the lane still applies and the SYNC/COMMIT line shows 0% prepared dispatches.
- **Dispatch slots** carry `is_dispatch`; TakePrepared refuses a slot of the other kind and keeps the draw counters
  (pr_*) draw-only. The SOTC_CULL_EXPRESS footprint of dispatch slots is filled as before (after PrepareDispatch).
- **Dispatch preparation takes the prep thread's registers** (`PrepareDispatch(regs, cs, pipeline, slot)`): the
  registers replayed on the GpuPrep thread equal the draw thread's back registers at that op, so the push data, bind
  cache eligibility and hashes, frame cache key, heap memo key and resolved resources of a dispatch are all computed
  ahead, as for draws. Mode 2 (and SOTC_VERIFY=1) compares every field with the draw thread's inline values (mismatch
  mask in the first 16 log lines); mode 1 with SOTC_VERIFY uses the slot only on a match.
- **The slot is used only for ops whose pipeline the command processor resolved** (`use_resolved`), so the slot and the
  pipeline the draw thread binds are the same one.
- **The commit lane forces the op's cache eligibility off** (cache_eligible and fast_eligible), which switches off every
  cache path at once (fast bind, the older bind cache, the frame cache, packet caches, heap holds, compute binds) for
  lookup and store, without a second bind path. BindCacheSync still runs as for any ineligible op.
- **Counting**: "dispatches prepared" counts slots available at the draw thread (liverpool); verify counts matches.
- **Review fixes (R6)**: Eligible caches only the per-pipeline part and reads Buf/Sync/Barrier on every call (a stage
  refused after first use must take the lane off); a pipeline with two heap stages of one kind is ineligible (the op is
  dropped); the bind cache sync still gets the op's pre-lane eligibility, so lane draws do not invalidate other cache
  entries; image-bearing pipelines need heap_images == 1 (not the heap GPU verify).
- **Lane tiers**: buffer-only pipelines (most compute) take the lane with S2/S4/S5 alone; pipelines with textures need
  the Resolve skip, so for them the lane has the same measurement-only status as SOTC_GCN_TEX_SKIP_RESOLVE.
