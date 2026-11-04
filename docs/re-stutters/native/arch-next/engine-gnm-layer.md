# Engine GNM layer: how the game emits PM4 (topic C2)

Read-only static analysis of the EU 1.01 eboot. Addresses are eboot VAs. Sources: the Ghidra export
`out/re/decomp/<hi16>/<addr>.c` (first line lists the callers, which is incomplete), the eboot image
(`out/eboot.elf`, read with `tools/re/eboot_re.py` helpers and capstone), and the port source. Scratch scripts
live under the agent scratchpad; nothing under `src/`, `gpu/` or `scripts/` was changed.

Labels: **proven** (decomp or bytes read, address given), **inferred** (reasoning, not run), **unknown**.

## 0. Corrections to the premise

1. **PM4 is encoded in two places, not one.**
   - The game encodes PM4 itself. Its inline emitters (eboot `0x869350..0x86fd60`, plus families at
     `0x85ecd0`, `0x883xxx..0x885xxx`) write 3-dword and larger packets straight into the command
     buffer cursor. **proven**
   - The port encodes PM4 for the `sceGnm*` imports the game calls for draws, dispatches, shader binds, VGT, markers,
     flip waits and submits. `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:744` (`sceGnmDrawIndexAuto`) calls
     `WritePacket<PM4ItOpcode::DrawIndexAuto>`; `sceGnmSetVsShader` (`:2100`) calls `PM4CmdSetData::SetShReg`. **proven**
   - The port decodes all of it in `gpu/shadps4/video_core/amdgpu/liverpool.cpp` (`Process` at `:3716`, decode cases
     at `:4388` SetContextReg, `:4461` SetShReg, `:4555` DrawIndexAuto, `:4312` Nop with payload types). **proven**
2. **"Draw and state calls go straight to the port" holds only for the imports.** Draws, dispatches and shader
   binds are `sceGnm*` calls, resolved by NID to the port (`src/runtime.c:300-330`, fallback `bbgpu_resolve`,
   `gpu/shim/bbgpu.cpp:508`). State writes do not go to the port: they are game code, and the port sees them only
   as bytes in the command buffer. **proven**
3. **The import calls have a size contract.** Each emitter reserves N dwords first, calls the reservation
   callback if the space is short, calls the import with `cb` as first argument, and on return 0 advances the
   cursor by a fixed size (table C). A replacement must write exactly that many dwords, or return a non-zero value
   and handle the cursor itself. **proven** (decomp of `0x86abb0` ... `0x86c9c0`).

## 1. The command buffer object

### 1.1 Fields (proven unless marked)

The object passed as first argument to every emitter is the graphics context (called `cb` here). Field offsets:

| Offset | Use | Evidence |
|---|---|---|
| `+0x08` | end of the current buffer (`end - cursor` in dwords is the free space) | every emitter, e.g. `0x869370`, `0x86a900` |
| `+0x90` (of the render context, not of the cb) | pointer to the cb for callers that hold the render context: `sub_85eca0`, `sub_85ecc0`, `0x85ecd0` load `[rdi+0x90]` first | `0x85eca0`, `0x85ecc0`, `0x85ecd0` (disassembly); `scripts/hooks.d/native_writes.py:402-404` |
| `+0x10` | **write cursor** (the next packet is stored here; advanced by the emitter) | `0x869370` (`puVar1 = *(param_1+0x10)`, `*(param_1+0x10) = puVar1+3`) |
| `+0x18` | reservation callback `(cb, dwords_needed, arg)`, returns a bool; 0 means "no space" and the emitter returns without writing | `0x86a900` and all emitters |
| `+0x20` | argument of the callback | same |
| `+0x28` | flags; bit 0 is the predication bit OR-ed into draw and dispatch initiators (`param << 0x1d \| cb+0x28 & 1`) | `0x86ac10`, `0x86b040`, `0x86b970`; port side `gnmdriver.cpp:744` tests `flags & 0x1ffffffe` |
| `+0x11e` (16-bit) | bit 1: dispatch-draw (second queue) mode | submit wrapper `0x863af0` |
| `+0x50` / `+0x128` | second stream (ACE-like) cursor / start | `0x863af0` (`param_6[10]`, `param_6[0x25]`) |
| `+0x90` / `+0x130` | third stream (CCB) cursor / start | `0x863af0` (`param_6[0x12]`, `param_6[0x26]`) |
| `+0xc0` | queue object for the compute submit `FUN_0085d380` (DingDong) | `0x863af0` (`param_6[0x18]`) |
| `+0x120` | start of the main DCB | `0x863af0` (`param_6[0x24]`), DCB size = `+0x10 - +0x120` |
| `+0x178` | chunk table, 6 dwords per entry, count (uint) at `+0x2e4` | `0x863af0` (`param_6+0x2f`, `*(uint*)(param_6+0x2e4)`) |

The names of the three streams are **inferred** from the arithmetic in `0x863af0`; the submit wrapper carries the
Gnmx source path `...\gnmx\lwgfxcontext.cpp`, line 0x2c8, `internalSubmitAndFlip` (string in the decomp of
`0x863af0`), so the object is the Gnmx `lwgfxcontext`.

### 1.2 Begin

