# Handoff: native render backend cutover (branch `native/render-vk13`) — state after N2-48

Repo: `C:\Users\ptr\Documents\sotc-native` (Windows, Git Bash, MSYS2 CLANG64 toolchain).
Scope: reimplement the whole RenderJob render backend of the Shadow of the Colossus PS4→PC port
(CUSA08809 EU 1.01 eboot on the shadPS4 fork) natively in C++ issuing Vulkan 1.3 directly,
removing the PM4 emulation layer while keeping the GCN→SPIR-V recompiler.

This document is the briefing for the next agent. It records what was done and verified up to
2026-11-21/22 (gap entry **N2-48** in `docs/engine/native-render-gaps.md`), what was learned from
the runs, and the concrete next steps.

---

## 1. Hard rules (binding)

- **Never launch the game without the user's explicit authorization FOR THAT RUN.** A "go" for one
  run is not standing authorization; ask again each time through the chat. When authorized, launch
  only with `skipSplash=1 nointro` (the run script already exports that).
- **No subagent may ever execute the game** (not `bb-probe.exe`, not `sotc.py`, not bench/play
  scripts) in any phase, under any authorization. Runtime verification is the MAIN session's step.
- Never copy game files or Sony modules into the repo; never distribute `.sprx`/`.prx`.
- Ghidra hex constants mislead on PM4 packet headers — verify against raw bytes in `out/eboot.elf`
  (VA→file offset = VA + 0x4000).
- Do not `source msys2-env.sh` in the same shell before a launch (it clears the `SOTC_*` env vars);
  export `MSYS2_ROOT` instead.
- Never kill processes that run from the repo.
- Docs/comments/commits in English; conversational replies in the user's language (PT-BR).
- Use `rg`, never `grep -r`; read file ranges with Read offset/limit.
- Work continuously; do not stop between phases.

## 2. Build, run, verify

```bash
export MSYS2_ROOT=/c/Users/ptr/Documents/Codex/toolchains/msys64
export PATH="$MSYS2_ROOT/clang64/bin:$PATH"
cd /c/Users/ptr/Documents/sotc-native
ninja -C out/gpu-phase3 bb-probe.exe        # green as of 2026-11-22 05:11
```

- New files under `gpu/native/` are picked up by `file(GLOB_NATIVE CONFIGURE_DEPENDS ...)`; after
  adding one, re-run `cmake -S gpu -B out/gpu-phase3` once or ninja may miss it.
- Ninja's globbing occasionally misses edits in `src/runtime*.c`; `touch` the file to force.
- Runtime C files compile with `-Wall -Wextra -Werror`.
- Verified run script: `bash out/re/run_native_probe.sh <log-name> [VAR=value ...]` → log at
  `out/re/cutover/<log-name>.log`. It exports `SOTC_CVARS="skipSplash=1 nointro"`,
  `BB_VBLANK_HZ=120`, `BB_GPU_USER_DIR`, `BB_DMEM_MB` default **9152** (launcher parity; a lower
  value reproduces the 1.09 GiB OOM), applies the overrides AFTER those, then a `BB_RUN_SECONDS`
  cap (default 150). Keep an eye on the `grep` order bug that was fixed: per-run overrides must come
  last.
- First boot may stop at guest offset `0xa5d064` (the game not finding a file); the script retries
  once.
- Gap table validator: `python out/check_pipes.py` → must print `bad 0`. Every N2 row must have
  exactly 7 pipes.

Useful env vars (all read at process start):

