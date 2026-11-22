# Direct draw pipeline: design for a non-blocking shadPS4 draw path (SotC PC)

Status: design only. No build, no run. Every time figure below is either a measured value from the
attract benchmark (36 FPS, about 5700 commands per frame, draw thread busy 25-26 ms per frame) or an
estimate marked "est.". Line numbers refer to branch `direct/vulkan` at Release 0.76.2.

## 0. Problem in one paragraph

The PM4 processor (`shadPS4:GpuCommandProcessor`, about 14 ms per frame) hands each draw to the draw thread
(`shadPS4:GpuDraw`) through a ring of `BackOp`s. The draw thread does all resource work for each draw
synchronously: descriptor and push-constant building, texture lookup and layout transitions, buffer lookup and
guest-memory synchronization, page protection, and `vkCmd*` recording. That is 25-26 ms per frame, and the
command processor also waits on it (`Liverpool::WaitBackWrites` at `liverpool.cpp:783`). The goal is a draw
thread that never waits, with the resource work moved to parallel workers and the CPU-memory checks moved to a
sweep thread. The shader output must not change.

## 1. Threads and stages

Target pipeline, one stage per thread, connected by lock-free rings or timeline semaphores:

| Stage | Thread(s) | Input | Output | May block? |
|---|---|---|---|---|
| A. Decode | CP thread (existing `GpuCommandProcessor`) | PM4 stream | `DrawPacket` (register snapshot id, V#/T#/S# words, draw params, dirty epoch) | Only on ring-full |
| B. Resolve | pool of N workers (N = hardware threads minus 3) | `DrawPacket` in order of sequence number | `ResolvedDraw` (pipeline handle, descriptor writes or bindless indices, buffer device addresses, render state, list of `UploadNeed` and `Transition` jobs) | Never on the GPU; short mutexes only inside the caches |
| C. Record | one recorder thread (existing `BackLoop` shrunk) | `ResolvedDraw` in sequence order | `vkCmd*` into the frame command buffer | Never on the host; uses GPU-side waits only |
| D. Sweep | one dirty-sweep thread | dirty-page bitmap, submit boundaries | `UploadJob`s, protection re-arm | Yes, but it is off the draw path |
| E. Upload | one transfer thread | `UploadJob` | staging copies on a transfer queue or the transfer part of the frame | Waits on host staging only |
| F. Readback | one readback thread (existing async readback, `buffer_cache.cpp:459`) | GPU-produced ranges after a timeline point | copies into guest memory | Yes, off the draw path |
| G. Present | existing presenter (`vk_presenter.cpp`) | flip | swapchain | As today |

Ordering rules:

1. **Sequence numbers are the only ordering.** The CP thread assigns `seq` to every draw, dispatch, and state
   event. Workers may finish out of order; the recorder consumes resolved items strictly by `seq` from a
   reorder ring. Render-pass begin/end, barriers, and layout transitions are all recorded from that ordered
   stream, so the command buffer is identical to what the serial path records.
2. **Layout state is single-owner.** `Rasterizer::RefreshImageLayouts` and the transition logic
   (`vk_rasterizer.cpp:3879` `BindResolvedImage` second pass) mutate per-image state. Workers do lookups only
   (`FindImage`, `FindView`, read-only). Transitions are emitted as `Transition` records and applied by the
   recorder, which owns the layout state. Workers never write image layout fields.
3. **Render-pass structure stays on the recorder.** `PrepareRenderState` (`vk_rasterizer.cpp:1073`) and
   `BeginRenderingFast` decide the render state in the resolver, but the `vkCmdBeginRendering` call and the
   hudless/UI-shadow special cases (`vk_rasterizer.cpp:1814`, `:1941`) remain recorder decisions that are
   triggered by a flag in the packet. The flag is computed from the same register snapshot, so the decision
   is the same.
4. **GPU-written memory.** Render targets, storage writes and shader-written buffers are tracked by the
   existing `AddGpuModified` / `MarkWrittenByGuest` logic (`buffer_cache.cpp:309`, `:439`). The resolver only
   records the ranges. Dependencies between a producing draw and a later consumer are expressed as a
   timeline value on the producing submit, and the consumer's recorder emits a GPU-side
   `vkCmdWaitSemaphores` or a barrier. The host never waits for a GPU result on the draw path.

## 2. Bindless model

### 2.1 What exists today

- Descriptor sets are built per draw. `Pipeline::BindResources` (`vk_pipeline_common.cpp:28-60`) pushes
  constants, then either calls `pushDescriptorSetKHR` (when `uses_push_descriptors`, line 46-47) or commits a
  heap set and calls `updateDescriptorSets` (lines 56-59). The set layout is built in
  `vk_graphics_pipeline.cpp:563-612`; it is a push-descriptor layout when the binding count is below
  `MaxPushDescriptors` (line 602).
- The recompiler emits one descriptor binding per resource. `Shader::Backend::Bindings` is two counters
  (`backend/bindings.h:10`: `unified`, `buffer`). `spirv_emit_context.cpp:849` decorates each variable with
  `binding.unified`; buffers are numbered at line 908; image variables are typed at lines 1042-1061.
- Buffers use SSBO-style bindings bound from `BindBuffers` (`vk_rasterizer.cpp:3572`). A physical-storage
  pointer already exists for the page table (`spirv_emit_context.cpp:1248`, capability at
  `emit_spirv.cpp:474`).
- `vk_instance.cpp:596-604` enables `timelineSemaphore`, `bufferDeviceAddress` and `synchronization2`.
  The descriptor-indexing features (`descriptorBindingPartiallyBound`, `runtimeDescriptorArray`,
  `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingSampledImageUpdateAfterBind`) are not in that
  block. They are core in Vulkan 1.2 and therefore allowed by the Vulkan 1.3 core-only rule, but they must be
  added and checked against `VkPhysicalDeviceVulkan12Features` support on every target GPU.

### 2.2 Target model

- **Buffers: buffer device addresses (BDA).** The resolver writes each buffer's device address into the draw's
  push data (or a per-frame SSBO of addresses). The shader reads through a `PhysicalStorageBuffer` pointer
  with the same offset arithmetic that the SSBO path uses. The guest VA-to-arena mapping in `BufferCache`
  (`buffer_cache.cpp:1938`, arenas from `address_space`) is stable, so the address is valid for the life of
  the arena.
- **Images and samplers: one global descriptor heap.** One `VkDescriptorSet` with large arrays:
  `SAMPLED_IMAGE[N]`, `STORAGE_IMAGE[N]`, `SAMPLER[M]`, bound once per frame and never rewritten during a
  frame (new images are written into free slots on the upload/sweep thread between frames or at a timeline
  point). The shader accesses `textures[idx]`, where `idx` comes from the T# word resolved by the resolver
  (push data or a per-draw index buffer). `NonUniformEXT` is required because the index is per draw.
- **Draw parameters.** Per-draw index data lives in a per-frame index buffer (one `uint` array); the push
  constant carries the offset into it. Multi-draw indirect uses the existing `DrawIndirect` path
  (`vk_rasterizer.cpp:2220`) with the count buffer from `ObtainBuffer` (`vk_rasterizer.cpp:2375-2381`).

### 2.3 Shader backend change

Changes are confined to `shader_recompiler/backend/spirv`:

1. `spirv_emit_context.cpp:1042-1061`: when the bindless flag is set (a `Shader::Profile` bit, so the same
   translation can be compared with the legacy one), image and sampler variables are declared as runtime
   arrays in the global set, and the access is `OpAccessChain` with the index value.
2. `spirv_emit_context.cpp:849`: the decoration uses the global set number and a fixed binding per array,
   not `binding.unified`.
3. `backend/bindings.h`: nothing new is needed for the legacy path. A `Bindings` field for the bindless array
   base is added only when the flag is on.

Why the image stays identical: the sampling instructions, the coordinate and LOD arithmetic, the format
conversions and the NaN/denorm handling are unchanged; only the source of the image and sampler handle
changes from a fixed descriptor to an indexed one. The rendered result is identical as long as the index
selects the same image that the legacy binding selects. The check is a frame hash on the attract scene and
the shrine scene, not a visual inspection only.

The SPIR-V changes, so the user's shader cache is rebuilt once. Per the project rule this must be stated in the
CHANGELOG entry ("O cache de shaders e refeito na primeira abertura").

### 2.4 Fallback if bindless is too invasive

Persistent descriptor sets per pipeline layout. Each pipeline layout owns a ring of descriptor sets; the
resolver fills a set's `VkWriteDescriptorSet` array off-thread (the write data is already built as
`DescriptorWrites`, `vk_pipeline_common.cpp:28`), and the recorder only calls `vkCmdBindDescriptorSets`. A set
is reused when its contents match (hash over the resolved image/buffer handles). This removes the
`updateDescriptorSets` call from the draw thread but keeps one descriptor write per changed slot. It is the
safe step-one even if bindless is later adopted.