- `0x90470` (509 bytes; callers `0x90430`, `0x90860`) builds a render context: it allocates a 0x238-byte object
  (`FUN_00017070(0x238)`), sets a size constant `0x500000` (5 MiB, value read from the decomp; its role as the DCB
  reservation is **inferred**), and calls `FUN_000917c0`. `*plVar4` is the cb that the emitters receive. **proven**
  for the object; **inferred** for the size role.
- The begin sequence in `0x90470` (proven call list): `0x869290` (import `sceGnmDrawInitDefaultHardwareState350`,
  asks for 0x100 dwords, `0x869290` line 17-20), `0x86f1d0` (EVENT_WRITE and ACQUIRE_MEM, 2 static callers),
  `0x86ded0` (PA_CL_CLIP_CNTL), `0x86c840` (`SET_CONTEXT_REG 0x20`). `0x8692f0` (ContextState400 import) has no
  static caller. **proven**
- `0x90470` also manages a chunk vector (the list object `param_1`, with begin, cursor, end at `+0x10/+0x18/+0x20`)
  that holds the chunks of the frame. **proven**

### 1.3 Chaining

- No live code emits an INDIRECT_BUFFER packet. `0x86fa00` and `0x86fa80` (opcode 0x3F) have no reference found.
  **proven** (method of section 2.1).
- Chaining happens at submit time instead: `0x863af0` walks the chunk table at `cb+0x178`, builds the arrays
  `local_b8` (addresses), `local_1f8` (sizes), `local_1b8`, `local_278` for the main DCB plus up to N chunks, and
  calls one import with count `N+1`. **proven** (decomp of `0x863af0`).

### 1.4 End and submit

- `0x91f80` (10 bytes, called only from `0xd07c0` = KickoffSubmitJob, per the hook notes) calls `0x8636e0`, which
  calls `import_sceGnmSubmitCommandBuffersForWorkload`. **proven** (header and decomp). The hook
  `NATIVE_DCB_SUBMIT` is at `0x91f80` and its flip variant at `0x91f97` (`scripts/hooks.d/native_writes.py:8-14,109-127`).
- `0x863310` (971 bytes) calls `import_sceGnmSubmitCommandBuffers` (no workload). **proven**
- `0x863af0` (1237 bytes) calls `import_sceGnmSubmitAndFlipCommandBuffers` (or the ForWorkload variant). Its callers
  are the eight wrappers `0x863aa0, 0x863fd0, 0x864020, 0x864070, 0x8640d0, 0x864130, 0x864190, 0x8641e0`. **proven**
- Second queue: when `cb+0x11e` bit 1 is set, `0x863af0` calls `0x85d380` (DingDong import on the compute queue) and
  spins while it returns `-0x7fffffff` (queue full, string "ComputeQueue for dispatch draw is full"). **proven**
- Flip: `0x91f50` (37 bytes, called by `0x118470` = Frame::InsertWaitFlipDone) calls `0x86f700`
  (`import_sceGnmInsertWaitFlipDone`). **proven**
- The port's submit imports are `gnmdriver.cpp:2525` (`sceGnmSubmitAndFlipCommandBuffers`) and `:2628`
  (`sceGnmSubmitCommandBuffers`). **proven** (names); their bodies were not read here.

### 1.5 Count per frame

- Static: one DCB submit per `0x91f80` call, by the hook note. **proven** (note), **unknown** (count).
- No per-frame submit count in the logs. `docs/re-stutters/native/allocators.md:118` says "about 33 submits per frame";
  that number is not verified here. **unknown**
- The logs show CP work per frame: `SOTC CP ops: func 257/frame` (`user/gcn-run1/base.log:450`) and
  `SOTC DRAW SPLIT per frame (167 frames) ... cp cpu 12.7 ms ... recorder cpu 29.7` (`base.log:459`). **proven** (log
  lines). They do not split by queue.
- Measure with the existing hook: count `NATIVE_DCB_SUBMIT` per frame and the emitter families of section 2.

## 2. Emitter catalogue

### 2.1 Method and coverage

- Candidates: every function in `0x869000..0x886000` whose decomp has a PM4 header literal `0xC0xxxxxx` or a
  `sceGnm*` import call. **410** candidates (195 in `0x869350..0x86fd60`, 123 in `0x86fdc0..0x87bfff`, 92 in
  `0x87c000..0x886000`).
- Direct references found by capstone over all eh_frame functions (call and jmp with a relative target): **133**
  candidates are referenced; **277** are not.
- Of the 277, the following were also checked: 8-byte absolute pointers in the data segments (0 found), RIP-relative
  `lea` operands and `mov imm` (1 found: the import stub `0x882cf0`, referenced from `0xc5913`). **proven** for
  these checks; not checked: 4-byte relative tables and computed calls. So "no reference" means **no reference
  found**, not proof of dead code.
- The 123 candidates in `0x86fdc0..0x87bfff` have no reference at all. They write a fixed default-state pattern
  (NOP plus UCONFIG). They are very likely unused in this build. **inferred** (no reference found).
