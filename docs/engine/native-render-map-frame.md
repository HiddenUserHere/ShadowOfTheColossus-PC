# Map: FRAME / CAMERA / CULLING / SHADOW functions of the RenderJob (EU 1.01 CUSA08809)
Tags: **proven** (decompile read, file + line), **inferred** (structure/name), **unknown**.
Decomp paths relative to `out/re/decomp/<hi16>/<addr>.c`. Sizes are the Ghidra function sizes (proven from the file header).

## 0. Object model (all offsets proven from the decompiles cited below)
- `G = DAT_02edf140` (global render/sync struct, image 0x2edf140). `G+0x220358` = per-frame counter (`000c/000cefa0.c:230`);
  `G+0x220370` = accumulated ms (`0011/0011d040.c:35`); `G+0x220384..+0x2203a4` = per-vector draw counters (`000c/000caed0.c:687-713`).
- `R = *G` = renderer object. `R+0x10` gate mutex, `R+0x30` wait-accumulator (`000b/000bd0a0.c:14-16`);
  `R+0x40 = F` (published by `000b/000bcde0.c:50`); sub-renderers at `R+0x48/0x50/0x58/0x68/0x78/0x80/0x90/0x98/0xa8/0xb0/0xb8`
  (`000c/000cefa0.c:223-478`); `R+0x8f8` shadow-cascade config (`000c/000caed0.c:169`); draw-table vectors `R+0x120/0x128`,
  `+0x948/0x950`, `+0x1170/0x1178`, `+0x21c0/0x21c8`, `+0x29e8/0x29f0`, `+0x3210/0x3218`, `+0x3a38/0x3a40`, `+0x4260/0x4268` (`000c/000caed0.c:687-713`).
- `F = *(R+0x40)` = frame object (RenderJob data). `F+0x280` = 0xac8-byte per-frame config block (`000c/000cefa0.c:153`);
  `F+0xb8` = view-config block (2nd arg of MainCamera); `F+0xd70`; `F+0xd88` fov-tan source; `F+0xd8c` = derived proj constant;
  `F+0xd90` = projection-override; `F+0xd98` = `M`; `F+0xda8/0xdb0` = job list; `F+0xdc0` particle ctx; `F+0xdc8` water ctx;
  `F+0x754` view-swap flag (`000b/000bcde0.c:24-49`, `000c/000cefa0.c:147-166`).
- `M = V = *(F+0xd98)` = render-target/target manager (K6.3; `pass+0x18` = `param_3[3]` in runners). Target sets:
  `V+0x1fb8`=kRTT_MainDepth, `+0x1fd8..+0x1ff0`=GBuffer colours, `+0x1ff8`=Ambient0, `+0x2000`=Ambient1,
  `+0x2078`=Motion, `+0x29a0`=SetA(depth-only), `+0x2a40`=SetB(GBuffer), `+0x2900`, `+0x2950` (K5/K6).
- job ctx (`param_3` in pass runners, `local_260` in `000c/000cefa0.c`): `[0]`=cb/context; `[2]`=RC render context
  (`+0xe88` job counter, `+0xe98` job-fn vector, `+0xe28/+0xe50` targets/labels); `[3]`=V; `[6]`=current target;
  `[9]`=config flags; `[0xc]`=0x50-byte draw table; `[0x1d]`=label slot array; `+0xf4`=pass slot k; `+0xf8`=pass flag word;
  `+0xfc`=flags (`000c/000caed0.c:180-205`, `000c/000c59d0.c:188`, `000c/000c1860.c:90-134`).
- cb / CRenderContext: `+0x08` end, `+0x10` cursor, `+0x18` reserve callback, `+0x20` cb arg, `+0x28` flags (bit0 = predication),
  `+0x150/+0x158` COND_EXEC block start / predicate address (`0085/0085cea0.c:10-22`).

## 1. GameFrame_Update — 0x25ae80 (497 B)
- Callers: `FUN_00536480` (the game main loop). Callees: `000bd0a0`, `000357f0`, `000199d0`, `00245440`, `00095ef0`,
  `0030d140`, `0030cfd0`, `000136f0` x5, `000bcde0`. (`0025/0025ae80.c`)
