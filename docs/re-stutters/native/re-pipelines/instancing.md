# Instance step rates and the VS SGPR offsets (RE, topic D2)

Scope: VGT_INSTANCE_STEP_RATE_0/1 (context regs 0xA2A8 / 0xA2A9), the instanced draw paths, the vertex and
instance user-SGPR offsets of the vertex shader, and what the port's shader key does with them.
Evidence: Ghidra decompile in `out/re/decomp/<4 hex>/<addr>.c` (first line lists callers), `tools/re/eboot_re.py`
(`callers`, `refs`), the eboot image `out/eboot.elf`, and the port source. Addresses are eboot offsets.
Labels: **proven** = read in code or data; **inferred** = reasoning, not run.

Read-only: no source, build, game run or git change.

---

## 0. Summary

1. **Step-rate register writer.** One writer exists: `FUN_0086BB90` (`0x86BB90`, 99 bytes). It writes
   `0xC0026900, 0x2A8, v0, v1` (SET_CONTEXT_REG, count 2), i.e. VGT_INSTANCE_STEP_RATE_0 = v0 and _1 = v1.
   **It has no static callers.** No other code writes 0xA2A8/0xA2A9 that the decompile or the tables show.
   **Inferred: the eboot never writes the step rates on any path I can see, so both stay at the reset value.**
   This is the most important finding and needs a runtime check (section 6).
2. **Instancing is not done with the step rate.** The engine sets the instance count with NUM_INSTANCES
   (`0xC0002F00`, writer `0x86C000`) and draws with DRAW_INDEX_OFFSET / DRAW_INDEX_AUTO (non-indexed, no instance
   field) or with indirect draw packets (instance count and start instance in memory). Instance data for list-driven
   draws is a vertex-buffer V# in user-data slot 7 (`0x133BD0`), read by the VS fetch.
3. **Indirect draws take their SGPR offsets from a per-program byte, not from the draw.** One byte of the bound
   shader record (low nibble = vertex, high nibble = instance user-SGPR index) is passed to `sceGnmDrawIndirect` /
   `sceGnmDrawIndexIndirect`. **Inferred:** these are constants per bound VS, so each VS has a fixed pair, and the
   pair is 0/0 when the VS has no base vertex or instance SGPR.
4. **Port side.** `step_rate_0/1` and both SGPR offsets are compared as a whole in `SwVertexRuntimeInfo::operator==`,
   so every VS permutation splits on step rates even when no attribute is instance-rate. The step rate only reaches
   the pipeline through the attribute divisor (`VsAttribSpecialization.divisor`, set only for instance-rate
   attributes). The proposal in section 5 removes the redundant compare, and it must also change where the
   non-dynamic pipeline reads its divisors.

---

## 1. The writers of VGT_INSTANCE_STEP_RATE_0 / _1

### 1.1 Register identity (proven)

- Port: `gpu/shadps4/video_core/amdgpu/regs.h:132-133` (`vgt_instance_step_rate_0/1` in `AmdGpu::Regs`), and
  `gpu/shadps4/video_core/amdgpu/regs.cpp:109-110` `static_assert(GFX6_3D_REG_INDEX(vgt_instance_step_rate_0) == 0xA2A8)`,
  `..._1 == 0xA2A9`. The index is the absolute dword index (context base 0xA000 included); the byte address is
  0x28AA0 / 0x28AA4. The PM4 packet offset field is the index minus 0xA000, so 0x2A8 / 0x2A9.
- Eboot setter convention (proven): `FUN_00869350` (`out/re/decomp/0086/00869350.c`) writes
  `0xC0016900, addr - 0xA000, value`. The packet-builder style is `[cb+0x10]` cursor, free-space check on `[cb+8]`,
  flush callback at `[cb+0x18]`, the same as the 80 setters in `render-states.md`.

### 1.2 The writer (proven code, callers unknown)

