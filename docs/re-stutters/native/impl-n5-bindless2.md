# N5 bindless2 (SOTC_N5_BINDLESS): storage images, bind cache and texture block replay, heap eviction

Status: code written, syntax-checked only (`clang++ -fsyntax-only`, the flags of `out/gpu/compile_commands.json`,
with the worktree's `gpu/shim` and `gpu/shadps4` first, over a scratch overlay of the current main
`gpu/shadps4` and `gpu/shim`). Not built, not run, no SPIR-V validation, no game. Uncommitted.

Base: the main tree's current files. `vk_rasterizer.cpp` moved in main while this task ran; my edits were
re-applied onto the newer main copy (all 13 + 1 anchors matched). The other owned files differ from main only by
these edits.

Mode 0 and `SOTC_N5_BINDLESS` unset: no path changes. Every new branch is behind `bindless_images` (the heap),
the heap generation (0 without the heap, equal on both sides) or a key/flag that only the heap sets.

## 1. Storage images

- **No per-format keys.** The emitter types a non-atomic storage image with the Unknown format
  (`ImageType` in spirv_emit_context.cpp: `format = desc.is_atomic ? GetFormat(image) : Unknown`), so its SPIR-V
  type is decided by dimension, sampled type and the storage flag only. A format key would split identical
  types into identical arrays. Counting the formats the shaders use would not change any key, so it was not done.
- Keys (`Shader::BindlessImageKeyOf`, resource.h): 0..5 sampled (float 2D / 2D array (cube too) / 3D, then uint),
  6..8 float storage, 9..11 uint storage (the same three shapes). `NUM_BINDLESS_IMAGE_KEYS` = 12,
  `NUM_BINDLESS_SAMPLED_IMAGE_KEYS` = 6. Fallback to descriptors: atomics, descriptor arrays, mip fallback,
  MSAA, sint.
- Binding: key k at binding 1 + k on set 1 (sampled 1..6, storage 7..12), descriptor type `eStorageImage` for the
  storage keys (`BindlessImageDescriptorType`), layout eGeneral (the host's second pass moves storage images to
  eGeneral, and the heap writes the layout the bind passed).
- Emitter: `DefineImagesAndSamplers` gives a bindless storage image `is_storage = true` and the storage sampled
  type; `EmitImageRead` / `EmitImageWrite` load it through `LoadImage` when `texture.bindless`. Capability
  `StorageImageArrayDynamicIndexing` is added when a bindless storage image exists (emit_spirv.cpp).
- Host: the heap branch of `BindTexturesFull` takes storage images too (it was `!is_storage`).
- Device (snippet, vk_instance.cpp): N5 also needs `descriptorBindingStorageImageUpdateAfterBind`,
  `shaderStorageImageArrayDynamicIndexing` and the update-after-bind storage limits for 4096 x 6 arrays (per set
  and per stage). The two features are enabled only when N5 is on.
- Pool (snippet, sampler.cpp): the sampled pool size is now the sampled keys only (it was the total key count, which
  would over-count once storage keys exist), plus `eStorageImage` for the storage keys.

## 2. Bind cache (SOTC_BIND_CACHE, _FRAME) and TEX_BLOCK

Before: a bind with a heap image was marked uncacheable and not stored by TEX_BLOCK, because a slot can be
reused. Now a stored bind keeps what it named and is replayed only while the heap generation is unchanged.

- `Shader`-free host types: `VideoCore::BindlessRef {key, slot, view}` (vk_bindless.h). `BindlessImages::Generation()`
  is bumped on every release and every eviction (the two events that can change what a slot holds for the bind
  that named it).
