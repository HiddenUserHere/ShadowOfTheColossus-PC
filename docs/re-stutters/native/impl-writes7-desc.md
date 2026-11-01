# Writes 7, group "desc": result

Specialist report (Haiku), saved by the coordinator from the hand-back. Read-only analysis, nothing built.

| rip | function | object written | class | hook |
|---|---|---|---|---|
| 0x865eaa | sub_865ea0 (writer init, 69 B) | base/end/cursor header of a DCB/CCB/ACE writer inside the context object (0x238 B, sub_90470) | (c) CPU bookkeeping | no |
| 0x86f308 | sub_86f2a0 (push marker) | DCB writer cursor +0x10 after sceGnmInsertPushMarker; the marker bytes are inside the announced DCB | (c) | no |
| 0x861522 | sub_861520 (bind-state cache) | dirty flag +0xc252 of the bind-state object at owner+0x2e8 (~50 KiB) | (c) | no |
| 0x16e9dc | sub_16e990 (CComputeContext ctor) | dword +0x1ae8 of the 0x1af0-byte compute context | (c) | no |

None writes command payload or descriptor words: those are covered by the DcbSubmit / CcbSubmit announces and the
NATIVE_DESC_* records. The "consumed" counts come from 128-192 KiB binds that overlap the objects' pages, so the
consumed filter still has false positives for CPU objects next to GPU data.

Open:
1. CComputeContext submit: its DCB/CCB (two 0x12820 blocks) are announced only if they reach KickoffSubmitJob's lists
   (job+0x210 for 0x91f80, param[0x80..0x83] for 0x16f100). Not proven; check at runtime by logging each submit's
   ctx base against the compute context blocks.
2. Indirect callers of sub_85c2a0 / sub_85cf40 / sub_862880: the object passed in rdi is not identified.
3. The context allocator 0x17070 was not traced (heap inferred).

Note: the oracle records every write fault as 8 bytes (the fault handler's granule), not the store width.