| Function | Address | Size | Packet | Values |
|---|---|---|---|---|
| `FUN_0086bb90(cb, v0, v1)` | `0x86BB90` | 99 | `0xC0026900`, offset `0x2A8`, `v0`, `v1` | VGT_INSTANCE_STEP_RATE_0 = v0, _1 = v1 |

Decompile (`out/re/decomp/0086/0086bb90.c`), the store part:
```c
*puVar2 = 0xc0026900;
puVar2[1] = 0x2a8;
puVar2[2] = param_2;
puVar2[3] = param_3;
```

Callers and references, all checked:
- `tools/re/eboot_re.py callers 0x86bb90`: no caller (the output is only the function name).
- `tools/re/eboot_re.py refs 0x86bb90`: no RIP-relative or data reference.
- Decompile: the text `86bb90` appears only in its own file. Its neighbour `FUN_0086bb30` (writes 0x2AD) also has zero
  direct callers, so the decompile call index is probably missing indirect call sites in this area.
- Raw byte scan of `out/eboot.elf` for the 4-byte and 8-byte value 0x86BB90: no hit. **Weak evidence only:** relocated
  pointers may keep their value in the RELA addend and not in the data bytes.

**Inferred:** `0x86BB90` is either dead code in this build, or called through a pointer or a dispatch that the static
index does not resolve. I could not tell which from the eboot alone.

### 1.3 Other possible routes (checked, none found)

- Generic writers `0x869350` (one dword), `0x8694E0` (block), `0x869370` (4-bit field): zero call sites in the decompile.
- Table writers `0x86D080` (`DAT_0128FED0`, two-register packet `0xC0026900`) and `0x86E3A0` (`DAT_0128FEE0`): the tables
  are read from the eboot image (`out/eboot.elf`, file offset = vaddr - 0x128FED0 + 0x1293ED0). Scanned 0x128FED0..0x1290200 for
  0xA2A8, 0xA2A9, 0x2A8, 0x2A9: no hit. `DAT_01290050` (UCONFIG table) has no hit either.
- Literal index in a store (`puVar[1] = 0x2a8` or `0xa2a8`): only in `0x86BB90`. Literal `0xa2a8` as an argument: none.
- `sceGnm*` state imports: `render-states.md` already notes the eboot does not import state writers, so no SDK call sets
  the step rate.

**Values written:** none known. Only `0x86BB90` writes the registers, and it has no caller.

### 1.4 Reset value (inferred)

The port zeroes the context on clear (`ClearContextState`, `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`,
used by the `sceGnmDrawInitDefaultHardwareState*` family, lines 937-1052). The SDK default context, which the port
replays from its own code, is not in the repository, so the reset value of the step rate is **inferred 0**.

---

## 2. The instanced draw paths

### 2.1 Draw packet wrappers (proven)

Eboot wrappers, each calling an import stub (the stub header lists its callers, `out/re/decomp/00f9/`):