- Full bind: `BindlessImageIndex` returns a ref per heap image; refs go to the member `bind_bindless_rec`
  (cleared per `BindResources`), and to the texture block memo (`bindless_push[d]` = push slot + 1,
  `bindless_block[d]` = ref, `bindless_gen` = generation at the start of the stage's bind).
- Commit (`CommitBindCache`, `CommitBindCacheFrame`): the entry takes `bind_bindless_rec` and `bind_bindless_gen`
  (the generation at the start of `BindResources`; a bind that moved the heap during its own run is never valid).
- Hit (`TryBindCache`, `TryBindCacheFrame`): an entry whose generation differs is refused (dropped). A hit calls
  `Use` on each stored ref (the slots are used by this tick) and, under mode 2, `Verify` against the stored view.
- TEX_BLOCK (`TryTextureBlock`): refused on a generation mismatch before any side effect, and again after the
  `UpdateImage` loop (an upload can destroy views, and a destroyed view gives its slots back). The replay writes
  the push words, appends the refs and calls `Use`/`Verify`; a heap image writes no descriptor and no binding
  number, matching `BindTexturesFull`.
- `bind_cache_uncacheable` and `store_ok` are no longer set by heap images.
- Fast bind: a stage with heap images is not fast-bound (`FastBindStatic` returns false for `UsesBindlessImages`).
  A fast bind keeps the previous push words without touching them, and an eviction could take one of those slots.
  The bind cache still serves these stages.
- Hit-rate effect: an entry lives as long as no slot is released or evicted. Evictions happen only when the heap
  is full. Layout churn (one view bound in two layouts) moves the generation on every switch and drops entries
  that name heap images. Not measured.

## 3. Heap full: eviction

- Slot state per slot: Free, Live (holds a view, its layout, `last_use` tick), Pending (released, held until
  `scheduler.IsFree(release tick)`).
- `Acquire` with no free or fresh slot calls `Evict(key)`: the live slot with the smallest `last_use`, taken only
  when `scheduler.IsFree(last_use)`. Only when no slot qualifies is the null slot drawn (counted in `full`).
- `Use(key, slot)`: every bind that names a live slot sets its `last_use` to the tick being recorded. A slot named
  by an earlier image of the same draw, or by the current tick's draws, is therefore not evictable before that tick
  has finished on the GPU.
- The owner keeps its old slot number: `Holds` fails, its next bind takes a new slot. `Release` of a slot the view
  no longer holds is a no-op. `ImageView::ReleaseBindless` passes the view handle to `Release`.
- Counter `evict N` in the report line.

## 4. Fix in the sampled path (from the previous pass)

`BindResources` sized `set_writes` for every image, but a heap image writes no descriptor, so the vector kept
trailing default entries (descriptorCount 0, not a valid write) and `set_writes.size() != set_write_index` marked
every such bind uncacheable. With the heap on, `BindResources` now truncates `set_writes` and `set_write_sets` to
`set_write_index` after the stage loop. Mode 0: not executed.

## 5. Verify (SOTC_N5_BINDLESS=2)

- `BindlessImageIndex` (sampled and storage, every full bind): `Verify` compares the heap mirror with the view.
- Cache hits and TEX_BLOCK replays: `Verify` of each stored ref against its stored view.
- `mismatch` must stay 0. A stale slot the generation check did not catch shows up there.

## 6. Snippets for files I do not own

`/c/Users/ptr/AppData/Local/Temp/claude/C--Users-ptr-Documents-sotc-pc/4f16c57a-0ca9-4bd4-9f8a-fa1c51178788/scratchpad/n5-bindless2-snippets.diff`
(unified diff against the main tree; applied and syntax-checked in the overlay):

- `video_core/texture_cache/sampler.cpp`: three pool sizes (sampler, sampled keys, storage keys); `poolSizeCount` 3.
- `video_core/renderer_vulkan/vk_instance.cpp`: `n5_sampled` / `n5_storage` limits, `storage` in the N5 condition,
  `shaderStorageImageArrayDynamicIndexing` (Vulkan 1.0 block) and `descriptorBindingStorageImageUpdateAfterBind`
  (Vulkan 1.2 block), both gated by `bindless_images`.

Needs nothing: `vk_graphics_pipeline.cpp`, `vk_compute_pipeline.cpp`, `vk_pipeline_common.cpp`,
`vk_pipeline_serialization.cpp` (they go through `BindlessImageSlotOf`, `AddBindings`, `UsesBindlessImages`).
The driver snippet of impl-n5-bindless.md is unchanged (`Report` keeps its signature; the line gains `evict`).

## Files changed (worktree, uncommitted)

- Recompiler: `shader_recompiler/resource.h` (keys), `info.h` (doc), `info.cpp` (`BindlessImageKeyOf`),
  `backend/spirv/spirv_emit_context.cpp` (`DefineImagesAndSamplers`), `backend/spirv/emit_spirv.cpp`
  (capability), `backend/spirv/emit_spirv_image.cpp` (`EmitImageRead`, `EmitImageWrite`).
- Host: `video_core/renderer_vulkan/vk_bindless.h/.cpp` (rewritten: slots, eviction, generation, refs),
  `video_core/texture_cache/image_view.h/.cpp` (no layout array, release with handle),
  `video_core/renderer_vulkan/vk_rasterizer.h` (`BindlessRef` use in memo and entry, `bind_bindless_rec`,
  `BindlessGeneration`), `vk_rasterizer.cpp` (`BindlessImageIndex`, `BindTexturesFull`, `BindResources`,
  `TryBindCache`, `TryBindCacheFrame`, `CommitBindCache`, `CommitBindCacheFrame`, `TryTextureBlock`,
  `FastBindStatic`).
- Unchanged copies (identical to main): `runtime_info.h`, `profile.h`, `spirv_emit_context.h`.

## Checks

- Syntax-only, zero errors: `vk_bindless.cpp`, `vk_rasterizer.cpp`, `image_view.cpp`, `info.cpp`,
  `spirv_emit_context.cpp`, `emit_spirv.cpp`, `emit_spirv_image.cpp`; snippet files `sampler.cpp`, `vk_instance.cpp`
  (patched overlay copies).
- Not checked: a build, the shader cache rebuild, `spirv-val` on storage arrays, any run, the hit rate.

## Remaining

1. Measure the hit rate of the frame cache with N5 on, and the cost of layout churn (the generation moves on every
   release).
2. Fast bind is off for stages with heap images in N5 mode: measure what that costs on the attract and Shrine scenes.
3. Storage formats: per-format keys would need a format in the emitted type (an emitter change); not done.
4. `UsesBindlessImages` is O(images^2) per stage and runs per draw in `FastBindStatic`; cheap at SotC's counts,
   but it could be cached per Info.
5. Eviction scans the key's slots (up to 4096) when the heap is full; fine for an overflow case, not for steady
   thrash. Count `evict` in the report to see whether it happens.
