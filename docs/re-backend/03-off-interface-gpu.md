# 03 - GPU traffic that bypasses the sceGnm* import interface

Read-only static analysis of `out/eboot.elf`. Nothing built, run or committed. Addresses are eboot VAs.
Primary source: Ghidra pseudocode in `out/re/decomp/<4 hex>/<8 hex>.c` (249 directories). Secondary:
`tools/re/eboot_re.py` (imports, icallers). Prior reports: `docs/re-stutters/reports/01, 02, 06`,
`reports2/01, 04, 07`.

Labels: **observed** (decomp, caller list, or prior report), **inferred**, **unknown**.

## 1. Answer

- **observed:** the engine does not sit on the sceGnm* import surface alone. The eboot carries its own gnmx-style
  command writer (0x85xxxx-0x88xxxx). It emits PM4 headers inline into the DCB/CCB (`*puVar = 0xc0xxxxxx`,
  with a chunk-refill callback at `[obj+0x18]`). Draw, dispatch, shader and VGT calls go through imports; most
  context/uconfig/SH register writes, DMA_DATA, WAIT_REG_MEM, EVENT_WRITE_EOP and ACQUIRE_MEM do not.
- **observed:** about 233 eboot functions write PM4 headers without referencing any sceGnm import. Their
  opcode mix: SET_UCONFIG_REG (101 functions), SET_CONTEXT_REG (87), DMA_DATA (20), SET_SH_REG (7),
  WAIT_REG_MEM (7), ACQUIRE_MEM (4), EVENT_WRITE_EOP (3), EVENT_WRITE (3), predication (5), NOP padding (163).
  These counts are a **lower bound** (see section 7).
- **observed:** the import-driven path covers draws (DrawIndex*, DrawIndexAuto, DrawOpaqueAuto, indirect and
  multi variants), dispatches (DispatchDirect, DispatchIndirect), shader binds (Set*Shader), VGT control, the
  submit thunks, flips and GPU waits (InsertWaitFlipDone, ComputeWaitOnAddress).
- **observed:** GPU-to-CPU sync is mostly not via GNM. It is EOP labels written inline, read by the CPU (XPR
  mask, particle counts, water query) with no CPU wait in most paths; flip and frame boundaries use the
  video-out and kernel event APIs; the frame boundary is `sceGnmSubmitDone`.
- **inferred, rough:** the inline share of per-draw state is large (the register setters are inline; the draw
  call itself is an import). A per-frame packet-type count (SotcTrace by packet opcode) is needed for a real
  percentage. This is **unknown** today.
- **Hardest parts for a native backend:** (1) the DCB is PM4, so the native backend must parse PM4 anyway, or
  replace every inline writer; an import-only shim will miss state. (2) EOP labels and WAIT_REG_MEM are the
  real sync contract; the CPU reads GPU-written memory at fixed points (XPR mask, particle stats). (3) CPU
  writes into GPU-visible memory (view constants, particle and instance data, per-frame contexts) have no
  import; they need an explicit coherence model. (4) Compute uses a second queue (ACE/MEC doorbell, ring read
  pointer) with a busy spin (0x863f80, no pause). (5) `sceGnmFlushGarlic` has 11 engine call sites; its
  semantics must be preserved.

## 2. Import inventory (step 1)

`eboot_re.py imports` counts functions that call each import through a PLT or GOT site in eboot. A count of 0
does not mean unused: the decomp shows the `sceGnmSubmit*` calls reached through `jmp` thunks (0x882ac0,
0x882ce0, 0x882bd0), which the caller index does not see. Decomp reference counts (grep of `sceGnm*(` names) are
in the last column.