| Wrapper | Import | Packet (port `pm4_opcodes.h`) | Instance count | Callers (decompile) |
|---|---|---|---|---|
| `0x86AC10(cb, indexCount, flags)` | `sceGnmDrawIndexAuto` (`0xF90410`) | DrawIndexAuto `0x2D` | none (from NUM_INSTANCES) | `0x93690`, `0x94EC0` |
| `0x86ABB0(cb, indexCount)` | same | same | none | none |
| `0x86B040(cb, indexOffset, indexCount, flags)` | `sceGnmDrawIndexOffset` (`0xF90450`) | DrawIndexOffset2 `0x35` | none | 9: `0x133E20`, `0x1A8C60`, `0x1338D0`, `0x94DD0`, `0x133BD0`, `0x133550`, `0x133750`, `0x133A50`, `0x94D00` |
| `0x86AC80(cb)` | `sceGnmDrawOpaqueAuto` (`0xF904A0`) | DrawOpaqueAuto | none | `0x86AC80` is called by none in decompile |
| `0x86A9D0(...)` | `sceGnmDrawIndex` (`0xF90400`) | DrawIndex (inline indices, copies the index data) | none | none for `0x86A9D0` itself. The stub `0xF90400` is also called by `0x86AAB0`, `0x86ADC0`, `0x86AE30`. |
| `0x86B150(cb, dataOffset, stage, vsSgpr, isSgpr, flags)` | `sceGnmDrawIndirect` (`0xF90460`) | DrawIndirect `0x24` | from memory | `0x10ADD0` |
| `0x86B3D0(cb, dataOffset, stage, vsSgpr, isSgpr, flags)` | `sceGnmDrawIndexIndirect` (`0xF90420`) | DrawIndexIndirect `0x25` | from memory | `0xDB620`, `0x1A6D20`, `0x1A7290` |
| `0x86B290`, `0x86B1F0` | `sceGnmDrawIndirectCountMulti` (`0xF90470`) | `0x2C` | from memory | not traced |
| `0x86B470`, `0x86B510` | `sceGnmDrawIndexIndirectCountMulti` (`0xF90430`) | `0x9D` | from memory | not traced |
| `0x86AEB0`, `0x86AF30` | `sceGnmDrawIndexMultiInstanced` (`0xF90440`) | multi-instanced | caller | not traced |

Port implementations of the same SDK calls: `gnmdriver.cpp:744` (`sceGnmDrawIndexAuto`), `:759` (`sceGnmDrawIndexIndirect`),
`:860` (`sceGnmDrawIndexOffset`), `:878` (`sceGnmDrawIndirect`). `sceGnmDrawIndexMultiInstanced` appears to be a stub at
`gnmdriver.cpp:854` (no arguments, not checked further).

### 2.2 NUM_INSTANCES writer (proven)

