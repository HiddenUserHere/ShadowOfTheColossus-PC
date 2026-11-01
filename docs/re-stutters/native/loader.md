# N0 inventory: resource loading, streaming, textures, render targets (specialist: loader)

Status: partial. Read-only; no code or engine files changed; nothing committed. Base code: main tree
`C:\Users\ptr\Documents\sotc-pc`. Labels: **observed** (decompilation or code with address), **reported**
(earlier report, not re-checked here), **inferred**, **unknown**.

## 1. Path: file read to GPU-ready data (observed chain)

| Step | Function (eboot) | What it does | Bytes end up |
|---|---|---|---|
| Request | `0x14cf20` ResourceWrapper request | Puts the resource in one of 10 priority queues (state 0 goes to queue 0 = StreamingTexture) | n/a |
| Dequeue, job setup | `ProcessLoadingThreadLoop` `0x148600` (3123 B) | Picks jobs (up to 10 per pass), `0x146ea0` PrepareNextResourceToLoad, `0x149c80` / `0x149df0` job slot (0x400 slots, 256 B each) | n/a |
| File read | `0x14b800` StartLoadJob, `0x14b980` LoadFileInternal | Fios read (`sceFiosFHPread` at `0x31830`) into a **CPUMemory (heap 1)** buffer from `0x12820(size, heap 1, align entry+0x54)` | CPU heap, transient |
| Decompress | `0x1aa2a0` (Oodle via `0x89d3b0`) | Output into a second heap-1 buffer when `entry+0x70 & 8` | CPU heap, transient |
| Build | `0x147160` CreateResource, then `0x144880` calls `[entry+0x78]` | Mesh `0x1264d0`, texture `0x98aa0`, particle texture `0x10e9b0`, model `0x110630` | see below |
| Publish (sync point) | `0x14d670` | Writes state 0xb at `[res+0x98]`, object at `[res+0x20]`, moves job to completed list under `[obj+0x60]` mutex | completion posted |
| Drain | `0x1480f0` SyncPointProcessPendingResources (1203 B) | Pops completed list, calls per-node callback (`[node+0x70]`, `[rax+0x38]`), **no item or time budget** | runs on the thread that calls the barrier |
| Barrier | `0x1469d0` WaitForAllCmdsToFinish | Loops `0x147f40` HasPendingWork (10 queue counts + 4 lists) and calls `0x1480f0` | game main thread at frame end (GameLoop `0x25cb12`, `0x3c5a42`) |

Consequence (observed in code, already reported): the frame end waits for the entire loader queue, and the
drain of a burst of completions lands in one frame. The bytes are final in guest CPU memory before publish.

### Build copies (where the GPU-visible bytes are written)

Build functions copy the blob into a **GPUMemory (heap 2)** allocation; the GPU reads that copy through the V#/T#.
The blob is never read by the GPU and never patched after build (reports3/03, re-checked for mesh and texture).

| Resource | Allocation | Copy | Size source | Per-frame? |
|---|---|---|---|---|
| Mesh VB | `0x8be60(obj+0x20, kind 6, stride, count, flags 0, src)`, GPUMemory via `0x12820`, align 0x10 (decomp `0012/001264d0.c:557-559`) | `memcpy` inside `0x8be60`; `0x8e0d0` gives zero-copy src when aligned | VB size | no, built once |
| Mesh IB / second buffer | `0x8be60` kind 0x18/0x19 (decomp line 669) | same | IB size | no |
| Texture, single block | `0x96680` → `0x12820(size, GPUMemory, align 0x100)` (decomp `0009/00096680.c`) | `0x8de50` copies texels from the stream | texel size | no |
| Texture, mip path | `0x12950(0, size, 1, family 0x8c370/DAT_02ed1f60, 0x100)` | mip copies (`0x98ae0`, memcpy at 0x98d67) | per mip | **streaming**, see 3 |
| Model | no GPU allocation, CPU metadata only | n/a | n/a | n/a |
| Particle texture | `0x10e9b0` calls `0x96680` (not hooked) | same as texture | n/a | no |