| Import | eboot callers (index) | Decomp refs | Role / where |
|---|---:|---:|---|
| sceGnmSubmitCommandBuffersForWorkload | 0 (via thunk 0x882ac0) | 5 | Submit of each DCB/CCB; `sub_d07c0` KickoffSubmitJob (observed, report 06) |
| sceGnmSubmitCommandBuffers | 0 | 5 | Same family, not used on the frame path |
| sceGnmSubmitAndFlipCommandBuffers(ForWorkload) | 0 | 5+5 | Not on the frame path (report 06, observed) |
| sceGnmSubmitDone | 0 (via thunk 0x882ce0) | 5 | Frame boundary; only the final submit (mode 1) |
| sceGnmRequestFlipAndSubmitDone(ForWorkload) | 0 | 1+1 | Not called |
| sceGnmDingDong (ForWorkload) | 0 | 8 | Compute doorbell, thunk 0x882bd0; callers 0x85d690, 0x85d6f0 (observed) |
| sceGnmDispatchDirect | 4 | 6 | Family A `0x883870` and B `0x86b9e0` (report 04) |
| sceGnmDispatchIndirect(OnMec) | 2 | 2 | Wrappers 0x86ba60, 0x8838f0; no static caller (indirect only) |
| sceGnmDrawIndex / DrawIndexAuto / DrawIndexOffset | 4 / 2 / 2 | 6 / 2 / 2 | Wrappers 0x86a9d0, 0x86abb0, 0x86afd0 |
| sceGnmDrawIndexIndirect / CountMulti / MultiInstanced | 2 / 2 / 0 | 1 / 4 / 1 | Wrappers 0x86b340-0x86b510 |
| sceGnmDrawOpaqueAuto | 2 | 4 | Wrappers 0x86ac80, 0x86ace0 |
| sceGnmDrawInitDefaultHardwareState350 / ...Context400 | 1 / 1 | 2 / 2 | Wrappers 0x869290, 0x8692f0 (state init) |
| sceGnmSetVgtControl / ResetVgtControl | 4 / 1 | 5 / 2 | Wrappers 0x86c950-0x86ca90, 0x86c900 |
| sceGnmSetPsShader350 / VsShader / HsShader / LsShader / EsShader / GsShader / EmbeddedPs,Vs | 1 / 1 / 2 / 1 / 1 / 1 / 1,1 | 2 / 2 / 3 / 2 / 2 / 2 / 2,2 | Wrappers 0x86c050-0x86c3a0, 0x86c760, 0x86c170, 0x86cb00 |
| sceGnmSetCsShaderWithModifier | 3 | 4 | Wrappers 0x86c7d0, 0x883e60, 0x883ed0 |
| sceGnmInsertWaitFlipDone | 1 | 2 | Wrapper 0x86f700 (WAIT_REG_MEM on flip label) |
| sceGnmComputeWaitOnAddress | 1 | 2 | 0x884d40 (WAIT_REG_MEM on compute queue) |
| sceGnmComputeWaitSemaphore | 1 | 2 | 0x885320; port stub emits nothing (report 02) |
| sceGnmFlushGarlic | 0 (index) | 11 | Calls in 0x86665a0-0x866680, 0x86f9a0-0x86fa80, 0x882bf0, 0x884840-0x884860 and others. CPU/GPU coherence point |
| sceGnmInsertSet/Push/Pop markers | 2-3 each | 3-4 each | Wrappers 0x86e670-0x86f3a0 (debug markers, no GPU state) |
| sceGnmGetPhysicalCounterFromVirtualized | 5 | 7 | 0x87bd00-0x87be50, 0x882c70 (port returns error) |
| sceGnmRegisterOwner / RegisterResource | 0 / 0 | 7 / 7 | Resource registration (0x87f3d0, 0x87f3e0); port-side bookkeeping |
| sceGnmLogicalCuMask/Tca/Cu conversions | 4 / 4 / 1 | 14 / 5 / 6 | Topology queries, not GPU traffic |
| sceGnmAddEqEvent / GetEqEventType / Create/DeleteEqueue | 0 / 2 / - | 1 / 2 | Flip event plumbing |
| sceVideoOutSubmitFlip | 2 | 2 | 0x956f8 (flip-mode change only, report 06) |
| sceVideoOutIsFlipPending / WaitVblank | 2 / 2 | 2 / 2 | Spin at 0x95700-0x95712 (report 02) |
| sceVideoOutAddFlipEvent / GetEventData / GetEventId | 1 / 1 / 1 | | Flip queue 0x95760, consumed by 0x960a0 |
| sceKernelCreateEventFlag / SetEventFlag / WaitEventFlag | 5 / 10 / 6 | | BPE sync events "PS4SyncEvent" (0x3b3d0-0x3b4a0), report 02 |
| sceKernelWaitEqueue | 1 | | Flip thread 0x960f2 |
| sceKernelUsleep | 95 | | Sound middleware mostly (report 02); also `sub_356cd` spin-then-sleep |

