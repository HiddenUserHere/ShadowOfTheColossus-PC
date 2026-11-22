# Native render map — cull / camera / shadow (Phase 2.7 / 2.8 / 2.9 appendix)

Field-level contract for the culling, camera and shadow paths, derived from the decomp corpus
(`out/re/decomp/<hi16>/<addr>.c`), raw eboot bytes (`out/eboot.elf`; VA → file offset = VA +
0x4000, little-endian) and the port's own PM4 decoder
(`gpu/shadps4/video_core/amdgpu/liverpool.cpp`). Companion to `native-render-map.md` (contract),
`native-render-map-frame.md` (frame/camera appendix — superseded here where this file is more
specific), `native-render-map-pass.md` (pass/device ctx offsets), `native-render-map-draws.md`
(draw objects), `native-render-map-bind.md` (material bind), and `native-render-gaps.md` (the
live gap register).

Tags: **proven** = read in the named decompile line(s) or raw eboot bytes; **inferred** =
reasoning from proven facts; **unknown** = not established. All addresses are EU 1.01 eboot VAs
(CUSA08809).

AI-memory note: `mcp__ai-memory__*` was not reachable this session (server down; wiki read
read-only). No durable pages were written. The Phase-0 register rows G3/G3a and frame-map §8
carry the earlier (contradictory) state of the predication question; this file supersedes them —
see §1.6/§5 and gap rows N2-14/N2-15/N2-16.

## 0. Object model delta (fields used by the functions in this file)

Everything in `native-render-map.md` §1 / `native-render-map-pass.md` §1.1 still holds. Additions:

| Symbol/offset | Meaning | Evidence |
|---|---|---|
| `ctx[9]+0x28` | u32 **marker-enable mask** on the per-frame config: bit0 = pass/group scopes, bit2(4) = per-material push marker, bit3(8) = per-draw set marker | `0016b8c0.c:8`, `0016b850.c:6`, `0016b810.c:6` |
| `ctx[0x1d]` | **EOP label-slot array** base; slots are 8 bytes apart (`+0x10,+0x18,+0x28,+0x30,+0x38,+0x68,+0x78,+0x80,+0x88,+0x90,+0xa8,+0xb0,+0xb8,+0xc0,+0xd0,+0xd8,+0x108,+0x110,+0x130` observed) | `000caed0.c:193,196,270,402,475,513,628`; `000c59d0.c:265,266,296,345`; `000ca6d0.c:104,105,176,177` |
| `RC+0xe88 / +0xe98 / +0xea0` | job counter / job-slot vector begin / vector end (the `0xc12a0` job scheduler) | `000c59d0.c:202-206`; `000caed0.c:678-686` |
| `RC+0xe28..0xe80` | 12 CPU job-completion counters waited at the end of MainCamera (`FUN_00035590` = job wait) | `000caed0.c:630-686` |
| `cb+0x28` (bit0) | **predication-active latch** on the GNM cursor: set by SET_PREDICATION enable, cleared by disable; OR-ed into every draw initiator's flags | `0086e770.c:17`, `0086e800.c:17`; `native-render-map-draws.md` §0 (`(dev+0x28)&1`) |
| `cb+0x150/+0x158` | COND_EXEC **stream-1** staging (saved cursor / saved predicate address) | `0085cea0.c:20-22` |
| `cb+0x1a48/+0x1a50` | COND_EXEC **stream-2** staging (same protocol, cursor at `cb+0x20`) | `0085d260.c:20-22` |
| `cb+0x40` | a second GNM cursor (marker/report sub-stream) used by `0x85cb40`/`0x85c9b0` | `0085cb40.c:10-12` |
| `R+0xd8` | main-scene draw table (`ctx[0xc]` during the G-buffer pass); `R+0x4a40` = shadow-cascade draw table (stepped −0x828 per cascade) | `000caed0.c:266,271`; `000cc9b0.c:79,89,124` |
| `R+0xe0..0xe8, +0x100..0x108, +0x120..0x128` (+0x20-stride copy loop to +0x8e8) | per-view draw tables double-buffered by `0xca6d0` (frame N → N+1 copy) before MainCamera | `000ca6d0.c:49-74` |
| `RC+0xa800` (0xae40-byte XPR block) | shadow render-record block (template + per-slot records), see §2.4 | `000ca150.c:13-18` |
| `RC+0x21e8` (Neo: `+0x2238`) | shared 0xa0-byte **dispatch-draw record** (compute-cull → CPU-draw handoff) | `0011dbe0.c:117-172` |

## 1. Culling (Phase 2.7)

### 1.1 Verdict (one paragraph)

There is **no GPU predication and no occlusion query in the culling path of this title**. The
only live predication packet in the entire build is a `SET_PREDICATION **disable**` (address 0)
emitted unconditionally at the end of each per-group draw loop; the enable emitter
(`0x86e770`) and both COND_EXEC drivers/emitters (`0x85cea0/0x85d260/0x86ea50/0x883300/0x866360`)
have **zero static callers and zero data references** (corpus `rg` + raw byte scan, §1.6). All
culling is decided before the CP: CPU cull jobs trim the per-group record vectors (§1.2), the
XPR system gates whole batches behind a compute-written CDB word + `WAIT_REG_MEM` label
(§1.5), and batch-draw counts are rewritten into indirect-args memory by compute, consumed via
`SET_BASE` + `DrawIndexIndirect` (§1.4). The Vulkan equivalent of the whole mechanism is
**host-side draw-list filtering + zero-count indirect draws** — not
`VK_EXT_conditional_rendering` and not occlusion queries.

### 1.2 `0x11d3d0` — per-group draw loop ("occluder pass"), 464 B

Signature `(R, list, ctx, group)` — `param_1` (R) is **unused** in the body; callers pass
`DAT_02edf140` (G) by convention (`000c1860.c:94,100,121,126,132`). Returns
`(end − begin) >> 5` = record count (`0011d3d0.c:68`).

Body (proven `0011d3d0.c`, line numbers cited):

1. `:19-23` group g selects the record vector at `list+0x68+0x20*g` (begin) /
   `list+0x70+0x20*g` (end); empty ⇒ return 0.
2. `:25` scope push `FUN_0016b8c0(local,(&PTR_s_kRS_Depth_Occluders_01597ea0)[g], ctx, 1)` —
   a conditional debug marker (see below). The name array (raw eboot, image `0x1597ea0`,
   pointers are raw VAs): `kRS_Depth_Occluders`, `kRS_Depth_Normal`, `kRS_Depth_DiscardTest`,
   `kRS_Depth_Fadeout`, `kRS_Depth_Fadeout_InvLODFade`, `kRS_GBuffer_Ambient_Baked`,
   `kRS_GBuffer_Ambient_Baked_Motion`, `kRS_GBuffer_Ambient_Dynamic` — **group g is the render
   sort-group**, not a cull bucket.
3. `:33-57` per 0x20-byte record: `rec8 = *(rec+8)` (the draw-record object);
   `FUN_00095580(rec8)` ensure-loaded; `R' = FUN_0014d7b0(*rec8)` (lazy material resolve);
   material-change handling:
   - `:38-39` if same material as previous record and already bound → skip rebinding;
   - `:42-44` else if a previous scope is open, pop it `FUN_0016b890(ctx,4)`;
   - `:45` push per-material marker `FUN_0016b850(R'+0x38, ctx, 4)` — **this is the "scope push"
     the older docs guessed at; proven = `sceGnmInsertPushMarker(material_name)` gated on
     `ctx[9]+0x28 & 4`** (`0086f2a0.c:21`, `0016b850.c:6`). Note for gap N2-5: `0x16b850` is
     *not* a draw-kind/material-commit writer; the N2-5 lead is dead (register row updated —
     append-only, see §7).
   - `:46` `FUN_000f8c90(R', ctx)` — bind records for pass slot k (bind doc §2.1).
