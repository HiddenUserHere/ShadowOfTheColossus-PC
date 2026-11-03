"""P0 measurement of the XPR draw loop (docs/re-backend/16-draw-path-gaps.md). Observe only.

    XPR_CULL_JOB      0xc5110 BPXPR_IndexCullJob, entry: queues 0x11dbe0 (BPXPR_IndexCullObjects), the GPU cull that
                      clears and writes the 0x14-stride args records the XPR draws read (doIndexBufferCulling).
    XPR_CULL_CASCADE  0xcdb00 the shadow-cascade variant (doSunShadowIndexBufferCulling). Both counted only;
                      0x11dbe0 itself holds other CPU patches, so the detour is refused there.

The draw loop itself (0x1a6d20) is measured from the NATIVE_MASK_VIS handler (gpu/sotc/native/native_results.cpp),
which already holds that entry. Switch SOTC_XPR_GROUPS (gpu/sotc/probe_xpr_groups.cpp). Patterns from
tools/re/eboot_re.py sig (unique).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    # 0x11dbe0 itself holds other CPU patches (the detour is refused there): its callers are counted instead.
    dict(
        name='XPR_CULL_JOB',
        switch='SOTC_XPR_GROUPS',
        group='xpr_groups_job',
        mode='entry',
        # BPXPR_IndexCullJob: lea rcx,[rip+..]; mov rax,rdi; mov rsi,[rax+0x60]; mov ...
        pattern='48 8d 0d ?? ?? ?? ?? 48 89 f8 48 8b 70 60 48 89',
        expected={EU_FINGERPRINT: 0xc5110},
    ),
    dict(
        name='XPR_CULL_CASCADE',
        switch='SOTC_XPR_GROUPS',
        group='xpr_groups_cascade',
        mode='entry',
        # sub_cdb00 (IndexBufferCullShadowCascade): push rbp; mov rbp,rsp; push r15,r14,rbx; sub rsp,0x28; ...
        pattern='55 48 89 e5 41 57 41 56 53 48 83 ec 28 4c 8b 35 ?? ?? ?? ?? 48 89 fb 48 8d 7d d0 b9',
        expected={EU_FINGERPRINT: 0xcdb00},
    ),
    # SOTC_DRAW_CALLERS: the engine's draw and dispatch wrappers, entry; the handler counts the caller ([rsp]).
    # The prologues are not unique alone (eboot_re sig: 2-3 matches); the EU 1.01 expected offsets resolve them.
    dict(
        name='DRAW_WRAP_INDIRECT',
        switch='SOTC_DRAW_CALLERS',
        group='draw_wrap_indirect',
        mode='entry',
        # BPGnm_DrawIndexIndirect: push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x18; mov rbx,rdi; mov r14d,ecx
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89 fb 41 89 ce',
        expected={EU_FINGERPRINT: 0x86b3d0},
    ),
    dict(
        name='DRAW_WRAP_OFFSET',
        switch='SOTC_DRAW_CALLERS',
        group='draw_wrap_offset',
        mode='entry',
        # sub_86b040 (DrawIndexOffset): push rbp; mov rbp,rsp; push r15,r14,r12,rbx; mov rbx,rdi; mov r14d,ecx; ...
        pattern='55 48 89 e5 41 57 41 56 41 54 53 48 89 fb 41 89 ce 41 89 d7 41 89 f4',
        expected={EU_FINGERPRINT: 0x86b040},
    ),
    dict(
        name='DRAW_WRAP_OFFSET2',
        switch='SOTC_DRAW_CALLERS',
        group='draw_wrap_offset2',
        mode='entry',
        # sub_86afd0 (unique, eboot_re sig, 40 bytes)
        pattern=('55 48 89 e5 41 57 41 56 53 50 48 89 fb 41 89 d6 41 89 f7 48 8b 7b 10 48 8b 43 08 48 29 f8 '
                 '48 c1 e8 02 83 f8 08 77 ?? 48'),
        expected={EU_FINGERPRINT: 0x86afd0},
    ),
    dict(
        name='DRAW_WRAP_DISPATCH',
        switch='SOTC_DRAW_CALLERS',
        group='draw_wrap_dispatch',
        mode='entry',
        # BPGnm_DispatchDirect: push rbp; mov rbp,rsp; push r15..rbx; push rax; mov rbx,rdi; mov r13d,r8d; mov r14d,ecx
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 50 48 89 fb 45 89 c5 41 89 ce',
        expected={EU_FINGERPRINT: 0x86b9e0},
    ),
]
