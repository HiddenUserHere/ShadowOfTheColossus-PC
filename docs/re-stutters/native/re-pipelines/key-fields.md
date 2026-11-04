# Port side: the graphics pipeline key and the shader specialization key (topic K3)

Read-only review of the port (`arch/gcn-native`, working tree as of this run). Scope: which fields make up
the key the port uses to find a Vulkan graphics pipeline, and the key it uses to find a shader permutation
(`StageSpecialization`). For each field: where the port reads it (Liverpool registers or guest memory), whether
it is baked into the pipeline or dynamic state, which game source sets it (the RE docs in this folder), and how
the port could know it before the first draw.

Labels: **proven** = read in the code, file and line given; **RE** = taken from the RE docs in this folder
(`pass-map.md`, `render-states.md`, `materials-meshes.md`, `shader-collections.md`); **inferred** = reasoning,
not run. No build, no game run, no source edit.

Source classes used in the tables:

| Class | Meaning |
|---|---|
| BOOT | fixed for the process or the device (env switches, device features, profile) |
| PROGRAM | fixed by one shader program (its binary and its descriptor registers, found by its hash) |
| PASS | set per render pass by the engine (render targets, pass constants) |
| MATERIAL | set per material and pass slot (blend/state object, sampler state, slot k) |
| TEXTURE | T# content of a bound texture (format, type, number format) |
| MESH | vertex declaration and V# of the vertex buffers of a mesh |
| DRAW | only known at the draw (draw packet, user SGPR offsets, per-draw register state, bound V#/T# values) |

---

## 1. Short answer

- The **graphics pipeline key** is `Vulkan::GraphicsPipelineKey` (`gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h:38-75`).
  It is a memcmp/XXH3 key over the whole struct (`graphics_pipelines`, `vk_pipeline_cache.h:571`). It holds six
  stage hashes (one per permutation found by `GetProgram`), the vertex formats (only when vertex input is not
  dynamic), the attachment formats, the blend words, the color write masks, the sample counts, and a few raster
  and primitive fields. It does **not** hold depth test/write/compare, stencil ops and refs, cull mode, front
  face, depth bias, viewport, scissor, blend constants, line width or index type. Those are dynamic state in this port.
- The **shader key** is `Shader::StageSpecialization` (`specialization.h:219-366`), one per permutation of a
  program. It holds `RuntimeInfo` (per-stage registers, see section 4), the resource presence bitset, the fetch
  shader, the vertex attribute specializations, and one specialization entry per buffer, image, F-mask and sampler.
  The value of each entry is read from a V#/T#/S# that the draw binds (guest memory, through the user SGPRs).
- Almost every entry is **DRAW-dependent** in the port today: the T#/V#/S# words are read at the draw. The
  pass-level and material-level parts are known from the game's own state (RE docs), but the port does not
  map a draw to its pass or material yet. Section 6 lists the proposals per field.

---

## 2. Active configuration (`sotc.ini`) and what each switch does to the keys

Read from `C:/Users/ptr/Documents/sotc-pc/sotc.ini`. Switches not listed there are at their code default.

| Switch | Value | Effect on the keys (proven unless marked) |
|---|---|---|
| `SOTC_HEAP` | 0 | Heap variant off. `SotcArch::Heap::On()` needs `SOTC_HEAP != 0` (`sotc_arch/heap.cpp:242`). All stages keep `StageVariantDefault` (`vk_pipeline_cache.cpp:2882-2885`, `2943-2959`). Hashes carry no variant. |
| `SOTC_SHADER_ADDR` | 0 | `profile.shader_addr = 0` (`vk_pipeline_cache.cpp:604`). Non-heap stages stay `StageVariantDefault` (`2882-2885`). |
| `SOTC_HEAP_VS` | 0 | Vertex heap candidate off (`heap.cpp:250`). |
| `SOTC_N5_BINDLESS` | 0 | `bindless_images` off (`vk_instance.cpp:1003-1049`, `vk_bindless.cpp:20-35`). Images stay classic descriptors. |
| `SOTC_BINDLESS_SAMP` | 0 | `bindless_samplers` off (`vk_instance.cpp:1007-1028`). |
| `SOTC_LAYOUT_UNIFIED` | 0 | Forced off (`sotc_arch/layouts.cpp:34-57`). |
| `SOTC_STAGE_BINDINGS` | unset | `profile.stage_bindings` off (`info.cpp:41-47`, `vk_pipeline_cache.cpp:595`). One descriptor set for all stages. |
| `SOTC_VS_BINDING_BASE` | 1 | The VS start bindings follow the PS bindings (`bindings.h:23-58`, `vk_pipeline_cache.cpp:2897-2899`). **This couples the VS permutation to the PS resource count** (see 4.3). |
| `SOTC_GENERIC_BUFFERS` | 2 | `FillBuffer` drops the stride of unswizzled buffers to 0, and replaces the format of untyped loads of read-only guest buffers with key value 63 (`specialization.h:93-115`, `info.h:98-153`, `185-188`). **Large reduction of the buffer part of the key.** |
| `SOTC_IMAGE_SRGB_NORMALIZE` | unset (on) | `is_srgb` of images is false unless a sampler of the stage has `force_degamma` (`specialization.h:117-150`). |
| `SOTC_PS_INPUT_NORMALIZE` | 1 (default on) | Flat PS inputs not sensitive by code are keyed as passthrough (`vk_pipeline_cache.cpp:3451-3466`, `4065-4074`). |
| `SOTC_PRETRANSLATE` | 1 | Programs are translated at load with synthetic sharps and the register state of their file (`gpu/sotc/pretranslate.h`). A draw whose key differs counts a miss (`vk_pipeline_cache.cpp:4253-4317`, `PretranslateDiff`). |
| `SOTC_NATIVE` | 1 | `PipeDirtyMode()` defaults to 1 (`vk_pipeline_cache.cpp:2187-2196`): the register part of the graphics key is reused while `PipeKeyRegs` is equal. No change of key content. |
| `SOTC_PIPE_DIRTY` | unset | See above (mode 1). |
| `SOTC_PIPE_MEMO` | unset | Key memo on (`vk_pipeline_cache.cpp:1564-1570`): a repeat of the previous key skips the map probe. No change of key content. |
| `SOTC_SPEC_MEMO` | unset (off) | Off (`vk_pipeline_cache.cpp:3228-3233`). |
| `SOTC_PIPE_GPL` | unset (off) | Off (`vk_instance.cpp:424-440`). No pipeline libraries; one pipeline per key. |
| `SOTC_NO_PREPASS` | 0 | Measurement only. Mode 1 is "not implemented" (`sotc_noprepass.h:88-93`). No key effect. The `t_promote` path (`vk_rasterizer.cpp`, `ApplyDynamic`) is a dynamic-state change, not a key change. |
| `SOTC_PR_KEYS` | 0 | Heap table keys off (`vk_rasterizer.cpp:4386-4393`). No graphics key effect. |
| `SOTC_SHADER_EXTENT` | 1 | Limits bound buffer sizes only (`sotc_shader_extent.h:18-30`). No key effect. |
| `SOTC_TEX_BLOCK` | 1 | Replays texture bind blocks (`vk_rasterizer.cpp:9509-9520`). No key effect. |
| `SOTC_ASYNC_SHADERS` | unset (on) | Chooses when a pipeline is built (`AsyncShadersEnabled`, `vk_pipeline_cache.cpp:366-370`). No key effect. |
| `SOTC_TINY_ASYNC` | unset (0) | Full-screen draws build on the command processor (`vk_pipeline_cache.cpp:755-768`). No key effect. |
| `SOTC_DESCRIPTOR_HEAP` | unset | Off (`vk_descriptor_heap.cpp:138`, `UsesDescriptorHeap()` returns false in `heap.cpp:260-263`). |
| `SOTC_GCN_*`, `SOTC_PREDICT_PROBE`, `SOTC_PREPASS_PROBE` | unset | Off by default (`predict_probe.cpp:293-299`, `sotc_prepass_probe.h:20-27`). The GCN profile fields are 0 unless their switches are set (inferred from `vk_pipeline_cache.cpp:607-608`). |

**Conclusion for the default config:** the stage variant is always default, the descriptors are classic (no
bindless, one set), the buffer part of the shader key is reduced by `SOTC_GENERIC_BUFFERS=2`, and the
graphics key is the one described in section 3 with the dynamic state of section 5.

---

## 3. Graphics pipeline key: `GraphicsPipelineKey`

Declared at `vk_graphics_pipeline.h:38-75`. The constructor memsets the struct (`:65-67`) and `operator==` is a
memcmp (`:69-71`). The hash is `XXH3_64bits` over the whole struct (`:195-199`), so padding counts too.
Built by `PipelineCache::RefreshGraphicsKeyRegs` (`vk_pipeline_cache.cpp:2731-2781`), then `RefreshGraphicsStages`
(`:2870-3101`, sets stage hashes and vertex formats), then the second pass of `RefreshGraphicsKey` (`:2809-2868`,
sets `mrt_mask` use, masks, samples). Lookup: `GetGraphicsPipelineLookupSlow` (`:1827-1880`,
`graphics_pipelines.try_emplace(graphics_key)` at `:1880`).

Approximate sizes (inferred from the types; no build was run, so `sizeof` is not measured).

| # | Field (type) | ~Size | Port source | Dynamic or baked | Class | Notes (proven unless marked) |
|---|---|---|---|---|---|---|
| 1 | `stage_hashes[6]` (`size_t`) | 48 B | `GetProgram` return: `HashCombine(params.hash, perm_idx)` (`vk_pipeline_cache.cpp:4249`, `4371`, `4432`) per stage; `0` for absent stage | baked (shader module identity) | PROGRAM x PERMUTATION | Program hash = `bininfo.shader_hash` of the code (`regs_shader.h:247-255`) plus variant. `perm_idx` is the position in `program->modules` (discovery order): **not stable across sessions** (inferred). `StageVariantDefault` in the default config, so no variant bits. |
| 2 | `vertex_buffer_formats[32]` (`vk::Format`) | 128 B | `fetch_shader` sharps: `Liverpool::SurfaceFormat(buffer.GetDataFmt(), GetNumberFmt())` (`vk_pipeline_cache.cpp:3086-3098`) | baked, **only when `!IsVertexInputDynamicState()`** (`vk_graphics_pipeline.cpp:117-123`) | MESH | With dynamic vertex input (device-dependent, BOOT) the array stays zero. The formats are the mesh's V# formats. |
| 3 | `patch_control_points` (`u32`) | 4 B | `ls_hs_config.hs_input_control_points` when `stage_enable.hs_en` (`:2747-2748`) | baked (tess) | PASS | Rect and quad lists force 3 and 4 (`vk_graphics_pipeline.cpp:143-146`). |
| 4 | `num_color_attachments` (`u32`) | 4 B | `std::bit_width(mrt_mask)` (`vk_pipeline_cache.cpp:2963`) | baked (render pass info) | PROGRAM (PS exports) | Comes from the **pixel shader's exports**, not from registers. |
| 5 | `color_buffers[8]` (`Shader::PsColorBuffer`) | ~8 x 8 B (inferred) | `CB_COLORn_INFO` (data/num format, number conversion, swizzle via `col_buf.*`), `SPI_SHADER_COL_FORMAT` via `color_export_format.GetFormat(cb)`, `blend_control[cb]` for `blend_self_scale` (`:2759-2779`) | baked (formats); `blend_self_scale` baked | PASS | Zeroed for targets the PS does not export (`:2836-2838`). Carries the format of every bound target with a target mask (see 7.2). Pipeline uses `data_format`/`num_format` for `SurfaceFormat` (`vk_graphics_pipeline.cpp:350-353`). |
| 6 | `blend_controls[8]` (`AmdGpu::BlendControl`) | ~8 x 4 B (inferred) | `blend_control[cb]`, copied only when `enable && !col_buf.info.blend_bypass` (`:2842-2844`) | baked (`attachments[i]` factors, ops, enable; `vk_graphics_pipeline.cpp:387-450`) | MATERIAL x PASS | Blend state of the draw's material and pass. Zero for disabled targets. |
| 7 | `write_masks[8]` (`vk::ColorComponentFlags`) | 32 B | `color_target_mask.GetMask(cb)` through `swizzle.ApplyMask` (`:2847-2848`) | **dynamic when `IsDynamicColorWriteMaskSupported()`** (`vk_graphics_pipeline.cpp:208-210`, `:445-449`); the mask is then set per draw (`vk_rasterizer.cpp` `ApplyDynamic`, `SetColorWriteMasks`) | PASS | Stored in the key in both cases, so a mask change makes a new key even when it is dynamic (see 7.1). |
| 8 | `cb_shader_mask` (`AmdGpu::ColorBufferMask`) | 4 B (inferred) | `color_shader_mask` whole register (`:2752`) | baked (alpha override, `vk_graphics_pipeline.cpp:468-478`) | PROGRAM / PASS | Stored whole, including slots that no attachment uses (see 7.1). |
| 9 | `logic_op` (`LogicOp`) | 4 B | `color_control.rop3` (`:2749`) | baked (`logicOpEnable = IsLogicOpSupported && logic_op != Copy`, `vk_graphics_pipeline.cpp:486-493`) | PASS | Engine pass default: `CB_COLOR_CONTROL` mode 2, rop3 `0xCC` = copy (RE `render-states.md` 2b, `0x93960`). Constant in that path. |
| 10 | `num_samples` (`u8`) | 1 B | `max(depth_samples, color samples)` (`:2750-2751`, `:2854`, `:2857-2865`) | baked (`rasterizationSamples`) | PASS | Samples come from `DB_Z_INFO` / `CB_COLORn_ATTRIB` (`NumSamples()`). The writers are not decoded (RE open item). |
| 11 | `depth_samples` (`u8`) | 1 B | `depth_buffer.NumSamples()` when depth valid, else 1 (`:2750`) | baked (depth attachment samples) | PASS | As 10. |
| 12 | `color_samples[8]` (`u8`) | 8 B | `col_buf.NumSamples()` per target (`:2851-2853`) | baked (mixed samples, `:363-373`) | PASS | As 10. Forced to the depth samples when mixed MSAA is unsupported (`:2857-2865`). |
| 13 | `mrt_mask` (`u32`) | 4 B | `fs_info->mrt_mask` (`:2962`) | baked (through attachment count) | PROGRAM | PS exports. |
| 14 | `z_format` (2 bits), `stencil_format` (1 bit), `depth_clamp_enable` (1 bit) | 4 B | `depth_buffer.z_info.format`, `stencil_info.format` (`DepthValid`/`StencilValid` gate, `:2734-2741`), `!depth_render_override.disable_viewport_clamp` (`:2741`) | baked (depth format; `depthClampEnable` in raster state; `vk_graphics_pipeline.cpp:149-155`) | PASS (formats); PASS (clamp) | `DB_Z_INFO`, `DB_STENCIL_INFO` writers not decoded (RE open). Depth **compare, test, write** are not in the key (dynamic). |
| 15 | `prim_type` (5 bits, `AmdGpu::PrimitiveType`) | 4 B (bitfield group) | `regs.primitive_type` (`VGT_PRIMITIVE_TYPE`, `:2745`) | baked (input assembly topology, `vk_graphics_pipeline.cpp:138-141`) | PASS / DRAW | Written by the engine's `0x86a900` (RE `render-states.md` 4): table `DAT_00F9F2E0[kind]`, `9` (PatchPrimitive) for tess, `0x11` (RectList) for full-screen passes. Small value set. |
| 16 | `polygon_mode` (2 bits) | (group) | `polygon_control.PolyMode()` (`:2746`) | baked (`polygonMode`, `:154`) | PASS | `PA_SU_SC_MODE_CNTL` (RE `0x86e1a0`; seed `0x240` in `0x93960`). Field positions not confirmed. |
| 17 | `clip_space` (1 bit) | (group) | `clipper_control.clip_space` (`:2743`) | baked (`negativeOneToOne`, `:184-186`) | PASS | `PA_CL_CLIP_CNTL` (RE `0x86ded0`, set in `0x90470` `RequestNewRenderContext`, i.e. per context). |
| 18 | `provoking_vtx_last` (1 bit) | (group) | `polygon_control.provoking_vtx_last` (`:2744`) | baked (`vk_graphics_pipeline.cpp:157-161`) | PASS | As 16. |
| 19 | `depth_clip_enable` (1 bit) | (group) | `clipper_control.ZclipEnable()` (`:2742`) | baked (`depthClipEnable`, `:162-164`) | PASS | As 17. Stored even when the device lacks `IsDepthClipEnableSupported` (`:170-172`). |

Not in the key (proven): index type and index buffer address, viewports, scissors, depth/stencil/bias/bounds,
blend constants, line width, cull mode, front face, primitive restart, rasterizer discard, the vertex stride
(dynamic `eVertexInputBindingStride`, `vk_graphics_pipeline.cpp:213-214`). These come from `ComputeDynamic` /
`ApplyDynamic` (`vk_rasterizer.cpp:11716-11770`, `11925-12007`, `12009-12060`).

### 3.1 Registers read by the key (`PipeKeyRegs`)

`vk_pipeline_cache.h:116-132` lists the registers the key part reads; the register snapshot of
`SOTC_PIPE_DIRTY` must match it (`:112-115`). They are: `depth_buffer`, `depth_render_override`,
`clipper_control`, `polygon_control`, `primitive_type`, `stage_enable`, `ls_hs_config`, `color_control`,
`color_shader_mask`, `color_target_mask`, `color_export_format`, `color_buffers[8]`, `blend_control[8]`.
Other registers feed `RefreshGraphicsStages` (`stage_enable`, `ProgramForStage` = the program address
registers, `vgt_gs_mode`, `vgt_strmout_config` in `PipeFastRegs`, `vertex` sharps through user data) and
`BuildRuntimeInfo` (section 4).

---

## 4. Shader specialization key: `StageSpecialization`

Declared at `specialization.h:219-366`. Built by `StageSpecialization(info, runtime_info, profile, start)`
(`:235-271`) in `GetProgram` (`vk_pipeline_cache.cpp:4230`). Compared by `operator==` (`:301-362`) when
`GetProgram` searches the permutations of a program (`:4253`, `std::ranges::find(program->modules, spec, &Program::Module::spec)`).
Permutations are stored with `Program::AddPermut` (`vk_pipeline_cache.h:100-102`, `MaxPermutations = 8` is the
small-vector capacity, not a limit: inferred).

### 4.1 Members

| Member | Built from | Class | Compared in `==` |
|---|---|---|---|
| `info` (`const Info*`) | the program | PROGRAM | not compared |
| `runtime_info` (`RuntimeInfo`, section 4.2) | `BuildRuntimeInfo` (`vk_pipeline_cache.cpp:181-362`) | PROGRAM / PASS / DRAW (mixed, see 4.2) | yes, whole (`:310`) |
| `bitset` (128 bits) | one bit per resource that has a non-null sharp (`ForEachSharp`, `:284-295`) | DRAW | yes, for entries present in both (`:344-354`) |
| `fetch_shader_data` | `Gcn::ParseFetchShader` of the VS (`:238`) | MESH | yes (`:314-316`) |
| `vs_attribs` (`VsAttribSpecialization`: divisor, num_class, dst_select) | per fetch attribute, from the V# (`FillVsAttrib`, `:79-91`) | MESH | yes (`:306`) |
| `buffers` (`BufferSpecialization`) | per `info.buffers` entry, from its V# (`FillBuffer`, `:93-115`) | DRAW (V#) | yes, when bit set (`:346`) |
| `images` (`ImageSpecialization`) | per `info.images` entry, from its T# (`FillImage`, `:142-154`) | TEXTURE (via DRAW binding) | yes, when bit set (`:351`) |
| `fmasks` (`FMaskSpecialization`: width, height) | per F-mask T# (`:156-159`) | TEXTURE / PASS | yes (`:318`) |
| `samplers` (`SamplerSpecialization`: force_unnormalized, force_degamma) | per S# (`:161-164`) | MATERIAL (inferred) | yes, always (`:356-358`) |
| `start` (`Backend::Bindings`: unified, buffer) | the binding counters at entry (`binding` argument of `GetProgram`) | derived (stage order) | yes, unless no resources (`:336-342`) |

`operator==` also requires equal list lengths for buffers, images and samplers (`:327-330`, added after a device
lost, see the comment at `:322-326`).

### 4.2 `RuntimeInfo` (`runtime_info.h:344-426`), per stage

Built by `BuildRuntimeInfo` (`vk_pipeline_cache.cpp:181-362`). Compared whole by `RuntimeInfo::operator==`
(`runtime_info.h:380-383`): `props`, `hw_stage`, `sw_stage`, and only the active union member of `sw` and `hw`
(`HasSameSwInfo`, `HasSameHwInfo`, `:385-415`).

| Field | Source (registers / memory) | Class | Notes |
|---|---|---|---|
| `props.num_user_data`, `num_input_vgprs`, `num_allocated_vgprs`, `fp_denorm_mode32/16_64`, `fp_round_mode32/16_64` | the stage's program descriptor settings (`program.settings`, `NumVgprs()`), `vk_pipeline_cache.cpp:184-192` | PROGRAM | Fixed per program binary. Known with the program. |
| VS `sw.vs.step_rate_0/1` | `VGT_INSTANCE_STEP_RATE_0/1` (`vk_pipeline_cache.cpp:337-338`, `Regs` lines 132-133) | DRAW (mesh instancing) | Compared even when no attribute uses an instance rate (see 7.1). |
| VS `sw.vs.vertex_sgpr_offset`, `instance_sgpr_offset` | `draw_indirect_params` of the draw (`:339-340`); set by the draw call (`vk_rasterizer.cpp:3344-3353`) and by `LookupGraphicsPipeline` (`:12129-12135`). The pre-translation record sets `0` and `0x4c+2` (`vk_pipeline_cache.cpp:3670-3675`, comment: instanced draws are indirect and put the start instance in user SGPR 2) | DRAW | Two values seen in the code. Direct draws use the default `{}` (`vk_pipeline_cache.h:213`). |
| VS `sw.vs.tess_emulated_primitive` | `primitive_type` is RectList or QuadList (`:341-343`) | PASS | |
| VS `hw.vs.num_outputs`, `outputs[3]` | `MapOutputs(vs_output_control)` (`:250`) | PROGRAM / PASS | |
| VS `hw.vs.user_clip_plane_mask` | `clipper_control.user_clip_plane_enable` (`:249`) | PASS | |
| VS `hw.vs.emulate_depth_negative_one_to_one` | device (`IsDepthClipControlSupported`) and `clip_space` (`:251-253`) | BOOT + PASS | |
| VS `hw.vs.clip_disable` | `regs.IsClipDisabled()` = `clip_disable` or RectList (`:254`, `Regs` line 186) | PASS | |
| FS `hw.fs.en_flags`, `addr_flags`, `num_inputs` | `ps_input_ena`, `ps_input_addr`, `num_interp` (`:259-261`) | PROGRAM (pixel program registers) | Matches the predict layout in `user/shader_predict/pass_layouts.txt` (ena, addr, interpolant count). |
| FS `hw.fs.inputs[32]` | `ps_inputs[i]` (`:303-311`), then flat-input normalization (`NormalizeFlatInputs`, `vk_pipeline_cache.cpp:4065-4074`, `3451-3466`) | PROGRAM (VS/PS pair) | Normalization is a program rule, keyed by the program code. |
| FS `hw.fs.front_face_all_bits` | `barycentric_control` (`:262`) | PROGRAM | |
| FS `hw.fs.num_samples` | `aa_config.NumSamples()` when sample coverage on (`:263-266`) | PASS | |
| FS `hw.fs.z_export_format`, `mrtz_mask` | `z_export_format`, `depth_shader_control` (`:267-273`) | PROGRAM | |
| FS `hw.fs.early_fragment_tests` | `depth_shader_control.z_order`, `mrtz_mask` bits 0-2, `Common::ElfInfo::IsSotc()` (`:280-289`) | PROGRAM + PASS | |
| FS `hw.fs.dual_source_blending` | `blend_control[0]` factors (`:290-302`) | MATERIAL x PASS | |
| FS `hw.fs.color_buffers[8]` | copied from `graphics_key.color_buffers` (`:312-314`) | PASS | **All bound targets, not only the exported ones** (see 7.1). |
| FS `hw.fs.clip_distance_emulation` | `vs_output_control`, `clipper_control`, `stage_enable`, and `profile.needs_clip_distance_emulation` (NVIDIA only, `vk_pipeline_cache.cpp:591`) (`:318-325`) | PASS + BOOT | |
| CS `hw.cs.*`, `props` | `FillComputeRuntimeInfo` from the compute program (`vk_pipeline_cache.cpp:161-179`): workgroup size, TGID enables, shared memory size | PROGRAM | |
| TCS / TES `sw.tcs`, `sw.tes` | `ls_hs_config`, `tess_config`, `hs_program.settings`, and the tess constant buffer read from guest memory (`info.cpp`, `ReadTessConstantBuffer`) (`:345-357`) | PROGRAM / DRAW (constant buffer) | Only with tessellation (SotC: not expected to occur; inferred). |

### 4.3 Cross-stage coupling

- `start` (`Backend::Bindings`) of a stage is the counter after the earlier stages. With `SOTC_VS_BINDING_BASE=1`
  the VS start moves to `VsUnifiedBase` (default 48) and the buffer base when the previous counters are below it
  (`bindings.h:51-58`). So **the VS permutation depends on the PS's binding count** (inferred from the rule; the
  `start` comparison is at `specialization.h:340-342`). A new PS with more resources creates new VS permutations.
