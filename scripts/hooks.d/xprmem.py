"""XPR memory probe hooks (docs/re-stutters/PC-MEMORY-REWRITE.md sections 4 and 5). Observe only.

    XPRMEM_BUILD_MASKS       0x19b760 BPXPR_BuildSetDraws; wrap. Pre: rdi = the mask owner, rsi = the
                             set. The four GPU mask pointers are written at table + index * 0xa8.
    XPRMEM_PRERENDER_BLOCKS  0xca150 (sub_ca150, called by XPRPreRender 0xca6d0); wrap. rdi = the frame
                             object. The per-frame table goes to frame+0x38; the four GPU block
                             pointers come from the global object at VA 0x2ed33f0 (+0xa80..+0xa98).

Both use the switch SOTC_HOOK_XPRMEM_PROBE (scripts/sotc_hooks.py), group xprmem_probe. Patterns are
the signatures of tools/re/eboot_re.py sig, checked with sigfind on the EU 1.01 eboot. Handlers:
gpu/sotc/probe_xprmem.cpp.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='XPRMEM_BUILD_MASKS',
        switch='SOTC_HOOK_XPRMEM_PROBE',
        group='xprmem_probe',
        mode='entry',  # wrap refused: the function realigns its stack (and rsp,-32)
        # push rbp; mov rbp,rsp; push r15..rbx; and rsp,-32; sub rsp,0x5e0; mov r14,rdx; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec e0 05 00 00 49 89 d6 48',
        expected={EU_FINGERPRINT: 0x19b760},
    ),
    dict(
        name='XPRMEM_PRERENDER_BLOCKS',
        switch='SOTC_HOOK_XPRMEM_PROBE',
        group='xprmem_blocks',  # own group: one refused hook must not turn the other off
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15,r14,r12,rbx; mov r14,rdi; call (wildcard); lea (wildcard)
        pattern='55 48 89 e5 41 57 41 56 41 54 53 49 89 fe e8 ?? ?? ?? ?? 48 8d 05 ?? ??',
        expected={EU_FINGERPRINT: 0xca150},
    ),
]
