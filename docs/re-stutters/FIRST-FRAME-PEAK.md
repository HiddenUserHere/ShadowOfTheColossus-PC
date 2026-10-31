# The first-frame peak after a load (design)

Status: design, 30 Oct 2026. Nothing below is built yet except where marked "exists".

## What the peak is

The last stutter the 30 Oct defaults did not touch: the first frame of the world after a save
load or a new session, 166-226 ms (traces `user/trace/_def2` frame 173, `_fill3` frame 474,
`_view2` frame 471, `_warm1` frame 456).

Breakdown of the 225 ms frame (`_def2`, every 30 Oct default on):

| Thread | Time in the frame |
|---|---|
| Draw thread | 72 ms copying guest memory into staging (1,297 bind copies, 242 MiB at **3.3 GB/s**), 34 ms ops, 26 ms texture binds (10 ms image uploads), 15 ms waiting for the GPU, 15 ms page protection |
| Command processor | 73 ms blocked on the draw thread |
| GPU queue | busy 138 ms |
| Game threads | all waiting (event flags, flip) |

The cause, from the trace:
- **First touch.** The frame created ~40 new 16 MiB tracking regions ("redirty first touch",
  640-736 MiB). A region the buffer cache never saw starts all CPU-dirty, so the first bind of
  every buffer in it copies the whole range.
- **The copy is slow because of page tables, not memory bandwidth.** 88% of the draw thread's
  samples inside these copies are on the `memcpy` instruction, at 3.3 GB/s: the speed of the
  first read of a page through a view whose page-table entries are not present (3.2 GB/s in the
  section microbenchmark, against 10.4 GB/s once present). File reads write through the guest
  address (`runtime_file.c`, bounce buffer then `memcpy` to the guest pointer), so the address
  the GPU binds is likely another mapping of the same physical pages (to confirm, phase 0).
- **There was idle time before it.** In the 5 frames before the peak the draw thread was busy
  0-7 ms of each 33 ms, and already copied 48-125 MiB per frame of the same data.
- **Addresses repeat in part across sessions:** 64-71% of the peak's 64 KiB pages match between
  sessions started from the same save, ~47% against a different start.

## Design: load-driven residency

Modern engines do not upload on first use: data read from disk goes to GPU memory while the
load screen is up (DirectStorage, console streaming). The port has the signal for that already:
it performs the game's file reads itself and knows every destination range.

1. **Note what arrives.** `runtime_file.c` reports each completed read into guest memory (the
   destination range) to the GPU library through a lock-free queue, as the write-fault path does
   for `SOTC_BACKING_WARM`. Only ranges in GPU-visible mappings are kept.
2. **Track it from the start.** The tracking region of a noted range is created then, not at the
   first bind, so later CPU writes (decoding in place) fault and are tracked precisely instead
   of the whole 16 MiB region starting dirty.
3. **Upload it in idle time.** The draw thread, when its queue is empty (it waits for work, as
   in the load frames), takes noted ranges and makes them resident: arena residency plus a sync
   copy, the same work a bind does without a descriptor. It stops as soon as work arrives, with
   a time budget per idle gap. Large ranges are split across the copy threads, since nothing
   waits on them.
4. **Pay the page-table cost in the same place.** The copy reads through the address the GPU
   will bind, so its page-table entries are filled during the load, not in the first frame.
