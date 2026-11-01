# Wave 1, t1b: render targets at maximum extent (SOTC_RT_MAXEXTENT, T1)

Agent id: t1b. Design row: `docs/re-stutters/reports3/draw/DRAW-THREAD-DESIGN.md`, T1 ("render targets created
at their maximum extent (no expand recreation)"). Owned files: `gpu/shadps4/video_core/texture_cache/texture_cache.{h,cpp}`.
`image_info.{h,cpp}` checked, not changed. The texbind code (`SOTC_GEN_TEX`) is intact (identity functions and
the `tid_*` counters untouched).

Nothing was built, run or copied to `out/`. The check was a syntax-only run (see "Verified").

## Switch

`SOTC_RT_MAXEXTENT=1` (default off; any other value is off). Read once in `TextureCache::RtMaxExtentMode()`.
No dependency on `SotcGen` or another switch.

No mode 2. The old path and the new path give different image identities by construction (the old path
expands to a new image, the new path serves a view of a larger one), so a value-by-value verify would need
the old path to run beside the new one. The only cheap mismatch signal is the guard itself
(`RtRaiseLayers`), and mode 1 already uses the guard as a fallback to the old path, so a mode 2 would add
nothing. Recommendation: judge the switch by the counters below and by the image-churn timers, not by a
mismatch count.

## What it does

Scope: render (`BindingType::RenderTarget`) and depth (`BindingType::DepthTarget`) binds only. Textures,
storage, video-out and everything else keep the old path.

The family of a target is everything that fixes its layout except the layer count: address, stencil
address/size, pitch, width, height, depth, levels, bits, samples, pixel format, image type, tile mode, array
mode, alt tile and the property flags. Pitch and height fix `BlockDim`, and the overlap rules
(`ResolveOverlap`) free an image whose `BlockDim` differs, so width, height and pitch cannot grow without
changing the family. Only the layer count varies within a family (that is the only extent the old code
ever expands: the resources branch of `ResolveOverlap`).

- Table: `TextureCache::rt_extents` (`std::unordered_map<RtFamily, RtExtentEntry>`, at most
  `MaxRtFamilies` = 4096 families; a full table is a fallback). Entry: `max_layers` (what new images are
  created with), `first_layers` (layers the old path's image would have had), `image_id` + `image_uid`
  (the family's latest image, valid only when the uid matches). All access under `mutex`.
- Creation (`FindImage`, the "create and register" branch): for a target bind with the switch on,
  `RtRaiseLayers` returns the request's info with the family's `max_layers` when that is larger. The image
  is created from that info. `RtNoteImage` records the family (first = the request's layers).
- Expansion (`ResolveOverlap`, the resources branch, the same branch that calls `ExpandImage` today): a
  target bind calls `ExpandRt`, which expands to the family's layer count when larger and records the
  family (first = the request's layers, max = the expanded layers).
- Served requests (`FindImage`, the overlap result and the memo hit): `RtNoteServed` counts a view of a
  larger image (guest size above the request's), and an "expand avoided" once per request layer count
  above the old path's model (`first_layers`), then moves that model up to the request's layers. This
  mirrors what the old path would have done with its own image, so it is a counterfactual count, not a
  measured saving.
- Guard (`RtRaiseLayers`): the layers are raised only when the image has one mip level, a non-zero layer
  count, and a guest size that divides evenly by its layers (true for both target constructors: slice size
  times layers). The raised guest size is `guest_size / layers * new_layers` (also `mips_layout[0].size`).
  Anything else returns the request unchanged and counts a fallback.

Reused unchanged: `ResolveOverlap`'s "size and resources less than or equal, use an image view" branch
(the path that serves a smaller request of a larger image today), `ResolveDepthOverlap`, `ExpandImage`
(called from `ExpandRt`), `FindView`.

### Functions (texture_cache.cpp / .h)

- `RtMaxExtentMode`, `RtFamilyOf`, `RtRaiseLayers`, `RtNoteImage`, `RtNoteServed`, `ExpandRt`,
  `ReportRtMaxExtent` (new).
- `ResolveOverlap` (resources branch): `ExpandRt` instead of `ExpandImage` for target binds when on.
- `FindImage`: memo hit calls `RtNoteServed`; the creation branch uses `RtRaiseLayers` + `RtNoteImage`;
  the end of the function calls `RtNoteServed` for served images (not for creations).
- Header: `IsRtTarget`, the declarations, `RtFamily`, `RtFamilyHash`, `RtExtentEntry`, `rt_extents`, and the
  `<unordered_map>` include.

## Counters (`SOTC RT MAXEXTENT` line, per window, `ReportRtMaxExtent`)

- `max-extent creations`: images created with more layers than the request (table raised them).
- `expands`: target binds that took `ExpandRt` (the resources branch, the layer expansion; the block and
  3D expansions are not counted). With the switch on, these still happen when a request is above the
  family's max.
- `expands avoided`: target requests served by the family's current image where the old path would have
  expanded that image (request layers above the old path's model). Counterfactual: compare with the
  `expands` line of a default run, not with this line alone.
- `views from a larger image`: target requests served by an image with a larger guest size than the request
  (a view), including the ones that came from the table and the ones from an expansion above the request.
- `fallbacks`: the guard refused a raise (non-layer-linear size, levels != 1, zero layers), or the family
  table was full.

## Call-site snippet (not mine: `gpu/shadps4/core/libraries/videoout/driver.cpp`)

Next to the existing line (about line 2046, inside the same report block as `ReportTexIdentity`):

```cpp
VideoCore::TextureCache::ReportTexIdentity(report, seconds); // sotc: SOTC_GEN_TEX
VideoCore::TextureCache::ReportRtMaxExtent(report, seconds); // sotc: SOTC_RT_MAXEXTENT
```

It prints only when the switch is on. Without this snippet the switch still works, only the counters are not
printed.

## How to test in game

1. Build with `bash build.sh` (not run by me). Check that the switch appears in the `SOTC RT MAXEXTENT` line.
2. Lab route, one round (`tools/pcmem_matrix.py --repeat 1`, `user/routes/tests.json`):
   - Config A: default (switch off). Record the `images_new` / image-free counters and `expands` (the
     ExpandImage counters already in the report).
   - Config B: `SOTC_RT_MAXEXTENT=1`. Pass if `max-extent creations` > 0 and `expands` falls against A, and
     no visual regression. Expected: fewer expands and fewer image creations per second (the 120-340
     images/s churn), plus the views-from-larger count.
3. Visual checks: render-target contents at area loads and in the Shrine (shadows, water, depth-dependent
   effects), since the rendered layers are now a larger image.

## Risks

- A raised image carries more layers than the request, so its guest range (and the upload from guest memory
  when it is created dirty) is larger. Expansion today already does the same for the expanded image; the
  switch moves that cost to the first creation of the family. Cost to measure in B.
- The table only grows: after a 6-layer use, a later 1-layer use at the same address creates a 6-layer
  image. This costs VRAM and possibly upload, and is only fixed by a per-address reset (not done).
- The raised guest range is tracked. Other surfaces that sit inside the extra layers can overlap the image
  earlier than they would have (the overlap rules may free it when the other surface is not a subresource).
  The old path had the same overlap, but only after the first bigger request; here it can happen after the
  first smaller one. Watch for render-target flicker or churn in B.
- Serving a view of a larger image relies on the existing "use image view" branch, which already serves
  smaller target requests today. Not new behaviour, but it is used more often now.
- Width, height and pitch are not raised (see the family rule). The largest-extent part of T1 is therefore
  the layer count only. A width/height change at one address stays a free-and-create, as it is today.
- The table is keyed by address and layout, not by a frame or a lifetime: an address reused by a different
  surface with the same layout inherits the family's layer count.

## Verified and assumed

Verified in code:
- Syntax: `clang++ -std=gnu++23 -Wall -Wextra -fsyntax-only` on `texture_cache.cpp` with the flags of
  `out/gpu/compile_commands.json` (`-MD/-MT/-MF/-o` removed, nothing written to `out/`). Exit 0, no
  diagnostics. The real `video_core/sotc_generation.h` was present for this check.
- The bind path through `ResolveOverlap`: equal `BlockDim` (pitch and height in the family) keeps the
  resources branch and the "use image view" branch reachable; `ResolveDepthOverlap` returns the cached image
  for a smaller depth request of the same family (`resources` not less).
- `guest_size` for both target constructors is slice size times layers (`image_info.cpp`), so the raise is
  exact for them. `UpdateSize` (used for mip layouts) is not called on the target path.
- `RtNoteServed` and `RtRaiseLayers` read `slot_images[...]` only for ids that are allocated in the same
  call (the id just returned by `ResolveOverlap`/`FindImage`, or the id just created).

Assumed (not run):
- That the views served from a larger image have the same render output as the old image path (they use the
  same branch as today's smaller-request views, but the RT views from a larger image have not been exercised
  by the game with more layers than they need).
- That a fresh image's first upload is not worse than the old expansion's (same guest-size formula).
- The `image_uid` lookup is the same identity the memo uses (it is, per `TexIdMemo`).

## Not finished

- Not built, not run in game, no measurements.
- Mode 2: not implemented (reason above).
- Width, height and pitch growth (the "largest extent" beyond layers): not done, by design, because it changes
  the family and the overlap rules free the image.
- Pool-allocated (RTAllocator) render targets: the counters include them; no separate treatment.
- Texture-side expansions and mip tails: out of scope (unchanged).