- The PS is bound first (`RefreshGraphicsStages`, `vk_pipeline_cache.cpp:2939-2959`), so its RT formats (section
  4.2) and its resource count reach the VS key through `start` as well.

---

## 5. Dynamic state enabled in this port (`vk_graphics_pipeline.cpp:192-215`)

Always dynamic (proven):
`ViewportWithCount`, `ScissorWithCount`, `BlendConstants`, `DepthTestEnable`, `DepthWriteEnable`,
`DepthCompareOp`, `DepthBiasEnable`, `DepthBias`, `StencilTestEnable`, `StencilReference`, `StencilCompareMask`,
`StencilWriteMask`, `StencilOp`, `CullMode`, `FrontFace`, `RasterizerDiscardEnable`, `LineWidth`,
`PrimitiveRestartEnable`.

Device-dependent (BOOT, proven as code paths; the device is not probed here):
- `DepthBoundsTestEnable`, `DepthBounds`: when `IsDepthBoundsSupported()`.
- `ColorWriteMaskEXT`: when `IsDynamicColorWriteMaskSupported()` (needs `extendedDynamicState3ColorWriteMask`).
- `VertexInputEXT`: when `IsVertexInputDynamicState()` (`VK_EXT_vertex_input_dynamic_state`, `vk_instance.cpp:413`).
  Otherwise `VertexInputBindingStride` when there are vertex bindings (`:213-214`).