- Reads/writes on `param_1` (the game/App object): `param_1[0x4d0]`=+0x2680 (frame time, `line 21`), `param_1[0x4cf]`=+0x2678 (min),
  `param_1[0x4ce]`=+0x2670 (a **vtable pointer**: `(*(code*)**(undefined8**)param_1[0x4ce])()` at `line 31` = the camera/view update callback),
  `param_1[0x76d]`=+0x3b68 (`line 33/72`), `*(int*)((long)param_1+0x265c)` (`line 72`).
- Virtual `(**(code**)(*param_1 + 0x90))(param_1)` (`line 32`) returns `uVar6` = the frame object F. (`inferred`)
- GPU output: none directly; it (1) accumulates the render-thread wait via `FUN_000bd0a0(G)` (=§3), (2) runs the view-update
  vtable callback (camera derivation lives behind `param_1[0x4ce]`, target **unknown**), (3) calls `FUN_000bcde0(G, F)` (=§2) to kick the render thread.
- Vulkan mapping: host frame-loop entry; the gate is a CPU fence, not GPU.
- Camera role: **this is the game-thread driver**, not the camera math (see §7 for the actual view math in SetViewParams).

## 2. RenderThread_Kick — 0xbcde0 (382 B)
- Callers: `FUN_0025ae80`, `FUN_000a2aa0`. (`000b/000bcde0.c`)
- `param_1 = G`; `param_2 = F`. Sequence (proven):
  - `FUN_00118370(G)`; `memcpy(F+0x280, &DAT_02ea8ee8, 0xac8)` — copies the render-thread config block into the frame (`line 17`).
  - view-swap detection: `plVar2=*(F+0xd98)`; `iVar1=*(*plVar2+0x4d80)`; `plVar2[0x17]=iVar1` (=M+0xb8); if changed clear `M+0xb0 bit0` (`lines 24-30`).
  - `cVar5=(**(code**)(*DAT_02dfd4b8+0x20))()`; if true `*(*(F+0xd98)+0xbc)=*(F+0xd70)` (`lines 31-34`).
  - `DAT_02ea93bc = (iVar1==lVar4 && DAT_02ea93b8==2) ? 1 : 0`; else clears `F+0x754` (`lines 35-41`).
  - loop `[F+0xda8,F+0xdb0)` → `FUN_000fc470(*e, *(F+0xdc0))`; if `*(F+0xdc8)` → `FUN_001297a0()` (`lines 42-48`).
  - `FUN_000357f0(DAT_02ea4718,0)` (job system); `*(*G+0x40)=F` publishes F into R (`line 50`); `FUN_0003b460(G+0x10)`, `FUN_0003b460(G+0x20)`, `FUN_0003b450(G)`.
- Vulkan mapping: frame publish + per-frame CPU resource reset; no GPU state.

## 3. RenderThread_WaitGateTimed_C — 0xbd0a0 (94 B)
- Callers: `FUN_00534690`, `FUN_00536480`, `FUN_0025ae80`. (`000b/000bd0a0.c`)
- `lVar1=*param_1` (=R). `FUN_000323d0(t)` timer start; `FUN_0003b4a0(R+0x10)` lock; `fVar2=FUN_00032430(t)` elapsed ms;
  `R+0x30 += fVar2` (`line 16`). Lock field `R+0x10`, accumulator `R+0x30`.
- Vulkan mapping: CPU wait-gate timing; becomes the frame-ticket fence wait (S2 in E-10).

## 4. RenderThread::FrameJobEntry — 0xcef90 (5 B) and ThreadFunction — 0xbd6d0 (686 B)
- `0xcef90`: `FUN_000cefa0()` — 5-byte jmp thunk; two instances are registered as job entry bodies (`000c/000cef90.c`).
- `0xbd6d0(param_1, param_2)` creates the "RenderJob" (`000b/000bd6d0.c`):
  - Locks `param_1` and `param_1+0x20` (`FUN_0003b4a0`, `lines 51,112`).
  - Waits the gate `FUN_0003b470(param_2+0x18)` (`line 52/116`).
  - Builds a job param block: `local_1d0/local_160 = "RenderJob"`; entry fn ptrs `&LAB_000d1b00` and `&LAB_000cef90`
    (`lines 86-98`); vtable `PTR_FUN_01597a08`; inline data words `{0xfa0c1a, 0xfa1672}` and `0x104e` (`lines 54,109`);
    flag `local_80[8] |= 0x11`, `local_80._12_4_ = 3` (`lines 99-104`).
  - `FUN_00035480(DAT_02ea4718, &local_160, 1, 0, &local_180, 3, -1)` submits the job (`line 111`); `FUN_00035730(...)` waits (`line 113`); invokes `local_88[3]` (a vtable callback, `line 114`).
