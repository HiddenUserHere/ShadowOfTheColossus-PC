# ARCH-GCN-NATIVE: let the shaders resolve their own resources, as GCN does

Status: design, nothing implemented. Written 4 Nov 2026 after the measurements below. Supersedes the direction of
ARCH-BINDSET.md (whose stages 2-5 it reuses) and gives the packet, BDA and bindless work of PERF-BRIEF..PERF4-BRIEF
one goal.

## 1. Where the time goes today

Test route (user/routes/tests.json, NVIDIA, 30 FPS cap), draw thread `shadPS4:GpuDraw`:

- 4,200-5,500 ops per frame in the heavy views, 6.5-9 us per op: 30-45 ms of draw thread per frame. The thread is
  the bottleneck: real GPU execution of the same frames is 13-20 ms (SOTC_TRACE_GPU timestamps), 44 ms in the
  worst area-entry frame.
- Entering an area the per-op cost is 9.5-13 us for ~40 frames (first use of every object: image and view
  creation, first syncs, cache misses), then 6.5-7 us.
- Profile of the draw thread (SOTC_PROFILE=GpuDraw, the 30 FPS cap wait excluded): BindResources 27-29% inclusive
  (BindBuffers 9-12, ObtainBuffer 6-8, BindTexturesFull 5-6), native copy bookkeeping 3-5, ExecuteShaderHLE 4,
  ReplaceChangedPages 4, SynchronizeMemory 3-4, FindTextureIdentity 3, ring and drain work 2-3 each, and a long
  tail of 1% items. ntdll (locks, waits) and ucrtbase (memcpy, malloc) another ~40% between them.

A native Vulkan/DX12 engine spends 0.5-2 us of CPU per draw; with bindless and indirect draws much less. The PS4
itself spends well under 1 us per draw on this game.

## 2. Why more caches do not get there

Measured on the same route on 4 Nov (one run per configuration):

| Attempt | Result |
|---|---|
| Op key repeat rate (SOTC_OPSET_STATS) | key D (decoded descriptors + pipeline + targets) repeats for 82-87% of ops (95-98% with the address modes on); keys with user data 25-33% |
| Frame bind cache with the content rule (SOTC_BIND_CACHE_FRAME + _CONTENT=1, META on) | 91-94% hits, 0 verify differences, but only ~3% less per op: only ~50% of ops are eligible (compute 30-40% and storage/fmask 16-20% refused) and a hit still does transitions, barriers, syncs |
| Same plus dispatches (SOTC_OP_COMPUTE_BIND=1) | no gain; the verify mode compares 100% of compute hits as different |
| Bindless images (SOTC_N5_BINDLESS) | not cheaper per op (it still looks every texture up per bind) and 11 frames over 100 ms |
| T# slot cache (SOTC_TSLOT) | 0 lookups: refused by the bind cache recording, storage images and mip tails |
| Buffer memo (SOTC_BUFFER_MEMO, 29 Oct) | 96-99% hits, no gain: the binds it skips were already cheap |

A cache has to prove, for each op, that its stored answer is still right; the proof costs most of what it saves,
and the work around the binding (freshness checks, transitions, barriers, write bookkeeping) stays. The per-op
model itself is the cost.

## 3. Principle

GCN is bindless by construction: a shader loads its V#/T#/S# descriptors from memory with `s_load`, and those words
carry the guest address, size, stride and format. The console's CPU only points the shader at its user data; it
never builds descriptors per draw.

The port does the opposite: per op the CPU reads every descriptor, finds the host buffer or image, checks its
memory, transitions it and writes a Vulkan descriptor. The modern design gives the work back to the GPU:

- **the shader resolves its own resources** from the descriptor words, as the console's shader does;
- **the CPU acts on events** (memory written, image created or freed), never per op;
- **the draw thread only records**: pipeline (memoized), push the user data, draw.

Target: 0.5-1 us per op on the draw thread, 3-6 ms for a 5,500-op frame, 60 FPS with room left.

## 4. Architecture

### 4.1 Guest memory as one device address space

The guest memory the shaders read is reachable by device address:

- the per-frame arena is imported host memory today (SOTC_PC_RING=8, `SotcGuestMemory`, `LookupGuestView`);
- everything else lives in buffer cache buffers, which already have `eShaderDeviceAddress`;
- a GPU page table maps a guest page to a device address. It exists upstream for DMA reads (`BdaPagetable`,
  `FaultBuffer`: a shader that reads an unmapped page records it, and the CPU maps it for the next submit).

