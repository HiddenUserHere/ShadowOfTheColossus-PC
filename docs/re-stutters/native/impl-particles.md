# N3 particles (C5): ProcessStats count readback, decision and status

Agent "particles". Base: main tree (branch re/pc-native) read with the Read tool and PowerShell; this worktree is
on the older base 606f800 and was not edited except for this file. Nothing built, no game run, nothing committed.
No hooks added, so `hooks_install_check` was not run for new patterns.

## Status

Option 3 approved by the user (1 Nov) and implemented (coordinator, after this report): `SOTC_NATIVE_PARTICLES=prev`,
default off (the exact read). Built and hook-checked; not run in game. Section "Implementation" at the end describes
it; sections 1-6 are the analysis it follows.

## 1. The read and the producer

`ParticleSystemPool::ProcessStats` 0x1018c0, called once per pool from `ParticleRenderer::ProcessStats` 0x109970
(`FUN_00109970` loops the pools in `param_3+0x10498..0x104a0`, stride 0x30, with the pool flag `+0x60 & 2`).

Call sequence in the decomp / disassembly (eboot offsets):
- `0x1018de..0x10197d`: pool counter `k = pool+0x64` is incremented; the descriptor of ring slot `k%3`
  (pool+0xf8 + slot*0x28) is filled and `sub_928f0` (0x928f0) dispatches the stats pass into it. Entries written
  = `(mult * (bits or 1)) >> 2` with `mult = desc+8` and `bits = desc+4 bits 16..29`. `sub_928f0` is a GPU
  dispatch (`FUN_0086b9e0`, groups = entries/64), so the array is GPU-written.
- `0x101982..0x1019af`: once `k >= 3`, `sub_8c1b0` (0x8c1b0, `base | (hi & 0xfff) << 32`) gives the array of
  ring slot `(k)%3`, which was dispatched two calls earlier (particle_prev.cpp timeline, consistent with the disasm).
- `0x101a98`: `mov r15d, dword [r12 + rbx*4]`, `rbx` = system index `u` (bit index in the active bitmap
  `pool+0x198`, bounded by `pool+0x1a8`). This is the only GPU-array read in ProcessStats and it is the
  4-byte load that faults on the GPU-tracked page. `r15d` is the count `c`.
- `0x101aa7`: `pool+0x70 += c` (sum of all counts of the frame; reset at 0x1018ed). Readers of `pool+0x70` were
  searched in the particle functions (`FUN_00107540`, `FUN_00103ad0`, `FUN_001051f0`, `FUN_0019d410`); none found
  (the `+0x70` hits there belong to other structs). Treated as an unread stat.
- `0x101ac1`: `FUN_000fc2d0(sysrec, handle, c)` stores `c` at `table+u*0x100+0xb4` when the handle's generation
  (`+0xb0` short) matches and the active bit is set.

What the count is: one u32 per system index, produced by the GPU stats pass, so the game consumes "live particles
of this system, two frames old". The GPU shader body was not read; "live particles" is inferred from the name
of the array and from how the CPU uses it (zero means empty).

## 2. CPU consumers of the count (complete list found)

| # | Consumer | Where | Effect |
|---|---|---|---|
| a | Retirement of AutoDestroy systems | 0x101ad1..0x101b90 (`c < 1` branch) | If owner flag `*(owner+0x60)+0x54` bit 0 (AutoDestroy) is set and `now - sys+0xc0 > 1000` ms, the system is released: active bit cleared, `pool+0x1b8 -= 1`, record released (`FUN_000fc0f0`), callback `FUN_00135d90` if `sys+0xcc != -1`. `sys+0xc0` is the creation time (`FUN_00100b80` line 60), so this is an age test since creation, not since the last particle. |
| b | Forced retirement | same branch | If `sys+200 & 4` (set elsewhere by game code, not in ProcessStats) the system retires at `c < 1` regardless of age. |
| c | Life stamp and log | `c >= 1` branch | `pool+0x188[u] = now`; for AutoDestroy with age > 30 s: `sys+0xc4 = now`, `sys+200 |= 8`, and a single log "Killing AutoDestroy ParticleSystem after %.0f seconds". The system is NOT killed here. |
| d | Copy into the system table | `FUN_000fc2d0` at 0x101ac1 | `table+u*0x100+0xb4 = c`. Readers NOT found: `UpdateSimulation` (0x103ad0, 0x1051f0), `BeginFrame` (0x107540), `FUN_001013f0`, `FUN_00100b80`, `FUN_000fc470` do not read `+0xb4`. A generic grep over `out/re/decomp` gives hundreds of `+0xb4` hits; no particle reader was isolated. |

