# 01 - Render backend interface: is there a vtable between BPE and GNM?

Read-only static analysis of `out/eboot.elf`. No build, no game run, no edits outside this file.
Main source: Ghidra pseudocode in `out/re/decomp/<4 hex>/<8 hex>.c`. Disassembly (`tools/re/eboot_re.py`)
used only where noted.

Labels: **observed** (code/data/string at the address given), **inferred**, **unknown**.

## 1. Answer

- **Observed: no vtable or function table holds any address of the 0x86a000-0x86ffff wrapper family.**
  Scanning the whole file for the 8-byte, 4-byte and RIP-relative forms of all 175 function starts in
  that range found no data slot and no code reference. The 0x86 wrappers are reached only by direct
  `call` instructions, or not at all.
- **Observed: the premise "draw wrappers have no direct callers" (report 06) is wrong for several of them.**
  `0x86b3d0` has 3 direct callers (`0xdb620`, `0x1a6d20`, `0x1a7290`); `0x86b9e0` has 64; `0x86ad40`
  has 20; `0x86c5f0` has 36. About 15 wrappers have zero direct callers (section 3).
- **Observed: the wrappers are not an engine API. They are a thin PM4 command-buffer writer.** Every
  wrapper takes a cursor object (`+0x08` end, `+0x10` write pointer, `+0x18` refill callback, `+0x20`
  callback user data, `+0x28` flag bit) and raw GNM arguments (guest pointers, shader-register blocks,
  index/dispatch dwords). It either calls a `sceGnm*` import with the cursor, or writes PM4 headers
  itself (`0xC0044700` EOP, `0xC0053C00` WAIT_REG_MEM, `0xC0055000`, `0xC0016900`, ...). No V#/T#
  abstraction and no engine object appears in the wrapper layer.
