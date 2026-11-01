# 04 - Allocation scopes, heap ids and frame resets

Read-only. Sources: `out/re/decomp`, `eboot_re.py` (callers, callees, refs, fstr), scratch scripts (not in repo).
Addresses are eboot offsets. Labels: **observed**, **inferred**, **unknown**.

## 1. Answers

1. **Observed.** `DAT_02ed1f48..1f60` hold memtype slot ids, not kinds. `render_context_register_heaps`
   (0x8c880) sets them to 1 as placeholders; `sub_8c370` (0x8c370) overwrites them with the slot from
   `FUN_120d0`. No other writer exists (all RIP references to `0x2ed1f40..1f70` classified).
2. **Observed.** Per-frame GPU writers never consult the scope stack. Their bytes come from `0x8c0d0` and
   `0x8c120`, which pass `DAT_02ed1f54` (RenderSingleFrameGPU, kind 2) as memtype. Scopes apply only to memtype 1.
   No writer is reachable from a scope window (direct calls only).
3. **Correction to the brief.** `0x8c0d0` is `(descriptor*, size)` and calls `0x12820(size, DAT_02ed1f54, 0x10, 0)`.
   Its memtype is not 1.
4. **Observed.** Frame resets go through `0x136f0` -> vtable `+0x20` = `0x1d230` (ring-index advance).
5. **Observed.** Only `sub_917c0` window B sends a memtype-1 request into a GPU family (RenderContextGPU, Garlic).

## 2. Heap-id globals (question 1)

| Global | Object (0x8c880) | Parent (kind) | Ring tag | Set at | Reset call |
|---|---|---|---|---|---|
| 1f48 | GameToRenderThread | CPU arena (1) | 3 | 0x8c3ad | 0x25b023 |
| 1f4c | GameToGPU | GPU arena (2) | 5 | 0x8c3ff | 0x25b02f |
| 1f50 | CPU_RenderToGPUToGame | CPU (1) | 6 | 0x8c43b | 0x25b03b |
| 1f54 | RenderSingleFrameGPU | GPU (2) | 2 | 0x8c470 | 0x8c864 |
| 1f58 | RenderSingleFrameCPU | CPU (1) | 2 | 0x8c4bb | 0x8c870 |
| 1f5c | MaterialParamHeap (kind 2) | - | - | 0x8c54c | none |
| 1f60 | VirtualMemoryGPU (kind 2) | - | - | 0x8c597 | none |
| 9f80 | CPUMemory family | - | - | 0x1340a | 1 |
| 9f84 | GPUMemory family | - | - | 0x1124b | 2 (inferred) |
| 9f88 / 9f8c | GameSingleFrame / GameDoubleFrame | CPU (1) | 1 / 2 | 0x1192b / 0x11b3b | 0x25b00b / 0x25b017 |
| 0x32f5dd0 | PhysX heap (`sub_a3da30`) | GPU (2) | 7 | 0xa3dae7 | 0xa3f1c8 |

- **Observed:** the ring tag is the u32 `0x1cfb0` stores at `+0x8c`; `0x1d230` uses it as the modulus.
- **Observed:** GameSingleFrame and GameDoubleFrame are created kind 0, but `0x1cfb0` copies the parent's kind,
  so both are kind 1.
- **Observed:** `0x8c370` runs from `sub_8f6f0` (0x8f8ed), called from AreaLoad_ShaderListAndAssets (0x256bbd).
  No once-guard was found (**unknown** whether it runs once per process).

## 3. Per-frame writers (question 2)

| Writer | Allocation (memtype) | Scope? | Chain to root |
|---|---|---|---|
| SetViewParams 0xbe1f0 | `0x8c120` (1f54) | no | EndFrameViewParams 0xcefa0 (0xcfa44..0xcfca8); BPXPR_PreRender 0xca6d0 |
| InstanceData_WriteStream 0x123410 | `0x8c0d0` x2 (1f54) | no | UpdateGPUData 0x118100 <- UpdateSkinning 0xcfdb0 <- EndFrameViewParams (0xcf710) |
| ParticleRenderer_ShiftRecords 0x101720 | none; descriptors `pool+0x80/+0xa8` via `0x8c0d0` in UpdateSimulation 0x103ad0 / 0x1051f0 (1f54) | no | UpdateSimulation <- sub_103aa0 (**unknown**, no static caller) |
| ParticleRenderer::BeginFrame 0x107540 | `0x8c0d0` x4 (1f54) | no | sub_d0040 (0xd0137) <- EndFrameViewParams (0xcf83a) |
| EndFrameView_WriteA 0x8b7f0 | `0x8c0d0` (1f54) | no | EndFrameViewParams (0xcf43c) |
| EndFrameView_WriteB 0xcedf0 | none direct | no | sub_ceb40, EndFrameViewParams |
| InstanceDynamicData_Update 0x11c710 | `0x8c0d0` (1f54) | no | BPXPR_PreRender 0xca8a7 <- EndFrameViewParams (0xcf82f) |

- **Observed:** `0x8be60` usage bit 0x2 copies into 1f54; bit 0x1 uses 9f80 (CPU, scope-capable); otherwise 9f84.
- **Observed:** 130 decompiled call sites pass 1f54, so most per-frame GPU allocations bypass scopes.
- **Observed:** EndFrameViewParams order: writers, then InsertWaitFlipDone (0xcfd45), then RenderBackend_Frame (0xcfd6c).

## 4. Scope pushers (question 4)

**Observed:** `0x12300` appends `{object, 16-byte pair (source file, signature), u32 tag}` to the TLS stack
(`TLS+0x400`); `0x12350` pops. `0x12820` reads the top entry only for memtype 1. Tags look like source line numbers (**inferred**).