Static depth/stencil block: `pDepthStencilState` is nulled only when `IsExtendedDynamicState3Supported()`
(`vk_graphics_pipeline.cpp:525-526`). The depth-stencil struct itself is empty (`:497`). So the depth and stencil
values are never in the key.

Values set per draw: `ApplyDynamic` (`vk_rasterizer.cpp:11720-11770`) and `ComputeDynamic` (`:11730-11741`).

---

## 6. Field-by-field source table (final)

Columns: the field, its class, the game source (RE evidence), and whether the port can know it before the first draw.

### 6.1 Graphics key

| Field | Class | Game source (RE, address) | Known before first draw? |
|---|---|---|---|
| stage_hashes VS/PS (program identity) | PROGRAM x MATERIAL x PASS | Pass slot k of a material: records 0 and 4 give the programs (`0xf8990`, `pass-map.md` 3, 7); program hash = OrbShdr hash (`pass-map.md` 6) | Yes, after the `.cmat` and `.csdr` are loaded (`pass-map.md` 7 algorithm). The port's predict table gives hash to pass name (`user/shader_predict/predict_table.tsv`). |
| stage_hashes perm index | DRAW (learned) | none (port-internal) | No. Learned at the first draw of each permutation. |
| vertex_buffer_formats (non-dynamic VI only) | MESH | Mesh vertex attribute array, 0x24 entries (`materials-meshes.md` 5.2, `0x127d90`); formats not named in RE | Partly: after the `.cmsh` build; formats unnamed (RE open). |
| patch_control_points | PASS | `LS_HS_CONFIG` writer not in RE | Rare (tess). |
| num_color_attachments, mrt_mask | PROGRAM | PS exports (`info.mrt_mask`, `export.cpp`) | Yes, from the PS translation (or the predict layout). |
| color_buffers formats, blend_self_scale | PASS | `0x85cbc0` / `0x86a1a0` write `CB_COLORn` from the pass RT descriptor (`pass+0xe4`, `render-states.md` 3 and 2a). Export format: `SPI_SHADER_COL_FORMAT` (not decoded) | No: the pass object is not identified in the port. Possible by hooking the pass RT write (proposal P2). |
| blend_controls | MATERIAL x PASS | `0xf8990` chooses the state object (`material+0x560`, or `DAT_02ee3ce0+0x28/0x30/0x48`, or `0x91b50`/`DAT_+0xb0`) (`pass-map.md` 3, `materials-meshes.md` 3.3). Blend words per object not decoded (`render-states.md` open) | Partly: the (material, k) pair decides it. Contents of the state objects are open. |
| write_masks | PASS | `CB_TARGET_MASK = 0xF` in the `0x93960` pass path (`render-states.md` 2b, `0x869990`) | Yes for the engine's pass path. |
| cb_shader_mask | PROGRAM / PASS | PS export mask (`SPI_SHADER_COL_FORMAT` / CB_SHADER_MASK writer not decoded) | Not decoded. |
| logic_op | PASS | `CB_COLOR_CONTROL` mode 2, rop3 0xCC in `0x93960` (`render-states.md` 2b, `0x869ce0`) | Yes (constant in the pass path). |
| num_samples, depth_samples, color_samples | PASS | `DB_Z_INFO` / `CB_COLORn_ATTRIB` writers not decoded | Not decoded. Probably 1 in SotC (inferred, not checked). |
| z_format, stencil_format | PASS | `DB_Z_INFO`, `DB_STENCIL_INFO` writers not isolated (`render-states.md` 3, open) | Not decoded. |
| depth_clamp_enable | PASS | `DB_RENDER_OVERRIDE` (`0x86be10`, `render-states.md` 1, inferred) | Yes if the writer is confirmed. |
| prim_type | PASS / DRAW | `0x86a900` with `DAT_00F9F2E0[kind]`, 9 (tess), 0x11 RectList (`render-states.md` 4) | Mostly: the value set is small (the table `DAT_00F9F2E0` is not dumped). |
| polygon_mode, provoking_vtx_last | PASS | `PA_SU_SC_MODE_CNTL` (`0x86e1a0`, seed `0x240` in `0x93960`, `render-states.md` 2b, 2c) | Likely constant; field positions not confirmed. |
| clip_space, depth_clip_enable | PASS (per context) | `PA_CL_CLIP_CNTL` (`0x86ded0`, from `0x90470` `RequestNewRenderContext`, `render-states.md` 2b) | Yes (per context). |

