"""PC-native GPU-result consumers (docs/re-stutters/native/impl-results.md).

The four XPR visibility-mask consumers read a mask that the GPU writes (gpuresults.md). With
SOTC_NATIVE_MASKS=all the port hands them an all-ones mask instead (gpu/sotc/native/native_results.cpp),
so the CPU never reads GPU-written memory there and every object of the XPR list is drawn.

Entry hooks only: the handler swaps the mask argument(s) and the guest function runs as before.
Switch SOTC_NATIVE (any non-zero value installs the group). The handler itself does nothing unless
SOTC_NATIVE_MASKS=all, so SOTC_NATIVE=1 with another masks mode leaves the masks untouched.

    NATIVE_MASK_VIS       BPXPR_ConsumeVisibilityMask, mask in rsi (C1)
    NATIVE_MASK_VIS_IND   BPXPR_ConsumeVisibilityMaskIndirect, masks in rsi and rdx (C2)
    NATIVE_MASK_DECAL     sub_1a8a40 (the decal consumer, called from sub_19c740), mask in rsi (C3)
    NATIVE_MASK_C4        sub_1a8180 (called from sub_19b330), mask in rsi (C4)

The group is all or nothing (one pattern failing turns all four off).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='NATIVE_MASK_VIS',
        switch='SOTC_NATIVE',
        group='native_masks',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x68; mov r9,rdi (unique, no rel32/RIP bytes)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f9',
        expected={EU_FINGERPRINT: 0x1a6d20},
    ),
    dict(
        name='NATIVE_MASK_VIS_IND',
        switch='SOTC_NATIVE',
        group='native_masks',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x58; movzx r11d,word [rdi+0x64] (unique)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 44 0f b7',
        expected={EU_FINGERPRINT: 0x1a7290},
    ),
    dict(
        name='NATIVE_MASK_DECAL',
        switch='SOTC_NATIVE',
        group='native_masks',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x28; mov [rbp-0x48],rcx; mov r15,rdi; movsxd rax,edx;
        # mov r12,rsi; then the mask scan (unique, no rel32/RIP bytes)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 89 4d b8 49 89 ff 48 63 c2 49 89 f4 45 8b 5c 87 50 41',
        expected={EU_FINGERPRINT: 0x1a8a40},
    ),
    dict(
        name='NATIVE_MASK_C4',
        switch='SOTC_NATIVE',
        group='native_masks',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x78; mov r14,r8 (unique)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 78 4d 89 c6',
        expected={EU_FINGERPRINT: 0x1a8180},
    ),
    # Verify of the XPR label spins (SOTC_NATIVE_MASKS=all removes them with byte patches written by
    # scripts/sotc_modes.py, native_results_patches). Each site is the first instruction after the spin's skip target:
    # the label pointer is still in rsi there (the skip path does not touch rsi). A site is reached only
    # by a skip when the spin is patched, so the handler counts only then, and it counts a label that
    # is not 1 yet (a skip where the GPU had not finished the label).
    dict(
        name='NATIVE_SPIN_DECAL',
        switch='SOTC_NATIVE',
        group='native_spins',
        mode='site',
        # RenderXPRDecals (0x11e190): lea rdi,[rbp-0x50]; rsi = label pointer
        pattern='48 8d 7d b0',
        expected={EU_FINGERPRINT: 0x11e219},
    ),
    dict(
        name='NATIVE_SPIN_XPR',
        switch='SOTC_NATIVE',
        group='native_spins',
        mode='site',
        # IndexCullXPRObjects (0x11dbe0, also the thunk 0xceae0): lea rdi,[rbp-0xd8]; rsi = label pointer
        pattern='48 8d bd 28 ff ff ff',
        expected={EU_FINGERPRINT: 0x11dc79},
    ),
    dict(
        name='NATIVE_SPIN_CULL',
        switch='SOTC_NATIVE',
        group='native_spins',
        mode='site',
        # BPXPR_RecordMaskedDraws (0x11d7a0), first spin: lea rdi,[rsp+0x120]; rsi = label pointer
        pattern='48 8d bc 24 20 01 00 00',
        expected={EU_FINGERPRINT: 0x11d839},
    ),
    dict(
        name='NATIVE_SPIN_CULL2',
        switch='SOTC_NATIVE',
        group='native_spins',
        mode='site',
        # BPXPR_RecordMaskedDraws (0x11d7a0), second spin: mov rax,[rsp]; rsi = label pointer
        pattern='48 8b 04 24',
        expected={EU_FINGERPRINT: 0x11db19},
    ),
    # Verify of the water ring read (0xe48b0): the store of the value the read produced. The read is
    # kept in mode 2 (no byte patch there), so this counts how often the value changes; no consumer reads
    # +0x11c (gpuresults.md, Q1). Old value at [r14+0x11c], new value in eax (rax).
    dict(
        name='NATIVE_WATER_STORE',
        switch='SOTC_NATIVE',
        group='native_water',
        mode='site',
        # mov dword ptr [r14+0x11c],eax
        pattern='41 89 86 1c 01 00 00',
        expected={EU_FINGERPRINT: 0xe4933},
    ),
]
