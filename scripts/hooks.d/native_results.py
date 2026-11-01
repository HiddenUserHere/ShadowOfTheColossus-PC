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
    # SOTC_NATIVE_MASKS=prev (gpu/sotc/native/native_masks.cpp): BPXPR_BuildSetDraws right after its cull dispatch
    # (call 0x86b9e0 at 0x19c0f1). r15 = the owner (u32 +0x148 = set entry index, +0x18 = set record with the u16
    # object count at +0x64); the four mask pointers are at [rsp+0x140..+0x158], stored into the entry just after.
    # Displaced: mov eax,[r15+0x148] (7 bytes, no RIP or rel32 operand); no branch targets 0x19c0f6..0x19c0fc.
    dict(
        name='NATIVE_MASK_PRODUCE',
        switch='SOTC_NATIVE',
        group='native_mask_produce',
        mode='site',
        pattern='41 8b 87 48 01 00 00 49',
        expected={EU_FINGERPRINT: 0x19c0f6},
    ),
    # The shadow mask producer sub_19c1a0 (from XPRRunFragmentCullShaderShadowScenes 0xc6bd0), right after its
    # cull dispatch (call 0x86b9e0 at 0x19c6ac): r12 = the owner, the two masks at [rbp-0x368] / [rbp-0x360], the
    # slot index at [rbp-0x348]. Displaced: mov eax,[r12+0x148] (8 bytes); no branch targets 0x19c6b1..0x19c6b8.
    dict(
        name='NATIVE_MASK_PRODUCE_SHADOW',
        switch='SOTC_NATIVE',
        group='native_mask_produce',
        mode='site',
        pattern='41 8b 84 24 48 01 00 00 48 8b 8d',
        expected={EU_FINGERPRINT: 0x19c6b1},
    ),
    # SOTC_NATIVE_MASKS=prev: the decal mask producer sub_19c7e0 (called per decal owner from the decal cull sub_c5200).
    # Site 0x19ce08: right after the cull dispatch (call 0x86b9e0 at 0x19ce03). The owner is at [rbp-0x340] and the
    # mask pointer at [rbp-0x358] (both written once per call). Displaced: mov rax,[rbp-0x340] (7 bytes, no RIP or
    # rel32 operand); no branch targets 0x19ce09..0x19ce0f.
    dict(
        name='NATIVE_MASK_PRODUCE_DECAL',
        switch='SOTC_NATIVE',
        group='native_mask_produce',
        mode='site',
        pattern='48 8b 85 c0 fc ff ff 49 8b 4d 38',
        expected={EU_FINGERPRINT: 0x19ce08},
    ),
    # SOTC_NATIVE_MASKS=prev: the CPU ordering the removed RecordMaskedDraws spins gave. XPRPreRender (0xca6d0)
    # resets the two cull labels (mov [rax+0x40],0; mov [rax+0x68],0, rax = obj) before it queues
    # BPXPR_RunCullPasses; the site is the next instruction (mov [rbp-0x148],rcx, 7 bytes, no RIP operand).
    dict(
        name='NATIVE_CULL_RESET',
        switch='SOTC_NATIVE',
        group='native_cull_sync',
        mode='site',
        pattern='48 89 8d b8 fe ff ff 48 8d 0d ?? ?? ?? ?? c5 fa',
        expected={EU_FINGERPRINT: 0xca9ab},
    ),
    # The decal cull label reset (RenderCullXPR sub_c53d0, the decal list is not empty): `mov qword [rax+0x78], 0`
    # with rax = obj = [cull+0xe8], the label RenderXPRDecals (0x11e190) spins on (label = obj+0x78). The site is the
    # store itself (8 bytes, no RIP operand; the next instruction, mov [rbp-0x80],rcx, is not displaced). No branch
    # targets 0xc54c8..0xc54cf.
    dict(
        name='NATIVE_DECAL_RESET',
        switch='SOTC_NATIVE',
        group='native_cull_sync',
        mode='site',
        pattern='48 c7 40 78 00 00 00 00 48 89 4d 80',
        expected={EU_FINGERPRINT: 0xc54c7},
    ),
    # BPGnm_WriteCsDoneLabel (0x925c0, rsi = the label): the cull job recorded the cull of that label. The same
    # bytes as XPR_CS_DONE_LABEL (scripts/hooks.d/xpr.py); sotc.py turns the XPR probe off under SOTC_NATIVE.
    dict(
        name='NATIVE_CULL_RECORDED',
        switch='SOTC_NATIVE',
        group='native_cull_sync',
        mode='entry',
        pattern='55 48 89 e5 48 8b 3f 48 89 f0 be 28',
        expected={EU_FINGERPRINT: 0x925c0},
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
