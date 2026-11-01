"""Memtype probes (gpu/sotc/probe_memtype.cpp, PC memory model wave B; docs/re-stutters/
PC-MEMORY-REWRITE.md section 5, reports3/04-alloc-scopes.md). They only observe.

Switch SOTC_HOOK_MEMTYPE_PROBE; one group, so every hook here resolves or none does.

MEMTYPE_ALLOC       wrap at the generic allocator 0x12820(size, memtype, align, flags).
                    rdi = size, esi = memtype slot id (u32), [rsp] = return address (caller).
                    Post phase (rax = returned pointer) only with SOTC_MEMTYPE_CLASSES=1: the
                    64 KiB granules of each slot are registered as owner blocks for
                    SOTC_CLASS_TRACK=2 (gpu/sotc/probe_memtype.cpp, "granule claims"). Same hook,
                    same switch, same group: the flag is read by the probe, not here.
RENDER_CTX_REQUEST  entry at PerFrameRenderContexts::RequestNewRenderContext (0x90470).
                    rsi = DCB size (0 selects the 0x500000 default), rdx = CCB size, rcx = this.
RING_FLIP           entry at the ring flip (0x136f0). rdi = address of a heap-id global; [rdi] is
                    that heap's memtype slot id (1 = scope, the flip returns at once).
MEMTYPE_FLIP_SITE   entry at sub_8c860 (the RenderBackend_Frame flip pair, the frame boundary).
                    Own switch SOTC_HOOK_FRAME_FLIP (group frame_flip): shared by this probe and
                    SOTC_PC_RING. Derived
                    rip_target fields: single_frame_gpu (0x2ed1f54) and single_frame_cpu (0x2ed1f58).

The pattern of MEMTYPE_FLIP_SITE has no wildcards: with the rip displacements wildcarded it matches
about 70 small wrapper functions, so the exact bytes are the only unique signature. The other
globals (0x2ed1f48..1f5c, 0x2dd9f80..8c) are not referenced by any hooked function and fall back to
the fingerprint-keyed offsets in probe_memtype.cpp.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='MEMTYPE_ALLOC',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='memtype_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x28;
        # mov r14,[rip+..] (wildcarded); mov r15d,ecx; mov r13d,edx; mov ebx,esi; mov r12,rdi; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 4c 8b 35 ?? ?? ?? ?? 41 89 cf 41',
        expected={EU_FINGERPRINT: 0x12820},
    ),
    dict(
        name='RENDER_CTX_REQUEST',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='memtype_probe',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x48; mov rax,[rip+..] (wildcarded);
        # mov [rbp-0x60],rdi; test rsi,rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ?? 48 89 7d a0 48 85 f6 41',
        expected={EU_FINGERPRINT: 0x90470},
    ),
    dict(
        name='MEMTYPE_FLIP_SITE',
        switch='SOTC_HOOK_FRAME_FLIP',  # shared frame boundary: memtype probe and SOTC_PC_RING
        group='frame_flip',
        # Entry, not a hook on the ring flip 0x136f0 itself: that function has an indirect branch
        # the detour cannot relocate (first live run, 1 Nov), which took the whole group down.
        mode='entry',
        # sub_8c860: push rbp; mov rbp,rsp; lea rdi,[rip+0x2ed1f54]; call 0x136f0;
        # lea rdi,[rip+0x2ed1f58]; pop rbp; jmp 0x136f0. Exact bytes (see the docstring).
        pattern='55 48 89 e5 48 8d 3d e9 56 e4 02 e8 80 6e f8 ff 48 8d 3d e1 56 e4 02 5d e9 73 6e f8 ff',
        expected={EU_FINGERPRINT: 0x8c860},
        derived=[
            # lea rdi,[rip+disp] at pattern offset 4: instruction ends at 11
            dict(field='single_frame_gpu', offset=7, size=4, kind='rip_target', insn_end=11),
            # the second lea at pattern offset 16: instruction ends at 23
            dict(field='single_frame_cpu', offset=19, size=4, kind='rip_target', insn_end=23),
        ],
    ),
]
