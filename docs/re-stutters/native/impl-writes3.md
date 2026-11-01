# N2 completion: material copy, view blocks, view table, instance transform (specialist: writes3)

Status: the four items of the brief are done for the EU 1.01 eboot (fingerprint 72c1d4a9...). Nothing committed. Files are
uncommitted in the worktree; base copies of the N0 docs, the tools and the main-tree `native_hooks.cpp` / `native_writes.py`
were copied from the main tree first (the worktree base is older).

Result of the checks:
- `tools/re/hooks_check.py` (scratch copy of the main tree with this worktree's `native_writes.py`): 84/84 hooks resolve to
  one site (the four new ones at their expected offsets).
- `tools/re/hooks_install_check.py` (same scratch copy): 83/84 accepted. The one refused hook is `RENDER_JOB_RUN` 0xc12a0
  ("function has an unresolved indirect branch"), declared in a file this agent does not own; it is not one of the
  native_writes hooks. No new CONFLICT line: the CONFLICT lines are the older XPR / PC_POOL / MEMTYPE / NATIVE_MASK pairs.
- Syntax: `gpu/sotc/native/native_hooks.cpp` passes `clang++ -fsyntax-only -Wall -Wextra` with the project flags of
  `out/gpu/compile_commands.json` (the `hook_handlers.cpp` line, source swapped to the worktree copy, worktree `gpu/` include dirs
  first). No warnings.
- Not run: the game, the build, mode 2 verify. The rel32 bytes of two patterns (below) are checked against this eboot only.

## 1. Files

| File | State | Content |
|---|---|---|
| `scripts/hooks.d/native_writes.py` | changed | 4 declarations added (17 existing unchanged): NATIVE_MAT_COPY, NATIVE_VIEW_BLOCK, NATIVE_VIEW_TABLE, NATIVE_INST_18AA20 |
| `gpu/sotc/native/native_hooks.cpp` | changed | 4 handlers (`MatCopy`, `ViewBlockPre/Post`, `ViewTable`, `InstClass2Entry`), `Window::view` counter, `view` class in `Report()`, 4 `RegisterHandler` lines |
| `docs/re-stutters/native/impl-writes3.md` | new | this report |

No other file was changed. `hook_handlers.cpp` and `driver.cpp` need no change beyond what impl-writes.md section 5 already
states (`SotcNativeHooks::Report` call in driver.cpp, still not present in the main tree: see section 6).

## 2. Hooks added

| Hook | Group | Offset (EU) | Mode | Pattern (unique in the text segment) | Handler and announce |
|---|---|---|---|---|---|
| NATIVE_MAT_COPY | native_matcopy | 0x11788f | Site | `48 8d 05 ?? ?? ?? ?? 41 8b 14 24 4c 8b 64 24 08 48 89 d9 48 8b 30 c1 ea 02 4c 89 e7 41 89 d0 e8 dd 72 05 00` | Dest `rbx`, size `u32 [r12]`, PerFrame (counted in `mat`). |
| NATIVE_VIEW_BLOCK | native_view | 0x8c120 | Wrap | `55 48 89 e5 41 56 53 48 8d 05 ?? ?? ?? ?? 48 89 fb 49 89 f6 ba 10 00 00` | Pre: caller filter, size `u32 [rdi+0x20]`, handle `rsi`. Post: block = `u32 [h] \| (u32 [h+4] & 0xfff) << 32`, pushed to the chunk list (announced PerFrame at the next DCB/CCB submit). |
| NATIVE_VIEW_TABLE | native_view | 0xcef0d | Site | `48 83 c4 08 5b 41 5e 41 5f 5d c3 48 8b 12 89 fb` | `r14 + rbx` (table base + view index * 16), 16 bytes, PerFrame (counted in `view`). |
| NATIVE_INST_18AA20 | native_inst2 | 0x18aa20 | Entry | `55 48 89 e5 41 56 53 48 83 ec 60 4c 8b 35 ?? ?? ?? ?? 48 89 fb c5 fa 10` | If `*(rdi+0xa8) != 0`: that block, 0x18 bytes, PerFrame (counted in `inst`). |

Groups: `native_matcopy`, `native_view` (the two view hooks share a group: they are one write path), `native_inst2` (separate
from `native_inst` of 0x121ed0, so a refusal of one takes down only its own path).

Snippets of the patterns' exact bytes are in the declarations (`scripts/hooks.d/native_writes.py`, the comments name each site).

### 2.1 Material copy 0x117160 (item 1)
- The copy is `memcpy(rbx, *DAT_02ed2108, [r12])` at 0x11788a (`call 0xf8f470`); its return site 0x11788f is followed by
  `mov edx,[r12]` and the GPU DMA `call 0x16eb90` (r8 = size >> 2). So the block at rbx is the source of the DMA, and it is
  rebuilt each frame (the profiler scope "Material Param Update" wraps it; the per-frame caller was not traced). PerFrame.
- The 0x11788f signature is not unique: sig tool gives "2 matches within 80 bytes". The second match is
  `CRenderer::UpdateTextureStreamingGPUData` (0x118046), which has the same memcpy + lea + mov sequence and a different DMA
  (`call 0x928f0`). The shared bytes are the whole instruction run up to the DMA `call`; the rel32 of that call differs
  (`e8 dd 72 05 00` here, `e8 6d a8 f7 ff` there). The pattern keeps those 5 bytes exact, so it is unique
  (`eboot_re.py sigfind` gives 0x11788f only). On another eboot the exact rel32 may not match, and the hook is then refused
  rather than placed at a wrong site. Only the EU fingerprint is declared, so the expected offset is used.
- Site order: the handler runs after the memcpy and before the displaced `lea rax` and the DMA. r12 still holds 0x2ed2100
  at that point (set at 0x117856, reused at 0x11789a), so the size read is the one the memcpy used.

### 2.2 View blocks 0x8c120 (item 2)
- `eboot_re.py callers 8c120`: exactly two direct callers, `sub_bdfd0` (0xbe055, range 0xbdfd0-0xbe1f0) and `SetViewParams`
  (0xbe27b, range 0xbe1f0-0xc0b6f). Both are the caller filter (`kViewBlockCallers`).
- 0x8c120 takes the block from `0x12820` (size `[rdi+0x20]`, category from `0x2ed1f54`), copies the 16-byte record of `[rdi]` into
  `[rsi]` and stores the block into the handle at `rsi` (low word at `[rsi]`, high 12 bits at `[rsi+4]`, same layout as
  0x8c0d0). The post phase reads the handle from memory (r14 is stashed in the pre phase), like `ChunkPost`.
- The block is not written by 0x8c120 itself: the callers fill it. The announce is the whole block, PerFrame, through the
  chunk list, the same policy as the chunk writers (conservative: a block that is not written is uploaded but not wrong).
- **Correction to the brief:** `0xcedf0` (EndFrameView_WriteB) does not get its blocks from 0x8c120. It has no call to 0x8c120
  and no allocation; it writes 16 bytes into a persistent table at `[*(0x2ee3ce0)+0x330] + edi*16` (the `sub_8f630` calls only
  compute the two floats). That is handled by NATIVE_VIEW_TABLE (section 2.3).

### 2.3 View table 0xcedf0 (item 2, the second writer)
- EndFrameView_WriteB stores 16 bytes at `r14 + rbx` (r14 = table base, rbx = view index * 16). Every path reaches the common
  store `mov [r14+rbx+8], rdx` at 0xcef08, and 0xcef0d follows it, so the Site after that store covers all paths.
- The table is persistent but rewritten each frame (`EndFrameViewParams`), so PerFrame.

### 2.4 Instance class 2 0x18aa20 (item 3)
- The body reads `*(rbx+0xa8)` at 0x18aaaf (it does not change it) and, when it is set, writes the 0x18-byte block there
  (`[rax+0..0x17]`, 0x18ab4a..0x18ab5e). An Entry hook reads the same pointer before the body.
- Direct callers: `UpdateGPUData_InstanceData` (0x118210, the class 2 case of the 0x118100 switch) and `sub_1b32f0` (0x1b35ba,
  called by `sub_1b0980`). Both are kept (no caller filter: the two callers are both instance writers; I did not check whether
  `sub_1b32f0` runs per frame, so the announce may cover a non-per-frame call with 24 bytes).
- Safety of the early announce: PerFrame data is read at the next submit, after the job waits of 0xd07c0 (the same argument as
  NATIVE_INST_121ED0).

## 3. Ordering and safety
- Site announces (0x11788f, 0xcef0d): after the store, before the next instruction; the displaced instructions do not write
  the announced range.
- Wrap view block (0x8c120): the block is queued at the post phase and reaches the GPU only through `FlushChunks()` at the next
  DCB or CCB submit (0x91f80 / 0x16f100), after the job waits of 0xd07c0. Same as the chunk writers.
- Entry 0x18aa20: the announce precedes the body; the bytes are read at the next submit (see 2.4).
- Mode 0 (`SOTC_NATIVE` = 0): every handler returns Continue before reading anything, except the view Pre (it clears
  `t_view.active` and reads only the switch). No behaviour change.

## 4. Counters
- `SOTC NATIVE WRITES PF ... mat <n> <MiB> ...` now also counts the 0x117160 copy (`mat` = MaterialParam sub-buffers plus the
  material copy). `inst` also counts 0x18aa20.
- New class `view <n> <MiB>` (NATIVE_VIEW_TABLE bytes). View blocks are counted in `chunk` (they go through the chunk flush).
- `Report()` line format: one more ` view <n> <MiB>` at the end of the PF line.

## 5. Sweep: CPU->GPU per-frame writers (framedata.md section 4, heaps.md section 7)

| framedata row | Writer | Status |
|---|---|---|
| 1 | 0x118100 UpdateGPUData | dispatch covered per class: 0x121ed0 (hook), 0x123410 and 0x11c710 (chunk), 0x18aa20 (new), 0x193e40 (CPU only, none) |
| 2, 3 | 0x123410, 0x11c710 | chunk list (caller filter), existing |
| 4, 18 | 0xbe1f0 SetViewParams, 0xcedf0, 0x8b7f0 | 0xbe1f0 blocks: NATIVE_VIEW_BLOCK (new). 0xcedf0 table: NATIVE_VIEW_TABLE (new). 0x8b7f0: chunk caller, existing |
| 5, 7, 8, 32, 36 | 0x928f0, 0x865410, 0x8655d0, 0x85c580, 0x865600 | descriptor hooks, existing. 0x928f0 8-byte block (count, stride): from 0x12820, encoded by 0x8655d0 with size 8 (verified in the disasm): NATIVE_DESC_8655D0 covers it |
| 6 | 0x861ce0 / 0x861e60 user-data shadow | CPU shadow, sent through the DCB: covered by the DCB announce |
| 9, 10, 11, 33, 34 | DCB embedded data, context ctor, DCB markers | DCB/CCB announce (0x91f80, 0x16f100), existing |
| 12, 16 | 0xd07c0, 0x91f80, 0x16f100 | submit hooks, existing (0x91f90 is the doorbell thunk: no writes) |
| 14 | 0xc12a0 RunFunctionAsRenderJob | hook refused (unresolved indirect branch, not this agent's hook). Its object creation 0x90670 / 0x906d0 allocates through 0x12300 and 0x17090 (CPU pointer vectors, memmove of pointers): CPU only. The GPU block of the 0x1af0 object is not located: open |
| 19 | 0x101720 particle shift | NATIVE_PARTICLE_SHIFT, existing |
| 22, 23 | 0x8c0d0, 0x12820 | 0x8c0d0: chunk hook. 0x12820 has 226 direct callers; only the ones routed through the hooked allocators are covered. Open (see section 6) |
| 28, 35 | 0x118470, 0x8be60 | zero-copy and resource copy hooks, existing |
| 29 | 0x121ed0 | NATIVE_INST_121ED0, existing |
| 31 | 0x18aa20 | NATIVE_INST_18AA20 (new) |
| 37 | 0x95650 / 0x95690 / 0x91f50 | flip (sync / not per frame): out of scope |

Remaining writers of heaps.md 7.4 (0x8be60 buffers, memcpy/memset writers): 0x94fb0, 0x198bb0, 0x19b760, 0x19c1a0, 0x19c7e0,
0xffe60 (zero-copy hooks, existing); 0xf5cd0 and 0xfef40 (MaterialParam, existing); 0xd8f40 and 0xdfcf0 (memcpy writers) are
load/setup paths per heaps.md 7.4 and not hooked. 0x1264d0 / 0x112db0 / 0x1abba0 memset writers are build-time descriptor
clears, not hooked.

## 6. What remains
1. **0x12820 direct callers (226).** Any per-frame writer that calls 0x12820 without passing through 0x8c0d0, 0x8be60, 0x8c120 or
   0x928f0 is not announced. The walk of heaps.md 7.5 counted memcpy callers of 0x8c0d0 only; a memcpy walk of the 226 direct
   callers of 0x12820 is the next step. Unclassified, not a known gap.
2. **0xc12a0 RENDER_JOB_RUN** is refused (unresolved indirect branch). Its owner must resolve it; the 0x1af0 object's GPU block is
   not located.
3. **driver.cpp** still needs `SotcNativeHooks::Report(report, seconds);` after the `SotcNative::Report` call (impl-writes.md section 5,
   unchanged by this work).
4. **Mode 2 verify.** The new hooks are not run. Expected: the `unannounced` count for the material copy, the view blocks and
   the instance class 2 transform drops. NATIVE_MAT_COPY keeps the exact rel32 of its DMA call (`e8 dd 72 05 00`), so it is
   EU-only by construction; another build is refused, not placed at a wrong site.
5. The `sub_1b32f0` per-frame status (section 2.4) is not verified.

## 7. Files outside this agent's ownership
None changed. Base copies of `docs/re-stutters/native/*`, `tools/re/*`, `scripts/*.py` and `gpu/sotc/native/*` were copied from
the main tree into the worktree before editing (the worktree base predated them); they are identical to the main tree, except
the two files this agent changed.