### 6.2 Shader key (`StageSpecialization`)

| Field | Class | Game source | Known before first draw? |
|---|---|---|---|
| runtime_info.props (vgpr counts, denorm/round, user regs) | PROGRAM | program descriptor (`.csdr` records, `shader-collections.md` 2b; `pretranslate.h` `rsrc1`, `rsrc2`, `regs`) | Yes, at program load. Already used by PRETRANSLATE. |
| runtime_info.fs.en_flags, addr_flags, num_inputs, inputs | PROGRAM (VS/PS pair) | predict layout `ena`, `addr`, `num_inputs`, `interp_mask`, `param` (`pretranslate.h` `Request`) | Yes, from the file (SotcPredict step 2a). |
| runtime_info.fs.color_buffers, num_samples | PASS | as 6.1 | No (see 6.1). |
| runtime_info.vs.step_rate_0/1 | DRAW (mesh instancing) | `VGT_INSTANCE_STEP_RATE_0/1` | Only at the draw (or mesh, inferred). |
| runtime_info.vs.vertex_sgpr_offset, instance_sgpr_offset | DRAW | draw packet kind; 0x4c+2 for instanced indirect draws (`vk_pipeline_cache.cpp:3670-3675`) | Only at the draw. Two values seen. |
| fetch_shader_data, vs_attribs | MESH | VS fetch shader in guest memory; V# of vertex buffers; mesh vertex declaration (`materials-meshes.md` 5) | At mesh build, if the formats are decoded (RE open). |
| buffers (V#) | DRAW (MESH for vertex buffers; ENGINE for constant buffers, inferred) | V# in user data (flat buffer) | Only at the draw, unless the owner's table is read. With `SOTC_GENERIC_BUFFERS=2` most of the V# fields are gone from the key (see 7.2). |
| images (T#) | TEXTURE | T# words: `materials-meshes.md` 3.1-3.2 (view table of each MatSlot, raw T# from `.ctxr`, base address rewritten at residency) | After the texture loads; the slot to shader-image index map is not known (RE open). |
| fmasks | TEXTURE / PASS | F-mask T# | Not decoded. |
| samplers (S#) | MATERIAL (inferred) | S# words in material data (not decoded in RE) | Not decoded. |
| bitset, start | DRAW | resource presence per draw; start from stage order | Only at the draw. Derived from the rest once the program is known. |

---

## 7. Keys that are larger than the pipeline needs (found while reading)

These differ between draws without changing the pipeline or the shader code. Each one multiplies the number of
keys and permutations.

1. **Write masks and the shader mask are stored whole.** `write_masks` is in the key even when the color write
   mask is dynamic (`vk_graphics_pipeline.cpp:208-210`, `:445-449`; `ApplyDynamic` sets it per draw,
   `vk_rasterizer.cpp` `SetColorWriteMasks`). `cb_shader_mask` (`:2752`) is copied whole, so bits of slots no
   attachment uses still split keys.
2. **Unexported render-target formats split the PS permutation.** `BuildRuntimeInfo` copies
   `graphics_key.color_buffers[i]` for all eight slots (`vk_pipeline_cache.cpp:312-314`) while `bind_stage(Fragment)`
   runs (`:2958`, `:2945`), before the second pass clears the slots the PS does not export (`:2836-2838`). The PS
   `RuntimeInfo` therefore holds the formats of every bound target with a target mask, and
   `HwFragmentRuntimeInfo::operator==` compares all eight (`runtime_info.h:315-326`). **Proven by read order; the
   number of extra permutations is inferred.**
3. **VS runtime info compares the step rates and the SGPR offsets always.** `SwVertexRuntimeInfo ==` compares
   `step_rate_0/1`, `vertex_sgpr_offset`, `instance_sgpr_offset` and `tess_emulated_primitive`
   (`runtime_info.h:169-177`, `:385-391`), but `FillVsAttrib` reads the step rate only for an instance-rate
   attribute (`specialization.h:82-88`). **Inferred:** a VS whose attributes are all per-vertex splits by draw kind
   or step rate for no code difference.
4. **Logic op and the blend words are stored in the disabled case.** `logic_op` is stored from `rop3` whatever the
   operation mode (`:2749`), and the blend words are copied only when enabled (`:2842-2844`). A mode `Disable`
   still gives a different key through `rop3` (inferred: rare in SotC).
5. **Depth clip flag stored when the device cannot use it** (`:2742`, `vk_graphics_pipeline.cpp:170-172`). Only if the
   device lacks `VK_EXT_depth_clip_enable`.
6. **Flat PS inputs** are normalized only in the runtime info (`SOTC_PS_INPUT_NORMALIZE`), which is the right place.

### 7.1 Notes on the buffer and image parts (proven)

- `SOTC_GENERIC_BUFFERS=2`: unswizzled buffers key `stride = 0` (`specialization.h:96`). Untyped formatted loads of
  read-only guest buffers with one of 17 listed format pairs key `data_format = 63`, `num_format = 0`, identity
  `dst_select`, no conversion (`specialization.h:99-104`, `info.h:155-188`). The remaining buffer key is
  `is_formatted`, `swizzle_enable`, and for swizzled buffers `index_stride` and `element_size`.
- Images: `is_srgb` is set only for non-storage images and only when `KeepSrgb` holds (`specialization.h:150`,
  `:130-140`). With the default switch, a stage with no `force_degamma` sampler keys `is_srgb = false`. So the
  sRGB number format of the T# no longer splits image keys in the default config.
- `dst_select` of an image is keyed only for storage images (`specialization.h:146-149`).
- `num_bindings` of an image is keyed (`desc.NumBindings(info)`, `:153`), from the shader (array size).

---

## 8. DRAW-ONLY fields: how to handle each one

| Field | Values seen (code) | Proposal |
|---|---|---|
| VS `vertex_sgpr_offset`, `instance_sgpr_offset` | `{0, 0}` (direct), `{0, 0x4e}` (instanced indirect, `vk_pipeline_cache.cpp:3670-3675`) | **Enumerate**: at pre-translation of a VS, build both variants (two permutations). Stays in the key; cheap. |
| VS `step_rate_0/1` | Register value per mesh (normally 0 for non-instanced; inferred) | **Enumerate** from the observed values, or drop them from the compare while no attribute uses an instance rate (section 7, item 3). |
| Buffer and image V#/T# words | Few values per resource class: formats, types, number formats, conversions | **Enumerate by class**: `SOTC_GENERIC_BUFFERS=2` already removes the largest part. For images, a small table per program (type, is_integer, is_storage, num_conversion, num_bindings) can be precomputed from the T# of the material's slots (MATERIAL x PASS), and checked against the draw (the PRETRANSLATE miss counter does this). |
| Vertex formats (`vs_attribs`, `vertex_buffer_formats`) | Per mesh declaration | **Enumerate per mesh** from the `.cmsh` attribute array (`materials-meshes.md` 5.2, once the 0x24-byte entry is decoded). Until then: record at first draw of each mesh class. |
| Render-target formats, samples, `mrt_mask` interplay | Per pass (`0x85cbc0`, `0x86a1a0`) | **Learn by pass**: hook the CB_COLORn writer (`0x86a1a0`, pass RT descriptor) and record (pass, format set). Map the program hash to the pass name through `predict_table.tsv`. Two steps: record first, then enumerate. |
| Blend (CB_BLEND) words | Per (material, k), from the state objects | **Read the state objects** (`DAT_02ee3ce0` table, `render-states.md` 2a) once their words are decoded, or **record** the blend words per (program, pass) at the first draw. |
| prim_type | About 3-4 values (table `DAT_00F9F2E0`, 9, 0x11) | **Enumerate**: all values are in the baked key; one pipeline per value. |
| `stage_hashes` perm index | Discovery order | **Replace the identity**: learn each permutation by its packed spec words (`BuildSpecKey`, `specialization.h:404-465`) instead of the index. Required before any cross-session pre-build. |
| Bitset, `start` | Per draw | No separate handling: they follow from the spec. |

Also possible in one step (no proposal needed): `write_masks` and the shader mask can be kept out of the key when
the device has dynamic color write (item 1 of section 7), and `cb_shader_mask` can be masked to the attachment
slots.

---

## 9. Open items and confidence

- **Proven:** the key structs and their fields (section 3, 4.1, 4.2); dynamic state list (section 5); the read
  order that puts unexported render-target formats into the PS runtime info (section 7, item 2); the default
  switch set (section 2).
- **Proven code, not measured:** the number of permutations or pipelines the port creates per run. No game run was made here.
- **Inferred:** `SOTC_VS_BINDING_BASE` coupling (4.3); the VS step-rate split (7, item 3); the approximate sizes;
  `MaxPermutations` being a capacity, not a limit.
- **Open (RE side, needed for the pass and material columns):** `DB_Z_INFO` and `CB_COLORn_ATTRIB` writers (sample
  counts, depth format); the blend words in the state objects (`DAT_02ee3ce0`); the sampler (S#) location in the
  material; the vertex attribute format names (`0x127d90`, 0x24-byte entries); the `DAT_00F9F2E0` table.
- **Port side, not checked here:** whether the device on which the game runs has `VK_EXT_vertex_input_dynamic_state`,
  `extendedDynamicState3ColorWriteMask`, `VK_EXT_extended_dynamic_state3`. These switch fields 2, 7 and the
  depth-stencil block (section 5). The answer decides the size of the graphics key.
- **Files read:** `vk_graphics_pipeline.h`, `vk_graphics_pipeline.cpp` (20-640), `vk_pipeline_cache.cpp`
  (181-362, 1790-1880, 2189-2200, 2296-2310, 2720-3160, 3225-3233, 3440-3480, 3558-3680, 4027-4400, 540-612, 755-810, 1560-1585),
  `vk_pipeline_cache.h` (76-146, 213, 560-580), `vk_instance.cpp` (400-445, 855-870, 1000-1060), `vk_instance.h`
  (226-260), `specialization.h`, `runtime_info.h`, `info.h` (90-345), `params.h`, `regs_shader.h` (235-258), `regs.h` (40-186),
  `vk_rasterizer.cpp` (3335-3360, 3664-3680, 4380-4400, 9505-9520, 11716-12135), `sotc_arch/heap.cpp` (225-290),
  `sotc_arch/layouts.cpp` (20-70), `sotc_noprepass.h`, `sotc_prepass_probe.h`, `sotc_shader_extent.h`, `bindings.h`,
  `info.cpp` (38-52), `vk_bindless.cpp` (20-40), `vk_pipeline_common.cpp` (36-60), `vk_compute_pipeline.h`, `gpu/sotc/pretranslate.h`,
  `gpu/sotc/predict_probe.cpp` (288-300), and the RE docs `pass-map.md`, `render-states.md`, `materials-meshes.md`,
  `shader-collections.md`, plus `sotc.ini` and `user/shader_predict/pass_layouts.txt` (head).
