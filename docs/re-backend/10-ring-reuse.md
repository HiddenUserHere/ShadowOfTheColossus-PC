# 10 - Frame ring reuse: what guards it

Read-only static analysis of `out/eboot.elf` (Ghidra pseudocode `out/re/decomp/<hi16>/<addr>.c`,
`tools/re/eboot_re.py`) plus a read of the port's submit gate (`gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`).
Addresses are eboot VAs. Labels: **observed** (decomp, disassembly, port code, with the address),
**inferred**, **unknown**. Builds on docs 03, 06, 09 and `docs/re-stutters/reports/06-render-frame.md`.

## 1. Verdict

**UNSAFE** as a basis for skipping GPU write tracking in the CPU frame ring (Part 2 fast path is not
implemented). Two independent reasons:

1. **observed:** nothing in the engine's ring rotation or allocation waits on the GPU. Reuse is decided
   by a CPU-side marker check only, and that check protects one slot (the previous frame), not GPU completion.
2. **unknown:** whether the GPU reads or writes the CPU ring data at all. No allocator user of
   ring A (`ctx+0x580`) or ring B (`ctx+0x670`) was found, so the GPU traffic into them cannot be ruled out.

The port's own gates only make reuse safe under the default gate (GpuIdle at the frame boundary). The
frame-ticket and frame-ahead modes let the CPU run ahead of GPU completion, and nothing bounds that lag.
So the answer is **SAFE ONLY IF the GPU is idle at each frame boundary**, which holds by default but is not
an engine guarantee. Given the unknown GPU traffic, the fast path must not be enabled on this basis.
Part 2 therefore implements only the `SOTC_FRAME_RINGS=2` measurement (section 6).

## 2. Ring mechanics (observed)

Ring objects: A = `ctx+0x580` (RenderContextFrameCPU, 0x8200000 bytes, family 1), B = `ctx+0x670`
(RenderContextFrameGPU, 0x1400000 bytes, family 2). Init in `FUN_0008f6f0`, which calls `0x481f0` at
`0x8ff7b` and `0x8ffa3` with `slots = 2`, `chunk array = ctx+0x660 / ctx+0x750`, `base = ctx+0x760 / ctx+0x768`.

Ring object (`0x481f0`, `0x48270`, `0x48440`):

| Offset | Field |
|---|---|
| `+0x10` | `const char*` name (set by ctor `0x475c0`: `RenderContextFrameCPU` / `RenderContextFrameGPU`) |
| `+0x70` | spinlock (`0x48440`, sleeps `sceKernelUsleep(1)` after 10000 tries) |
| `+0x80` | marker array (2 entries; 0 = slot free, else the slot's start cursor) |
| `+0x88` / `+0x90` | block base / block end |
| `+0x98` | cursor |
| `+0xa0` | spill cursor |
| `+0xa8` / `+0xb0` | current slot limit / slot start |
| `+0xb8` / `+0xbc` | slot count (2) / current slot index |

**Rotate `0x48270`** (vtable slot 4, called from `0x90860` at `0x90a9b` and `0x90aac`, once per frame):
- clears the marker of the slot being left if it equals the cursor (slot had no allocation);
- advances the index modulo 2;
- if the other slot has a nonzero marker, sets the new slot's limit to that marker (wrapping to block end when
  the marker is behind the cursor) and keeps the cursor where it is; otherwise cursor = block base;
- sets the new marker to the cursor. **No GPU value is read and no fence is consulted.**

**Alloc `0x48440`** (vtable slot 20): bump allocation from the cursor, checked against the limit, with spill
through `+0xa0`. Returns 0 on failure. Only the lock is a wait (CPU spin).

Consequence (**observed**, from the arithmetic): the rotation never lets the new frame overwrite the
previous frame's data (the limit is the previous slot's start). Older frames are overwritten only when the
cursor laps the whole block, i.e. after about `block size / per-frame usage` frames. The headroom is a
size margin, not a wait. The per-frame usage is **unknown** (high-water at `+0xd0` would give it at runtime).

## 3. Who uses the rings (the open question)

- **observed:** ring pointers (`ctx+0x580`, `ctx+0x670`) appear only in `0x8f6f0` (init), `0x90860` (reset
  and rotate), `0x90050` (free, `0x47620`) and a destructor in `0x25xxxx`. No field-offset reference to
  `ctx+0x580` / `ctx+0x670` elsewhere in the decomp (checked for hex and decimal forms).
- **observed:** no direct or indirect call site of the alloc slot (`0x48440`) was found.
- **unknown:** the caller(s) of vtable slot 20 on either ring. Hypotheses: a stored pointer to the ring
  object (not found by offset grep), or a generic allocator interface. Until a user is found, what the GPU
  reads or writes in the rings is **unknown**.

## 4. Per-frame order and the 8-slot table (observed)

- `EndFrameViewParams` (`0xcefa0`) calls `0x118440`, which zeroes three 32-byte blocks and calls `0x90860`.
- `0x90860`: resets ring A and ring B (`0x48270`), resets the context list (`ctx+0x18`), builds the per-flip
  context (`0x90470`, DCB and CCB from family 1), embeds the frame label block (`0x92500(cmd, 0x27, 1)`, stored at
  `frame+0xe8`), embeds 8 slot labels (`0x92500(cmd, 8, 0)`), and fills the 24-entry table at `frame+0x40`
  for all 8 slots: GPU data `ctx+0x7a0 + 200000*k`, GPU dword `ctx+0x7a8 + 4k`, label `label + 8k`.