Note: `0x8be60` has 106 callers and `0x12820` many more. Only the resource builds (mesh/texture under a build
depth) are inventoried; the other callers of `0x8be60` are per-frame and out of this loader inventory.

### Where it breaks: the upload happens at first bind, not at ready

Measured in `user/trace/20261101-085933/hitch.md` (1 Nov, Lab route + free play):
- Line 7: a frame with **sync copy by caller: bind 138.9 MiB, dma 26.0 MiB** (the area-load frame; the figure in
  the brief). Other area-like frames: bind 51.9 and 50.1 MiB (lines 224, 257).
- Steady frames: dma 2-12 MiB, bind 0-6 MiB.

So the port does not upload the resource when the loader publishes it. The port's page tracking (classify /
protect, `SOTC_STATIC_CLASS`) marks the GPUMemory copy as CPU-clean, and the upload runs on the draw thread at the
first bind (`bind` column). The "63% written earlier and bound for the first time" split quoted in the brief
was **not found** in any file under `docs/re-stutters` or in `hitch.md`; it needs the per-range data from the
`SOTC_TRACE` sync-copy log (open question 1).

## 2. Port-side code that touches this path

- `gpu/sotc/static_class.cpp` + `scripts/hooks.d/staticclass.py`: GPU_STATIC classification of fresh build
  allocations. Hook sites: mesh build entry `0x1264d0` (wrap, depth counter), texture build `0x98aa0` (wrap),
  buffer alloc site `0x8bf0b` in `0x8be60` (site, mesh/tex depth gate), texel alloc site `0x96b48` in `0x96680`.
  Modes: 1 apply (page classification), 2 count, 3 apply + skip per-bind scan for clean mesh ranges. **This is
  tracking only: no upload is announced. Its output is `pages_clean` / `pages_gpu`, and the bind still uploads.**
- `gpu/sotc/probe_loader.cpp` (319 lines): loader probe (not read in detail here).
- `gpu/sotc/area_prewarm.cpp`: per-area **image manifest** (`user/cache/area_manifest/<area>.bin`), replayed on
  re-entry, not zone activation. The first visit in a session has no manifest and is not covered.
  The name "zone" does not appear in `gpu/sotc` as a loader concept; the streaming zones are the area load's
  `0x533100` AreaLoad_PrecacheLists (reports2/03).