| var | meaning |
| --- | --- |
| `SOTC_RENDER` | stage selector; `native-pass` installs the pass shims (0x93960/0x94600/0x93ef0) but NOT the 0x93690/0x92140 entry hooks; `native` installs every hook; `off` = pure emulated path |
| `SOTC_NATIVE_STREAM_OPS=0` | (N2-65) native ops record immediately from the hook thread instead of going through the PM4 NOP stream op (racy; A/B only) |
| `SOTC_NATIVE_PASS_A=0` / `SOTC_NATIVE_PASS_B=0` / `SOTC_NATIVE_PASS_FENCE=0` / `SOTC_NATIVE_CLEAR_RECORD=0` | (N2-59..N2-61) bisection switches for the pass bodies |
| `SOTC_NATIVE_PASS_DEPTH=1` | (N2-49) run the OLD native 0x93ef0 body, a depth clear that is semantically wrong; bisection only. Default: the 0x93ef0 shim classifies and traces the call, then chains to the guest |
| `SOTC_RECORD_THREAD=0` | disables the recording thread. No longer needed for native-pass since N2-65/N2-68 (stream ops record on the producer); it was required before because of the single-producer abort (N2-39/N2-40) |
| `SOTC_RENDER_VERBOSE=1` | turns on the budgeted traces |
| `SOTC_NATIVE_TRACE_BUDGET` | trace lines before the shared budget closes (default 64) |
| `SOTC_PASS_TEMPLATE=1` | every pass call goes straight to the guest body (template run) |
| `BB_STACKS_TRIGGER=<file>` | touch the file → every other thread's rip + rbp chain is dumped |
| `SOTC_PASS_TRACE=1` | per-flip pass list from the emulated path (depth target, size, colour target, draws) |

## 3. Where the code is

