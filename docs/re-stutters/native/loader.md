# N0 inventory: resource loading, streaming, textures, render targets (specialist: loader)

Status: deep pass, second revision. Read-only: no engine or port code changed, nothing committed. Base code: main
tree `C:\Users\ptr\Documents\sotc-pc` (decomp in `out/re/decomp`, index via `tools/re/eboot_re.py`).
Labels: **observed** (decompilation or code, with address), **reported** (earlier report, not re-checked),
**inferred**, **unknown**.

## 0. Summary

- **Publish (0x14d670)** writes the object and state 0xb only. The GPU copy is complete before publish. Ready means
  `state == 0xb && obj != 0` (0x14cf00). Nothing is uploaded or announced at publish today.
- **Mesh and single-block texture** copies go into GPUMemory (heap 2) inside the build job, before publish.
  After publish the GPU copy is never written by the loader.
- **Mip streaming (0x98ae0)** is the only loader-side writer of GPU-visible memory after build. It runs on the
  **render job** (game thread, via 0x13cae0 → 0x13ccd0 → 0x13a3e0), once per mip-level change, as a memcpy from the
  resource's CPU data into the pool. The port sees that memcpy as write faults.
- **The mip level is chosen by GPU feedback.** `GET_LOD_STATS` writes 256 LOD banks to memory, the game reads them,
  and the streamer requests mips from them. This is a GPU→CPU consumer that the native plan must cover (section 5).
- **Texture format and size are final at 0x96680**, before the T# base is set at 0x881e60 (0x96b8b). An image can
  be created there (section 3).
- **Type dispatch**: the registry key is the **last four characters of the resource name** (0x14c870), not a type
  word in the file record. Three registry build functions allocate GPU memory (mesh, texture, particle texture).

## 1. Path from file read to GPU-ready bytes (observed)

| Step | Function (eboot) | What it does | Bytes end up |
|---|---|---|---|
| Request | `0x14cf20` | Puts the resource in one of 10 priority queues. Request code 0xd maps to the stored state, or 5 | n/a |
| Loop | `0x148600` ProcessLoadingThreadLoop (3123 B) | Picks up to 10 jobs per pass; `0x146ea0` PrepareNext; job slots (0x400, `shl 8`) via `0x149c80`/`0x149df0` | n/a |
| Prepare | `0x146ea0` (681 B) | Finds the job's registry descriptor (section 4); `0x14aee0` (2282 B) sets `job[1] = descriptor` and the load state | n/a |
| Read | `0x14b800` StartLoadJob → `0x14b980` LoadFileInternal | Fios read into a **CPUMemory (heap 1)** buffer, `0x12820(size, heap 1, align entry+0x54)` | CPU, transient |
| Decode | `0x1aa2a0` (Oodle via `0x89d3b0`) | Output in a second heap-1 buffer when `descriptor+0x70 & 8` | CPU, transient |
| Stream | `0x4d470` → `0x8db10` | Stream object owns the blob (`+0x120`), cursor `+0x108`, end `+0x110` | CPU |
| Build | `0x147160` CreateResource (813 B) → `0x144880` (731 B) calls `[descriptor+0x78](&{handle, stream, size}, &out)` | Type-specific build (section 4) | GPU copy (section 2) |
| Publish | `0x14d670` (306 B) | Writes state 0xb at `[res+0x98]`, object at `[res+0x20]`; if `[res+0x40]==0` it notifies the loader (vtable `*DAT_02ee2d00+0x28`); else it pushes the completion via `[res+0x68]` vtable `+0x40` or `+0x20` (not resolved) | completion queued |
| Drain | `0x1480f0` SyncPointProcessPendingResources (1203 B) | Pops completed nodes under the `[obj+0x80]` spin lock; calls per-node callbacks `[node+0x70]`, `[rax+0x38]`; **no item or time budget** | runs on the barrier's thread |
| Barrier | `0x1469d0` WaitForAllCmdsToFinish (362 B) | Loops `0x147f40` HasPendingWork (10 queue counts + 4 lists) and calls `0x1480f0` | GameLoop end (`0x25cb12`, `0x3c5a42`) |

Ready predicate (observed, `0x14cf00`): `state == 0xb && obj != 0`. Loader state `0x60`: 1 = in progress, 2 =
done-with-no-object, 5 = failed. Failed creates log "Failed to create resource %s".