Gap: the index sees only eboot. Calls from linked PRX modules (the bundled `sce_module` set) are not in this
count. The map of PRX callers is **unknown**.

## 3. Inline PM4 writers (step 2)

Method: grep of the decomp tree for literal type-3 header constants (`0xc0xxxx00`/`01`, opcode in bits 15:8),
then removal of files that also reference a `sceGnm*` import. A literal header is missed when the decomp
builds it from variables (the function at 0x86a700 has the literal `0xc0055000` but builds its count
field by arithmetic; another writer may do the same and be missed). A capstone immediate scan returned zero
hits and was not debugged; the decomp grep is the source here.

Writer layout (observed in 0x86eb60, 0x86a700, 0x869350): `[obj+0x10]` cursor, `[obj+0x8]` end,
`[obj+0x18]` chunk-refill callback (`[obj+0x20]` context). The writer checks free dwords, calls the refill,
then stores header and body. The packet bytes go into the same DCB the import path uses. The port parses that
DCB (`Liverpool`), so these packets reach the port without any import call.

### 3.1 Register writers (state the import wrappers do not carry)

| Opcode | Header | Example writers (observed) | Notes |
|---|---|---|---|
| SET_CONTEXT_REG 0x69 | 0xc0016900 (1 reg), +counts | 0x869350 (single reg; offset = arg - 0xa000), 0x869bb0, 0x869f50, 0x869990, 0x869a50, 0x869ce0, 0x86d160 (conditional write of reg 0x2d5 by value), 0x86d890, 0x86e3a0, 0x86e4f0, 0x86e1a0, 0x86dd20, 0x86be10, 0x86cc20, 0x86d7b0 | 87 functions in the decomp with 0x69 headers. Purpose of most: unknown (inferred: view and RT setup) |
| SET_UCONFIG_REG 0x79 | 0xc0017900 | 0x86a900 (reg 0x242, 0x10000000 added if `FUN_00882ec0()==1`), 0x8839c0, 0x883ac0 (compute), 0x869580, 0x883260 | 101 functions. Most common. Purpose unknown |
| SET_SH_REG 0x76 | 0xc0017600 / 0xc0027600 / 0xc0047600 | 0x869630 (size 213, 8 header sites, per-stage user data), 0x86d2e0, 0x86d380, 0x86d420 (size 154 each), 0x86d4c0 (called by 0x85f220, 0x85fc10, 0x85f410), 0x86d550 (called by 0x862c50, 0x862510, 0x85f410) | inferred: user data (SGPR/constant buffer pointers) and per-draw SH state. Writers are the main route for constant-buffer addresses |
| SET_CONTEXT_REG 0x69 (indices) | 0xc0026900 | 0x869b50, 0x869bb0, 0x869ee0, 0x86bb90, 0x86d080, 0x86d7b0 | |
| INDEX_BASE 0x26 | 0xc0012600 | 0x86bf50 (size 92; callers 0x11d7a0 = per-draw recorder, 0x1a6d20 = XPR consumer, 0x85c9b0, 0x85cad0, 0x94c60) | Per-draw index base written inline, not via a draw import |

### 3.2 DMA, sync and cache packets (inline)

