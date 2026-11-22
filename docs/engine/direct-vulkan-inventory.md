# Direct Vulkan: inventory of the game's PM4 writers (step 1)

Date: 19 Nov 2026. Branch: `direct/vulkan` (from upstream/main, Release 0.76.2, emulated shadPS4 path).
Scope: every function of the EU 1.01 eboot (CUSA08809) that writes PM4 packets into a command buffer, plus the
host-side writers and consumers the packets meet. Nothing was built, run or committed. Prior RE is read from
`pc/native` (`docs/re-engine/E-10-backend-boundary.md`, `docs/engine/gpu-command-inventory.md`,
`scripts/hooks.d/pc_*.py`, `scripts/hooks.d/native_bridge.py`) and checked against REA.

Tags: **proven** = read in REA (bytes, decompile or caller list) or in the repo's code, with address or line;
**inferred** = from a structure or a name; **unknown** = not established.

Addresses are EU 1.01 eboot VAs. In the REA image (`eboot_pe_fde.exe`) the file offset equals the VA.

## 1. Summary

- There are two writer classes, and they must be handled differently.
  - **Class A, PM4 encoded inside the eboot.** Context, config, UCONFIG and SH register writers, WRITE_DATA,
    DMA_DATA, EVENT_WRITE_EOP, WAIT_REG_MEM and COND_EXEC emitters carry their packet header as an immediate in
    `.text` (proven, section 3). These are the game's own PM4 encoders.
  - **Class B, draw and dispatch packets encoded outside the eboot.** The cursor-level draw and dispatch wrappers
    0x86ac10 (DrawIndexAuto) and 0x86b9e0 (DispatchDirect) only check the cursor's room, call an import stub, and
    advance the cursor. The stubs (0xf90416, 0xf903e6) push an import index and jump to the common import
    dispatcher 0xf8f380 (proven, REA instructions). No DRAW_INDEX_AUTO (0x2d) or DISPATCH_DIRECT (0x15) header
    immediate exists in `.text` (proven, byte scan). The packet is therefore written by the host side of the
    import, which is not yet named (gap G1). This decides the seam for the typed stream: see section 7.
- Prior-doc corrections found in this pass:
  - 0x86ac10 has **2** static callers (0x93690, 0x94ec0). E-10 section 2's "about 30 static call sites" is the
    count of callers of the draw object 0x94ec0, not of the wrapper 0x86ac10.
  - 0x86b040 has 6 REA callers (0x94d00, 0x94dd0, 0x133550, 0x133a50, 0x133bd0, 0x1a8c60). The earlier
    `pc_draw.py` comment also lists 0x133750, 0x1338d0 and 0x133e20, which REA does not report now.
  - 0x8663e0 (WRITE_DATA, dst_sel 5) has **0** static callers in REA. It is reached through a computed call or a
    table; its hook (PCR_WRITE_DATA_A, NATIVE_PM4_WRITE_DATA_A) therefore fires only if that path runs.
  - The hook `NATIVE_JOB_VIEW_COPY` in `native_bridge.py` (line 216) is registered at 0x92b50. E-10 identifies
    0x92b50 as BPGnm_DispatchWithLabelWait, so the name and the address disagree. Verify before relying on it.
- Shader and user-data state reach the GPU through SET_SH_REG writers (0x86d5e0 generic, 0x86d550, 0x86d4c0,
  0x86d380, 0x86d420, 0x86d2e0). Their callers are the bind and flush functions 0x85f220, 0x85f410, 0x85fc10 and
  0x861xxx-0x862xxx (proven, REA callers of 0x86d5e0).
- SET_PREDICATION is written (0x86e770 / 0x86e800), but shadPS4 does not implement it. `Liverpool::ProcessGraphics`
  logs "Unimplemented IT_SET_PREDICATION (reported once)" and continues (proven, liverpool.cpp:1427-1431).

## 2. Method and evidence quality

- REA session `eboot_pe_fde.exe` (Ghidra 12.2, already open; not reopened or closed). Used: `search_bytes`
  (type-3 header patterns `00 OP ?? C0`, full .text/.rdata/.data/.pdata for SET_CONTEXT_REG, SET_SH_REG,
  SET_CONFIG_REG, UCONFIG, DRAW_INDEX_AUTO, DISPATCH_DIRECT), `procedure_pseudo_code`, `procedure_info`,
  `read_function_instructions`, `procedure_callers` (summary detail).
- A local byte scan (ripgrep on the eboot, offsets = VAs) covered 24 opcodes. Its raw counts are noisy: 0x10, 0x49,
  0x79 and 0x20 match ordinary code and data. Only the families listed as proven below are trusted; the rest is
  in section 3.9 as candidates.
- Header patterns cannot be matched when the header is built by arithmetic at run time. The absence of a literal
  DRAW_INDEX_AUTO or DISPATCH_DIRECT header therefore proves only that none is an immediate, not that none is
  built.
- Caller lists are "resolved Ghidra call references". Computed calls are not in them (REA reports `callers:
  unknown`).

## 3. Writer inventory

### 3.1 Draw and dispatch wrappers (cursor level, class B)

Cursor layout (proven from the decompile of 0x86ac10 / 0x86b9e0): `cb+0x08` = end, `cb+0x10` = write pointer,
`cb+0x18` = reserve callback (called as `(buffer, N, cb)`, see 0x862800), `cb+0x20` = buffer argument,
`cb+0x28` bit 0 = a flag OR-ed into the packet.