## 2. Every GPU-visible write in the load path

| # | Site | Writer | Address source | Size | When | Class |
|---|---|---|---|---|---|---|
| W1 | Mesh VB/IB, `0x1264d0` → `0x8be60` (decomp 0012/001264d0.c:557-559, 669) | `memcpy` inside `0x8be60` from the stream (zero-copy source `0x8e0d0` when aligned) | GPUMemory via `0x12820` (align 0x10) | VB/IB size | build job, once | GPU_STATIC |
| W2 | Single-block texture, `0x96680` (decomp 0009/00096680.c, `0x12820(size, GPUMemory, align 0x100)`) | `0x8de50` copies texels from the stream | GPUMemory | texel size | build job, once | GPU_STATIC |
| W3 | Particle texture, `0x10e9b0` → `0x96680` | same as W2 | GPUMemory | texel size | build job, once | GPU_STATIC |
| W4 | Mip stream, `0x98ae0` (693 B), memcpy at 0x98d67 | `memcpy(pool_top, resource data (0x112b30 of 0x14d7b0(res)), mip size)` | pool (`[param_1+0xb8]`) via `0x12950` mode 0 (commit) | mip size | render job, per mip-level change | streaming, GPU-visible |
| W5 | Mip decommit, `0x98ae0` (param_2 < current) | `0x12950(top, size, 2)` (decommit), no copy | pool | mip size | render job, on mip drop | streaming |
| W6 | T# base, `0x881e60` (1665 B) | writes the base into the T# words (not data) | T# at `obj+0xc4` | 8 words | build, and per mip | descriptor |

Notes on W4 (observed):
- It runs only when the target mip (`param_2`) is above the loaded level `[param_1+0xc0]`, and only when the mip's
  resource is ready (`0x14cf00`). Each change is one copy; a ready mip is not copied again.
- The source is the decoded data of the resource (`0x14d7b0` gives the resource, `0x112b30` its data pointer). That
  data was built on the loader thread and is CPU-only.
- While not ready, `0x98ae0` requests the resource each call (`0x14cf20(res, 0xd)`), so the request traffic is
  per frame per texture.
- After the copy it sets `[param_1+0xc0] = param_2` and calls `0x112d50` on the drop path (not read).

Nothing else writes GPU memory after build: the reports3/03 absence check holds for mesh and texture.

## 3. Texture format and size: where the image can be created (observed)

The header is read sequentially from the stream by `0x8eae0` (u32) and `0x8ea20` (u16), so the order below is the
file order:

1. `0x96680` reads `a` (u32), then `b` (u32). `a` is the format index. It is used as `DAT_02ed3680 + a*8` for the
   format word passed to `0x87dea0`. `b < 2` selects the single-block path; `b >= 2` selects the mip-tail path.
2. The texture object is created by `0x112430` (117 B, 0x1c0 B object), with flags at `+0x61..+0x63`.
3. Single block: `0x12820(size, GPUMemory, 0x100)` and copy (W2). Mip tail: `0x88f000` per part, `0x880c30` sets the
   mip count/size fields at T# word `+0xc` (bits 16-19 and 12-15), `0x12950` for the mip block, copy.
4. T# words: `0x8829e0(T#, 0x6e)` sets word 1 and word 3 bits (fixed value); `0x87dea0(T#, format)` sets the
   format word from the table entry; `0x87e5f0(T#+0xe4, T#, 0, mips, 0, 0)` and the same at `T#+0x124` fill the
   width, height, depth and mip fields from the T# itself (1299 B, decodes the GCN data format).
5. `0x881e60` sets the base (`base >> 8`) at `0x96b8b`.

**Decision:** the format, mip count, width, height and base are all final once `0x87e5f0` has run for the
T# at `obj+0xc4`, which is the point just before the `0x881e60` call. The port can read the T# words there
(hook site: the `0x881e60` entry, which already exists as the GPU range hook in reports3/03) and create the
VkImage then, with the existing port decoder (`texture_cache`). The image is then tied to the T# base, and the
upload waits for publish (section 5). **Caveat:** the port decodes T# words at bind today. Moving that decode to
`0x881e60` is a change in the port's texture path, not in the engine.

