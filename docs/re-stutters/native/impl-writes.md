# N2 implementation: write announces (specialist: writes)

Status: the N2 sites of the list are hooked except the ones in section 4. Nothing committed; the files are uncommitted
in the worktree (base: main tree, branch re/pc-native, uncommitted native_hooks.cpp and native_writes.py).

## 1. Files

| File | State | Content |
|---|---|---|
| `scripts/hooks.d/native_writes.py` | changed | 17 declarations, switch `SOTC_NATIVE`, one group per write path |
| `gpu/sotc/native/native_hooks.cpp` | changed | handlers, caller filters, chunk list, coverage counters, `Report()`, `Register()` |

Syntax check: `native_hooks.cpp` passes `clang++ -fsyntax-only -Wall -Wextra` with the project flags of
`gpu/sotc/native/` (from `out/gpu/compile_commands.json`, the `area_prewarm.cpp` line), the worktree's `gpu/` include
dirs first, and a scratch copy of `native.h` (the header is owned by the API agent and is not in the worktree).

Hook check: `tools/re/hooks_install_check.py` on a scratch copy of the main tree (`scripts/`, `tools/`, `out/eboot.elf`)
with the worktree's `native_writes.py`: all 17 hooks `accepted`. The refused hooks and the CONFLICT lines in that run are
all older declarations (XPR, PC_POOL, MEMTYPE), none of them native_writes. One change came from the check: the
instance hook 0x121ed0 is refused as Wrap (indirect jump), so it is an Entry hook.

`scripts/` unit tests (`python -m unittest discover -s tests`, scratch copy): 160 run, the 5 import errors are
missing modules in the scratch copy (`paths`), not these files.

## 2. Hooks

Mode meanings: Entry = handler before the body; Wrap = pre stashes, post announces after the body; Site = one
instruction inside a function. All hooks use `switch='SOTC_NATIVE'` and the handlers return Continue.

First batch (unchanged): NATIVE_RES_COPY 0x8bf50 (Site, Persistent), NATIVE_TEX_COPY 0x96b59 (Site, Persistent),
NATIVE_TEX_TAIL_COPY 0x96b1b (Site, Persistent), NATIVE_MIP_COPY 0x98d6c (Site, Persistent), NATIVE_MIP_RELEASE 0x98be0
(Site, Release).

N2 batch:

| Hook | Group | Offset (EU) | Mode | Handler and announce |
|---|---|---|---|---|
| NATIVE_DCB_SUBMIT | native_submit | 0x91f80 | Entry | the submit thunk: `ctx = [rdi]` (the 0x8636e0 context). Announce DCB `[ctx+0x00, ctx+0x10)`, CCB `[+0x40, +0x50)`, ACE `[+0x80, +0x90)`, PerFrame. Then flush the chunk list. |
| NATIVE_CCB_SUBMIT | native_submit | 0x16f100 | Entry | CCB object rdi: `[u32 +0x10, u32 +0x20)` (`& ~3`), PerFrame. Then flush. |
| NATIVE_ZC_CREATE | native_zc | 0x8be60 | Entry | resource creator with flag 0x800 (zero-copy) and a backing r9, called from the zero-copy builders (section 2.1): `[r9, edx*ecx)` PerFrame. |
| NATIVE_CHUNK | native_chunk | 0x8c0d0 | Wrap | pre: caller (retaddr at rsp) in the chunk-writer ranges, size esi (0: handle+0x20). post: block = handle[0] \| (handle[1] & 0xfff) << 32, queued. |
| NATIVE_MATPARAM | native_mat | 0xf5cd0 | Wrap | post: `[u64 +0x08, u32 +0x38)` PerFrame (the MaterialParam sub-buffer). |
| NATIVE_FEF40_BLOCK | native_mat | 0xfef40 | Wrap | post: block `[+0x48]`, size u32 at `[param_2+4]`, PerFrame (the memcpy target). |
| NATIVE_INST_121ED0 | native_inst | 0x121ed0 | Entry | `[rdi+0x1d8]` 0x18 bytes, PerFrame (wrap refused: indirect jump). |
| NATIVE_DESC_865410 | native_desc | 0x865410 | Wrap | buffer V#: post reads word1 stride (bits 16..29), word3 format; size = stride * count (rcx). |
| NATIVE_DESC_8655D0 | native_desc | 0x8655d0 | Wrap | constant V#: size = edx bytes; format = word3. |
| NATIVE_DESC_865600 | native_desc | 0x865600 | Wrap | image/buffer word encoder: size = edx bytes; format = word3. |
| NATIVE_DESC_865500 | native_desc | 0x865500 | Wrap | buffer V# with stride and count (as 865410). |
| NATIVE_PARTICLE_SHIFT | native_particle | 0x101817 | Site | after the record writes of ParticleRenderer_ShiftRecords: source `r14+r15` and destination `[rbp-0x30]+r15`, each stride bytes (u32 at this+0x74), Persistent. |

### 2.1 Caller filters and why they are needed
- `NATIVE_ZC_CREATE` and `NATIVE_CHUNK` are funnels: 0x8be60 and 0x8c0d0 are called from about 100 and 153 functions, most
  of them CPU-only. The handler reads the caller's return address (rsp at entry) and announces only the callers
  listed in `gpu/sotc/native/native_hooks.cpp` (`kZeroCopyCallers`, `kChunkCallers`, image offsets from
  `tools/re/eboot_re.py func`). Zero-copy callers: particles 0xffe60, BPXPR 0x19b760 / 0x19c1a0 / 0x19c7e0, the draw
  buffers 0x94fb0 and 0x198bb0. Chunk callers: the 27 memcpy callers of heaps.md 7.5, the instance writers 0x11c710 and
  0x123410, and EndFrameView_WriteA 0x8b7f0.
