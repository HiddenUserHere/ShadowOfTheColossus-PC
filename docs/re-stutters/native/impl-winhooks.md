# Native allocator windows: hook wiring (winhooks)

Wires the window entry points of `gpu/sotc/pc_pool.cpp` (SetupWindows, BindRenderTargets, SetHeapKey, ClearHeapKey,
WindowRelease, WindowReuse) into the running game, all behind `SOTC_NATIVE`. Uncommitted, in the worktree
`agent-adbbd730768836158`. Built nothing and ran nothing: syntax-only compile and the install check.

## Files

| File | Change |
|---|---|
| `gpu/sotc/hooks.h` | `HookAction::SkipOriginal = 3` (Wrap pre: skip the original, run the post phase with the handler's rax/rdx/xmm0). |
| `gpu/sotc/hooks.cpp` `RunDispatch` | SkipOriginal maps to Return at an Entry, is kept at a Wrap, is Continue at Site and Time. The Wrap rule "Return is ignored" is unchanged (`hook_detour_test.cpp` still holds). `CountPre` counts SkipOriginal as a return. |
| `gpu/sotc/cpu_patches.cpp` `EmitGuestDetour` | Wrap stubs only: after the save, `cmp eax, SkipOriginal; je skip_original`. `skip_original` restores the registers and joins the post phase, so the post runs with the handler's rax and the original is not called. Every other path emits the same code as before (the call path gained one `jmp` to the post label). |
| `gpu/sotc/probe_alloc.cpp` | `AllocPre` (ALLOC_BLOCK pre), after the frame push: `if (SotcNative::On()) return SotcNativeWindowHooks::AllocPre(regs);`. `FreePre` split into `ForgetNode` (the probe's node bookkeeping) and the pool's free check. New `SotcProbes::NoteWindowRelease(node)` = `ForgetNode`. |
| `gpu/sotc/probe_memtype.cpp` | `AllocPre`: `MemtypePre(raw_slot)` (heap key). `AllocPost`: `MemtypePost()` first, and the granule claims only when `g_post_granules` (classes mode or placement glue, as before). `g_post_on` also when SOTC_NATIVE is on, so the post phase runs and clears the key. |
| `gpu/sotc/native/native_windows_hooks.h` / `.cpp` (new) | `AllocPre` (RT bind, owner-enabled check, WindowReuse, SkipOriginal), `MemtypePre` / `MemtypePost`, handlers `NATIVE_WINDOW_RELEASE` (0x476f0 entry) and `NATIVE_WINDOW_SETUP` (0x1171f site), `Register()`. |
| `scripts/hooks.d/native_windows.py` (new) | Records NATIVE_WINDOW_RELEASE and NATIVE_WINDOW_SETUP, switch `SOTC_NATIVE`, one group each. |

Mode 0: `SotcNative::On()` is false, so the probes take the old code paths; the two new hooks are not installed (switch
unset); the Wrap stub gains one compare that never matches. `hook_detour_test` is unaffected (Wrap Return still ignored).

## Hooks

| Name | Offset | Mode | Switch | What it does |
|---|---|---|---|---|
| (existing) ALLOC_BLOCK | 0x479a0 | wrap | SOTC_HOOK_ALLOC_PROBE | pre: RT bind, owner-enabled check, WindowReuse (SkipOriginal). post: unchanged probe. |
| (existing) MEMTYPE_ALLOC | 0x12820 | wrap | SOTC_HOOK_MEMTYPE_PROBE | pre: SetHeapKey(memtype). post: ClearHeapKey (the wrap's post runs on every return path). |
| NATIVE_WINDOW_RELEASE | 0x476f0 | entry | SOTC_NATIVE | `if (WindowRelease(rdi, rsi)) { probe forgets rsi; return Return; }`, otherwise the original. |
| NATIVE_WINDOW_SETUP | 0x1171f | site | SOTC_NATIVE | `SetupWindows()` once per session; logs the two arena pointers once. |

### Where the render-target bind is (no new hook for 0x9b778)

`BindRenderTargets(rt_obj)` is called from the ALLOC_BLOCK pre (no new hook). The RT object is the owner of the RT
block request: `FUN_0009aef0` (decompile `out/re/decomp/0009/0009aef0.c`) creates the object at lines 472-482
(`FUN_000475c0(plVar25, "RTAllocator", 2)`, stored at `param_1+0x4d78`), then calls `FUN_000479a0(lVar43, 0x14280000,
0x10000, *(lVar43+0x18))` at line 491, where `lVar43 = *(param_1+0x4d78)`, the same object. So at the 0x479a0 pre the
owner rdi is the RTAllocator object, its name (char* at +0x10) is "RTAllocator", the kind (rcx) is 2 and rsi is
0x14280000. The PS4 Pro size (0x23d10000) is not windowed (window C serves only 0x14280000). 0x9b778 is not used and
has no decompile; the RT argument is verified above.

### Why the existing hooks, and what changed in the engine

- 0x479a0 and 0x12820 already carry wraps, and two hooks cannot displace the same bytes. So the calls ride on those
  handlers. The window calls need an early return with rax set: a Wrap could not return before the change, and the
  window reuse must skip the original (0x19ae0 and the list stores). That is the new `SkipOriginal` action (above).
- The 0x12820 post phase is the wrap's post (it runs after the original on every return path), so ClearHeapKey needs no
  exit hook. This is what `impl-windows.md` asked for ("exit needs a wrap hook").
- 0x476f0 is an entry hook (its pattern is unique; 12 bytes; expected offset 0x476f0). Its handler skips the whole
  function when WindowRelease takes the node: the function's stores are done by WindowRelease and the call to 0x1b0e0
  (pool_free_block) is skipped. The FREE_BLOCK probe hook is therefore not called for a parked block, so the probe
  calls `ForgetNode` itself (without the pool's `OnFreeNode`, because the pool keeps the block in use).
- 0x110a0: the site is 0x1171f (`mov edi, 0x2e0`), after the kind-2 arena store (`mov [0x2dfb2b8], rbx` at 0x11718).
  The site is also the target of `jne` at 0x115cd, which is taken when the kind-2 arena already exists, so it runs on
  both paths. A Site hook runs before the displaced instruction; the site is the instruction after the store, so
  the arena pointer is already set. SetupWindows reads the arenas itself.

## Ordering and safety

1. ALLOC pre: the frame is pushed first, so the post pops it also for SkipOriginal. The owner-enabled byte (+9) is
   read before WindowReuse, as 0x479a0 does before its own work, so a disabled owner gets the original's 0.
2. The reuse size is `(rsi + 0xffff) & ~0xffff`, as 0x479a0 rounds it.
3. The probe's post for a reused node adds it to the live table again (the release removed it), so the probe's live
   counts match the game's use. Nothing else in the probe changes.
4. The memtype key is set in the pre and cleared in the post. A nested 0x12820 clears the key early; then window B
   does not match and the call takes the normal path (safe, counted as not routed). It cannot route a wrong key.
5. Hook installs are checked by the detour engine (below). The window path itself is inert without the pool's native
   path: `WindowsOn()` needs `NativeActive()` (SOTC_PC_POOL_NATIVE and the FREE_BLOCK hook).

## Snippets for files I do not own

`gpu/sotc/hook_handlers.cpp` (include next to the other native includes, register in `RegisterAllHandlers`):

```cpp
#include "native/native_windows_hooks.h"   // sotc: SOTC_NATIVE allocator windows
...
    SotcNativeWindowHooks::Register();     // sotc: SOTC_NATIVE windows (native_windows.py), after SotcNativeResults
```

`sotc.py`, after the `SOTC_PC_RING` defaults (around line 369), so the probes the windows ride on are installed in native mode:

```python
    # sotc: the native windows (gpu/sotc/native/native_windows_hooks.cpp) need the allocator and memtype probes.
    if env.get('SOTC_NATIVE', '0') not in ('', '0'):
        env.setdefault('SOTC_HOOK_ALLOC_PROBE', '1')
        env.setdefault('SOTC_HOOK_MEMTYPE_PROBE', '1')
```

Note: MEMTYPE_ALLOC and PC_RING_ALLOC (`SOTC_PC_RING_DISPATCH`, comparison only) share the wrap at 0x12820. The install
check reports that conflict (the later one is refused). Do not set SOTC_PC_RING_DISPATCH in native mode.

`SOTC_HOOK_MEMTYPE_PROBE` off means no heap key: window B never routes (safe). `SOTC_HOOK_ALLOC_PROBE` off means no
reuse and no RT bind; the NATIVE_WINDOW_RELEASE hook alone then parks nothing (WindowRelease needs the window path).

## Install check

`tools/re/hooks_install_check.py` run on a staged copy of the main tree's `scripts/` with `native_windows.py` added, against
`out/eboot.elf`: 81 of 82 accepted. The refused one is `RENDER_JOB_RUN` (0xc12a0, unresolved indirect branch; not mine).
Both new hooks are accepted: `NATIVE_WINDOW_RELEASE 0x476f0 accepted mode=entry displaced=6`, `NATIVE_WINDOW_SETUP
0x1171f accepted mode=site displaced=5`. No new CONFLICT lines. The existing CONFLICT lines are the switch-exclusive
pairs (for example `PC_POOL_ALLOC`/`PC_POOL_NATIVE`, `MEMTYPE_ALLOC`/`PC_RING_ALLOC`), as before.

## Syntax check

`clang++ -fsyntax-only` (the `out/gpu/compile_commands.json` flags of `cpu_patches.cpp`, plus `-Wall -Wextra`) over an
overlay of the main tree's `gpu/sotc` with these edits: `cpu_patches.cpp`, `hooks.cpp`, `probe_alloc.cpp`,
`probe_memtype.cpp`, `native/native_windows_hooks.cpp`. No errors. The warnings in `cpu_patches.cpp` (unused TCB helpers,
a sign compare) are in code I did not touch.

## Counters and what to look for at the first run

- `SOTC HOOK NATIVE_WINDOW_RELEASE 0x476f0 expected` and `SOTC HOOK NATIVE_WINDOW_SETUP 0x1171f expected` (the manifest).
- `SOTC NATIVE WINDOWS: setup at 0x1171f: GPU arena 0x... (read), CPU arena 0x... (read)` once.
- `SOTC PC POOL WINDOWS` (ReportNative, pool): per window live, parked, routed, served, full, committed, parked,
  reused, reuse miss, and the reserve summary (reserved, refused by host or limit, register failed).
- Window C should show one route at the first RT allocation (the RTAllocator owner). If it shows "reserve_limit", the
  port placed the reserve above 0xFC01000000 (correction 2 of impl-windows.md).

## Open items and what remains

1. Not run in the game. The first run has to show the setup line, the RT bind, and the reuse and park counters.
2. The probe's reuse path re-adds the node to its tables (see point 3 above). Probe counts for a parked-then-reused
   block are counted once per release and once per reuse, which is what the game does; not verified in the run.
3. The owner list stores of WindowReuse (0x479a0) and WindowRelease (0x476f0) run without the pool lock, as the game's
   own stores after 0x19ae0 do; the game takes the pool lock inside 0x19ae0, which the reuse skips. The release's
   stores are also outside the lock. Single-thread ordering is assumed (the same as the game's own list code); a
   concurrent allocation of the same owner was not checked.
4. `hook_detour_test.cpp` has no case for SkipOriginal (it cannot be built here by rule). A case "Wrap SkipOriginal runs
   the post with the handler's rax and not the original" would close the stub change.
5. `RegisterWindow` exists now in `gpu/sotc/native/native.cpp` (impl-windows open item 2 is resolved in the main tree).
6. The 0x12820 heap key is per thread; a nested 0x12820 on the same thread clears it early (point 4 above).
7. Mode 2 (verify) has no separate counters here: the pool's verify of the windowed call and its window checks are the
   ones in `impl-windows.md`.