Hot data (meshes read by thousands of draws) must sit in device-local memory, not be read over PCIe from host
memory: the buffer cache keeps those as today; only the binding changes.

### 4.2 Buffers by address

The recompiler emits every guest buffer access as a load or store at
`device_address(V#.base) + offset`, clamped to the V#'s `num_records * stride` in the shader (today the descriptor
range clamps it). The V# words are already in the stage's flat buffer at run time (SOTC_GENERIC_BUFFERS=2 reads
stride and format from them per access), so no V# is decoded on the CPU.

- Formatted (texel) buffers: decoded in the shader from the V# format (the SOTC_GENERIC_BUFFERS=2 decoder), so no
  texel buffer views either.
- Vertex and index buffers: vertex pulling in the shader, indices read in the shader for indexed draws or bound
  once per buffer, indirect arguments read from the arena.
- Stay as descriptors: GDS, shared memory (LDS), the port's own special buffers (flat buffer, clip planes, LOD
  feedback), and devices without 64-bit integers or BDA.

### 4.3 Textures by table

- Every image view the texture cache makes gets a permanent slot in a global descriptor heap when it is created,
  freed when it is destroyed (after the GPU is done with it, as views are freed today).
- A **GPU-side table** maps a T# (its 8 dwords, hashed) to a slot. The shader reads its T# from the flat buffer,
  probes the table, and samples the slot.