Mip-streamed textures (the `0x12950` family) have their own format setup per mip; their image should be created
from the T# with the mip count of the streamer's maximum level, which the port does not know until the requests
settle (unknown).

## 4. Type dispatch: from resource name to build function (observed)

- The key is `FUN_0014c870(name)` (45 B): the **last four characters of the resource name**, read as a
  big-endian u32 (a file-extension FourCC). So the type is set by the name, not by a type word in the record.
- `0x146ea0` looks that key up in the loader's tree at `[loader+0x20]` (keyed by the FourCC, u32 at `+0x20`), and
  passes the matching node (`node+0x30` = descriptor) to `0x14aee0`, which stores it as `job[1]`. If not found, the
  loader's tree at `[loader+0x40]` is tried; otherwise state 6 or 7 (error).
- Registration: `0x1444e0` (306 B) with the FourCC in esi, called from every `ResourceFactory` registration (about
  30 callers including `0x127860` and `0x112730`). Its insertion into the loader's tree was **not traced**; the
  descriptor layout is confirmed (`0x144640` sets name `+0x58`, build fn `+0x78`, alignment `+0x54`, flags `+0x70`).
- The build function is `[descriptor+0x78]`, called by `0x144880` with the stream and the output object.

Registry build functions and their GPU allocation calls (direct calls in the function body; helpers not followed):

| Type (FourCC, esi) | Build fn | GPU copy? | Evidence |
|---|---|---|---|
| Mesh ("hsmc" = 0x636d7368) | 0x1264d0 | **yes**, `0x8be60` (W1) | 2 calls to 0x8be60 |
| Texture ("rxtc" = 0x63747872) | 0x98aa0 → 0x96680 | **yes** (W2, mip path via 0x12950) | 0x98aa0 calls 0x96680 |
| ParticleTexture ("xtpc" = 0x63707478) | 0x10e9b0 → 0x96680 | **yes** (W3) | 0x10e9b0 calls 0x96680 |
| Model ("ldmc" = 0x636d646c) | 0x110630 | no direct allocation (CPU metadata) | no GPU call in body; reports3/03 |
| TextureStreamingChunk ("cxtc" = 0x63747863) | 0x1129a0 | not direct (0x88-byte object) | no GPU call in body |
| Material ("tamc" = 0x636d6174) | 0xf9390 | not direct | no GPU call in body |
| Shader ("rdsc" = 0x63736472) | 0xb0d4a0 | not direct | no GPU call in body; shader binaries are the GPU's, not checked |

"Not direct" means no call to 0x8be60, 0x96680, 0x12820 or 0x12950 in the function body. Helpers were not followed
for those four types, so GPU allocation through a helper is **unknown** for Model, Material, Shader and
TextureStreamingChunk.

## 5. Streaming, GPU feedback, and the mip selection (observed + reported)

- `0x13ccd0` (1016 B) runs every frame from the render job (`0x13cae0` at 0x117930 and 0x117160). It walks active
  texture slots (bitset up to 0xffff) and calls `0x13a3e0` (760 B) per texture.
- `0x13a3e0` drives a per-texture state machine: it reads the requested level from `param_1[1]` (low byte 3..7 are
  the states) and calls `0x98ae0` with the level (`uVar15 >> 24`), then `0x98da0`. Reported: no per-frame cap was
  found; the cvars `Num Mip Requests` and `Max simultaneous load requests` have no reader.
- **GET_LOD_STATS (port side, `gpu/shadps4/video_core/sotc_lod_stats.h`, 349 lines):** a T# with LOD_HW_CNT_EN
  counts its sampling in one of 256 counter banks. The port marks banks at bind (`SotcLod::Mark`, bind thread) and
  collects them when the game issues GET_LOD_STATS. The GPU feedback (`gpu_mip[256]`, filled by
  `Rasterizer::SotcLodFeedbackCycle`, merged by `MergeGpu`) gives the finest mip each bank was sampled at. The game
  reads those banks from memory and asks the streamer for that mip. **This is a GPU→CPU consumer on the streaming
  path.** The port currently reads it through its readback path. Section 6 gives the native alternative.
