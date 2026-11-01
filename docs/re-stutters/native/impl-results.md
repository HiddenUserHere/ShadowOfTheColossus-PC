# N3 implementation: GPU-result consumers (agent "results")

Status: implemented, syntax-checked, not built, not run in the game. Uncommitted, in the worktree
agent-a0fbf0edadc252858 (detached at re/pc-native ebf7e5c, so the base is the main tree's current commit).

## Files

| File | State | Content |
|---|---|---|
| `scripts/hooks.d/native_results.py` | new | 9 hook declarations (4 entry, 5 site), switch `SOTC_NATIVE`, groups `native_masks`, `native_spins`, `native_water` |
| `gpu/sotc/native/native_results.cpp` | new | handlers, all-ones mask block, verify counters, `Report` |
| `gpu/sotc/native/native_results.h` | new | `SotcNativeResults::Register()`, `SotcNativeResults::Report()` |
| `docs/re-stutters/native/impl-results.md` | new | this report |
| `scripts/sotc_modes.py` | snippet only (below) | byte patches for the spins and the water read |
| `gpu/sotc/hook_handlers.cpp` | snippet only (below) | registration |
| `gpu/shadps4/core/libraries/videoout/driver.cpp` | snippet only (below) | 5 s report line |

Syntax check: `native_results.cpp` compiles with `-fsyntax-only` using the flags of `gpu/sotc/probe_xpr.cpp`
from `out/gpu/compile_commands.json` (worktree include dirs first, `-MD/-MF` dropped). Clean, no warnings.
`tools/re/hooks_check.py` and `tools/re/hooks_install_check.py` (eboot `out/eboot.elf` of the main tree):
all 9 native hooks resolve at the expected offsets and are accepted by the detour engine.

## 1. Masks fallback (SOTC_NATIVE_MASKS=all)

Consumers, verified in the decomp (`out/re/decomp`), all take the object in rdi:

| Hook | Consumer | Mask argument(s) | Count |
|---|---|---|---|
| `NATIVE_MASK_VIS` 0x1a6d20 | BPXPR_ConsumeVisibilityMask (C1) | rsi | u16 at obj+0x64 |
| `NATIVE_MASK_VIS_IND` 0x1a7290 | BPXPR_ConsumeVisibilityMaskIndirect (C2) | rsi and rdx (both read: an object is drawn when both bits are set, a merge walk) | u16 at obj+0x64 |
| `NATIVE_MASK_DECAL` 0x1a8a40 | sub_1a8a40 (C3, from sub_19c740) | rsi | u32 at obj+0 |
| `NATIVE_MASK_C4` 0x1a8180 | sub_1a8180 (C4, from sub_19b330) | rsi (rdx is the object record, not a mask) | u16 at obj+0x64 |

Mechanism: Entry hooks, the handler sets `regs.rsi` (and `regs.rdx` for C2) to the address of one all-ones block.
The block is a single 64 KiB `runtime_low_map(65536, PROT_READ|PROT_WRITE)` (guest-visible, low range), filled
with 0xff once on first use. Size argument: 8192 qwords = 524288 bits. A consumer walks `(count+63)/64` qwords
with count <= 65535, so one block covers every mask length; a call whose count would exceed the block keeps the
real mask and counts `bad_count`. Count 0 returns early in the consumer and is left alone. The consumers only
read the mask, never write it.

Result: every object up to the count is drawn (all visible), and the CPU never reads the GPU-written mask in
these four functions. Mode 0 is unchanged: the hooks are off without `SOTC_NATIVE`.

Safety argument: the masks fallback is safe for these functions only if the mask is their only GPU-result read.
The decomp shows only the mask pointer reads in C1 to C4; the other inputs (the per-object bit array at
`plVar[4]`, indexed with `param_3+0x20` or `param_4+0x20`, and the object record) were not traced to their writers.
That is an open check, not a verified fact.

## 2. XPR label spins (SOTC_NATIVE_MASKS=all)

Spins checked in the decomp and with the raw bytes (capstone via `out/eboot.elf`):

| Function | Spin / skip site | Original | Patch (jump over spin and JobWait) | Target |
|---|---|---|---|---|
| RenderXPRDecals 0x11e190 (S1) | 0x11e1e2 `je 0x11e210` | `74 2c` | `eb 35` jmp 0x11e219 | 0x11e219 `lea rdi,[rbp-0x50]` |
| IndexCullXPRObjects 0x11dbe0 | 0x11dc49 `je 0x11dc70` | `74 25` | `eb 2e` jmp 0x11dc79 | 0x11dc79 |
| BPXPR_RecordMaskedDraws 0x11d7a0, spin 1 | 0x11d80f `je 0x11d830` | `74 1f` | `eb 28` jmp 0x11d839 | 0x11d839 |
| BPXPR_RecordMaskedDraws 0x11d7a0, spin 2 | 0x11daeb `je 0x11db10` | `74 23` | `eb 2c` jmp 0x11db19 | 0x11db19 `mov rax,[rsp]` |

Notes:
- 0xceae0 is not a fourth spin: it is a 42-byte thunk (`cmp byte [rcx+0x20],0; je ret; ...; jmp 0x11dbe0`),
  so the spin there is 0x11dbe0's. Its only job is to tail-call IndexCullXPRObjects.
- The 0x11d7a0 function has two label spins (decomp lines 36 and 108), not one. Both are patched.
- Each patch is 2 bytes and was checked against the image: the original bytes match, and the new short jump
  lands on the continuation (script check, all five sites).
- The byte patches go through `patches.bin` (scripts/sotc_modes.py), gated by the group's expected bytes, so a
  different build leaves them off. The patch is only applied when `SOTC_NATIVE_MASKS=all`, because with the GPU
  masks the spin is still the only thing that orders the CPU mask read after the GPU write.

Verify (per continuation site, `NATIVE_SPIN_*` Site hooks, rsi = label pointer on the skip path):
the site counts `skips` and `label not ready` (the label at [rsi] is not 1). With the spin patched, every arrival
is a skip, so the counter is exact. The sites are not counted without `SOTC_NATIVE_MASKS=all`, because rsi is
clobbered by the JobWait call on the other path.

Risk to review: the spin is the CPU's only wait on the label that the GPU's EOP event (writer 0x925c0) sets to 1.
Skipping it lets the CPU run ahead of the GPU on that label. Whether the label memory is re-armed (set to 0) by the
CPU later, and whether a late EOP write can then corrupt the next frame, was not traced. The verify counter
`label not ready` measures how often the skip happens before the GPU write.

## 3. Water ring read (0xe48b0, SOTC_NATIVE=1)

- Patch `72 1e` (jb 0xe493a at 0xe491a) to `eb 1e` (jmp 0xe493a): the `[r14+0x118] < 2` path already skips the
  read; this patch skips the read when it is taken. The WAIT_REG_MEM packet (BPGnm_EmitWaitRegMem at 0xe490b) stays.
- The store `mov [r14+0x11c],eax` is not read by any consumer (gpuresults Q1).
- Mode 2 keeps the read (no patch) and counts the stores and the changes (`NATIVE_WATER_STORE` Site at 0xe4933,
  `41 89 86 1c 01 00 00`), which is the verify for "count changes of +0x11c".

## 4. Particles (C5, ProcessStats 0x1018c0 / 0x101a98)

Not done. Option 1 needs the emitter stop state and the maxLife fields, and they are not pinned in the decomp
yet. Follow-up: read the ProcessStats body from the call at 0x1019af (sub_8c1b0 caller) and the emitter record
fields that gate the per-system count, then decide between option 1 (count from the CPU-side pool) and option 3.

## 5. Verify counters

`NATIVE_SPIN_*`, `NATIVE_MASK_*` and `NATIVE_WATER_STORE` are installed with `SOTC_NATIVE` set. The masks handler
counts in both modes; the real-mask comparison runs only in mode 2 (SOTC_NATIVE=2):

- masks: `calls`, `swapped` (masks=all), `verify bits` (bits below the count), `zero` (of those, bits that were 0
  in the real mask, after `real & second mask` for C2), `calls with zero` (calls with at least one zero bit),
  `unreadable`, `bad count`.
- spins: `skips`, `label not ready`, `unreadable`.
- water: `stores`, `value changed`, `unreadable`.

The window line is `SOTC NATIVE_RESULTS mode=<n> masks=<all|gpu> window=<s>`. Caveats: "frames where the real value
would differ" is counted per call (`calls with zero`), not per frame; the real mask is read from the backing view
(`SotcGuest::Backing`), which in mode 2 holds what the old page-tracking path read back.

## Snippets for files this task does not own

### scripts/sotc_modes.py

Add after the `PSTATS_STORE` / `POOL_FIX` definitions:

```python
# sotc: PC-native GPU-result consumers (docs/re-stutters/native/impl-results.md). The XPR label spins: each skip
# branch jumps to the continuation (2-byte short jumps). SOTC_NATIVE_MASKS=all only (the masks are all visible then).
NATIVE_SPINS = [
    (BASE + 0x11e1e2, bytes.fromhex('742c'), bytes.fromhex('eb35')),  # RenderXPRDecals: je 0x11e210 -> jmp 0x11e219
    (BASE + 0x11dc49, bytes.fromhex('7425'), bytes.fromhex('eb2e')),  # IndexCullXPRObjects: je 0x11dc70 -> jmp 0x11dc79
    (BASE + 0x11d80f, bytes.fromhex('741f'), bytes.fromhex('eb28')),  # BPXPR_RecordMaskedDraws: je 0x11d830 -> jmp 0x11d839
    (BASE + 0x11daeb, bytes.fromhex('7423'), bytes.fromhex('eb2c')),  # BPXPR_RecordMaskedDraws: je 0x11db10 -> jmp 0x11db19
]
# WaterRendering::ReadQueries: jb 0xe493a -> jmp (the ring read is skipped). SOTC_NATIVE=1 only (mode 2 keeps it).
NATIVE_WATER = (BASE + 0xe491a, bytes.fromhex('721e'), bytes.fromhex('eb1e'))


def native_result_patches(elf, ph, supported, log=print):
    """Groups for SOTC_NATIVE (each group applied only when all its bytes match). Empty when SOTC_NATIVE is unset,
    so mode 0 gets no patch."""
    native = os.environ.get('SOTC_NATIVE', '0')
    groups = []
    if not supported or native in ('', '0'):
        return groups
    if os.environ.get('SOTC_NATIVE_MASKS') == 'all':
        if all(holds(elf, ph, address, expected) for address, expected, _ in NATIVE_SPINS):
            groups.append([patch_entry(*entry) for entry in NATIVE_SPINS])
            log('Native: XPR label spins removed (masks all visible)')
        else:
            log('Native: spin bytes differ, not applied')
    if native == '1':
        if holds(elf, ph, NATIVE_WATER[0], NATIVE_WATER[1]):
            groups.append([patch_entry(*NATIVE_WATER)])
            log('Native: water ring read skipped')
        else:
            log('Native: water bytes differ, not applied')
    return groups
```

In `build_patches`, before `return groups, cvars`:

```python
    groups += native_result_patches(elf, ph, supported, log)
```

### gpu/sotc/hook_handlers.cpp

```cpp
#include "native/native_results.h"   // with the other includes

// in RegisterAllHandlers(), after SotcFreecam::Register():
    SotcNativeResults::Register(); // sotc: PC-native GPU-result consumers (SOTC_NATIVE)
```

### gpu/shadps4/core/libraries/videoout/driver.cpp

```cpp
#include "../../../../sotc/native/native_results.h"  // next to the hooks.h include (line 51)

// next to SotcProbes::ReportParticlePrev(report, seconds) (line ~2061):
            SotcNativeResults::Report(report, seconds);  // sotc: SOTC_NATIVE
```

## Interactions and open points

- `NATIVE_MASK_VIS` and `NATIVE_MASK_VIS_IND` displace the same bytes as the existing `XPR_CONSUME_MASK` and
  `XPR_CONSUME_MASK_INDIRECT` probes (scripts/hooks.d/xpr.py, switch `SOTC_HOOK_XPR_PROBE`). The engine refuses the
  later one when both are on (`CONFLICT` line of hooks_install_check). Do not enable the XPR probe together with
  SOTC_NATIVE.
- The water store hook is installed in mode 1 too, but unreachable there (the store is skipped by the patch).
- Not checked: whether sub_19af20 (called from 0x11d7a0), sub_19b110 and sub_19c740 read any other GPU-written data
  besides the masks. The analysis assumed the masks were the only GPU result there.
- Not built and not run (per the brief). Nothing committed.

## What remains

1. Build (`ninja -C out/gpu bb-probe`, after applying the two snippets and the sotc_modes group), then a short run with
   `SOTC_NATIVE=2 SOTC_NATIVE_MASKS=all` to read the counters. Expected: `label not ready` near 0 (if not, see Risk above).
2. Particles option 1 (section 4).
3. A `SOTC_NATIVE_MASKS=gpu` design (compute + DrawIndexedIndirectCount, gpuresults.md design A) to replace the all-visible
   fallback once the verify numbers show how many objects the real masks cull.
