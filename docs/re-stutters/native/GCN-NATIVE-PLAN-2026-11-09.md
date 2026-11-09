# GCN native, for real: research synthesis and plan (9 Nov 2026)

## Goal

The draw thread spends about half its time (`BindResources`, ~12 ms of ~27 ms per frame) translating PS4
resource binding into Vulkan binds, once per op, ~5,000 ops per frame. On the PS4 there is no bind: shaders
read V#/T#/S# descriptors from memory. "GCN native" makes the port work the same way, so the per-op bind
disappears. This file combines 13 read-only research passes run on 9 Nov (code, decompiles, web).

## What exists (inventory)

Stages S1–S6 are built behind switches, all off, and **none has been measured in the game**. The CHANGELOG has no
entry for them. The S2 runs of 7 Nov recorded image bugs, not timings.

| Stage | Switch | State | Per op, still on the draw thread |
|---|---|---|---|
| S1 page table | `SOTC_GCN_MEM` (=2 verify) | built | `ObtainBuffer` on every classic bind |
| S2 buffers by address | `SOTC_GCN_BUF` (needs `SOTC_SHADER_ADDR` 3/4, `SOTC_GENERIC_BUFFERS=2`) | raw buffers ran; formatted buffers off (`SOTC_GCN_BUF_FMT=0`) | the `BindBuffers` loop; descriptors for formatted, written and special buffers |
| S3 textures (T# → heap slot hash table) | `SOTC_GCN_TEX` (needs `SOTC_HEAP`, `N5_BINDLESS`, `BINDLESS_SAMP`, `LAYOUT_UNIFIED`) | the most complete: GPU table, shader probe, tombstones | `TableMemo::Resolve`, `BindHeapImages`; skipping them (`SOTC_GCN_TEX_SKIP_RESOLVE`) is measurement-only |
| S4 coherence batch per submit | `SOTC_GCN_SYNC` (=2 verify) | built | a bit test per addressed buffer |
| S5 barriers | `SOTC_GCN_BARRIER` | narrow | one global barrier per writing op |
| S6 thin draw thread | `SOTC_GCN_COMMIT` (needs `SOTC_PARALLEL_RECORD`) | eligibility only | **all of `BindResources`**: no code path skips the bind |

**No switch combination removes the per-op bind today.** S6 bypasses caches; it does not skip `BindBuffers`
or `BindHeapImages`.

Other facts:
- `SOTC_GCN_MISS_EMIT` defaults **off** in code (`info.h:155-160`, true only for `=1`), although the docs say on.
- `SOTC_SPV_FUNCS` has the same inverted default.

## Findings that change the plan

### Engine side (reverse engineering)

- **Material T#s behave like Switch texture pools.** Each material keeps its texture descriptors in two GPU
  blocks inside the material object (`material+0x578`, `+0x5b8`).
  - They are written once at load (`0xf5cd0` inside the parser `0xf6c90`). Per draw the engine copies only a
    pointer.
  - Streaming and mip changes rewrite the T# **in place**, at the same address (`0x881e60`, and the block
    walkers `0xf9c40`, `0xf9ef0`, `0xf8d10`). The engine never allocates a new descriptor.
  - Render targets are fixed objects created at area load (`0x9aef0`).
  - Engine pass textures (`0x133550`, `0xa8c50`) are the exception: they are copied per bind into a per-frame
    chunk ring.
  - **Consequence:** S3 can register every material T# at material creation (the existing hook at `0xf9390`),
    refresh entries from the few in-place rewriters, and register pass textures at their two bind sites. No
    per-op texture resolution is needed on the material path, and no first-sight miss.
- **Draw path** (`0x11d3d0` loop → `0x123810` per-draw bind → `0x94d00` direct draw / `0x189060` indirect).
  - The per-draw state is not one constant buffer. Each stage has a dword array at `S+0x90 + n*0x1800`.
  - When the stage's dirty byte is set, the flush `0x85fc10` copies the array into a fresh chunk, and user-SGPR
    pairs point into the copy. The SGPR bases come from the table at `DAT_01290410`.
  - Per-draw pointers come from:
    - node `A+0x68`, written to `ctx+0xa8`;
    - a 2-entry per-frame parity ring, written to `ctx+0x90`;
    - `record+0x48`, written to `ctx+0xc0`.
  - Index buffers are bound from a 3-dword descriptor (`0x94c60`).
  - Still unknown: who writes `S+0x78` and the `ctx+0xc130/c140/c150/c5d0` pointers (likely the vertex/fetch
    tables), and the per-frame writer of the parity ring.
  - `0x123d20` is only a debug-name getter.