- Header comment facts (observed): mip 0 everywhere fills the game's pool and breaks guest memory within 1-4 minutes;
  mip 2 ran the attract demo but broke a save load until the readback on unmap was dropped. `SOTC_LOD_LEVEL` picks
  another mip for tests. `Linger = 0`: only banks used since the last report count.
- Unload: `DeleteResource` `0x1478d0` and the destructor `0x146770` call the full barrier `0x1469d0`. **Open:**
  whether either runs during gameplay streaming (decides whether an unload waits for pending loads).

## 6. Proposed PC-native mechanism

### 6a. Buffers and meshes: announce at publish, upload once on workers

- **Patch point:** the publish `0x14d670`, for objects whose descriptor is Mesh or Texture or ParticleTexture (the
  `[descriptor+0x78]` target is 0x1264d0, 0x98aa0 or 0x10e9b0). At publish the build job has finished; the
  GPUMemory copies are complete (happens-before via the publish).
- **Call:** `NativeWrite(addr, size)` for each GPU copy. Addresses come from the build's allocation: `[obj+0x10]`
  and `[obj+0x20]` for buffers (W1); the single-block texture base and size (W2, `0x96680` local `uVar9`).
- **Correctness:** these copies are never written after publish (section 2). Verify mode: any CPU write to an
  announced range after publish is an error (counts through the existing page tracking, which still runs).
- **Upload:** a worker copies the guest copy into a device-local buffer (staging then transfer), paced by the
  existing 32 MB budget (`SOTC_UPLOAD_BUDGET_MB`). The publish drain (`0x1480f0`) must not upload.
- **Bind:** announced ranges skip the page scan and the sync copy. This is the class behind the 138.9 MiB bind.

### 6b. Textures: create the image at the T# write, upload at publish