- Constant PM4 fragments: the read-only range `0x128f9f0..0x128fed0` (about 1.2 KB, segment flags R+X) holds
  packet templates (`ACQUIRE_MEM` `0xC0055800` with body `0x80c47fc0`, a DMA_DATA template `0xC0055000` with body
  `0xc4004000`, UCONFIG `0x200` with `0xe0000000`, then NOP words `0x68750007`). **proven** (bytes). It is referenced
  RIP-relative by 77 functions, among them the live emitters `0x86a290` and `0x86cc20`. **proven**

### 2.2 Table A: state writers (live, `0x869350..0x86fd60`)

Register numbers are context dword indices (byte = index x 4, base `0xA000`); the packet header is in brackets.
Names come from `docs/re-stutters/native/re-pipelines/render-states.md` where that doc has them. The "Call sites"
column is the number of decompiled call sites (`out/re/decomp`, self-calls excluded). "none" means no machine-level
call, jump or pointer reference was found (section 2.1); such an emitter is not in the live set.

| Emitter | Packet, register(s) | Call sites | Notes |
|---|---|---|---|
| `0x869990` | [SET_CONTEXT_REG] `0x8E` CB_TARGET_MASK | 4 | |
| `0x869a50` | [SET_CONTEXT_REG] `0x1E0+i` CB_BLEND0_CONTROL[i] | 11 | `i` in arg 2 |
| `0x869bb0` | [SET_CONTEXT_REG x2] `0x10C,0x10D` STENCILREFMASK pair | 6 | |
| `0x869ce0` | [SET_CONTEXT_REG] `0x202` CB_COLOR_CONTROL | 6 | mode and rop3 |
| `0x869d60` | [SET_CONTEXT_REG] `0x200` DB_DEPTH_CONTROL, mask `0xC07007FF` | 2 | |
| `0x869f50` | [SET_CONTEXT_REG] `0x0B` DB_DEPTH_CLEAR | 11 | |
| `0x869dc0` | [SET_CONTEXT_REG] `0x10B` (name not decoded) | 1 | |
| `0x86a010` | [SET_CONTEXT_REG] `0x8D` (pair of 9-bit fields) | 1 | name not decoded |
| `0x86a090` | [SET_CONTEXT_REG x4] `0x2FA` | 1 | name not decoded |
| `0x86a120` | [SET_CONTEXT_REG] `0x80` PA_SC_WINDOW_SCISSOR_TL | 1 | |
| `0x86a1a0` | [SET_CONTEXT_REG x14] `0x318+15n` CB_COLORn block (or [SET_CONTEXT_REG] INFO=0), then one NOP word with a payload dword | 2 | descriptor in pass-targets.md section 4 |
| `0x86a290` | [SET_CONTEXT_REG] x5 (`0x10`..`0x15`, DB_Z_INFO to DB_STENCIL_WRITE_BASE), [SET_CONTEXT_REG] `0x02`, `0x05`, `0x2AF`, `0x0F`, then a NOP word with one payload dword | 4 | depth block, 12 dwords; pass-targets.md section 5 |
| `0x86a7f0` | DMA_DATA to register `0x3022C` | 9 | (see Table D) |
| `0x86b6f0` | [SET_CONTEXT_REG] `0x2DD` | 1 | |
| `0x86b750` | [opcode 0x8C] packed 10-bit fields, `0x420` | 1 | not in port enum (`pm4_opcodes.h`) |
| `0x86bc00` | [SET_CONTEXT_REG block] from `0x191` (SPI_PS_INPUT_CNTL, **inferred**), `n<=32` | 1 | **switch: not hookable** (5.2) |
| `0x86be10` | [SET_CONTEXT_REG] `0x03` DB_RENDER_OVERRIDE | 4 | |
| `0x86bed0` | [INDEX_TYPE] `0x2A` | 8 | draw setup |
| `0x86bf50` | [INDEX_BASE] `0x26` | 5 | draw setup |
| `0x86bfb0` | [INDEX_BUFFER_SIZE] `0x13` | 3 | draw setup |
| `0x86c000` | [NUM_INSTANCES] `0x2F` | 18 | draw setup |
| `0x86c650` / `0x86c6c0` | [SET_CONTEXT_REG] `0x290`, `0x291` | 2 / 1 | |
| `0x86c840` | [SET_CONTEXT_REG] `0x20` | 1 | used in begin |
| `0x86cbc0` | [SET_CONTEXT_REG] `0x2AB` | 1 | |
| `0x86cc20` | [SET_CONTEXT_REG x4] `0x2D7` (x4 dwords), `0x298` (x3), `0x2CE`, `0x2AC` | 1 | viewport-like block; `sub_85c9a0` is a jump to it |
| `0x86ced0` | [EVENT_WRITE] event type from `DAT_012901D0` | 7 | section 2.5 |
| `0x86d7b0` | [SET_CONTEXT_REG x2 + x6] | 1 | |
| `0x86d890` | [SET_CONTEXT_REG x2] | 7 | |
| `0x86d950`, `0x86dd20` | [SET_CONTEXT_REG] `0x02` DB_RENDER_CONTROL / `0x00` | 1 / 4 | |
| `0x86db40` | [SET_CONTEXT_REG] `0x206` PA_CL_VTE_CNTL | 1 | |
| `0x86ded0` | [SET_CONTEXT_REG] `0x204` PA_CL_CLIP_CNTL | 1 | begin sequence |
| `0x86e1a0` | [SET_CONTEXT_REG] `0x205` PA_SU_SC_MODE_CNTL | 2 | |
| `0x86e3a0` | [SET_CONTEXT_REG] table `DAT_0128FEE0` value (`0x2DE`) | 4 | the table is now decoded (4.4) |
| `0x86e410`, `0x86e480`, `0x86e5e0` | [SET_CONTEXT_REG x2] | 1 each | |
| `0x86e4f0` | [SET_CONTEXT_REG] | 7 | |
| `0x86e560`, `0x86e800` | [SET_CONTEXT_REG] / [CLEAR_STATE, opcode 0x12] | 1 each | `0x86e800` from `0x11d3d0` |
| `0x86f0c0` / `0x86f170` | [SET_CONTEXT_REG] | 1 each | |

