# N5 bindless images (SOTC_N5_BINDLESS): implementation report

Status: code written, syntax-checked only (`clang++ -fsyntax-only`, the compile flags of
`out/gpu/compile_commands.json`, `gpu/shim` before `gpu/shadps4`). Not built, not run, no SPIR-V
validation, no game. Uncommitted.

Scope: sampled images (plain 1D/2D/array/cube/3D, float and uint) move from per-draw descriptors to
one heap per sampled-image key. Samplers already had their heap (SOTC_BINDLESS_SAMP) and are unchanged
except for the push block size. Storage images, atomics, descriptor arrays, mip-fallback images, MSAA
and sint images keep descriptors (see "Remaining").

## Design

- Heap: one set (set 1, the bindless set of `BindlessSamplers`) holds binding 0 (samplers, unchanged)
  and bindings 1..6, one runtime array of sampled images per key. Keys (`Shader::BindlessImageKeyOf`):
  0 float 2D, 1 float 2D array (cube too), 2 float 3D, 3..5 the same for uint. Capacity 4096 per key,
  update-after-bind, partially bound, update unused while pending.
- Push: the image slots live in `BindlessSamplerSlots::image_words` (16 words, 2x u16 per word, 32 slots,
  per stage ranges in `GetBindlessImageRange`: FS 0..11, VS 12..19, GS 20..23, TCS 24..27, TES 28..31,
  compute 0..31). The struct grows from 72 to 136 bytes, so the push block is 192 bytes (PushData 56 +
  136). The layout range uses `sizeof(BindlessSamplerSlots)`, so no layout code changed for it. The
  instance requires `maxPushConstantsSize >= 192` for any bindless heap (Vulkan minimum is 128; NVIDIA,
  AMD and Intel desktop drivers report 256).
- Emitter (`spirv_emit_context.cpp`): an eligible image gets no variable and no binding number; its
  `TextureDefinition` has `bindless`, `bindless_slot`, `bindless_key`. `LoadImage` (emit_spirv_image.cpp)
  is the single load point for sampled images: it reads the 16-bit slot from push words 25..40 and
  does `OpAccessChain` into the key's runtime array. Storage paths never see a bindless texture.
- Binding numbers: `Info::AddBindings`, the host's `binding.unified` counter and the pipeline layout
  builders (graphics and compute) skip eligible images, so the numbering stays identical on both sides.
- Slot lifetime (`vk_bindless.cpp`, `BindlessImages`): a view gets a slot per key the first time a
  bindless stage binds it (`ImageView::bindless_slots[key]`, lazily, not at view creation). A new slot is
  taken when the same view is bound in another layout (the descriptor fixes its layout at write time;
  the old slot is released). Release puts (key, slot, scheduler tick) on a FIFO; the slot is cleared and
  reused only after `scheduler.IsFree(tick)`. A destroyed view releases all its slots
  (`ImageView::~ImageView`, moves hand the slots over). Slot 0 of each key is the null slot (a null
  descriptor: reads zero, nullDescriptor is required by the instance already).
- Per draw (`BindTexturesFull`, set-write loop): an eligible image writes its heap index into
  `bindless_slots.image_words`; no descriptor write, no binding increment. The bind is marked
  uncacheable and not stored by TEX_BLOCK (heap slots can be reused, so a replayed index could be stale).
- Verify (`SOTC_N5_BINDLESS=2`): every bindless bind compares the heap mirror (view handle written at each
  slot) with the view the descriptor path would bind (`BindlessImages::Verify`). Counted, not logged per
  draw.

## Switches and cache

- `SOTC_N5_BINDLESS=1|2` with `SOTC_NATIVE=1|2`. It also turns the sampler heap on (`n5_wanted` in
  instance), since the image arrays live in its set. Not combined with `SOTC_STAGE_BINDINGS` or
  `SOTC_DESC_BUFFER` (same rules as the sampler heap).
- Process flags: `Shader::SetBindlessImagesEnabled` (instance) gates emission and eligibility; the
  profile member `Profile::bindless_images` (last member, 68 -> 72 bytes) keys the shader cache. The
  serialization snippet pads 68-byte blobs; 64-byte blobs already resize to `sizeof`.
- Changing the switch rebuilds the shader cache once (profile change), as the brief allows.
- The sampler-only mode now also needs the 192-byte push limit (the sampler push block grew). Devices
  below it lose `SOTC_BINDLESS_SAMP` (logged), not a crash.

## Counters

`SOTC N5 BINDLESS acquire N writes N releases N reclaims N full N verify N mismatch N pending N`
(`BindlessImages::Report`, counters reset on read). `full` > 0 means the heap ran out (null views drawn,
logged once). `mismatch` must be 0 in mode 2. `pending` is the number of released slots waiting for
their tick.