- Vulkan mapping: render-job construction; the job body is `0xcefa0`. No GPU state.

## 5. RenderThread::EndFrameViewParams — 0xcefa0 (3599 B) — the frame body
- Caller: `FUN_000cef90` (via FrameJobEntry). `param_1 = R`. (`000c/000cefa0.c`)
- Local frame state: `local_d28` = copy of `F+0x280` (0xac8 B, `line 153`), aliased as `auStack_230._24_8_`; `local_260` = the
  job ctx passed to every callee.
- Reads/writes (proven):
  - `lVar2 = *(*(R+0x40)+0xd98)` = M (line 147); `lVar3 = *(*(R+0x40)+0xdc8)` = water ctx (line 148).
  - `FUN_00032420(R+0x38)` (timer); `FUN_001a2cd0(*(R+0x50), cfg)`; `local_980` zeroed if `*(*(R+0x40)+0xd90)!=0` (lines 152-157).
  - `FUN_00118440(DAT_02edf140, local_260)` (line 159) → `0x90860` (frame setup, §5.1).
  - `fStack_170 = *(float*)(F+0xd68)` (line 166); copies `*(M+0x2b80..0x2bc8)` (10 qwords) and `FUN_00092140` binds them (lines 169-180).
  - Config-block reads (`local_d28`): `+0x1e4,+0x1e8,+0x1ec` → `FUN_000cedf0(id, val)` GPU copy-count labels (`lines 220-222`);
    `+0x42c` (line 279), `+0x279` (line 314), `+0x246` (line 344), `+0x4d4` (line 403), `+0x174/+0x163` (line 356).
  - Per-frame view/light record at `local_260._24_8_ = M` then `*(M+0x2b80..)`; `F+0xd88` fov, `F+0xd8c` written:
    `*(float*)(F+0xd8c) = fVar23*fVar24` (line 296) — projection scale.
  - Publishes `M+0x24f0..0x2538` (light/view block) via `FUN_00092140` (lines 459-469).
- Callee chain (all proven, in order): `00032420`, `001a2cd0`, `00118440`(→`90860`), `000cedf0` x3, `0019fed0`, `0016c500`,
  `00187510`, `0008b7f0`, `0016bb70`, `000c95c0`, `000bc010`, `000bbb00`, `000cfdb0`, `0018a000/0018a170`, `00129580`,
  `000ca6d0`, `000d0040`, `0016b8c0`("main cam"), **`000caed0` (§6, MainCamera)**, `000e2d70`, `0019d410`, `0012a930`,
  `000d04e0`, `000bbbe0`, `000da7d0`, `00092140`, `0011d040`, `0011d140`, `000c4de0`, `000da110`, `000e48b0` (water readback),
  `001038a0`, `00118470` (frame tail + flip), `000c5560` (submit), `000ccfd0` (release), `00090ea0` (frame fence spin),
  `0003b450` x2.
- `M+0x18..0xa0` matrices written from `auStack_230._0_8_+0x94..` (lines 406-455): three 4x4 blocks copied into the view
  manager (`+0x8`, `+0x38`, `+0x68`), then `FUN_0016bc50`/`0016c520` finalize; flag `M+0xb0` bits set from `uStack_164` (lines 451-455).
- Vulkan mapping: this is the frame orchestrator (render graph root). "EndFrameViewParams" = end-of-frame view-constant upload.

### 5.1 Frame setup FUN_00090860 (1306 B, caller `00118440`)
- `param_1 = DAT_02ed33f0` (video-out owner O); `param_2 = S` (config). Sets view mode `M+0x3b70` from
  `S+0x4b8`/`+0x4c0` (K6.5); resets the context list; per-frame scopes rotate; `FUN_00090470` builds the DCB/CCB context.