4. `:51` `FUN_00123cc0(rec8[2], ctx, *(u32*)(rec+0x14), rec8[5] != 0)` — instance/index/stencil
   bind → `0x123810` (bind doc §2.7).
5. `:52-53` root table `FUN_00095320(*(u32*)(R'+0x468), ctx+0x90, DAT_02ed1f54)` +
   `FUN_000ae150(ctx[0], table)` (bind doc §2.7).
6. `:54` `FUN_001222e0(rec8, ctx)` — the draw (§1.3).
7. `:58-60` trailing scope pop; `:62` **`FUN_0086e800(ctx[0])` = SET_PREDICATION disable
   (site `0x11d570`, raw-verified call at VA 0x11d570 targeting 0x86e800)**; `:63` scope pop.

Callers (header): `0xc1860, 0xc2fa0, 0xc4de0, 0xc4f80, 0xc3320, 0xc3ec0, 0xce850, 0xcd330,
0xc1ce0, 0xc3820, 0xc3a50, 0xc1fc0, …` — i.e. every main-scene and shadow-caster group runner.

**What culls what here:** the record vectors themselves are produced by CPU cull jobs upstream
(the `0xc12a0`-scheduled "IndexCull"/"XPR" jobs); `0x11d3d0` draws whatever survived and
reports the count. Its only GPU-side coupling is the (vestigial) SET_PREDICATION disable.

### 1.3 The inner draw dispatch — `0x1222e0` (84 B) → `0x189060` / `0x128d20`

Proven `001222e0.c`, `00123d20.c`, `00189060.c`, `00128d20.c`, `00094bf0.c`:

1. `001222e0.c:8` gate: if bit **2** of the pass-ctx `+0x8` force/dirty qword is clear:
   - `:9-10` `name = FUN_00123d20(*(rec8+0x10))` — resolves `*(*(rec8+0x10)+0xf0)`, loads
     `+0x20`, material-resolves (`0x14d7b0`), returns `material+0x38` (the material **name
     string**);
   - `:10` `FUN_0016b810(name, ctx, 8)` — conditional `sceGnmInsertSetMarker(material_name)`
     gated on `ctx[9]+0x28 & 8` (`0086e670.c:21`, `008843d0.c:21` — one emitter per stream).
   - `:11-13` if `*(rec8+0x28) != 0` → `FUN_00189060(*(rec8+0x28), ctx)` and return.
2. `00189060.c:6-7` (indirect path): `FUN_00094bf0(*ctx, 0)` (primitive type for mode 0) +
   `FUN_000db620(*ctx, args_rec+0x38, 0)` = **DrawIndexIndirect** with the `{lo,hi}` args pair
   at `args_rec+0x38` (draws doc §0xdb620; the pair flows through `SET_BASE` `0x86ad40`).
3. else `00128d20.c:5-8` (indexed path): `FUN_00094d00(*ctx,`
   `*(u64*)(rec2+0x38)` `→ (mode, —)`, `*(u64*)(rec2+0x40)` `→ (—, index_offset)`,
   `*(u32*)(rec2+0x48)` `→ index_count)` — the DrawIndexOffset draw object (`00094d00.c:11-27`:
   prim from `DAT_00f9f2e0[mode]`, `0x862510(mat,0,0)`, material bind `0x85fc10`,
   `0x86b040(cb, index_offset, index_count, 0)`, `0x862510(mat,0,0)`).

`*ctx` above is `ctx[0]` = the pass context's first qword = the cb (pass-map §1.1), consistent
with every draw object taking the cb. `FUN_00094bf0` (`00094bf0.c:8-18`): prim =
`DAT_00f9f2e0[mode]` when pass-ctx `+0x18` byte bits 1-2 are clear, else 9 (RectList); raw dump
of the table head: `DAT_00f9f2e0[0..3] = {4, 6, 2, 3}` (TriList, TriStrip, LineList,
LineStrip); dword 4 onward is unrelated float/resolution data — master-map open item 4 can be
narrowed to modes 0..3.

### 1.4 `0x11d7a0` — XPR batch draw (indirect-count culling), 1066 B

Called by `0xc1860` (after groups 1/3/4), `0xce850` (cascade caster), `0xcd330`, `0xc1ce0`,
`0xc1fc0`, `0xc46c0`. Proven `0011d7a0.c`:

1. `:32-42` `slot = ctx[7] + 0xa800 + id*0x28` (id = param_4 & 0xffffffff, e.g. 0x10/0x11/0x12
   from `0xc1860`); **wait for the slot's job token** `*(u64*)slot == 1` (busy-spin when
   `ctx[9]+0x16` clear, else job-system wait `0x35590`) — the CPU gate for "the compute cull
   for this batch has run".
2. `:44` scope push with the slot's name `*(ctx[7]+0xa810+id*0x28)` (set by `0xca120` from
   `PTR_s_XPR_kSMLT_Shadow_01597980[idx]`).
3. `:47-63` if `*(char*)(*(slot+8)+0x4c)` (batch has dispatch-draw payload) or
   `ctx[9]+0x22`: bind the index buffer from `*(*(slot+8)+0x30)` (BASE `0x86bf50` + SIZE
   `0x86bfb0(0xe00000)` + `INDEX_TYPE(1)` 32-bit, cached at cb `+0x38/+0x39`), then
   **`FUN_0086ad40(*ctx, 0, *(u64*)(*(slot+8)+0x28))`** = SET_BASE (0x11) pointing at the
   **compute-rewritten indirect-args table** (gap N2-8 verdict; predicate bit = 0 → header
   `0xc0021100`).
4. `:65` `FUN_00094bf0(*ctx, 0)`; `:66-69` per 16-byte entry of the batch vector
   (`list+0x8..+0x10`): `FUN_0019af20(*entry, ctx, slot)` — the per-record indirect draws;
   `:70` `FUN_0086c000(*ctx, 1)` (instances restore).
5. `:71-129` optional telemetry CSV ("`%s%s_shadowPass%d_info.csv`").

**Vulkan mapping:** the args table is a host-visible buffer the compute cull writes with
zero/truncated counts; native path = `vkCmdDrawIndexedIndirect` over that buffer (zero-count
draws are the cull). No predication, no query. (`SET_BASE` itself maps to nothing — N2-8.)

### 1.5 Label slots, EOP tags and the compute→CPU gates (not predicates)

- **Label alloc:** `0x86d210(cb, n, align)` emits a count-1 **NOP with inline-data marker**
  header pair `{0xc0001000 | ((n+align_pad+1)<<16), (align<<12) | 0x68750000}` and returns the
  payload GPU VA (`0086d210.c:24-30`). The `0x6875` prefix is the same inline-data marker the
  port decodes as `CbDbExtent` (gap N2-7 evidence, `liverpool.cpp:1488-1494`); for the label
  slots the alignment arg is 3, so dw1 = `0x68753000|…` — **the marker distinguishes label
  allocations from RT-extent hints only by the alignment field**, and the *RT-extent producer*
  remains the open side of N2-12 (no new writer found this session).
- **EOP write:** `0x92540(cb, 2, slot)` = `0x86eb60(cb, DAT_00f9f2c0[2]=0x28, 0, slot, 1, 1,
  DAT_00f9f2c4[2]=0x38, 2)` (`00092540.c:10-11`; raw `0xc0044700` EVENT_WRITE_EOP at
  `0x86eb60`+0x6f). `0x925c0` = the same with a caller-chosen slot (`000925c0.c:6`).