| Opcode | Header | Writers (observed) | Callers and meaning |
|---|---|---|---|
| DMA_DATA 0x50 | 0xc0055000 (7 dwords) | 0x86a700 (size 238; 13 callers incl. 0x19b400 and 0x19b760 = XPR set build, 0xde9d0, 0xde0c0, 0xd690, 0x1acb10, 0x19c1a0, 0x137b90, 0x18eb10, 0xb3590 = ComputeExposure, 0x18cf40 = fur) | GPU-side copy/fill from guest memory. Used for mask/draw-list and constant copies (inferred). Source and destination addresses come from the caller |
| DMA_DATA (compute) | 0xc0055000 | 0x883640 (same body, compute side; callers 0x85e4c0, 0x1907f0, 0x1adc90, 0x1a5310, 0x191a90, 0x10c8c0, 0x1375e0, 0x193810) | Compute-side DMA |
| DMA_DATA (other) | 0xc0055000 + NOP | 0x870190, 0x870650, 0x870b10, 0x871360, 0x871830, 0x871c90, 0x872960, 0x873150, 0x873b40, 0x874540, 0x875460, 0x875600, 0x876020, 0x876d20, 0x877ae0, 0x878540, 0x8799a0 (size 286-560, no callers in header) | Large family in 0x87 (inferred: resource / texture or buffer setup). Not decoded |
| WAIT_REG_MEM 0x3c | 0xc0053c00 | 0x86edc0 (size 170; 13 callers incl. 0x92720, 0x92b50, 0x93960, 0x927d0, 0x932a0, 0x9264x, 0xa69d0, 0xcaed0), 0x86eec0 (8 callers incl. 0x109ca0, 0x92720, 0x18a170, 0x183640, 0xdb230, 0x11dbe0, 0x10aef0, 0x183d10), 0x86e860 (size 383), 0x86cff0, 0x884a60 (compute) | GPU-side waits. Do not stall the CPU (report 02). 0x86edc0 mode 5 ref 1 is the XPR dispatch wait (report 01) |
| EVENT_WRITE_EOP 0x47 | 0xc0044700 | 0x86eb60 (size 225; 13 callers incl. 0x925c0 = XPR label write, 0x92720, 0x92b50, 0x85cb40, 0x90e30, 0x94320, 0x93960, 0x927d0, 0x932a0, 0x92600, 0x92850, 0x95420), 0x86ec50, 0x86ed40 | The GPU label/EOP writer. Event 0x28 (BOTTOM_OF_PIPE_TS), data 1 for the XPR label (report 01, observed) |
| EVENT_WRITE 0x46 | 0xc0004600 | 0x86ced0, 0x86f1d0, 0x86ead0, 0x884520 | Cache/event flush |
| EVENT_WRITE_EOS 0x48 | 0xc0034800 | 0x86f5e0 (callers 0x92640, 0x926c0), 0x86f530 | End-of-shader event (inferred) |
| ACQUIRE_MEM 0x58 | 0xc0055800 | 0x86cf40 (callers 0x93d70, 0x94320, 0x93840), 0x86f1d0 (callers 0x92640, 0x90470), 0x884dc0, 0x884e80 (compute) | Cache invalidate/flush before reads (inferred). 0x86f1d0 is also an EVENT_WRITE, so it is a combined sync |
| SET_PREDICATION 0x20 / PRED_EXEC 0x22 | 0xc0012000 / 0xc0032200 | 0x86e770, 0x86e800 (caller 0x11d3d0), 0x86ea50 (caller 0x85cea0), 0x866360, 0x883300 (caller 0x85d260) | Conditional draw/dispatch (occlusion-style predication, inferred) |
| INDIRECT_BUFFER 0x3f | 0xc0023f00 | 0x86fa00, 0x86fa80 (no callers in header) | Nested command buffer (not decoded) |
| WRITE_DATA 0x37 | 0xc0003700 | 0x884b70 (compute), 0x8663e0, 0x86a440, 0x86a590, 0x883380, 0x8834d0 | Inline memory write from the CPU stream (inferred: label/flag writes) |
| DRAW_INDEX_MULTI_AUTO 0x34 | 0xc0043400 | 0x86ef60, 0x86f010 (no callers in header) | Not decoded |
| opcodes 0x8c, 0x8e | 0xc0048c00, 0xc0008c00, 0xc0028e00 | 0x86b750 (caller 0x85c580), 0x883ac0 (caller 0x85c580), 0x86a880 (caller 0x13d620) | Opcodes 0x8c/0x8e are not in the standard PM4 set used here (unknown) |
| NOP 0x10 | 0xc0001000 (plus counts) | 163 functions, e.g. 0x85eca0, 0x86a1a0, 0x86a290, 0x86a9d0 | Mostly padding or fill. Not verified |

### 3.3 Import-driven state (for contrast)