Per-block masks in several of these emitters (for example `0x869990` with `& 0xffff1fff` on the blend value in
`0x869a50`) are in the decomp; render-states.md section 1 gives the field layout.

### 2.3 Table B: user-data and shader-register writers (SH)

| Emitter | What it writes | Call sites | Notes |
|---|---|---|---|
| `0x86d4c0` | [SET_SH_REG x2] a raw 64-bit value at `stage_base(param_2) + param_3 - 0x2C00` | 67 (45 in `0x85fc10`) | most likely the shader code address pair of a stage (inferred). **hookable** (5.2) |
| `0x86d5e0` | [NOP tag `0x6875000D`] + [SET_SH_REG x n] `n<=16` from a caller array | 37 | **switch: not hookable** (5.2) |
| `0x86d550` | [SET_SH_REG x1] (`0xC0017600`) | 5 | called from `0x862510` and `0x862c50` |
| `0x86d160` | [SET_CONTEXT_REG] `0x2D5` = a stage usage id (`0`, `0x45`, `0xAD`, `0xB0`, `0x145`, `0x1AD`, `0x200`) | 43 | called from the bind selector `0xF8AA0`; the register name is not decoded |
| `0x86d210` | allocates `n` dwords aligned to `1<<param_3` inside the cb, NOP tag `0x68750000 \| (log2 align)<<12` (inferred), returns a pointer | 21 | embedded data: the caller writes the words (4.3) |
| `0x884240` | [SET_SH_REG]/[NOP] packet writer behind `0x85ECD0` (user-data bind, 35 bytes, tail jump) | 19 | **indirect jump: not hookable** (5.2); the port already wraps `0x85ECD0` and reads the cursor (`scripts/hooks.d/native_writes.py:399-407`) |
| `0x8841d0`, `0x884360`, `0x883d60`, `0x883de0` | [SET_SH_REG] writers, probably the compute stage (`0x2E40` base, inferred) | 16 / 3 / 2 / 4 | `0x8841d0` is the tail target of `sub_85ecc0`; section 2.6 |

### 2.4 Table C: draw, dispatch, shader and VGT imports (the port already implements these)

The emitter reserves the dword count given in column 3, calls the import with `cb` first, and on return 0 advances the
cursor by the byte count given in column 3 after the slash. The port's handler is the NID of column 2 resolved by `runtime_resolve`.

| Emitter | Import (NID resolved to the port) | Dwords / bytes | Call sites | Port side |
|---|---|---|---|---|
| `0x86abb0` | `sceGnmDrawIndexAuto` | 7 / 0x1C | none | `gnmdriver.cpp:744` (requires size 7) |
| `0x86ac10` | `sceGnmDrawIndexAuto` (with predicate) | 7 / 0x1C | 2 | same |
| `0x86ac80`, `0x86ace0` | `sceGnmDrawOpaqueAuto` | 7 / 0x1C | none | |
| `0x86adc0`, `0x86ae30` | `sceGnmDrawIndex` | 10 / 0x28 | none | |
| `0x86afd0`, `0x86b040` | `sceGnmDrawIndexOffset` | 9 / 0x24 | 10 | `gnmdriver.cpp:860` |
| `0x86aeb0`, `0x86af30` | `sceGnmDrawIndexMultiInstanced` | 13 / 0x34 | none | |
| `0x86b0c0`, `0x86b150` | `sceGnmDrawIndirect` | 9 / 0x24 | 1 | |
| `0x86b340`, `0x86b3d0` | `sceGnmDrawIndexIndirect` | 9 / 0x24 | 4 | `gnmdriver.cpp` `sceGnmDrawIndexIndirect` (writes the header with a predicate) |
| `0x86b1f0`, `0x86b290`, `0x86b470`, `0x86b510` | `sceGnmDrawIndirectCountMulti`, `sceGnmDrawIndexIndirectCountMulti` | 16 / 0x40 | none | |
| `0x86b970`, `0x86b9e0` | `sceGnmDispatchDirect` | 9 / 0x24 | 83 | `gnmdriver.cpp:640` |
| `0x86ba60`, `0x86bac0` | `sceGnmDispatchIndirect` | 7 / 0x1C | 19 | |
| `0x86c050`, `0x86c0b0`, `0x86c110` | `sceGnmSetPsShader350`, `sceGnmSetEmbeddedPsShader`, `sceGnmUpdatePsShader350` | 40 / 0xA0 | 2 each | |
| `0x86c170`, `0x86c1e0`, `0x86c250` | `sceGnmSetVsShader`, `sceGnmSetEmbeddedVsShader`, `sceGnmUpdateVsShader` | 29 / 0x74 | 2 each | `gnmdriver.cpp:2100` |
| `0x86c2c0` | `sceGnmSetEsShader` | 20 / 0x50 | 1 | |
| `0x86c330`, `0x86c460` | `sceGnmSetHsShader`, `sceGnmUpdateHsShader` | 30 / 0x78 | 1 each | |
| `0x86c760` | `sceGnmSetLsShader` | 23 / 0x5C | 1 | |
| `0x86c7d0` | `sceGnmSetCsShaderWithModifier` | 25 / 0x64 | 1 | |
| `0x86cb00`, `0x86cb60` | `sceGnmSetGsShader`, `sceGnmUpdateGsShader` | 29 / 0x74 | 1 each | |
| `0x86c900`, `0x86c950`, `0x86c9c0`, `0x86ca20`, `0x86ca90` | `sceGnmResetVgtControl`, `sceGnmSetVgtControl` | 3 / 0xC | 1 to 3 | `gnmdriver.cpp:2069` |