- **PM4 writers.** 333 game functions write type-3 packet headers.
  - The big engine functions (state flushers `0x919e0/0x91b50/0x91cd0/0x91e10/0x93960`, `0xf8990`, `0x85f410`)
    **do not write packets inline**: they call small leaf writers (`0x869a50`, `0x86d4c0`, `0x86d5e0`,
    `0x86a700`, `0x86a900`, `0x86edc0`, `0x86eb60`, the `0x884xxx` RELEASE_MEM/INDIRECT/WRITE_DATA family).
  - 133 emitters in `0x86f`–`0x87c` / `0x884`–`0x885` have no static caller (reached by pointer) and tag
    themselves with NOP markers `0x6875xxxx`.
  - All writers use one command-buffer wrapper: cursor `+0x10`, end `+8`, refill `+0x18`.
  - Draw, dispatch and shader-bind are sceGnm imports that the port itself implements (`gnmdriver.cpp`): the port
    encodes those packets and then parses them back.
  - The flushers emit only changed groups (dirty flags), so a structured hook sees deltas and the port keeps its
    own state shadow.
  - **Cost warning:** a hook transition costs an estimated 50–100 ns. At ~60k writes per frame that is 3–6 ms,
    which can exceed the CP parse it replaces.

### Port side

- **S2 formatted-buffer corruption** (horse copies, grass without alpha). Most likely cause:
  - the table read fast path (`addr_table.cpp:425-428`) answers placed, CPU-clean pages without checking
    GPU-modified state;
  - the clean bit is cleared only by CPU writes (`memory_tracker.h:525`), never by GPU writes;
  - so a page rewritten by a render target or image keeps serving stale bytes. Formatted buffers are the ones that
    alias images.
  - Fix: drop the fast path when `IsRegionGpuModified`, and clear the clean bit from the GPU-modified path.
  - Second candidate: the texel image sync at `buffer_cache.cpp:6452` runs only under `kOptOff`.
- **S2 dark start with miss emission.**
  - Likely the miss record is stored without release/make-available semantics into host-coherent memory, so the
    CPU sees misses a frame or more late. Pages are placed late, which matches "normalizes as you walk".
  - Also, page placement on a miss writes entries the shader did not expect.
  - The report counters reset per window, so "0 misses" may be a read-timing artefact.
- **Coherence.**
  - CPU writes already arrive as announces (`SotcNative::Announce`), drained at submit start.
  - S4 uploads the CPU-dirty set at submit start, but scans the whole 4 Mi-entry table each submit (unmeasured).
  - Proposal: a summary bitmap so the batch costs O(dirty pages), and `GCN_SYNC=2` with GPU_ONLY faults as the
    detector for unannounced writes. Windows write-watch only if verify finds misses.
- **Table-mode shader cost.**
  - A buffer access is ~120–150 SPIR-V ops with 6 memory operations (2 table lookups, 4 data loads), against 1 for
    a descriptor load. That is why the NVIDIA compiler is slow on them.
  - Software bounds checks are required, because robustness does not cover buffer-device-address loads.
  - Cuts:
    - one page lookup when the access is aligned and cannot cross a 16 KiB page;
    - drop per-element checks when the host proves `num_records` covers the access at bind time;
    - subgroup broadcast for uniform V#s.
  - Compatible with the shader-only GPL libraries. The 128-byte push block is full.
- **S3 caution from history.** `SOTC_N5_BINDLESS` was not cheaper per op (it still looked up each texture per
  bind) and caused 11 frames over 100 ms. The gain only exists if the lookup truly leaves the op.

### Quick win found on the way