- **CPU wait:** `0x86edc0` = WAIT_REG_MEM (`0xc0053c00` raw at +0x61) on the slot, and
  `0x86eec0` = `sceGnmComputeWaitOnAddress` → WAIT_REG_MEM `{0xc0053c00, mem_flag, addr,
  mask, ref, 10}` (`0086eec0.c:18-28`; port decode `gnmdriver.cpp:176-194` `WriteHeader<Nop>`
  + `WaitRegMem` with `is_mem` when `addr > 0xffff`). These are the *gates* between compute
  culls and draws — e.g. `0011dbe0.c:37-40` waits `RC+0xab70+id*0x28`, then
  `:114` `FUN_00092540(cb,1,RC+0xab78+id*0x28)` + `FUN_0086eec0(cb, …, 0xffffffff, 1)` re-arms
  it; `0011e190.c:19-22` waits `ctx[0x1d]+0x78` the same way.
- Vulkan: semaphore/timeline-value waits or `vkCmdWaitEvents`; the labels live in the DCB
  inline-data area, so the native backend can substitute its own timeline semaphores keyed by
  slot index.

### 1.6 The predication / COND_EXEC emitters — packet bytes and reachability

All packet headers below were **raw-verified** in `out/eboot.elf` (immediate found at the cited
VA offset).

| Function | Size | Packet (raw-verified literal) | Body semantics | Static callers | Data refs |
|---|---|---|---|---|---|
| `0x86e770` SET_PREDICATION **enable** | 135 B | `0xc0012000` (= opcode 0x20, 2 dw body; at +0x57); `dw1 = addr & 0xfffffff0`; `dw2 = (addr>>32 & 0xff) + 0x10000 + ((p3&1)<<12) + ((p4&1)<<8)` | sets `cb+0x28` bit0 | **none** (header list empty; corpus `rg FUN_0086e770` → own file only) | **0** (raw scan for both `0x0086e770` and bias form `0x0146e770` over the whole image) |
| `0x86e800` SET_PREDICATION **disable** | 83 B | `0xc0012000`, `dw1 = dw2 = 0` (at +0x35); clears `cb+0x28` bit0 | — | exactly one: `0x11d3d0` line 62 (site 0x11d570, call rel32 raw-verified) | 0 (the two raw hits of the literal are `call rel32` byte noise) |
| `0x85cea0` COND_EXEC begin/end **driver, stream 1** | 116 B | — | BEGIN (`param_2 != 0`): `0x86f680(cb,5)` reserves a 5-dw NOP marker, saves cursor→`cb+0x150`, addr→`cb+0x158`. END (`0`): rewinds cursor to `cb+0x150−0x14`, calls `0x86ea50(cb, cb+0x158, (end−saved)>>2)`, restores cursor, clears state | **none** | **0** |
| `0x85d260` COND_EXEC driver, **stream 2** | 114 B | — | identical protocol on `cb+0x20` with state at `cb+0x1a48/+0x1a50`; emitter `0x883300(cb+0x10, …)`; NOP stager `0x885220` (its count-1 NOP literal is `0xffff1000`, not `0x80000000` — `00885220.c:17`) | **none** | **0** |
| `0x86ea50` / `0x883300` / `0x866360` COND_EXEC **emitters** | 116 B each | `0xc0032200` (= opcode 0x22, 4 dw body; at +0x51 each); `dw1/dw2` = 48-bit address (`addr & 0xffffffffffff`); `dw3/dw4` = `(count & 0x3fff) << 0x20` (⇒ dw4 = exec_count, dw3 = 0) | — | only the two drivers above (`0x866360`: none at all) | 0 |
| `0x86f680` NOP stager, stream 1 | 117 B | count 1 ⇒ `0x80000000`; count n ⇒ `0xc0001000 | n<<16` + zero | — | only `0x85cea0` | — |

Dead-code proof method: (a) corpus-wide `rg "FUN_<addr>"` — Ghidra renders *every* recovered
reference (direct call, rip-relative lea, data pointer) under that name; (b) raw byte scan of
the whole image for the 32-bit LE VA and its `+0xc00000` loader-bias form (the N2-9 lesson) —
0 hits for each function. The only opcode-0x20/0x22 writers in the corpus are the functions
above (`rg "0xc0012000|0xc0032200" out/re/decomp` → 5 files). Therefore **no DCB emitted by
this eboot can contain a predication enable or a COND_EXEC packet**: at runtime the CP only
ever sees the disable with address 0.

Port cross-reference (already implements the semantics for other titles):

- `liverpool.cpp:1558-1566` — `case PM4ItOpcode::SetPredication:` sotc-modified stub: logs
  `Unimplemented IT_SET_PREDICATION` once ("the game sends it every frame" — that stream is the
  disable from `0x11d570`), no register effect.
- `liverpool.cpp:2136-2152` — `case PM4ItOpcode::CondExec:` reads
  `*cond_exec->Address() == false` and skips `exec_count` dwords via `NextPacket(dcb, …)`;
  sotc additions: `DrainBack()` (queued EOP/EOS/ReleaseMem writers) + `SotcStats::Drop`.
- `pm4_cmds.h:1218-1235` — `PM4CmdCondExec`: `bool_addr_lo` = dw1 bits 2..31 (4-aligned),
  `bool_addr_hi` = dw2 bits 0..15, `command` = dw2 bits 28..31 (0 required),
  `exec_count` = dw4 bits 0..13; `Address()` = `bit_cast<bool*>(hi<<32 | lo)` — **the
  predicate is one byte in guest memory**.
- Port GNM driver: `sceGnmComputeWaitOnAddress` (`gnmdriver.cpp:176-194`) lowers to
  `WAIT_REG_MEM` — used for the label gates of §1.5, the only "conditional" traffic this game
  actually produces.

## 2. Shadows (Phase 2.8)

### 2.1 `0xcc9b0` RenderShadows, 1086 B

Sole caller: `0xcaed0` (MainCamera) line 195. Signature `(R, shadow_cfg = *(R+0x8f8), unused,
ctx)` (`000cc9b0.c:1-5`). Sequence (proven, line numbers cited):

1. `:52-63` scope "RenderShadows" (static name "CascadedShadows", guard `DAT_02ededf8`).
2. `:64` gate: `shadow_cfg != 0 && *(ctx[9]+0x188) == 0` — config present and no override flag.
3. `:65-76` **RT setup**: copies the 10-qword pass-target descriptor array from
   `RC+0x31c0..0x3208` (the shadow target set of the target manager `V`) onto the stack and
   binds it with `FUN_00092140(*ctx, &local_88)` — one RT-set for all cascades; each cascade
   then re-binds per-cascade targets through `ctx[6]` (below) without touching this set.
4. `:77` clear `FUN_000932a0(0, 0x3f80000000000000, 0x3f800000, *ctx, 2, 0)` — clear kind 2
   (depth+colour to 1.0/0? kind 2 = the shadow clear; colour args 1.0f,1.0f).
