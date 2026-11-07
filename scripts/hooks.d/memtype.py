"""Memtype probes (gpu/sotc/probe_memtype.cpp, PC memory model wave B; docs/re-stutters/
PC-MEMORY-REWRITE.md section 5, reports3/04-alloc-scopes.md). They only observe.

Switch SOTC_HOOK_MEMTYPE_PROBE; one group, so every hook here resolves or none does.

MEMTYPE_ALLOC       wrap at the generic allocator 0x12820(size, memtype, align, flags).
                    rdi = size, esi = memtype slot id (u32), [rsp] = return address (caller).
                    Post phase (rax = returned pointer) only with SOTC_MEMTYPE_CLASSES=1: the
                    64 KiB granules of each slot are registered as owner blocks for
                    SOTC_CLASS_TRACK=2 (gpu/sotc/probe_memtype.cpp, "granule claims"). Same hook,
                    same switch, same group: the flag is read by the probe, not here.
MEMTYPE_FREE        entry at the generic free 0x12af0 (rdi = the pointer 0x12820 returned, 0 = no-op). Every
                    release of a 0x12820 block ends here: 0x170b0, the XPR area blocks (sub_90050) and the
                    deferred texture deletes (DeletePendingItems). SOTC_ALLOC_SITES ends the allocation there.
MEMTYPE_FREE_SMALL, MEMTYPE_FREE_BPE  entries at the heap frees reached through vtbl+0xa8: the small-object
                    allocator 0x17e90 and the BPE heap 0x493d0 (rdi = heap, rsi = block). They also see the frees
                    that skip 0x12af0 (direct vtable calls, the realloc).
MEMTYPE_FREE_FAMILY entry at the family free 0x47f30 (the GPUMemory / CPUMemory heaps' vtbl+0xa8, rsi = block). With
                    it, SOTC_GPU_PLACE can place sites of those heaps (slots 1 and 5).
MEMTYPE_CLASSIFY    entry at the pointer classifier 0x19a50(ptr) (pool class tables -> heap object). SOTC_GPU_PLACE
                    answers it for GPU-region pointers with the heap the block's site asked for (callers 0x12af0,
                    0x12c50, 0x47840, 0x47c20, 0x47f30).
MEMTYPE_REALLOC     wrap at the realloc 0x12c50(ptr, size, align, flags): it allocates through vtbl+0xa0 and frees
                    the old block through vtbl+0xa8; post rax = the block. The four free/realloc hooks only feed
                    SOTC_ALLOC_SITES and form their own group (alloc_sites_free), so a refused one leaves the
                    memtype probe on.
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
        name='MEMTYPE_FREE',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='alloc_sites_free',
        mode='entry',
        # the generic free 0x12af0(ptr, esi): push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx;
        # sub rsp,0x30; mov r15,[rip+..] (the stack guard, wildcarded); mov r14,rdi; test r14,r14
        pattern='55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 30 4c 8b 3d ?? ?? ?? ?? 49 89 fe 4d 85 f6',
        expected={EU_FINGERPRINT: 0x12af0},
    ),
    dict(
        name='MEMTYPE_FREE_SMALL',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='alloc_sites_free',
        mode='entry',
        # small-object free 0x17e90(heap, ptr): push rbp..rbx; sub rsp,0x18; mov [rbp-0x30],rdi; mov r15,rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89 7d d0 49 89 f7',
        expected={EU_FINGERPRINT: 0x17e90},
    ),
    dict(
        name='MEMTYPE_FREE_BPE',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='alloc_sites_free',
        mode='entry',
        # BPE heap free 0x493d0(heap, ptr): push rbp..rbx; sub rsp,0x18; mov rax,[rip+..] (stack guard); mov r14,rdi;
        # mov r15,rsi; mov rax,[rax]; mov [rbp-0x30],rax; mov r13,[r14+..]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 8b 05 ?? ?? ?? ?? 49 89 fe 49 89 f7 48 8b 00 48 89 45 d0 4d 8b 6e',
        expected={EU_FINGERPRINT: 0x493d0},
    ),
    dict(
        name='MEMTYPE_FREE_FAMILY',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='gpu_place_family',  # own group: the function ends in an indirect jmp; a refusal keeps the rest on
        mode='entry',
        # family free 0x47f30(family, ptr, ctx) (vtbl+0xa8 of the GPUMemory / CPUMemory family heaps): push rbp;
        # mov rbp,rsp; push r14; push rbx; mov rbx,rsi; mov r14,rdx; mov rdi,rbx; then owner = 0x19a50(ptr) and
        # jmp [owner+0xa8]
        pattern='55 48 89 e5 41 56 53 48 89 f3 49 89 d6 48 89 df',
        expected={EU_FINGERPRINT: 0x47f30},
    ),
    dict(
        name='MEMTYPE_CLASSIFY',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='alloc_sites_free',
        mode='entry',
        # pointer classifier 0x19a50(ptr) -> owning heap object or 0: push rbp; mov rbp,rsp; push rbx;
        # sub rsp,0x18; mov rbx,[rip+..] (stack guard); mov rax,[rbx]; mov [rbp-0x10],rax; mov rax,[rip+..]; mov ..
        pattern='55 48 89 e5 53 48 83 ec 18 48 8b 1d ?? ?? ?? ?? 48 8b 03 48 89 45 f0 48 8b 05 ?? ?? ?? ?? 48 8b',
        expected={EU_FINGERPRINT: 0x19a50},
    ),
    dict(
        name='MEMTYPE_REALLOC',
        switch='SOTC_HOOK_MEMTYPE_PROBE',
        group='alloc_sites_free',
        mode='wrap',
        # realloc 0x12c50(ptr, size, align, flags): push rbp..rbx; sub rsp,0x68; mov rax,[rip+..]; mov [rbp-0x80],rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 48 89 75 80',
        expected={EU_FINGERPRINT: 0x12c50},
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
