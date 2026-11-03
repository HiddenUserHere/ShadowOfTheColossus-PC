# 08 - Engine texture lifecycle: events for a texture cache

Read-only static analysis of the Shadow of the Colossus eboot (EU 1.01 = US 1.00 code). Primary source:
Ghidra pseudocode in `out/re/decomp/<hi16>/<addr>.c`; `tools/re/eboot_re.py` for callers and disassembly.
Addresses are eboot VAs. Labels: **observed** (decomp or disassembly, with address), **inferred**
(from bit masks, call patterns or names), **unknown** (not read or not resolved). Builds on
`02-resource-objects.md`.

Calling convention: the PS4 guest uses System V x86-64. Argument registers are rdi, rsi, rdx, rcx, r8, r9;
the 7th and later arguments are on the stack. Mapping of decomp parameter names to registers is by order.

## 1. Answer

- **Creation is a single engine call per texture.** Every `CTexture` is created by one of four factories
  (`0x96680` file, `0x97520` derived alias, `0x96f70` derived, `0x97a50` descriptor / render target).
  The object is fully described (T#, base address, resident mips) when the factory returns. **observed**
- **The base address (T# dword0) is written only at creation.** Every caller of the base writer `0x881e60`
  is a creation function. Streaming changes only the base/last level fields (`0x880c30`) and the mip
  records. No defragmentation or relocation was found. **observed** (caller list) / **inferred** (no move)
- **Pixel data is written by the CPU, except one path.** File textures are copied with `memcpy` from the
  loader stream (`0x8de50`); streamed mips are copied by a CPU job (`0x112d30` -> `0x98ae0`). A GPU DMA
  copy (`DMA_DATA`, `0x86a700`) is used only by the texture-to-texture copy `0x98800` when a PM4 writer is
  passed (callers `0x186a60`, `0x10e9b0`). **observed**
- **Render targets are distinguishable.** They come from `render_targets_allocate` (`0x9aef0`, named
  `kRTT_*`) through `0x97a50` with a name argument. Only those are registered with
  `sceGnmRegisterResource`. A static texture has no name and no registration. **observed**
- **Destruction is one dtor (`0x985d0`)** that releases three memory regions (+0xa0, +0xa8, +0xb0) into a
  deferred-free queue (`0x90300`), unregisters the GNM resource if registered, and frees the object. **observed** /
  **inferred** (deferred queue drained later: unknown)
- **Recommended hook set:** the engine events and 2 import hooks in section 4. The cache can key on the
  CTexture pointer and invalidate only on creation, destruction, copy-destination writes, and the
  render-target write events it cannot see (section 6).

## 2. Object layout used by the events

| Offset | Meaning | Source | Label |
|---|---|---|---|
| +0x00 | vtable `0x15977d0` (slot 0 `0x985d0`, slot 1 `0x98720`) | `0x96680`, `0x985d0` | observed |
| +0x60 | streaming job pointer (base class) | `0x112b80`, `0x112a80` | observed |
| +0x61 / +0x62 | class/type bytes (copied from the descriptor or the file) | `0x112430` | observed |
| +0x64 | bit 1 = flag from file (cube-like); bit 0 = 1 | `0x96680` | inferred |
| +0x80 / +0x88 | mip record vector, 0x50-byte records | `0x98ae0`, `0x98da0` | observed |
| +0xa0 / +0xa8 / +0xb0 | memory region pointers (primary, second, third) | `0x96680`, `0x97a50`, `0x985d0` | observed |
| +0xb8 | committed end of the streamed region (pool) | `0x98ae0` | inferred |
| +0xc0 | resident level index (current mip level) | `0x98ae0` (writes param_2) | inferred |
| +0xc4 .. +0xe3 | **T#** (32 bytes): dword0 = base address >> 8; dword3 bits 12-15 BASE_LEVEL, 16-19 LAST_LEVEL | `0x8de50` (load), `0x881e60`, `0x880c30` | observed (writers), inferred (bit layout) |
| +0xe4 .. +0x103 | view block 1 (32 bytes) | `0x87e5f0`, `0x87dea0`, `0x97a50` | observed (writes) |
| +0x124 .. +0x143 | view block 2 (32 bytes) | same | observed (writes) |
| +0x164 .. +0x183 | second descriptor (RT path, `0x866fa0`) | `0x97a50`, `0x97520` | observed (writes), role unknown |
| +0x1a8 | mip count | `0x96680` | inferred |
| +0x1b0 | resident bytes (counter, added by `0x98ae0`) | `0x98ae0`, `0x96680` | inferred |
| +0x1b8 | GNM registration id (`-1` = not registered) | `0x985d0`, `0x97a50` | observed |

Note: `02-resource-objects.md` says the dtor releases "views"; the region pointers at +0xa0..+0xb0 are the
memory regions, released through the deferred queue (see T8). **observed** (correction)

## 3. Answers to the six questions

### 3.1 Create and data ready

- Factory `0x96680` (1,629 bytes, called from `0x98aa0` for file textures and from `0x10e9b0` for particle
  textures, memtype `&DAT_02dd9f84` for GPU family). Steps: allocate object `0x17070(0x1c0)`, base ctor
  `0x112430`, vtable write, read T# (32 bytes) from the stream via `0x8de50` into +0xc4 (**observed**),
  allocate memory (`0x12820` for one region, or `0x12950` reserve + commit for a mip range), copy resident
  mip data with `0x8de50` (CPU memcpy from the loader stream), write base via `0x881e60`, set base/last
  level via `0x880c30`, then return the object. **observed**
- **Data final:** at `0x96680` return for resident mips. Upload is a CPU copy; no GPU fence. **observed**
- Derived textures `0x96f70` (1,447 bytes) and `0x97520` (1,319 bytes) copy T# dwords from a source
  CTexture (`0x97520`: `*(obj+0xc4) = *(src+0xc4)`, read at `0x882710`), so they **alias** the source memory
  with a new base. **observed**. Their data is whatever the source holds: no copy.
- Descriptor / render-target factory `0x97a50` (2,834 bytes): allocates the region with `0x12820`, builds
  the T# via `0x8806d0/0x8806f0` or `0x866f50/0x866fa0`, writes the base via `0x881e60` (in some paths
  through `0x881b10` / `0x882150`), and registers with GNM (see section 3.5). Returns the object in rax.
  **observed**
- CPU texture factory `0xa2ac00` (496 bytes, vtable `0x15d1028`, object 0xa0 bytes): **not a CTexture**.
  Allocates CPU buffers with `0x17090` and fills them from the stream (`0x8de50`). It has no T#. **observed**.
  Whether it is later converted or uploaded to a CTexture: **unknown** (no caller read).
- Job-based streaming copy (T5/T6 below) is CPU-side; its completion is observed by the job counter at
  chunk+0x70 == 0 (`0x13cb10`, `0x13a3e0`). **observed**

### 3.2 Placement, relocation, defragmentation

- Writers of the T# base address (dword0 at +0xc4): `0x881e60` only, plus the full-T# rewriters `0x881b10`
  (zeroes the whole 32-byte block first, then rebuilds it from a view block), `0x882150` and `0x882420`. Their callers:

  | Writer | Callers (all creation) |
  |---|---|
  | `0x881e60` | `0x96680`, `0x96f70`, `0x97520`, `0x97a50`, `0x881b10`, `0x882150`, `0x882420` |
  | `0x881b10` | `0x96f70`, `0x97a50` |
  | `0x882150` | `0x96f70`, `0x97a50` |
  | `0x882420` | `0x97520` (only for format id 0x16) |

  All of these are reached from the four factories. **observed**
- No call site moves a region after creation. Streaming (`0x98ae0`) keeps the region base (+0xa0) and only
  changes which mip ranges are committed. **observed** (no writer of +0xa0 outside the factories and the dtor)
- Defragmentation: **no evidence**. The pool allocator (`0x12820` = alloc through vtable at
  `DAT_02dfaab0[category]`, `0x12950` = reserve/commit/decommit, mode 1/0/2) is not decompiled; its internal
  compaction is **unknown**. The engine-level texture object never changes its region. **inferred**
- Relocation of the T# itself: none. The T# stays at object+0xc4 for the object's life. **observed**

### 3.3 Streaming: mip in and out

- Request path: per-frame `0x117930` -> `0x13cae0` -> `0x13ccd0` (bitset scan) -> `0x13a3e0` / `0x13a6f0`
  (per-texture state machine). `0x13cae0` runs once per frame from `0x117930` (CRenderer
  UpdateTextureStreamingGPUData) and from `0x117160` (compute context). **observed**
- Slot table: records of 0x20 bytes at manager+0x2008, indexed by a 16-bit slot (max 0xffff). Bitsets at
  manager+0x202030 (requests), +0x204030 (queue), +0x206030 (previous). **observed**
- Grow / shrink: `0x98ae0(texture, target_level)` (693 bytes). `target > resident (+0xc0)`: commit the mip
  range with `0x12950` (mode 0), copy with CPU `memcpy` from the loaded file buffer (`0x112b30` gives the
  pointer), set `+0xc0 = target`. `target < resident`: decommit the range with `0x12950` mode 2, set +0xb8
  and +0xc0. **observed**
- The T# is rewritten after the change: `0x98da0` and `0x98df0` call `0x880c30` on +0xc4 with
  base_level = the mip record's first level and last_level = mip count - 1. **observed**
  The base address is **not** changed. **inferred** from the code that does not call `0x881e60` here.
- Which mips change: the mip records (0x50-byte entries, +0x18 onward) carry the level index and the
  file offset (`*(ulong*)(rec+8)` & ~0xffff gives the offset within the region). The range that changes is
  from the current resident level to the target. **observed** (record fields) / **inferred** (meaning)
- Copy completion: CPU job. The chunk factory `0x1129a0` (object 0x88 bytes, `TextureStreamingChunk`);
  `0x112b80` (`ProcessTextureData`) kicks job array `0x35480` with body `0x112d30`, which calls `0x98ae0`.
  Done when the job counter at chunk+0x70 reaches zero. **observed**
- Texture-to-texture copy `0x98800` (585 bytes): copies per mip (256-byte units) from the source
  (param_3 + 0xc4) to the destination (param_1 + 0xc4). **Only when the 4th argument (PM4 writer) is non-null**
  it uses DMA (`0x85db40` -> `0x86a700`). Callers: `0x10e9b0` (particle atlas) and `0x186a60`
  (copy between two textures with equal size/format). **observed**. GPU completion of that DMA: **unknown**
  (no fence or label read).

### 3.4 Destroy and reuse

- Dtor `0x985d0` (177 bytes, vtable slot 0): sets the vtable, then for each of +0xa0, +0xa8, +0xb0 with a
  non-zero value calls `0x90300(heap=DAT_02ed33f0, region)` and clears the field. If +0x1b8 != -1, calls
  `sceGnmUnregisterResource` and sets -1. Then the base dtor `0x1124b0` (-> `0x112a80`, frees the streaming
  job at +0x60). **observed**
- `0x90300` (282 bytes): spin lock on heap+0x7c0, pushes the pointer into a vector at heap+0x7d0, unlocks.
  It is a **deferred-release queue** (push under lock). Who drains it and when (after GPU use?) is **unknown**.
  **inferred** from the push pattern. Note: `02-resource-objects.md` described this as a "view" free; the
  argument is the region pointer, so it is a memory release.
- Deleting dtor `0x98720` (28 bytes, vtable slot 1): calls `0x985d0` then frees the object with `0x170b0`.
  Direct caller: none (called through the vtable). Resource-level delete `0x1478d0` (693 bytes) was only
  partly read: it finds the entry in a map keyed by hash at +0x38 and clears +0x20. Whether it calls slot 1
  is **unknown**. The failure path of `0x97a50` calls slot 0 (`0x985d0`) directly.
- Object reuse: none found. Each create allocates a new 0x1c0 object (`0x17070`) and each destroy frees it
  (`0x170b0`). The memory regions are released to the deferred queue, not kept in a texture pool. **observed**

### 3.5 Render targets and dynamic textures

- `render_targets_allocate` (`0x9aef0`, 19,353 bytes, caller `0x95a90` / `VideoOut_OpenSetFlipRate`
  `0x95dd8`) creates named targets through `0x97a50`. Names are passed as the 9th argument (in every call:
  `"kRTT_DOFAmount"`, `"kRTT_OccluderDepth"`, `"kRTT_FogScatter0"`, ...). **observed** (call sites)
- Registration: `0x97a50` calls `sceGnmRegisterResource(&obj+0x1b8, owner=*0x2ed37a4, base=+0xa0,
  size, name, 5, 0)` (`0x984da`) only when `param_5 == 1` and `param_7` is 1 or 3 and the name pointer is
  non-zero. **observed**. Other RT calls (e.g. `param_7 == 0`) create the same kind of region without
  registration. **observed**
- A static texture (`0x96680`, `0x96f70`, `0x97520`) is never registered and has no name. **observed**
- Flags that separate the two classes at runtime: the GNM registration id at +0x1b8 (`!= -1`) and the
  creation event (T1b) with a name. The per-texture format id is the `param_6` of `0x97a50`. **observed**
- Per-frame writes by the GPU: no engine-side dirty flag found. The engine never reads back a render
  target on the CPU in the code read. **unknown** for the GPU write events. The port must derive them
  from its own draw targets (see section 6).

### 3.6 CPU-written textures

- `CPUTexture` (`0xa2ac00`) is a CPU-side buffer object (section 3.1). It is written once by the factory
  (`0x8de50` from the stream). No later write was found in the factory. **observed** (factory only)
- The CPU-to-GPU upload path for it is **unknown**. No T# is built from it in the factory.
- Video / UI textures (other factories) were not read. **unknown**

## 4. Hook points

Mode names follow `scripts/hooks.d/_example.py` (entry, wrap, site). Pattern bytes are not generated
here; derive them with `tools/re/eboot_re.py sig ADDR` and check with `sigfind`. Use `hooks_check.py`.
Args are SysV: rdi, rsi, rdx, rcx, r8, r9, then stack.

| ID | Event | Site (addr) | Mode | Arguments at the site | State valid at the point | Label |
|---|---|---|---|---|---|---|
| T1 | Texture created (file or particle) | `0x96680` post | wrap (post) | rax = CTexture*; rdi = stream | T# complete, base and levels set, resident bytes final (CPU). Existing hook: `native_writes` post texel copy in `0x96680` | observed |
| T1b | Descriptor / RT texture created | `0x97a50` post | wrap (post) | rax = CTexture*; name = [rsp+0x18] at entry (9th arg) | T# complete; registration done if registered | observed |
| T1c | Derived alias created | `0x97520` post, `0x96f70` post | wrap (post) | rax = new CTexture*; source = rdi (`0x97520` param_1, `0x96f70` param_1 is the source) | new object shares source memory | observed |
| T2 | T# base address written | `0x881e60` entry | entry | rdi = CTexture+0xc4 (CTexture = rdi-0xc4); esi = base>>8 | before the write; creation only | observed |
| T3 | Base or last level written | `0x880c30` entry | entry | rdi = CTexture+0xc4; esi = base_level; edx = last_level | before the write; from creation and streaming (`0x98da0`, `0x98df0`) | observed |
| T4 | Streaming request started | `0x112b80` entry | entry | rsi = CTexture; edx = target level | job about to be kicked | observed |
| T5 | Mip residency change | `0x98ae0` entry (mipbudget hook already here) | entry | rdi = CTexture; esi = target level | before change; exit gives the new state | observed |
| T5x | Mip residency changed | `0x98ae0` exit | entry (after return) | rdi = CTexture | level and committed range updated | observed |
| T6 | Streamed copy finished (CPU job) | `0x112d30` exit | entry (after return) | rdi = chunk (CTexture at chunk+0x78) | copy done; job counter at chunk+0x70 may still be non-zero (check) | observed |
| T7 | Texture-to-texture copy (dst modified) | `0x98800` entry | entry | rdi = dst CTexture; rdx = src CTexture; r8 = PM4 writer (non-zero: DMA) | DMA queued (GPU completion unknown) or CPU copy done (r8 zero) | observed |
| T8 | Texture destroy | `0x985d0` entry | entry | rdi = CTexture | before the region release and GNM unregister | observed |
| T8b | Deleting destroy | `0x98720` entry | entry | rdi = CTexture | as T8 | observed |
| T9 | GNM register (RT only) | import `sceGnmRegisterResource` (NID hook, no address) | import | rdi = &obj+0x1b8; rdx = base (+0xa0); rcx = size; r8 = name; r9 = 5 | after the T# is written | observed (decomp args) |
| T10 | GNM unregister | import `sceGnmUnregisterResource` | import | rdi = resource id (expected); the decomp shows no argument setup | before the object is freed | observed call; **args unknown** (check disasm of `0x985d0`) |
| T11 | Region released to deferred queue | `0x90300` entry | entry | rdi = heap (`DAT_02ed33f0`); rsi = region pointer | queued; the drain point is unknown | observed call; semantics inferred |
| T12 | CPU texture created | `0xa2ac00` exit | entry (after return) | rsi = out pointer | CPU buffers only | observed |

Import hooks (T9, T10) need no address and follow the rule in `docs/HOOK-DESIGN.md` (kind 1). They work on
any build. The factory and streaming sites (T1 to T8) need signatures.

Notes on each site:

- T2 and T3 are the cheapest events for the cache: they fire on every T# change and need no per-draw lookup.
  T3 also fires from the streaming state machine, so it covers base-level changes.
- T5 and T1 cover residency. T5 fires before the change; use T5x or T6 to know when the data is final.
- T7 is the only event that changes texture contents other than through creation or streaming. A copy into
  an existing texture must invalidate its image.
- T8 is the only point where the object and its regions are about to die. The region pointers are still valid there.

## 5. What a texture cache needs, and which events give it

| Cache need | Event | Notes |
|---|---|---|
| Register each engine texture once (VkImage) | T1 (file), T1b (RT), T1c (alias) | Key on the CTexture pointer. Alias (T1c) shares memory with its source: map to the same VkImage, or a view |
| Know the T# of an image | T2, T3 (after creation) | Re-read the 32 bytes at +0xc4 on T2/T3 |
| Invalidate on real change | T5x, T6 (streaming), T7 (copy destination) | Not on every draw |
| Free on destroy | T8, T8b | Also drop aliases that point to the same memory |
| RT vs static | T9 (registered), T1b name | Static textures: no event after creation except streaming and copy |
| RT writes by the GPU | none in the engine | Track from the port's draw targets |

## 6. Gaps and risks

- **RT writes by the GPU**: no engine event. The cache must mark RTs dirty from its own draw targets.
- **GPU completion of DMA copies** (T7 with a PM4 writer): no fence read. Unknown when the destination is final.
- **Deferred release drain** (T11): the drain point and the GPU fence are unknown. The port must not free a
  VkImage before the region release, so it should key on T8 (object) and wait for its own fence.
- **Alias lifetime** (T1c): the alias shares memory with the source. It is unknown whether the source can be
  destroyed while an alias is alive. Not checked.
- **Index limits**: `eboot_re.py refs` did not show the CTexture vtable (`0x15977d0`) in direct references
  (only `0x15977f0` via `sub_9a9a0`), so caller counts for constructors came from decomp. `icallers` shows 0
  callers for GNM register/unregister (import stubs missed), but the decomp of `0x97a50` and `0x985d0` shows them.
- **Not read**: `0x1478d0` (rest), `0x9aef0` (19 KB, only the call list), `0x117930` (only the head),
  `0x13d0f0` / `0x13d280` (slot helpers), `0x186a60` callers, `0x12820`/`0x12950` pool internals,
  `0x88f000` (mip offset helper), `0x112b30`, `0x14cf00`/`0x14d2c0` (load-state checks: inferred as readiness).
- **US build**: the same code is assumed (see `docs/HOOK-DESIGN.md`). Not checked here.

## 7. Open items

1. Disassemble `0x985d0` at the `sceGnmUnregisterResource` call to get its argument.
2. Read `0x9aef0` head (RT allocator flow) and its caller `0x95a90`, to know when RTs are created (startup or resize).
3. Read `0x90300`'s drain: find the reader of the vector at heap+0x7d0.
4. Read `0x186a60` callers: which game path copies texture to texture (T7).
5. Read `0x1478d0` fully: whether `DeleteResource` calls slot 1 (`0x98720`).
6. Read `0x88f000` for the mip offset and size function used by `0x98800`.
7. Confirm the T# bit layout (dword3 BASE_LEVEL/LAST_LEVEL) against the port's T# decoder in `gpu/shadps4`.


## Port implementation (SOTC_TEX_LIFECYCLE)

Switch `SOTC_TEX_LIFECYCLE`: 0 or unset off, 1 fast validation, 2 verify (today's path plus counts only).
Hooks: `scripts/hooks.d/texlife.py`. Each hook has its own group, so one refused site does not switch off the
others. Handlers and registry: `gpu/sotc/texlife.h` / `gpu/sotc/texlife.cpp`. Cache use: `UpdateImage` in
`gpu/shadps4/video_core/texture_cache/texture_cache.h` (per bind, not FindImage). Report: `SotcTexLife::Report`
in the 5 s report of `driver.cpp`.

Sites (eboot VAs, EU 1.01; `hooks_check.py` resolves each to one site):

| Hook | Site | Mode | Event | Handler reads |
|---|---|---|---|---|
| `TEXLIFE_CREATE_FILE` | `0x98ac0` | site (after `call 0x96680` in `0x98aa0`) | create (file texture, T# complete) | rax = CTexture |
| `TEXLIFE_LEVEL` | `0x880c30` | wrap, pre + post | level (T3) | pre: CTexture = rdi - 0xc4 |
| `TEXLIFE_MIP` | `0x112d30` | wrap, pre + post | streamed mip copy done | pre: CTexture = [chunk+0x78] |
| `TEXLIFE_DESTROY` | `0x985d0` | entry | destroy (T8) | rdi = CTexture |

Why not `0x96680` itself: a wrap or a site inside it is refused by the CPU patch pass while the NATIVE_TEX_COPY
and NATIVE_TEX_TAIL_COPY sites are installed there ("the function has other CPU patches"). The check in
`gpu/sotc/cpu_patches.cpp` compares the function's bounds with every patched range, for Site mode as well. The
caller site avoids it. Static scan of the sites: none of `0x96680`, `0x98aa0`, `0x880c30`, `0x112d30`,
`0x985d0` contains an SSE4a, rdtsc or `fs:` instruction (the CPU patch passes that set patched ranges, apart
from red-zone rewriting, which is decided from the function list at runtime and was not checked here).

Not hooked, and why:
- `0x97a50` (descriptor / render target), `0x97520` and `0x96f70` (derived aliases): not registered, so their
  images have no record and take today's path. A render target is never a record, so the fast path cannot skip
  its upload. Aliases are not registered either: an alias of a static texture is covered by the source's base.
- Particle textures (`0x10e9b0` calls `0x96680`): not hooked, so they have no record.

Registry (`texlife.cpp`): key = guest base of the T# (dword0 << 8). Each record has a generation and a live count.
Generations come from one global sequence, so a record erased on destroy and created again at the same base gets a
new value (no image can match a stale one). A CTexture pointer maps to its base for the level, mip and destroy
events. A level or mip event for a CTexture that is not in the registry counts as unmatched.

The registry is used only when all of the create, level, mip and destroy hooks are installed (`Mode()`). Without
one of them a record could go stale (for example, no destroy hook leaves a dead record at a base that is reused),
so the mode is off and the report says which hook is missing.

Cache use (`UpdateImage`): `StaticGen(image.info.guest_address)` gives the generation (0 when the base has no live
record). The image stores the generation it was last refreshed at (`sotc_texlife_gen`).
- Mode 1: when the generation is unchanged and the image has no Dirty or GpuModified flag, `UpdateImage` runs only
  the protect flush (`TexLifeFastFlush`) and skips `RefreshImage`. Any other case runs `RefreshImage` as today, and
  the image stores the generation after it. A changed generation refreshes once.
- Mode 2: always runs `RefreshImage`. When the generation is unchanged, a verify check is counted; it is missed
  when the image had CpuDirty or GpuDirty set before the refresh (a write that no event reported).

Report lines (every 5 s when the switch is set; cumulative since launch):
`SOTC TEX LIFECYCLE: textures live N, events create/level/mip/destroy a/b/c/d, fast validations F, verify checks C, missed events X`
and `SOTC TEX LIFECYCLE: unmatched events U`. If the switch is set but a hook is missing, the line is
`SOTC TEX LIFECYCLE: inactive, hook(s) not installed: ...`.

Limits:
- **Byte range is not in the event.** The registry keeps base and generation only. The byte range stays the port's
  `ImageInfo::guest_size`. The texture's memory regions (+0xa0..+0xb0) are heap pointers, not guest addresses.
- **Only the events above are hooked.** A texture-to-texture copy (T7, `0x98800`) and RT GPU writes are not events.
  A static texture written by such a copy is not seen by the registry: mode 2 counts it as a missed event, and mode 1
  would not refresh the image. Keep mode 1 off for scenes that copy into engine textures until mode 2 shows zero
  misses there.
- **Gain of mode 1 is expected to be small.** `RefreshImage` already returns early for a clean image. Mode 1 saves
  that call and the protect flush only; the per-bind cost is mostly `TrackImage`, `TouchImage` and `NoteBoundImage`,
  which the mode keeps.
- **Mip-tail or rebased images** (guest address != T# base) find no record and take today's path.
- **Not built or run here by the author of this section.** The syntax check and `hooks_check.py` on the EU eboot pass.
  Whether each site survives the runtime CPU patch pass, the counts, and the frame-time effect were not measured.