5. `:78` **cascade gate**: `*(char*)(ctx[9]+0xe9) != 0 && (*(byte*)(shadow_cfg+0x170) & 1) != 0`.
6. `:79-126` **cascade loop** — 5 iterations, `lVar5 = 0,-1,-2,-3,-4` (`while (lVar5 != -5)`):
   - `:84` `ctx[6] = *(RC + 0xdf8 + lVar5*8)` — the per-cascade depth target (current-target
     slot of the job ctx).
   - `:85-87` `*(ctx+0x10) = *(shadow_cfg+0x134 + lVar5*8)` and `*(ctx+0x84) =
     *(shadow_cfg+0x138 + lVar5*8)` — per-cascade split-distance (near) and bias/far float pair.
   - `:88` `*(int*)(ctx+0x11) = lVar5 + 4` — **cascade index 4,3,2,1,0** (read back by the job
     body `0xcdbc0:26`).
   - `:89` `ctx[0xc] = R + 0x4a40 + lVar5*(-0x828)` — the per-cascade draw table
     (`param_1` is stepped by `−0x828` per iteration, `:124`).
   - `:90` `ctx[0xe] = (&PTR_s_RenderShadowCascade_4__Super_Clo_01597a00)[lVar5]` — the scope
     name (negative index ⇒ "RenderShadowCascade_4" down to "_0").
   - `:91` `uVar3 = *(RC + 0xe50 + lVar5*8)` — the per-cascade completion label/job slot.
   - `:92-107` if `lVar5 != 0 && *(char*)(ctx[7] + lVar6) != 0` (lVar6 starts `0xacfc`, stepped
     `−0x50` per cascade — a per-cascade flag in the shadow-config block): schedule the CPU job
     **"IndexBufferCullShadowCascade"** (`FUN_000cdb00`) via `FUN_000c12a0(..., 0x302, uVar3)`.
   - `:108-122` schedule the job **"RenderShadowCascade"** (`FUN_000cdbc0`), same scheduler,
     priority 0x302, label `uVar3`.
7. `:128-137` after the loop: five repetitions of `FUN_00869f50(0, cb)` (SET_CONTEXT_REG
   `DB_DEPTH_CLEAR` = 0.0) + `FUN_00093d70(*ctx, *(RC+0x2120/0x2128/0x2130/0x2138/0x2140))` —
   the depth-cache-touch wrapper (§3.3) run over the five cascade depth targets **on the
   submitting stream** (cache invalidation between the cascade jobs and the main scene).

### 2.2 The cascade jobs

- **`0xcdbc0` RenderShadowCascade (253 B, job body — reached only through the job slot, zero
  static callers).** Proven `000cdbc0.c`: reads cascade index `*(int*)(job+0x11)`; scope push
  from `job[0xe]`; builds the RT-descriptor array from the **per-view record block**
  `ctx[3] + 0x2400 + (idx+0x23)*0x50` — 4 qwords at `+0x2400..+0x241f` (colour/depth target
  descriptors), 4 at `+0x2420..+0x243f`, 2 at `+0x2430+0x10/+0x18` — binds with `FUN_00092140`
  (`:46`), then calls `FUN_000ce850(job)` (`:47`) — the cascade caster draw (which drives
  `0x11d3d0` group loops + `0x11d7a0` XPR batch draws, §1.2/§1.4).
- **`0xcdb00` IndexBufferCullShadowCascade (181 B, job body).** Proven `000cdb00.c`: reads
  cascade id `*(int*)(job+0x88)`; if `*(char*)(*(job+0x48)+0x20)` set, calls
  `FUN_0011dbe0(DAT_02edf140, *(job+0x60), job, id)` — the XPR cull driver: builds the 0xa0-byte
  dispatch-draw record (16 dwords of frustum planes from `ctx[6]` floats `+0x31..+0x3c` + 1.0,
  width/height from the target, view constants from `*(RC+0x21e8+…)` block) into `ctx[0x13]`
  (`0011dbe0.c:117-172`), clears `*(RC+0xabac+id*0x28) = 0`, runs `FUN_0019b330` per XPR record,
  then EOP + WAIT on the per-cascade label (`:173-176`: `FUN_00092540(cb,1,RC+0xab78+id*0x28)`
  + `FUN_0086eec0(cb, …, 0xffffffff, 1)`).
- **`0xcc690` RenderSpotLightShadows (788 B)** — caller `0xcaed0` line 194 (before
  RenderShadows). Gate `*(*(shadow_cfg+0x48)+0x118) != 0`; loops
  `n = *(int*)(RC+0xfe0)` spot lights, scheduling "RenderSingleSpotShadow" jobs (`FUN_000cd330`)
  with draw table `R+0x900 + n*0x828` and scope name `PTR_s_RenderShadowSpot_0_015979c0[n]`
  (body detail in frame map §9.2; caller + scheduling re-verified this session).
- **The shadow render-record block**: `FUN_000ca150` (`000ca150.c:13-24`) allocates the 0xae40-byte
  block per view (`R+0x38`), copies a static template from image `DAT_02ede710` (0x370 B) into
  `RC+0xa800` and `DAT_02edea80` (0x2d0 B) into `RC+0xab70`, then fills the per-slot job-token /
  name / label pointers (`+0xa800+0x28*i` triples: token at `+0x00`, name at `+0x08` (via
  `0xca120` from `PTR_s_XPR_kSMLT_Shadow_01597980[idx]`), job id at `+0x20`, telemetry id at
  `+0x24`). `FUN_000ca6d0` (the shadow-view setup runner, called from the frame body before
  MainCamera) invokes it at `000ca6d0.c:47`.
- **Cascade view constants**: the per-cascade view/projection lives in the per-view record
  block `ctx[3]+0x2400 + (idx+0x23)*0x50` (see §4.3 for the block family and gap N2-16 for the
  untraced producer).

### 2.3 Ordering relative to MainCamera (proven `000caed0.c`)

```
0xcefa0 (frame body): … 0xca6d0 (shadow-view setup: builds shadow view constants,
                      record block, draw tables; SetViewParams per shadow view at :139)
  └─ 0xcaed0 (MainCamera):
       :185-192  flush check (mid-frame submit if any view list non-empty)
       :193      WAIT_REG_MEM on label ctx[0x1d]+0x38   (frame-setup EOP)
       :194      0xcc690  spot-light shadows            (jobs scheduled)
       :195      0xcc9b0  RenderShadows (cascades)      (cull + draw jobs scheduled)
       :196      EOP write ctx[0x1d]+0x80               (shadows-done label)
       :197-203  flush check
       :204-217  ctx[6]=main RT, pass slot 1, RT set V+0x29a0 (SetA, depth-only)
       :218      SetViewParams (main camera constants)
       …         G-buffer / forward passes (§3, §4)
       :629-686  wait the CPU job counters RC+0xe28..0xe80 (shadows included)
```

Shadows therefore run **before** the main RT bind and the main SetViewParams; GPU-side ordering
between cascades is the per-cascade labels (`RC+0xe50+8i`) + the EOP/WAIT pairs each job emits,
not stream order (the jobs record PM4 from worker threads into the same DCB via the shared
`ctx[0]` cursor).

### 2.4 Vulkan mapping (shadows)

- 5 cascades = 5 depth-only passes over `RC+0x31c0`'s depth targets (one VkImage per cascade or
  a 5-layer array — the target descriptors decide, N2-7 decode applies).
- The cull job maps to a compute pass writing the indirect-args buffer; the caster pass draws
  `DrawIndexIndirect` from it (zero-count = culled). The `IndexBufferCullShadowCascade` /
  `RenderShadowCascade` pair = one barrier + two passes in the native graph.
- The five trailing `0x93d70` touches = L2 cache invalidates → `vkCmdPipelineBarrier`
  (or nothing: the native backend owns its own barrier placement).
- `DB_DEPTH_CLEAR = 0.0` writes between touches → load-op clear on the next depth pass.

## 3. Depth pre-pass / G-buffer (Phase 2.8)

### 3.1 `0xc59d0` G-buffer / main scene, 2936 B

Sole caller: `0xcaed0` line 272 (with `ctx[0xc] = R+0xd8` set at `:271`). Signature
`(R, F+0xb8, ctx, R+0xd8)`. Proven `000c59d0.c`:

1. `:159` `FUN_000c53d0(R, ctx)` — the XPR index-cull jobs ("XPR_IndexCullObjectsNormal",
   "XPR_ProjectedDecalCull").