Note: draws, dispatches and shader binds are **not** in the state writers. Shader binds exist twice: as these
imports (section 2.4) and as the inline `0x86d4c0` / `0x86d5e0` writes (Table B). `0x85fc10` (3,994 bytes, the bind
B path) calls both. Which one runs for a given draw is decided by flags **not traced** here.

### 2.5 Table D: sync, DMA, markers

| Emitter | Packet | Call sites | Notes |
|---|---|---|---|
| `0x86edc0` | [WAIT_REG_MEM] (`0xC0053C00`) | 81 | the hottest single emitter; 46 callers |
| `0x86eec0` | [WAIT_REG_MEM] | 16 | |
| `0x86eb60` | [EVENT_WRITE_EOP] (`0xC0044700`), type and address | 20 | |
| `0x86ee70` | [PFP_SYNC_ME] (`0xC0004200`) | 19 | |
| `0x86ced0` | [EVENT_WRITE] (`0xC0004600`), type from `DAT_012901D0` | 7 | |
| `0x86cf40` | [ACQUIRE_MEM] (`0xC0055800`) | 6 | |
| `0x86f1d0` | [EVENT_WRITE] + [ACQUIRE_MEM] | 2 | begin sequence |
| `0x86a700` | [DMA_DATA] (`0xC0055000`), src, dst, size | 62 | 26 callers: the CP DMA copy |
| `0x86a7f0` | [DMA_DATA] to `0x3022C` (fixed) | 9 | called from `0x85fc10` and `0x85f220/0x85f410` |
| `0x86a440` | [WRITE_DATA] (`0xC0003700`) inline payload | 1 | from `0x85c9b0` |
| `0x86f2a0`, `0x86f320`, `0x86f3a0`, `0x86e670`, `0x86e6f0`, `0x8843d0`, `0x884450`, `0x884f10`, `0x885010` | `sceGnmInsertPush/Pop/SetMarker` (imports) | 1 to 3 | marker groups |
| `0x86f700` | `sceGnmInsertWaitFlipDone` (import) | 1 | from `0x91f50` |
| `0x86f9a0`, `0x86f9c0`, `0x86fa80`, `0x882bf0`, `0x884840` | `sceGnmFlushGarlic` (import) | 0 to 1 | |
| `0x86e770`, `0x86e800` | CLEAR_STATE (`0xC0012000`) | 0 / 1 | `0x86e800` from `0x11d3d0` |
| `0x86f4e0` | [opcode 0x85] INCREMENT_DE_COUNTER (`0xC0008500`) | 1 | |

### 2.6 Table E: compute and queue family (`0x880000..0x886000`, live part)

| Emitter | Role | Call sites |
|---|---|---|
| `0x883870` | `sceGnmDispatchDirect` import wrapper (compute queue) | 47 |
| `0x883950` | `sceGnmDispatchIndirectOnMec` | 18 |
| `0x883640` | [DMA_DATA] | 41 |
| `0x884d40` | `sceGnmComputeWaitOnAddress` | **56** |
| `0x885320` | `sceGnmComputeWaitSemaphore` | 2 |
| `0x883e60` | `sceGnmSetCsShaderWithModifier` | 2 |
| `0x8846f0` | [opcode 0x49] RELEASE_MEM | 10 |
| `0x884c40`, `0x884cc0` | [INDIRECT_BUFFER] (`0xC0023F02`) | 3 |
| `0x883f80` | NOP-tagged payload | 13 |
| `0x8839c0` | [SET_UCONFIG_REG x3] (unnamed) | 2 |
| `0x882dc0` | internal predicate, 72 bytes, 125 direct references (feature gate for thread-trace markers; **inferred**) | 125 |
| `0x882ec0` | internal predicate, 68 bytes, the Neo-mode gate for the bit-28 flag (`0x10000000`); **inferred** | many |
| `0x882ab0`...`0x882ce0` | import stubs for `sceGnmSubmit*`, `sceGnmDingDong*`, `sceGnmSubmitDone` | 0 to 3 |