- The table is rebuilt every frame for all 8 slots. Which slot a reader uses (frame number modulo 8 or a
  fixed index) is **unknown**. The table reads are not traced. Entry `[0x12]` is an artefact (doc 09 section 2.3).
- **observed:** the 8 labels are EOP targets (`0x86eb60` = EVENT_WRITE_EOP), so the GPU writes them in the DCB.
- **unknown:** whether any GPU engine reads the 1.6 MB slot data (`ctx+0x7a0`) or the dword block (`ctx+0x7a8`).

## 5. Waits that could tie reuse to the GPU (observed unless marked)

| Wait | Where | Ties reuse to GPU completion? |
|---|---|---|
| Ring rotation | `0x48270` | No (marker arithmetic only) |
| Game Render event waits | `RenderThread::ThreadFunction` `0xbd717`, `0xbd908`, `0xbd93d` (`sub_3b4a0`) | No known label or flip value. Producer of event C (`R+0x20`) is **unknown** (report 06) |
| RenderFlip thread | `0x960a0`, `sceKernelWaitEqueue` at `0x960f2` | Flip events only (no frame or label value) |
| Label polls | `0x92b50`, `0x925c0`, `0x92500` callers | GPU-side (`WAIT_REG_MEM` in the DCB); the CPU does not poll a label in these functions |
| SubmitDone | `sceGnmSubmitDone` thunk `0x882ce0`, called once per frame (mode 1, `KickoffSubmitJob` `0xd07c0`) | Yes, but only through the port's gate (below). The engine itself does not wait on a value |
| Flip-mode change | `0x956e0` (vblank spin) | Mode change only |

No engine-side wait compares a label value or a submit count with a ring slot. The engine's own
guarantee is therefore the frame boundary plus the ring margin, not a fence.

## 6. Port side (how submits and labels complete)

- `sceGnmSubmitDone` (`gnmdriver.cpp`, around 2475) calls `GateWait(2)` (**observed**, report 06).
- `GateWait` (`gnmdriver.cpp:254`): with `SOTC_NATIVE` on, it returns at once and calls
  `SotcNativeSync::WaitReuse()` (one frame ticket). Otherwise mode 2 (`SOTC_CP_AHEAD=2`, frame tickets, or
  `SOTC_FRAME_AHEAD`) waits until `marked - lag` frames are **recorded** by the command processor, with
  `lag = 1` (frame-ahead) or `SOTC_CP_AHEAD_FRAMES` (default 1, range 0..3). Mode 0 or 1 (default at site 1
  `GateWait(1)`, line 549) waits for the GPU-idle IRQ (`WaitGpuIdle`).
- **observed:** "recorded" means parsed by the CP thread, not executed by the draw thread. So under the frame-ticket
  modes the CPU can build frame N+1 while the draw thread still executes frame N. Lag is bounded only by the
  CP-to-draw queue, not by a GPU value.
- **observed:** under the default mode, `WaitGpuIdle` at the frame boundary means the GPU has finished frame N
  before the CPU begins frame N+1's ring writes. That is the one condition under which reuse is safe.
- **unknown:** `label_honest.cpp` (`SOTC_LABELS_HONEST`) and the EOP label handling in the command processor
  were not read. Whether the port signals a guest label or flip before Vulkan consumed the ring data is
  therefore **unknown**. If the port writes a label value early, a CPU-side reader would see it early. Not checked.

**Conditions for SAFE (summary):** the default gate (mode 0/1, `WaitGpuIdle` at each frame boundary), `SOTC_NATIVE`
off, `SOTC_CP_AHEAD` and `SOTC_FRAME_AHEAD` off, and the GPU never reads or writes the CPU ring. The last
condition is **unknown**, so the status is UNSAFE for the fast path.

## 7. Measurement added (Part 2, =2 only)

`SOTC_FRAME_RINGS=2` (`=1` is the same measurement; no fast path exists):
- hook `RING_INIT` on entry of `0x481f0` (`scripts/hooks.d/frame_rings.py`, pattern `c6 47 08 01 48 89 97 80`,
  EU expected `0x481f0`); the probe keeps the ring whose name at `+0x10` is `RenderContextFrameCPU` or
  `RenderContextFrameGPU` (`gpu/sotc/probe_frame_rings.cpp`);
- `BufferCache::RecordCurrentWrite` calls `NoteWrite`: records inside the CPU ring are "fast ops" F;
  `NativeExactUpload` (the CPU copy path) calls `NoteCpuPath`, so verify X = F minus those CPU-origin records;
- report line (5 s, `driver.cpp`): `SOTC FRAME RINGS: rings found N (cpu base/size, gpu base/size), fast ops F,
  verify gpu-writes-in-cpu-ring X`.

Not verified by a run (the main session builds and runs). Syntax checked only. Risks: the hook addresses
are from the EU eboot (the US eboot is scanned by the signature); `RecordCurrentWrite` addresses are assumed to
be guest addresses in the same space as the ring base.

## 8. Unknowns and next checks

1. Caller(s) of ring alloc slot 20 (`0x48440`) on `ctx+0x580` / `ctx+0x670`. Method: the `RING_INIT` probe gives the
   ring; a return-address log on `0x48440` (entry hook) gives the callers.
2. Whether the GPU reads or writes the CPU ring. Method: X from the =2 run. X > 0 means the fast path is wrong.
3. Per-frame ring usage (`+0xd0` high-water) and the slot used by each frame: needed for the size margin.
4. Reader of the 8-slot table (`frame+0x40`) and of `ctx+0x7a0`: frame index and label values.
5. `label_honest.cpp` and the command processor's EOP label write timing (section 6).
