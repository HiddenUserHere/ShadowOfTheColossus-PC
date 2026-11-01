# Wave 1, texbind: texture identity memo (SOTC_GEN_TEX) and RT class completion

Agent id: texbind. Design source: `docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md` (V1 texture side,
M2 RT class, T1 optional). Scope: `video_core/texture_cache/texture_cache.{h,cpp}`, `image_view.{h,cpp}`
(checked, not changed), `gpu/sotc/rt_class.{h,cpp}` (checked, not changed). Call sites are snippets below.

## Status

| Part | State |
|---|---|
| A. Texture identity memo, `SOTC_GEN_TEX` | Implemented (texture_cache.cpp/.h). Syntax-checked. Not run. Needs the call-site snippet 3 to take effect. |
| B. RT class completion, `SOTC_RT_CLASS` | One gap closed (render targets no longer queued for readback in mode 1). Rest of the plan rows is out of these files (report only). |
| C. T1 render targets at maximum extent, `SOTC_RT_MAXEXTENT` | Not started. Reason at the end. |

Nothing was built, run or copied to `out/`. Checks were syntax-only (see "Verified").

## A. Texture identity memo (`SOTC_GEN_TEX`)

### Switch

`SOTC_GEN_TEX`: `0` (default, off), `1` (memo on), `2` (memo on and the full path also runs on every
hit; the full path's answer is returned, mismatches are counted). Read once in
`TextureCache::TexIdentityMode()`. Forced to 0 with a warning when `SotcGen::On()` is false, so the
memo never runs on an unmaintained generation. Only texture binds (`BindingType::Texture`) and only
mip-tail-free descriptors (`desc.rebase_mip == 0`) use the memo; storage and render-target binds keep the
old path.

### What changed (file: function)

- `texture_cache.h`:
  - `TexIdentityKey { std::array<u64,4> words; u64 extra; }`: raw T# words plus the caller's bits.
  - `TexBind { ImageId image_id; ImageView* view; }`.
  - `FindTextureIdentity(key, desc, exact_fmt)`, `TexIdentityMode()`, `ReportTexIdentity(out, seconds)`.
  - `TexIdMemo` (key, exact_fmt, valid, image_id, image_uid, pre_view_info, view_info, epoch, sum),
    `tex_id_memos` (4096 direct-mapped entries, allocated only when the mode is on), `tex_epoch`.
- `texture_cache.cpp`:
  - `FindTextureIdentity`: lookup, hit, mode 2 verify, miss and store (see below).
  - `TexIdentityMode`, `TexIdentityHash`, `ReportTexIdentity`, the `tid_*` counters.
  - Constructor: allocates `tex_id_memos` when the mode is on.
  - Generation helpers `GenOn`, `GenBump`, `GenBumpImage`, `GenSumImage` (only call `SotcGen` when on).
- Generation bumps (`SotcGen::Bump` on the image's or the written range):
  - `InvalidateMemory`: each image in the faulting region (CPU write).
  - `InvalidateMemoryFromGPU`: each image set to GpuDirty (both loops).
  - `WriteBackSkippingStacks`: GPU write-back of a download into guest memory.
  - `MarkWritten`: every GPU write into an image (called by FindRenderTarget, FindTexture storage, MarkWritten callers).
  - `RefreshImage`: right before the upload from guest memory.
  - `UnregisterImage`: image unregistered (and `tex_epoch` bumped).
- `tex_epoch` bumps (all under `mutex`): `RegisterImage`, `UnregisterImage`, `MipTailCreated`. Every
  other ImageInfo replacement (`ResolveDepthOverlap` twin, `TakeUnmapParked`) goes through `UnparkImage`
  which registers, so it is covered by `RegisterImage`.

### Algorithm

Lookup (eligible binds): the slot is `(*tex_id_memos)[hash(key) % 4096]`. Under `mutex` the memo answers
only if all of these hold, otherwise the reason is counted:

1. Key equal (T# words, extra), `exact_fmt` equal, and the caller's `desc.view_info` before FindImage equal to the stored one (miss reason "no entry").
2. `m.epoch == tex_epoch` (miss reason "set moved").
3. The image in the slot has the stored `image_uid`, is Registered, has no Dirty bits (MaybeCpuDirty, CpuDirty, GpuDirty), no pending warm record, is not a mip tail, and is fully tracked (`track_addr`/`track_addr_end` equal to its guest range) (miss reason "image state").
4. `SotcGen::Sum(guest range) == m.sum` (miss reason "range changed").

Hit (mode 1): `desc.view_info = m.view_info`; the image is touched as FindImage touches it
(`tick_*`, `TouchImage`); `FlushTexProtect()` as the refresh does; the GPU-bind evidence hooks of
UpdateImage run (`SotcMemClasses::NoteGpuBind`, `SotcPcPlaceGlue::OnGpuBind`); returns the image and
`image.FindView(desc.view_info)`. No FindImage, no overlap scan, no refresh.

Miss: `FindImage` + `FindTexture` as before, then a store if the image is clean now (Registered, no
Dirty bits, fully tracked, not warm, not a tail; otherwise "not stored"). The Sum is taken after the
refresh, which has bumped it, so the stored Sum is the one for the uploaded contents. The stored view
info is `desc.view_info` after FindImage.

Mode 2: the hit is computed on a copy (`probe`), then the full path runs on `desc`. A mismatch is any of
image id differs, view pointer differs, or the view info after FindImage differs from the probe's.

### Counters (`SOTC TEX IDENTITY` line, per window, from `ReportTexIdentity`)

- `lookups`: eligible binds (texture, not a tail candidate, mode on).
- `hits`, and the percentage of lookups.
- `bypass`: texture binds that are mip-tail candidates (`rebase_mip != 0`), never memoized. A large share here means the memo cannot help those binds.
- `stores`, `not stored` (the image was not clean after the refresh, or was not tracked).
- misses by reason: `no entry` (empty slot, different key, or different pre-view), `set moved` (epoch), `image state`, `range changed` (Sum).
- `verify`, `mismatches`: mode 2 only; the required value is 0.

### Call-site snippets (not mine, `vk_rasterizer.cpp`)

1. Report: `gpu/shadps4/core/libraries/videoout/driver.cpp`, next to the existing line
   `VideoCore::TextureCache::ReportImageFrees(report, seconds); // B4 (SOTC_FPS)` (about line 2031), add:
   ```cpp
   VideoCore::TextureCache::ReportTexIdentity(report, seconds); // sotc: SOTC_GEN_TEX
   ```
   It prints only when the mode is on. If that report block is inside an `SOTC_FPS` gate, it is printed only with that gate.

2. Generation bump on every GPU write, `gpu/shadps4/video_core/texture_cache/image.h`, `MarkGpuModified`
   (inline, called by `vk_runtime.cpp` copy/clear paths and `vk_rasterizer.cpp:5053`). The bump is
   needed for the Sum to reflect those writes, which currently bump the gpu epoch only on the first write:
   ```cpp
   #include "video_core/sotc_generation.h" // at the top, with the other includes
   inline void MarkGpuModified(Image& image, bool bump_epoch = true) {
       if (SotcGen::On()) { // sotc: SOTC_GEN_TEX, every GPU write moves the range's Sum
           SotcGen::Bump(image.info.guest_address, image.info.guest_size);
       }
       ...existing body unchanged...
   ```
   Note on correctness: a GPU write does not change which image or view a bind gets, and a clean hit
   does not upload. The bump matters only for Sum consumers that care about GPU writes. Snippet 2 is
   recommended, not required for the identity; the memo still misses after every MarkWritten bump.

3. The bind itself. The first-pass texture branch, the line `image_id = texture_cache.FindImage(desc);`
   that follows the `MipStorageFallbackMode` block (about line 6291 of `vk_rasterizer.cpp`). Do NOT
   change the element-buffer branch above it (about line 6214). In that branch `tsharp` (AmdGpu::Image)
   and `image_desc` (Shader::ImageResource) are in scope:
   ```cpp
   // sotc: SOTC_GEN_TEX: the texture identity memo (texture_cache.cpp). The key is the T# words and
   // the resource bits; the caller's view adjustments (mip fallback index) are compared inside.
   static_assert(sizeof(tsharp) == 4 * sizeof(u64));
   VideoCore::TextureCache::TexIdentityKey tkey{};
   std::memcpy(tkey.words.data(), &tsharp, sizeof(tsharp));
   tkey.extra = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                u64{image_desc.constant_mip_index} << 8 |
                u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
   image_id = desc.type == VideoCore::TextureCache::BindingType::Texture
                  ? texture_cache.FindTextureIdentity(tkey, desc).image_id
                  : texture_cache.FindImage(desc);
   ```
   Caveats for this snippet: `FindTextureIdentity` also runs `FindTexture` (UpdateImage, refresh) for the
   bind, so the refresh now happens in the first pass, before later bindings' `FindImage` calls. The
   second pass's `FindTexture` on the same image is idempotent (a clean image is a no-op), so its
   `needs_rebind` logic is unchanged, but the ordering of refreshes against later overlap resolutions
   changes for images shared by two binds of one draw. Test with `SOTC_GEN_TEX=2` (mismatches 0) and the
   existing bind-cache/rebind verifies. The T# in the second-pass re-bind (`needs_rebind`) is left on the plain path.

   The mode 0 path of `FindTextureIdentity` is `FindImage` + `FindTexture`, the same calls as today, so
   with `SOTC_GEN_TEX` unset the snippet is behaviour-neutral apart from the one extra branch.

## B. RT class (`SOTC_RT_CLASS`)

### Already in the tree (checked, unchanged)

- Mode 1 download skip: `DownloadImageMemory` returns before the copy for an image in an RTAllocator block (`NoteDownloadSkipped`).
- Mode 1 track skip: `TrackImage` untracks a clean render/depth/storage image in a block (`RtClassSkipTrack`, `NoteTrackSkipped`).
- CPU write counter: `InvalidateMemory` counts a CPU write that reaches a block image (`NoteCpuInvalidate`).
- Block feed: `probe_alloc.cpp` (`OnBlock`, `OnFreeBlock`), 5 s report: `driver.cpp` (`SotcRtClass::Report`).

### Added

- `RtClassNeverDownloads(image)` (texture_cache.cpp): an image in an RTAllocator block is not queued for
  readback in mode 1. Used in `FindRenderTarget` and in the storage branch of `FindTexture`, which
  filled `download_images` (only when `readback_linear_images`). Effect: no queue churn for render targets
  whose download is never done; `NoteDownloadSkipped` counts fewer per-frame repeats. The download itself
  was already skipped, so the image contents are unaffected.

### Not done (outside these files or not doable here)

- VRAM-only backing for GPU_PRIVATE images (needs the allocation side in `image.cpp`/`vk_runtime.cpp`, not owned).
- Read-fault readbacks for render-target pages (the fault path is in the buffer/memory side).
- A separate "GPU-only scratch" class: nothing in these files identifies scratch images apart from RTAllocator blocks.

## C. T1 render targets at maximum extent (`SOTC_RT_MAXEXTENT`)

Not started. The change sits in `ExpandImage` and image creation (`texture_cache.cpp` ~1013), where it
changes which image sizes a render target gets and how views are sized. It needs runtime validation
(images at the larger extent, view ranges, the resolve and copy paths) that I could not do without
building. No switch was added.

## Verified and assumed

Verified in code:
- Syntax: `clang++ -std=gnu++23 -Wall -Wextra -fsyntax-only` on `texture_cache.cpp` with the project's
  compile flags (from `out/gpu/compile_commands.json`) and the scratch stub (below) passes with no errors.
  The only warning is pre-existing (`texture_cache.cpp` ~1612, sign compare in `ResolveOverlap`-adjacent code).
- Read-side hooks used by the hit: `UpdateImage` (TrackImage, TouchImage, RefreshImage, the two GPU-bind
  hooks), `FindImage` (tick stamps, `TouchImage`, warm record), `FindView`.
- Fields used in `check`: `image_uid`, `flags` (Registered, Dirty), `sotc_warm_pending`, `info.sotc_first_mip`, `track_addr`, `track_addr_end`.

Assumed (not checked, because `video_core/sotc_generation.h` does not exist in the tree yet):
- The stub used: `scratchpad/stub/video_core/sotc_generation.h` (`On()` false, `Bump` and `Sum` no-ops), only for the syntax check. The real header was not compiled against.
- `SotcGen::Sum` is a monotonic sum of per-granule counters, so an equal Sum means no bump in the range. If it can return to an old value, the range check is unsafe.
- `SotcGen::Bump` is safe to call from the write-back callback (`WriteBackSkippingStacks` runs on the scheduler's thread for async downloads) and from the CPU fault handler.
- `SotcGen::On()` does not change during a run.
- Image upload and tracking invariants: a clean, fully tracked image has its CPU writes reported through `InvalidateMemory` (the existing design relies on this too).

## How to test in game

1. Syntax and link are not checked. Build with `bash build.sh` (not run by me).
2. Lab route, one round (`tools/pcmem_matrix.py --repeat 1`, route file `user/routes/tests.json`).
   Config A: `SOTC_GEN_TEX=2` (with snippets 1 and 3 applied). Pass if `mismatches 0` in every `SOTC TEX IDENTITY` line and the hit rate is reported.
3. Config B: `SOTC_GEN_TEX=1` against the default, alternating over three runs. Metric: draw work (the draw-thread timers), `BindResources` share, `FindImage` share in the profile. Expected: hit rate high on the attract and Shrine scenes; if hits are low, read the miss reasons (a large "set moved" share means image churn, a large "range changed" share means GPU writes or uploads).
4. Check visually after the change (texture artefacts, stale textures after area load, water/AMD tint).

## Risks

- Hit path takes `mutex` (as FindImage does). The design's lock-free hit is not achieved; it needs atomics for the flags and the epoch.
- `tex_epoch` is global. Each register or unregister (image churn: 120-340 images per second in the profile) invalidates every memo entry. Expect a low hit rate under churn. The next step is a per-address epoch like `addr_gens`, which needs the alias rules worked out per image.
- Mip-tail candidates are never memoized (`bypass` counter). For textures with base level > 0 this may be a large share; check the counter before drawing conclusions.
- Images untracked by design (mode 1 render targets, partial track ranges) never hit. That is correct but means render-target sampling gets no memo benefit.
- Mode 2 counts mismatches but does not stop the run. A non-zero count is the signal to disable the memo.
- The rasterizer snippet moves the first-pass refresh earlier (see snippet 3 caveats).
- Generation bumps in `InvalidateMemory` are per overlapping image (cheap, but on the CPU fault path, under `mutex`).

## Changes not needed

`image_view.{h,cpp}`: no change. `rt_class.{h,cpp}`: no change (the gap was in texture_cache.cpp, the callers of `SotcRtClass`).
