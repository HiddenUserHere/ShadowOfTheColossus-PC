# Native render map — draw objects (Phase 2.2 / 2.3 appendix)

Field-level contract for the five draw objects, derived from the decomp corpus
(`out/re/decomp/<hi16>/<addr>.c`), raw entry bytes from `out/eboot.elf`
(VA → file offset = VA + 0x4000), and the port's own GNM driver
(`gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`), which implements the
same `sceGnm*` emitters the guest calls. Companion to `native-render-map.md`,
`native-render-map-pass.md` (pass/device context offsets) and
`native-render-map-bind.md` (material bind `0x85fc10`).

All functions take the **pass context** as arg 1 (SysV `rdi`) — the same object
documented in `native-render-map-pass.md`; `dev = *ctx` is the device context,
`mat = dev + 0x2e8` is the material/bind state block.

## Shared primitive — the draw preamble

Every draw object opens with the same state sequence (exact packet bytes below):

| Step | Emitter | Packet | Purpose |
|---|---|---|---|
| Primitive type | `0x86a900(dev, prim)` | `0xc0017900, 0x242 \| NeoBit, prim & 0x3f` | `SET_UCONFIG_REG` word `0xC242` = `regs.primitive_type` (the port's `Regs` overlay places `primitive_type` at uconfig word `0x242`, right after `vgt_esgs_ring_size`/`vgt_gsvs_ring_size`). **Neo (PS4 Pro) variant ORs `0x10000000` into the reg dword** — that is the CI+ `index` field (bits 28–31) of `PM4CmdSetData`, which the port decodes and ignores, so both variants land at `0xC242`. `0x882ec0` is a cached `sceKernelIsNeoMode()`. |
| Index-type reset | `0x86bed0(dev, 0, 2)` | `0xc0002a00, 0` | `INDEX_TYPE` packet, value 0 = index parsing disabled. Gated on `(ctx+8) & 1` (force) or `ctx[0x39]` (`ctx+0x1c8` cache non-zero); cache cleared to 0. |
| NumInstances | `0x86c000(dev, v)` | `0xc0002f00, v` | `VGT_NUM_INSTANCES`. Used as a bracket around `0x94dd0`'s draw (`param_7` before, `1` after). |

The primitive type value itself: `(byte)(ctx+0x18) & 6` selects between the
mode table and RectList:

- `& 6 == 0` → `prim = *(int*)(DAT_00f9f2e0 + mode*4)` (mode-table lookup),
  cache `ctx+0x1cc` compared against it;
- `& 6 != 0` → `prim = 9` (RectList — the fullscreen-quad path `0x93690` uses),
  cache compared against 9.

Force bit `(u32)(ctx+8) & 1` bypasses the cache check in both arms.

## Common emitters referenced below

| Function | Packet | Semantics (cross-checked against the port's `gnmdriver.cpp`) |
|---|---|---|
| `0x86ac10` | `sceGnmDrawIndexAuto(cmdbuf, 7, count, flags)` | `PM4ItOpcode::DrawIndexAuto` (0x2D), 4 dwords + 3 trailing `Nop`. `flags = param_3 << 29 \| (dev+0x28) & 1`. |
| `0x86b040` | `sceGnmDrawIndexOffset(cmdbuf, 9, index_offset, index_count, flags)` | `DrawIndexOffset2` (0x35), body `index_count, index_offset, index_count, neo_flags`, 9 dwords + 3 `Nop`. |
| `0x86b150` | `sceGnmDrawIndirect(cmdbuf, 9, data_offset, stage, vs_sgpr, inst_sgpr, flags)` | `DrawIndirect` (0x24), 9 dwords + 3 `Nop`; dword 3 = `2` (auto index) on base, `flags & 0xe0000000 \| 2` on Neo. |
| `0x86b3d0` | `sceGnmDrawIndexIndirect(...)` same shape | `DrawIndexIndirect` (0x25), dword 3 = `0`/`flags & 0xe0000000`. |
| `0x86ad40` | `0xc0021100 \| pred<<1, addr_lo, addr_hi&0xff` | **`SET_BASE` (0x11)**, not RegRmw (raw-body verdict 2026-11-20, gap N2-8 closed): the earlier `0xc0021000, 1` reading was a decimal/hex misread — the literal is `-0x3ffdef00 + (pred&1)*2` = `0xc0021100`/`0xc0021102`, opcode = **0x11 = `PM4ItOpcode::SetBase`**. Body `[2..3]` re-join the caller's `u32` pair into a **40-bit GPU VA** (`lo & ~7`, `hi & 0xff`) — the address of the **indirect-args table** the following `DrawIndirect`/`DrawIndexIndirect` packet reads (`draw_index_indir_patch_table` slot). The port executes it: `liverpool.cpp:1801` `case PM4ItOpcode::SetBase` → `indirect_args_addr`, consumed by its `DrawIndirect`/`DrawIndexIndirect` cases. |
| `0x86d550(sh_ctx, stage, nib, value)` | `0xc0017600 (+2 if stage==0), (u16)DAT_01290410[stage] + nib − 0x2c00, value` | `SET_SH_REG` (0x76): writes `value` into the user-data SGPR slot selected by the nibble, at the per-stage base from `DAT_01290410[stage]` (the `SPI_SHADER_USER_DATA_x_0` table; stage = GNM enum 0=CS,1=PS,2=VS,3=GS,4=ES,5=HS,6=LS). The `+2` header tweak for stage 0 is the Compute shader-type bit. |
| `0x862510(mat, v1, v2)` | calls `0x86d550` 0–2× | Draw-kind-driven start-vertex / base-instance constants: reads the draw kind at `mat+0xc548` (= `dev+0xc548`), picks the geometry descriptor (`dev+0xc398` kind 0 / `+0xc3a8` kind 0xb0 / `+0xc3b8` kinds 0x45/0xad/0x145/0x1ad), takes the nibble byte at `geom+0x27` (kind 0) or `geom+0x19`, and writes `v1` into the low nibble's SGPR slot and `v2` into the high nibble's. **Vulkan mapping: these slots are exactly the first-vertex / base-instance SGPRs the shader recompiler reads — in the native backend they become the `firstVertex`/`baseInstance` (or `vertexOffset`/`firstIndex`) arguments of `vkCmdDraw*`, not descriptor writes.** |

## 0x94ec0 — DrawIndexAuto draw object (Phase 2.2)

`void f(ctx*, int mode, u32 start_vertex, u32 index_count)` — SysV:
`rdi=ctx, esi=mode, edx=start_vertex (→r15d), ecx=index_count (→r14d)`.
Entry bytes confirm: `48 89 fb` (mov rbx,rdi), `41 89 d7` (mov r15d,edx),
`41 89 ce` (mov r14d,ecx). Size 234 B.

Callers (static, 12+): `0xb9710, 0xa69d0, 0x1666e0, 0x5d5ec0, 0x164a60,
0x5e3b50, 0x5d1540, 0x166c90, 0xb8c60, 0x166dc0, 0xba870, 0x118470` (frame
tail) and more — the workhorse draw object used by the pass functions.

Body order:
1. Primitive type (table lookup, cache `ctx+0x1cc`).
2. Index-type reset (`0x86bed0`, cache `ctx+0x1c8`) — only draw object that
   resets it (it may follow an indexed draw).
3. `0x862510(dev+0x2e8, start_vertex, 0)` — start-vertex SGPR write.
4. Material bind `0x85fc10(dev+0x2e8)` (see `native-render-map-bind.md`).
5. `0x86ac10(ctx, index_count, 0)` — the draw.
6. `0x862510(dev+0x2e8, 0, 0)` — reset the constant.

Vulkan: `vkCmdDrawIndexed(cmd, index_count, 1, 0, start_vertex, 0)` with
topology from the primitive type; index binding disabled.

## 0x94d00 — DrawIndexOffset draw object (Phase 2.2)

`void f(ctx*, int mode, unused, unused, u32 index_offset, u32 index_count)` —
SysV: `rdi=ctx (→r13), esi=mode, rdx/rcx unused, r8d=index_offset (→r15d),
r9d=index_count (→r14d)`. Entry: `49 89 fd` (mov r13,rdi), `45 89 ce` (mov
r14d,r9d), `45 89 c7` (mov r15d,r8d). Size 204 B. Single static caller:
`0x128d20`.

Body: primitive type; **no index-type reset**; `0x862510(dev+0x2e8, 0, 0)`;
material bind; `0x86b040(ctx, index_offset, index_count, 0)`; reset constants.

Vulkan: `vkCmdDrawIndexed(cmd, index_count, 1, index_offset, 0, 0)`.

## 0x94dd0 — DrawIndexOffset + instance bracket (Phase 2.2)

`void f(ctx*, int mode, unused, unused, u32 index_offset, u32 index_count,
u32 instances_before)` — SysV: `rdi=ctx (→r14), esi=mode, rdx/rcx unused,
r8d=index_offset, r9d=index_count (→r15d), stack `[rbp+0x10]`=instances_before
(→r12d, spilled to `[rbp-0x2c]`)`. Entry: `49 89 fe`, `44 8b 65 10` (mov
r12d,[rbp+0x10] — **takes its 7th argument from the stack**), `45 89 cf` (mov
r15d,r9d). Size 231 B. **Zero static callers — reached only through a computed
call/vtable**, so a hook on its entry alone is not sufficient to enumerate its
call sites (gap **N2-9**).

Body = `0x94d00` plus a `NumInstances` bracket: `0x86c000(ctx, instances_before)`
before the draw, `0x86c000(ctx, 1)` after. Vulkan: pass `instances_before` (and
the restore to 1) as the `instanceCount` discipline around the indexed draw.

**Caller verdict (N2-9 closed 2026-11-20):** still zero *static* callers, and the address
`0x94dd0` appears nowhere as a call target — all 128 corpus references are
`&PTR_FUN_01594dd0` (the image-anchored function-pointer word at `0x1594dd0`), stored into
per-object dispatch slots / globals across 30+ files (`0004/000433e0.c:221-273` slots
`+0x5c0/0x660/0x700/0x7a0`, `0001/000161f0.c`, `0024/00247f00.c:103-297`,
`0016/0016a810.c:51` next to the render root `DAT_02ee3ce0`; full list in the gap register).
Vtable-only dispatch confirmed; runtime attribution deferred (non-blocking: the entry hook
covers the body regardless of which table slot fires).

## 0x10add0 — DrawIndirect mesh draw object (Phase 2.3)

`void f(ctx*, u32* pair, u32 data_offset)` — SysV: `rdi=ctx (→r12), rsi=pair
(→rbx), edx=data_offset (→r14d)`. Size 264 B. Callers: `0x109ca0`,
`0x18e3d0`.

Body order:
1. Index-type reset preamble (same gate as `0x94ec0`).
2. `0x86ad40(ctx, *(u32*)(ctx+0x14), pair[0] | (pair[1] & 0xfff) << 32)` —
   the `RegRmw` (gap N2-8); `ctx+0x14` is the predication/force flag input.
3. Material bind.
4. Draw-kind switch on `dev+0xc548` (values `0, 0x45, 0xad, 0xb0, 0x145,
   0x1ad`): selects the geometry descriptor and the GNM stage enum
   (`u` ∈ {2, 4, 6} = VS / ES / GS-class) and the SGPR nibble byte
   (`geom+0x27` or `geom+0x19`).
5. `0x86b150(ctx, data_offset, u, nib & 0xf, nib >> 4, 0)` — `DrawIndirect`
   with per-stage SGPR offsets for the indirect-fetch shader.
6. `0x86c000(ctx, 1)`.

Vulkan: `vkCmdDrawIndirect(cmd, buffer, data_offset, 1, stride)`; the SGPR
nibbles are consumed by the recompiled shader's indirect path — in Vulkan they
map to nothing (the GPU reads the indirect buffer natively); the draw-kind word
only selects which descriptor's fetch constants were staged.

## 0xdb620 — DrawIndexIndirect mesh draw object (Phase 2.3)

`void f(ctx*, u32* pair, u32 data_offset)` — same signature layout as
`0x10add0` (`rdi=ctx (→r15), rsi=pair live in rsi, edx=data_offset (→r14d)`).
Size 213 B. Callers: `0x189060, 0xdb390, 0x10b0a0, 0x18d6f0`.

Identical body to `0x10add0` except: **no index-type reset preamble**, and the
draw is `0x86b3d0` (`DrawIndexIndirect`, 0x25) instead of `0x86b150`. Vulkan:
`vkCmdDrawIndexedIndirect(cmd, buffer, data_offset, 1, stride)`.

## Notes for the native implementation

- The pass context cache fields (`ctx+0x1cc` primitive, `ctx+0x1c8` index
  type) are **guest-side state mirrors**, not GPU state: the native backend
  keeps its own dirty flags and never needs to write them (except to keep the
  guest consistent while the guest body still runs alongside — Phase 2
  dual-path requirement).
- The draw-kind word at `dev+0xc548` and the geometry descriptors at
  `dev+0xc398/+0xc3a8/+0xc3b8` (see `native-render-map-pass.md` §device) are
  read by `0x862510` through the material block (`mat+0xc090/+0xc0b0/
  +0xc0c0/+0xc0d0/+0xc260` — same words, `mat = dev+0x2e8`).
- `DAT_00f9f2e0` (mode → primitive table) is an image-anchored static: read it
  through `Guest::ImageQword`/image base, never as a host pointer (gap N2-3
  lesson).
- Entry prologues are all `push rbp; mov rbp,rsp; push gpr…; … ≥ 14 bytes`,
  so the 14-byte absolute-jump detour covers whole instructions on all five
  (verified against the dumped entry bytes).

## Gaps opened by this appendix

| ID | Description | Status |
|---|---|---|
| N2-8 | ~~`0x86ad40` emits `RegRmw` (0x21)…~~ **Closed 2026-11-20: re-classified as `SET_BASE` (opcode 0x11, header `0xc0021100\|pred<<1`); the caller's `u32` pair is a 40-bit GPU VA of the indirect-args table (`lo & ~7` / `hi & 0xff`), executed by the port at `liverpool.cpp:1801` (`case PM4ItOpcode::SetBase` → `indirect_args_addr`).** The earlier "RegRmw / OR 1" reading was a decimal-literal misread of the decompiler constant (`-0x3ffdef00 + (pred&1)*2`); nothing is OR-ed into the address. See the common-emitters table above and the gap register row for the full evidence. | closed |
| N2-9 | ~~`0x94dd0` has zero static callers…~~ **Closed 2026-11-20: vtable-only confirmed** — 128 corpus references, all `&PTR_FUN_01594dd0` stores into dispatch slots (see §0x94dd0 above for the verdict and the gap register for the full list). Runtime per-site attribution deferred as non-blocking (the entry hook covers the body regardless of dispatching site). | closed |