2. `:161-174` if `*(char*)(ctx[9]+0x1c) == 0`: RT set `V+0x29a0` (SetA, depth-only) via
   `FUN_00092140` + clear `FUN_000932a0(0, 1.0f, 1.0f, *ctx, 6, 0)` (kind 6); else `:176-187`
   the same RT set **without** the clear (kRTT_MainDepth reuse path).
3. `:188` `*(u32*)(ctx+0x1f) = 0x1f` — the DepthPrePass **group-enable word** (5 groups, bits
   0..4; consumed by `0xc1860:90`).
4. `:189-206` schedule job "RenderDepth" = `FUN_000c1860` (priority 0x302, label `RC+0xe28`).
5. `:207-213` flush check over the `F+0xb8` view list (0xa8 stride, `+0x90 != +0x98` →
   `FUN_000c5560` mid-frame submit).
6. `:214` `FUN_000a6640(*(R+0xa8), ctx, *(R+0xb8))`.
7. `:215-227` RT set `V+0x2590` + clear kind 1.
8. `:228-264` conditional (all 8 draw-table vectors of `R+0xd8` empty, or `ctx[9]+0x3c`, or
   `ctx[9]+0x198`): RT sets `V+0x2a40` (the 7-colour G-buffer set) and `V+0x24a0`, each with
   clear kind 1.
9. `:265-266` EOP labels `ctx[0x1d]+0x18` and `+0x30`; `:267-278` RT set `V+0x2a40` again
   (final G-buffer bind — the pointer-identity cache makes the rebind free unless the
   intermediate bind changed it, bind doc §3.4).
10. `:279-282` conditional fur pre-render `FUN_00183d10(R+0x90, ctx, F+0xb8+0x20)`.
11. `:283-296` RT set `V+0x29a0` again; `FUN_00093d70(*ctx, *(RC+0x1fb8))` (kRTT_MainDepth
    touch); EOP `ctx[0x1d]+0xd0` via `FUN_00092540(*ctx, 2, …)`.
12. `:297-308` job "LightRendering::ResolveSunShadowJob" (`FUN_000afca0`, priority 4).
13. `:309-340` jobs "RenderGBufferBakedPerVertex" (`FUN_000c1ce0`) and
    "RenderGBufferBakedPerProbe" (`FUN_000c1fc0`) (priority 0x302, label `RC+0xe28`).
14. `:341-455` view-list loop → at `LAB_000c60d7`: EOP `ctx[0x1d]+0xd0`, `FUN_00091fb0`,
    `FUN_000bae70(R+0x48, ctx, RC+0x1fb8)` — the G-buffer draw runner; then "FurRendering::
    PostRenderJob" (`0x183950`), "RenderDecal" (`FUN_000c28c0`), AmbientOcclusion
    (`0x53b90/0x17daa0`), `0x17c6c0`, RT set `V+0x2630`, `0xaea10`/`0xb0570`, the particle
    lightmap passes (`FUN_001a2800` × 14 + the spot-shadow target list), RT set `V+0x24f0`,
    `FUN_000d79c0` ("DoSimulatedClears"), `FUN_000c2fa0` ("SkyboxOpaque"),
    `FUN_000c3320` ("SkyboxAlpha").

**Targets** (from the target manager `V = M`, frame map §0): `V+0x29a0` = SetA depth-only
(kRTT_MainDepth); `V+0x2a40` = SetB — 7 colour slots (MRT0 Albedo_AmbientOcclusion, MRT1
Normal_Roughness, MRT2 PBR, MRT3 ExtraPBR, MRT4 Motion, MRT5 Ambient0, MRT6 Ambient1) + depth
kRTT_MainDepth; `V+0x2590`, `V+0x24a0`, `V+0x2630`, `V+0x24f0` = auxiliary sets.

### 3.2 `0xc1860` DepthPrePass runner, 1033 B (the "RenderDepth" job body)

Zero static callers (job body). Proven `000c1860.c`:

1. `:48-58` RT set `V+0x29a0` (SetA, depth-only) via `FUN_00092140`.
2. `:61-88` **depth-stencil state select** — three variants copied from the static state block
   `DAT_02ee3ce0` straight into the device object (`cb+0x210..+0x230`): `+0x110..+0x130`
   (default), `+0xc8..+0xc8+0x20` (when `ctx[9]+0x19a`), `+0xf8..+0x118` (when `ctx[9]+0xa8c`) —
   then `FUN_00091e10(cb, DAT_02ee3ce0+0x17c, 0xff)` emits the DB state block (`:89`).
3. `:90-134` the **group loop** over `uVar6 = *(u32*)(ctx+0x1f)` (the 0x1f word written by
   `0xc59d0:188`):
   - bit 0x1: pass slot `ctx+0xf4 = 1`; `FUN_0011d3d0(G, ctx[0xc], ctx, 0)` (group 0) — gated
     also on `ctx[9]+0x1c == 0 || group-0 vector empty`;
   - bit 0x2: slot 1; group 1 + `FUN_000db700(R+0x80, ctx, …)` ×2 (conditional) + particle
     readback (`0x10aee0` when `F+0xdc0`) + `FUN_0011d7a0(G, ctx[0xc], ctx, 0x10)`;
   - bit 0x4: slot 1; group 2;
   - bit 0x8: slot **10**; group 3 + `FUN_0011d7a0(..., 0x11)`;
   - bit 0x10: slot **0xc**; group 4 + `FUN_0011d7a0(..., 0x12)`.

So the depth pre-pass = 5 sorted groups of the same per-group draw loop (§1.2), with the
XPR batch draws (§1.4) attached to groups 1/3/4. Vulkan: depth-only render pass into
kRTT_MainDepth (Z32F), 5 draw batches; groups = the sort keys `kRS_Depth_*` (§1.2 name array).

### 3.3 `0x93d70` — depth-touch wrapper, 372 B

Proven `00093d70.c`. Given a pass-target descriptor `param_2`:
- `:15-16` sub-rect from `+0x184` (x = `&0x7ff`, lines = `(>>0xd & 0x7ff) − x + 1`);
- `:17-21` if bit `0x20` of byte `+0xde` clear and `+0x167 & 0x20` set: **call the depth pass
  object** `FUN_00093ef0(ctx, param_2, param_2, 0)`;
- `:22-27` else (bit `+0xde` set): `FUN_0086ced0(cb, 0x2c)` (EVENT_WRITE) +
  `FUN_0086cf40(cb, offset, size, 0x4000, 0, 0x4000000, 1)` — ACQUIRE_MEM (opcode 0x58) over
  the sub-rect;
- `:28-33` always: compute the surface byte size `(pitch(+0x180 & 0x3fffff)·0x40 + 0x40) ·
  bytes_per_element(fmt) << swizzle(+0x164 >>2 & 3)` and `FUN_0086cf40(..., 0x38, 0x4000000, 1)`
  — a second ACQUIRE_MEM (flags 0x38) over the depth surface.

In RenderShadows (§2.1 step 7) the five touches are therefore **L2/K cache invalidates scoped
to each cascade depth surface** (the first may also dispatch the depth-clear pass when the
descriptor flags allow).

### 3.4 `0x93ef0` — depth pass object, 1067 B

Static callers: `FUN_00093d70` (§3.3) and `FUN_00094320` — the latter has **zero static
callers itself** (vtable-only dispatch, same class as N2-9; noted, no new row — it is a
`0x93d70` variant with an extra ACQUIRE_MEM pair, `00094320.c:23-46`).

Proven `00093ef0.c` (pass ctx = `param_1`, target descriptor = `param_2`, depth owner =
`param_3`, flag = `param_4`):

1. `:26` gate: `*(char*)(param_2+0x62) == 2` (depth-type target, pass-map §1.4) **and**
   (`param_2 != param_3` or `param_4 == 0`) — skips the self-clear case.