| file | role |
| --- | --- |
| `gpu/sotc/hooks.{h,cpp}`, `gpu/sotc/hook_table.inc` | entry detours (14-byte abs jump + relocated-prologue trampoline, `OriginalEntry`, child trampolines for unhooked VAs), hook table by stage |
| `gpu/native/native_hooks.cpp` | 0x93690 (`RunNativeClear`, `TryNativeDepthClear`, `NativeClearHasStagedDepth`), draw objects 0x94ec0/0x94d00/0x94dd0/0x10add0/0xdb620, RT binder 0x92140, material bind 0x85fc10, cull loop 0x11d3d0 |
| `gpu/native/native_pass_callbacks.cpp` | pass shims 0x93960 (`RunPassA`), 0x94600 (`RunPassB`), 0x93ef0 (`RunPassDepth`); `PassFullscreenRect` calls the native 0x93690 body directly |
| `gpu/native/native_pass.{h,cpp}` | `TargetSet`, `Rt::DecodeColour/DecodeDepth`, `Pass::Begin/End`, `Pass::ClearDepth`, `Pass::ResolveViews`, EOP/WAIT bridge (`Rt::Eop`) |
| `gpu/native/native_draw.{h,cpp}` | `Draw::IssueFromRegs` → `Rasterizer::DrawNative` (the port's own GPU-state path: targets, transitions, descriptors, user data, viewport, binds) |
| `gpu/native/native_stream_ops.{h,cpp}` | (N2-65) native ops placed in the guest PM4 stream as `SotcNativeOp` NOPs, run by the command processor's NOP handler (`liverpool.cpp`) on its draw thread |
| `gpu/native/native_stack.{h,cpp}` | (N2-63) runs the pass shims' native part on a 1 MiB host stack (guest job fibers are 16-64 KiB) |
| `gpu/native/native_clear_pass.{h,cpp}` | **new**: colour-strip module (`DecodeTargets`, `ReplayTargets`). Decodes an 0x92140 target manager and records a rendering scope over the target's views (`eLoad`, optional fullscreen rect). Compiles; NOT wired to any hook yet. |
| `gpu/native/native_shadow.*`, `native_camera.*`, `native_tables.*`, `native_frame.*`, `native_cull.*`, `native_fill.*`, `native_bind.*` | later phases; mostly still chain to the guest |
| `src/probe.c` | bb-probe: crash handler (`fatal_exception`, `print_rbp_chain`, `describe_address`, `bb_runtime_backtrace/bb_runtime_describe`), fiber/stack tooling |
| `src/runtime_fiber.c` | fibers; `check_overflow` reports the fiber + host chain |
| `docs/engine/native-render-gaps.md` | the gap register (N2-1 … N2-48; read N2-32, N2-39…N2-48 first) |
| `docs/engine/native-render-map*.md` | proven maps: pass functions, draw objects, binder, frame, cull/camera/shadow |

## 4. What was done in this session (verified)

### 4.1 Depth gate (N2-46/N2-47 follow-up) — FIXED

- `NativeClearWantsDepthClear` relied on bits `pass+0x11` bit0 and `pass+0x21` bit0. Trace shows both
  are reused by unrelated guest state: run `probe-passonly-11` logged `dirty=0xff ... b21=0x43` on a
  COLOR pass whose cached block is 0; the 0x93690 chain itself writes `pass+0x11 = 0xfe`.
- New rule in `native_hooks.cpp`:
  - `NativeClearHasStagedDepth(pass, explicit_block)`: true when the caller passes an explicit
    block (0x94600's `desc+0x164`), or when `pass+0x11` bit0 is set AND `pass+0xa8` decodes through
    `Rt::DecodeDepth` as `Z16`/`Z32F` (`AmdGpu::DepthBuffer::ZFormat`).
  - `RunNativeClear` uses only that for the depth-vs-colour decision.
- Result (`probe-passonly-12/13/15`):
  `SOTC RENDER native: 0x93690 pass=0x124d0f9680 depth clear block=0x124670c8e4 depth=0.0 (768x540) issued natively`
  — the shadow cascade clear is now a native `vkCmdClearDepthStencilImage`.

### 4.2 `Pass::ClearDepth` made rendering-scope safe

The clear recorded `vkCmdPipelineBarrier2` + `vkCmdClearDepthStencilImage` unconditionally. If the
scheduler had a dynamic-rendering scope open on an image the scope uses, the transition is illegal
and the driver faults (`nvoglv64+0xe1373a`). Now `record_clear` closes the in-flight scope with
`scheduler.EndRendering()`, records barrier/clear/barrier, and reopens it with
`scheduler.BeginRendering(saved_state)` (eLoad load ops are always legal; the scheduler has no
metadata pass).

### 4.3 Diagnostics added

- `src/probe.c`: `print_rbp_chain()` — safe, bounded rbp-chain dump with `describe_address` naming
  ("guest offset 0x…" for guest frames, "module+0x…" for host), called on every fatal fault right
  after the registers and before the existing dump (which adds locals).
- `src/runtime_fiber.c`: the fiber-overflow report now prints the host chain via
  `bb_runtime_backtrace`/`bb_runtime_describe` (those exist in `probe.c`). Unused `fiber_trace`
  removed.
- Verbose traces: `0x93690 depth-path pass=… block=… cached=… dirty=… force=… b21=…`.

### 4.4 Colour-strip module (new, not wired)

`gpu/native/native_clear_pass.{h,cpp}`:
- `DecodeTargets(const u64* M, TargetSet&)` — the RT binder's decode (type gate `(b|2)==3`, colour
  block at `+0xe4`/`+0x124` by selector, DB block at `+0x164`, extents at `+0x198/+0x19c`) without
  the viewer-wide extent sanity test.
- `ReplayTargets(set, pipeline)` — `ResolveViews` + `Renderer::Scheduler().HoldRecorder(...)`
  recording `Pass::Begin` (all colour attachments `eLoad`/`eStore`, writable depth `eLoad`) +
  `SetPassViewportScissor` + optional fullscreen rect + `Pass::End`.
Idea: the engine's colour-strip helper binds the target manager M with a call-site pointer (e.g.
0x152120 in the 0x1472e1 call), so M can be captured and the strip replayed as a scope over the
*current* targets instead of a full clear (the old plan cleared — that would have erased the scene).

## 5. What the runs showed (diagnosis)

Latest runs (`probe-passonly-12..15`, `SOTC_RENDER=native-pass SOTC_RECORD_THREAD=0`):

1. The cascade depth clear issues natively. ✅
2. Pass `0x124d406500` (0x93960, `dims=1920x1080 kind=274334208 prim=0x6 color=3`) declines with
   `pipeline unresolved`: `ResolvePassFullscreenPipeline` (material content key) misses and
   `Bind::LookupFullscreenProgram` has no producer (N2-32). `prim=0x6` is a *triangle list*, so this
   is a **scene draw through the pass shim**, not a fullscreen rect. It needs the real pipeline path.