Driver snippet (not applied, `core/libraries/videoout/driver.cpp`, inside the 5 s report block next to
the other `SOTC_*` report lines; not syntax-checked):

```cpp
if (VideoCore::BindlessImages* heap = VideoCore::BindlessImages::Active()) {
    char n5_line[256];
    heap->Report(n5_line, sizeof n5_line);
    std::fprintf(stderr, "%s\n", n5_line);
}
```
Include `video_core/renderer_vulkan/vk_bindless.h` in driver.cpp.

## Files changed (worktree, uncommitted; base taken from re/pc-native)

Recompiler (owned): `shader_recompiler/resource.h` (constants, `image_words`, `ImageSlotIndex`,
static asserts), `runtime_info.h` (flag, `GetBindlessImageRange`), `info.h` (`BindlessImageSlotOf`,
`BindlessImageKeyOf`, `UsesBindlessImages`, `AddBindings`), `info.cpp` (the functions), `profile.h`
(`bindless_images`), `backend/spirv/spirv_emit_context.h/.cpp` (`TextureDefinition` fields,
`DefineImagesAndSamplers`, `DefinePushDataBlock`, `LoadBindlessImageIndex`),
`backend/spirv/emit_spirv_image.cpp` (`LoadImage`), `backend/spirv/emit_spirv.cpp` (capabilities).

Host (owned): new `video_core/renderer_vulkan/vk_bindless.h/.cpp` (`BindlessImages`, layout
bindings, `BindlessImagesMode`), `video_core/texture_cache/image_view.h/.cpp` (slots, move, release),
`video_core/renderer_vulkan/vk_rasterizer.h/.cpp` (member `bindless_images`, ctor, `BindlessImageIndex`,
set-write loop of `BindTexturesFull`).

Snippets for files not owned: `/c/Users/ptr/AppData/Local/Temp/claude/C--Users-ptr-Documents-sotc-pc/4f16c57a-0ca9-4bd4-9f8a-fa1c51178788/scratchpad/n5-snippets.diff`
(unified diff against re/pc-native; applied and syntax-checked in the overlay):
- `texture_cache/sampler.cpp`: layout gets bindings 1..6 and the pool gets `eSampledImage` 24576 when
  `Shader::BindlessImagesEnabled()`.
- `renderer_vulkan/vk_instance.cpp`: N5 implies the sampler heap; the 192-byte push probe; the
  `SetBindlessImagesEnabled` decision (limits 4096 x 6 update-after-bind sampled images, per stage and
  total; no stage bindings).
- `renderer_vulkan/vk_graphics_pipeline.cpp`: layout skips eligible images; `uses_bindless_samplers`
  also true for stages with bindless images (the trailing set and the push range are then bound).
- `renderer_vulkan/vk_compute_pipeline.cpp`: same for the compute layout.
- `renderer_vulkan/vk_pipeline_cache.cpp`: `.bindless_images` in the Profile initializer.
- `renderer_vulkan/vk_pipeline_serialization.cpp`: 68-byte profile padding and the new byte.

Application order in the main tree: apply the diff (it already adds the includes of sampler.cpp), then
the driver snippet.

## Checks

- `-fsyntax-only` passes on all changed owned files and on the six patched snippet files (overlay with
  gpu/shim first). Overlay: `scratchpad/overlay/gpu/shadps4` (base re/pc-native + my files + snippets).
- Not checked: the driver snippet; generated SPIR-V (spirv-val); anything at run time.

## Remaining (not done)

1. Storage images (`is_written`), atomics, descriptor arrays (`array_size`), mip-fallback images, MSAA
   and sint sampled images: still descriptors. The brief asked for storage in the heap; that needs
   per-format keys (storage formats are part of the SPIR-V type) and is not in this pass.
2. Bind cache (`SOTC_BIND_CACHE`) and TEX_BLOCK stop storing binds with bindless images: hit rate drops
   while the mode is on. A replay check of the slot (mirror compare on hit) would restore it.
3. Slot is taken lazily at the first bind (per view, key and layout), not at view creation; the view
   variant (depth/stencil, cube-to-array, format substitution) is already a separate ImageView, so each
   variant has its own slots. Metadata fallback and LOD feedback do not touch the image, so they are
   fine as they are.
4. Heap full: 4096 views per key; beyond that the bind reads null (zero) and logs once. No eviction.
5. Layout churn: a view bound alternately in two layouts (feedback loops, depth sampled as attachment)
   takes a new slot per switch; cost is one descriptor write per switch, not measured.
6. Key from the compile-time T# (as the descriptor type already is); the verify mode does not check the
   key against the draw-time T#.
7. No measurement: the attract benchmark is for the user to run after authorization.