2. `:27-28` `FUN_0086ced0(cb, 0x2c)` (EVENT_WRITE) + `FUN_00869ce0(cb, 0, 0xcc)` (CB_COLOR_CONTROL
   mode 0 = blend off).
3. `:29-41` copy the 12-dword depth block `param_2+0x164..+0x198` and **patch the format/stencil
   words from the depth OWNER** `param_3+0x164` via `FUN_00868710/0x008688e0` (z-format) and
   `FUN_0086ca0? (0x86ca0)/0x868e90` (stencil format) — i.e. the view descriptor carries the
   address, the canonical target carries the format.
4. `:42-50` if force (bit0 of ctx `+0x8`) or depth-dirty (`ctx+0x11` bit0) or the cached block
   (`ctx[0x15]` = `ctx+0xa8`) differs: `FUN_0086a290(cb, &block)` (the DB_Z_INFO.. register
   block, gap N2-11 words 10/11), `FUN_0086e3a0(cb, block&3)` (PA_SU_POLY_OFFSET_DB_FMT_CNTL),
   `FUN_00869f50(0, cb)` (DB_DEPTH_CLEAR = 0.0); cache `ctx[0x15]`, clear the dirty bit.
5. `:51-63` DB_RENDER_CONTROL (`0x86dd20`) from `0x87c620/c680/c670` words with `param_4^1`;
   when `param_2 != param_3` also DB_RENDER_OVERRIDE (`0x86be10`) from the `0x87d070..d120`
   word set.
6. `:64-65` `FUN_00091e10(ctx, DAT_02ee3ce0+0x20c, 0xff)` + `FUN_00091cd0(ctx,
   DAT_02ee3ce0+200)` — the standard depth-stencil + SC-mode/bias state blocks.
7. `:66-72` CB_TARGET_MASK = 0 (`FUN_00869990`) when its cache is dirty (no colour writes).
8. `:73-88` viewport/scissor from the owner dims `param_3+0x194` (low16 w, high16 h) via
   `FUN_0085de40(0.5, 0.5, cb, 0, 0, w, h)` (zdepth 0.5 — **note: the depth pass clamps
   viewport min/max depth to 0.5, unlike the 0x91fc0/0x85de40(1.0, 0) call the RT binder
   makes**); caches `ctx+0x16..0x18/+0xb4/+0xbc/+0xc4`.
9. `:89-103` primitive reset block (pass-map §1.2): `cb+0xc548 = 0`, vertex-cache reset, prim
   RectList (0x11), `ctx[4] = PTR_DAT_01604408`, `ctx+0x14 = 0`.
10. `:104-105` `FUN_00861600(cb+0x2e8, 0, 0)` (material PS-header reset) +
    `FUN_00093690(param_1)` — **the fullscreen-rect draw object** (the depth clear draw:
    `vkCmdDraw(3,1,0,0)` with the clear material, draws doc §0x93690).
11. `:106-117` label alloc `FUN_0086d210(cb, 4, 3)`, `*slot = 0`, EOP
    `FUN_0086eb60(cb, 4, 0, slot, 1, 1, 0, 0)`, WAIT `FUN_0086edc0(cb, slot, 0xffffffff, 3, 1)`
    (a 4-byte label, WAIT cmp 3), CB_COLOR_CONTROL mode 1 restore, `ctx+0x11 |= 1` (depth
    dirty), DB_RENDER_CONTROL/`DB_RENDER_OVERRIDE` restore.

**Vulkan mapping:** depth attachment bind (DB block) + load-op-clear via the fullscreen draw +
EOP/WAIT fence + depth-stencil dynamic state. The `param_2` vs `param_3` split (view descriptor
vs format owner) is the guest's aliasing mechanism — the native binder takes
(image, view, format) from `param_3`'s block and the *address* from `param_2`'s.

## 4. `0xcaed0` RenderView::MainCamera (Phase 2.9), 6077 B

Callers: `0xcefa0` (line 339 of the frame body) and `0xccdf0` (cube-map face). Signature
`(R, F+0xb8, ctx)`. It is the **pass-graph orchestrator** — it computes no matrices itself
(confirmed: the body never touches a view/projection float; the math is `0xbe1f0` SetViewParams,
frame map §7). Full ordered sequence (proven `000caed0.c`, line numbers cited):

1. `:167-169` reads: `lVar12 = ctx[9]` (per-frame config), `uVar15 = ctx[4]` (main RT base),
   `uVar14 = *(R+0x8f8)` (shadow config).
2. `:170-184` `FUN_000d9cf0(*(R+0x80), ctx)` → if true schedule job
   "VirtualGeometryRendering::PreRenderJob" (`FUN_000da160`, priority 4).
3. `:185-192` view-list flush check; `:193` **WAIT_REG_MEM on label `ctx[0x1d]+0x38`** —
   the CP-side gate for the frame-setup EOP (`FUN_0086edc0(cb, slot, 0xffffffff, 5, 1)`).
   (Slot `+0x38` is written by the XPR cull setup at `000ca6d0.c:104-105`
   (`FUN_0086edc0(cb, ctx[0x1d]+0x8/+0x10, …)`) and by the frame-setup path; the exact
   writer of this specific slot is not statically pinned — gap N2-17.)
4. `:194-196` `FUN_000cc690` (spot shadows) → `FUN_000cc9b0` (RenderShadows, §2.1) → EOP
   write `FUN_00092540(*ctx, 2, ctx[0x1d]+0x80)` (shadows-done label).
5. `:197-203` flush check (the shadow jobs record PM4 from workers — this submits their DCB).
6. `:204-217` `ctx[6] = ctx[4]` (main RT), `ctx+0xf4 = 1` (pass slot 1), RT set `V+0x29a0`
   (SetA depth-only) via `FUN_00092140`.
7. `:218-219` **`FUN_000be1f0(ctx, shadow_cfg)` = SetViewParams — the camera math** (writes the
   0x1228-byte view-constant block; frame map §7; `pfVar31 = ctx[6]` is the camera object,
   `pfVar31[0x28..0x30]` = projection-override scalars, `lVar33 = *(M)`-adjacent camera block
   `+0x20..+0x48/+0x98/+0xa0` = fov/aspect inputs — `000be1f0.c:505-535,528,566,790`) —
   then `FUN_000bdfd0(ctx, 0x10)`.
8. `:220-249` "ForwardRendering::CreateClustersJob" (`FUN_000aaf80`, when `R+0x78 != 0`) and
   "FurRendering::PreRenderJob" (`FUN_001832a0`, conditional).
9. `:250-264` "bpe::renderer::ParticleRenderer::UpdateLightmapsJob" (`0x107510`).
10. `:265-272` wait job counter `RC+0xe28`; **`ctx[0xc] = R+0xd8`** (main draw table);
    `FUN_000c59d0(R, F+0xb8, ctx, R+0xd8)` — the G-buffer runner (§3.1).
11. `:273-296` "FogRendering::UpdateJob" (`0xa5e50`); conditional fog/flares (`0xc3600` —
    note `0xc3600` also reads the RC+0x2400 record block), `0x187ce0`; deferred pass list
    `FUN_0011d240`; `0x93d70` kRTT_MainDepth touch when any of those ran; "PreDebug" scope
    with RT `V+0x1f88` + `FUN_0011cf50`.
12. `:324-398` RT set `V+0x24a0`; conditional `FUN_000c4210` (SpecularIBL) +
    `FUN_000a69d0`; water/particle branches (`V+0x2770/0x27c0` sets, `0xc3c70`, `0x103a40`,
    `0x109ab0`, `0x10a5e0`); `0x93960` passes; `0xa20b0`.