The following are pure import wrappers (decomp: the function body calls only the import, no inline header):
0x86a9d0, 0x86aab0, 0x86adc0, 0x86ae30, 0x86abb0, 0x86ac10, 0x86afd0, 0x86b040, 0x86b340-0x86b510, 0x86b970,
0x86b9e0, 0x883800, 0x883870, 0x86c050-0x86c3a0, 0x86c760, 0x86c7d0, 0x86c950-0x86ca90, 0x86cb00, 0x86c170,
0x86c900, 0x86c0b0, 0x86c1e0, 0x86c2c0, 0x86c330, 0x86f700, 0x884d40, 0x885320. Their callers are
listed in report 04 (dispatch families A and B). The per-draw state that is set through these wrappers
(VGT, shader binds, draw) and the inline register writers above (context, uconfig, SH) both reach the same DCB.

## 4. Compute and GPU-driven culling (step 3)

| Item | Address | Evidence / status |
|---|---|---|
| Dispatch family A (sceGnmDispatchDirect via 0x883870) | 36 direct callers (`a1e10`, `a7310`, `103ad0`, `1acdc0`, ...) | observed (report 04) |
| Dispatch family B (0x86b9e0) | 64 direct callers (`928f0`, `a20b0`, `b3590`, `b6760`, `e6350`, `186df0`, ...) | observed (report 04) |
| Dispatch with label wait (0x92b50) | 14 callers (`ab630`, `bdfd0`, `SetViewParams`, `d8660`, `11dbe0`, `18b5b0`, ...). It is WAIT_REG_MEM + EOP label | observed (decomp: 0x92b50 calls 0x928f0, 0x86eb60 EOP, 0x86edc0 WAIT) |
| sub_928f0 (dispatch + label) | 9 callers (`94320`, `95420`, `1018c0`, `117930`, `123410`, `189000`, `18a000`, `18f830`) | observed |
| GPU culling (XPR, index cull) | IndexCullXPRObjects 0x11dbe0; mask cull 0xc6670 -> 0xc6bd0 (shadow) / 0xc6f70 (main); EOP label via 0x925c0 | observed (report 01). Main cull is a compute dispatch: inferred |
| GPU-written mask read by CPU | 0x1a6d20 (BPXPR_ConsumeVisibilityMask) reads the bitset, emits one DrawIndexIndirect per visible object (0x86b3d0) | observed (report 01, 07). Most read faults in the sample (43%) |
| Second consumer (GPU-written count) | 0x1018c0 ProcessStats reads a two-slot ring at 0x101a98 via 0x8c1b0 | observed (report 07) |
| Water query | 0xe48b0 emits WAIT_REG_MEM then reads one dword; no CPU wait | observed (report 07) |
| Compute ring | 0x863af0 (gnmx internalSubmitAndFlip): retries 0x85d380 until not busy at 0x863f80 with no pause or sleep; 0x85d3d0 reads a GPU-written read pointer | observed code; read pointer role inferred (report 02) |
| Compute doorbell | 0x91f90 -> 0x863aa0 (mode 1 only); sceGnmDingDong thunk 0x882bd0 from 0x85d690, 0x85d6f0 | observed |
| Compute wait on address | 0x884d40 (WAIT_REG_MEM, compute queue) | observed |
| Compute wait semaphore | 0x885320, port emits nothing | observed (report 02) |

Per-frame dispatch counts are **unknown** (report 04, open question 1).

## 5. CPU-GPU sync (step 4)

