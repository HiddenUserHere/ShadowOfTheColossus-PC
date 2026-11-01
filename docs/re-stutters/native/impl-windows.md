# Native allocator windows (impl-windows)

Implementation of the window layout of `allocators.md` (sections 5 to 7): per-owner VA windows A (GPU arena, 128 MiB), B (XPR mask heap, 16 MiB), C (render targets, 352 MiB). D (GPU static) keeps its per-chunk import, no window. Uncommitted, in the worktree `agent-ae2dd1c5f784935d9`. Built and tested with the MSYS2 CLANG64 toolchain (unit tests only; the game was not run).

## Files

| File | Change |
|---|---|
| `gpu/sotc/pc_pool_windows.h` (new) | Window, Table (route, park, reuse, counters), WindowFreshHost, ReportTable. |
| `gpu/sotc/pc_pool_windows.cpp` (new) | Implementation. Pure bookkeeping; the only host call is `Table::Reserve`. |
| `gpu/sotc/native/native_windows.h` / `.cpp` (new) | Bridge to `SotcNative`: `ModeOn()` (SotcNative::On) and `RegisterRange()` (SotcNative::RegisterWindow). The only file of the windows that calls native.h. |
| `gpu/sotc/pc_pool.h` | `Request::force_fresh`; `kFreshPathMask`; declarations of the hook entry points `SetupWindows`, `BindRenderTargets`, `SetHeapKey`, `ClearHeapKey`, `WindowRelease`, `WindowReuse`. |
| `gpu/sotc/pc_pool.cpp` | `Predict`: steps 1 and 3 (the free-VA searches) are skipped when `force_fresh`. `Controller::Run`: eligibility uses `kFreshPathMask` for a forced-fresh call. Glue: `WindowTable()`, `WindowsOn()`, `ReserveSlot`, `NativeHost` (window host, `window_slot_`, give-back on refusal), `NativeEntry` (route, commit), `ReportNative` (windows report), and the entry points. |
| `gpu/sotc/tests/pc_pool_windows_test.cpp` (new) | 130 checks: window bookkeeping, table, fallbacks, park/reuse, WindowFreshHost, forced-fresh prediction. |

Mode 0: nothing changes. `NativeEntry` returns before any window code unless native mode is on; `force_fresh` stays false; the `Predict` and eligibility changes are guarded by `force_fresh`.

## Corrections to `allocators.md`

1. Render-target sizes are wrong in the hex-to-MiB conversion. `0x14280000` is **322.5 MiB** (not 320 MiB), and `0x23d10000` is **573 MiB** (not 595 MiB). Window C is 352 MiB and serves exactly the 0x14280000 block (`Spec(2).block`). The 573 MiB request takes the normal path.
2. The address limit is stricter than 2^40. The owner-tag table (`root+0x111908`, one byte per MiB, index `VA >> 20`) is modelled only up to `root+0x20d918` (`pc_pool_native_dispatch.cpp` InModelled). That is 0xfc010 MiB, so a window must end below **0xFC01000000**. `Window::Init` and `Table::Reserve` enforce `min(0xFC01000000, 2^40)`. A window above it is refused and counted (`reserve_limit`), and its VA is leaked at boot. Where the port places reserved ranges is unknown; if they land above 0xFC01000000, the windows stay inactive. This must be checked at run time.

## Design (what each piece does)