- A miss (a T# the CPU has not seen) samples a fallback slot and writes the T# to a feedback buffer, like virtual
  texturing. The CPU reads the feedback once per submit, creates or finds the image, inserts it in the table; the
  next frame draws it. First use of a texture costs one frame of a fallback colour instead of a stall.
- Samplers: S# to sampler slot the same way (SOTC_BINDLESS_SAMP has the heap).
- The texture cache stays: detiling, format conversion, GPU-written images, mip tails. It runs on events (image
  dirty, image written by the GPU, new T# from the feedback), not per bind.

### 4.4 Freshness by event

- CPU writes: page tracking already marks dirty pages. Dirty buffer pages are uploaded in one batch per submit, before
  it, not checked per op. Imported memory needs nothing.
- GPU writes: each shader knows statically whether it writes memory (`Info::writes_memory`); render targets are
  known by the pass. Images over GPU-written memory are marked as today, per pass instead of per op.
- Images dirtied by the CPU are re-uploaded in the per-submit batch.

### 4.5 Barriers

All images stay in `GENERAL` (SOTC_LAYOUT_UNIFIED). A global memory barrier goes at render pass boundaries and after
an op whose shaders write memory (static bit), instead of per-image transitions per op.

### 4.6 The draw thread

Per op: pipeline lookup (memoized; the key no longer depends on bindings), push of the user data registers and a
small per-draw packet (flat buffer address), draw. The prep threads (SOTC_PARALLEL_RECORD) compute the keys ahead.

## 5. Recompiler changes

- Buffer loads/stores: BDA with an in-shader bounds clamp; texel decode from the V# words.
- Image ops: T# probe in the GPU table, then a heap index; the image's type key (2D/array/3D/cube, float/uint,
  sampled/storage) picks the heap array (the key scheme of SOTC_N5_BINDLESS).
- Shaders no longer depend on binding layouts: one module per program variant instead of per layout. This removes
  a class of bugs (the device lost of 4 Nov: permutations of one program with different resource layouts) and
  helps SOTC_PRETRANSLATE.
- Shader cache rebuilt once (CHANGELOG note).

## 6. Stages

Each stage behind its own switch, unset = today's behaviour bit for bit, `=2` a verify mode that runs the old and the
new path side by side and counts differences. A stage becomes default only with 0 differences on the test route,
a correct image, and a measured gain. Measurements: one run per configuration on the test route (see the memory
rule "matrix one round"), per-op cost from metrics.csv (draw_busy_ms / draw_ops), the draw-thread profile, worst
frame and p99.

| # | Stage | Switch | What it removes | Verify | Main risks |
|---|---|---|---|---|---|
| S0 | Measurement harness: per-op cost by phase on the route (SOTC_OP_PHASES, profile), op mix (draw/dispatch, storage, compute) | — | nothing | — | — |
| S1 | Guest page table for every buffer kind, device addresses for all buffer cache buffers and the arena | SOTC_GCN_MEM | nothing yet (infrastructure) | page table vs ObtainBuffer answer per binding | stale entries when buffers move (OnPlace/OnRemove events) |
| S2 | Buffers by address in the recompiler (plain and formatted), bounds clamp in the shader | SOTC_GCN_BUF | BindBuffers, ObtainBuffer, buffer descriptors | shader reads both ways into a debug buffer, mismatches counted | PCIe reads of host-resident data; robustness; Intel BDA speed |
| S3 | View-owned heap slots + GPU T# table + miss feedback | SOTC_GCN_TEX | BindTexturesFull, FindTextureIdentity, texture descriptors | table answer vs FindTexture per bind | mip tails, arrays, cubes; fallback frames visible on first use; heap limits per vendor |
| S4 | Freshness by event: per-submit dirty upload, image events | SOTC_GCN_SYNC | SynchronizeMemory, ReplaceChangedPages, per-op freshness checks | per-op check kept in verify, counts stale reads | a CPU write source not seen (DMA, native copies); readbacks |
| S5 | Static barriers, GENERAL layouts | SOTC_GCN_BARRIER | per-op transitions and barrier checks | validation layers off-game; image compare | hazards between passes |
| S6 | Thin draw thread: keys on prep threads, commit only | SOTC_GCN_COMMIT | the remaining per-op work | SOTC_VERIFY=1 (existing CheckPrepared) | ordering with guest memory changes |

Expected cut per stage (from the profile shares): S2 ~15-20% of op time, S3 ~10-15%, S4 ~10%, S5 ~5-10%, S6 the
rest down to the commit. The 1 us target needs all of them; S2+S3 alone should bring the heavy views under 30 ms.

## 7. Risks and open questions

- **Host memory over PCIe**: imported arena reads are slower than VRAM; per-frame data is small, meshes are not.
  Measure the GPU time of S2 on the heavy views; keep hot meshes device-local.
- **Coherence of GPU-written memory** read back by the CPU (readbacks, native copies): unchanged mechanisms must
  keep working while the binding no longer goes through ObtainBuffer.
- **First-use fallback** in S3 is visible (one frame of a fallback texture). A pre-pass that inserts the T# of a
  draw on the CPU when it is seen for the first time can avoid it at the cost of a lookup per new T#.
- **Vendor limits**: descriptor indexing counts (NVIDIA, AMD, Intel Arc), BDA on Intel; the probe must refuse what a
  device cannot hold and keep today's path.
- **Debugging**: GPU-side resolution hides errors that the CPU path would assert on; every stage needs its verify
  mode and the GPU crash report (SOTC_GPU_CHECKPOINTS) kept working.

## 8. S0 baseline (4 Nov 2026)

Configuration: the sotc.ini of 4 Nov (SOTC_NATIVE=1, SOTC_PC_RING=8, SOTC_GENERIC_BUFFERS=2, SOTC_PRETRANSLATE=1,
SOTC_VS_BINDING_BASE=1, SOTC_BLOCK_EXTENT=1, SOTC_PRELOAD_UPLOAD=2 with age 20 ms, the frame bind cache with the
content rule, SOTC_NATIVE_COPIES_PAGES=1), test route, NVIDIA, 30 FPS cap, prepared shader cache, one run per tool:
SOTC_GCN_CENSUS=1 + SOTC_OP_PHASES=1, SOTC_PROFILE=GpuDraw, SOTC_TRACE_GPU=1. Figures are the steady windows after
the area entry (4 windows of 5 s); the area-entry window is noted apart. The phase probes add ~2 ms per frame.

### 8.1 Ops

| | Share of ops | Cost per op |
|---|---|---|
| Draw | 15-28% | 6.6-7.7 us |
| Draw indirect | 42-46% | 6.3-7.3 us |
| Dispatch | 21-27% | 9.7-10.7 us |
| Dispatch indirect | 7.5-11.5% | 10.2-12.7 us |

- 2,800-3,700 ops per frame on average over the route (4,300-5,400 in the heavy views), draw thread 26-41 ms per
  frame. Real GPU execution of the same frames is 13-20 ms (measured with SOTC_TRACE_GPU on 4 Nov); there is GPU
  headroom for the work the stages move to the shaders.
- **Dispatches are a third of the ops and cost ~50% more each than draws**: every stage must cover compute.
- Shaders that write memory: 40-45% of ops. Storage images: 19-24%. Fmasks: 0. DMA reads: 0.1-0.2%.

### 8.2 Where an op's time goes (SOTC_OP_PHASES, ms per second of play)

| Phase | ms/s |
|---|---|
| bind resources | 383-439 |
| — textures | 67-106 |
| — written buffers | 80-83 |
| — clean buffers | 45-55 |
| — dirty buffers | 2-13 |
| — the rest of bind resources (prologue, caches, hashes, flat buffer copies) | ~185 |
| other (not attributed) | 136-159 |
| HLE shaders | 41-43 |
| begin rendering | 26-33 |
| render state | 23-29 |
| reset | 25-30 |
| descriptor push | 19-23 |
| vertex/index | 14-19 |
| dynamic state | 14-18 |

Draw-thread profile (inclusive, the cap wait excluded): BindResources 32.6%, BindBuffers 11.0%, ObtainBuffer 6.4%,
std::sort 5.7% (native copy bookkeeping and the ring drain), BindTexturesFull 4.8%, TryBindCacheFrame 4.5%,
PopPendingOperations 3.4%, ExecuteShaderHLE 2.9%, ReplaceChangedPages 2.9%, NoteWrittenBind 2.7%.

Bookkeeping per second: 1,360-1,680 dirty syncs uploading 35-95 MB; 62-73k written-range walks.

### 8.3 Resources per op (SOTC_GCN_CENSUS)

| | Per op |
|---|---|
| Plain read guest buffers | 1.9-2.0 |
| Formatted (texel) buffers | 2.5-2.7 |
| Written guest buffers | 0.65-0.9 |
| Port special buffers (flat buffer, clip planes...) | 2.1-2.2 |
| Bound bytes | 2.1-3.1 MiB |
| Sampled images | 3.0-3.5 |
| Storage images | 0.14-0.23 |
| — of the images: mip tails (base level > 0) | 2.05-2.45 |
| — arrays / 3D / cubes | 0.05-0.09 / 0.01 / ~0 |
| Samplers | 0.9 |

- Buffer sizes: 47-49% under 4 KiB, 21-28% under 64 KiB, 13-17% under 1 MiB, 10-16% under 16 MiB.
- **Only 6.5-8.4% of the guest buffers are in the imported arena**: the rest are buffer cache buffers, so S1/S2 need
  device addresses for those, not only the arena.
- **Formatted buffers outnumber plain ones**: the in-shader format decode (SOTC_GENERIC_BUFFERS=2) is required in S2.
- **Most sampled images are mip tails** (~70% of the image binds): S3's table must resolve them (SOTC_TSLOT refused
  them, which is one reason it never served).