## 3. Memory tracking without blocking

### 3.1 What exists today

- Page protection is per guest page through `PageManager` (`page_manager.cpp:333` `UpdatePageWatchers`;
  `:657` `SignalImpl::Protect`, which calls `AddressSpace::Protect`). A protected page that the CPU or the
  GPU touches faults; the fault handler (`:866` `GuestFaultSignalHandler`) marks it dirty.
- Per draw, `BufferCache::ObtainBuffer` (`buffer_cache.cpp:1273`) calls `SynchronizeMemory` (`:1296`,
  definition `:1938`), which uploads dirty ranges and re-arms protection. `SynchronizeDmaBuffers` (`:1628`)
  does the same for DMA runs. `PageManager` marshals protection changes to the draw thread with
  `RunOnDrawThread` (`page_manager.cpp:880`, `vk_rasterizer.cpp:6211`).

Profile: page protection about 15% of the draw thread (`SignalImpl::Protect`, `UpdatePageWatchers`,
`AddressSpace::Protect` syscalls), buffer sync 9.5%, `ObtainBuffer` 8.9%.

### 3.2 Target model: epoch snapshot plus async sweep

1. **Epoch snapshot on the CP thread.** Each `DrawPacket` carries `dirty_epoch`, the value of a global
   counter incremented at every guest-visible submit boundary (flip, `sceGnmSubmitDone`, and any
   `WaitBackWrites` point). Reading the counter is one atomic load.