`FUN_0086C000(cb, n)` (`out/re/decomp/0086/0086c000.c`): `0xC0002F00, n` (SET of NUM_INSTANCES, opcode `0x2F`, the
port's `PM4CmdDrawNumInstances`, `liverpool.cpp:4733`). Direct callers: `0x94DD0`, `0xDB620`, `0x10ADD0`, `0x11D7A0`,
`0x133550`, `0x133750`, `0x1338D0`, `0x133A50`, `0x133BD0`, `0x133E20`, `0x85C9B0`, plus `0x10ADD0` (after the draw).

Persistence: NUM_INSTANCES is not reset per draw. Draws that do not write it (`0x94D00`, `0x94EC0`, `0x86AC10`
callers) inherit the last value. **Proven** by the code: `0x94D00` and `0x94EC0` do not call `0x86C000`.

### 2.3 Instanced direct draws (proven code; content names inferred)

| Function | NUM_INSTANCES | Draw | Instance data | Notes |
|---|---|---|---|---|
| `0x133A50(p1, p2, obj, flags)` | `*(obj+0x28)` | DrawIndexOffset(0, `*(*(model+0x80)+0x48)`) | none extra | Called by `0x12A930` mode 1 (light probe, string "Showing light probes from %s") and mode 2 (a tree of objects, each with flag `+5 & 4 == 0`). |
| `0x133BD0(p1, p2, obj, flags)` | count of the copied list = `(end-begin) >> 2` | DrawIndexOffset(0, same index count) | list copied out of `obj+0xE0` by `0x135450`; V# built by `0x8BE60(sharp, 0x1B, 4, count, 0, list, 0)` and bound with `0x1A6440(..., 7, ...)`. Obj `+0x08` and `+0x30` are bound to slots 5 and 6. | Four-byte entries (count = bytes / 4). Called by `0x12A930` mode 4 with list at `p1+0x178`. |
| `0x135450` | - | - | - | Takes the list at `obj+0xE0` (via `0x1354F0`) under a mutex at `obj+0xD8` (`scePthreadMutexLock`). Only caller is `0x133BD0` in the decompile. Producer of the list not found. |
| `0x133E20(p1, p2, ..., p4, ...)` | `p4` (caller) | DrawIndexOffset(0, count from `*(model+0x80)+0x48`) | V# bound with `0x1A6430` (slot 4) and slot 5 from `p5` | No caller in the decompile. |
| `0x133550`, `0x133750`, `0x1338D0` | 1 | DrawIndexOffset(0, ...) | `0x133550` binds a V# through `0x861FE0(..., 7, ...)` (meaning not decoded) | Single instance. |
| `0x94DD0(p, kind, ..., idxOff, idxCount, inst)` | `inst` (param 7) | DrawIndexOffset(idxOff, idxCount) | none | Primitive type from `DAT_00F9F2E0[kind]` (`0x86A900`). |
| `0x94D00(p, kind, ...)` | not written | DrawIndexOffset(...) | none | Caller `0x128D20` (draw record at `+0x38`, `+0x40`, `+0x48`). |
| `0x94EC0(p, kind, ..., count)` | not written | DrawIndexAuto(count) | none | Many callers, e.g. `0x118470`, `0xA69D0`. |

Facts about the instanced paths:
- Direct instanced draws never pass a start instance and never use a user SGPR. `DrawIndexOffset` and `DrawIndexAuto`
  have no SGPR field, so the start instance is 0 and the instance ID VGPR counts from 0.
- The instance-rate attributes of a list-driven draw (`0x133BD0`) would have to be fetched per instance from the V#
  in slot 7. Whether that happens through the step rate or through a buffer load keyed by the instance ID is decided
  by the fetch shader's per-attribute field (section 4.1). **Open:** which attributes the VS fetches with instance rate.

### 2.4 Indirect draws (proven code)

| Function | Packet | Callers | Stride of args |
|---|---|---|---|
| `0x10ADD0` (`FUN_0010add0`) | DrawIndirect via `0x86B150` | `0x109CA0` (many), `0x18E3D0` | 16 bytes (`DrawIndirectArgs`, `gpu/.../pm4_cmds.h:1080`) |
| `0xDB620` (`FUN_000db620`) | DrawIndexIndirect via `0x86B3D0` | `0x189060`, `0xDB390`, `0x10B0A0`, `0x18D6F0` | 20 bytes (`DrawIndexedIndirectArgs`) |
| `0x1A6D20` | DrawIndexIndirect | `0x19AF20` | not decoded (offsets `iVar6`) |
| `0x1A7290` | DrawIndexIndirect | `0x19AF20` | `iVar4 * 0x14` (20 bytes per entry) |

Both `0x10ADD0` and `0xDB620` write `NUM_INSTANCES = 1` before or after the packet; the real instance count and the
start instance come from memory. `0x10ADD0` and `0xDB620` pick the stage from the context field `cb+0xC548`
(section 3).

What draws these are: **not established.** The strings that are close to these paths are "InstanceType: VIRTUAL"
(`0xE1610`, caller `0x195DD0`), "Virtual Geometry - Instances - %d of %d" (`0xDA820`, callers `0xDA7D0` <- `0xCEFA0`,
`0xD05F0`), "InstanceType: Particle" (`0xFE240`, `0xFE320`) and "XPRRenderInstancePreShaders" (`0xC6810`). The link
from these functions to `0xDB620` / `0x10ADD0` is **inferred**, not traced. The "grass, foliage, rocks, particles"
question needs a runtime trace (section 6).

---

## 3. The VS SGPR offsets

### 3.1 Packet fields and the SDK conversion (proven)

Packet fields `DrawIndirect` / `DrawIndexIndirect` (`pm4_cmds.h:1088-1098`): `dw2.base_vtx_loc` (bits 0-15) and
`dw3.start_inst_loc` (bits 0-15), the register offsets where the CP writes the base vertex and start instance.

Port conversion (`gnmdriver.cpp:72` and `:878-905`):
```c
static constexpr std::array indirect_sgpr_offsets{0u, 0u, 0x4cu, 0u, 0xccu, 0u, 0x14cu};   // by Gnm shader stage
cmdbuf[1] = vertex_sgpr_offset == 0 ? 0 : (vertex_sgpr_offset & 0xffff) + sgpr_offset;   // vsSgpr = user-data index
cmdbuf[2] = instance_sgpr_offset == 0 ? 0 : (instance_sgpr_offset & 0xffff) + sgpr_offset;
```
So the SDK argument is a user-data index (0 = none), and the packet holds the index plus the stage base. VS base is
0x4C (stage index 2), ES 0xCC (index 4), LS 0x14C (index 6) (**inferred** from the stage numbering, which the port
uses in `translate.cpp:68`: `{0, 0x4c, 0, 0xcc, 0, 0x14c}` by HW stage).

### 3.2 Where the eboot gets the two indices (proven code, source of the values inferred)

`FUN_0010ADD0` (`out/re/decomp/0010/0010add0.c`) and `FUN_000DB620` (`out/re/decomp/000d/000db620.c`) have the same logic:

- `cb+0xC548` (int) selects the stage:
  - `0` -> stage 2 (VS), byte = `*(u8*)(*(cb+0xC398) + 0x27)`.
  - `0xB0` -> stage 4 (ES), byte = `*(u8*)(*(cb+0xC3A8) + 0x19)`.
  - `0x45`, `0xAD`, `0x145`, `0x1AD` -> stage 6 (LS), byte = `*(u8*)(*(cb+0xC3B8) + 0x19)`.
  - anything else: no packet (the draw is dropped, and only NUM_INSTANCES = 1 is written).
- The byte: low nibble = vertex SGPR index, high nibble = instance SGPR index (`bVar1 & 0xF`, `bVar1 >> 4`).

So the offsets are **a per-bound-shader constant** read from the shader record. The draw does not supply them.
The record fields at `+0x27` and `+0x19` are not decoded here. **Inferred:** they are the Gnm shader header's
vertex/instance user-data indices. Confidence medium.

Consequence: for one VS, indirect draws always carry the same two SGPR offsets, and direct draws carry 0/0 (no SGPR
field). So a VS used both ways makes two translations (one with and one without the SGPR copies).

### 3.3 Which draws are indirect

Indirect submitters: `0x10ADD0`, `0xDB620`, `0x1A6D20`, `0x1A7290` (section 2.4). Whether these are the GPU culling
output (cull compute writing DrawIndexedIndirectArgs) is **inferred** from the 20-byte stride and the XPR and Virtual
Geometry strings; not traced to the compute side.

---

## 4. The port side (read-only)

### 4.1 Where the step rate and the SGPR offsets enter the key (proven)

| Item | Location | What it does |
|---|---|---|
| `SwVertexRuntimeInfo { step_rate_0, step_rate_1, vertex_sgpr_offset, instance_sgpr_offset, tess_emulated_primitive }` | `gpu/shadps4/shader_recompiler/runtime_info.h:169-177` | Compared as a whole (`operator== = default`). |
| `RuntimeInfo::HasSameSwInfo`, VS case `sw.vs == other.vs` | `runtime_info.h:385-391` | Every VS permutation compare includes all five fields. |
| Fill of runtime info | `vk_pipeline_cache.cpp:337-340` (`regs.vgt_instance_step_rate_0/1`, `draw_indirect_params.vertex/instance_sgpr_offset`) | The values per draw. |
| Pre-translation default | `vk_pipeline_cache.cpp:3670-3675` | `vertex_sgpr_offset = 0`; `instance_sgpr_offset = num_input_vgprs >= 3 ? 0x4C + 2 : 0` (port guess, not from the engine). |
| Permutation compare | `vk_pipeline_cache.cpp:3518-3520` | Splits on step rate and both SGPR offsets. |
| Step rate to attribute | `specialization.h:79-91` (`FillVsAttrib`) | `spec.divisor = step_rate_0` (OverStepRate0), `step_rate_1` (OverStepRate1), else not set (0). Set only when `desc.GetStepRate() != None`. |
| Attribute spec compare | `specialization.h:306` (`vs_attribs != other.vs_attribs`), packed at `:173-175` (divisor included) | The divisor is in the key for instance-rate attributes only. |
| Instance-rate source | `shader_recompiler/frontend/fetch_shader.h:17-34` (`InstanceIdType`: None 0, OverStepRate0 1, OverStepRate1 2, Plain 3) and `fetch_shader.cpp:112` (`instance_data = word1 & 0xff`) | Per attribute, a field of the guest fetch shader. The choice of register is in guest memory, not in the registers. |
| Vulkan binding | `vk_graphics_pipeline.cpp:116-122` (non-dynamic: uses `vs_info.step_rate_0/1` of the module's runtime info) and `:818-850` (`GetVertexInputs`: `Plain` and `None` give divisor 1, OverStepRate0/1 use the step rates) | The divisor is part of the pipeline (`sdata.divisors`, `vi_key` at `:579-581`). |
| Dynamic vertex input | `vk_rasterizer.cpp:7729-7732` (`BindVertexBuffers`: uses live `regs.vgt_instance_step_rate_0/1`) | Divisors are re-applied per draw from the registers. |
| Extent check | `vk_rasterizer.cpp:7846-7850` | A divisor of 0 is treated as "no extent"; Vulkan allows divisor 0 only with a device feature (`vertexAttributeInstanceRateZeroDivisor`, not checked here). |
| SGPR use in the translator | `shader_recompiler/frontend/translate/translate.cpp:67-111` | `UdRegFromShOffset` and `SetScalarReg(BaseVertex / BaseInstance)` are emitted only when the offset is nonzero, whether or not the shader reads them. |
| Instance attribute SPIR-V | `backend/spirv/spirv_emit_context.cpp:448-453` | Instanced inputs are plain inputs with a name. The shader never reads the step rate. |

### 4.2 When the step rate matters to the translated shader (proven by the code above)

1. **The shader code never depends on the step rate.** The step rate only changes the pipeline's vertex-input
   divisor. The SPIR-V of the VS is identical for any step-rate value.
2. **The divisor matters only for attributes whose fetch-shader field is OverStepRate0 or OverStepRate1.** For all other
   attributes the divisor is 1 (`GetVertexInputs`) and the step rate is not read.
3. **Two places carry the divisor:** (a) the shader key, through `vs_attribs[i].divisor`, which is set only for
   instance-rate attributes; (b) the runtime info compare, which covers all five VS fields. The non-dynamic pipeline
   takes its divisors from (b) through the module's runtime info (`vk_graphics_pipeline.cpp:116-122`).
4. **The SGPR offsets matter to the shader code** whenever nonzero (`translate.cpp:97-111`), so they change the SPIR-V
   for a VS that does not even use the instance id.

### 4.3 Redundant comparisons (proven by the read; cost inferred)

- The compare of `step_rate_0/1` is redundant for every VS without an OverStepRate attribute, and for the instanced
  ones it is already covered by `vs_attribs[i].divisor`.
- The compare of the SGPR offsets is needed only while the translated code emits the copy. It splits direct and
  indirect draws of one VS even when the shader never reads the start instance.

---

## 5. Proposal (no code edited)

### P-SR: compare the step rate only through instance-rate attributes

1. Remove `step_rate_0` and `step_rate_1` from `SwVertexRuntimeInfo` (`runtime_info.h:170-171`), or at least from its
   `operator==` (the struct is used in `BuildRuntimeInfo` at `vk_pipeline_cache.cpp:337-338`). Then VS permutations
   split on step rate only through `vs_attribs[i].divisor` (`specialization.h:82-88`, compared at `:306`), which is
   set only for instance-rate attributes. This is the "compare the step rate only when an attribute is instance-rate"
   rule, enforced by the existing spec field.
2. **Required companion change:** the non-dynamic pipeline must take its divisors from the attribute spec, not from
   `vs_info.step_rate_0/1` of the module's runtime info (`vk_graphics_pipeline.cpp:116-122`). Otherwise a cached
   module created under one step rate would build a pipeline with the wrong divisor for a later draw. The spec
   divisor equals the step rate used when that permutation was made, because `FillVsAttrib` takes it from the same
   runtime info.
3. Keep the dynamic path (`vk_rasterizer.cpp:7729-7732`) on live registers. It is already correct per draw.
4. Verification: for a recorded set of draws, the divisors from `GetVertexInputs` must be identical before and after.
   The PRETRANSLATE miss counter (`vk_pipeline_cache.cpp:4253-4317`) must not rise.
5. Caveat: `ForEachSharp` fills `vs_attribs` only when the attribute's sharp is present (`specialization.h:240`). An
   instance-rate attribute with no sharp would have divisor 0 in the key. It has no data either way, but check it.

### P-SGPR: the SGPR offsets only when the VS reads the copied user SGPR

1. In the key, keep `vertex_sgpr_offset` and `instance_sgpr_offset` only if the translated VS reads
   `IR::Attribute::BaseVertex` / `BaseInstance` (the copy at `translate.cpp:97-111` is the only use). Otherwise
   normalize both to 0 in the key and skip the copy. This merges direct and indirect permutations of a VS that
   does not use the start instance.
2. Pre-translation (`vk_pipeline_cache.cpp:3670-3675`): replace the `0x4C + 2` guess with the value from the
   bound shader record (the byte at `+0x27` / `+0x19`, section 3.2) once that writer is decoded. Until then the guess
   is not evidence of what the engine does.

---

## 6. Open items and runtime checks

1. **Is VGT_INSTANCE_STEP_RATE ever written?** Static evidence says no. The first check: log writes to `regs` at
   `liverpool.cpp:4733`-style PM4 handling for context index 0xA2A8 / 0xA2A9 (a counter and the value), then run the
   title for one minute. If the counter stays at 0, the step rate is the reset value on every draw.
2. **Semantics of step rate 0 in hardware.** If the game uses OverStepRate0 attributes with register value 0, the port
   passes divisor 0 to Vulkan. That is only valid with the zero-divisor feature. Confirm the device feature and the
   intended hardware meaning (0 may mean "no step", i.e. 1) before the proposal changes the key.
3. **Which attributes are instance-rate.** The fetch shaders of the instanced draws (`0x133A50`, `0x133BD0`) need their
   `instance_data` bytes read at run time (`fetch_shader.cpp:112`). This tells which draws depend on the step rate at all.
4. **Indirect SGPR bytes.** Decode the writer of `record+0x27` (VS) and `record+0x19` (ES, LS) to say whether the
   instance nibble is 2 for instanced VSes.
5. **Caller of `0x86BB90`.** Candidates to check: the command-buffer dispatch (`cb+0x18` callback) and any
   function-pointer table. A dynamic trace (breakpoint on `0x86BB90`) answers this faster than static search.
6. **Content of instanced draws.** Map `0x12A930` mode numbers to the caller of `0xCEFA0` and to the strings
   (light probes, VIRTUAL, Particle). Not done.

## 7. Confidence

- Proven: register indices (regs.cpp:109-110); writer `0x86BB90` and its packet; zero static callers; NUM_INSTANCES
  writer and its callers; draw wrappers and their import stubs; the two SGPR byte sources; port compare sites and
  their line numbers; `translate.cpp` use of the offsets.
- Inferred: reset value 0 of the step rate; the eboot never writes it; the byte `+0x27`/`+0x19` meaning; the
  content of instanced draws; the zero-divisor semantics.
- Not checked: the Gnm default context blob; sceGnmDrawInitDefaultHardwareState contents; fetch shader instance
  fields of the game's VS.