- **`SOTC_DRAW_MDI=1`** (exists, off) merges the XPR per-object indirect draws (~3,980 per frame, 57% of draws,
  all from `0x1a6d20`) into Vulkan multi-draw-indirect. It has a verify mode (`SOTC_DRAW_MERGE_STATS=2`).
  - Estimate: ~2,200 fewer ops per frame, which shrinks every per-op cost including the bind.
  - The visual risk is low: the draws are the same.

### What the 9 Nov trace showed about the remaining slow frames

The slow frames are not bind cost:
- two frames of ~100 ms were **one GPU submit taking ~80 ms** (submits 605 and 37882);
- one 67 ms frame was a **synchronous image upload** on the draw thread (`RefreshImage`, 61 ms);
- two were **NVIDIA compiles on workers** (jobs of types 5 and 6, 84–262 ms) not covered by the pack or spec-free;
- the rest were game threads waiting on those.

## Step 0 results (measured 9 Nov, warm route, user's smooth sotc.ini; logs user/prof/m0)

- **Draw merge / MDI is a dead end.**
  - Verify (`SOTC_DRAW_MERGE_STATS=2`): 67,961 of 366,818 draws per 5 s (18.5%) mergeable, runs of 3.1 on
    average, 0 mismatches.
  - Real (`MERGE_STATS=1 SOTC_DRAW_MDI=1`): only 41,803 of 378,827 draws merged (11%). Runs are cut by pipeline
    changes (217k) and non-joins. No two draws shared instance parameters, so the multi-draw extension made no
    calls.
  - The draw thread was **worse**: 51.6 ms against ~25–30 ms, and 28% of frames were shown for 4 or more vblanks.
  - Removed from the plan.
- **GCN census** (`SOTC_GCN_CENSUS=1`, descriptor mode, measurement only), ~2,400–4,050 ops per frame:
  - Op kinds: indirect draws 38–54%, dispatches 28–38%, direct draws 16–24%.
  - Buffers per op: plain read 1.7–1.8, **formatted 2.1–2.4**, written 0.7, special 2.0–2.1.
  - Images per op: sampled 2.1–2.5, storage 0.15–0.17; samplers 0.74–0.77.
  - Per frame: distinct T# **480–700** (max 1,041), new T# 2–16, distinct S# 7–8.
  - Meaning: about 40k resource binds per frame resolve the same ~700 textures and 8 samplers again and again. A
    T# table (S3) has a small, stable working set. Formatted buffers are the largest buffer class, so the S2
    formatted-buffer fix is a prerequisite for skipping the buffer bind.
- **All GCN stages on** (`… SOTC_GCN_COMMIT=2`): the game crashed after ~70 s. A game thread read a null pointer
  at `0x776f0010`, likely GPU-written data the game reads coming back zero in that mode. The count waits for the
  S2 fixes.
- **The "80 ms GPU submits" are queue waits, not GPU work.** With `SOTC_TRACE_GPU=1` the GPU was busy only
  22–41% of the slow frames (16–38 ms of real GPU time); the submit sat in the queue. The GPU is not the limit.

## Step 1 status (9 Nov)

- **S2 image fixed.** With `SOTC_GCN_BUF_FMT=1 SOTC_GCN_MISS_EMIT=1` the user confirmed the image is correct: no
  horse copies, grass with alpha, no dark start. Changes made:
  - `NoteGpuWrite`: GPU writes clear the table's clean bits, and the fast paths refuse GPU-modified ranges.
  - The miss stamp is stored with Release, and the host reads it with acquire.
  - A miss on a formatted or written buffer touches the whole V# range with the right flags.
  - The counters accumulate.
- **The table-mode "infinite loading" was a cold compile, not a CP deadlock.** In table mode the pretranslate
  dropped every request, compute included, so no compute was prebuilt, and the CP compiled missing compute
  programs inline (`nvgpucomp64.dll` on the CP thread). The fix:
  - compute requests now reach `QueueComputePrebuild` with the table-mode key (Plain variant, mixed hash);
  - a missing program goes to an urgent worker (`TranslateComputeOnWorker`).
  - A cold S2 run then prebuilt 4,253 programs, got 99.9% hits and made 0 CP compiles, but its first loading
    screen took 431 s.
  - The CP-switch bisect that pointed at `SOTC_CP_LABEL_SLEEP` / `SOTC_BACK_WRITE_EXACT` was cache warm-up
    order. Both switches stay as they were; a long-sleep counter and a hang log were added.
