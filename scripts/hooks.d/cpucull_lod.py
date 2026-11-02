"""CPU mirror of the per-model LOD pre-shader (gpu/sotc/native/cpu_cull_lod.cpp, docs/re-stutters/native/CPUCULL-BRIEF.md).

The LOD and fade state the main cull reads (owner+0x58 / owner+0xd0) is written by the GPU pre-shader that the
CPU builds in BPXPR_BuildSetEntry (0x19b400). The mirror steps the same state on the CPU at that dispatch, so the
CPU cull of the frame reads the state the GPU will have. Switch SOTC_NATIVE (any non-zero value installs the group);
the handler does nothing unless SOTC_NATIVE_MASKS=prev.

    NATIVE_LOD_DISPATCH   site 0x19b708 in 0x19b400: `movzx r8d, byte [rbx+0xc8c1]`, right after the material binds
                          (0x85f220) and before the pre-shader dispatch (0x86b9e0 at 0x19b720). Registers at the
                          site: rbx = the command-list context (material = rbx + 0x2e8), r15 = the owner,
                          r13 = the list (stage index at [[r13+0x20]+0x14]), rbp = the frame (header at [rbp-0x160],
                          header V# at [rbp-0x188]). r12 and r14 are NOT the set entry / scene object there.
                          No other detour or CPU patch lies in 0x19b400..0x19b759 (checked in scripts/ and gpu/sotc/).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='NATIVE_LOD_DISPATCH',
        switch='SOTC_NATIVE',
        group='native_lod',
        mode='site',
        # movzx r8d, byte ptr [rbx+0xc8c1]: REX.R 0f b6 modrm(disp32, rbx) disp32 (no rel32 or RIP operand);
        # the displaced bytes are not a branch target: no jump of 0x19b400 lands in 0x19b709..0x19b70f.
        pattern='44 0f b6 83 c1 c8 00 00',
        expected={EU_FINGERPRINT: 0x19b708},
    ),
]