13. `:401-419` conditional (water): EOP `+0xd8`, `FUN_000c3820`, `0x93960(V+0x2078)`,
    `0x93d70` main depth, "ForwardDecal" (`0x1818e0`).
14. `:420-428` `FUN_001038b0`; `FUN_00093610` clear-rect calls on `V+0x1f98/0x1fa0`.
15. `:429-545` SpecularIBL branch + particle branches (above/below-water) — per-view record
    blocks `RC+0x2400 + uVar16*0x50` bound as RT sets (`:490-504`, `uVar16 = 10|((bit1)<<1)`,
    i.e. view ids 10/11 = particle views), `FUN_00109ab0/0x103a40/0x103a60`.
16. `:546-557` `FUN_000bbbf0` (resolve blends); `0xc3a50/0xc3c70` decal pairs; `0x93d70`
    main depth.
17. `:558-584` particle debug passes (`0x109ab0` ×4, `0x10b3b0`).
18. `:585-620` conditional jobs "RenderDebug" (`FUN_000c46c0`) / "RenderDebugCollision"
    (`FUN_000c4a50`).
19. `:621-627` `FUN_000c4f80` (EditorZFail) + `FUN_00093960(*ctx, V+0x1f88)` + the particle
    list draws (`FUN_001a2800`).
20. `:628` EOP label `ctx[0x1d]+0xc0`.
21. `:629-686` **CPU job waits**: the 12 fixed counters `RC+0xe28,0xe30,…,0xe80` then the
    vector `[RC+0xe98, RC+0xea0)` (`FUN_00035590(DAT_02ea4718, *slot, 0, 0, 1)` each).
22. `:687-713` draw-counter accumulation into `G+0x220384..+0x2203a4` from the 9 draw-table
    vectors `R+0x120/0x948/0x1170/0x19a0/0x21c0/0x29e8/0x3210/0x3a38/0x4260` (>>5 = per-0x20
    record count).

### 4.1 Viewport / scissor state MainCamera writes (directly)

MainCamera itself writes **no** viewport packets — every viewport/scissor emission inside its
sub-tree comes from the RT binder `0x92140` (which calls `0x91fc0` → `0x85de40(1.0, 0, …)` =
zdepth 1.0, `00091fc0.c:19-24`) or the depth pass object (`0x85de40(0.5, 0.5, …)`, §3.4 step 8).
The second cache pair `ctx+0xb4/+0xbc` and depth-range `ctx+0xc4` are the `0x85de40` cache
slots (pass-map §1.1). `FUN_0086d890` (called by `0x91fc0:31`) emits the second (scissor-only)
packet family. So the Phase 2.9 native viewport state source is the **pass-target descriptor
dims** (`+0x194`/`+0x120` u16 pairs — N2-12 producer still open) fed through `0x91fc0`.

### 4.2 Where the matrices live (offsets + writers)

| Location | Content | Writer | Evidence |
|---|---|---|---|
| `pfVar27` (0x1228 B, bump-alloc from `DAT_02ee3ce0+0x408` heap) | per-frame view-constant block: world/inv-world `+0x00..+0x1f`, camera basis `+0x20..+0x2f`, inv basis `+0x30..+0x3f`, view matrix `+0x40..+0x4f`, projection rows `+0x50..+0x5f/+0x68..+0x7f`, width/height/1/w/1/h `+0x80..+0x83`, near/far `+0x84..+0x87`, proj-override scalars `+0x8c..+0x8f`, depth-linearize `+0x98..+0x9b`, frustum planes `+0xa0..+0xaf`, rotation basis `+0x150..+0x15b`, jitter `+0x210..+0x216` | `0xbe1f0` SetViewParams (the only writer) | `000be1f0.c:446-452,672-689,790-795,1300-1336`; frame map §7 |
| second 0x1228 block | same layout, second view id | `0xc95c0` (`pfVar12`/`pfVar13` pair; `+0x00`/`+0x01` = width/height from `FUN_00096f50/0x96f60(*(RC+0x1fb8))` = kRTT_MainDepth; jitter fixup from `*(F+0xd90)` at `:306-323`) | `000c95c0.c:39,140,302-333` |
| `ctx[0x20]/[0x28]/[0x30]` | pointers to the two view-constant blocks (`[0x30]` aliases `[0x20]`) | `0xc95c0:329-331` | — |
| `ctx+0x98` (u64) | **view id** = `{DAT_02ee3ce0+0x430, (+0x434 & 0xfff)}` — a GPU VA of the uploaded constants; also `param_1[0x1c]/[0x13]` in SetViewParams | `0xc95c0:332-334`, `000be1f0.c:1323-1335` | — |
| `ctx[3]+0x2400 + id*0x50` (per-view record, 0x50 B; V = target manager) | RT-descriptor quadruples for the view (main view id 0x23-adjacent per cascade; particle views 10/11) | `0xc89f0` (writes `*(RC+0xe20) = block` for one view family; the direct `RC+0x2400` writer is **untraced** — gap N2-16) | `000cdbc0.c:32-45`, `000caed0.c:490-504`, `000c89f0.c:242` |
| `M+0x18..0xa0` | the three 4×4 view matrices copied into the target manager at frame end | `0xcefa0:406-455` (frame map §5) | — |

**Downstream consumers** (what the native backend must feed): the 0x1228 block's GPU VA goes
through `0x92640`/`0x92b50` (`000be1f0.c:1324-1330`: SET_SH_REG user-data writes from
`DAT_02ee3ce0+0x430/+0x434/+0x438` = address / size-hint / dword-count) into the bound
shaders' user-data windows — i.e. it is **push-constant/UBO payload, uploaded once per view
via the compute stream (`0x92640:12` = `0x86f1d0(cb, 0x38, 0x8000000, 1)` marker)**. The
frustum-plane words `+0xa0..+0xaf` are what the CPU cull jobs re-derive (e.g.
`0011dbe0.c:78-93` reads `ctx[6]` floats `+0x31..+0x3c` = the same 12 floats for the XPR
dispatch record).

### 4.3 The per-view record block `ctx[3]+0x2400..0x26xx` (V = target manager)

**Base correction (2026-11-21, Phase 2.9):** this block lives on the *target manager* `V = ctx[3]`,
not on the render context. Proven: `000caed0.c:489-491` (`lVar3 = param_3[3]; ... lVar3 + 0x2400`)
and `000cdbc0.c:31-33` (`lVar3 = param_1[3]; ... lVar3 + 0x2400 + (iVar2+0x23)*0x50`). Producer
traced: `FUN_000a0a60` (chain `0xcefa0 -> 0x118440 -> 0x90860 -> 0x95690 -> 0xa1cd0 -> 0xa0a60`,
table allocated by `0x95a90`, `00095a90.c:128-148`). Cascade ids 0x23..0x27 receive their `+0x40`
depth qword from `V+0x2120..0x2140` (`000a0a60.c`).

Layout (proven readers): 0x50 bytes per view id; `+0x00..+0x1f` = 4 RT-descriptor pointers
(bound as the first 4 of the 10-qword RT array by the runners — `000c3600.c:34-44`,
`000c3ec0.c:38-48`, `000cdbc0.c:33-45`), `+0x20..+0x3f` = 4 more, `+0x40/+0x48` = two extra
qwords (`0x2430+0x10/+0x18` in the 0xcdbc0 reading). View ids observed: 0..9 (main/pass
families via `0x1a8`-stride loops in `0xca6d0`), 10/11 (particles, `000caed0.c:429,490`),
`+0x23..+0x27` (the five cascades, `000cdbc0.c:32`), `0x1f` (`0x8d730` marker; read
`000a1a80`-adjacent). **Producer untraced** — gap N2-16.

## 5. Vulkan mappings summary (this file's functions)