- Texture bank / LOD stats: `gpu/shadps4/video_core/sotc_lod_stats.h` exists (the brief's file). Not read in
  detail in this pass.
- Render targets: `gpu/sotc/rt_class.cpp` (`SOTC_RT_CLASS`: RT blocks not downloaded and not CPU-tracked, 1 =
  apply). RTAllocator `render_targets_allocate` `0x9aef0`: 322 MiB, 19,928 allocations (PC-MEMORY-REWRITE.md).
  Not traced here.
- File IO: Fios threads `SceFiosIO` (reads) and `SceFiosCallback` (completions), `src/runtime_file.c:380-415`
  (`do_read` / `do_pread`). `SceFiosZlib0` is inside the linked Fios2 PRX (reports2/09).

## 3. Streaming textures (open, partly reported)

- `0x98ae0` streaming window (`0x12950` decommit mode 2 on mip drop). Mip copy: memcpy at `0x98d67`.
- `TextureManager` update `0x13ccd0` runs every frame from the render job (reports2/03): walks the active texture
  slots and calls `0x13a3e0`; a ready texture (`0x14cf00`) gets its mip copied by `0x98ae0`. **No global cap found.**
  Cvars `Num Mip Requests` / `Max simultaneous load requests` have **no reader** in the eboot (reports2/03): setting
  them may do nothing.
- Unload path: `DeleteResource` `0x1478d0` and the destructor `0x146770` call the full `WaitForAllCmdsToFinish`.
  **Open:** whether these run during gameplay streaming (decides whether an unload waits for pending loads).

## 4. Proposed PC-native mechanism (loader and textures)

### 4a. Buffers and meshes: announce on ready, upload once on workers

- **Patch point:** the publish `0x14d670` for mesh and texture objects whose type is built into GPU_STATIC copies.
  At publish the decoded blob is final and the GPUMemory copy is complete (the copy is inside the build job, which
  finishes before publish, and publish is the happens-before edge).
- **Call:** `sotc_native.NativeWrite(addr, size)` for each GPU copy (`0x8be60` allocation `[obj+0x10]`, size
  `[obj+0x20]`), queued to an upload worker. The worker copies from the guest GPU copy into a device-local buffer
  (or staging then transfer) and the frame reads the device buffer, not the guest address.
- **Why this is correct:** the GPU copy is never written after publish (reports3/03: no fixups, no in-place
  patching). The only later writer is texture streaming (3), which is a separate class.
- **Remove from the bind path:** once announced, the bind for these ranges skips the page scan and the sync copy
  (this is the `bind 138.9 MiB` class). Verify mode: a CPU write after publish must count as an error.
- **Budget:** the publish drain (`0x1480f0`) must not upload. Pace the workers (the existing 32 MB
  `SOTC_UPLOAD_BUDGET_MB` pacer is a reference).

### 4b. Textures: create the image at build, upload at ready

- **Create:** the VkImage can be created where the engine creates the texture object (`0x98aa0` entry hook, or
  the T# write `0x881e60`), but format and size are known only after the header is parsed inside `0x96680`. Create
  at the `0x96b48` site (single block, size known there) and keep the image with the T# base.
- **Upload at ready:** same as 4a, `NativeWrite(base, size)` at publish for single-block textures. Mip-streamed
  textures (0x12950 path) need per-mip announces from `0x98ae0` (memcpy at 0x98d67) and stay on the streaming
  pacer.
- **Mismatch risk:** the image must be created with the mip count and format of the header; if the header is
  read only in the build, creating at `0x98aa0` entry is not possible for format. **Unknown until the format read
  site is pinned** (open question 4).

### 4c. Render targets (RTAllocator, 0x9aef0)

GPU-written, not uploaded. Under the native model they are created when the engine allocates them and never
uploaded; the CPU never reads them (no readback in the loader path). `SOTC_RT_CLASS` already drops tracking on
them. Nothing to announce except the allocation itself.

## 5. Counts (this pass)

| Class | Sites | Notes |
|---|---|---|
| Loader sync points (publish, drain, barrier) | 3 (`0x14d670`, `0x1480f0`, `0x1469d0`) | drain has no budget |
| Build copies to GPU (mesh/texture) | 4 (`0x8be60`, `0x8de50` in `0x96680`, `0x8e0d0`, `0x98ae0` mip copy) | copy then first-bind upload |
| CPU-only transient (blob, decompress) | 3 (`0x14b800` read, `0x1aa2a0` decode, `0x12820` heap 1) | freed at CreateResource end (`0x8dc30`) |
| Streaming (per frame) | 1 (`0x13ccd0` update) + `0x98ae0` | no cap found |
| RT allocations | 1 (`0x9aef0`) | GPU-written |

## 6. Open questions (what would resolve them)

1. The 63% split of the 139 MiB frame: log per sync copy the range's first-bind flag and whether it was written
   by a build before (`SOTC_TRACE` sync-copy records with address and caller). Needs a session.
2. Trace the file record's type word to the registry key (`PrepareNextResourceToLoad` 0x146ea0): decides which
   build runs for a given file.
3. Whether `DeleteResource` (`0x1478d0`) runs during gameplay: log `0x1469d0` entry with caller during a session.
4. Texture format: the header read site inside `0x96680`/`0x98aa0`, to know when the image can be created.
5. `sotc_lod_stats.h` texture banks: not read; needed for mip-streaming sizes.
6. `0x12950` family name (`VirtualMemoryGPU`) still unverified (reports3/03).

Not done in this pass (time): full reads of `probe_loader.cpp`, `sotc_lod_stats.h`, `rt_class.cpp`, the
`0x14d670` decompilation, and `0x98ae0`. The decomp is at `out/re/decomp/0014/*.c`, `0008/0008be60.c`,
`0009/00096680.c`, `0004/000479a0.c`.
