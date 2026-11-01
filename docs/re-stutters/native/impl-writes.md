# N2 implementation: write announces (specialist: writes)

Status: partial. Five persistent-write and mip sites are hooked and checked. The per-frame, descriptor and DCB/CCB
sites of the N2 list are not hooked yet (section 4). Nothing committed; files are uncommitted in the worktree.

## 1. Files

| File | State | Content |
|---|---|---|
| `scripts/hooks.d/native_writes.py` | new | five Site hooks, switch `SOTC_NATIVE`, one group per write path |
| `gpu/sotc/native/native_hooks.cpp` | new | handlers, coverage counters, `Report()`, `Register()` |

Syntax check: `native_hooks.cpp` passes `clang++ -fsyntax-only` (C++23, `-Wall -Wextra`), with `native.h` copied from
the main tree to a scratch include directory (not into the worktree; the header is owned by the API agent).

Hook check: `tools/re/hooks_install_check.py` run from a scratch copy of the main tree's `scripts/` and `tools/`
with the new file added, against `out/eboot.elf` (EU 1.01, fingerprint 72c1d4a9...): all five hooks accepted by the
detour engine (`SOTC DETOUR NATIVE_* accepted mode=site`). The existing 53 hooks are unchanged in the main tree. The
worktree's copy of `tools/re/` is older and lacks the checker, so the check was run from the scratch copy.

## 2. Hooks

All are `mode='site'`, `switch='SOTC_NATIVE'`. Patterns are `eboot_re.py sig` output, unique in the text segment.

| Hook | Group | Offset (EU) | Displaced | Handler | Action |
|---|---|---|---|---|---|
| NATIVE_RES_COPY | native_res | 0x8bf50 | 7 bytes | after the initial-data memcpy in the resource wrapper 0x8be60 | `Announce(r15, u32[rbx+0x20], Persistent)` |
| NATIVE_TEX_COPY | native_tex | 0x96b59 | 7 bytes | after the single-block texel copy in 0x96680 (texture, particle texture) | `Announce(r14, rbx, Persistent)` |
| NATIVE_TEX_TAIL_COPY | native_tex | 0x96b1b | 7 bytes | after the mip-tail texel copy in 0x96680 | `Announce(r13, r12, Persistent)` |
| NATIVE_MIP_COPY | native_mip | 0x98d6c | 11 bytes | after the mip memcpy in the streamer 0x98ae0 | `Announce(r13, r14, Persistent)` |
| NATIVE_MIP_RELEASE | native_mip | 0x98be0 | 5 bytes | before the mode-2 decommit call (0x98bf0) in 0x98ae0 | `Release(rdi, r14)` |

### Register evidence (from `eboot_re.py dis` and the decompilation)
- 0x8bf50: after `call memcpy` at 0x8bf4b, r15 = destination (set at 0x8bf0b/0x8bf3f), rbx = resource object, the size
  is `[rbx+0x20]` (u32, the `imul` product), and the flags word is `[rbp-0x34]` (param_5). Callee-saved registers survive
  the call; the size and flags are read from guest memory through `runtime_memory_backing_pointer`. This site is
  reached only when `(flags & 0x800) == 0`, so zero-copy resources never announce here.
- 0x96b59: `add [r15+0x1b0], ebx` directly follows `call 0x8de50` at 0x96b54 (dest r14 from 0x12820, size rbx).
- 0x96b1b: `add [r15+0x1b0], r12d` follows `call 0x8de50` at 0x96b16 (dest r13 = uVar2 + uVar9). r12 is the accounted
  size (the same value the decompilation adds as `lVar16`). The copy's own size is `[rbp-0x90]`; the announce uses
  the accounted size, which covers it. **Verify this by reading the register assignments once more** (the mip-tail path
  is the least direct of the five).
- 0x98d6c: `mov rdi,r13 / mov rdx,r14` before `call memcpy` (0x98d67); r13 = pool destination, r14 = mip size.
- 0x98be0: `mov edx,2` before `call 0x12950` (0x98bf0, mode 2 = decommit). rdi = pool base (`uVar7`), r14 = size
  (`r13 - rdi`, set at 0x98bd4). The site is the instruction before the call, so it does not overlap the
  `DECOMMIT_BEGIN` hook (0x98bf0) owned by `scripts/hooks.d/decommit.py`.

### Corrections to the N0 inventory
- The particle texture (0x10e9b0) calls 0x96680 with `&DAT_02dd9f80`, which is the **CPUMemory** category (loader.md
  says GPUMemory). The announce is the same either way (the upload path does not depend on the heap), but the
  category counts in the report say CPU for particle textures.
- The memcpy in 0x8be60 writes with the scope or category of the flags. The heaps walk (heaps.md 7.4) says
  flags 0x2 without 0x800 copy the initial data into RenderSingleFrameGPU. This is the same site and the same announce.

