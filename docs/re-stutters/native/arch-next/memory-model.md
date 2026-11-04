# Memory model for the native port: target design (M1)

Status: design only. Written 4 Nov 2026 by a read-only pass over the docs, the code and the logs. No edits, no builds,
no game runs. Scope: how guest memory is placed, uploaded, read back and protected. The command path (PM4 parsing,
draw thread work) is covered by the other briefs; the consumer list of GPU results is M2's (`readbacks.md`, not present
when this was written; the classes below come from `docs/re-stutters/native/gpuresults.md`).

Labels: **measured** (a log line, cited), **code** (a file:line read here), **reported** (an older doc, not re-checked),
**inferred** (reasoning from the above), **open** (needs a run).

Log names are relative to `C:/Users/ptr/Documents/sotc-pc/`. "Window" means one 5 s report; windows are listed in the
order they appear in the log.

---

## 0. Summary

1. **The default config already does not protect pages.** `sotc.ini` sets `SOTC_NATIVE=1` (line 7). That turns the
   page tracking off (`gpu/sotc/native/native.h:39-41`, `TracksGuestPages()` is false in mode 1). In the logs made in
   this config: 0 write faults per second, 0 tracked readbacks ("readbacks skipped 0", `user/gcn-run1/s14v.log:595`,
   `user/gcn-run1/base.log:557`). The fault and readback numbers in `PC-MEMORY-REWRITE.md` (1 Nov, 3,000-7,000
   faults/s, 730-1,070 readbacks/s) are from `SOTC_NATIVE=0`. They describe the tracked model, not the default.
2. **The default still waits and copies.** Per 5 s window, summed over threads: 0.7-2.9 s of frame-slot waits
   (`user/gcn-run1/s14v.log:592`: 3,614 waits, avg 0.62 ms), 5.6-81 MB of draw-thread uploads, and spikes where one
   frame copies 129.5 MB (`user/gcn-run1/base.log:653`). The 1% low is 5-13 FPS in several windows
   (`base.log:464,657,838`; `s14v.log:487,672`).
3. **Correctness of the default is not settled.** The native model serves GPU results from copies of the previous
   completed frame (`SOTC NATIVE_MASKS prev ... served older 1779`, `s14v.log`). The flicker work measured a
   frame-to-frame difference of 1.51 against 0.86 for the tracked reference (`docs/re-stutters/native/FLICKER-BRIEF.md`,
   section "Remaining bugs and evidence"). Any target model must remove this stale-copy path, not just the faults.
4. **Target.** Classify by use, not by PS4 memory type. Three placements: (a) CPU-written, GPU-read per-frame data is
   host-imported and read in place (zero copy, zero faults; this is ring8, already built); (b) static data is copied
   once to device-local memory when the loader publishes it, and imported until that copy is ready; (c) GPU-written
   results are host-imported in place, and the CPU reads them after the game's own label, with no readback. CPU-only
   data is not touched. Protection is removed per class; in the end state no class is protected.
5. **Budget.** The target removes host staging (0.45-0.9 GB measured in the default) and device-local staging
   (0.18-0.21 GB measured). It adds no VRAM and no RAM beyond the optional third frame slot (+35-50 MiB, section 5 M5).
   Imported memory is guest memory the game already commits, so it is not extra memory, but it is pinned.
6. **What this will not do alone.** The draw thread bounds the frame (`PC-MEMORY-REWRITE.md` section 6: about 34.5 ms per
   frame, and no switch moved FPS). The memory model targets the stutters (p99, max, guest-thread stalls and draw-thread
   copy spikes), not the average frame.

---

## 1. Current state

### 1.1 What the user's default runs

| Switch (sotc.ini) | Value | Effect | Ref |
|---|---|---|---|
| `SOTC_NATIVE` | 1 | Page tracking off; CPU writes reach the port by announce (per-memcpy hooks) or ring host import | `sotc.ini:7`, `native.h:35-41` |
| `SOTC_PC_RING` | 8 | Per-frame arena imported as host memory; binds served in place; EOP labels deferred | `sotc.ini:6`, `sotc.py:563`, `pc_ring.h:58-85` |
| `SOTC_HOOK_ALLOC_PROBE` / `SOTC_HOOK_MEMTYPE_PROBE` | 1 / 1 | Allocator and memtype probes (also forced on by `SOTC_NATIVE`) | `sotc.ini:24,26`, `sotc.py:570-573` |
| `SOTC_HOOK_FRAME_FLIP` | 1 | Frame boundary hook (flip wait, per-frame figures) | `sotc.ini`, `sotc.py:564-566` |
| `SOTC_PRELOAD_UPLOAD` | 2 | Idle upload of ranges the loader built (`preupload.h`) | `sotc.ini:55` |
| `SOTC_NATIVE_MASKS`, `SOTC_NATIVE_PARTICLES` | prev | GPU results served from the newest complete GPU copy | `sotc.ini`, `native_copies.h:1-10` |
| `SOTC_NATIVE_COPIES_PAGES` | 1 | Copy-ring page bookkeeping | `sotc.ini` (last line) |