- An unfiltered announce would upload every CPU-only chunk (CPUMemory allocations are about 700k calls per second) as
  PerFrame fallback. The filter is what keeps the cost to the GPU-read data.
- The filter ranges are for the EU 1.01 image (the same build as US 1.00). Another eboot resolves nothing (fingerprint),
  so the filter cannot drift silently.

### 2.2 Chunk list (one announce point for the chunk writers)
- The post of 0x8c0d0 queues `(block, size)` for the chunk writers. `FlushChunks()` runs at every DCB and CCB submit
  entry (0x91f80, 0x16f100), before the GPU reads the submitted command buffers, and announces everything queued as
  PerFrame. The list is capped at 2^17 entries; overflow is counted (`dropped`).
- Ordering: the chunk writers fill their chunks before the submit (0xd07c0 waits on the jobs first), so every
  announce follows its writes. A chunk queued after a flush is announced at the next submit, which is the first submit
  that can reference it.

### 2.3 Ordering of the Entry announces (PerFrame)
- For the zero-copy creators that memcpy before the call (0x94fb0, 0x19c1a0, 0x19c7e0, 0x198bb0) the announce follows
  the writes. For the particle and BPXPR builders (0xffe60, 0x19b760) the bytes are written after the call. PerFrame data
  is read at the next submit (uploaded there when no window holds it), and each submit is preceded by the job waits of
  0xd07c0, so the early announce is safe. This is the only place where the announce is before the writes; it is the
  reason the verify mode checks PerFrame classes at submit, not at the announce.

## 3. Counters and verify

`SotcNativeHooks::Report(std::string& out, double seconds)` appends two lines per 5 s window and resets:
- `SOTC NATIVE WRITES res ... tex ... mip ... rel ... skipped ... per <s> s` (first batch, unchanged);
- `SOTC NATIVE WRITES PF dcb <n> <MiB> ccb <n> <MiB> chunk <n> <MiB> zc <n> <MiB> mat <n> <MiB> blk <n> <MiB> inst <n>
  <MiB> part <n> <MiB> desc <n> dropped <n>`: announced PerFrame bytes per class, and the particle record bytes
  (Persistent). Divide the MiB by the frames in the window (30 FPS locked) for MiB per frame.

Verify mode (mode 2): the counters count what the hooks announced; the page tracker (the old path, still running in
mode 2) counts the writes the announces missed. Once these sites exist, `unannounced` should fall for the classes above.
The per-class totals here are the announced side of that comparison.

## 4. Not hooked (remaining N2 work)

- **0x117160 (MaterialParam 4 MiB copy per frame).** The copy is `memcpy(rbx, DAT_02ed2108, DAT_02ed2100)` at 0x11788a;
  the return site 0x11788f has the destination in rbx and the size global in r12 (size = u32 at r12). The byte
  signature of 0x11788f is not unique (`eboot_re.py sig` reports two matches within 80 bytes), so no Site yet.
  Next step: a longer signature that includes the `lea rcx,[rip+...]` of 0x117876, checked with `sigfind`.
- **UpdateGPUData 0x118100 tail.** Not hooked. The instance writers it calls are covered per writer (0x121ed0 Entry,
  0x123410 and 0x11c710 through the chunk list). 0x18aa20 takes no arguments (the object is a global) and writes its
  transform block only when `*(p+0xa8)` is set: not hooked.
- **View writers 0xbe1f0 (SetViewParams) and 0xcedf0.** Their blocks come from FUN_0008c120 (a different allocator from
  0x8c0d0), not from the chunk filter. Next step: hook 0x8c120 with the same chunk list, after reading its caller list.
- **Particle destination.** `NATIVE_PARTICLE_SHIFT` announces the destination record from `[rbp-0x30]`, which is the
  memcpy target (dest = param_2 of 0x101720). The memcpy size is assumed to be the stride (`rdx` before the call); not
  verified in the disassembly.

## 5. Snippets for files this agent does not own

`gpu/sotc/hook_handlers.cpp`: already present in the main tree (`SotcNativeHooks::Register();` in
`RegisterAllHandlers()`, declaration in the namespace block). No change.

`gpu/shadps4/core/libraries/videoout/driver.cpp`: `SotcNativeHooks::Report(report, seconds);` is declared at line 60
(`namespace SotcNativeHooks { void Report(std::string& out, double seconds); }`) but not called. Add it right after the
existing `SotcNative::Report(report, seconds);` call (line 2023):
```cpp
        SotcNative::Report(report, seconds);      // sotc: SOTC_NATIVE
        SotcNativeHooks::Report(report, seconds); // sotc: SOTC_NATIVE write hooks (native_hooks.cpp)
```

`native.h` / `native.cpp`: `NoteDescriptor` is declared in `native.h` and defined in the main tree's `native.cpp`;
the descriptor hooks call it with `(desc, base, size_bytes, format_word3)`. The `size` of the buffer encoders is
`stride * count` (bytes); the API agent should treat `size` as bytes.

## 6. Open points

1. Register checks of the Entry/Wrap hooks (rdi, rsi, rcx, rdx, r8, r9 at the entry) follow the decompilation of the
   callees; the first game run in mode 2 (once the user authorizes it) should confirm the DCB range matches the CP
   submit (`S[0]` base and `S[2]` cursor), the CCB range `[+0x10, +0x20)`, and the chunk sizes.
2. `NATIVE_DCB_SUBMIT` also announces the CCB (`+0x40`) and ACE (`+0x80`) ranges of the same context. These come from
   the 0x8636e0 field use, not from a separate hook; a wrong offset would show as oversized `ccb` bytes in the report.
3. The particle destination size (section 4) and the 0x117160 gap are the two sites with an assumption left open.