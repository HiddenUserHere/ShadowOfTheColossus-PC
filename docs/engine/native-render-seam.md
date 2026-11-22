# Render seam: where the game builds GPU commands (Phase 0.1 finding)

Branch `native/render-vk13`, base `upstream/main` `53fd0292`. Evidence: `out/eboot.elf`
(user's own decrypted eboot), `out/re/orbis/names_orbis.txt` (571/571 imports),
`out/re/decomp/` (56,900 decompiled functions), and the port sources.

## The seam, proven

1. **The eboot statically links the GNM library.** Every PM4 packet builder named in
   `docs/engine/direct-vulkan-inventory.md` (`0x85cea0`, `0x85d260`, `0x86e770`, `0x86e800`,
   `0x86ea50`, `0x86f680`, `0x866360`, `0x884c40`, `0x86fa00`, `0x86fa80`, `0x8663e0`,
   `0x869450`, `0x869530`, `0x883140`, `0x883210`, the `0x86d2e0..0x86d5e0` SET_SH_REG family)
   lives in the eboot's first LOAD segment (`p_vaddr=0`, `p_flags=5` = R+X), file offset
   `0x4000 + va`. These are the game's own compiled-in GNM command-buffer builders, not
   shadPS4 code.
2. **The `sceGnm*` system entry points cross a PLT seam.** The eboot imports them by NID:
   e.g. stub `0xf90416` = `push 0x108; jmp plt0` -> GOT `0x16007f8` -> NID `GGsn7jMTxw4`
   = `sceGnmDrawIndexAuto`; stub `0xf903e6` = `push 0x105` -> `0x16007e0` -> `0BzLGljcwBo`
   = `sceGnmDispatchDirect`. The port's `gnmdriver.cpp:476` / `:376` implement those and
   **themselves write PM4** (`WritePacket<PM4ItOpcode::DrawIndexAuto>` / `DispatchDirect`).
3. **Both writers feed the same command buffer.** The game's static GNM library writes the
   bulk of the packet stream (state, predicates, indirect buffers) directly; the `sceGnm*`
   system calls write the draw/dispatch packets. `sceGnmSubmit*CommandBuffers` (PLT
   `0x15f..0x163`) hands the finished buffer to `liverpool.cpp`, which decodes PM4 -> Vulkan.

```
game RenderJob (0xc*****)                 <-- Phase 2 reimplementation target
   |  calls
   v
static GNM library (0x85xxxx/0x86xxxx)    <-- builds PM4 directly (R+X in the eboot)
   |  + sceGnm* system calls (PLT 0xf9xxxx -> port gnmdriver.cpp)  <-- also writes PM4
   v
one command buffer  ->  sceGnmSubmitCommandBuffers (PLT 0x161)  ->  liverpool.cpp (PM4 -> Vulkan)
```

## Consequence for the rewrite

The native backend replaces the **callers** of the static GNM library — the RenderJob and its
passes/draws — so that instead of `game -> GNM -> PM4 -> liverpool -> Vulkan` we issue
`game -> native Vulkan` directly. The static GNM library is then never entered, which means
**library-internal helper functions are not blocking**: we do not need to know who calls
`0x8663e0` or where the predicate address is produced *inside* the library, because the whole
library is bypassed.

Two things this changes for the plan:
- Phase 2's unit of work is the **RenderJob code that calls the GNM library** (the `0xc*****`
  functions and the draw-object/pass/bind writers in `docs/engine/direct-vulkan-inventory.md`
  section 6), not the `0x85xxxx/0x86xxxx` builders themselves. The builders are the
  *specification* of the GPU state each call sets, which is exactly what the map (Phase 0.2)
  must tabulate: "this GNM call sets these regs -> this Vulkan state".
- The predicate/cull rewrite (Phase 2.7) can key off the RenderJob's own predicate value
  (the 48-bit address passed into `0x85cea0`/`0x85d260`), which the RenderJob computes, rather
  than the library-internal COND_EXEC emitter.

## Opcode reference (decoded from `pm4_opcodes.h`; header = `0xc0 | count<<16 | opcode<<8`)

| Header literal | Opcode | Packet | Emitter(s) |
|---|---|---|---|
| `0xc0012000` | 0x20 | SET_PREDICATION | `0x86e770` (enable), `0x86e800` (disable) |
| `0xc0032200` | 0x22 | COND_EXEC | `0x86ea50`, `0x883300`, `0x866360` |
| `0xc0023f00` / `0xc0023f02` | 0x3F | INDIRECT_BUFFER | `0x86fa00`, `0x86fa80`, `0x884c40` |
| `0xc0044700` | 0x47 | EVENT_WRITE_EOP | `0x86ec50` |
| `0xc0003700` | 0x37 | WRITE_DATA | `0x8663e0` |
| `0xc0016800` / `0xc0NN6800` | 0x68 | SET_CONFIG_REG | `0x883140`, `0x869530`, `0x883210` |
| `0xc0016900` / `0xc0016902` | 0x69 | SET_CONTEXT_REG | `0x869450`, `0x86e2d0` |
| `0xc0NN7600` | 0x76 | SET_SH_REG | `0x86d2e0..0x86d5e0`, `0x8695d0`, `0x869630` |
| `0xc0055000` | 0x50 | DMA_DATA | `0x883640` and 23 other sites |
| `0x80000000` / `0xffff1000` | 0x10 | NOP (fill) | `0x86f680`, `0x885220` |
