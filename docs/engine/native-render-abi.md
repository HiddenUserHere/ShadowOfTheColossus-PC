# Descriptor / SGPR ABI (Phase 0.3)

Branch `native/render-vk13`. This confirms the guest→shader ABI against the port's own bindings,
so the native backend can feed the *same* values the port feeds, keeping translated shaders
bit-identical (the recompiler is kept unchanged).

## 1. SGPR / user-data: the guest side

The GNM library uploads per-stage user SGPRs through three writers, all proven in
`out/re/decomp`:

| Writer | VA | Packet | Body |
|---|---|---|---|
| `0x86d550` | per-stage, one dword | `0xC0017600` (stage 0: `0xC0017601`) | reg `param3-0x2c00+DAT_01290410[stage]`, value |
| `0x86d4c0` | per-stage, 64-bit | `0xC0027600` (stage 0: `0xC0027601`) | reg `param3-0x2c00+DAT_01290410[stage]`, lo/hi |
| `0x86d5e0` | per-stage, N dwords | `0xC0001000` + `0xC0007600` | reg `param3-0x2c00+DAT_01290410[stage]`, memcpy N |

`DAT_01290410` (dumped from the eboot at VA `0x1290410`) is the per-stage SH-register base:

| stage | base | meaning |
|---|---|---|
| 0 | `0x2e40` | VS (SPI_SHADER_PGM_*) |
| 1 | `0x2c0c` | PS user data |
| 2 | `0x2c4c` | (stage 2) |
| 3 | `0x2c8c` | (stage 3) |
| 4 | `0x2ccc` | (stage 4) |
| 5 | `0x2d0c` | (stage 5) |
| 6 | `0x2d4c` | (stage 6) |
| 7+ | — | unused |

So the user-data window for stage `s` starts at SH reg `0x2c00 + DAT_01290410[s] - 0x2c00`. The
`+ (stage==0)` in the header selects predicate bit (stage 0 = graphics) and the VS window.

The **start-vertex / base-instance SGPRs** are written by `0x862510` (shared draw preamble): by
draw kind it picks a descriptor (`+0xc0c0`/`+0xc0d0`/`+0xc0b0`), reads byte `+0x19`, splits into
nibbles (`low` = vertex SGPR offset, `high` = instance SGPR offset) and calls `0x86d550` per
nonzero nibble. The draw kind itself comes from `cb+0xc548` (§7 of the master map).

## 2. SGPR / user-data: the port side

- `AmdGpu::NUM_USER_DATA = 16`; `UserData = std::array<u32,16>` (`regs_shader.h:12-15`).
- Each `ShaderProgram` has `settings.num_user_regs : 5` and `UserData user_data` (`regs_shader.h:70-74`).
- The recompiler emits `EmitGetUserData` as a **flatbuffer load**: `ud_reg = flatbuffer[reg]`
  (`emit_spirv_context_get_set.cpp:49-53`). The flatbuffer is the SRT walker's output: it flattens
  the guest user-data block (16 dwords) plus the descriptors the shader dereferences, indexed by
  dword (`srt.h:35-37`, `PersistentSrtInfo::flattened_bufsize_dw`, default 16 = NumUserDataRegs).
- `num_user_regs` is written into the SPIR-V module as `info.props.num_user_data`
  (`vk_pipeline_cache.cpp:126` / `:272`), which becomes `PushData` push constants + the descriptor
  push at `pipeline->BindResources` (`vk_rasterizer.cpp:84`).

**Conclusion:** the guest's per-stage user SGPR window is exactly the 16-dword `user_data` array the
port already binds. The native backend must produce, per stage, the same 16 dwords the guest's
`0x86d550`/`0x86d4c0`/`0x86d5e0` calls write (start-vertex, base-instance, root-table pointer, view
constants pointer, material/object block pointers), and hand them to the same push-constant /
flatbuffer path. **No recompiler change is needed** — the shader's `GetUserData(reg)` reads the same
dword index regardless of who wrote it.

## 3. Resource descriptors (V# / T# / S#)

The guest never builds per-draw descriptors on the CPU (bindless by construction): shaders read
V#/T#/S# through `s_load` from a pointer passed in user SGPRs (the root table `0x95320` builds
from the masked `ctx+0x90..` window; `0xae150` binds it). The port decodes those sharp descriptors
in the SRT walker (`srt.h`, `resource_patching_pass.cpp`, `flatten_extended_userdata_pass.cpp`) and
binds them as SPIR-V `UniformConstant` images (`spirv_emit_context.cpp:1052-1061`) or storage
buffers. The native backend feeds the same guest pointers, so the walker and the sharp decoders are
reused unchanged.

## 4. Verification status

- The writers `0x86d550`/`0x86d4c0`/`0x86d5e0` and the stage table `DAT_01290410` are **proven**
  (decompile + raw eboot bytes).
- The port's `UserData`/flatbuffer binding is **proven** (source above).
- The quad-swizzle / WQM workarounds (`SOTC_QUAD_SWIZZLE`, `SOTC_WQM_COPY`) are recompiler-side and
  unchanged by the render rewrite, so they are out of scope for this phase (they were only
  re-checked against the ISA in the plan; the recompiler is kept bit-identical, so they stay valid).
- CLRadeonExtender 0.1.9 is durable at `tools/clrx/` (`clrxdisasm.exe`, `clrxasm.exe`). The raw
  shader blobs (`user/csdr/csdr_in.bin`, `program_files.tsv`) are **not present in this worktree**
  (they are runtime artifacts), so an ISA-level re-check of a specific heavy draw is deferred to
  Phase 2, when a live capture produces the blobs. This does **not** block Phase 1/2: the ABI is
  proven from the writers + the port bindings, and the recompiler is unchanged.