## 3. Ordering and safety

- Every announce is placed after its copy has returned and before the function returns to the guest. The copy and
  the announce run on the same guest thread, so the announce cannot precede the copy's data.
- Persistent ranges are announced once per copy. Mip copies are announced when they land; mip drops `Release` the
  pool range before the decommit, so the device copy is dropped before the memory goes away.
- The `MIP_BUDGET_COPY` Wrap (scripts/hooks.d/mipbudget.py) sits at the entry of 0x98ae0; `NATIVE_MIP_COPY` and
  `NATIVE_MIP_RELEASE` are later in the function and do not overlap it (`SOTC_NATIVE` and `SOTC_MIP_BUDGET` can be on
  together). The decommit hooks (`DECOMMIT_BEGIN` at 0x98bf0, `DECOMMIT_END` at 0x98bf5) are untouched.
- The publish hook (0x14d670) is not installed. The copies are announced when they land, which is before publish, so
  publish needs no announce of its own. If the API agent wants a publish-time fallback, the entry hook is
  `NATIVE_PUBLISH` at 0x14d670 (pattern from `eboot_re.py sig 0x14d670`, 28 bytes, unique).
- Every handler returns Continue and reads only the registers and guest words named above. When `SotcNative::On()` is
  false the handlers do nothing (the hooks are off anyway unless `SOTC_NATIVE` is set).

## 4. Not done (remaining N2 sites)

- **Per-frame writers** (rewriters, from heaps.md 7.4 and 7.5): the zero-copy particle buffers (`0xffe60`, slot 22,
  per frame, three sites), BPXPR draw/instance buffers (`0x19b760`, `0x19c1a0`, `0x19c7e0`), MaterialParam sub-buffers
  (`0xf5cd0`, `0xfef40`, and the 4 MiB copy in `0x117160`), and the 27 chunk memcpy callers of 0x8c0d0 (heaps.md 7.5).
  Each needs a Site after its memcpy/memset; not written yet because each caller's register map has to be read first.
- **UpdateGPUData 0x118100**: the dirty-span walk (renderer+0x210070) and the tail before the EOP at 0x92720. Not
  hooked: the instance writers below are announced one by one instead.
- **Instance writers** 0x123410, 0x11c710, 0x121ed0, 0x18aa20 and the view writers 0xbe1f0 / 0xcedf0 / 0x8b7f0:
  the per-block announce belongs after each writer's last store. Not read in full yet (0x123410 is 210 lines; its tail
  writes the block state back at `puVar42`).
- **DCB and CCB**: announce `[start, cursor)` before each `sceGnmSubmitCommandBuffersForWorkload` (0x91f80, called
  from 0xd07c0) and CCB at 0x16f100. Not hooked.
- **Descriptor side records** (`NoteDescriptor`): entry hooks on the V#/T# encoders 0x865410, 0x8655d0, 0x865600,
  0x865500 need their argument order read (not read yet).
- **Particle record shift** 0x101720 (PERS, 96-byte shift): Site after its memmove. Not read.
- **Verify counters per class** exist only for the hooked sites (section 5).

## 5. Counters and verify

`SotcNativeHooks::Report(std::string& out, double seconds)` appends one line per window and resets:
`SOTC NATIVE WRITES res <announces> <MiB> (gpu <MiB> cpu <MiB>) tex <n> <MiB> mip <n> <MiB> rel <n> <MiB> skipped <n> per <s> s`.

Snippets for files this agent does not own:

`gpu/sotc/hook_handlers.cpp`, add with the other declarations (before `namespace SotcHooks {`):
```cpp
namespace SotcNativeHooks { void Register(); void Report(std::string& out, double seconds); }
```
and in `RegisterAllHandlers()`, after `SotcCtl::StartIfEnabled();`:
```cpp
    SotcNativeHooks::Register(); // sotc: PC-native write announces (SOTC_NATIVE, native/native_hooks.cpp)
```
(`<string>` is already included through hooks.h.)

Driver snippet (the 5 s report, next to the other reports): call `SotcNativeHooks::Report(out, seconds);` after the
`SotcNative::Report` call, so the coverage line is printed with `SOTC_FPS` / `SOTC_VERIFY`.

Verify mode (mode 2) checks the announce coverage against the page tracker only once the per-frame sites exist: the
counters above count the persistent and mip bytes announced. The per-class frame counters for the rest are not in
this report.

## 6. Open points

1. `NATIVE_TEX_TAIL_COPY` needs one re-read of the register assignments before the first game run (section 2).
2. The API agent's `Announce` must accept `Persistent` for ranges in CPUMemory (particle textures) and GPUMemory.
3. The per-frame sites (section 4) decide whether the verify mode's "unannounced writes = 0" can pass; until they are
   hooked, page tracking will count every per-frame write as unannounced.