| Pusher | Push -> pop | Object pushed | Tag | Caller chain |
|---|---|---|---|---|
| JobSystem_Initialize 0x335d0 | 0x33720 -> 0x33bd1 | FrameAllocateJanitor, bump arena (0x481f0) | none | sub_255180 (0x255245), sub_255610 |
| RequestNewRenderContext 0x90470 | 0x904ee -> 0x9051c | RenderContextCPU family (ctx+0x3c0, kind 1) | 0x25d | sub_90430 <- RunFunctionAsRenderJob, MainCamera, UpdateGPUData; Flip::SetupFlipState |
| RequestNewComputeContext 0x906d0 | 0x90732 -> 0x9076b | RenderContextCPU family | 0x286 | sub_90670 (0xc160d) |
| CRenderContext ctor 0x917c0 A | 0x918c1 -> 0x91909 | RenderContextCPU family | 0xe3 | RequestNewRenderContext (0x90517) |
| CRenderContext ctor 0x917c0 B | 0x91926 -> 0x91942 | RenderContextGPU family (ctx+0x4a0, kind 2) | 0xe9 | as above |
| CComputeContext ctor 0x16e990 A | 0x16ea05 -> 0x16ea21 | RenderContextCPU family | 0x2e | sub_906d0 (0x9075d) |
| CComputeContext ctor 0x16e990 B | 0x16ea42 -> 0x16ea62 | RenderContextGPU family | 0x32 | as above; **inert** (uses 1f54) |
| StaticModelSection::Init 0x19aad0 | 0x19acfa -> 0x19aee3 | child heap of CPU arena, 1 MiB, ring 1 | 0x60 | sub_271b10 (0x2723b2) <- sub_271570 |
| BuildXPR 0x1b2390 | 0x1b2e4a -> 0x1b3163 | CPUMemory family (identity) | 0xdd | sub_1b0670 (0x1b08c2) <- 0x19aad0 |
| BuildXPR 0x1b32f0 | 0x1b37b7 -> 0x1b3e99 | CPUMemory family (identity) | 0x180 | sub_1b0980 (0x1b0987) <- 0x19aad0 |
| sub_a46640 | 0xa4676b -> 0xa46786 | bump arena, 0x300 bytes | unknown | sub_a46b20 (0xa46b42) |
| sub_a47d60 | 0xa47e67 -> 0xa47e86 | bump arena, 0x18000 bytes | unknown | sub_a4d330 (0xa4d3c7) |
| sub_a48280 | 0xa484c1 -> 0xa484d5 | bump arena, 0x18000 bytes | unknown | sub_a4d330 (0xa4d3d6) |
| sub_a4ab90 | 0xa4ac9c -> 0xa4acb3 | bump arena, 0x18000 bytes | unknown | sub_a4d330 (0xa4d3e3) |

- **Observed:** 12 functions, 14 windows; these are all the direct callers of `0x12300`.
- **Observed:** 99 decompiled functions call `0x12820` with literal memtype 1, including the `new` wrappers `0x17070`,
  `0x17090`, `0x170d0`, `0x170f0`, so every C++ `new` inside a window goes to the scope.

## 5. Frame resets (question 3)

- **Observed:** `0x136f0` returns at once for memtype 1; otherwise it calls vtable `+0x20`. For heaps built on
  `0x475c0` (vtable `0x1594ca0`) that slot is `0x1d230`: it writes list heads at `+0xa0..+0x2d0`, adds list
  sizes to `+0x90`, keeps the peak in `+0x98`, and sets `+0x88 = (index+1) % (+0x8c)`. Which slot is emptied is **unknown**.

| Heap(s) | Call site | Caller, thread | When |
|---|---|---|---|
| GameSingleFrame, GameDoubleFrame, GameToRenderThread, GameToGPU, CPU_RenderToGPUToGame | 0x25b00b..0x25b03b | GameFrame_Update 0x25ae80 <- GameUpdate_FrameGate 0x536484; game thread | game frame (inferred) |
| RenderSingleFrameGPU, RenderSingleFrameCPU | 0x8c864, 0x8c870 in `sub_8c860` | RenderBackend_Frame 0x90ea0 (call 0x90fd2, after FUN_8ef20 flush); from EndFrameViewParams 0xcfd6c; render thread | end of frame, after all writers |
| ParticleDoubleFrame (ring 3) | direct `0x1d230(param_4)` in 0x107540 | sub_d0040 <- EndFrameViewParams (0xcf83a); render thread | start of particle frame |
| PhysX heap | 0xa3f1c8 in PhysicsJob (`sub_a3f140`) | sub_25bb30 <- ShadowGameUpdate_ThreadFunc (0x535899) | per physics step |

- **Inferred:** `param_4` of 0x107540 is read from `*(ctx+0xdc8)`, the object 0x101ee0 builds.
- **Inferred risk:** particle and instance descriptors take bytes from 1f54 (ring 2), while the particle object
  rings separately (ring 3). A ring-boundary change must move both together.

## 6. Corrections and open items

- **Observed:** `sub_917c0` window B puts the second context buffer (`param_3`) in the GPU family; the first
  (`param_2`, 0x500000 by default) is in the CPU family. **Inferred:** these are the DCB and CCB of call `0x862690`.
- **Observed:** in `16e990`, the second buffer uses 1f54, so window B is inert.
- **Unknown:** whether `sub_8f6f0` runs once per process; indirect calls are not in the index; roots of
  `sub_103aa0`, `sub_a46b20`, `sub_a4d330`, `sub_255130`; exact slot numbers (runtime only).