| Address | Name / role | Packet(s) | Arguments (registers at entry) | Callers (REA) | Typed record | Hook status | Tag |
|---|---|---|---|---|---|---|---|
| 0x86ac10 | DrawIndexAuto wrapper (`CRenderContext`, count, flags) | DRAW_INDEX_AUTO 0x2d, produced by import stub 0xf90416 (`push 0x108; jmp 0xf8f380`). Reserves 7 dwords, advances `cb+0x10` by 0x1c | rdi = ctx, esi = vertex count, edx = flags (`param_2 << 0x1d \| flags & 1` is the import's first argument) | 2: 0x93690, 0x94ec0 | `DrawIndexAuto{count, initiator}` | PCR_CUR_DRAW_AUTO (pc_cursor_draws, pc only); PCR_DRAW_AUTO (0x94ec0), PCR_DRAW_AUTO_RECT (0x93690) | proven (decompile), packet inferred |
| 0x86b9e0 | DispatchDirect wrapper | DISPATCH_DIRECT 0x15, produced by import stub 0xf903e6. Reserves 9 dwords, advances `cb+0x10` by 0x24 | rdi = cb, esi = dim (threads), edx = low dword of arg 1, ecx = `(flags & 1) + 8 * param_3` | 60 summary rows, 3 omitted (partial). Includes 0x928f0, 0x92bd0, 0xb3590, 0xde0c0, 0xde9d0, 0x10d1b0, 0x10d690, 0x18cf40, 0x18d400, 0x18e920, 0x19c1a0, 0x1a4630, 0x1a8180, 0x1acb10, 0x1ad3f0, ... | `DispatchDirect{x, y, z, initiator}` | PCR_CAP_DISPATCH (pc_passes, Return); PCR_CAP_DISPATCH_IMP (0x883870) | proven (decompile, callers), packet inferred |
| 0x883870 | DispatchDirect, import twin | as 0x86b9e0 | as 0x86b9e0 | not counted | as 0x86b9e0 | PCR_CAP_DISPATCH_IMP (pc_passes) | proven (hook pattern) |
| 0x86b040 | DrawIndexOffset wrapper | DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2 (import-side) | rdi = ctx, esi = index offset, edx = index count (`rdi, esi, edx` per pc_draw.py) | 6: 0x94d00, 0x94dd0, 0x133550, 0x133a50, 0x133bd0, 0x1a8c60 | `DrawIndexOffset{offset, count}` | PCR_CUR_DRAW_OFFSET (pc_draw, Entry) | proven (callers), packet inferred |
| 0x86b150 | DrawIndirect wrapper (non-indexed) | DRAW_INDIRECT 0x24 (import-side) | rdi = ctx, esi = args byte offset | 1: 0x10add0 | `DrawIndirect{args_addr, offset}` | PCR_CUR_DRAW_INDIRECT (pc_cursor_draws) | proven (callers) |
| 0x86b3d0 | DrawIndexIndirect wrapper | DRAW_INDEX_INDIRECT 0x25 (import-side) | rdi = ctx, esi = args byte offset | 3: 0xdb620, 0x1a6d20, 0x1a7290 | `DrawIndexIndirect{args_addr, offset}` | PCR_CUR_INDEX_INDIRECT (pc_draw) | proven (callers) |
| 0x86bac0 | DispatchIndirect wrapper (`sceGnmDispatchIndirect`) | DISPATCH_INDIRECT 0x16 (import-side) | rdi = cb, esi = data offset, edx = flag | not counted | `DispatchIndirect{args_addr, offset}` | PCR_CC_IND_B (pc_cull_chain) | proven (hook), packet inferred |
| 0x883950 | DispatchIndirectOnMec (import wrapper) | DISPATCH_INDIRECT (MEC) | rdi = cursor, rsi = argument block address, rdx = flag | not counted | as 0x86bac0 | PCR_CC_IND_A (pc_cull_chain) | proven (hook) |
| 0x8838f0 / 0x86ba60 | twins of 0x883950 / 0x86bac0 without the flag | as above | as above | - | as above | PCR_CC_IND_A2 / PCR_CC_IND_B2 | proven (hook) |
| 0x86ad40 | SET_BASE (index 1 = indirect args base) | SET_BASE 0x11 | rdx = base | 20 (0xdb620, 0xde0c0, 0xde9d0, 0xdf300, 0xdf7e0, 0x1051f0, 0x106ce0, 0x10add0, 0x11d7a0, 0x18eb10, 0x18ed40, 0x18f8a0, 0x18fc70, 0x1905b0, 0x1a42e0, 0x1a4800, 0x1a6c80, 0x1a6d20, 0x1ad060, 0x1ad590) | `SetBase{index, address}` | PCR_SHADOW_SET_BASE (Continue) | proven (callers) |
| 0x86bf50 | INDEX_BASE | INDEX_BASE 0x26 | rsi = base | not counted | `IndexBase{address}` | PCR_SHADOW_INDEX_BASE | proven (hook) |
| 0x86bed0 | INDEX_TYPE | INDEX_TYPE 0x2a | esi = flag | not counted | `IndexType{u32 or u16}` | PCR_SHADOW_INDEX_TYPE | proven (hook) |
| 0x86c000 | IT_NUM_INSTANCES | NUM_INSTANCES 0x2f | esi = count (`c7 00 00 2f`) | not counted | `NumInstances{n}` | PCR_SHADOW_INSTANCES | proven (hook) |
| 0x86a900 | VGT_PRIMITIVE_TYPE via UCONFIG 0x242 | SET_UCONFIG_REG 0x79, reg 0x242 | esi = primitive & 0x3f; literal write at 0x86a94e (`c7 06 00 79 01 c0`) | not counted | `SetPrimitive{type}` | PCR_SHADOW_PRIM (entry), PCR_PRIM_WRITE (0x86a94e) | proven (bytes, hook) |

Draw objects (callers of section 3.1; they bind state, then call the wrapper):

| Address | Role | Calls | Typed use | Hook | Tag |
|---|---|---|---|---|---|
| 0x93690 | colour clear / fullscreen rect draw, prim 0x11 | 0x86ac10 (count 3) | `DrawIndexAuto{3}` in a clear pass | PCR_DRAW_AUTO_RECT (pc_draw) | proven |
| 0x94ec0 | DrawIndexAuto(count) draw object, ~30 callers of its own | 0x86ac10 | `DrawIndexAuto{count}` | PCR_DRAW_AUTO (pc_draw) | proven |
| 0x94d00 | DrawIndexOffset draw object | 0x85fc10, then 0x86b040 | `DrawIndexOffset` | PCR_DRAW_OFFSET (pc_draw) | proven |
| 0x94dd0 | DrawIndexOffset + instances (7th argument) | 0x86b040, 0x86c000 (0xC0002F00) | `DrawIndexOffset`, `NumInstances` | PCR_DRAW_OFFSET_INSTANCED (pc_draw) | proven; no static caller (E-10) |
| 0x10add0 | DrawIndirect draw object | 0x86b150, 0x86ad40 | `DrawIndirect` | PCR_DRAW_INDIRECT (pc_draw) | proven |
| 0xdb620 | DrawIndexIndirect mesh draw object | 0x86ad40, 0x86b3d0 (count from +0xc398 / +0xc3a8 / +0xc3b8 by kind +0xc548) | `DrawIndexIndirect` | PCR_DRAW_INDEX_INDIRECT (pc_draw) | proven |

### 3.2 Material and shader binds (class A + import-side)

| Address | Role | Packet(s) | Callers / use | Hook | Tag |
|---|---|---|---|---|---|
| 0x85fc10 | Material_BindGraphicsShaders_B (3994 B): reads ctx+0x2e8, emits VS/PS/GS binds and user data | SET_SH_REG (via 0x86d4c0, 0x86d5e0), SET_CONTEXT_REG | >= 13 callers (E-10) | none (fallback for callers not covered by draw objects) | proven (E-10) |
| 0x860bb0, 0x861260, 0x861520 | PS, record pointers (+0xc0c0 / +0xc0d0 / +0xc0b0) and dirty bits | none directly | material setters | none | proven (E-10) |
| 0x861600 | PS header setter (S+0xc0a8) | none directly (state) | resets from 0x93960 / 0x93ef0 / 0x94600 | PCR_SHADER_SET_PS (pc_shader_programs) | proven (hook) |
| 0x86c2c0, 0x86cb00, 0x86cb60 | VS / PS / GS bind emitters called from 0x85fc10 | SET_SH_REG | from 0x85fc10 | none | proven (E-10 names only) |
| 0x862510 | start-vertex SGPR writer (stage 2, VS) | SET_SH_REG 0x76 (via 0x86d550) | from draw objects | PCR_SGPR_WRITE1 (0x86d550) | proven (hook doc) |

### 3.3 State writers: SET_CONTEXT_REG, SET_CONFIG_REG, SET_UCONFIG_REG

Each writer emits one packet of one register (or a small table). The header is an immediate in the eboot (class A).
The state hooks are the `pc_draw_state.py` group (0x869350 family) and `pc_draw_state2.py` (`PCR_STATE4_*`).

| Address | Register(s) | Packet | Hook | Tag |
|---|---|---|---|---|
| 0x869350 | generic: esi = absolute register (0xA000 + index), edx = value | SET_CONTEXT_REG 0x69 | pc_draw_state (Continue) | proven (hook doc) |
| 0x869370 | generic with index field; no caller | SET_CONTEXT_REG | PCR_STATE4_REG_INDEXED | proven (hook doc) |
| 0x8694e0 | range of ecx registers copied from rdx; no caller | SET_CONTEXT_REG | PCR_STATE4_REG_RANGE | proven (hook doc) |
| 0x869990 | CB_TARGET_MASK (0x8e) | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869a50 | CB_BLEND n control (0x1e0 + slot) | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869ac0 | CB_BLEND_RED..ALPHA 0x105..0x108; dead in index | SET_CONTEXT_REG | PCR_STATE4_* | proven (hook doc) |
| 0x869b50 | STENCILREFMASK, one value (0x10c, 0x10d) | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869bb0 | STENCILREFMASK front / back | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869ce0 | CB_COLOR_CONTROL (0x202) | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869d60 | DB_DEPTH_CONTROL (0x200) | SET_CONTEXT_REG | pc_draw_state | proven |
| 0x869dc0 | DB_STENCIL_CONTROL (0x10b) | SET_CONTEXT_REG | pc_draw_state2 | proven |
| 0x869e80 | DB_DEPTH_CONTROL = 0; no caller | SET_CONTEXT_REG | PCR_STATE4_DEPTH_CONTROL_ZERO | proven (hook doc) |
| 0x869ee0 | DB_DEPTH_BOUNDS_MIN/MAX (0x08, 0x09); dead in index | SET_CONTEXT_REG | PCR_STATE4_* | proven (hook doc) |
| 0x869f50 | DB_DEPTH_CLEAR (0xb), clear value in xmm0 | SET_CONTEXT_REG | PCR_CAP_DEPTH_CLEAR (Return) | proven (E-10, hook) |
| 0x869fb0 | DB_STENCIL_CLEAR (0x0a); dead in index | SET_CONTEXT_REG | PCR_STATE4_* | proven (hook doc) |
| 0x8699f0 | DB_ALPHA_TO_MASK (0x2dc); no caller | SET_CONTEXT_REG | PCR_STATE4_ALPHA_TO_MASK | proven (hook doc) |
| 0x86a3d0 | PA_SC_AA_MASK (0x30e, 0x30f); no caller | SET_CONTEXT_REG | PCR_STATE4_AA_MASK | proven (hook doc) |
| 0x86a120 | scissor (E-10 table A) | SET_CONTEXT_REG | none | proven (E-10) |
| 0x86a1a0 | colour target block (CB_COLOR0..5, 14 dwords, 0x318 + 15n) | SET_CONTEXT_REG | via 0x85cbc0 | proven (E-10) |
| 0x86a290 | depth target block | SET_CONTEXT_REG | PCR_CAP_DEPTH_BASE | proven (hook) |
| 0x86bb90 | VGT_INSTANCE_STEP_RATE_0/1 (0x2a8, 0x2a9); dead | SET_CONTEXT_REG | PCR_STATE4 | proven (hook doc) |
| 0x86bc00 | SPI_PS_INPUT_CNTL_0.. (0x191 + n) from table; caller 0x94ab0 | SET_CONTEXT_REG | PCR_STATE4_PS_INPUT_CNTL, HLE_F_86BC00 | proven (hook doc) |
| 0x86be10 | DB_RENDER_OVERRIDE (0x003) | SET_CONTEXT_REG | PCR_STATE4_RENDER_OVERRIDE | proven (hook doc) |
| 0x86d7b0 | viewport n (0xb4/0xb5 + 0x10f..0x114) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86d890 | PA_SC_VPORT_SCISSOR_n | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86d950 | PA_SC_SCREEN_SCISSOR (0x0c, 0x0d) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86db40 | PA_CL_VTE_CNTL (0x206) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86dba0 .. 0x86dd80 | group of single-register writers (E-10 table A class) | SET_CONTEXT_REG | not hooked | proven (literal header, section 3.9) |
| 0x86dd20 | DB_RENDER_CONTROL (0x000) | SET_CONTEXT_REG | PCR_STATE4_RENDER_CONTROL | proven (hook doc) |
| 0x86ded0 | PA_CL_CLIP_CNTL (0x204) | SET_CONTEXT_REG | PCR_STATE4_CLIP_CONTROL | proven (hook doc) |
| 0x86e140 | VGT_MULTI_PRIM_IB_RESET_INDX (0x103); dead | SET_CONTEXT_REG | PCR_STATE4 | proven (hook doc) |
| 0x86e0e0 | VGT_MULTI_PRIM_IB_RESET_EN (0x2a5); dead | SET_CONTEXT_REG | PCR_STATE4 | proven (hook doc) |
| 0x86e1a0 | PA_SU_SC_MODE_CNTL (0x205) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86e200 | PA_SC_LINE_CNTL (0x282); dead | SET_CONTEXT_REG | PCR_STATE4 | proven (hook doc) |
| 0x86e340 / 0x86e410 / 0x86e480 | PA_SU_POLY_OFFSET depth bias (0x2df) / scale-offset (0x2e0, 0x2e1) / back (0x2e2, 0x2e3) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86e3a0 | PA_SU_POLY_OFFSET_DB_FMT_CNTL (0x2de) = table 0x128fee0[esi] | SET_CONTEXT_REG | PCR_STATE4_POLY_OFFSET_DB_FMT | proven (hook doc) |
| 0x86e4f0 | PA_SC_MODE_CNTL_0 (0x292) | SET_CONTEXT_REG | PCR_STATE4_SC_MODE_CNTL0 | proven (hook doc) |
| 0x86e560 | AA_CONFIG (0x2f8) | SET_CONTEXT_REG | pc_draw_state2 | proven (hook doc) |
| 0x86c000 | IT_NUM_INSTANCES (0xC0002F00) | SET_CONTEXT_REG | PCR_SHADOW_INSTANCES | proven (hook) |
| 0x862690 | command-buffer init (rdi = new cb, one caller 0x917c0) | many | PCR_STATE4_CB_INIT | proven (hook doc) |
| 0x869290 | sceGnmDrawInitDefaultHardwareState350 wrapper, one caller 0x90470 | many | PCR_STATE4_HW_DEFAULTS | proven (hook doc) |

Configuration (SET_CONFIG_REG 0x68): literal header at 0x86943c, 0x869530, 0x883140, 0x88322d (REA search,
proven as bytes). No hook covers them; their owners are not decoded (gap G7).

### 3.4 User-data and SGPR writers (SET_SH_REG 0x76)

| Address | Role (proven where stated) | Callers (REA) | Hook | Tag |
|---|---|---|---|---|
| 0x86d5e0 | generic user-SGPR writer (cmdbuf, stage, slot, data, count) | 13: 0x85f220, 0x85f410, 0x85fc10, 0x8611a0, 0x8619a0, 0x861b40, 0x861ce0, 0x861e60, 0x861fe0, 0x862160, 0x8622e0, 0x862460, 0x862490 | PCR_SGPR_WRITE, HLE_F_86D5E0 | proven (REA callers, hook) |
| 0x86d550 | one-dword SGPR writer (cmdbuf, stage, slot, value) | 0x862510 (start vertex) | PCR_SGPR_WRITE1 | proven (hook doc) |
| 0x86d4c0 | 64-bit SGPR writer (chunk pointers) | 0x85f220, 0x85fc10, 0x85f410 | PCR_SGPR_WRITE64 | proven (hook doc) |
| 0x86d380 | 8 dwords copied from a pointer (direct) | not counted | PCR_SGPR_DIRECT8 | proven (hook) |
| 0x86d2e0 | 4 dwords (direct), variant A | not counted | PCR_SGPR_DIRECT4A | proven (hook) |
| 0x86d420 | 4 dwords (direct), variant B | not counted | PCR_SGPR_DIRECT4B | proven (hook) |
| 0x884240 | import-path compute SGPR writer (cursor at S+0xc098) | import setters | PCR_SGPR_IMPORT | proven (hook) |
| 0x8841d0 / 0x884360 / 0x884050 / 0x884150 / 0x8840d0 | import-path writers: 64-bit, 1 dword, 4 dwords (A and B), 8 dwords | import flushes 0x85e5d0, 0x85f410, 0x862c50 | PCR_SGPR_IMPORT2 / 1 / 4A / 4B / 8 | proven (hook) |
| 0x8832b0 / 0x883180 | absolute compute user register, N dwords / 1 dword | - | PCR_SGPR_IMPORT_ABSN / _ABS1 | proven (hook) |

### 3.5 Memory writes: WRITE_DATA, DMA_DATA, copies

| Address | Packet | Header / control | Callers | Hook | Tag |
|---|---|---|---|---|---|
| 0x8663e0 | WRITE_DATA, dst_sel 5 (async memory) | 0xC0003700 \| (count+2) << 16 (`81 c1 ..`), control 0x84000500 \| (flags & 1) << 20 | 0 (REA) | PCR_WRITE_DATA_A, NATIVE_PM4_WRITE_DATA_A | proven (bytes), callers zero |
| 0x86a440 | WRITE_DATA, dst_sel 5 | control 0x04000500 \| (flags & 1) << 20 | not counted | PCR_WRITE_DATA_B, NATIVE_PM4_WRITE_DATA_B | proven (bytes) |
| 0x86a590 | WRITE_DATA, dst_sel 2 (TC-L2) | control built from `lea r8d,[r14+r13+0x200]` | not counted | PCR_WRITE_DATA_C, NATIVE_PM4_WRITE_DATA_C | proven (bytes) |
| 0x86a700 | DMA_DATA 0x50 (7 dwords per packet, byte count `& 0x1fffff`) | header 0xC0055000 (`c7 00 00 50 05 c0` form, asm proven), DW1 bit 20 = DST_SEL bit 0, bits 29-30 = SRC_SEL | 20: 0xa8e10, 0xb3590, 0xde0c0, 0xde9d0, 0x10d1b0, 0x10d690, 0x137b90, 0x18cf40, 0x18d400, 0x18e920, 0x18eb10, 0x193aa0, 0x194a00, 0x19b400, 0x19b760, 0x19c1a0, 0x1a4630, 0x1acb10, 0x1ad3f0, 0x85db40 | PCR_CAP_DMA (Return), NATIVE_PM4_DMA | proven (asm, callers) |
| 0x883640 | DMA_DATA import twin (GDS counter loads of compute jobs) | DMA_DATA | not counted | PCR_CC_DMA_IMP | proven (hook) |

### 3.6 Synchronisation: EVENT_WRITE_EOP, CS-done, WAIT_REG_MEM, COND_EXEC, predication

| Address | Packet | Callers (REA) | Hook | Tag |
|---|---|---|---|---|
| 0x86eb60 | EVENT_WRITE_EOP 0x47 (label write) | 17: 0x90e30, 0x92540, 0x925c0, 0x92600, 0x92720, 0x927d0, 0x92850, 0x92b50, 0x932a0, 0x93960, 0x93ef0, 0x94600, 0x95420, 0x13d620, 0x16f180, 0x16f1f0, 0x85cb40 | PCR_CAP_EOP (Return), NATIVE_JOB_EOP | proven |
| 0x925c0 | cull label write (EOP value 1 at rsi) | 0xc6bd0, 0xc6f70, 0xc5200 (E-10) | PCR_CAP_CSDONE, NATIVE_JOB_CSDONE | proven |
| 0x86edc0 | WAIT_REG_MEM 0x3c | 39 | PCR_CAP_WAIT, NATIVE_JOB_WAIT | proven |
| 0x86eec0 | WAIT_REG_MEM variant | not counted | PCR_CAP_WAIT_B, NATIVE_JOB_WAIT_B | proven |
| 0x884d40 | ComputeWaitOnAddress (`sceGnmComputeWaitOnAddress`) | not counted | PCR_CAP_CW_ADDR | proven (hook) |
| 0x92b50 | BPGnm_DispatchWithLabelWait: dispatch via 0x928f0, EOP, WAIT | 0x91xxx-0x9xxxx callers (E-10) | NATIVE_JOB_VIEW_COPY (name mismatch, see 1) | proven (E-10) |
| 0x90e30 | frame-slot EOP (only when its mode arg is nonzero) | from 0x118470 | none | proven (E-10) |
| 0x86e770 | SET_PREDICATION enable (rdi = cb, rsi = address, edx = op) | not counted (0x11d3d0 calls 0x86e800) | pc_predication hooks (mode per file) | proven (emitters) |
| 0x86e800 | SET_PREDICATION disable | called by 0x11d3d0 (site 0x11d570) | pc_predication | proven |
| 0x85cea0 | COND_EXEC driver (BEGIN when rsi != 0) | not counted | pc_predication | proven (emitters) |
| 0x86ea50 | COND_EXEC emitter (predicate not evaluated) | 0x85cea0 | PCR_CAP_COND_A | proven |
| 0x85d260 / 0x883300 | COND_EXEC variant on second stream | not counted | PCR_CAP_COND_B | proven (hook) |

### 3.7 Setup, reserve and RT writers

| Address | Role | Hook | Tag |
|---|---|---|---|
| 0x862800 | DCB reserve callback (buffer, N, cb); every emitter calls it through cb+0x18 | PCR_CURSOR_RESERVE, NATIVE_CURSOR_RESERVE | proven (hook doc) |
| 0x92140 | render-target set binder (colour via 0x85cbc0, depth via 0x86a290) | PCR_CAP_RT_SET (pc_cursor) | proven (hook doc) |
| 0x85cbc0 | SetRenderTarget: colour slot -> target id, emits CB_COLOR block via 0x86a1a0 | PCR_CAP_SETRT (Return) | proven (E-10) |
| 0x85c9b0 | command-buffer reset (replays the primitive type, pc_prim) | none | proven (pc_prim doc) |
| 0x932a0 | target clear (Kind::Clear for colour; depth via DB_DEPTH_CLEAR) | PCR_CAP_CLEAR | proven (hook) |
| 0x93960 / 0x94600 | pass functions (RT set, depth state, draws, EOP and WAIT) | PCR_CAP_PASS_BEGIN_A / _B (Continue) | proven (E-10) |
| 0x93ef0 | depth function (pass object) | PCR_CAP_DEPTH_SET | proven (hook) |
| 0x8636e0 / 0x863aa0 | GfxCmdList::Submit and the flip walker (rdi = cb) | NATIVE_RJ_DCB_SUBMIT, NATIVE_RJ_DCB_SUBMIT_FLIP, PCR_SUBMIT_WALK(_FLIP) | proven (hook) |
| 0x85d600 / 0x16f100 | async (ACE) IB submission, `sceGnmDingDongForWorkload` | NATIVE_RJ_DCB_ASYNC (0x16f100) | proven (hook) |
| 0x1222e0 | render-job draw entry (NATIVE_RJ_DRAW) | NATIVE_RJ_DRAW | proven (hook) |
| 0x11d3d0 | render-job draw loop caller (occluder pass) | NATIVE_RJ_DRAW_LOOP | proven (hook) |

### 3.8 Candidates from the byte scan, not yet decoded (inferred)

| Packet | Scan result (.text) | Note |
|---|---|---|
| INDIRECT_BUFFER 0x3f | 3 hits, one function in range: 0x86ec50 (near the EOP emitter) | Row 21 of gpu-command-inventory is "unknown". 0x86ec50 is the first candidate. Not hooked. |
| COPY_DATA 0x40 | 4 hits: 0x785210, 0x85d3d0, 0x86e2d0, 0x950850 | Row 10 is "unknown". 0x85d3d0 is near the async path. Decode needed. |
| DRAW_INDIRECT 0x24 | 9 hits: 0x7a8330 (3), 0xfc140, 0x20aa70, 0x655bc0, 0x7a87c0, 0xbd57d0, 0xbedcb0 | Not in the draw-object list. Could be data or a second draw path. |
| COND_EXEC 0x22 | 3 hits: 0x865500, 0x86dc70, 0x882420 | 0x86dc70 is a state writer (DB_*), so likely a false positive. |
| SET_PREDICATION 0x20 | 4 hits: 0x92be30 (2), 0x86d950, 0x86d9e0 | 0x86d950 / 0x86d9e0 are state writers. Only 0x92be30 is new. |
| RELEASE_MEM 0x49 | 209 hits, mostly noise | Not usable. Verify by decompile of candidate writers only. |
| SET_UCONFIG_REG 0x79 | 284 hits (scan incomplete at 0x87b489) | Noise likely in code. Reliable UCONFIG writers are 0x86a900 and the 0x86a94e site only. |
| SET_SH_REG 0x76 | 15 hits: 0x869450..0x8696e8 (7), 0x86d2e0, 0x86d380, 0x86d420, 0x86d4c0, 0x86d550, 0x86d5e0 | The 0x869450-0x8696e8 cluster (FUN_00869450, FUN_00869630 with 7 hits) has no hook and no decode. Gap G6. |
| EVENT_WRITE 0x46 / ACQUIRE_MEM 0x58 | ACQUIRE_MEM hits at 0x86c170, 0x86e410, 0x883f80, 0x8840d0 | Cache-coherency packets. No CPU effect (gpu-command-inventory row 17). |

## 4. Hook coverage (pc/native, by address)

Hook names are those in `scripts/hooks.d/pc_*.py` (SOTC_ENGINE=pc) and `native_bridge.py` (SOTC_ENGINE=native).
Addresses are the EU 1.01 expected values.

| Address | Hook(s) | Mode | Switch |
|---|---|---|---|
| 0x86ac10 | PCR_CUR_DRAW_AUTO | entry, Return | pc only |
| 0x86b150 / 0x86b3d0 | PCR_CUR_DRAW_INDIRECT / PCR_CUR_INDEX_INDIRECT | entry, Return | pc only |
| 0x86b040 | PCR_CUR_DRAW_OFFSET | entry, Return | pc only |
| 0x86b9e0 / 0x883870 | PCR_CAP_DISPATCH / PCR_CAP_DISPATCH_IMP | entry, Return | pc only |
| 0x93690, 0x94d00, 0x94dd0, 0x94ec0, 0x10add0, 0xdb620 | PCR_DRAW_AUTO_RECT, PCR_DRAW_OFFSET, PCR_DRAW_OFFSET_INSTANCED, PCR_DRAW_AUTO, PCR_DRAW_INDIRECT, PCR_DRAW_INDEX_INDIRECT | entry | pc only |
| 0x86ad40, 0x86bf50, 0x86bed0, 0x86a900, 0x86c000 | PCR_SHADOW_SET_BASE / _INDEX_BASE / _INDEX_TYPE / _PRIM / _INSTANCES | entry, Continue | pc only |
| 0x86a900 site 0x86a94e | PCR_PRIM_WRITE | site | pc only |
| 0x86d5e0, 0x86d550, 0x86d4c0, 0x884240 | PCR_SGPR_WRITE, _WRITE1, _WRITE64, PCR_SGPR_IMPORT | entry, Continue | pc only |
| 0x86d380, 0x86d2e0, 0x86d420, 0x8841d0, 0x884360, 0x884050, 0x884150, 0x8840d0, 0x8832b0, 0x883180 | PCR_SGPR_DIRECT8, _DIRECT4A, _DIRECT4B, _IMPORT2, _IMPORT1, _IMPORT4A, _IMPORT4B, _IMPORT8, _IMPORT_ABSN, _IMPORT_ABS1 | entry, Continue | pc only (group pc_sgpr_writers) |
| 0x8663e0, 0x86a440, 0x86a590 | PCR_WRITE_DATA_A/B/C; NATIVE_PM4_WRITE_DATA_A/B/C | entry, Continue | pc and native |
| 0x86a700 | PCR_CAP_DMA; NATIVE_PM4_DMA | entry, Return / Continue | pc and native |
| 0x86eb60 | PCR_CAP_EOP; NATIVE_JOB_EOP | entry, Return | pc and native |
| 0x925c0 | PCR_CAP_CSDONE; NATIVE_JOB_CSDONE | entry, Return | pc and native |
| 0x86edc0 / 0x86eec0 | PCR_CAP_WAIT / _WAIT_B; NATIVE_JOB_WAIT / _WAIT_B | entry, Return | pc and native |
| 0x884d40 | PCR_CAP_CW_ADDR | entry, Return | pc only |
| 0x86ea50 / 0x883300 | PCR_CAP_COND_A / _COND_B | entry, Return | pc only |
| 0x86e770 / 0x86e800 / 0x85cea0 / 0x85d260 | pc_predication hooks | entry | pc only |
| 0x862800 | PCR_CURSOR_RESERVE; NATIVE_CURSOR_RESERVE | entry | pc and native |
| 0x8636e0 / 0x863aa0 | PCR_SUBMIT_WALK / _FLIP; NATIVE_RJ_DCB_SUBMIT / _FLIP | entry, Continue | pc and native |
| 0x16f100 | NATIVE_RJ_DCB_ASYNC | entry | native only |
| 0x1222e0 | NATIVE_RJ_DRAW | entry | native only |
| 0x92b50 | NATIVE_JOB_VIEW_COPY (name / address mismatch, see 1) | entry | native only |
| 0x85cbc0 / 0x869f50 / 0x932a0 / 0x93960 / 0x94600 / 0x93ef0 / 0x86a290 | PCR_CAP_SETRT, PCR_CAP_DEPTH_CLEAR, PCR_CAP_CLEAR, PCR_CAP_PASS_BEGIN_A/B, PCR_CAP_DEPTH_SET, PCR_CAP_DEPTH_BASE | entry | pc only |
| 0x869350 ... 0x86e560 (state set) | pc_draw_state, pc_draw_state2 (PCR_STATE4_*) | entry, Continue | pc only |
| 0x862690 / 0x869290 | PCR_STATE4_CB_INIT / _HW_DEFAULTS | entry | pc only |

The `native` render-job hooks (NATIVE_RJ_*) and the `pc` hooks are selected by different switches
(SOTC_ENGINE value), so one run uses one set. Coverage is per address, not per run: the candidates in section 3.8
and the writers listed in section 3 with no hook (for example 0x869350 family members not in the state groups,
0x86dba0..0x86dd80, the SH cluster 0x869450..0x8696e8, and the SET_CONFIG_REG writers) have no hook in either set.

## 5. How shadPS4 consumes each packet today

All references are `gpu/shadps4/video_core/amdgpu/liverpool.cpp` on `direct/vulkan` unless stated.

| Packet | Consumer (line) | What it does | Rasterizer / state call |
|---|---|---|---|
| CONTEXT_CONTROL, CLEAR_STATE | 1312, 1315 | accepted | none |
| SET_CONFIG_REG 0x68 | 1320 | memcpy into `regs.reg_array[ConfigRegWordOffset + off]`, `MarkRegs` | none (read on draw) |
| SET_CONTEXT_REG 0x69 | 1328 (CB/DB base heuristic 1343) | memcpy into context register array, `MarkRegs`; CB/DB base writes read NOP-hint for dims | none (read on draw) |
| SET_SH_REG 0x76 | 1402 | offset 0x200.. goes to `mapped_queues[GfxQueueId].cs_state` (compute); else `reg_array[ShRegWordOffset + off]` | none |
| SET_UCONFIG_REG 0x79 | 1420 | memcpy into `reg_array[UconfigRegWordOffset + off]` | none |
| SET_PREDICATION 0x20 | 1427-1431 | logs "Unimplemented" once; no state change | none (gap G3) |
| INDEX_TYPE / INDEX_BASE / INDEX_BUFFER_SIZE / NUM_INSTANCES / SET_BASE | 1435, 1657, 1664, 1651, 1670 | register state | none |
| DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2 | 1441, 1465 | draw state, then draw | SubmitDraw(is_indexed, offset) (779) |
| DRAW_INDEX_AUTO 0x2d | 1488-1506 | `regs.num_indices`, `draw_initiator`, then `rasterizer->ScopeMarker("gfx:...")` around `SubmitDraw(false, 0)` | `SubmitDraw` (779): back-threaded path, `rasterizer->IsDrawFilteredOut`, `LookupGraphicsPipeline`, BackOp, or `rasterizer->Draw` |
| DRAW_INDIRECT / DRAW_INDEX_INDIRECT (+ Multi, CountMulti) | 1507, 1546, 1526, 1566, 1586 | args from guest memory | `SubmitDrawIndirect` (805) |
| DISPATCH_DIRECT 0x15 | 1609-1628 | `cs_program.dim_x/y/z`, `dispatch_initiator`; runs only if initiator bit 0 is set | `SubmitDispatch(false,...)` (841) |
| DISPATCH_INDIRECT 0x16 | 1630 | args from memory | `SubmitDispatch(true, ...)` (841) |
| EVENT_WRITE 0x46 / EVENT_WRITE_EOS / EVENT_WRITE_EOP 0x47 | 1676, 1705, 1725 | writes fences/labels (`WriteFenceValue`, 862) | none |
| DMA_DATA 0x50 | 1801 | guest-to-guest copy or fill | none (memory) |
| WRITE_DATA 0x37 | 1847 | memory write | none (memory) |
| COPY_DATA 0x40 | 1867 | not implemented in graphics path | none |
| MEM_SEMAPHORE, ACQUIRE_MEM | 1877, 1890 | no-op / accepted | none |
| WAIT_REG_MEM 0x3c | 1905 | polls a memory location | none |
| INDIRECT_BUFFER 0x3f | 1924 | executes a nested buffer (ProcessGraphics recursion) | none |
| COND_EXEC 0x22 | 2005 | conditional skip of dwords | none |
| Compute (ACE): SET_SH_REG 1402-equivalent 2221, DISPATCH_DIRECT 2245, DISPATCH_INDIRECT 2271, WRITE_DATA 2290, WAIT_REG_MEM 2323, RELEASE_MEM 2332, EVENT_WRITE 2372 | `ProcessCompute` (2071) | compute queue state and dispatch | `SubmitDispatch` |
| Submit entry | `SubmitGfx` (2424) -> `ProcessGraphics` (1220); `SubmitAsc` (2443) -> `ProcessCompute` (2071) | frame's DCB and ACE IB | - |

Important: the emulated path (`ProcessGraphics`) decodes *every* packet in the list, including the ones the game
wrote with literal headers in the eboot. The typed stream must therefore produce the same register array updates
(`regs.reg_array` through `MarkRegs`) for every state record, or the rasterizer will see stale state.

## 6. Proposed typed records (for step 2)

Record list, grouped. Each record carries the source address and the packet it replaces, so the order and the
register effect can be checked against `ProcessGraphics`.

| Record | Replaces (address) | Fields | Consumer |
|---|---|---|---|
| `SetContextRegs{first_reg, values[]}` | 0x869350, 0x869a50, 0x869990, 0x869ce0, 0x869d60, 0x869bb0, 0x869b50, 0x869dc0, 0x86e1a0, 0x86d7b0, 0x86d890, 0x86d950, 0x86db40, 0x86a1a0, 0x86a290, 0x86bc00 ... (section 3.3) | register numbers and values | writes `regs.reg_array` (line 1328 semantics) |
| `SetConfigRegs{first_reg, values[]}` | 0x86943c, 0x869530, 0x883140, 0x88322d | as above | line 1320 |
| `SetUconfigRegs{first_reg, values[]}` | 0x86a900 (reg 0x242), site 0x86a94e | as above | line 1420 |
| `SetShRegs{stage, first_reg, values[]}` | 0x86d5e0, 0x86d550, 0x86d4c0, 0x86d380, 0x86d2e0, 0x86d420, 0x884240 and the import writers (section 3.4) | stage (VS, PS, CS), slot, data | line 1402 (compute shadow for 0x200+) |
| `SetVsShader{addr, regs}` / `SetPsShader{addr, regs}` | 0x85fc10 (+ 0x86c2c0 / 0x86cb00 / 0x86cb60) | shader address and register set | line 1402 path, then shader lookup |
| `SetUserData{stage, slot, values}` | SGPR writers (same as SetShRegs) | | |
| `SetPrimitive{type}` | 0x86a900 | `esi & 0x3f` | UCONFIG 0x242 |
| `SetBase{index, address}` | 0x86ad40 | index 1 = indirect args | line 1670 |
| `IndexBase{address}`, `IndexType{u16/u32}`, `NumInstances{n}` | 0x86bf50, 0x86bed0, 0x86c000 | | lines 1657, 1435, 1651 |
| `DrawIndexAuto{count, initiator, flags}` | 0x86ac10 (import 0xf90416), 0x94ec0, 0x93690 | | `SubmitDraw(false, 0)` (779) |
| `DrawIndexOffset{offset, count, instances}` | 0x86b040, 0x94d00, 0x94dd0 | | `SubmitDraw(true, offset)` |
| `DrawIndirect{args_addr, offset, stride, count}` | 0x86b150, 0x10add0 | | `SubmitDrawIndirect` (805) |
| `DrawIndexIndirect{args_addr, offset, ...}` | 0x86b3d0, 0xdb620 | | `SubmitDrawIndirect(true, ...)` |
| `DispatchDirect{x, y, z, initiator}` | 0x86b9e0 (import 0xf903e6), 0x883870 | | `SubmitDispatch` (841) |
| `DispatchIndirect{args_addr, offset}` | 0x86bac0, 0x883950, 0x8838f0, 0x86ba60 | | `SubmitDispatch(true, ...)` |
| `WriteData{dst, values[], dst_sel}` | 0x8663e0, 0x86a440, 0x86a590 | | line 1847 semantics (memory) |
| `Dma{src, dst, bytes, src_sel, dst_sel}` | 0x86a700 (and 0x883640) | | line 1801 |
| `EventEop{address, value, data_sel}` | 0x86eb60, 0x925c0 (cull label), 0x90e30 | | line 1725 |
| `WaitMem{address, ref, mask, func}` | 0x86edc0, 0x86eec0 | | line 1905 |
| `Predicate{address, op}` / `CondExec{address, size}` | 0x86e770, 0x86e800, 0x85cea0, 0x86ea50 | | line 1427 (unimplemented), 2005 |
| `Nop{marker}` | inline NOP 0x10 and the 20-byte marker (gpu-command-inventory row 18) | | line 1261 |
| `IndirectBuffer{address, size}` | candidate 0x86ec50 | | line 1924 |

Two design points for step 2 follow from section 3:
- Class A writers can be replaced at the emitter (Entry hook, Return, append the typed record) because their
  header is in the eboot and the bytes are known.
- Class B wrappers cannot be replaced at 0x86ac10 / 0x86b9e0 alone: the cursor is not an argument of the import
  stub (see G1). The seam is either the import handler, or the draw-object layer (0x93690, 0x94d00, 0x94dd0,
  0x94ec0, 0x10add0, 0xdb620, E-10 section 6) which already has the context and the material.

## 7. Gaps and next steps

- **G1 (blocking for step 2).** The import stubs 0xf90416 (`push 0x108`) and 0xf903e6 (index not yet read) jump to
  the common dispatcher 0xf8f380, which jumps through a pointer slot at 0x015fffb0 (proven). The cursor is not in
  the arguments (rdi = flags word, rsi = count). The NID behind index 0x108 and the dispatch index must be found
  in the eboot import table (the port resolves imports by NID, CLAUDE.md "Import binding"). Then read the host
  implementation (`gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`: `sceGnmDrawIndexAuto` at line 476 takes a
  `cmdbuf`, `sceGnmDispatchDirect` at line 376 writes `WriteHeader` at line 382). Until then the write target of
  the packet is unknown.
- **G2.** The DRAW_INDEX_AUTO (0x2d) and DISPATCH_DIRECT (0x15) headers are absent as immediates in `.text`
  (proven). Consistent with G1.
- **G3.** SET_PREDICATION / COND_EXEC: the predicate source (occlusion or memory) is not decoded. Which query type
  the game uses is still open (gpu-command-inventory gap 1).
- **G4.** INDIRECT_BUFFER: candidate 0x86ec50 (one hit). Verify by decompile and xref count.
- **G5.** COPY_DATA: candidates 0x85d3d0, 0x86e2d0, 0x785210, 0x950850. Verify.
- **G6.** SET_SH_REG cluster 0x869450..0x8696e8 (FUN_00869450, FUN_00869630, 7 literal hits). Not hooked. Find its
  caller and role (it may be the 0x869xxx state-table group or a user-data table).
- **G7.** SET_CONFIG_REG writers at 0x86943c, 0x869530, 0x883140, 0x88322d: owner not decoded.
- **G8.** WRITE_DATA 0x8663e0 has zero static callers. Find the computed call site (vtable or table) that
  reaches it, or confirm it is dead for this build.
- **G9.** Name/address mismatch: `NATIVE_JOB_VIEW_COPY` registered at 0x92b50 (native_bridge.py line 216-221). Check
  whether this is a stale entry before a native run.
- **G10.** Caller completeness: 0x86b9e0 (60 rows, 3 omitted), 0x86edc0 (39 rows, 1 omitted). Use per-caller
  decompiles if an exact count is needed.
- **G11.** The 0x86b040 caller list: 0x133750, 0x1338d0, 0x133e20 (in `pc_draw.py`) are not in the current REA
  result. Re-check with `xrefs` before relying on the group-draw inventory.

## 8. Next actions (not executed here)

1. Read the NID for import index 0x108 and the dispatch index in 0xf903e6 from the eboot import table; map to the
   gnmdriver function (G1). This is a single lookup and decides whether draw/dispatch are a host-side seam.
2. Decompile 0x86ec50, 0x85d3d0, 0x869630 (G4, G5, G6) with `procedure_pseudo_code`.
3. Confirm the `NATIVE_JOB_VIEW_COPY` address (G9) with the decompile of 0x92b50 before any native run.