2. **Sweep thread.** At each epoch boundary the sweep thread reads the dirty bitmap accumulated by the fault
   handler and by the write-watch, merges it into per-64 KiB pages, and computes the set of `UploadJob`s
   needed by the epoch. Protection is re-armed here, in batches, with one `Protect` call per contiguous run
   (`ReapplyProtection`, `page_manager.cpp:283`, is already a run-based design). The syscalls are no longer on
   the draw thread.
3. **Resolve checks the epoch, not the pages.** A resolver that needs a range records
   `UploadNeed{range, epoch}`. It does not protect or copy anything. If `epoch` is not yet swept, the resolver
   parks the packet and moves to the next one (no host wait; the sweep thread publishes a `swept_epoch`
   counter and the reorder ring releases parked items).
4. **Upload thread.** Each `UploadJob` copies from guest memory into a staging buffer and records a transfer
   into the frame's transfer command buffer. It signals the upload timeline with the job's value.
5. **Recorder waits on GPU.** Before recording a draw that depends on an upload, the recorder emits
   `vkCmdPipelineBarrier2` or a semaphore wait on the upload timeline value. The wait is GPU-side.

### 3.3 Correctness contract

- **Contract C1.** A draw observes every CPU write that completed before the draw's packet was enqueued by the
  CP thread. This is the contract the legacy path gives in practice: the per-draw `SynchronizeMemory` runs
  after the CP has already decoded the packet, so writes that landed before enqueue are seen.
- **Contract C2.** A CPU write that lands after the enqueue of draw D but before D executes on the GPU may or
  may not be seen by D. The legacy path is not stricter than this, so the image does not change.
- **Contract C3.** A GPU write (render target, storage write) followed by a CPU read is ordered by the
  readback timeline (`buffer_cache.cpp:459` `BeginAsyncReadback`, `:636` `DownloadMemory`). The CPU fault that
  reads the page waits on that timeline; this wait is on the faulting guest thread, never the draw thread.
- **Contract C4.** Protection on a page is always re-armed after an upload of that page, before the next
  epoch's packets are resolved. The sweep thread enforces this by processing the upload list before the
  protection list for the same epoch.

## 4. Where today's costs go

Draw thread today: 25-26 ms per frame. Shares from the inclusive profile:

| Today (draw thread) | Share | ms per frame (est. from the share) | New stage |
|---|---|---|---|
| `Rasterizer::BindTextures` | 16.2% | 4.1 | B resolve (lookup); C record (transition, `vkCmd`) |
| `BufferCache::SynchronizeMemory` | 9.5% | 2.4 | D sweep and E upload (off the draw thread) |
| `BufferCache::ObtainBuffer` | 8.9% | 2.3 | B resolve (address only, no sync) |
| `TextureCache::FindImage` (rasterizer call sites `vk_rasterizer.cpp:4233`, `:4353`) | 6.2% | 1.6 | B resolve (read-only lookup) |
| ImageView and `FindView`, push descriptors | part of the 41.8% `BindResources` | est. 4 | B resolve (descriptor writes built off-thread) |
| Page protection (`SignalImpl::Protect`, `UpdatePageWatchers`, syscalls) | about 15% | 3.9 | D sweep (off the draw thread) |
| Waits: `WaitBackWrites` (`liverpool.cpp:783`), `Scheduler::WaitSubmitted` (`vk_scheduler.cpp:91-106`), recorder `sync_done` (`vk_recorder.cpp:847-851`), ntdll | about 30% | 7.5 | removed: stages communicate through rings and timelines |
| `vkCmd*` recording, `pipeline->BindResources` push and bind | remainder | est. 3 | C record only |