5. **Textures:** the same idle executor replays the area manifest (`SOTC_AREA_PREWARM`, exists:
   it records each zone's images) with `SOTC_ASYNC_IMAGE_CREATE` (exists) and also uploads the
   images whose guest memory is complete, batching their detiles while no render pass is open.

Correctness: an idle-time upload is an early bind. The tracker keeps every later write visible,
and a range rewritten after its upload is copied again at its bind, as today. A wrong prediction
costs a copy, not a wrong frame.

Expected effect: the 72 ms of copies, most of the 15 ms of protection and part of the 26 ms of
texture binds move into the load frames, where the draw thread is idle. What stays in the peak is
the frame's own drawing and the GPU time.

## Unknowns, measured first (phase 0)

1. **Lead time:** how long before the first bind each range is complete in memory. Needs the
   stream mode (`SOTC_TRACE=2`) over a whole save load, and the destination address in the
   "file read" span (`b`, today 0).
2. **Share from file reads:** how much of the peak's copy bytes lie in ranges a file read wrote,
   and how much the game wrote itself after decoding.
3. **Alias:** whether the bound address and the file read's address are different mappings of
   the same physical pages (runtime_memory's VMA table gives the physical offset of both).
4. **GPU side:** what the 138 ms of GPU queue time is (`SOTC_TRACE_GPU=1`): uploads and detiles,
   or the frame's own rendering. If the GPU itself needs most of it, the CPU side alone cannot
   remove the peak.

## Phase 0 results (30 Oct, `user/trace/_p0`, one save load)

Stream trace (`SOTC_TRACE=2` with `SOTC_TRACE_ONLY`), new `file dest` and `sync phys` events.
Worst frame 163 ms: 1,459 copies, 205 MiB (180 bind, 25 DMA), 672 MiB of first-touch regions.

1. **Share from file reads: 1%.** The session read 1.5 GB from disk into guest memory, but only
   2 MiB of the peak's copied bytes lie in physical pages a file read wrote. The peak uploads data
   the game wrote itself after reading (decompressed or generated), into memory the buffer cache
   did not track yet. **The load-driven design above does not fit**: file reads are not the
   signal.
2. **Lead time:** not meaningful for 1% of the bytes.
3. **Alias:** inconclusive (2 MiB matched, half through another mapping).
4. **GPU:** `SOTC_TRACE_GPU` recorded no timestamps on this machine (no GPU_CALIB chunk). The GPU
   queue occupancy, a rough proxy, was 113 ms in the peak frame, and the two frames after it were
   close to GPU-bound (78 ms with 60 ms busy, 61 ms with 50 ms busy). The GPU side of the first
   frames is real and needs its own measurement.
5. **Idle time confirmed:** in the 3 s before the peak, copies and uploads took 168 ms of 3,000.

## Revised direction: write-driven residency

The signal is the game's own writes, which the port does not see in an untracked region (a new
region starts all CPU-dirty, with nothing protected, so no write faults). Idea to evaluate:

- **Track GPU-visible mappings from the map**, not from the first bind: at map time the GPU
  library zero-fills the arena range on the GPU (`vkCmdFillBuffer`), marks the range clean and
  write-protects it. Guest writes then fault and mark exactly what was written.
- **Upload written ranges in idle time** (the draw thread's empty-queue gaps during the load, a
  time budget per gap), so the first frame finds them clean.
- **Precondition to check first:** the guest memory must read as zero at map time for "clean" to
  be true. Fresh section pages and released pages (`host_discard`) are zero; a range mapped a
  second time (an alias of memory already written) is not. The runtime would need to know whether
  a physical range was written since its last release, or fall back to today's first-touch path.
- **Cost to measure:** write faults during the load (672 MiB in 64 KiB granules: ~10k faults, on
  the game's loading threads) against the 72 ms of copies and 15 ms of protection removed from the
  first frame.

Next measurement (phase 0b): trace GPU-visible map events (address, size, physical offset, time)
to see when the peak's regions were mapped and whether their physical pages were fresh; and fix
the GPU timestamps so the GPU part of the first frames is measured.

## Phase 0b results (30 Oct, `user/trace/_p0b`, one save load with the Lab)

`SOTC_TRACE_GPU` was off in phase 0 because of a static-initialisation-order bug
(`SotcGpuTrace::kOn` read `SotcTrace::kOn`, an inline variable of another header); fixed, and the
GPU timestamps now record. New `gpu map` / `gpu unmap` events.

Worst frame 208 ms (7.0 s into the session): 1,395 copies, 241 MiB (218 bind, 23 DMA), 752 MiB of
first-touch regions, command processor blocked on the draw thread 96 ms.

1. **GPU execution: 76 ms** of the 208 ms (47 command buffers, real timestamps). The CPU side is
   the larger part, but the GPU alone is already over two frames of work in that frame; part of it
   is the transfer and detile of the very uploads the CPU makes in that frame.
2. **Map time is not a signal.** Every copy of the peak lies in the 8 GiB direct-memory mapping
   the game makes at boot (`0x1040300000`, 8,124 MiB, at 0.0 s). The game sub-allocates inside it,
   so "track from the map" means tracking the whole pool from boot.
3. **That pool also holds CPU data** (the BPE heap arena lives inside Garlic,
   STUTTER-ARCHITECTURE.md). Tracking the whole pool from boot would make every CPU heap write
   fault; today those ranges are never bound, so never tracked. **The map-time design is
   rejected** unless CPU-only ranges are told apart first (the class tracking work).
4. File reads: again 1% of the peak's bytes.

## Current direction: learned working set, warmed in idle time

The data the first frame binds is in memory a few frames before (the frames before the peak
already copy 48-125 MiB each), the draw thread is idle then (172 ms of copies and uploads in the
3 s before the peak), and 64-71% of the peak's 64 KiB pages are at the same addresses in sessions
started from the same save.

- **Learn:** per area (the zone key of `SOTC_AREA_PREWARM`, plus a "save load" key), record the
  buffer ranges and images bound in the first ~2 s after the area starts, next to the image
  manifest that already exists.
- **Warm:** while that area loads, the draw thread spends its idle gaps on the recorded ranges:
  residency plus a sync copy (an early bind), and image creation and upload. It repeats each
  range every few frames, so a range the game rewrites after a warm is copied again then, and
  most of it is clean by the first frame.
- **Correctness** is that of a bind: tracking stays on, a stale or wrong range costs a copy.
- **GPU:** the warmed uploads also move their transfer and detile work out of the first frame.
- **Measure:** the peak's bind bytes and copy time, the first frame's GPU time, and the cost of
  the repeated warm copies during the load.

## Phase 1 first results (30 Oct, `SOTC_LOAD_WARM=1`, `user/trace/_lw1` records, `_lw2` warms)

Built: `gpu/sotc/load_warm.cpp` (keys `gameload` and the area probe's areas, 64 KiB pages bound
in the 20 s after the event, saved in `user/cache/load_warm/`), early binds in `BackLoop`'s idle
branch (1 ms budget, 1 MiB runs, a pass every 250 ms), "sync copy" reason 5. The first session
saved `gameload` with 683 MiB and `boot` with 1.1 GB.

| Peak frame | `_lw1` (records) | `_lw2` (warms) |
|---|---|---|
| Frame | 217 ms | 195 ms |
| Bind copies | 241 MiB, 52 ms | 67 MiB, 10 ms |
| First-touch regions | 752 MiB | 336 MiB |
| Texture binds | 30 ms | 50 ms |
| CP blocked on the draw thread | 93 ms | 119 ms |
| GPU execution | 73 ms | 58 ms |

- The warm did what it was built for: 72% fewer bind bytes and 42 ms less copying in the peak.
- The frame only fell 22 ms: the rest of the draw thread's time in that frame (draws, texture
  binds, pipelines) is not in the filtered trace yet and is now the larger part.
- Frames of 150-181 ms appeared at 20.7-21.2 s, at the end of the warm window (`_lw1`: 100 ms
  there). Suspected cause: every pass calls `ObtainBuffer` for ~680 runs every 250 ms even when
  they are clean, which records no "sync copy" and uses the idle gaps of gameplay frames. The
  warm copied 1.2 GB in 612 ms over 22 s.

Next: a "load warm" span around each idle warm, stop the passes once the peak is past (a pass
that copies almost nothing, or the first heavy frame), and the draw thread's own spans in the
filter to break down the rest of the peak.

## Phase 1 adjusted (30 Oct, `user/trace/_lw3`, `_lw4`)

Changes: `WarmRange` returns the bytes it uploaded; a window stops warming after a pass that
uploads under 4 MiB once the window uploaded 64 MiB or more; 1 MiB runs; a "load warm" span per
idle warm; the filter now holds the draw thread's own spans.

| Peak frame | `_lw1` (no warm) | `_lw3` | `_lw4` |
|---|---|---|---|
| Frame | 217 ms | **142 ms** | **172 ms** |
| Bind copies | 241 MiB, 52 ms | 28 MiB, 5 ms | 17 MiB, 3 ms |
| GPU execution | 73 ms | 49 ms | 51 ms |
| CP blocked on the draw thread | 93 ms | 65 ms | 98 ms |
| Warm stopped at | — | 8.8 s (1.8 GB, 795 ms) | 10.7 s (2.0 GB, 893 ms) |

The late 150-181 ms frames of `_lw2` are gone. The peak's copies are nearly gone; what is left
on the draw thread in the peak (exclusive time):

| Span | `_lw3` | `_lw4` |
|---|---|---|
| ops (per-op work outside any nested span: render state, pipelines, begin rendering) | 38 ms | 45 ms |
| bind textures (outside image upload and find image) | 25 ms | 30 ms |
| sync protect (page protection) | 11 ms | 18 ms |
| bind buffers | 11 ms | 14 ms |
| sync copy | 11 ms | 8 ms |
| wait gpu | 11 ms | 8 ms |

Next: sample the draw thread in the peak (`SOTC_TRACE_SAMPLE`) to name the 40 ms of "ops" (new
pipelines are the first suspect) and the texture bind time; phase 2 then warms textures (the area
image manifest, image creation and upload in idle time) the way phase 1 warms buffers.

## Peak samples (30 Oct, `user/trace/_lw5`, `SOTC_TRACE_SAMPLE=1000`, warm on)

Peak 133 ms, 131 draw-thread samples (~1 ms each), grouped by the innermost draw-thread span:

| Where | ~ms | What |
|---|---|---|
| bind buffers + draw indirect (index buffers) | 37 | `ObtainBuffer`, almost all in `SotcLoadWarm::NoteBind`: the recording walked every page of large buffers (64 MiB pools) on every bind and inserted them into a hash set under the mutex |
| no span (`BackLoop` between ops) | 31 | `CollectFreeTree` 10 (the BPE heap guard reading guest memory), `write_backing_fast` 6 |
| wait gpu + func (`WaitSubmitted`) | 25 | GPU waits in the driver |
| ops | 13 | `PrepareRenderState` |
| copies, protection, textures | ~11 | |

The "40 ms of ops" of the span table were mostly not pipelines: the recording itself was the
largest cost. Fixed: a per-thread cache of the ranges already recorded in the window (4,096
entries, allocated on the draw thread only), so a repeated bind returns before the clock read and
the page walk. Next on the list: the heap guard's `CollectFreeTree` on the draw thread in the
first frame, and the GPU waits.

## After the recording fix (30 Oct, `user/trace/_lw6`)

Peak **121 ms** (217 ms without the warm, `_lw1`). Bind buffers fell from ~37 to ~8 ms of samples.
What is left in the peak (119 samples): texture binds 24 + find image 10 (~34 ms: phase 2),
page protection 18, ops 14 (render state), GPU waits 12, copies 8. The warm stopped at 10.3 s
after 2.3 GB uploaded in 1.15 s. Two frames at 8 s (83-84 ms) each held ~20 ms of warm copies
(42-53 MiB): the warm needs a per-frame byte cap once gameplay frames start.

## Phase 2 first try: textures (30 Oct, `user/trace/_tw1` records, `_tw2` warms)

Built: texture records per key (the T# and the shader's image resource, `<key>.tex.bin`, blob
size in the header and set by the build), replayed once per window before the buffer runs as
`FindImage` + `FindTexture` with the ImageDesc the real bind builds (`Rasterizer::WarmTexture`,
stencil-to-depth redirect, 16 MiB per texture, 32 textures per frame); warm uploads are kept out
of the per-flip texture upload budget (`SotcLoadWarm::InWarm`), and buffer warm copies are capped
at 16 MiB per frame.

- **Crash found and fixed:** the first two warm sessions closed ~6 s after boot, a read fault in
  XXH3 hashing guest memory at 0x108e1dc000 (a texture hash in RefreshImage). An old T#'s range
  can run into unmapped memory, or into memory the GPU writes (read-protected pages the draw
  thread cannot fault on). `WarmTexture` now requires the whole range mapped and no GPU-written
  bytes in it. The third session ran clean.
- **No gain yet:** peak 125 ms with textures warmed (121 ms in `_lw6` without), texture time in the
  peak ~43 ms of samples (~34 before). 5,440 textures were warmed.
- Suspected causes, not verified: the textures are warmed right after the event, before the game
  loads their data, so the real bind tracks and uploads them again; or the texture cache's load
  flush (`load_flush`, set by the same "starting game" event) collects the warmed images before
  the first frame uses them.
- The saved sets only grow (each session adds to the last): `boot` is at the 2 GiB cap and
  `gameload` at 1.9 GiB. They need an age-out (keep pages bound in the last N sessions).

## Why the texture warm did not help (30 Oct, `user/trace/_tw3`)

New trace instants (texture_cache.cpp `TraceWarmTex`): "tex new" (a real bind created the
image), "tex warm hit" (it found an image the warm created), "tex warm reupload", "tex warm freed".

| | Whole session | Second 5 (the first world frame) |
|---|---|---|
| tex new | 1,603 | 861 |
| tex warm hit | 16 | 12 |
| tex warm freed | 138 | — |
| tex warm reupload | 1 | — |

The warmed images were neither collected nor uploaded twice: the real binds did not find them,
because the T#s recorded in one session seldom match the next session's (address, size, format
and extent must all match). **The T#-record texture warm is moved behind its own switch,
`SOTC_LOAD_WARM_TEX=1`, off**; `SOTC_LOAD_WARM` warms buffers only.

Also seen: this session's first world frame took **77 ms** (frame 156, 553 new textures, 75 MiB
copied), against 121-172 ms in the earlier warm sessions. It ran without the stack sampler and
with a smaller trace filter: the 1 kHz sampler suspends every thread 1,000 times a second, so the
sampled sessions overstate the peak. The buffer warm's real gain needs an A/B with one light trace
setup, warm off then on.

## Phases (original plan, kept for reference)

| Phase | Work | Done when |
|---|---|---|
| 0 | Trace fields (file read destination, physical offsets), one stream-traced save load with GPU timestamps | the four unknowns have numbers |
| 1 | Notes from file reads, early tracking regions, idle-time residency for buffers (`SOTC_LOAD_RESIDENCY=1`, off) | the peak's bind copies fall from ~240 MiB |
| 2 | Idle-time texture creation and upload from the area manifest and completed memory | the peak's texture binds fall |
| 3 | If phase 0 shows GPU-bound time: batch the uploads and detiles on the GPU (one pass per idle gap) | the GPU queue time of the peak falls |