- **Observed: the engine-level object is above that layer.** BPE has real backend classes
  (`CRenderBackend`, `CRenderContext`, `CComputeContext`) with source paths under
  `BPE\Source\Renderer\Backend\PS4\` (strings `0xf9ef3b`, `0xf9f1ff`, `0xf9f24c`). The decisions
  (which material shaders to bind, which index buffer, virtual geometry and XPR visibility) are made in
  engine code that reads engine state (for example `Material_BindGraphicsShaders_B` `0x85fc10`
  reads fields at `ctx+0x2e8+0xc260` and friends) and then calls the wrappers.
- **Step-4 verdict: BPE abstraction above, GNM-shaped PM4 writer below.** The engine decides *what* to
  bind with its own objects; the layer it calls to *emit* state is a raw PM4/GNM writer. The
  sceGnm-import subset (about 27 wrappers) is the only seam to host functions. The rest write PM4 bytes
  directly, so an import-level backend would miss them. The practical seam is the PM4 stream itself,
  which the port already decodes (`gpu/`, shadPS4 video core).

## 2. Vtable search (step 1)

| Check | Result | Label |
|---|---|---|
| 8-byte absolute pointers to each of the 175 function starts in `0x86a000-0x870000`, anywhere in the file | none | observed |
| 4-byte absolute values (`imm32`) of `0x86b3d0`, `0x86b9e0`, `0x86c050`, `0x86c170`, `0x86b470`, `0x86ad40` | none | observed |
| RIP-relative references into `0x86a000-0x870000` (`refs`) | none | observed |
| Function-pointer runs in data (`0x1594050`-`0x1594b30` and beyond) | exist, but none point into the 0x86 range | observed |
| Relative (table-base-relative) jump or vtable tables | not checked | unknown |

Ghidra-visible function-pointer use in the wrapper layer is only the cursor refill callback at
`[obj+0x18]`, called as `(**(code **)(param_1+0x18))(param_1, n_dwords, *(param_1+0x20))`
(`out/re/decomp/0086/0086b3d0.c`, `0086b9e0.c`, `0086c170.c`). That is one callback per cursor, not a
method table.

### 2.1 The CRenderContext constructor (step 2)

- **Observed:** `FUN_000917c0` (`out/re/decomp/0009/000917c0.c`, called from `FUN_00090470` =
  `PerFrameRenderContexts::RequestNewRenderContext`) builds `CRenderContext(size_t, size_t,
  IAllocator*, IAllocator*)` (string `0xf9f24c`).
  - It allocates a 0xcbb8-byte object (`FUN_000170d0`, `memset`) and stores it at `*this`. This object
    is the command buffer, and `FUN_00862690` initialises it with two chunk pools (`FUN_00865ea0`,
    callbacks `LAB_008627f0` and `FUN_00862800`). So `[ctx+0x0]` = cursor object, and the wrappers
    receive `*this`, not `this`.
  - It stores `PTR_DAT_01604408` at `this+0x20`. The value at `0x1604408` is `0x1604370`, a pointer to
    a data block, not a function. Entries 0-4 are data pointers, not methods. **Observed: this is not a
    function vtable.** Its meaning is **unknown**.
  - It stores two callbacks, `LAB_00091790` at `obj+0xc8` and `LAB_000917a0` at `obj+0x2e8`. Both
    `return 0` (`0009/00091790.c`, `0009/000917a0.c`). Their role is **unknown**.
- **Observed:** `FUN_000db620` (virtual geometry draw, `out/re/decomp/000d/000db620.c`) calls
  `FUN_0086ad40(*param_1, ...)`, `FUN_0085fc10(*param_1+0x2e8)`, `FUN_0086b3d0(*param_1, ...)` and
  `FUN_0086c000(*param_1, 1)`. `*param_1` is the CRenderContext. So the cursor object is
  `*(CRenderContext)`, and the material state sits at `CRenderContext+0x2e8`.

## 3. Wrapper table (step 3)

Classification uses the Ghidra decomp where read, the import name, and the PM4 header. "Callers" =
direct call sites from the eboot index. Label: **observed** for names and callers; the class column is
**inferred** unless it says "import".

### 3.1 Shader binds

| Address | Name (names.txt / import) | Callers | Class |
|---|---|---|---|
| `0x86c050` | GnmShader_SetPs350 (`sceGnmSetPsShader350`) | 2 | shader bind (import) |
| `0x86c110` | GnmShader_UpdatePs350 (`sceGnmUpdatePsShader350`) | 2 | shader bind (import) |
| `0x86c170` | GnmShader_SetVs (`sceGnmSetVsShader`), decomp read | 2 (`0x85fc10`, `0x85f410`) | shader bind (import) |
| `0x86c250` | GnmShader_UpdateVs (`sceGnmUpdateVsShader`) | 2 | shader bind (import) |
| `0x86c2c0` | SetEsShader | 1 | shader bind (import) |
| `0x86c330` / `0x86c460` | SetHsShader / UpdateHsShader | 1 / 1 | shader bind (import) |
| `0x86c760` | SetLsShader | 1 | shader bind (import) |
| `0x86c7d0` | GnmShader_SetCsWithModifier | 1 (`0x85f220`) | compute shader bind (import) |
| `0x86cb00` / `0x86cb60` | SetGsShader / UpdateGsShader | 1 / 1 | shader bind (import) |
| `0x86c0b0`, `0x86c1e0` | SetEmbeddedPs / SetEmbeddedVs (import) | 0 / 0 | shader bind (import), no direct caller |

### 3.2 Draws

| Address | Import | Callers | Class |
|---|---|---|---|
| `0x86a9d0`, `0x86aab0`, `0x86adc0`, `0x86ae30` | DrawIndex | 0 each | draw (import), no direct caller |
| `0x86abb0`, `0x86ac10` | DrawIndexAuto | 0 / 2 | draw (import) |
| `0x86ac80`, `0x86ace0` | DrawOpaqueAuto | 0 / 0 | draw (import), no direct caller |
| `0x86aeb0`, `0x86af30` | DrawIndexMultiInstanced | 0 / 0 | draw (import), no direct caller |
| `0x86afd0`, `0x86b040` | DrawIndexOffset | 0 / 9 | draw (import) |
| `0x86b0c0`, `0x86b150` | DrawIndirect | 0 / 1 (`0x10add0`) | draw indirect (import) |
| `0x86b1f0`, `0x86b290` | DrawIndirectCountMulti | 0 / 0 | draw indirect count (import), no direct caller |
| `0x86b340` | DrawIndexIndirect | 0 | draw indirect (import), no direct caller |
| `0x86b3d0` | DrawIndexIndirect, decomp read | 3 (`0xdb620`, `0x1a6d20`, `0x1a7290`) | draw indirect (import) |
| `0x86b470`, `0x86b510` | DrawIndexIndirectCountMulti | 0 / 0 | draw indirect count (import), no direct caller |

`0x86b3d0` (decomp): args `(cmd, esi, edx, ecx, r8d, r9d, stack)`; packs the stack argument as
`param_6 << 0x1d | (flag & 1)`; advances the cursor by `0x24` bytes (9 dwords) on success.

### 3.3 Dispatch

| Address | Import | Callers | Class |
|---|---|---|---|
| `0x86b9e0` | DispatchDirect, decomp read | 64 | compute dispatch (import) |
| `0x86b970` | DispatchDirect (name `CmdBuf::DispatchDirect`) | 0 | compute dispatch (import), no direct caller |
| `0x86ba60`, `0x86bac0` | DispatchIndirect | 0 / 16 | compute dispatch indirect (import) |

`0x86b9e0` (decomp): `sceGnmDispatchDirect(cursor, 9, edx, ecx, r8d, (flag & 1) + r9 * 8)`. The
`param_5 * 8` term is a dword-address scale, so the wrapper converts a raw argument into the PM4 field.

### 3.4 Barriers, labels, EOP, flip, markers

| Address | Evidence | Callers | Class |
|---|---|---|---|
| `0x86eb60` | raw EOP header `0xC0044700` (`EVENT_WRITE_EOP`) | 18 | label / EOP (raw PM4) |
| `0x86edc0` | raw WAIT_REG_MEM header `0xC0053C00` | 41 | wait on label (raw PM4) |
| `0x86eec0` | raw header `0xC0053C00`, `ecx` shift | 9 | wait on label, variant (raw PM4, **inferred**) |
| `0x86f700` | import `InsertWaitFlipDone` | 1 | flip wait (import) |
| `0x86e670`, `0x86f2a0`, `0x86f3a0`, `0x86e6f0`, `0x86f320` | import SetMarker / PushMarker / PopMarker / SetColorMarker / PushColorMarker | 1-3 | debug marker (import) |

### 3.5 Index buffer, VGT, context and config registers (raw PM4)

| Address | Evidence | Callers | Class |
|---|---|---|---|
| `0x86ad40` | raw 4-dword packet, header `2*flag - 0x3ffdef00`, body from `rdx` (64-bit address and size) | 20 | index-buffer bind (**inferred**, from the call order in `0xdb620`) |
| `0x86c900`, `0x86c950`, `0x86c9c0`, `0x86ca20`, `0x86ca90` | import `ResetVgtControl` / `SetVgtControl` | 0 / 0 / 2 / 0 / 0 | VGT state (import) |
| `0x86bed0`, `0x86bf50`, `0x86bfb0`, `0x86c000` | raw headers `0xC0002A00`, `0xC0002F00` (context-register blocks) | 8 / 5 / 3 / 11 | context / config registers (raw PM4, **inferred** from headers) |

### 3.6 User data and generic PM4 writers (raw)

| Address | Evidence | Callers | Class |
|---|---|---|---|
| `0x86a700` | raw header `0xC0055000`, bit-packed fields | 26 | packet writer, **unknown** opcode use |
| `0x86a7f0`, `0x86a880`, `0x86a900` | raw writes, one `sub_882ec0` call in `0x86a900` | 3 / 1 / 6 | packet writer, unknown |
| `0x86d160` | raw header `0xC0016900` | 35 | context or SH register writer (**inferred** from header) |
| `0x86d210` | raw writer | 18 | packet writer, unknown |
| `0x86d4c0` | raw `0xC0001000`-style, bit-packed | 45 | SH/user-data register writer (**inferred**) |
| `0x86d5e0` | raw headers `0xC0001000`, `0xC0007600` | 15 | user-data / constants writer (**inferred**) |
| `0x86c5f0` | raw writer | 36 | packet writer, unknown |
| `0x86ced0`, `0x86cf40` | raw writers | 6 / 3 | packet writer, unknown |
| `0x86d550`, `0x86d730`, `0x86d7b0`, `0x86d890`, `0x86d950` | raw writers | 1-5 | packet writers, unknown |

These are mostly bit-packed PM4 headers and register offsets. Ghidra does not name them, and no
vtable points to them. Their exact classes are **unknown** without a per-packet decode.

### 3.7 Wrappers with no direct caller

About 15 wrappers have zero direct callers and are not in any data table: `0x86a9d0`, `0x86aab0`,
`0x86abb0`, `0x86ac80`, `0x86ace0`, `0x86adc0`, `0x86ae30`, `0x86aeb0`, `0x86af30`, `0x86b0c0`,
`0x86b1f0`, `0x86b290`, `0x86b340`, `0x86b470`, `0x86b510`, `0x86b970`, `0x86ba60`, `0x86c0b0`,
`0x86c1e0`, `0x86c900`, `0x86ca20`, `0x86ca90`. Whether they are dead code in this build, reached by
a relative-offset switch table, or reached through a pointer built at run time, is **unknown**.

## 4. Who builds and uses the objects (step 2, continued)

- **Observed:** `Material_BindGraphicsShaders_B` (`out/re/decomp/0085/0085fc10.c`, 3994 bytes) takes
  `param_1 = CRenderContext+0x2e8` and reads state at fixed offsets (`+0xc260` flags with bit `0x20`,
  `+0xc0b8`, `+0xc0c0` shader-hash buffers, `+0xc090`, `+0xc170`, `+0xc24c`, `+0xc5e4`). It is called from
  at least 15 engine functions: the draw paths `0x93690`, `0x94d00`, `0x94dd0`, `0x94ec0`, `0xdb620`,
  `0x10add0`, `0x133550`-`0x133e20`, `0x1a7102` (XPR consumer), `0x1a8c60`. This is engine-level
  state with engine-level cache keys. Its bind calls go to the raw wrappers `0x86c170` and `0x86c050` etc.
- **Observed:** `FUN_000db620` (virtual geometry, per object) and the XPR consumer `0x1a6d20` (report 01)
  are the two draw recorders that use `0x86b3d0`. The XPR consumer reads visibility bits, then calls
  the wrapper once or twice per visible object.
- **Observed:** the shader-bind wrappers take a guest pointer to a shader-register block
  (`sceGnmSetVsShader(cursor, 0x1d, ptr, modifier)`, `0x86c170`). The engine passes guest pointers
  and raw dwords, not engine handles.
- **Unknown:** the constructor of the cursor object's callback vtable (who stores `0x8627f0` or
  `0x862800` at `+0x18`). It is set by `FUN_00865ea0`, which was not decoded.

## 5. Interpretation

- A native Vulkan backend that replaces the GNM implementation of "the interface" would not replace one
  table. The engine's call sites are direct calls to about 175 functions, and about 15 of them are never
  called directly.
- The only exact seam is the `sceGnm*` import set (about 27 wrappers, all in the table in 3.1-3.3). It
  does not cover raw PM4 writers (`0x86ad40`, `0x86eb60`, `0x86edc0`, `0x86d4c0`, ...). A backend at
  that seam would have to see the packet stream anyway.
- Hence the practical seam is the PM4 stream written into the cursor object. The port already consumes
  that stream (`gpu/`, shadPS4 video core), which matches the existing architecture in
  `CLAUDE.md`. The existing `bbgpu_resolve()` path binds the import NIDs, which is **inferred** from
  `CLAUDE.md` and `src/import_names.inc`, not checked in code here.

## 6. Corrections and open items

- Report 06 says the draw wrappers have no direct callers. **Observed: false for 0x86b3d0, 0x86b9e0,
  0x86ad40, 0x86c5f0 and others** (section 3).
- Report 06 says "function table". **Observed: no table references the wrappers** (section 2).
- Open: relative-offset switch tables and run-time built pointers (not searched); the callback vtable
  of the cursor object (`FUN_00865ea0`); the meaning of `CRenderContext+0x20` (`0x1604370` block) and
  `+0xc8`/`+0x2e8` (`return 0` callbacks); the per-packet class of the 0x86a7xx-0x86d9xx raw writers.
- Index limitation: `eboot_re.py refs` covers RIP-relative operands only, so absolute tables need the
  separate scans described in section 2.

## 7. Files read

- `docs/re-stutters/reports/06-render-frame.md`, `reports/07-first-use.md`,
  `reports2/14-draw-sources.md`, `reports/01-xpr-cull.md`, `tools/re/names.txt`.
- Ghidra: `out/re/decomp/0086/0086b3d0.c`, `0086/0086b9e0.c`, `0086/0086c170.c`, `0086/00862690.c`,
  `0009/000917c0.c`, `0009/00091790.c`, `0009/000917a0.c`, `000d/000db620.c`, `0085/0085fc10.c`.
- Disassembly (`eboot_re.py dis/callers/callees/refs`) for the call-site and caller counts.