- Vulkan mapping: per-frame render-target/context setup + view-mode (resolution) select.

## 6. RenderView::MainCamera — 0xcaed0 (6077 B)
- Callers: `FUN_000cefa0` (line 339), `FUN_000ccdf0` (cube-map face). `param_1=R`, `param_2=F+0xb8`, `param_3=job ctx`. (`000c/000caed0.c`)
- Proven sequence:
  1. `lVar12=param_3[9]` (config); `uVar15=param_3[4]`; `uVar14=*(R+0x8f8)` (shadow config).
  2. `FUN_000d9cf0(*(R+0x80), ctx)` → schedules "VirtualGeometryRendering::PreRenderJob".
  3. Wait loop over `[param_2+0x1a8, param_2+0x1b0)` (0xa8-stride jobs) using `FUN_00090430(...,0x300000,0)` (`lines 185-192`).
  4. `FUN_0086edc0(cb, ctx[0x1d]+0x38, -1, 5, 1)` = WAIT_REG_MEM on label slot 7 (`line 193`).
  5. `FUN_000cc690` (RenderSpotLightShadows, §9.2), **`FUN_000cc9b0` (RenderShadows, §9.1)**.
  6. `FUN_00092540(cb, 2, ctx[0x1d]+0x80)` = EOP label slot 0x10 (`line 196`).
  7. Binds `M+0x29a0` (SetA) via `FUN_00092140` + `FUN_000be1f0` (SetViewParams) + `FUN_000bdfd0`.
  8. Schedules "ForwardRendering::CreateClustersJob" (`FUN_000aaf80`, `R+0x78`).
  9. Fur pre-render, particle lightmaps, `FUN_000c59d0` (§10, G-buffer) on `param_1+0xd8` (=R+0xd8 draw table).
  10. Schedules "FogRendering::UpdateJob"; `FUN_000c3600` (0xe), `FUN_00187ce0`, `FUN_0011d240` (deferred pass list).
  11. `FUN_00093d70`/`FUN_00093960` binds; `FUN_00091fb0`; `FUN_000a20b0`; `FUN_000c3c70` (Alpha/Transparent);
      `FUN_000c3ec0(param_1, lVar11, ctx, 0x12, 0x24)` (TransparentDepth); `FUN_00103a40`/`FUN_00109ab0`/`FUN_0010a5e0` (particles);
      `FUN_000bbbf0`, `FUN_000bb840`; "RenderSpecularIBL" (`FUN_000c4210`), "RenderDebug"/"RenderDebugCollision" (`FUN_000c46c0`,`FUN_000c4a50`);
      `FUN_000c4f80` (EditorZFail); `FUN_000c4f80`; `FUN_001a2800` (particle draws); label waits; `FUN_00035590` per target slot
      `RC+0xe28..0xea0` (`lines 623-686`).
- Writes (proven): pass ctx `+0xf4` (k) set per sub-pass; `ctx[6]` target; `ctx[0xc]`; `ctx[0x1e]=...`; draw counters `G+0x220384..+0x2203a4` (lines 687-713).
- Vulkan mapping: main-scene pass graph (shadow → depth → G-buffer → forward/particles/decal → debug). No single Vulkan call; maps to the ordered render passes.

## 7. Camera / view derivation
- The actual view/projection math is `FUN_000be1f0` **SetViewParams** (10622 B), called from `000ce850`, `000cd330`, `000caed0`,
  `000cefa0`, `000d05f0`, `000cd7a0`, `000c6810`, `00118470`, `000ca6d0`. (`000b/000be1f0.c`)
  - Inputs: `param_1 = ctx`; `param_2` optional override. Reads camera object `param_1[6]` (`pfVar31`) and config `param_1[9]` (`lVar16`).
  - Output buffer `pfVar27 = FUN_0008c120(DAT_02ee3ce0+0x408, &local_60)` (a persistent per-frame view-constant block).
  - Writes (proven, sample): `pfVar27[0..0xf]` = world matrix (from `pfVar31[3..0x12]`); `pfVar27[0x10..0x1f]` = inverse world;
    `pfVar27[0x20..0x2f]` = camera basis (from `pfVar31[3]`); `pfVar27[0x30..0x3f]` = inverse basis; `pfVar27[0x40..0x4f]` = view
    matrix (built from `lVar33` camera block, lines 555-593); `pfVar27[0x50..0x5f]`, `[0x68..0x7f]` = projection rows;
    `pfVar27[0x80]=width`, `[0x81]=height`, `[0x82]=1/w`, `[0x83]=1/h`, `[0x84..0x87]` = near/far/far-near/near*far,
    `[0x93]=F+0xd8c` (proj scale, line 1105), `[0x98..0x9b]` = depth-linearization, `[0xa0..0xaf]` = frustum planes,
    `[0x150..0x15b]` = rotation basis, `[0x210..0x216]` = jitter offset. `param_1[0x1c]` and `param_1[0x13]` get target addrs (lines 1333-1336).
  - `FUN_000c0b70(ctx, pfVar27)`, `FUN_00092640`, `FUN_00092b50` (dispatch with label wait) finish the constant upload.