| Guest | Native (Vulkan) |
|---|---|
| CPU cull jobs trimming the group record vectors (`0xc53d0`, `0x19af20`, XPR) | host draw-list build (the native backend consumes the already-trimmed vectors — no extra GPU work) |
| `0x11d3d0` per-group loop | one `vkCmdDrawIndexed*` per record with the bind-doc pipeline/descriptor discipline; group scope = debug label only |
| XPR batch draw `0x11d7a0` | `vkCmdDrawIndexedIndirect` over the compute-written args buffer (SET_BASE → just the buffer binding) |
| label EOP/WAIT pairs (§1.5) | timeline semaphores / `vkCmdWaitEvents` (labels are DCB inline data — replace with native sync) |
| SET_PREDICATION / COND_EXEC (§1.6) | **nothing live to map**; if a future path needed it: `VK_EXT_conditional_rendering` (memory-value predicate ⇒ `VkConditionalRenderingBeginInfoEXT.buffer`) |
| RenderShadows cascades | 5 depth-only passes + compute cull + indirect caster draws (§2.4) |
| G-buffer / depth pre-pass | depth-only pass (SetA) → 7-colour MRT pass (SetB) + auxiliary sets (§3, §5 of the bind doc for attachment decode) |
| `0x93ef0` depth clear | depth attachment load-op clear + fullscreen clear draw |
| MainCamera view constants | UBO/push constants from the 0x1228 block; frustum planes re-derived, not read back |

## 6. Contradictions with the existing map docs (supersessions)

1. **frame map §8 / G3 row** ("source = memory value; producer unknown … G3 not fully closed"):
   superseded — with the dead-code proof of §1.6 the producer question is void; the only live
   predication is the disable. Register rows N2-14 (doc correction) and N2-15 (runtime
   confirmation) record this.
2. **master map §7 / pass map §7** ("`0x16b850(R+0x38,ctx,4)` (scope push)" + gap N2-5's
   "material-commit path" lead): `0x16b850` proven = conditional `sceGnmInsertPushMarker` of
   the material name string (`material+0x38`), not a state emitter. N2-5's "next probe" is
   dead; the row note is updated (append-only) in §7.
3. **frame map §8.1** ("`FUN_001222e0(puVar3, ctx)` (draw)" without the split): the draw is
   conditional on the marker gate + record `+0x28` (indirect) vs the DrawIndexOffset path —
   §1.3 here is the full contract.
4. Everything else in the pass/bind/draws/frame maps is consistent with this file (the
   `ctx[0x1d]` label slots, `0x92140` RT-set protocol, `SET_BASE` N2-8 verdict all re-verified
   in place).

## 7. Gap rows added (register updated, see native-render-gaps.md)

| ID | One-line summary |
|---|---|
| **N2-14** | frame-map §8/G3 stale wording: predication producer question closed as dead-code; docs corrected by this file |
| **N2-15** | runtime confirmation hooks for the §1.6 dead-code verdict (counters on `0x86e770`/`0x85cea0`/`0x85d260` entries) |
| **N2-16** | producer of the per-view record block `RC+0x2400..` (0x50 B per view id) |
| **N2-17** | writer of MainCamera's wait label `ctx[0x1d]+0x38` |

## 8. Evidence index

Primary decompiles read in full this session (under `out/re/decomp/`): `0011/0011d3d0.c`,
`0011/0011d7a0.c`, `0011/0011d5b0.c`, `0011/0011dbe0.c`, `0011/0011e190.c` (label wait),
`0012/001222e0.c`, `0012/00123d20.c`, `0012/00128d20.c`, `0018/00189060.c`,
`0086/0086e770.c`, `0086/0086e800.c`, `0086/0086ea50.c`, `0086/00866360.c`,
`0086/0086e670.c`, `0086/0086f680.c`, `0086/0086d210.c`, `0086/0086f2a0.c`,
`0086/0086eec0.c`, `0086/0086ced0.c` (head), `0088/00883300.c`, `0088/00885220.c`,
`0088/008843d0.c`, `0088/00884d40.c`, `0085/0085cea0.c`, `0085/0085d260.c`,
`0085/0085cb40.c`, `0085/0085c9b0.c`, `0085/0085d690.c`, `0085/0085d6f0.c`,
`0009/00094d00.c`, `0009/00094bf0.c`, `0009/00093ef0.c`, `0009/00093d70.c`,
`0009/00094320.c`, `0009/00091fc0.c`, `0009/00092540.c`, `0009/000925c0.c`,
`0009/00092640.c`, `0009/00092b50.c` (head), `0009/00092720.c` (head),
`0009/00095a90.c`, `0009/0009aef0.c` (head), `000c/000cc9b0.c`, `000c/000cdbc0.c`,
`000c/000cdb00.c`, `000c/000c1860.c`, `000c/000c59d0.c`, `000c/000caed0.c`,
`000c/000c95c0.c`, `000c/000c0b70.c`, `000c/000c89f0.c` (target-publish part),
`000c/000ca6d0.c`, `000c/000ca150.c`, `000c/000ca120.c`, `000c/000cefa0.c` ( excerpts),
`000d/000da7d0.c`, `000d/000da820.c` (head), `000d/000da160.c` (head),
`000d/000da220.c`, `000d/000da110.c`, `0016/0016b810.c`, `0016/0016b850.c`,
`0016/0016b890.c`, `0016/0016b8c0.c`, `0016/0016b900.c`, `0018/00187810.c`,
`0018/001832a0.c` (head), `0018/00183310.c` (head).

Raw eboot checks (`out/eboot.elf`, VA+0x4000): SET_PREDICATION header `0xc0012000` immediates
at `0x86e770+0x57` and `0x86e800+0x35`; COND_EXEC header `0xc0032200` at `0x86ea50/0x866360/
0x883300+0x51`; `0x3fff` mask at `0x86ea50+0x41`; EVENT_WRITE `0xc0004600` at `0x86ced0+0x48`;
EOP `0xc0044700` at `0x86eb60+0x6f`; WAIT_REG_MEM `0xc0053c00` at `0x86edc0+0x61` and inside
`0x86eec0` (computed `{mem,addr,mask,ref,10}` form, `0086eec0.c:18-28`); label-alloc NOP
marker `0x68750000` at `0x86d210+0x82`; the `call 0x86e800` rel32 at VA `0x11d570`;
scope-name array at image `0x1597ea0` (8 raw pointers, strings `kRS_Depth_Occluders` …
`kRS_GBuffer_Ambient_Dynamic`); `DAT_00f9f2e0[0..3] = {4,6,2,3}` (primitive table);
`DAT_00f9f2c0[2]/[3] = {0x28, 0x38}` (EOP control/data words); stage-index tables
`0x1604370/0x1604390/0x16043b0/0x16043d0/0x16043f0` (see §4 note); whole-image 32-bit scans
for `0x86e770/0x85cea0/0x85d260/0x86ea50/0x883300/0x86f680/0x885220` and their `+0xc00000`
bias forms → 0 hits.

Port cross-checks: `liverpool.cpp:1558-1566` (SetPredication stub), `:2136-2152` (CondExec
skip + `DrainBack` + `SotcStats::Drop`), `:1801-1806` (SetBase → indirect_args_addr, N2-8),
`:1488-1494` (CbDbExtent NOP-hint decode, N2-7), `pm4_cmds.h:1218-1235` (`PM4CmdCondExec`,
`Address()` = 1-byte bool), `pm4_opcodes.h:11-22` (Nop 0x10 / SetBase 0x11 / SetPredication
0x20 / CondExec 0x22), `gnmdriver.cpp:176-194` (`sceGnmComputeWaitOnAddress` → WAIT_REG_MEM).