- **S2 alone is slower:** draw 37 ms against 24 ms, about 21 FPS in the user's session. The bind is not skipped
  yet, and every buffer read pays the table lookup. The gain needs steps 2 and 4.

## Step 2 status (9 Nov, code only, not run)

- **Registration ahead of draws** behind `SOTC_GCN_TEX_REGISTER=1` (needs `SOTC_GCN_TEX`; off by default). Hooks in
  `scripts/hooks.d/gcn_tex_register.py`, handlers in `gpu/sotc/gcn_tex_register.cpp`, queue and drain in `gcn_tex.cpp`.
  - Material blocks: read at the factory post-phase through the existing `PIPE_PREDICT_MATERIAL` wrap (the loader
    turns it on for this switch). Block data T#s, plus the payload T#s when they differ.
  - In-place rewrites: `0x881e60` (wrap) and a site inside `0x880c30` (`0x880c46`, so `TEXLIFE_LEVEL` can stay on the
    entry). The new T# takes the view classes of the old one, and the old entries become tombstones. A rewrite of a T#
    the table does not hold is only counted. The block walkers are not hooked: they copy words these two write.
  - Render targets: `0x97a50` wrap. Pass textures: entries of the T# setters `0x861fe0` and `0x862160` (every caller).
  - The draw thread resolves the queue at submit start (`Globals::DrainMisses` → `Tex::DrainRegistered` →
    `OnImageMiss`), at most `SOTC_GCN_TEX_REGISTER_BUDGET` (64) views per submit. Registered entries may take at most
    half of each heap array (`slot-cap` counter), because every live entry pins its slot.
  - The view class is learned from the per-op resolves per T# shape; before that it is guessed (not depth; array for
    array and cube types).
- `SOTC_GCN_TEX_REGISTER_SOURCES` (mask: 1 material, 2 refresh, 4 render target, 8 pass) and
  `SOTC_GCN_TEX_REGISTER_LOG=1` (one-time render-target log: the per-op T# next to the object's `+0xc4`, `+0xe4`,
  `+0x124`, `+0x164`; it settles the `+0xe4` question).
- 5 s line `SOTC GCN TEX REGISTER`: queued, inserted, present and failed per source, refreshes, table size, and the
  per-op lookups that found a registered entry (the gate for step 4).
- Not covered: a material whose list entry was still the pool default at parse gets the real T# later through the
  walkers; that T# is not registered (the per-op path still inserts it).

## Plan

Each step is behind a switch, off by default, with a verify mode where one exists. Measure one round per config
on the test route (`user/routes/tests.json`), cold-cache checks with a new `SOTC_SPV_SALT`, and get the user's
visual check before any default changes.

### 0. Measure first (no code)
1. `SOTC_DRAW_MDI=1` with `SOTC_DRAW_MERGE_STATS=2`: verify, then timed.
2. All GCN stages in count mode: `SOTC_PARALLEL_RECORD=1 … SOTC_GCN_COMMIT=2` reports how many ops would be
   eligible and which blockers dominate.
3. Identify the two 80 ms GPU submits (GPU timestamps per pass, `SOTC_TRACE_GPU=1`) and the remaining
   compile job types 5/6.

### 1. Fix S2 (buffers)
1. Fast-path fix: GPU-modified check, and clear the clean bit on GPU writes.
2. Verify `SOTC_GCN_BUF_FMT=1`: image correct (user check).
3. Miss emission: add release/make-available to the stamp store, read `write_index` directly, and log the first
   10 s of drained pages. Then decide on the default.
4. Table-mode shader slimming: single page lookup for aligned accesses, host-proven range checks.

### 2. S3 (textures) without per-op lookup
1. Register material T# blocks at material creation (`0xf9390` hook). Hook the in-place rewriters
   (`0x881e60`, `0xf9c40`, `0xf9ef0`, `0xf8d10`) to refresh entries, and the pass-texture bind sites
   (`0x133550`, `0xa8c50`).
2. Render targets: register at creation (`0x9aef0`). Resolve which 32 bytes a shader samples (the conflict in
   `pass-targets.md` vs `texture-binding.md`) with one logging run.
