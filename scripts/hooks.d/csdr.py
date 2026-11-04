"""Resource decompress probe (gpu/sotc/probe_csdr.cpp). Observe only.

RES_DECOMPRESS_DONE  site in the resource loader 0x147160, right after its call of the decompress wrapper 0x1aa2a0
                     (which calls OodleLZ_Decompress 0x89d3b0, zlib or a copy by the descriptor's type). The wrapper
                     itself cannot be wrapped: its type switch is an indirect jump. At the site: rax = bytes produced,
                     r14 = the raw buffer, r12 = raw size, r15 = the request (+0x80 compressed size, +0x90 compressed
                     data, +0x98 name object, +0xf0 the u32 descriptor type << 29 | raw size). Switch SOTC_CSDR_PROBE.
RES_LOADED           entry at 0x4d470, the memory-stream constructor the loader 0x147160 hands every resource to
                     (call at 0x1472ae, after the decompressed and the stored paths meet; 6 callers in all):
                     rdx = the resource data, rcx = its size. Stored (uncompressed) files pass here too: 7,536 of the
                     game's 17,196 shader files are stored. A site at the meeting point 0x147295 is refused (the
                     loader holds other CPU patches). Own group, so a refusal does not take RES_DECOMPRESS_DONE down.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='RES_DECOMPRESS_DONE',
        switch='SOTC_CSDR_PROBE',
        group='csdr_probe',
        mode='site',
        # mov r13,rax; mov al,1; jmp short (after call 0x1aa2a0 at 0x147257; not a branch target)
        pattern='49 89 c5 b0 01 eb',
        expected={EU_FINGERPRINT: 0x14725c},
    ),
    dict(
        name='RES_LOADED',
        # sotc.py sets it for SOTC_CSDR_PROBE, SOTC_PREDICT_PROBE, SOTC_PRETRANSLATE and SOTC_PRECOMPILE.
        switch='SOTC_RES_LOADED',
        group='csdr_loaded',
        mode='entry',
        # push rbp; mov rbp,rsp; push rbx; push rax; mov rbx,rdi; call 0x8db10; lea rax,[rip+..]; add rax,0x10;
        # mov [rbx],rax
        pattern='55 48 89 e5 53 50 48 89 fb e8 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 83 c0 10 48 89 03',
        expected={EU_FINGERPRINT: 0x4d470},
    ),
]