- **Window**: one reserved range, 1 MiB aligned. An address-ordered free list (`start -> end`) with first fit on the aligned start. The pad before the aligned start stays free, and the tail stays free. `Give` coalesces with both neighbours. Live blocks are `start -> bytes`.
- **Table**: three slots. `Reserve(slot)` reserves the VA from the host (1 MiB aligned, limit-checked) and registers it. `Bind(slot, obj, key)` binds a window to the owner object (the family object the pool wrapper gets in rdi, which is the arena for 0x479a0). `Route` matches owner, kind, key and exact block size, and checks capacity. A bound window that is full returns -1 (counted `full`), so the call takes the normal path.
- **Forced fresh route**: a windowed call must not use a free-VA run, or the pool's free lists would pick the VA. `Request::force_fresh` makes `Predict` skip steps 1 and 3, so the prediction is the fresh path (its VA is the window's, not predicted, `va_align` = `max(1 MiB, align64)`). The early reject and the page loop are unchanged, so phys accounting is the pool's own.
- **WindowFreshHost**: the `FreshHost` the fresh writers use for a windowed call. `ReserveVirtual` takes from the selected window; if the window cannot place it, it uses the normal host (the call stays native, counted). `BatchMap` always goes to the normal host.
- **Commit**: after a verified native return, the window's block is bound to its node (`node_of`). A refused call gives its VA back (`GiveVa`) when nothing was written.
- **Release park** (`WindowRelease`, 0x476f0): when the node is the live block of its node in a window, the hook performs the 0x476f0 stores before its call (node+0x18 and node+0 zeroed, unlink through node+8/+0x10, self-links, owner count -1, owner bytes -= node+0x50), and skips the call to 0x1b0e0. The pool sets are not touched, so the pool's accounting keeps the block in use, and its VA and phys stay mapped.
- **Reuse** (`WindowReuse`, 0x479a0 entry): a parked node of the owner's binding and size (lowest VA first) is inserted into the owner's list with the 0x479a0 stores (node+0, +8, +0x10, +0x18, owner head, old head+8, owner count +1, owner bytes += node+0x58), and the node is returned. No pool call, no batch op.
- **Heap key**: window B is keyed by memtype 17 (RenderSingleFrameCPU). The key is a thread-local set by the dispatcher 0x12820 (memtype = rsi) and cleared at its exit. Without a scope, the key is -2 and B never matches. Window A has no key (any).

## Ordering and safety argument

1. Windowed calls use the same writer as any fresh call (`ApplyFreshPlain` / `Spill` / `Reserve` / `PoolFix`). The only difference is the VA source, and the writer already checks alignment (`HostVa`) and the whole guest state. A window VA that is misaligned is refused before any write.
2. The VA is taken from a window's free list only inside `ReserveVirtual`, that is in the real run, after the dry run has passed. A refusal after the take gives it back (`GiveVa`) when the write list is empty; a partial write kills the session, as today.
3. Park and reuse never touch the pool's set state (region list, free-VA list, bitmap, counts). So the controller's expectation and the periodic checks are unaffected, and the release does not reach `OnFreeNode` (it is called from 0x1b0e0, which is skipped).
4. Windows never return VA to the pool's free lists, so the game's VA leak does not reach them, and no quarantine is needed.
5. Capacity: A holds 4 blocks, B 2, C 1. A full window routes the call to the normal path and counts it.

## Counters

`SOTC PC POOL WINDOWS` report line per active window: live, parked, free MiB, routed, served, full, committed, given back, take failed, parked (refused), reused (miss). Plus the reserve summary (reserved, refused by host or limit, register failed). Printed from `ReportNative` (native mode only).

## Verify (mode 2)

- The existing controller verifies every native call: the post state against the windowed prediction, the write set against the modelled memory, and the sets the prediction leaves untouched. A window VA is accepted by `Compare` through `va_align` (any multiple of `max(1 MiB, align64)`).
- Window checks: `Commit` requires the block to be live in its window (`take_failed` otherwise); `Release` requires the same node and size (`park_refused` otherwise).
- **Not done**: the planned "run the non-window path and compare" is not implemented. The non-window prediction (an exact hit) changes different state (the free-VA list), so a comparison would be a second model, not a check on the window. The verify is the windowed prediction plus the counters above.

## Hook sites (not in my owned files: `scripts/hooks.d/`, `gpu/sotc/hook_handlers.cpp`)

| Site | When | Call | Result |
|---|---|---|---|
| 0x110a0, after the kind-2 arena creation (0x115e0) | once per session | `SotcPcPool::SetupWindows()` | window A and B reserved, registered and bound (reads the arena objects at 0x2dfb2b8, 0x2dfb2c0). Kind-1 arena (0x114bb) must come first. |
| 0x9b778 entry | render-target allocator | `SotcPcPool::BindRenderTargets(rt_obj)` | window C reserved and bound. **rt_obj is the open item**: the register or memory operand of the RT object is not in `out/re/decomp` (0x9b778 is not decompiled there). |
| 0x12820 entry (memtype = rsi) and exit | every dispatch | `SetHeapKey(rsi)` / `ClearHeapKey()` | the heap key of the call on this thread. Exit needs a wrap hook (all return paths). |
| 0x476f0 entry (owner = rdi, node = rsi) | every release | `if (SotcPcPool::WindowRelease(rdi, rsi)) return;` | window park; the rest of 0x476f0 is skipped |
| 0x479a0 entry (owner = rdi, size = rsi rounded up to 64 KiB) | every allocation | `if (byte[owner+9] && WindowReuse(rdi, size, &node)) { rax = node; return; }` | reuse of a parked node. The owner-enabled check is the one 0x479a0 makes at +9. |