Item d is open. Item a is the only consumer with a gameplay effect that was verified in the decomp.

## 3. Decision

Options as in PLAN.md and the brief:

- **Option 1 (CPU count from emitter state): rejected for now.** It would need, per system, the emitter's stop/spawn
  state and the longest particle lifetime (`maxLife`). Neither is pinned: the emitter descriptor layout behind
  `*(owner+0x60)` is only partly read (`+0x54` AutoDestroy, `+0x58` frame index, `+0x60` float gate, `+0x68` blob
  copied into the system record at `+0xd8` by `FUN_00100b80`). An estimate would change when AutoDestroy systems
  retire, which is visible, and it cannot be verified without the GPU count anyway.
- **Option 2 (drop the consumer): rejected.** Consumer a is lifecycle: without it AutoDestroy systems never
  release their slots (`pool+0x1b8`, the "Exceeded particle system instances" limit at `FUN_00100b80`), which is a
  gameplay and memory effect, not a debug stat. Consumer d is open, so it cannot be dropped either.
- **Option 3 (last completed value, no wait): chosen.** The game already acts on a count that is two calls old.
  Serving the completed value of the same slot gives the same input the game sees today, so retirement timing
  does not change, provided the served copy is the one the GPU wrote for that slot and is complete. This is the
  only option that keeps consumer a exact.

Default for the switch when it exists: keep the exact read (`gpu`). Do not default to any native serving until the
copy below is verified in a game run.

## 4. What option 3 needs (not built)

1. A port-owned copy of the 3 slot arrays (entries `min(pool+0x1a8, dispatched)` u32), written by a GPU copy
   right after each stats dispatch, so the copy target is CPU-readable and not on a GPU-tracked page.
   `SotcRbClass::RequestCopy(base, bytes)` already requests exactly these ranges (`RequestDispatched` in
   `particle_prev.cpp`, with `SOTC_RB_CLASS=1`), and `NoteStatsReadCheck` already counts whether the copy matches
   the array read two calls later (`kMatch` / `kMismatch` / `kNoRequest`). So the copy exists in RB_CLASS, but
   it is a host-side copy, not guest-addressable, and it is applied by the RB_CLASS draw-thread path. Serving it
   needs that path to report "copy of slot s finished" (a fence value), which is in `rb_class.h` / `rb_class.cpp`
   (not owned by this task).
2. A site hook at 0x101a98 (after `mov r15d, [r12+rbx*4]`, the instruction ends at 0x101a9c) that, in native mode,
   replaces `r15d` with the served value and keeps `r15d` as is when no completed copy is ready. Register-write
   site hooks are supported by the hook engine (`SotcHooks` site mode with `GuestRegs`), as the water and spin
   sites in `native_results.cpp` do for reads.
3. Freshness rule: serve the copy of the slot read in this call only if its fence is complete; otherwise serve the
   previous completed slot (one call older, the same tolerance the 1 s retirement already has) and count the case.
   If none is complete, keep the exact read (counted as a fallback). Never serve zero for an unknown value: a zero
   would retire live AutoDestroy systems after 1 s.

Why the backing view (`runtime_memory_backing_pointer`, used by `particle_prev.cpp`) is NOT used as the served
source: it holds whatever the port last downloaded for that page, which may be from a previous frame, or zeros if
the page was never downloaded. Serving it could retire live systems.

## 5. Verify (for the implementation, when it exists)