| Mechanism | Where | Stall? | Status |
|---|---|---|---|
| EOP label write (EVENT_WRITE_EOP, data 1) | 0x86eb60 callers incl. 0x925c0 (XPR), 0x92b50 | Not on CPU | observed |
| CPU wait on a GPU label | none found in render code (report 01, 02) | - | unknown whether any exists |
| GPU-side wait (WAIT_REG_MEM) | 0x86edc0, 0x86eec0, 0x884d40, 0x86f700 (flip label) | No (GPU side) | observed |
| Frame boundary: sceGnmSubmitDone | thunk 0x882ce0, called once per frame from `KickoffSubmitJob` (mode 1) | Yes: port's gate `GateWait(2)` (report 06) | observed |
| Submit | thunk 0x882ac0 (5 mid-frame submits, 1 final) | Port: `GateWait(0)` | observed (report 06) |
| Event flags (render thread gates) | 0x3b3d0-0x3b4a0, `sub_bd6d0` waits at 0xbd717, 0xbd908, 0xbd93d | Yes (CPU) | observed (report 02) |
| Spin then usleep(1) | `sub_356cd` in the job system, `sub_90ea0` (deferred free wait) | Yes, 0.1-1 ms per poll on Windows | observed (report 02) |
| Flip wait | 0x956e0: sceVideoOutSubmitFlip then spin on IsFlipPending + WaitVblank (0x95700-0x95712) | Yes, mode change only | observed |
| Flip event thread | 0x960a0 blocks in sceKernelWaitEqueue (0x960f2) | Own thread | observed |
| sceGnmFlushGarlic | 11 call sites in 0x86-0x88 (0x8665a0, 0x866680, 0x86f9a0, 0x86fa80, 0x882bf0, 0x884840-0x884860, ...) | Unknown | semantics unknown; likely CPU write visibility |

The one path that clearly needs a GPU result on the CPU is the XPR mask (report 07, open question 1: the
game may consume a frame late). Until that is settled, a native backend cannot assume a fence is always
present before a CPU read.

## 6. CPU writes into GPU-visible memory (step 5)

No import is involved in these; the GPU reads the memory directly through pointers set in SH registers,
DMA_DATA sources or descriptor tables.

| Writer | Address | Evidence |
|---|---|---|
| Per-frame render context (command plus data) | `PerFrameRenderContexts::RequestNewRenderContext` 0x90470 (two sizes) | observed (report 03, decomp `RequestNewRenderContext`) |
| Compute context | 0x906d0 | observed |
| View constants | `SetViewParams` 0xbe1f0, `EndFrameViewParams` callees | observed writes (report 03) |
| Particle records and stats | 0x101720 (writes, 8.7% of write faults), 0x107540 BeginFrame | observed (report 03) |
| Instance and skinning data | 0x118100 -> 0x123410 (`InstanceData_WriteStream`) | observed writers, no CPU read (report 07) |
| Material params | 0x117160 (`updateDynamicGPUDataJob`) | observed (report 04 names) |
| Host memcpy into GPU pages | host `0x7ffda260xxxx`, about 47% of write faults in the sample | observed, attribution inferred (report 03) |
| Command buffers (DCB/CCB) | writers 0x86xxxx family into `RequestNewRenderContext` chunks | observed (report 06, 01 of reports2) |
| Heap metadata sharing GPU pages | BPE heap 0x48a70-0x4a477 | observed (report 03). Not a GPU consumer |

The GPU memory type is `GPUMemory` (`sub_110a0`, observed in report 03). Whether constant data is copied with
DMA_DATA or read directly from a pointer is **unknown**; the 20 DMA_DATA writers are the first place to check.

## 7. Limitations

- Literal-header scan only (section 3). Headers built from variables or computed counts are missed. The
  caller lists in the decomp headers are truncated (`...`), so caller counts are minimums.
- Opcodes 0x8c and 0x8e are not in the standard PM4 set and are not decoded.
- PRX-linked modules are not in the eboot index; their PM4 writers and imports are not counted.
- The 163 NOP-header functions may include non-PM4 constants; not verified.
- Packet semantics (what each register value means) are not decoded here. That needs the port's PM4 parser
  (`gpu/shadps4/`) or a runtime trace.

## 8. Open questions

1. Per-frame count of inline packets by opcode, and the share of draw-state packets that are inline vs import.
   Method: SotcTrace counter by opcode in the port's DCB parser, grouped by the writer's return address.
2. Does the game CPU-wait on any EOP label? Settle with a trace of 0x1a6d20 entry and the label value
   (report 01 open question 1).
3. What does `sceGnmFlushGarlic` guarantee on PS4, and which of its 11 call sites need a real flush on Vulkan?
4. Which writers build the DMA_DATA sources and destinations (0x86a700 callers: 0x19b400, 0x19b760, 0xb3590)?
5. Roles of SET_CONTEXT_REG 0x2d5 (0x86d160), UCONFIG 0x242 (0x86a900), and opcodes 0x8c / 0x8e (unknown).
6. Whether the PRX-linked modules contain any additional PM4 writers or GNM callers.