3. Turn the resolve-decline cases (render target equal to a sampled image, stencil, rebind, composite) into
   BindSet events, so `SKIP_RESOLVE` becomes a real mode.

### 3. S4 (coherence) exact and cheap
Summary bitmap for the dirty set; `GCN_SYNC=2` plus GPU_ONLY faults as the detector for unannounced writes.

### 4. S6: a real skip path
Write the code path where an eligible op records pipeline, push words and draw, **with no `BindBuffers` or
`BindHeapImages` call**. Gate: verify mode with 0 mismatches, then the draw thread drops by at least 5 ms on the
route.

### 5. Structured PM4 (optional, after 0–4 are measured)
1. Micro-benchmark one Entry hook transition.
2. If cheap enough, hook the hottest leaf writers (`0x869a50`, `0x86d4c0`, `0x86d5e0`, `0x86a900`) into a
   structured op stream beside the PM4, with a verify mode against `ProcessCommands`.
3. Otherwise, a table/SIMD PM4 decode on a helper thread one submit ahead.

### 6. Still to reverse-engineer
- Writers of `S+0x78` and `ctx+0xc130/c140/c150/c5d0` (vertex/fetch tables).
- The per-frame writer of the parity ring.
- The layouts of the 16/32-byte blobs written by `0x8619a0` / `0x861fe0`.
- The UI draw path.

These are needed for a native re-issue of draws (the renderer-rewrite direction); they are not needed for steps
1–4.

## Renderer rewrite: verdict for now

A full rewrite would replace the binding/submission layer (about 20 KB of engine code) and re-implement the pass
logic: shadows, G-buffer, lighting, post, water, fur and particles, about 30–40% of the render path. The 12,499
graphics and 4,252 compute shaders could be kept, translated as today. No PS4 project has done it; RT64 shows the
pattern works when the renderer is a separate component that replays the game's draw calls. Steps 1–4 deliver
most of the bind saving without it, so the rewrite stays a later option. Step 6 is its prerequisite.

## S3 (texture table on the heap) measured, 9 Nov 2026

Warm route, one round each (`user/prof/m0/*.out`), after the heap compute prebuild fix (compute: 4,253 programs
prebuilt, 1 at first dispatch) and the sampled verify (mapping read for 1 resolve in 64, the host shadow otherwise;
reading the ReBAR mapping on every resolve cost 100-250 ms/s of draw time):

| Run | CP ms | Draw ms | p50 | p99 | >50 ms |
|---|---|---|---|---|---|
| descriptor mode, smooth set (`e_desc_base`) | 16.0 | 22.3 | 33.3 | 41.7 | 0 |
| S2 table + sync (`f_s2sync`) | 10.7 | 21.3 | 33.4 | 50.4 | 6 |
| S2 + bind skip, GCN_BUF=1 (`g_buf1_skip`) | 14.5 | 23.8 | 33.4 | 50.1 | 4 |
| + heap stack, no texture table (`k_heap_only_warm`) | 17.7 | 26.5 | 33.4 | 74.4 | 31 |
| + texture table verify + register (`l_tex2_reg`) | 13.4 | 29.2 | 41.6 | 84.1 | 39 |
| + texture table skip (`l_tex1_skip`) | 21.5 | 30.5 | 41.7 | 101.3 | 52 |

Texture table verify: 0 mismatches in every run. But the heap stack (HEAP + N5_BINDLESS + BINDLESS_SAMP +
LAYOUT_UNIFIED) costs more draw time than the texture skip saves, and the skip itself shows no gain over verify.
S3 on the heap is not a lever as built; descriptor mode stays the play default. The first `k_heap_tex_warm` run
(2.4 FPS) was confounded by ~434 pipelines missing from the manifest, not by a key mismatch.

Found on the way, all configurations: 134 stages are never prebuilt ("specialization differs", diff masks
00300100 / 00200180 / 00300180 = image swizzle + conversion, buffers) because `BuildHint`
(`gpu/sotc/pretranslate.cpp:255-258`) drops images whose raw T# is not valid, so the manifest translation uses the
synthetic default T#. Fix: record invalid images in the hint (hint format change).