- Mode 2 already has the right comparator for option 3: `NoteStatsReadCheck` (RB_CLASS) compares the copy with the
  read. For the served value, the verify should compare the served value with the exact read (both available in
  mode 2, the exact read via the backing view as particle_prev does) and count: served, fallback (no complete
  copy), mismatches per system and per call, and AutoDestroy retirements that differ from the exact run.
- Field check: count retirements per minute in mode 0 and mode 2 on the same route; they must match.

## 6. What the user must check visually (once implemented)

- AutoDestroy effects (sparks, dust, smoke that end on their own) still disappear when their particles are gone,
  with no visible lingering or early cut-off. Check the Shrine of Worship and an emitter-heavy area.
- No "Killing AutoDestroy ParticleSystem" flood in the log (it was once per pool before).

## What remains

1. Find the readers of `table+u*0x100+0xb4` (item d) and of `pool+0x70` (item in section 1), with the runtime
   evidence (`SOTC_PARTICLE_PREV=2`) or a wider decomp search for the table's readers by the 0x100 stride.
2. Get a "copy of slot s is complete" fence from RB_CLASS (outside this task), then implement section 4 behind
   `SOTC_NATIVE_PARTICLES=prev` (default `gpu`).
3. Option 1 stays as the later target only if the emitter's `maxLife` and spawn state are pinned in the decomp;
   it would need its own user approval, since it changes retirement timing.
4. The GPU stats shader body (what exactly the count counts) is not read; confirm before relying on "live particles".

## Implementation (SOTC_NATIVE_PARTICLES=prev)

Files: `gpu/sotc/native/native_particles.{h,cpp}` (policy, copies, verify), `gpu/sotc/particle_prev.cpp` (mode 3 hands the
call's arrays over), `scripts/hooks.d/particle_prev.py` (NATIVE_PARTICLE_VERIFY site), `vk_rasterizer.cpp`
(CopyParticleStats after each dispatch, the 2 MiB HostCached copy ring), `sotc.py` (sets SOTC_PARTICLE_PREV=3).
Needs SOTC_NATIVE (the frame ticket).

- Register: the call with pool counter c (after its increment) registers the array its pass writes as entry id c,
  `min(dispatched, extent + 256)` u32 values (at most 4096; larger arrays keep the exact read).
- Copy: after every dispatch, each written bind (`bound_buffers`, full bind or bind-cache hit) that overlaps a
  registered array gets a GPU copy of the overlap into the ring (barrier compute write -> transfer, copy, barrier
  transfer -> host read and all commands). The newest entry of an array takes the write.
- Complete: the copies cover the whole entry and `SotcNative::FrameCompleted()` reached the frame the dispatch was
  built in (FrameSubmitted + 1 at registration). Nothing waits: the check is a poll.
- Serve: the entry of id c - 2 (the data the exact read sees) when complete; else the newest complete one of
  c - 3 .. c - 4; else the exact read. Only an entry that holds every system the loop reads is served, so no
  unknown value is served as zero. The counts go into one of 16 guest blocks of 16 KiB (runtime_low_map) and the
  accessor 0x8c1b0 returns that block (HookAction::Return). ProcessStats only reads the array (decomp 0x1018c0).
- Verify (SOTC_NATIVE=2 or SOTC_VERIFY=1): nothing is served; the candidate's counts are kept and the site after
  the load at 0x101a98 (0x101a9c) compares each exact count with them.
- Report line "SOTC NATIVE_PARTICLES": served same / older, exact reads (and how many had no copy), registered,
  GPU copies, verify counts mismatched, "same-call" mismatches (must be 0), copy 0 for a live count.

Memory: 2 MiB host-cached ring plus 256 KiB of low guest blocks. The first game session should run
`SOTC_NATIVE=2 SOTC_NATIVE_PARTICLES=prev` (verify), then `SOTC_NATIVE=1` with the visual check of section 6.
Open: if the count pass writes the array through something other than a buffer bind (texel or GDS), "no copy"
stays high and nothing is served; the verify line shows it.