`BindResources` as a whole is 41.8%, about 10.9 ms; that is the largest single item and it moves to
workers (B). The 30% of samples in ntdll (waits and syscalls) disappears in two ways: the waits are removed
by the rings, and the protection syscalls are batched on the sweep thread.

Expected draw-thread (recorder) time per frame, est.:

- Step 1 (resolve on workers, serial recorder): 25.5 ms to about 9-10 ms.
- Step 2 (sweep off the draw thread): about 5-6 ms.
- Step 3 (bindless, BDA, no per-draw descriptor writes): about 3-4 ms for about 5700 commands.

At that point the command processor (about 14 ms, decode only after this change) becomes the frame limit. The
expected frame time is then about 14-16 ms, which is 60-70 FPS on this scene, and the next target is the CP
thread. These are estimates: the CP share will be re-measured at each step.

## 5. Migration plan

Each step is a switch with the legacy path as default until the step is verified. Each step is measured with
the attract benchmark (`tools/sotc_bench_attract.sh`) for FPS and per-thread ms (`SOTC_PROFILE`, and the split
timers in `SotcStats`), and verified with a frame hash comparison against the legacy path at a fixed frame on
the attract and shrine scenes. Identical hash on the attract scene is the acceptance test; a mismatch is a bug
to fix, not a tolerance.

| Step | Change | Switch (new) | Acceptance |
|---|---|---|---|
| 0 | Baseline: record per-thread ms for CP, draw, recorder, sweep (SotcStats split timers) | none | Numbers match section 4 within 10% |
| 1 | Move descriptor-write construction out of `Pipeline::BindResources` (`vk_pipeline_common.cpp:28`) into a pure function that returns `DescriptorWrites` | none (refactor) | Hash identical, FPS unchanged |
| 2 | Resolve pass on a worker pool for lookups (`FindImage`, `ObtainBuffer` address only), recorder still in order and synchronous with a small reorder ring | `SOTC_DRAW_PIPELINE=staged` (default `legacy`) | Hash identical; draw thread drops to about 9-10 ms |
| 3 | Sweep thread: epoch snapshot on enqueue, protection and `SynchronizeMemory` moved to the sweep and upload threads, `UploadNeed` parks packets | `SOTC_DIRTY_SWEEP=1` | Hash identical on shrine (CPU-written buffers); no page fault storm in `SOTC_POOL_WATCH` |
| 4 | Remove host waits from the draw path: `WaitBackWrites` replaced by epoch wait on the reorder ring; upload dependencies as GPU-side waits | part of step 3 | `vk_recorder` `sync_done` and `WaitSubmitted` absent from the draw thread profile |
| 5 | Persistent descriptor sets per pipeline layout (section 2.4), written off-thread | `SOTC_DESC_RING=1` | Hash identical; `updateDescriptorSets` absent from recorder |
| 6 | Buffer device addresses for vertex, index and storage buffers | `SOTC_BDA=1` | Hash identical on attract; SSBO path still builds |
| 7 | Global bindless heap for images and samplers; recompiler flag; enable descriptor-indexing features | `SOTC_BINDLESS=1` (shader cache rebuild) | Hash identical; CHANGELOG note on shader cache |
| 8 | Readbacks on their own thread with timeline gating (`buffer_cache.cpp:459`, `:636`) | `SOTC_READBACK_THREAD=1` | Hash identical on pools and shrine; no guest-thread stalls in `SOTC_SLOW_OPS` |

Rules that apply to every step:

- Default-on only after the step passes its acceptance test on both the attract and shrine scenes. When it does,
  it goes to `DEFAULT_UPGRADES` in the launcher so existing installs get it.
- A step that measures worse gets its cause found and fixed, not switched off (project rule).
- The fallback for any step is the legacy path, selected by its switch, until step 8 is complete.

## 6. Risks

- Reorder-ring stalls: if the sweep thread lags, resolvers park packets and the CP ring fills. The ring size
  must be measured at step 3; the CP thread must not block on a full ring in steady state.
- Thread safety of the caches: `FindImage` and `ObtainBuffer` are used from workers only in read mode.
  Anything that creates images or buffers stays on the upload thread or the sweep thread.
- Driver shader-cache growth from bindless variants (see `driver-cache-bloat` in memory): keep one pipeline
  per layout, not one per index.
- Hook and patch interaction: `sotc_modes.py` patches and `gpu/sotc/sotc_patches.cpp` run on the CPU side and
  must not depend on draw-thread timing; check them at step 3.