## 3. Who calls them, and whether PM4 avoids this layer

### 3.1 Callers (proven call edges)

- Pass runners and per-pass apply: `0x93960` (calls `0x869990`, `0x869a50`, `0x869ce0`, `0x86d160`, `0x86d210`,
  `0x86edc0`), `0x93ef0`, `0x94600`, `0x91b50`, `0x919e0`, `0x91cd0`, `0x91e10`.
- Draw submitters: `0x93690`, `0x94dd0`, `0x94d00`, `0x94ec0`, `0x94bf0` call `0x86a900` (VGT_PRIMITIVE_TYPE),
  `0x86b040` and `0x86ac10` (draws), `0x85fc10` (bind B), `0x862510` (resource user data). `0x1a8c60` (draws one item
  twice) calls the same set.
- Resource and user data: `0x862510` (calls `0x86d550`), `0x862c50` (draw record, calls `0x86b6f0`, `0x86b810`,
  `0x86d550`), `0x85fc10` (calls `0x86d4c0` 45 times, `0x86d5e0`, `0x86a7f0`), `0x85f220`, `0x85f410`.
- MRT and depth: `0x92140` (calls `0x869f50`, `0x86a290`, `0x86e3a0`).
- Frame and submit: `0xd07c0` -> `0x91f80` -> `0x8636e0`; `0x91f50` <- `0x118470`; per-context begin `0x90470`.

### 3.2 Paths that do not go through the emitter bodies

1. **Thunks, not copies.** `sub_85c9a0` (Ghidra size 1) is a 5-byte `jmp 0x86cc20`: its four packets (regs `0x2D7`,
   `0x298`, `0x2CE`, `0x2AC`) come from the emitter `0x86cc20`, so an entry hook on `0x86cc20` sees them.
   `sub_85ecc0` loads `[rdi+0x90]` and tail-jumps to `0x8841d0`; `0x85ecd0` does the same into `0x884240`
   (section 1.1). **proven** (disassembly).
2. **Packet templates** in `.rodata` (`0x128f9f0..0x128fed0`), copied by live emitters such as `0x86a290`. Their
   constant dwords are part of the PM4 output without a call to the emitter. **proven** (references), **inferred**
   (copy use).
3. **Port-side encoders** (section 0): the PM4 for about 45 imports is written by gnmdriver, not by the game.
4. **Embedded data**: `0x86d210` and `0x86d5e0` put data into the stream (NOP-tagged). The data is written by the
   callers after the emitter returns (section 4.3). **proven** (emitter), **inferred** (caller).

Search for header immediates (byte pattern over the whole eboot image): `0xC0016900` (SET_CONTEXT_REG x1) occurs in
60 functions, all inside the emitter block `0x869350..0x86fd60`, plus 4 hits in the rodata template table.
`0xC0017900` (SET_UCONFIG_REG) occurs outside the emitter block in the dead `0x86fdc0..0x87bfff` family, in the
compute family (`0x883160`, `0x883ac0`, `0x883bb0`, partly live) and in the rodata table. `0xC0055000` (DMA_DATA)
occurs outside the block in the live compute emitter `0x883640` (41 call sites) and in the dead family.
**proven** (byte search; hit lists in `scratchpad/c2/imm.json`).

Conclusion: PM4 from live game code goes through this layer **except** the `.rodata` templates (copied by
`0x86a290` and `0x86cc20`, which are themselves in the layer), the NOP payloads that callers fill (section 4.3), and
the compute family, which has its own live writers in `0x883xxx..0x885xxx`. Interception at this layer covers live
state if those are handled. **inferred** (no runtime coverage check yet).

## 4. Calling conventions

### 4.1 Common contract

- Arg 1 is the cb pointer. Setters take the register and value and return void. Import wrappers return the import
  result in `eax` (0 = written). **proven**
- Reservation: `if ((end - cursor) >> 2 < N) { if (!cb->callback(cb, N, cb->arg)) return; }`. A refused reservation
  drops the packet silently. **proven**
- Register offsets are relative: context `reg - 0xA000`, UCONFIG `reg - 0xC000`, SH `reg - 0x2C00`. **proven**
  (`0x869350`, `0x869370`, `0x8693d0`, `0x86d5e0`).
- SH stage bases (table `DAT_01290410`, ushort per stage index), decoded from the eboot: index 0 `0x2E40` (CS),
  1 `0x2C0C` (PS), 2 `0x2C4C` (VS), 3 `0x2C8C` (GS), 4 `0x2CCC` (ES), 5 `0x2D0C` (HS), 6 `0x2D4C` (LS), 7 `0`
  (none). These are the USER_DATA_0 registers of each stage. **proven** (table bytes); **inferred** (names by the
  GCN register map).
- EVENT_WRITE type table `DAT_012901D0`/`DAT_012901B0`: decoded here as event-type byte values (`0xFFFFFFFF` = no
  event). **proven** (bytes), mapping to event names **unknown**.

### 4.2 Shader and user-data inputs