The register names of 0x476f0 and 0x479a0 follow their decompile signatures (`FUN_000476f0(long owner, undefined8 *node)`, `FUN_000479a0(long owner, long size)`). The hook records follow `scripts/hooks.d/pcpool.py` (name, switch SOTC_NATIVE, group, mode, pattern, expected offsets); the handler names to register are the ones in `pc_pool.cpp` `Register()` (not done here; hook names are for the hooks owner to choose).

## Snippets for files I do not own

`gpu/sotc/hook_handlers.cpp` (add to `RegisterAllHandlers`, handler functions with the registers above):

```cpp
// 0x476f0 entry
static SotcHooks::HookAction WindowReleaseEntry(SotcHooks::GuestRegs& r, void*) {
    return SotcPcPool::WindowRelease(r.rdi, r.rsi) ? SotcHooks::HookAction::Return : SotcHooks::HookAction::Continue;
}
// 0x479a0 entry
static SotcHooks::HookAction WindowReuseEntry(SotcHooks::GuestRegs& r, void*) {
    uint64_t node = 0;
    const uint64_t size = (r.rsi + 0xffffu) & ~uint64_t{0xffffu};
    if (r.rdi != 0 && SotcPcPool::WindowReuse(r.rdi, size, &node)) { // (owner enabled byte check goes here)
        r.rax = node;
        return SotcHooks::HookAction::Return;
    }
    return SotcHooks::HookAction::Continue;
}
```

`SotcPcPool::Register()` does not register these names yet; add them there or in `hook_handlers.cpp`. `WindowRelease` and `WindowReuse` return false when the window path is off, so the hooks are inert in mode 0 and mode 1 without windows.

## Open items

1. **0x9b778 object register** (RT binding), see the table above.
2. **RegisterWindow** is declared in `gpu/sotc/native/native.h`, but I found no implementation in the main tree. `native_windows.cpp` calls it; the link fails until the native module provides it. `RegisterRange` failures are counted and do not disable the window.
3. **Boot registration of a range that is not yet mapped** (window reserve at 0x110a0): the window is reserved and registered before any block is mapped. Whether the port's import accepts an unmapped range is unknown; a refusal is counted (`register failed`).
4. **Arena objects at 0x2dfb2b8 / 0x2dfb2c0**: the arena object pointers are taken from the globals named in `allocators.md` section 2.1 ("ptr"). This is inferred; the first SetupWindows must log the values (it does not yet).
5. **Window placement**: whether the port's reserve returns VA below 0xFC01000000 (see correction 2). If not, A and B are refused and the feature is inert.
6. **Kind-1 blocks shared across keys**: the 0x14240 global walk can carve from window B for other memtypes. This is placement only (not correctness), but it means B does not guarantee that its blocks hold only the mask heap's data. The exclusion from the walk (allocators.md section 6, B) is not implemented.
7. **Not run in game**: none of the window paths has been exercised by the game. Mode 2 needs a run with `SOTC_NATIVE=2 SOTC_PC_POOL_NATIVE=1 SOTC_PC_POOL_REFILL=1` (with the hooks above) and the report counters.

## Tests run

- `pc_pool_windows_test` (new): 130 checks, PASS.
- `pc_pool_native_test`: 901 checks, PASS. `pc_pool_test`: PASS. `pc_pool_native_fresh_test` (with fresh and reserve sources): 111 checks, PASS. `pc_pool_dispatch_test` (with its six sources): 46 checks, PASS. `pc_pool_native_primary_test` (with `pc_pool_native.cpp`): 1850 checks, PASS.
- Syntax only: `gpu/sotc/pc_pool.cpp` (glue, with the main tree's include directories and `bb_win_compat.h`), `gpu/sotc/native/native_windows.cpp`, `gpu/sotc/pc_pool_windows.cpp`: no errors or warnings.
- Not built: the full bbgpu target (ninja), by rule.