- **Create:** at the `0x881e60` entry hook (the T# base is set there; section 3). The port reads the T# words
  `obj+0xc4` (8 words) and creates the VkImage with its existing decoder.
- **Upload:** same as 6a for single-block textures (W2 and W3). For mip-tail textures, one `NativeWrite` per part.
- **Streamed mips (W4):** the copy in `0x98ae0` is the write. Two options, with the second preferred:
  1. Announce each W4 copy (`NativeWrite(pool_top, size)`) and upload on the worker (the copy itself still runs on
     the render job, unchanged).
  2. Replace the W4 memcpy with a registration (the mip's CPU data is the source, the pool range the destination)
     and let the worker copy it. This needs the `0x98ae0` change to be verified against the T# state (the T# base
     is written by `0x881e60` before the copy, so the worker must wait for it).
- **Mip selection (section 5):** the GPU feedback should be consumed without a CPU wait: use the last completed
  feedback (one frame old), not the current frame. This is the "read without waiting" class of the plan, and it
  needs approval per the plan's rules.

### 6c. Render targets (RTAllocator, `0x9aef0`)

GPU-written, never uploaded, never read by the loader. Under the native model they are created at the allocation
point and not announced. `gpu/sotc/rt_class.cpp` already keeps them out of download and tracking (`SOTC_RT_CLASS`):
it records RTAllocator blocks in `g_blocks` (base to size), counts downloads and tracks, and offers
`Contains()` as a lock-free test. Nothing else is needed for the loader inventory.

## 7. What the port already captures (item 3)

| File | What it captures | Gap for the loader inventory |
|---|---|---|
| `gpu/sotc/probe_loader.cpp` (319 L, `SOTC_HOOK_LOADER_PROBE`) | Wrap hooks on `0x1469d0` (LOADER_WAIT_ALL) and `0x1480f0` (LOADER_SYNCPOINT): call counts, per call-site counts (return address from the guest stack), SyncPoint calls inside vs outside a barrier, per window | No bytes, no sizes, no resource names, no timing of the drained items |
| `gpu/sotc/static_class.cpp` (SOTC_STATIC_CLASS) | Classification counts (ranges, MiB, pages, tracked, GPU bit, clean, missing spans) for the fresh allocations of mesh build (`0x8bf0b`) and texture build (`0x96b48`) | Only the allocations, not the bind that uploads them; no link to the resource |
| `gpu/sotc/area_prewarm.cpp` (610 L, `SOTC_AREA_PREWARM`) | Per-area manifest of **image records** (distinct images the first load created), replayed on re-entry from `user/cache/area_manifest/<area>.bin`; saved by a worker and at exit | Images, not zone activation; the first visit is not covered; records image creation, not the upload bytes |
| `gpu/sotc/rt_class.cpp` (135 L, `SOTC_RT_CLASS`) | RTAllocator block map and counts of downloads and tracks skipped | No per-render-target use |
| `gpu/shadps4/video_core/sotc_lod_stats.h` (349 L) | Per-bank LOD use marks at bind, GPU feedback merge, pool free-page reads (`PoolFree`, `PoolBitmap`) | No link from a bank to the resource that consumes it |

Nothing in the port records, per upload range: the resource (name, FourCC, object), the publish frame, the first
bind frame, or the writer of the last write. That is the gap for question 5.

## 8. Question 5: per-range logging for the 139 MiB frame

The load frame's upload is the sum of sync-copy ranges that the draw thread uploads at bind (hitch.md line 7: bind
138.9 MiB, dma 26.0 MiB). "Written long before" means the range was published (or W4-copied) in an earlier frame
than the bind that uploaded it. To split the 139 MiB, the verify mode (`SOTC_VERIFY`, the plan's name) should record,
per sync-copy range at the upload point (the buffer cache's sync copy, `bind` and `dma` callers):

1. `addr`, `size`, `bind_frame` (the frame index of the bind), `caller` (the port's upload site).
2. `publish_frame`: the frame in which the range's object was published (`0x14d670` hook, recorded in a table
   keyed by the GPU copy's base address, from section 6a; also the W4 copy frame for streamed mips).
3. `last_write_frame` and `last_write_rip`: from the page tracker's write fault that last bumped the range's
   granule generation (`SOTC_ARENA_ATTRIB` already attributes faults to RIPs).
4. `resource`: the object's name (`[obj+0x78]` string, the same name the request logs) and its FourCC (section 4).
5. `class`: `static` (publish-to-bind with no later write), `streamed` (W4 copy), `dynamic` (a later write).

Derived per load frame: the share of bind bytes where `bind_frame - publish_frame >= 2` (written long before, bound
first time), the share of bytes whose first bind is the first bind of that address, and the top ten resources by
bytes. Output as one line per 5 s window next to the existing sync-copy counters, so the matrix can read it with
`--repeat 1`. The table is capped (for example 16k entries), off by default, and read with the same atomics as
the other counters.

## 9. Counts (this pass)

| Class | Sites |
|---|---|
| Loader sync points | 3: publish `0x14d670`, drain `0x1480f0`, barrier `0x1469d0` |
| Build copies to GPU | 3 build types (mesh `0x8be60`, texture `0x96680`, particle `0x96680`) |
| Post-build writers to GPU memory | 2: W4 mip copy and W5 decommit in `0x98ae0` |
| CPU-only transient | 3: read `0x14b980`, decode `0x1aa2a0`, stream `0x8db10` |
| GPU-result consumer on the streaming path | 1: GET_LOD_STATS banks (`sotc_lod_stats.h`) |
| RT allocations | 1: `0x9aef0` (GPU-written) |

## 10. Open questions

1. The 63% split of the 139 MiB frame: needs a session with the logging of section 8 (not run; no game without the
   user's go-ahead).
2. Whether the loader's tree at `[loader+0x20]` is filled by `0x1444e0`: trace one registration (for example
   `0x127860`) through its insert call.
3. Whether `DeleteResource` (`0x1478d0`) runs during gameplay: a log of `0x1469d0` entries with the caller.
4. The publish's vtable targets (`*DAT_02ee2d00+0x28`, `[res+0x68]` `+0x40`/`+0x20`): the classes behind them are not
   read; they decide whether publish is a list push or a direct call.
5. Mip-tail texture format per part (`0x88f000`, `0x880c30`): the image for a multi-part texture needs its mip count
   before the parts are read, and this was not traced.
6. The family name `0x12950` (`VirtualMemoryGPU`) is still unverified (reports3/03).
7. Helpers of Model, Material, Shader and TextureStreamingChunk builds (GPU allocation through a helper).
8. `0x98ae0` callers beyond `0x13a3e0`: `0x98ae0` is called from `0x13a6f0` too (decomp caller list); that path was
   not read.