- `0x86d4c0` takes a raw 64-bit value and writes it as two dwords at the stage's register (most likely the shader
  code address, **inferred**).
- `0x86d5e0` takes a pointer to up to 16 dwords (the SGPR values) and copies them into a NOP-tagged block followed by
  a SET_SH_REG of n dwords. **proven**
- `0x86d160` writes the stage usage id into context `0x2D5`. The usage ids are set by the selector `0xF8AA0` from
  the slot mask (pass-map / slot-stages notes). **proven** (values), **inferred** (register name).

### 4.3 Descriptors (V#, T#, S#)

- The emitters do not encode descriptors. They receive pointers to values. The encoders are separate functions
  named by the native hooks: `0x865410` (buffer V#), `0x8655D0` (constant V#), `0x865600` (image or buffer word),
  `0x865500` (buffer V# with stride) (`scripts/hooks.d/native_writes.py:183-200`). **proven** (hook names)
- Descriptor words end up in memory (the encoders write to an object, for example `0x85c580` passes
  `param_1 + 0xE0`, decomp line 42) and the GPU reads them through the user-data SGPRs written by Table B.
  **inferred** (object identity), **proven** (the GCN path: user data points at memory).
- For embedded descriptors, `0x86d210` reserves aligned space in the cb and returns a pointer. The caller writes the
  words. **proven** (emitter), **inferred** (writer = caller).

### 4.4 Tables decoded for the open items of render-states.md

- `DAT_0128FED0` (`0x86d080`) is a context-register table: `0xA2B4, 0xA2B8, 0xA2BC, 0xA2C0, 0, 0xF0, 0xE8, 0x1E9,
  0xD884..0xD887`. **proven** (bytes). `DAT_0128FEE0` (`0x86e3a0`) is the second half of the same map. Used as
  "value minus `0xA000`". **inferred** (names not decoded).
- `DAT_01290050` is a UCONFIG table (`0xDB40, 0xDB42, 0xDB44, 0xDB45, 0xD340, ...`). **proven** (bytes).

## 5. Feasibility verdict for an HLE at this layer

### 5.1 Number of functions

| Group | Functions | Hook kind | Status |
|---|---|---|---|
| Imports in Tables C, D, E (draws, dispatch, shader, VGT, markers, flush, submit) | about 45 NIDs | runtime NID handler | already HLE in the port; the PM4 encoding inside is the part to change |
| Live state writers (Table A) | 30 SET_CONTEXT-family plus 2 UCONFIG and misc, about 45 | entry hook or guest patch | hookable (5.2) |
| SH and user-data writers (Table B) | 6 live plus `0x884240` | entry hook for 4 of them; body patch or caller hook for `0x86d5e0`, `0x884240` | 2 blocked (5.2) |
| Sync, DMA, markers (Table D) | about 15 | entry hook | hookable |
| Compute family (Table E) | about 15 | entry hook | mostly hookable |
| Size-1 Ghidra subs | `sub_85c9a0`, `sub_85ecc0` are tail jumps (hook the target: `0x86cc20`, `0x8841d0`); `sub_85eca0` loads `[rdi+0x90]` and calls on; `sub_85ed00` not analysed | target entry or site patch | size-1 subs are not in the function table |

Total to cover for live coverage: about 90 entry points; the thunks need no patch of their own; `sub_85eca0` and `sub_85ed00` need a check. The 123 dead candidates of
`0x86fdc0..0x87bfff` need nothing (no reference). The 277 "no reference" candidates need a runtime check before
they are excluded.

### 5.2 Hookability (the port's rules)

- `gpu/sotc/cpu_patches.cpp:3085` refuses any function with an unresolved indirect branch. In the range
  `0x869000..0x886000`, a capstone scan found `jmp reg` or `jmp [mem]` in 14 functions: `0x86bc00`, `0x86d5e0`,
  `0x87c170`, `0x87c1b0`, `0x8806f0`, `0x884240`, `0x885540`, `0x885640`, `0x885690`, `0x885890`, `0x885bd0`,
  `0x885d00`, `0x885e00`, `0x885f90`. **proven**. Of these, the live ones (direct references) are `0x86bc00` (1 call
  site), `0x86d5e0` (37), `0x884240` (19, behind `0x85ecd0`) and `0x8806f0` (2 direct references).
- The other live emitters pass this rule. The port also refuses "not a function entry" (size-1 Ghidra subs, which
  are not in the eh_frame table) and red-zone functions. **proven** (rule), **inferred** (per-function result, not run).
- The same rule explains the **MATERIAL_LOADED** refusal (`pass-map.md:277`). `0xf6c90` is outside this layer; its
  parser has a jump table. For this layer, the entry points to avoid are `0x86bc00`, `0x86d5e0`, `0x884240`, `0x8806f0`.
- `hooks.h` entry hooks run the handler on the host stack; the detour saves a `GuestRegs` block of 18x8 + 128 bytes
  (`gpu/sotc/hooks.h`, struct `GuestRegs`). Each call to a hooked setter costs a host transition. **proven** (struct); the cost per
  call is **unmeasured**.

### 5.3 What the port must track to replace the stream

- Register shadow per context and per SH stage (the port's `regs.h` already has the layout; liverpool fills it from
  PM4 today).
- The cb cursor and the three stream ranges (section 1.1), because `NATIVE_DCB_SUBMIT` announces `[base, cursor)` to
  the upload path (`native_writes.py:8-10`). Changing the layout changes that announce.
- The predicate bit (`cb+0x28 & 1`) and the draw-initiator bits 29..31 (passed from the caller, `0x86b040`).
- Stage usage id `0x2D5` and SH bases (section 4).
- The chunk table (`cb+0x178`, count `cb+0x2e4`) for multi-buffer submits.

### 5.4 Expected gain and its basis

- No gain number is measured here. The known budget: the CP thread is the bottleneck, with `cp cpu 12.7 ms/frame`
  (`base.log:459`). Gain is bounded by the share of that time spent in SET_*, WAIT and EVENT packets. The profiler
  (`SOTC_PROFILE=GpuCommandProcessor`, `tools/profile_functions.py`) gives the share per function. **unknown**
- The game-side cost of an inline setter is about three stores. A per-call host hook costs more than that (5.2).
  An entry hook on the hottest setters (`0x86edc0` 81 refs, `0x86d4c0` 67, `0x86a700` 62, `0x86d160` 43) is therefore
  likely slower than the PM4 it replaces. **inferred** (no timing).
- The import layer gain is small: the port already decodes these packets itself. Replacing draw PM4 with a record
  saves the encode and the decode of a few dozen dwords per draw. Its size depends on the draw count, which is
  **unmeasured**.

### 5.5 Recommended design

1. Do not hook each setter with a host handler. The host transition cost is the risk.
2. For live state, patch each setter body in the image so that it also appends `(reg, value)` to a per-cb shadow
   array in guest code. This needs a code-cave or a guest-side stub. The port has no such mechanism yet (the hook
   handlers run on the host). **inferred**
3. Keep the PM4 stream. The shadow is a second source for the CP, switched on step by step, and checked against the
   decoded PM4 in verify mode (`=2`, as the port already does for other paths).
4. For the import layer, keep the cursor contract (same dword count, or a non-zero return with a cursor the port
   manages).

### 5.6 First three steps

1. Count per frame: the 20 hottest live emitters (Table A, B, D) and `NATIVE_DCB_SUBMIT`, with a counting hook.
   This gives the cost model.
2. Profile `GpuCommandProcessor` by packet opcode for the same scene (`tools/profile_functions.py`), to get the share
   of SET_*, WAIT, EVENT and DMA time.
3. Prototype one family (SH user data, `0x86d4c0` and `0x86d5e0`, the family already wrapped by NATIVE_UDATA) with the
   shadow in verify mode.

### 5.7 Risks

- Coverage: 277 candidates have no reference found. A computed call or a relative table could reach them. Confirm with
  runtime counters before excluding any path.
- The cursor contract and the NATIVE announces depend on the cb layout (section 1.1 and 5.3).
- Two paths write the same registers (import SetVsShader and inline `0x86d4c0` in `0x85fc10`). A shadow must cover both.
- Four entry points are not hookable (`0x86bc00`, `0x86d5e0`, `0x884240`, `0x8806f0`). `NATIVE_UDATA_85ECD0` already works around
  this by reading the cursor, which decodes after the fact and does not save encode time.
- The `.rodata` template table is used by live emitters (`0x86a290`); output changes must keep those constants.

## 6. Open items

1. Per-frame counts of every family (sections 1.5, 5.6).
2. Whether `0x85fc10` runs the import or the inline SH path for each shader (flags not traced).
3. Names of `0x2D5`, `0x8D`, `0x2FA`, `0x10B` (Table A rows marked "not decoded").
4. Whether the 277 "no reference" candidates are dead, reached through 4-byte relative tables, or reached through
   computed calls.
5. The reservation callback `cb+0x18`: its target was not traced; the chunk allocator hook (`NATIVE_CHUNK` at
   `0x8C0D0`, `native_writes.py:27`) is the likely one (**inferred**).
6. Meaning of the NOP tags `0x6875000D` and `0x68750007` (not in `pm4_cmds.h` PayloadType, lines 225-246).
7. Whether the rodata templates feed any live path other than `0x86a290` and `0x86cc20`.

## Evidence files

- Scratch (not in the repo): `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/c2/emit.txt` (bodies of the `0x869350..0x86fd60` emitters),
  `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/c2/table.json` (per-function headers, imports, call counts), `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/c2/dref.json` (direct refs),
  `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/c2/imm.json` (header immediates), `C:/Users/ptr/AppData/Local/Temp/claude/2331aaf31c520dc5/aad34483-bb0c-427c-b86e-432086b85d93/scratchpad/c2/indir.json` (indirect jumps).
- Port: `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`, `gpu/shadps4/video_core/amdgpu/liverpool.cpp`,
  `gpu/shadps4/video_core/amdgpu/pm4_cmds.h`, `gpu/shadps4/video_core/amdgpu/pm4_opcodes.h`, `gpu/sotc/hooks.h`,
  `gpu/sotc/cpu_patches.cpp:3085`, `scripts/hooks.d/native_writes.py`, `src/runtime.c:259-330`.
- Logs: `user/gcn-run1/base.log:450, 459`.