- MainCamera (0xcaed0) does **not** compute camera matrices itself; it calls SetViewParams (via `FUN_000be1f0`) and
  `FUN_000c95c0` (creates two 0x1228-byte view-constant blocks; `param_3[0x20/0x28/0x30]` point at them; `param_3+0x98` = view id).
  `000c/000c95c0.c` builds the view-projection from `param_2` (=F+0xb8) rows `+0x8..+0x70` and writes the block; sizes from `V+0x1fb8`.
- Vulkan mapping: the SetViewParams output block is the UBO/user-data view constant buffer; `pfVar27` maps to a uniform
  buffer + push constants (view/proj matrices, 1/w,1/h, near/far, frustum planes). `F+0xd8c`/`F+0xd90` = projection
  override (FOV tangent / orthographic toggle).

## 8. Culling: predicate source (inventory gap G3)
- **Emitters (proven):**
  - `0x86e770` SET_PREDICATION **enable** (135 B): header `0xc0012000` (opcode 0x20, 2 body dwords); `dw1 = addr & 0xfffffff0`;
    `dw2 = (addr>>32 & 0xff) + 0x10000 + ((op&1)<<12) + ((hint&1)<<8)`; sets `cb+0x28 bit0` (`0086/0086e770.c:17-22`).
    **No static caller** (header caller list empty; `grep` finds no other file referencing it).
  - `0x86e800` SET_PREDICATION **disable** (83 B): header `0xc0012000`, `dw1=0`, `dw2=0`; clears `cb+0x28 bit0` (`0086/0086e800.c:17-20`).
    **Called only by `0x11d3d0` line 62** (occluder pass), site `0x11d3d0+0x1a0 = 0x11d570`.
  - `0x85cea0` COND_EXEC **driver** (116 B, no caller): BEGIN (`rsi!=0`) reserves 5 dwords via `0x86f680(cb,5)` and stores
    `cb+0x150=cursor`, `cb+0x158=predaddr`; END (`rsi==0`) rewinds cursor to `cb+0x150-0x14`, calls `0x86ea50(cb, cb+0x158, (end-start)>>2)`,
    restores cursor, clears the fields (`0085/0085cea0.c:9-23`).
  - `0x86ea50` COND_EXEC **emitter** (116 B): header `0xc0032200` (opcode 0x22, 5 dwords total); `dw1/dw2 = predaddr(48b)`;
    `dw3/dw4 = (size & 0x3fff) << 0x20` (`0086/0086ea50.c:17-20`). Called only by `0x85cea0`.
  - `0x85d260` COND_EXEC driver on the **second stream** (114 B): identical logic on `cb+0x10`/`cb+0x20` with state at
    `cb+0x1a48`; its emitter `0x883300` (116 B) writes the same `0xc0032200` packet (`0085/0085d260.c`, `0088/00883300.c`).
- **G3 answer (proven mechanism, source address unknown):** both SET_PREDICATION and COND_EXEC carry a **48-bit memory
  address** and the CP evaluates the predicate by reading the value at that address — this is a **memory-value predicate**,
  not a hardware occlusion-query counter. The enable's predicate address is supplied by its (unfound) caller; the only
  statically reached site writes **address 0** (disable). Therefore: **source = memory value; the producer of that value is
  unknown** (it may be an occlusion-query result written by EVENT_WRITE_EOP, but no such producer is tied to the address in
  the decomp). Do not assume a query object. **G3 not fully closed.**
