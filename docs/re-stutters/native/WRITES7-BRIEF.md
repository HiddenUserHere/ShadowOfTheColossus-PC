# Writes wave 7: the unannounced writes the GPU takes (brief for the specialists)

You are a reverse-engineering specialist of one area of the Shadow of the Colossus PS4 eboot (EU 1.01), working on
the PC-native memory model of this port (`docs/re-stutters/native/PLAN.md`, contract in `gpu/sotc/native/native.h`).
The engine must announce every guest write the GPU reads (`SotcNative::Announce(addr, size, Kind)`), so the port can
delete page tracking. A verify session measured which engine writes are NOT announced and whose bytes the GPU then
uploaded: `docs/re-stutters/native/verify6-consumed.md` (read it first). "via binds <= 64 KiB" is the share taken by a
small bind (a read the shader almost surely makes); a write taken only through huge binds (MiBs, a V# over a whole
heap) may be CPU bookkeeping that merely shares a page with GPU data.

## Your job, per RIP of your group
1. The function and what the instruction writes: the structure, the field, its size, how often (per frame, once).
   Name the object (the decomp often has Bluepoint names and assert strings).
2. Who allocates that memory (heap, allocator call, kind: per-frame ring, persistent resource, pool table).
3. Does the GPU read it? Look for where a V#/T# (buffer/texture descriptor) or a command-buffer pointer to that
   memory is built: descriptor writers 0x865410 / 0x8655d0 / 0x865600 / 0x865500 (framedata.md), constant V# writers
   0x861ce0 / 0x861e60, dispatch 0x928f0, the per-frame heap 0x8c0d0, resource creation 0x8be60. Use the
   "largest bind" and "last address" columns as evidence.
4. Class: (a) GPU-read per-frame data: announce PerFrame after the last write and before the frame's submit;
   (b) persistent GPU data: announce Persistent after its final write; (c) CPU-only bookkeeping that only shares a
   tracked page with GPU data (no announce needed once tracking is gone): say why; (d) unknown: say what is missing.
5. For (a) and (b): a concrete hook. Declaration in the style of `scripts/hooks.d/native_writes.py` (name, switch
   `SOTC_NATIVE`, group, mode entry / wrap / site, pattern bytes, expected offset), and the handler in the style of
   `gpu/sotc/native/native_hooks.cpp` (`AnnounceRange(begin, end, Kind, counter)`, guest reads through `GuestU64`,
   caller filters as `Span` tables). Check the pattern is unique with `python tools/re/eboot_re.py sigfind "<bytes>"`,
   that the displaced bytes (>= 5, whole instructions) hold no RIP-relative or rel32 operand and no branch target
   (`eboot_re.py dis`), and give the registers the handler reads at that point. Prefer one hook that covers a whole
   object over one per field. Hook semantics are in `gpu/sotc/hooks.h`. Existing hooks you must not duplicate:
   `scripts/hooks.d/native_*.py`; existing decisions: `docs/re-stutters/native/impl-writes*.md`.

## Tools and rules
- Source of truth: the Ghidra decompilation `out/re/decomp/<bucket>/<offset>.c` (offset as 8 hex digits, bucket =
  its first 4, e.g. 0x1018c0 -> `out/re/decomp/0010/001018c0.c`), names in `out/re/ghidra/names*.txt` and
  `tools/re/names.txt`. `python tools/re/eboot_re.py func|dis|callers|callees|refs|str|sig|sigfind` for addresses,
  callers and patch bytes (disassembly only to confirm patch sites).
- Go deep: never stop at the first finding. Follow the callers up to where the memory is allocated and down to where
  the GPU gets its address. Cross-check against the verify numbers. Report what stays unknown.
- Do not edit any source file, do not build, do not run the game, do not commit. Write only your report:
  `docs/re-stutters/native/impl-writes7-<group>.md`, one section per RIP plus a summary table (rip, function, object,
  class, hook proposal yes/no). Keep it factual and short; English.
