# CPU writes into GPU-written pages: who writes what (static analysis, 5 Nov 2026)

Context: with `SOTC_NATIVE_TRACK=3 SOTC_NATIVE_TRACK_GPU_ONLY=1` the write faults left are 65-350/s, but each one
waits 1.7-6.6 ms on average for the GPU (p99 21-59 ms) to merge the page's GPU bytes before the CPU write
(`SOTC FAULTS` / `SOTC FAULT SPLIT` lines). The owners come from `SOTC_FAULT_OWNERS` (HEAP-MAP.md). This note
records what the eboot shows for each owner: whether the CPU writes the same bytes as the GPU (true sharing,
needs a merge) or other bytes in the same page (false sharing, no merge needed). Static analysis only; the game
was not run. "Verified" means seen in the code; everything else is marked inferred.

## Result

No true sharing was found in any of the systems behind the remaining faults. In each one the CPU writes CPU-only
data that shares 4 KiB pages with GPU-written buffers, or the GPU writes data the CPU only reads.

| System | CPU writes | GPU writes | Same bytes? | Fix without a merge |
|---|---|---|---|---|
| Render jobs (`RenderThread::SubmitJob` block 0x610, `RunFunctionAsRenderJob` block 0x100, both RenderSingleFrameCPU) | job records, command-list pointer lists | nothing in them (the GPU reads the DCBs the lists point to) | no | the blocks are CPU-only: place them in a CPU heap, or skip the merge when the write is outside GPU ranges |
| ResolveSunShadows header (`0xafde0` region, 16 bytes, CPU_RenderToGPUToGame) | the header once per frame before the draw | nothing (GPU reads it in place through a `sub_8be60` usage 0x804 view) | no | upload on bind as today |
| EOP completion labels (`BPGnm_EmitEventWriteEop` 0x86eb60, `BPGnm_WriteCsDoneLabel`) | other words of the same command-buffer block | label qwords (e.g. +0x68, +0x78) | no (different bytes) | skip the merge when the CPU write is outside the GPU range |
| XPR visibility masks (`BuildSetDraws` 0x19b760, shadow variant `sub_19c1a0`) | **never** (no clear before, no write after) | the masks, by compute (`BPGnm_DispatchDirect` 0x19c0f1), then an EOP label = 1 after all dispatches | no | the masks' neighbours in RenderSingleFrameCPU are the faulting writers (`sub_c7850` 0x1228-byte records, job blocks); the masks themselves only need a GPU->CPU path |
| Particle stats (`sub_fffa0` pool, `ParticleSystemPool::ProcessStats` 0x1018c0, dispatch `sub_928f0`) | CPU-only pool arrays: stamp array `pool+0x188` (the hot store 0x101ae4 is a clock stamp), records `+0x178`, handle table `+0x180`, bitmap `+0x198` | a 3-slot stats ring (usage 1 buffers `+0xf8/+0x120/+0x148`) | no; the CPU reads slot (k-2)%3, two frames late, and never writes the ring | placement of the CPU arrays away from the GPU buffers |
| PhysX writer (`sub_a3ee50`, 64-byte records in GameSingleFrame), GameToRenderThread (`sub_4f6f40`), `xadd` in `sub_b30ce0` | CPU data / a CPU refcount under a mutex | nothing | no | none needed |

Open items that could still hide true sharing:
- The particle simulation dispatches (`UpdateSimulation` 0x1051f0, 0x103ad0) were not traced; GPU writes into the
  record chunks (`+0x80/+0xa8`) are not ruled out.
- The XPR cull shader body is not in the eboot: if it ORs bits into a buffer it expects zeroed, someone must
  clear it first (no CPU clear was found; whether the allocator returns zeroed blocks is unknown).
- Several label producers and the heap of some CPU blocks (`sub_c8110`, `sub_c7850`'s neighbours) were not traced.

## `sub_8be60` (GPU buffer wrapper) usage word

`sub_8be60(obj, type, stride, count, usage, data, name)`; obj+0..0xf is the V#, +0x10 the allocation, +0x18 the
usage, +0x20 count*stride. Verified bit effects:

| Bit | Effect |
|---|---|
| 0x1 | CPUMemory heap (the pointer at 0x15fe920 holds slot 0x2dd9f80) instead of GPUMemory; V# MTYPE 3 |
| 0x2 | no persistent allocation: with init data, a copy into RenderSingleFrameGPU; without, base 0 (bound later by the caller) |
| 0x4, 0x20 | V# MTYPE 3 only |
| 0x8, 0x10 | allocate size+4 with a trailing zero dword (no call site uses them) |
| 0x100 | typed path: stride 0, count = bytes (raw) |
| 0x400 | `sub_8be10` does not free the allocation (not owned) |
| 0x800 | no allocation, no copy: the V# base is the caller's pointer (a view over memory allocated elsewhere) |
| 0x40, 0x80, 0x200 | no effect in `sub_8be60`/`sub_8be10` |

MTYPE: the code is 0x6e with bit 0x1, 0x6d when usage & 0x3c, else 0x10; usage & 0x3d gives V# MTYPE 3, else 4
(shadPS4 treats MTYPE 3 as coherent memory). The wrapper never encodes shader write access. Per value (341
sites): 0x2 (146) and 0x0 (48) are CPU-filled and GPU-read; 0x4 (84, GPUMemory, MTYPE 3, mostly no init data)
is the best static candidate for shader-written buffers; 0x800/0x804/0x805 wrap memory allocated elsewhere and
are used by the XPR cull, shadow/decal cull and sun-shadow passes (compute), so their GPU writes land in the
caller's allocation (for the masks, RenderSingleFrameCPU blocks from `sub_12820` in `BuildSetDraws`), which
an interception of `sub_8be60` alone would not move.

## What this means for the port

- Every remaining merge examined is a false-sharing merge: the CPU write does not need the GPU's bytes. A fault
  whose written bytes lie outside the buffer cache's exact GPU-written ranges (`gpu_modified_ranges`) can do the
  store and an exact upload with no GPU wait (the fault-time emulation in `gpu/sotc/native/fault_emu.cpp`
  already does the store and protects the bytes until their upload ran). Bytes inside a GPU range keep the
  merge, which is the safe fallback for anything not analysed here.
- Placement (CPU-only blocks out of the heaps that hold GPU output, GPU output in pages of its own) removes the
  same faults at the source; it needs the allocation sites above (`sub_12820` call sites in `BuildSetDraws`,
  `sub_fffa0`, `SubmitJob`, `RunFunctionAsRenderJob`, `sub_c7850`), not only `sub_8be60`.
- The GPU->CPU reads (masks, particle stats) are one-way: a copy to staging after the producer's label would
  give the CPU fresh data without a merge; today NATIVE skips those readbacks and the CPU reads old data.

Per-agent evidence (instruction addresses): session scratchpad files `flags_8be60.md`, `flags_callers.md`,
`shared_renderjobs.md`, `shared_xpr.md`, `shared_particles.md`.