- Vulkan mapping: VK_EXT_conditional_rendering (`vkCmdBeginConditionalRenderingEXT` reads a 32-bit value from a buffer), or
  CPU-side skip (the game already CPU-consumes XPR masks; see §8.1). The port currently drops it (liverpool logs
  "Unimplemented IT_SET_PREDICATION", `direct-vulkan-inventory.md` §5).

### 8.1 Per-group draw loop / occluder pass — 0x11d3d0 (464 B)
- Callers: `000c1860`, `000c2fa0`, `000c4de0`, `000c4f80`, `000c3320`, `000c3ec0`, `000ce850`, `000cd330`, `000c1ce0`,
  `000c3820`, `000c3a50`, `000c1fc0`, ... (`0011/0011d3d0.c:1`). Signature `(R, list, ctx, group)`.
- Proven body: `lVar9 = *(list+0x68+0x20*g)` (begin), `*(list+0x70+0x20*g)` (end) — vector of 0x20-byte draw records.
  For each record `puVar3=*(rec+8)`: `FUN_00095580(puVar3)` (load); `lVar8=FUN_0014d7b0(*puVar3)` (material);
  `FUN_0016b850(lVar8+0x38, ctx, 4)` (scope push); `FUN_000f8c90(lVar8, ctx)` (bind slot `ctx+0xf4`);
  `FUN_00123cc0(puVar3[2], ctx, rec+0x14, puVar3[5]!=0)` (instance V#, → `0x123810`);
  `uVar6=FUN_00095320(*(lVar8+0x468), ctx+0x90, DAT_02ed1f54)` (masked root table); `FUN_000ae150(ctx[0], uVar6)` (user-data);
  `FUN_001222e0(puVar3, ctx)` (draw). Ends with `FUN_0086e800(cb)` = SET_PREDICATION disable (`line 62`).
- Scope name = `(&PTR_s_kRS_Depth_Occluders_01597ea0)[g]` (`line 25`). Returns `(end-begin)>>5` (record count).
- GPU output: per-draw material/user-data/instance state + DrawIndexAuto/Offset; ends by disabling predication.
- Vulkan mapping: a per-group draw loop; the predicate disable = end conditional rendering.

## 9. Shadows
### 9.1 RenderShadows — 0xcc9b0 (1086 B)
- Caller: `FUN_000caed0` (line 195). `param_1=R`, `param_2=*(R+0x8f8)`, `param_4=job ctx`. (`000c/000cc9b0.c`)
- Scope "RenderShadows" / "CascadedShadows" (`lines 52-63`).
- Gate: `param_2!=0 && *(param_4[9]+0x188)==0` (`line 64`). MRT-bind `M+0x31c0..0x3208` via `FUN_00092140` (`lines 66-76`);
  clear `FUN_000932a0(0, 0x3f80000000000000, 0x3f800000, cb, 2, 0)` (kind 2) (`line 77`).
- **Cascade selection (proven):** if `*(param_4[9]+0xe9)!=0 && (*(byte*)(param_2+0x170)&1)!=0`, loop
  `lVar5 = 0,-1,-2,-3,-4` (5 iterations, `while(lVar5 != -5)`), with:
  - `param_4[6] = *(RC+0xdf8 + lVar5*8)` per-cascade target (`RC=param_4[2]`);
  - `*(param_4+0x10) = *(param_2+0x134 + lVar5*8)` and `*(param_4+0x84) = *(param_2+0x138 + lVar5*8)` (cascade split / bias);
  - `*(param_4+0x11) = lVar5+4` (cascade index 4..0);
  - `param_4[0xc] = param_1+0x4a40` (draw table, stepped −0x828 per cascade);
  - `param_4[0xe] = (&PTR_s_RenderShadowCascade_4__Super_Clo_01597a00)[lVar5]` (negative index into the name array);
  - `uVar3 = *(RC+0xe50 + lVar5*8)` (per-cascade label);
  - if `lVar5!=0 && *(param_4[7]+lVar6)` (lVar6 starts 0xacfc, −0x50/cascade): job "IndexBufferCullShadowCascade" (`FUN_000cdb00`);
  - job "RenderShadowCascade" (`FUN_000cdbc0`).
  - After the loop: `FUN_00869f50(0, cb)` (depth clear) then `FUN_00093d70` for the 5 shadow targets `RC+0x2120..0x2140` (`lines 128-137`).
- `FUN_000cdbc0` (per-cascade): reads `param_1[3]+0x2400 + (idx+0x23)*0x50` (the cascade view-constant block), MRT-binds,
  runs `FUN_000ce850` (the cascade caster draw).
- Vulkan mapping: 5-cascade shadow map pass; each cascade = a depth target + a caster draw pass + cull dispatch.

### 9.2 RenderSpotLightShadows — 0xcc690 (788 B)
- Caller: `FUN_000caed0` (line 194). Gate `*(*(param_2+0x48)+0x118)!=0`. Loops `n = *(int*)(RC+0xfe0)` spot lights
  (`param_2[2]`), building per-light jobs "RenderSingleSpotShadow" (`FUN_000cd330`), `param_2[0x60]=R+0x900 + n*0x828`,
  name `PTR_s_RenderShadowSpot_0_015979c0[n]` (`000c/000cc690.c:57-106`).
- Vulkan mapping: spot-light shadow array pass.

### 9.3 RenderSunShadowProxy / shadow proxy chain
- `FUN_000cdcc0` "RenderSunShadowProxy" (`000c/000cdcc0.c:41`) → `FUN_000ce850` (draws proxy) + `FUN_00093d70` + label.
- `FUN_000cd680` "ParticleBlocker", `FUN_000cd7a0` "RainBlockerView" (`000c/000ca6d0.c:110-166`).

## 10. Depth pre-pass / G-buffer
### 10.1 DepthPrePass runner — 0xc1860 (1033 B)
- Caller: `FUN_000c59d0` (via `FUN_000c12a0` job "RenderDepth"). `param_1 = ctx`. (`000c/000c1860.c`)
- Scope "DepthPrePass" (`lines 37-47`). MRT-bind `V+0x29a0` (SetA, depth-only) via `FUN_00092140` (`lines 48-58`).
- Reads pass flag word `*(uint*)(param_1+0xf8)` (`line 90`); sets `param_1+0xf4` = 1/10/12 and runs group draws:
  bit1→`FUN_0011d3d0(R, list, ctx, 0)`; bit2→`FUN_0011d3d0(R, list, ctx, 1)` + `FUN_000db700` + `FUN_0011d7a0(R, list, ctx, 0x10)`;
  bit4→group 2; bit8→group 3 + `FUN_0011d7a0(...,0x11)`; bit0x10→group 4 + `FUN_0011d7a0(...,0x12)` (`lines 90-134`).
- Vulkan mapping: depth-only pre-pass into `kRTT_MainDepth` (Z32F); groups 0..4 = occluder/normal/fade cull groups.

### 10.2 Depth pass object — 0x93ef0 (1067 B)
- Callers: `FUN_00093d70`, `FUN_00094320`. `param_1=ctx, param_2=target, param_3=depth-owner, param_4=flag`. (`0009/00093ef0.c`)
- Gate: `*(char*)(param_2+0x62)==2 && (param_2!=param_3 || (param_4^1)!=0)`.
- Reads target block `param_2+0x164..0x198` (DB_Z_INFO..DB_STENCIL_WRITE_BASE, 12 dwords; pass-targets §5) and depth owner
  `param_3+0x164`; emits: `FUN_0086ced0(cb,0x2c)` (EVENT_WRITE), `FUN_00869ce0(cb,0,0xcc)`, `FUN_008688e0`/`FUN_00868e90`
  (depth format), `FUN_0086a290(cb, &block)` (DB_Z_INFO..STENCIL_WRITE_BASE + DB_DEPTH_VIEW/HTILE/INFO), `FUN_0087c620/680/670`
  (blend off), `FUN_0086dd20(cb,...)` (DB_RENDER_CONTROL), `FUN_0087d070/d0a0/d0e0/d0c0/d100/d120` (depth stencil state),
  `FUN_00091e10`, `FUN_00091cd0`, `FUN_00869990(cb,0)`, `FUN_0085de40(0.5,0.5,cb,0,0,w,h)` (viewport), `FUN_0086d160(cb,0)`,
  `FUN_00861600(cb+0x2e8,0,0)`, `FUN_00093690(param_1)` (fullscreen clear draw), EOP + WAIT.
- Vulkan mapping: depth attachment bind + depth-clear + depth-stencil pipeline state (no colour); `0x164` block = VkImage depth view + clear.

### 10.3 G-buffer / main scene — 0xc59d0 (2936 B)
- Caller: `FUN_000caed0` (line 272). `param_1=R, param_2=F+0xb8, param_3=job ctx, param_4=R+0xd8`. (`000c/000c59d0.c`)
- Proven: `FUN_000c53d0` ("XPR_IndexCullObjectsNormal", "XPR_ProjectedDecalCull") (`lines 159-213`); `param_3+0x1f = 0x1f`
  (flag word) (`line 188`); MRT-bind `V+0x29a0` + `FUN_000932a0(...,6,0)` clear (`lines 162-174`); job "RenderDepth"
  (`FUN_000c1860`) (`lines 189-206`); `FUN_000a6640`; MRT-bind `V+0x2590` + clear kind 1 (`lines 214-227`); conditional
  MRT-bind `V+0x2a40` (GBuffer) + `V+0x24a0` (`lines 228-264`); `FUN_0086edc0` labels `+0x18/+0x30`; MRT-bind `V+0x2a40`;
  jobs "LightRendering::ResolveSunShadowJob", "RenderGBufferBakedPerVertex" (`FUN_000c1ce0`), "RenderGBufferBakedPerProbe"
  (`FUN_000c1fc0`) (`lines 296-340`); then "FurRendering::PostRenderJob", "RenderDecal" (`FUN_000c28c0`), `FUN_000d79c0`
  ("DoSimulatedClears"), `FUN_000c2fa0` ("SkyboxOpaque"), `FUN_000c3320` ("SkyboxAlpha").
- Target setup (from K5/K6, proven): `V+0x2a40` = 7 colour slots (MRT0 Albedo_AmbientOcclusion, MRT1 Normal_Roughness,
  MRT2 PBR, MRT3 ExtraPBR, MRT4 Motion, MRT5 Ambient0, MRT6 Ambient1) + depth kRTT_MainDepth; `V+0x29a0` = depth-only.
- Vulkan mapping: G-buffer MRT pass (7 colour attachments + depth) followed by forward/transparent/skybox passes.

## 11. Callee chains (summary, all proven edges)
- Frame: `0x25ae80 → 0xbcde0 → (publish F) → 0xcef90 → 0xcefa0`.
- `0xcefa0 → 0xcaed0 → {0xcc690, 0xcc9b0, 0xc59d0, 0xc1860, 0xc1ce0, 0xc1fc0, 0xc28c0, 0xc2fa0, 0xc3320, 0xc3ec0, ...}`.
- `0xcaed0 → 0xbe1f0 (SetViewParams) → 0xc0b70/0x92640/0x92b50`.
- Cull: `0xc1860/c1ce0/c1fc0/... → 0x11d3d0 (group loop) → 0x11d7a0 (cull job) → 0x19af20 → 0x1a6d20/0x1a7290`; ends `0x86e800`.
- Shadow: `0xcc9b0 → 0xcdb00/0xcdbc0 → 0xce850`; `0xcc690 → 0xcd330`.
- Depth: `0xc59d0 → 0xc1860 → 0x11d3d0`; `0x93d70 → 0x93ef0 → 0x86a290/0x869f50`.

## 12. Open gaps / unknowns
1. **G3 (not closed):** predicate producer address (SET_PREDICATION enable has no static caller; COND_EXEC drivers have no
   static caller). Source is a memory value; the writer of that value is unknown.
2. Camera view-update vtable target at game-object `+0x2670` (`param_1[0x4ce]` in `0x25ae80`) is not resolved.
3. `0x86e770` (predicate enable) call site: not in the decomp; confirm via a run counter.
4. `M+0x2288` writer (K6.4) still unknown; affects Neo-mode shadow/target views.
5. `0x90860`'s `S = param_2[9]` identity (K6.5) unknown.