3. **Corrected by N2-49:** 0x93ef0 *is* hooked under `native-pass`. Its calls were traced under
   0x94600's name, and its native body cleared the 1920x1080 depth to 0.0 although the guest does
   an in-place depth decompress (bound == target) or a depth copy bound -> target. The fault in
   item 4 is printed before the 0x93690 clear and right after two of those native 0x93ef0 clears.
4. **Main wall**: `Fault: read access to ffffffffffffffff at nvoglv64.dll+0xe1373a` (or
   `+0xe9a91f`), on the recording thread, logged immediately after the depth clear line. `0xffffffff`
   is the descriptor/page-table "invalid" sentinel of the port. Just before, the guest is inside the
   colour-strip helper: `0x14328a` → `0x1472e1` (`0x1472e1` calls `0x1431f0` region), which calls
   `0x39a73`/`0x38ad2` (looks like a save/adjust/restore of a global colour-register state) and then
   binds descriptor `0x152120` with `esi = 0`. All occurrences are at the same stack depth; only
   sub-frames vary between runs → consistent with a race / thread-local state.
5. With the scope-safe `ClearDepth` the fault window once showed instead a **guest write-ahead**
   fault at `0x8eb0f` (`probe-passonly-14`: `mov eax,[rbx+rax+0x100]` reading unmapped memory, guest
   chain `0x1472e1` → `0x14328a`; `rbx = unk_18a0a5f0`, saved in frame `0x15644f` → `0x174828ff`
   computes `frame[0x0e] + 0x100 - 5`). Not yet attributed.
6. No native draw is ever issued: `g_issue_trace_budget` (64 lines) has no separate output and no
   `draw count=… issued` line appears; the fault happens before the first scene draw.

## 6. Next steps (in order)

1. **Done differently (N2-49):** 0x93ef0 was already hooked. The shim now classifies the call
   (`ClassifyDepthPass`), traces it and chains to the guest by default; `SOTC_NATIVE_PASS_DEPTH=1`
   brings back the old clear for bisection. Run A (default) / B (`=1`) decide whether that clear
   caused `nvoglv64+0xe1373a`. A real native body is a `CopyDepthStencil` bound -> target.
2. **Find where a guest-side fix invalidates the descriptors.** Use `SOTC_PASS_TRACE=1` with
   `SOTC_RECORD_THREAD=0` on the emulated path over the same 4 s window: if the emulated path also
   enters `0x14328a` at the same moment, compare the GPU commands around it (the native
   `ClearDepth` barriers are the only GPU-side difference). If the emulated path does not enter it,
   the strip is being reached only because a native stage changed guest state — bisect with
   `SOTC_NATIVE_EMIT=0` and per-stage expels.
3. **Determine whether the scene pass must be native before the strips.** The strip replay only
   makes sense once the scene draws (`prim=0x6`) are native; otherwise the emulated draws and a
   native scope interleave on the same attachments. Priority: give `ResolvePassFullscreenPipeline`
   a real producer (N2-32) so the fullscreen rect path resolves and the pass shim stops chaining.
4. **Attribute `0x8eb0f`** (`unk_18a0a5f0` / `0x174828ff`): read the bytes at `0x1563e0..0x156460`
   and `0x174828e0..` in `out/eboot.elf`; it decodes a buffer length (`rbx+0x100` floor buffer?).
5. **Wire the colour-strip module only after (2).** Plan: patch the strip helper head 0x1431f0 once
   (region head), let the guest body run, and record the replay after it; capture M from the
   call-site pointer (`0x152120`). Do not land a clear — the strip blends over the scene.
6. **Register everything in `docs/engine/native-render-gaps.md`** as new N2 rows (exactly 7 pipes)
   and re-run `python out/check_pipes.py` (expect `bad 0`).

## 7. Known-good reference points

- Honest reference run with 0 hooks, 9152 MiB: stable at 30 FPS.
- `SOTC_RENDER=native-pass SOTC_RECORD_THREAD=0` reaches the depth-clear issue and the strip fault
  every time (~4–5 s in, ~150–200 trace lines).
- Previous runs and their conclusions: `probe-native-trace.log`, `probe-native-fix{1..8}.log`,
  `probe-passonly-{1..15}.log` in `out/re/cutover/`.
