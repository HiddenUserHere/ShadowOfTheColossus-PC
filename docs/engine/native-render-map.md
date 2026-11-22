# Native render map — the contract for the RenderJob rewrite

Branch `native/render-vk13` (base `upstream/main` `53fd0292`). This is the Phase 0.2 deliverable:
for every function in the rewrite scope, the exact guest structure fields read/written, the GPU
state produced, and the Vulkan call it maps to. Each Phase 2 function is written and validated
against this document.

Read alongside:
- `native-render-seam.md` — proves the GNM library is statically linked in the eboot and the
  native backend bypasses it (so library-internal helpers are specification, not work).
- `native-render-map-frame.md` — frame / camera / culling / shadow, full field-level detail.
- `native-render-map-pass.md` — pass / draw-object / material-bind / target, full detail.
- `native-render-gaps.md` — the live gap register.
- `direct-vulkan-inventory.md` — the PM4 writer inventory (superseded as destination, kept as spec).

## 0. The seam (from `native-render-seam.md`)

```
game RenderJob (0xc*****)                <-- Phase 2 reimplements THESE callers
   |  calls
   v
static GNM library (0x85xxxx/0x86xxxx)   <-- builds PM4; bypassed by the native backend
   |  + sceGnm* system calls (PLT 0xf9xxxx -> port gnmdriver.cpp)   <-- also writes PM4
   v
one command buffer -> sceGnmSubmitCommandBuffers (PLT 0x161) -> liverpool.cpp (PM4 -> Vulkan)
```

Phase 2 replaces the **callers** of the GNM library. The unit of work is the `0xc*****` RenderJob
code and the draw-object/pass/bind writers below; the `0x85xxxx/0x86xxxx` builders tell us *what
GPU state* each call sets, which is the mapping table in §6.

## 1. Object model (all offsets proven — see the two appendix maps)

| Symbol | Meaning | Definition |
|---|---|---|
| `G` | global render/sync struct | `DAT_02edf140` |
| `R` | renderer | `*G`; `R+0x40 = F`; sub-renderers at `R+0x48..0xb8`; `R+0x8f8` shadow config |
| `F` | frame object (RenderJob data) | `*(R+0x40)`; `F+0x280` per-frame config (0xac8 B); `F+0xb8` view config; `F+0xd98 = M` |
| `M` / `V` | render-target / target manager | `*(F+0xd98)`; target sets at `V+0x1fb8..0x2a40` |
| `O` | video-out owner | `DAT_02ed33f0` |
| `ctx` | job context (passed to every pass/draw) | `[0]`=cb, `[2]`=RC, `[3]`=V, `[6]`=current target, `[9]`=config, `[0xc]`=draw table, `[0x1d]`=label slots, `+0xf4`=pass slot k, `+0xf8`=pass flags |
| `cb` | device / GNM cursor object (`*ctx`) | `+0x08` end, `+0x10` write cursor, `+0x18` reserve cb, `+0x20` cb arg, `+0x28` flags (bit0 = predication); `+0x2e8` material block; `+0xc548` draw kind |

## 2. Frame / thread / submit

| VA | Function | Size | Vulkan mapping |
|---|---|---|---|
| `0x25ae80` | GameFrame_Update (game-thread driver) | 497 B | host frame-loop entry; CPU fence only |
| `0xbcde0` | RenderThread_Kick (publishes F, copies config block) | 382 B | frame publish + CPU resource reset |
| `0xbd0a0` | RenderThread_WaitGateTimed_C | 94 B | frame-ticket fence wait |
| `0xbd6d0` | RenderThread_ThreadFunction (creates "RenderJob") | 686 B | job construction |
| `0xcef90` | RenderThread::FrameJobEntry (jmp thunk) | 5 B | — |
| `0xcefa0` | RenderThread::EndFrameViewParams (**frame body**) | 3599 B | render-graph root |
| `0x90860` | frame setup (view mode, DCB/CCB context) | 1306 B | per-frame RT/context setup |
| `0x90ea0` | RenderBackend_Frame (frame-slot fence, parity toggle) | 351 B | timeline semaphore / frame slot |
| `0xc5560` | RenderThread::SubmitJob (package DCB/CCB arrays) | 1128 B | `vkQueueSubmit` package |
| `0xd07c0` | KickoffSubmitJob (submit + flip + submit-done) | 394 B | `vkQueueSubmit` + present + wait-idle |
| `0x118470` | Frame::InsertWaitFlipDone (HUD pass, VBlank) | 2095 B | HUD pass + present wait + gamma |
| `0x956e0` | Flip::SubmitAndWaitVblank | 117 B | `vkQueuePresentKHR` + acquire/wait |
| `0x1469d0` | ResourceFactory::WaitForAllCmdsToFinish | 362 B | `vkDeviceWaitIdle` / fence set |