- Per frame: 640-800 distinct T# (max 1,039), 8 distinct S#. New T# (never seen in the session): 0.8-3.7 per frame
  in steady play, 19 per frame in the area-entry window. A 4,096-slot heap and a T# table of a few thousand
  entries hold a whole area; the first-use fallback of S3 would show on a handful of textures per frame.

### 8.4 Success criteria per stage

Measured against this baseline on the same route, one run per configuration:

| Stage | Gate to become default |
|---|---|
| S1 | page table answers equal ObtainBuffer's for every binding (verify 0); per-op cost not above baseline +2% |
| S2 | buffer phases (clean + dirty + written, ~130-150 ms/s) and BindBuffers/ObtainBuffer down by 80%; draw and dispatch per-op cost down by at least 1.5 us; GPU frame time up by no more than 3 ms; verify 0 |
| S3 | texture phase (67-106 ms/s) down by 80%; mip tails served by the table; first-use fallbacks counted and under 5 per frame in steady play; verify 0 |
| S4 | dirty syncs and range walks off the per-op path (per submit); no stale read in verify |
| S5 | per-op transitions gone; barriers per frame counted; image compare equal |
| S6 | draw thread under 1.5 us per op on the heavy views |

### 8.5 The time the phase probes do not name

From the draw-thread profile by callee (`tools/profile_functions.py --callees`, `tools/profile_lines.py`), shares of
all draw-thread samples:

| Where | Share | Notes |
|---|---|---|
| Waits | ~20% | 11.7% the 30 FPS cap (the flip arrives as an interrupt inside a "func" back op and waits in GetRenderFrame), 5.5% condition waits, 3.1% sleeps: not targets |
| Ops (draws and dispatches) | ~64% | DrawIndirect 21.4, DispatchDirect 18.2, Draw 16.1, DispatchIndirect 8.4 |
| — BindResources | ~38% | BindBuffers 13.3 (ObtainBuffer inside), **the frame bind cache lookup and replay (TryBindCacheFrame) 8.1**, **BindResources' own inlined code 8.2** (fast-bind and cache prologue, hashes, flat buffer copies), BindTexturesFull 6.3, the rest ~2 |
| — dispatch extras | ~6% | ExecuteShaderHLE 2.9, NoteWrittenBind (native result copies) 2.9 |
| — draw extras | ~11% | BeginRendering 2.6, PrepareRenderState 2.1, UpdateDynamicState 2.2, BindIndexBuffer 1.5, ResetBindings 1.5, descriptor push (Pipeline::BindResources) 1.5 |
| Other CP commands on the draw thread | ~3-4% | label writes (LandLabel 2.5), fence writes, register-state lambdas |
| Per-submit work | ~5% | PcRingCopy::OnSubmitStart 2.6, NativeDrain 2.5 (with its sort) |
| Scheduler pending operations | 3.5% | Scheduler::PopPendingOperations in the back loop |
| Back-op replay | ~1-2% | register chunk copies, user data and flat buffer assignment per op |

So the ~185 ms/s inside bind resources are the frame bind cache itself (about as much as it saves: it is a net
gain of only ~3% per op) and BindResources' prologue; the ~150 ms/s of "other" are the dispatch and draw extras
listed above. Every one of them is per-op validation or bookkeeping that S2-S6 remove; the frame bind cache
becomes unnecessary once S2 and S3 land.

## 9. What already exists and is reused

- Guest memory import: SOTC_PC_RING=8, `SotcGuestMemory`, `PcHostBind`.
- Shader-side addressing: SOTC_SHADER_ADDR, SOTC_N5_BDA_BUFFERS / BdaTable (per-draw tables today: replaced by the
  page table), upstream BdaPagetable + FaultBuffer.
- In-shader V# format decode: SOTC_GENERIC_BUFFERS=2.
- Heaps: SOTC_N5_BINDLESS (image arrays by type key), SOTC_BINDLESS_SAMP (samplers), SOTC_TSLOT (T# hashing).
- Layouts: SOTC_LAYOUT_UNIFIED. Static write bits: `Info::writes_memory` (SOTC_BARRIER_STATIC).
- Prep threads: SOTC_PARALLEL_RECORD, SOTC_PR_RESOLVE.
- Verify patterns: SOTC_VERIFY, SOTC_TEX_BLOCK=2, SOTC_BIND_CACHE_CONTENT=2, SOTC_SHADER_ADDR=2.
- Research notes of 4 Nov (scratchpad, not in the repo): texture slots, BDA crash analysis, packet and parallel
  resolve, RE of how the engine emits draws (PM4 rebuilt every frame; meshes, textures, VB/IB persistent; per-frame
  material, instance and user-data blocks in rings of depth 2-5).

## 10. Implementation status (4 Nov 2026)

S1-S5 are implemented behind their switches (all off by default), built and linked; nothing has run in the game yet.
Contracts: `gpu/shadps4/shader_recompiler/gcn_layout.h`, `gcn_rules.h` and `gpu/shadps4/video_core/sotc_arch/gcn_native.h`;
host parts in `video_core/sotc_arch/gcn_*.cpp`; the T# probe in `backend/spirv/emit_spirv_gcn_tex.cpp`. Every decision
taken during the implementation, the review fixes, what is known not to be covered, and the order to try the stages in
are in [GCN-NATIVE-DECISIONS.md](GCN-NATIVE-DECISIONS.md). Main differences from the plan above: S2 checks residency on
the CPU with a bit test and keeps the GPU miss ring as a safety net (no frames of zeros); S3 keeps the per-op Resolve
by default (the table is proven first, the skip is a measurement switch); S5's own transition drop adds little on top of
the unified layouts, its value is the global writer barrier. S6 is not started.