Not in the ini, so default on: async readbacks (`SOTC_RB_ASYNC`, `buffer_cache.cpp:1259`), the precise flip wait
(`SOTC_PC_RING8_FLIPWAIT=2`, `HANDOFF.md`), deferred labels in mode 8 (`SOTC_PC_RING8_LABELS`, `HANDOFF.md`).

Probes cost: the memtype, classes and class-track probes together cost 2.6-3.6 FPS in one configuration
(`docs/re-stutters/HANDOFF.md`, "Heap serving measured"). Two of them are on in the default. This is reported, not
re-measured on the default.

### 1.2 Numbers: tracked model against the default

Tracked model: `user/pcmem/20261101-075954/logs/base-r1-a1.log` (SOTC_NATIVE unset, SOTC_PC_RING=0) and
`ring8-r1-a1.log` (same, with ring8). RTX 3060 Ti (`base-r1-a1.log:11`, `GPU: NVIDIA GeForce RTX 3060 Ti`).
Default: `user/gcn-run1/base.log` and `user/gcn-run1/s14v.log` (SOTC_NATIVE=1, 4 Nov, same GPU family, Lab route).

| Metric (per 5 s window) | Tracked, base | Tracked, ring8 | Default (native + ring8) |
|---|---|---|---|
| Write faults, eboot | 16.1k, 26.4k, 27.8k, 30.2k, 33.3k (3.2k-6.7k/s) (`base-r1-a1.log:322,417,...`) | 3.2k, 9.0k, 11.1k, 14.5k, 19.4k (`ring8-r1-a1.log:281,388,...`) | 0 (`base.log:520`, `s14v.log:555`) |
| Fault time, summed | 0.6-2.3 s (avg 22-144 us per fault, `base-r1-a1.log:322`) | 0.1-1.4 s (avg 14-427 us) | 0 |
| Readbacks (RIPS) | 54, 787, 748, 736, 796 /s (`base-r1-a1.log:308,402`) | 52, 1,053, 1,073, 875, 862 /s (`ring8-r1-a1.log:267,373`) | 0 (`s14v.log:595`) |
| Readback wait, summed | 4,593 (first window, load), 2,621, 1,970, 3,219, 3,068 ms (`base-r1-a1.log:264,357`) | 4,591, 2,324, 1,823, 2,025, 2,597 ms (`ring8-r1-a1.log:214,321`) | 0 readback waits; 0.7-2.9 s of frame-slot waits (`s14v.log:592`) |
| Working set | 5.26, 5.50, 5.52, 5.75, 5.99 GB (`base-r1-a1.log:285,379,...`) | not printed | not printed (no `SOTC_RAM_REPORT` in the native runs) |
| VRAM used (port's count) | 2.2-3.1 GB (`base-r1-a1.log` `SOTC VRAM` lines) | 2.0-2.9 GB | 2.0-2.9 GB (`base.log`, `s14v.log:505`) |
| 1% low FPS | 28.2, 10.2, 26.2, 10.9, 20.0 | 25.8, 19.7, 27.8, 28.3, 17.3 | 29.0, 5.4, 9.2 (`base.log`); 13.1, 11.9 (`s14v.log`) |

Reading: ring8 removed most of the eboot write faults and most of the fault time, and did not remove readbacks. Its
readback count is not lower (860-1,070/s, against 740-800/s in the tracked base windows). The default removes both, but at the price of
stale GPU results (section 1.8) and frame-slot waits.

### 1.3 Uploads (draw thread)

- Steady: `SOTC NATIVE uploads` and `exact uploads` (`s14v.log:592,770`; `base.log:765`): 0.9k-30k uploads per 5 s,
  5.6-81 MB per 5 s (about 1-16 MB/s). In the tracked model the DMA sweeps cost 19.5-35 ms per second
  (`PC-MEMORY-REWRITE.md` section 6, "Second run"); in the default 4-8 ms per second (`base.log`, "DMA sweeps").
- Spikes: a frame that enters an area copies 129.5 MB in 396 copies (`base.log:653`, worst frame 283.8 ms, "copy 13.4
  ms"). The texture upload path also detiles on the CPU, 50.8 ms in another worst frame (`base.log:460`).
- Page faults dominate the copy cost. `base.log:460`: 25.9 MB copied in **68.2 ms** with **42,838** process page faults
  (0.38 GB/s). `base.log:653`: 129.5 MB copied in **13.4 ms** with **745** faults (9.7 GB/s). The fault count, not the
  byte count, sets the cost (counter: `SplitProcessFaults`, `gpu/shadps4/video_core/sotc_stats.h:1224`).
  **Inferred:** the faulting pages are first-touch pages of freshly committed destination or source memory. The
  split between the two is open.
- Bind-time big copies with no loader prediction: `user/gcn-run1/s14v.log` "PRELOAD UPLOAD big copy ... no built range"
  (about 8 MB each, early windows). `SOTC_PRELOAD_UPLOAD=2` covers the loader's ranges only (`preupload.h:1-20`), so
  these copies still happen on the draw thread at bind.

### 1.4 GPU results read by the CPU

Consumer classes (from `docs/re-stutters/native/gpuresults.md`, rates from the tracked lots, which are the only ones
with readback counts):

| Class | What | Share of readbacks (tracked) | Today in the default |
|---|---|---|---|
| A | XPR visibility and decal masks (C1-C4, C13) | 40-45% | copy ring, previous complete frame (`native_masks.cpp`) |
| B | Particle counts (C5, 0x101a98) | about 25% | copy ring (`native_particles.cpp`), `served older` counted |
| C | Water query (C6, 0x11c) | about 3% | not located; consumer not found (`gpuresults.md`) |
| D | CPU data on pages the GPU also writes (page sharing) | about 25% | none (tracking off) |
| E | CPU spin on a GPU label | 0 readbacks, one spin | frame-slot waits (below) |

Measured in the default: masks served older 1,779 per 5 s and zero masks 822 per 5 s (`s14v.log`, `NATIVE_MASKS` line,
"no copy 822"). Copy ring: 509 GPU copies per second, 16 MiB ring (`gpu/sotc/native/native_copies.h:24`).

Note on class D: it exists only because tracking is page-granular. Class D data is not a GPU result. It costs readbacks
only when a GPU-written page also holds CPU data.

### 1.5 Waits

- Readback waits (tracked): 1.8-3.2 s summed per 5 s; average 330-550 us each, max 20-93 ms (`base-r1-a1.log:357`,
  `ring8-r1-a1.log:321`). Waits happen on guest threads, not on the draw thread (`PC-MEMORY-REWRITE.md` section 6).
- Frame-slot waits (default): `FrameWait` from the engine's two-deep frame slot spin, `native_sync.cpp:42-52`
  (`SlotSpinPre`) and `WaitReuse` (`native_sync.cpp:62-69`). Measured: 442-4,775 per window, avg 0.46-1.68 ms
  (`s14v.log:592`; `base.log`, native windows). Summed: 0.7-2.9 s per 5 s. **Inferred:** these waits happen because the
  GPU trails the CPU by about one frame, and the engine recycles frame N-2's slot (`HANDOFF.md`: "the GPU lags ~1 frame"). The memory model cannot remove that by
  itself (section 5 M5).

### 1.6 Memory today

Host RAM (tracked lot, `base-r1-a1.log:285-302`):
- Process working set 5.3-6.0 GB; private commit 5.6-6.6 GB.
- Guest pool: resident 0.36-0.85 GB (sampled, `SOTC RAM guest pool`); the report also prints "committed 0.0 MiB of the 9.9 GiB view", which does not fit the other lines and is not explained here.
- Private remainder 4.3-5.2 GB, **not attributed** (driver, Vulkan host memory, shader compiler, untracked; the
  largest named blocks are 0x35740000 about 650 MB, 0x333b0000000 about 555-680 MB, mimalloc 555-680 MB).
- Vulkan staging held (`SOTC RAM host staging held`): host-uncached 272-624 MiB, device-local 176-208 MiB, host-cached
  16-18 MiB.
- "Outside VMA" raw `vkAllocateMemory` 1.4-1.5 GB (staging rings, host import, driver; `base-r1-a1.log` line 302).

Default (native): `SOTC ALLOC` summary, `user/gcn-run1/s14v.log:488`: staging-block 28 (448 MiB), staging-large 6 (480
MiB), arena 1,952 allocations (827.5 MiB in the window). `base.log:465`: staging-block 29 (464 MiB), staging-large 4
(320 MiB, 52 ms to allocate). The default RAM total is **not measured**: the native runs did not set `SOTC_RAM_REPORT`.

Device memory: heap 0 device-local budget 7,256 MiB; heap 2 is the small device-local host-visible heap, budget 79 MiB
(`base-r1-a1.log`, `SOTC RAM vulkan heap 2`). Port VMA: device-local 1.2-1.5 GB; buffer arenas 0.5-1.3 GB; images
0.65-1.0 GB (`SOTC VRAM`). Staging device-local 176-208 MiB. Allocator probe, default (`base.log:521-533`): GPU PageAllocator 1.35-1.47 GB
live, GPU HeapAllocator 1.14-1.18 GB live, RTAllocator 322 MiB (`base.log:524-529`).

### 1.7 Allocation classes today

- The port can see the owner of each block (allocator name and kind: `SOTC ALLOC OWNER`, `s14v.log`). Kind 1 is CPU;
  kind 2 is GPU. But the PS4 memory type does not separate CPU and GPU data. A BPE heap arena sits inside the GPU pool
  (`docs/re-stutters/reports/04-memory-model.md`, section 1).
- Memtype slots decide more: `RenderSingleFrameGPU` (slot 16) is CPU-written, GPU-read; `RenderSingleFrameCPU` (slot 17)
  carries GPU-written XPR masks (conflict settled, `PC-MEMORY-REWRITE.md` section 6); `CPUMemory` (slot 1) is mostly
  CPU-only, but its granules get GPU binds (935 bound of 32,797) and take faults (2,415/s attributed,
  `docs/re-stutters/HANDOFF.md`, 1 Nov).
- So neither the allocator kind nor the memtype is a final class. A class is a hint at allocation, corrected at the
  first GPU use. The corrections are the port's own bind and write events.

### 1.8 Correctness state of the default

- Native model is the default, yet not verified end to end. `FLICKER-BRIEF.md` and `FLICKER-BRIEF2.md` list: grass flicker (1.51 vs 0.86
  reference), depth bugs from stale masks when turning fast, "zero masks" right after load, and a load-time hang when
  the CPU reads of GPU-written memory are removed (`FLICKER-BRIEF2.md`, lines 34 and 43).
- Unannounced writes: 57,458 per session in mode 2 (44,174 consumed), mostly to memcpy from 0x101ae4, the command
  processor and small binds (`docs/re-stutters/native/verify6-consumed.md`). Those were measured with tracking on.
- `SOTC_STALE_CHECK` and the byte oracle found no stale page in the tracked lots (`PC-MEMORY-REWRITE.md`, section 8).

---

## 2. Target model

### 2.1 Classes

| Class | Who writes / reads | Placement | Protection | Upload or import | Wait |
|---|---|---|---|---|---|
| **CPU_ONLY** | CPU only; C++ heap, BPE metadata, command buffers read by the port's command processor | host RAM, never imported | none | none | none |
| **STREAM** (per-frame) | CPU writes each frame; GPU reads in the same frame (`RenderSingleFrameGPU`, the Default TaggedArena blocks, descriptors, instances, view constants, particle input) | host RAM, imported (ring8) | none; the frame-slot wait orders reuse | imported; GPU reads host memory in place | none in the port (engine's frame slot) |
| **STATIC** | CPU writes once at build (resource creator, mesh and texture); GPU reads forever | device-local copy; imported until the copy is ready | none; verify hash of a sample | one copy at publish (transfer queue) | none (import serves until copy ready) |
| **STREAM_MIP** | the mip stream copy (0x98ae0), once per mip change | as STATIC, per copy | none | copy at each mip change | as STATIC |
| **RESULT** | GPU writes (masks, particle counts, LOD banks, water query); CPU reads later | host RAM, imported (in place) | none | imported; the GPU writes host memory | the game's own label; deferred in the port until the GPU is done (ring8 rule) |
| **RT** | GPU writes and reads (render targets) | device-local only | none | none | none |
| **GPU_SCRATCH** | GPU only (compute outputs, LDS-backed or GDS) | device-local | none | none | none |

CPU_ONLY and RT need no hook at all once their class is known; the port only has to prove nothing GPU binds them (a
bind feed, section 2.2).

### 2.2 Classify at allocation, promote at first GPU use

Options evaluated:

- **A. Classify only at allocation (allocator kind, memtype slot).** Fast (no later work). Wrong for `CPUMemory`
  granules that the GPU binds, and for `RenderSingleFrameCPU` where the GPU writes. Its errors show up as faults or
  stale data, not as a crash. Rejected as the sole rule.
- **B. Classify only at first GPU use (bind or write).** Correct by construction. The first use is then a stall (the
  copy or the fault happens at that moment). That is the cost we are removing. Rejected as the sole rule.
- **C. Hybrid (recommended).** Allocation sets the hint (memtype slot, allocator kind; `probe_memtype.cpp`,
  `probe_alloc.cpp`). The first GPU bind or write of a granule promotes it (one way; no demotion). The promotion
  point already exists: `BufferCache::PcHostBind` (`buffer_cache.h:913`) sees every GPU read of guest memory, and the
  write side is known from the shader's `Info::writes_memory` (ARCH-GCN-NATIVE S5) and from the port's own EOP label
  writes. Cost: one granule table lookup per bind; the table exists (`mem_classes.cpp`, granule table, 131,072 entries).

What the bind feed must prove for CPU_ONLY and RT: no GPU bind or write of a granule in those classes. A violation is
counted and promotes the granule (a "class trip"). `SOTC_CLASS_TRACK=2` already counted 0 trips in every lot that ran it
(`PC-MEMORY-REWRITE.md`, section 8). Its cost was the bind fast path (`HANDOFF.md`, "CPU_ONLY ... bind fast path fixed the
cost"), so the feed must be on the bind path and cheap.

### 2.3 CPU-written, GPU-read: the placement ladder

Four ways to get CPU-written bytes to the GPU, from best to worst for this game:

| Option | Mechanism | RAM | VRAM | CPU cost | GPU read cost | Where it applies |
|---|---|---|---|---|---|---|
| **Z1. Host import, read in place** (VK_EXT_external_memory_host) | the guest range is imported; the shader reads host memory over PCIe (or system RAM on an iGPU) | 0 extra (guest memory, pinned) | 0 | one write, the game's own; no copy | PCIe reads; measured 5.5 GB/s GPU reads on an RTX 2060 SUPER (`PC-MEMORY-REWRITE.md`, "Ring apply") | STREAM (per-frame): ring8 today |
| **Z2. Device-local host-visible (ReBAR / SAM)** | the CPU writes straight into VRAM | 0 | the bytes | CPU writes to write-combined memory; **CPU reads are very slow** | VRAM speed | Only write-only data. Unsafe for arenas unless proven write-only: the arena allocator writes its own headers into the arena (`HANDOFF.md`, "Arena writers after submit") |
| **Z3. Staged copy** (today's path) | CPU copies into a staging buffer; GPU copies to VRAM | staging, 0.45-0.9 GB measured | the copy | one full CPU copy, on the draw thread (today) | VRAM speed | Fallback when import is refused |
| **Z4. Import, then GPU copy** (SOTC_HOST_IMPORT) | the guest range is imported; the transfer queue copies to VRAM | 0 (no staging) | the copy | none on the CPU | one copy | STATIC uploads, big ranges |

Recommendations:
- STREAM: stay on Z1 (ring8 today). Keep Z2 off for arenas.
- STATIC and STREAM_MIP: Z4 (one copy at publish, on the transfer queue); Z3 only where import is refused.
- RESULT: Z1 for GPU writes (section 2.5); no copy.
- Z2 only when the device has a large device-local host-visible heap (`vk_instance.cpp:1516`, threshold 256 MB), for
  write-only staging of uploads, under verify. This machine has none (section 4).

PCIe cost of host reads (inferred from measured numbers): the per-frame arena is about 15 MiB per frame
(`PC-MEMORY-REWRITE.md`, section 6 second run: 13.8-15.1 MiB/frame). At 30 FPS that is about 0.45 GB/s; at 60 FPS about
0.9 GB/s; each byte may be read by several draws, but L2 is small (3-4 MB on these cards), so reuse across draws is
limited. Both are far below the 5.5 GB/s measured. The unknown is the efficiency of many small reads (each PCIe
transaction carries 32-64 bytes). **Open:** GPU time of the frame with ring8 against the tracked model (SOTC_TRACE_GPU
was not run on both). Until that is measured, the claim "host reads are cheap" is an inference.

When a copy to VRAM is still better: meshes and textures read by thousands of draws per frame (ARCH-GCN-NATIVE 4.1
keeps hot meshes device-local). Those are STATIC, and they are copied once.

### 2.4 Streaming loads: upload at publish, serve from import until ready

- The loader writes the GPU copy of a mesh or single-block texture before it publishes the resource (`loader.md`,
  section 0: "The GPU copy is complete before publish"). Publish is `0x14d670` (`loader.md`, section 1). At that point
  the data is final: no later loader write (`loader.md`, section 6a: "these copies are never written after publish").
- Hook: publish (or the existing `RES_LOADED` hook, `SOTC_RES_LOADED`, `sotc.py` pretranslate path) queues the
  published ranges for a transfer-queue copy with the existing budget (`SOTC_UPLOAD_BUDGET_MB`, 32 MB, `loader.md`
  section 6a). `preupload.h:47` (`NoteLoaded`) already records loader-built ranges; the change is to upload them at
  publish instead of only when idle.
- **Serve until ready:** while the copy is in flight, the bind uses the imported range (Z1 or Z4 source), which is valid
  because STATIC data is never written after publish. The bind switches to the device copy when the transfer completes.
  No stall in either case. This is what removes the "no built range" bind copies.
- Mip streaming (`0x98ae0`, render job; `loader.md` section 5): one memcpy per mip change. Announce it as STATIC per
  copy and upload from the import. Texture-cache work per mip is separate (detile, image update) and is not solved here.

Expected: removes the draw-thread big copies (129.5 MB in one frame, `base.log:653`) and the bind waits for them.

### 2.5 GPU-written, CPU-read: import in place, no readback

The classes that need the CPU to read GPU output (section 1.4) get the same treatment as STREAM, in the other direction.
The port already does this for arena blocks: GPU writes into served ranges go straight to host memory
(`pc_ring.h:58-85`, mode 8). The missing parts:

- **Placement.** The XPR masks sit in `RenderSingleFrameCPU` (slot 17, a CPU-kind heap). The particle stats and the water
  ring are in other heaps (`gpuresults.md`). The rule must be by use: any range the GPU writes is imported as RESULT,
  whatever its heap. That also removes class D (CPU data on GPU-written pages): the import is per range, so a page
  shared by a CPU structure and a GPU result is read in place with no copy. **Inferred** from the ring8 rule (`pc_ring.h`,
  "served in place").
- **Ordering.** The CPU may read a RESULT byte only after the GPU has finished the work that wrote it. The game waits for
  that with its own labels (EOP, ReleaseMem, EventWriteEos). ring8 delays the label the port writes until the GPU is done
  (`liverpool.cpp`, EventWriteEop; `pc_ring.h:58-85`, deferred labels). ReleaseMem and EventWriteEos labels are **not
  covered yet** (`pc_ring.h` mode 8 note: "Open until a log shows the game reading served data behind one of them").
  This is the one correctness item that must close before the readback is removed.
- **Skip proof.** Some results may never need the CPU at all. Class C (the water query at +0x11c) has no consumer found in
  the decompile (`gpuresults.md`, "Resolved open questions", item 1). The rule for skipping: no read of the range in a full
  route, under verify (section 5 M2). M2 decides the rest.
- **Latency.** The game reads masks two frames after the GPU writes them (`PC-MEMORY-REWRITE.md`, section 6: "the masks are
  read two frames after their write"). So the game tolerates a delay of about two frames. An exact in-place read after
  the game's own label matches the PS4 behaviour. The current copy ring instead serves a copy of the previous completed
  frame, which is a different value. **Inferred:** the stale-copy path is a cause of the flicker and the depth bugs in
  section 1.8; the in-place path removes that cause. **Open:** not measured.

Effect on the readback counts (tracked lots, measured): about 730-1,070 per second and 1.8-3.2 s of guest waits per 5 s
(section 1.2). In-place RESULT removes both, and removes the copy-ring work (copy ring 16 MiB, 509 copies per second).

### 2.6 No page protection at the end state

What replaces the tracker for each class:

| Class | Tracker today | End state |
|---|---|---|
| CPU_ONLY | none needed in default (tracking off); faults in tracked mode | nothing; bind feed counts trips (verify only) |
| STREAM | arena: write faults until ring8; per-memcpy announce (native) | import; reuse safety from the frame-slot wait. The flip wait (`SOTC_PC_RING8_FLIPWAIT=2`, precise) is the port-side check that the GPU finished the half being reused |
| STATIC / STREAM_MIP | write faults; announce (native) | copy at publish; verify: sampled hash of guest bytes against the device copy (`SOTC_STALE_CHECK` style) |
| RESULT | read faults and readbacks; copy ring (native) | import; the game's label is the ordering |
| RT | none (RT class, `SOTC_RT_CLASS`) | nothing; verify counter for any CPU read |
| GPU_SCRATCH | none | nothing |

The per-memcpy announce hooks (`gmem` in `SOTC NATIVE WRITES PF`, `s14v.log:597`: 2.33M announces and 4.8 GB announced
bytes in the first window, 0.65M and 1.3 GB in the second) are needed for STATIC and for the copies, not for STREAM, which
is imported and read live. Removing them for imported ranges is a CPU saving; its size is **not measured** (the announce
cost per call is unknown).

Verify modes, not protection, carry correctness after this (`SOTC_STALE_CHECK`, hash of a sample of STATIC pages at each
bind; mode 2 byte oracles on STREAM binds).

### 2.7 Options considered and rejected

- **Keep the tracker for everything.** The status quo in the tracked model: 3-7k faults per second, 1.8-3.2 s of guest
  waits per 5 s, and the draw thread at its limit. Rejected.
- **Keep the copy ring and stale serves for results.** Fast to keep; serves a frame that the PS4 would not have shown.
  Rejected once RESULT import works, kept until then.
- **Device-local host-visible for everything (Z2).** Needs ReBAR or SAM and it is unsafe for arenas (CPU reads). Rejected
  as the default; allowed only for write-only staging under verify.
- **Pure CPU readback copies (more of the existing path).** Rejected: the readbacks are the cost.
- **A PS4-style unified heap in one VkBuffer.** Would make the whole guest space a device address space (ARCH-GCN-NATIVE
  4.1, AddrTable). Costs 32 MiB of device memory for the table (`addr_table.h:50`) and needs the BDA path, which is unsafe
  with ring8 today (`HANDOFF.md`). Kept as a later option, not part of this design.

---

## 3. Budget: today against the target

Numbers are measured where cited; "est." marks an estimate from the cited sizes. Target = all of section 2 implemented.
No item adds memory except where marked.

| Item | Today (default) | Target | Change |
|---|---|---|---|
| Host staging (VkBuffer host-uncached, host-cached) | 272-624 MiB tracked; staging-block 448 MiB + staging-large 480 MiB in default (`s14v.log:488`) | upload fallback ring, 32-64 MiB (est.) | **-0.4 to -0.9 GB RAM** |
| Device-local staging | 176-208 MiB (`base-r1-a1.log`, `SOTC RAM host staging held`) | 0 or the same ring (est.) | **-0.18 to -0.21 GB VRAM** |
| Imported guest memory (pinned) | 96-128 MiB (ring8 `blocks imported 3-4`, `ring8-r1-a1.log:261,367`) | 128-256 MiB (arena plus results) | 0 extra (guest memory); pinned |
| Arena device copies | part of buffer arenas 0.5-1.3 GB; split not measured | 0 for STREAM (imported only) | **-small** (share to measure, M0) |
| GPU result copy ring | 16 MiB (`native_copies.h:24`) | 0 | **-16 MiB VRAM** (the ring is host-readable; check the memory type) |
| Frame slot 3 (optional, M5) | 2 slots | 3 slots of per-frame copies | **+35-50 MiB RAM (est.)** |
| AddrTable (only if GCN S1 is on) | off | 32 MiB device (`addr_table.h:50`) | +32 MiB VRAM, optional |
| Private remainder | 4.3-5.2 GB, unattributed | unchanged until M0 splits it | unknown |
| Working set | 5.3-6.0 GB (tracked lot) | est. -0.4 to -0.9 GB from staging | to measure (native runs need `SOTC_RAM_REPORT=1`) |

Net (est.): RAM -0.4 to -0.9 GB (+0.05 GB with M5); VRAM -0.2 GB or so, plus or minus the arena device copies. The user's
"no extra RAM/VRAM" holds for every item in this design.

Machine: heap 1 (system) budget 15,578 MiB (`base-r1-a1.log`, `SOTC RAM vulkan heap 1`), which suggests 16 GB of RAM
(inferred). Pinned imports reduce the RAM the OS can reclaim; with a 5-6 GB working set this is not the limit, but it is
on a 16 GB machine.

---

## 4. Vendor differences

Measured here: NVIDIA RTX 3060 Ti (logs) and RTX 2060 SUPER (the import read rate, `PC-MEMORY-REWRITE.md`). AMD: the
readback path was measured on a Radeon RX 6650 XT (`buffer_cache.cpp:1259`, `buffer_cache.cpp:5142`), not the memory
model. Intel: only the Iris Xe feature gate (no shaderInt64, `vk_instance.cpp:208`). No vendor other than NVIDIA has run
the memory model.

| Case | Z1 import | Z2 device-local host-visible | Staging, Z4 | Notes |
|---|---|---|---|---|
| NVIDIA, ReBAR off (this machine, inferred) | yes (measured) | no large heap: heap 2 is the 256 MiB window, OS budget 79 MiB (`base-r1-a1.log`) | Z4 to be re-measured (earlier "slower than the CPU copy" result on bulk, `PC-MEMORY-REWRITE.md`) | The existing check (`vk_instance.cpp:1516`) finds no large heap |
| NVIDIA, ReBAR on | yes | large heap (about VRAM size); usable for write-only staging | Z4 | Z2 still unsafe for arenas |
| AMD, SAM on | yes (expected; not measured) | large heap; SAM is ReBAR | Z4 | AMD had the readback issue with async copies (`buffer_cache.cpp:5142`); test the copy path |
| Intel Arc | yes (expected; not measured) | ReBAR-type heap; behaviour not measured | Z4 | Some Intel drivers limit import size and alignment; the port checks the import alignment (`host_import.cpp:32,57`) |
| Intel iGPU (Iris Xe) | yes; device-local and host-visible are the same RAM | not applicable | no staging needed (Z4 is a RAM copy) | VRAM budget is system RAM: the 2-3 GB VRAM use is a RAM cost there |
| Any GPU without VK_EXT_external_memory_host | no | depends | Z3 (today's path) | The design degrades to today's copies; the port must keep Z3 |

Points to test on each vendor: import alignment and maximum size; whether coherent host memory has to be invalidated
for CPU reads after a GPU write (the ring8 path reads after the game's label, so the fence ordering is what matters);
the cost of GPU writes into imported host memory (posted writes over PCIe).

Integrated GPUs: a RESULT or STREAM import is memory the GPU and CPU share anyway, so Z1 is the natural path and the
"VRAM" budget is the RAM budget.

---

## 5. Step path

Each step is off by default, behind its own switch, with a verify mode (`=2`: run both paths and compare, count
differences). Gains are the measured costs the step removes; a step does not claim more than its line in section 1.

| Step | Switch (new unless marked) | What it does | Verify (`=2`) | Expected gain (basis) |
|---|---|---|---|---|
| **M0. Measure, no behaviour change** | `SOTC_MEM_CLASS_LOG=1` (new), `SOTC_RAM_REPORT=1` (exists) | (1) Per granule: memtype slot, allocation kind, GPU binds, GPU writes, CPU faults per 5 s, to test the classes in 2.2. (2) Native-mode RAM report, to fill section 1.6. (3) Split the 4.3-5.2 GB private remainder by owner (staging, import, caches, pipelines, driver). (4) Attribute the frame-slot waits to their callers and measure GPU lag per frame. (5) Fix `unique read/written MiB/frame` (printed 0.000 in every log, so not working). | n/a (observe only) | Not a speed gain. It sets the size of every later gain. One route run. |
| **M1. Probe cost in the default** | `SOTC_HOOK_MEMTYPE_PROBE` (exists, default 1) | After M0 shows which probe output is still used, keep only that one. The native memtype post phase announces `RenderSingleFrameCPU` blocks (`sotc.py:570-573`), so this step needs that replaced first | Class log equal with and without | 2.6-3.6 FPS (reported, one configuration, `HANDOFF.md`). Re-measure on the default first |
| **M2. RESULT import** | `SOTC_RESULT_IMPORT=1` (new) | GPU-written ranges of classes A, B (and C if a consumer exists) are imported and read in place after the game's label; the copy ring is retired for them. Needs the EOP/ReleaseMem/EventWriteEos cover (2.5) | `=2`: keep the tracked readback for each consumer read; compare bytes, require 0 differences over a full route | Tracked lots: 730-1,070 readbacks/s and 1.8-3.2 s of guest waits per 5 s to about 0 (measured counts; FPS effect small, the draw thread bounds the frame). Default: removes stale serves (1,779 per 5 s, `s14v.log`); flicker effect **unmeasured** |
| **M3. STATIC upload at publish** | `SOTC_STATIC_UPLOAD=1` (new; uses `SOTC_RES_LOADED`, `NoteLoaded`) | Publish queues the published ranges for transfer-queue upload; bind serves from import until ready | `=2`: at bind compare the device copy with guest bytes (sampled); count "bind found range not ready" (must be 0 after warm-up) | Removes draw-thread bind copies: 5.6-81 MB per 5 s steady, and the spikes (129.5 MB in one frame, `base.log:653`; the 68 ms copy with 42,838 faults, `base.log:460`) |
| **M4. Staging removal (Z4)** | `SOTC_HOST_IMPORT=1` (exists, off) with M3 | Big STATIC and STREAM_MIP uploads copied by the GPU from imported guest memory; staging only as Z3 fallback | `=2`: read back a sample of device copies, compare | -0.4 to -0.9 GB host RAM; -0.18 to -0.21 GB VRAM (measured staging sizes). Risk: earlier bulk result was slower (`PC-MEMORY-REWRITE.md`); re-measure on the transfer queue |
| **M5. Third frame slot (optional)** | `SOTC_FRAME_SLOTS=3` (new) | Per-frame copies of the engine's frame data so the game's N-2 reuse waits are not needed | `=2`: compare the output of both slot counts for N frames | Removes most of the 0.7-2.9 s of frame-slot waits per 5 s (measured), if the cause is GPU lag (M0 decides). Cost +35-50 MiB RAM (est.). Large engine change |
| **M6. Tracker off per class** | per class, after M2-M4 verify clean | Remove the last tracking for each class; keep only the verify hashes | the verify modes of M2-M4 over a full route on each vendor | Write-fault time (0.1-2.3 s per 5 s, tracked) and the announce storm for imported ranges (2.3M announces per 5 s, `s14v.log:597`; CPU cost unmeasured) |

Order and gates: M0 first (no behaviour change; one route run). Then M2 and M3 together, because M2 retires the stale
serve and M3 removes the draw-thread copy spikes; both are in the route the user checks. M4 after M3 (it needs the
transfer path and the ready state from M3). M1 whenever M0 says the probe output is unused. M5 only if M0 shows the wait is
GPU lag that a third slot can cover. M6 last.

Each step needs the user's game test before the default changes (the user's rule for every migration step,
`PC-MEMORY-REWRITE.md`, section 6, "Decision").

---

## 6. Risks

- **Ordering.** Reading a RESULT or STREAM byte before the GPU has finished it gives stale or half-written data. The
  game's own labels are the ordering. ReleaseMem and EventWriteEos labels are not covered (`pc_ring.h`). This is the risk
  that produced the vertex explosions with `SOTC_RB_CLASS=2` (`PC-MEMORY-REWRITE.md`, "Known problems"). Verify must cover
  every label type before M2 is the default.
- **Reuse.** A CPU write into a STREAM half the GPU still reads corrupts the frame. The engine's frame slot protects it, and
  the flip wait checks it. A third slot (M5) moves the boundary; it must be re-checked.
- **Stale STATIC.** A CPU write to a published range is a bug, not a normal case. The sampled hash must catch it; the
  current `SOTC_STALE_CHECK` is sampled and found no stale page in the valid lots (section 1.8).
- **Pinning.** Imported memory cannot be paged out. Fine at 5-6 GB on 16 GB; check on smaller machines.
- **Import limits.** Size and alignment limits differ by driver. A refused import must fall back to Z3 and must not stop
  the draw thread (`host_import.cpp:99,131,180` already log refusals).
- **Latency change.** RESULT serves exact values after the game's label. The game may then wait on the GPU where the copy
  ring did not. That is the PS4 behaviour, but it can move cost to the GPU lag, and the frame-slot waits already show that
  the GPU trails the CPU.
- **Verify cost.** Every verify mode adds reads of guest memory. Use sampling (the ring8 verify samples 1 bind in 32;
  `pc_ring.h`), and time it before a matrix run.
- **The default is not verified.** Section 1.8. Do not claim the native default as the target model until the flicker and
  stale-mask symptoms are closed by M2.

---

## 7. Not measured, open

1. GPU time of the frame: host-read arena (ring8) against the tracked model. Needed to back "host reads are cheap" (2.3).
2. RAM of the native default (`SOTC_RAM_REPORT` never ran with `SOTC_NATIVE=1`). The private remainder (4.3-5.2 GB) has no
   owner list.
3. The split of the page faults in the copies (source or destination first touch; `base.log:460`).
4. Whether the frame-slot waits are GPU lag or CP lag (M0).
5. Vendors other than NVIDIA for every memory path. ReBAR and SAM behaviour is inferred from the heap budget.
6. Announce cost per call, and how many of the 2.3M announces per 5 s are for imported ranges.
7. Whether any RESULT consumer reads a value in the same frame (the masks are read two frames later per `PC-MEMORY-REWRITE.md`;
   other consumers were not checked).
8. The `unique read/written MiB/frame` counter in ring8 prints 0.000 in every log: a bug or not implemented.
9. `readbacks.md` (M2) was not present when this was written; the consumer list here is from `gpuresults.md`
   (2026-11-01 lots). Re-check the rates against M2 when it lands.

Logs used: `user/pcmem/20261101-075954/logs/base-r1-a1.log`, `ring8-r1-a1.log`; `user/pcmem/20261101-084248/logs/default-r1-a1.log`
(tracked; RAM and fault lines); `user/gcn-run1/base.log`, `s14v.log` (native default; the file names and line numbers above are
from these); `user/gcn-cold5/cold_w2.log` (native, cold start: 1% low 1.6-2.7 FPS, pipeline spikes, not memory).