## 3. Camera / view

| VA | Function | Size | Notes |
|---|---|---|---|
| `0xcaed0` | RenderView::MainCamera | 6077 B | main-scene pass graph; does **not** compute matrices itself |
| `0xbe1f0` | SetViewParams (**the camera math**) | 10622 B | writes the per-frame view-constant block (`pfVar27`): world/inv-world, camera basis, view matrix, projection rows, `1/w`,`1/h`, near/far, frustum planes, jitter |
| `0xc95c0` | view-constant block builder (2× 0x1228 B) | — | builds view-projection from `F+0xb8` rows; sizes from `V+0x1fb8` |

Vulkan: `pfVar27` → a uniform buffer + push constants (view/proj matrices, `1/w`,`1/h`,
near/far, frustum planes). `F+0xd8c`/`F+0xd90` = projection override (FOV tangent / ortho toggle).

## 4. Culling

| VA | Function | Size | Notes |
|---|---|---|---|
| `0x11d3d0` | per-group draw loop / occluder pass | 464 B | 0x20-byte draw records; ends with SET_PREDICATION disable (`0x86e800`, site `0x11d570`) |
| `0x86e770` | SET_PREDICATION enable | 135 B | header `0xc0012000`, 48-bit predicate address |
| `0x86e800` | SET_PREDICATION disable | 83 B | header `0xc0012000` + zeros |
| `0x85cea0` / `0x85d260` | COND_EXEC begin/end driver (2 streams) | 116 B / 114 B | stages a NOP marker, back-patches the COND_EXEC packet |
| `0x86ea50` / `0x883300` / `0x866360` | COND_EXEC emitter | 116 B | header `0xc0032200`, 48-bit address + count |

**Predicate semantics (G3 closed):** the predicate is a **memory value** — the CP reads the value
at a 48-bit GPU address; it is **not** a hardware occlusion query. The port already implements
this: `liverpool.cpp:2136` `PM4Opcode::CondExec` does `skip = *Address() == false`. Phase 2.7 keys
off the RenderJob's own predicate value (the argument the RenderJob passes into `0x85cea0`), not
the library-internal emitter. Vulkan: `VK_EXT_conditional_rendering`
(`vkCmdBeginConditionalRenderingEXT`, 32-bit read from a buffer), or CPU-side skip.

## 5. Passes / draws / binds / targets

