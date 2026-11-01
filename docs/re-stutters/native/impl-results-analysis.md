# N3 implementation report: GPU-result consumers (agent "results")

Status: analysis only. No code was written, nothing was built, no game was run. Uncommitted.

## Base and environment

- The brief's files live on branch re/pc-native (main tree). This worktree is on an older base (606f800), so
  nothing was copied. Reads were done with `git show re/pc-native:<path>` and the main tree's out/re/decomp.
- gpu/sotc/native/native.h on re/pc-native was read: the contract (Announce, Release, RegisterWindow, FrameWait)
  has no result-consumer API. No signature needs to change.

## Verified facts (from decomp and eboot_re.py dis)

1. Mask consumers take the mask as a pointer argument, walk it with a bit scan over (count+63)/64 qwords, and
   read the object count as u16 at param_1+0x64.
   - FUN_001a6d20 (C1): `(param_1, param_2 = mask, param_3, param_4)`. Mask in rsi.
   - FUN_001a7290 (C2, indirect): `(param_1, param_2, param_3, param_4, param_5)`. Two masks. Signature still to confirm.
   - FUN_001a8a40 (C3, decal): `(param_1 = owner, param_2 = mask, param_3, param_4)`. Mask in rsi. Called from FUN_0019c740.
   - FUN_001a8180 (C4): `(param_1, param_2, param_3, param_4, param_5)`. Mask position not yet confirmed.
2. RenderXPRDecals 0x11e190 (S1 spin). Disassembly (`eboot_re.py dis 0x11e190`):
   - 0x11e1db..0x11e1e2: `cmp byte [rax+0x16],0; je 0x11e210` selects the branch.
   - Spin loop at 0x11e210: `mov rax,[rsi]; cmp rax,1; jne 0x11e210` (9 bytes, 0x11e210..0x11e218).
   - Else branch at 0x11e1e4..0x11e200: `call 0x35590` (JobSystem_WaitForCounter on the same label, value 1).
   - The decal loop (0x19c740 -> 0x1a8a40) reads the decal mask after the wait. So S1 cannot be removed while C3 reads
     GPU masks; otherwise the CPU reads GPU memory without the wait.
3. Water C6 0xe48b0 (ReadQueries). Disassembly:
   - 0xe491a: `jb 0xe493a` (bytes 72 1e) skips the read when `[r14+0x118] < 2`.
   - 0xe492b: `call 0x8c1b0` (the ring accessor), then `mov eax,[rax+4]` (0xe4930) and the store to +0x11c (0xe4933).
   - The value at +0x11c has no consumer in the decomp (gpuresults Q1). Option 3 (read the fence-complete slot) still reads
     GPU-written memory, so the cleaner variant is to drop the read.
4. Particles C5: the slot read in ProcessStats call k is the slot dispatched in call k-2 (the particle_prev.cpp timeline).
   Option 3 is therefore the current read. Making it stall-free needs a guest-addressable finished copy of the count array;
   the RB_CLASS copy target (rb_class.h, RequestCopy) is not a guest address. Not built.
5. Guest memory for an all-ones mask buffer: `runtime_low_map(size, prot)` (src/runtime_memory.c, exported, low
   guest-visible range from 0x800000000) is the candidate allocator. Its Windows prot mapping (win_prot) has not been read.

## Work not done (and why)

- Masks fallback (SOTC_NATIVE_MASKS=all): not implemented. Needs (a) the all-ones buffer from runtime_low_map, (b) entry
  hooks on 0x1a6d20, 0x1a7290, 0x1a8a40, 0x1a8180 with the mask pointer replaced, (c) confirmed patterns through
  `eboot_re.py sig`. Pattern bytes were not collected.
- Masks design A (compute + DrawIndexedIndirectCount): not implemented. Needs engine command emission and a compute pass;
  it is a follow-up. The count/args layout is in gpuresults.md.
- Water: intended as an Entry hook on 0xe48b0 returning early (HookAction::Return), or as the 2-byte static patch 72 1e -> eb 1e
  at 0xe491a. The static patch is simpler and keeps the WAIT_REG_MEM packet. Not added to patches.bin.
- S1 spin: static patch of 0x11e210..0x11e218 to `e9 04 00 00 00 90 90 90 90` (jmp 0x11e219). Gate: only when
  SOTC_NATIVE_MASKS is not gpu. Not applied; the else branch (JobWait at 0x11e1fb) stays until it is patched too.
- Particles option 1: needs emitter stop state and maxLife (not read in the decomp yet).
- Page sharing D: see the list below.

## Coordinator addition (sync specialist, sync.md rev 2)

Additional XPR-culling label spins to remove together with the masks. Shape
`if (obj[9]+0x16==0) do{}while(label!=1) else JobWait(label,1)`:

- 0x11dbe0 (label obj[7]+0xab70)
- 0x11d7a0 (BPXPR_RecordMaskedDraws, label obj[7]+0xa800, 6 callers)
- 0xceae0 (label obj[7]+0xab70)
- 0x11e190 RenderXPRDecals (label obj[0x1d]+0x78), as above.

Writer: 0x925c0 (EOP event 0x28, data 1), from 0xc6bd0 (+0x40), 0xc6f70 (+0x68), 0xc5200 (+0x78).
Plan: skip each spin and its JobWait branch only when the masks no longer need the GPU result (SOTC_NATIVE_MASKS=all).
In verify mode, count how often the label is not yet 1 when skipped. These four sites are not yet disassembled in this
task; the 0x11dbe0 and 0xceae0 shapes are taken from the coordinator's note, not checked.

## Page sharing (D), placement list for the windows agent

From gpuresults.md, CPU-owned records on pages that GPU-written buffers also use:
- BpeHeap free-list nodes (0x48a70, 0x493d0, 0x49030, 0x495e0, 0x179c0, 0x17e90): heap metadata, C7.
- Object records copied into the upload staging array (0xf9c40, callers 0xfb1e0 and 0xfb2d0): C8.
- Descriptor records (0x937c0, 0x92af0): C9, C10.
- State block for constant upload (0x1122e0 -> 0x85e8e0): C11.
- Particle pool bookkeeping and the system records (+0xb4, +0x188 time array): C14, sharing with count arrays.
- Water object (+0x118..+0x11c) sits next to the ring at +0xc8 (GPU-written), so the water object should move off that page.
- Profiler scope flag word (0x16b8c0 reads param_3[9]+0x28): C14, not GPU, but also page-shared.

Fix requested: place each CPU record class on CPU-only pages (allocator placement, N1).

## Verify checks (planned, not implemented)

- Masks: count calls where the real mask (read through SotcGuest, verify only) differs from all-ones within count.
- Spins: count skipped waits where the label is not 1 at the skip point.
- Water: count changes of +0x11c with the read kept.

## Remaining

Everything in "Work not done". The first safe step is the water static patch plus the masks verify counters, then
the all-ones fallback with confirmed patterns.