| VA | Function | Size | Vulkan mapping |
|---|---|---|---|
| `0x93960` | pass A (RT+depth+state, fullscreen rect, EOP+WAIT) | 1029 B | render pass + dynamic state + one fullscreen draw |
| `0x94600` | pass B (clear pass, depth clear) | 1005 B | clear pass (`vkCmdClearDepthStencilImage` / load-op) |
| `0x93ef0` | depth pass object (depth-stencil state, clear) | 1067 B | depth attachment bind + clear + depth-stencil pipeline |
| `0x93690` | clear / fullscreen rect draw object (prim 0x11) | 291 B | `vkCmdDraw(3,1,0,0)` (procedural fullscreen) |
| `0x94d00` | DrawIndexOffset draw object | 204 B | `vkCmdDrawIndexed(count,1,firstIndex,…)` |
| `0x94dd0` | DrawIndexOffset + instances | 231 B | `vkCmdDrawIndexed(count,instances,…)` |
| `0x94ec0` | DrawIndexAuto draw object | 234 B | `vkCmdDraw(count,1,0,0)` |
| `0x10add0` | DrawIndirect draw object | 264 B | `vkCmdDrawIndirect` |
| `0xdb620` | DrawIndexIndirect mesh draw object | 213 B | `vkCmdDrawIndexedIndirect` |
| `0x85fc10` | Material_BindGraphicsShaders_B | 3994 B | `vkCmdBindPipeline` + push constants/UBO (per-stage user data) |
| `0x92140` | render-target set binder | 924 B | bind colour/depth attachments + dynamic viewport/scissor |
| `0x85cbc0` | SetRenderTarget (one colour slot) | 269 B | colour attachment image view + format |
| `0x95320` | root-table builder (masked qword table) | 252 B | descriptor-set-like table of N qwords |
| `0xae150` | user-data table binder | 441 B | per-draw user-data (root table) into bound block |
| `0x123810` / `0x123cc0` | instance/object block writers | 390 B / 44 B | index buffer + instance buffer (V#) + stencil ref |
| `0xc59d0` | G-buffer / main scene | 2936 B | 7-colour MRT pass + depth, then forward/transparent/skybox |
| `0xc1860` | DepthPrePass runner | 1033 B | depth-only pre-pass into `kRTT_MainDepth` |
| `0xcc9b0` | RenderShadows (5 cascades) | 1086 B | 5-cascade shadow map pass |
| `0xcc690` | RenderSpotLightShadows | 788 B | spot-light shadow array pass |

Pass context (`ctx`) field map and pass descriptor (`param_2`) field map: `native-render-map-pass.md`
§1.1/§1.4. Material block (`ctx+0x2e8`) field map: §1.3.

## 6. GPU state → Vulkan mapping (the emitter table)

From `native-render-map-pass.md` §6, re-verified against `pm4_opcodes.h`
(header = `0xc0 | count<<16 | opcode<<8`). This is the table `native_state.cpp` implements.

| Emitter | Header / reg | Effect | Vulkan |
|---|---|---|---|
| `0x86a900` | `0xC0017900` reg `0x242` | VGT_PRIMITIVE_TYPE | `VkPrimitiveTopology` |
| `0x86bed0` | `0xC0002a00` reg `0x2a` | INDEX_TYPE | `VkIndexType` |
| `0x86bf50` | `0xC0012600` | INDEX_BASE | index buffer addr |
| `0x86c000` | `0xC0002f00` | IT_NUM_INSTANCES | instance count |
| `0x86ced0` | `0xC0004600` | EVENT_WRITE | event / marker |
| `0x869a50` | `0xC0016900` reg `0x1e0+i` | CB_BLEND i control | blend state |
| `0x869990` | `0xC0016900` reg `0x8e` | CB_TARGET_MASK | colour write mask |
| `0x869ce0` | `0xC0016900` reg `0x202` | CB_COLOR_CONTROL | blend/logic op |
| `0x869d60` | `0xC0016900` reg `0x200` | DB_DEPTH_CONTROL | depth test/write |
| `0x869dc0` | `0xC0016900` reg `0x10b` | DB_STENCIL_CONTROL | stencil op |
| `0x869bb0` | `0xC0026900` reg `0x10c/0x10d` | DB_STENCILREFMASK | stencil ref |
| `0x869f50` | `0xC0016900` reg `0xb` | DB_DEPTH_CLEAR | depth clear value |
| `0x86dd20` | `0xC0016900` reg `0x0` | DB_RENDER_CONTROL | depth render ctl |
| `0x86be10` | `0xC0016900` reg `0x3` | DB_RENDER_OVERRIDE | depth override |
| `0x86e1a0` | `0xC0016900` reg `0x205` | PA_SU_SC_MODE_CNTL | cull/poly mode |
| `0x86e3a0` | `0xC0016900` reg `0x2de` | PA_SU_POLY_OFFSET_DB_FMT_CNTL | depth-bias fmt |
| `0x86e410` / `0x86e480` | `0xC0026900` reg `0x2e0`/`0x2e2` | PA_SU_POLY_OFFSET | depth bias |
| `0x86a290` | `0xC0016900`/`0xC0086900` regs `0xf/2/5/0x2af` | depth-target block | depth attachment |
| `0x86a1a0` | `0xC00E6900` reg `0x318+0xf*n` | CB_COLORn block | colour attachment |
| `0x86d160` | `0xC0016900` reg `0x2d5` | draw-kind | pipeline select |
| `0x86c5f0` | `0xC0016900` regs `0x290`,`0x2e5` | VGT prim-reset | primitive restart reset |
| `0x86d7b0` | `0xC0026900`/`0xC0066900` regs `0xb4+2i`,`0x10f+6i` | PA_SC_VPORT | `VkViewport` |
| `0x86d950` | `0xC0026900` regs `0xc/0xd` | PA_SC_SCREEN_SCISSOR | `VkRect2D` |
| `0x86a010` | `0xC0016900` reg `0x8d` | PA_SC_WINDOW_SCISSOR | `VkRect2D` |
| `0x86a090` | `0xC0046900` reg `0x2fa` | PA_CL_VPORT_Z | viewport depth range |
| `0x86d4c0` | `0xC0027600` reg `-0x2c00+table[stage]` | 64-bit user SGPR | push constant / UBO |
| `0x86d5e0` | `0xC0001000`+`0xC0007600` | N-dword user SGPR | push constant / UBO |
| `0x86eb60` | `0xC0044700` | EVENT_WRITE_EOP | timeline semaphore / timestamp |
| `0x86edc0` | `0xC0053c00` | WAIT_REG_MEM | `vkCmdWaitEvents` / barrier |
| `0x86ac10` | import `sceGnmDrawIndexAuto` | DRAW_INDEX_AUTO | `vkCmdDraw` |
| `0x86b040` | import `sceGnmDrawIndexOffset` | DRAW_INDEX_OFFSET_2 | `vkCmdDrawIndexed` |
| `0x86b150` | import `sceGnmDrawIndirect` | DRAW_INDIRECT | `vkCmdDrawIndirect` |
| `0x86b3d0` | import `sceGnmDrawIndexIndirect` | DRAW_INDEX_INDIRECT | `vkCmdDrawIndexedIndirect` |
| `0x86b9e0` | import `sceGnmDispatchDirect` | DISPATCH_DIRECT | `vkCmdDispatch` |
| `0x86f700` | import `sceGnmInsertWaitFlipDone` | wait-flip-done | present wait |

## 7. Draw chain (proven)

```
0x11d3d0 (per-group loop, 464 B)
  per 0x20-byte draw record:
    R = 0x14d7b0(record[1])                          (lazy material resolve)
    if R changed: 0x16b850(R+0x38,ctx,4); 0xf8c90(R,ctx)
    0x123cc0(record[2], ctx, record+0x14, record[5]!=0)  -> 0x123810 (instance block)
    T = 0x95320(u32(R+0x468), ctx+0x90, DAT_02ed1f54)    (root table)
    0xae150(ctx[0], T)                                   (user-data bind)
    0x1222e0(record[1], ctx)                             (draw)
  -> 0x1222e0 (84 B): record+0x28==0 ? 0x189060(...) : 0x128d20(record+0x20, ctx)
       -> 0x128d20 (37 B): 0x94d00(*ctx, record+0x38, record+0x40, record+0x48)  [DrawIndexOffset]
```
Draw objects → `0x85fc10` (material bind) → `0x86ac10`/`0x86b040`/`0x86b150`/`0x86b3d0` (draw).
Pass functions `0x93960`/`0x94600`/`0x93ef0` → `0x93690` (fullscreen rect) or the draw objects.

## 8. Open items carried from the appendix maps

Resolved by this phase (now closed in the register): G1 (import seam), G3 (predicate = memory
value), G3a and G8 (de-scoped — library-internal, bypassed), G4/G5/G6/G7 (classified by PM4 header).

Still open, to close during Phase 2 with REA when Ghidra is responsive:
1. `0x95320` return value (decompile is `void`; caller consumes a returned pointer).
2. `0x93840` (pass outer setup, called at `0x93960:18`) — not decoded.
3. Camera view-update vtable target at game-object `+0x2670` (`0x25ae80`).
4. `DAT_00f9f2e0` primitive table contents (indexed by draw mode).
5. Which pass sets each `cb+0xc548` draw kind beyond the `=0` writes in the pass functions.
6. `M+0x2288` writer (Neo-mode shadow/target views); `0x90860`'s `S` identity.
