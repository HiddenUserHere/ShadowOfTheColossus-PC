"""PC pool verification model (gpu/sotc/pc_pool.cpp, gpu/sotc/pc_pool.h, docs/re-stutters/PC-MEMORY-REWRITE.md).

PC_POOL_ALLOC  wrap at pool_alloc_block_wrapper (0x19ae0), the game's block allocator in front of
               pool_alloc_region (0x19e00). It serves mem_allocator_alloc (0x479a0, ALLOC_BLOCK) and
               sub_55fd30. Entry arguments: rdi = owner (not modelled), rsi = size, edx = align,
               ecx = kind. The wrapper always calls 0x19e00 with va_hint = 0, node = 0, mode = 0.
               The pre phase snapshots the three pool sets (taking the game's pool lock around the read).
               The post phase reads rax (the node) and the state in memory, predicts the result from the
               pre snapshot (a fresh block's VA is only known there), compares, and never changes guest
               registers or memory.
               Why this site and not 0x19e00: 0x19e00 has an indirect branch that the detour engine
               cannot resolve (it refuses the whole function, every mode). 0x19ae0 has no indirect
               branch, no `and rsp`, no stack argument read (the Wrap refusals of cpu_patches.cpp pass)
               and it is the only entry for the block path.
               Not covered: pool_node_alloc (0x19660) calls 0x19e00 for its 64 KiB node blocks, and
               pool_alloc_region_aligned (0x1a660) for the family path. The model counts the state changes
               it does not see ("unseen state changes").
               Switch SOTC_PC_POOL=2 (verify; other values are not a mode and do nothing).
               The frees are not hooked here: FREE_BLOCK in alloc.py (0x1b0e0, entry) calls
               SotcPcPool::OnFreeNode from the probe_alloc handler.
PC_POOL_NATIVE entry at the same address (0x19ae0), switch SOTC_PC_POOL_NATIVE=1. It runs the eligible calls
               natively (SotcPcPoolNative::ApplyExactHit under the game's pool lock), performs the wrapper's
               tail and returns the node in rax (HookAction::Return); every other call continues into the
               wrapper. Two hooks at one address cannot both install: with both switches set the loader
               refuses the second one by overlap, and the handlers refuse both (SotcPcPool::Register).
               Its own group, so a refusal of either does not turn the other off.
PC_POOL_SIMPLE  wrap at pool_alloc_region_simple (0x19c60), a direct caller of pool_alloc_region (0x19e00) with the
               wrapper's arguments. Verified like PC_POOL_ALLOC (or announced under the native switch).
PC_POOL_ALIGNED entry at pool_alloc_region_aligned (0x1a660) and PC_POOL_ALIGNED_EXIT site at its normal exit
               (0x1a7db). The entry takes the arguments (kind from the stack), the site runs the post phase. A wrap
               is refused by the engine (the function reads its stack arguments). Called only by mem_allocator_alloc_reuse
               (0x47a10) through sub_8d770, which is in a function-pointer table, so the callers are not static.
PC_POOL_BPE_INIT and PC_POOL_FLEX_INIT: entries at bpe_PlatformPageAllocator_Init (0x18aa0) and pool_init_flexible
               (0x197d0). Boot only. They re-base the verification (no clean base to count an unseen change against)
               and name the source in the native expectation.
Every hook above has a _NATIVE twin under SOTC_PC_POOL_NATIVE at the same address (the twins never install with the
verify hooks: the loader refuses the second by overlap). The source tags are counted in the report (SOTC PC POOL source).
PC_POOL_MAP    wrap at vmem_batch_map (0x1af80), the flush of the pool's 64-entry op queue (root+0x2102f0,
               32-byte entries). Every VA -> phys change goes through it: the handler reads the queued ops in
               the pre phase and applies them to the shadow VA map (PhysMap) in the post phase, once the flush
               has returned. It runs under the game's pool lock. Its own group, so a refusal of this wrap does
               not turn PC_POOL_ALLOC off (and the reverse). No guest argument is read: the queue is in memory.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PC_POOL_ALLOC',
        switch='SOTC_PC_POOL',
        group='pc_pool',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x28;
        # mov rax,[rip+..] (wildcarded); mov [rbp-0x40],edx; mov ebx,ecx
        # (eboot_re.py sig 0x19ae0; unique in the text segment)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ?? 89 55 c0 89',
        expected={EU_FINGERPRINT: 0x19ae0},
    ),
    dict(
        name='PC_POOL_MAP',
        switch='SOTC_PC_POOL',
        group='pc_pool_map',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0xc8;
        # mov rcx,[rip+..] (the TLS-style global, wildcarded). Unique in the text segment
        # (eboot_re.py sig 0x1af80; the four displacement bytes are wildcarded).
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 8b 0d ?? ?? ?? ??',
        expected={EU_FINGERPRINT: 0x1af80},
    ),
    # The same wrap under the native switch: native mode keeps the shadow VA map too (Predict needs it for the
    # reserve-map path). Same address as PC_POOL_MAP, so the two never install together (the loader refuses the
    # second by overlap, and pc_pool.cpp refuses both switches at once).
    dict(
        name='PC_POOL_MAP_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_map_native',
        mode='wrap',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 8b 0d ?? ?? ?? ??',
        expected={EU_FINGERPRINT: 0x1af80},
    ),
    dict(
        name='PC_POOL_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_native',
        mode='entry',
        # The same prologue as PC_POOL_ALLOC (one site, one pattern).
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ?? 89 55 c0 89',
        expected={EU_FINGERPRINT: 0x19ae0},
    ),
    # Direct callers of pool_alloc_region (0x19e00) that bypass the block wrapper. Same handlers as PC_POOL_ALLOC
    # (verify) or a source announcement (native); the source is named in the report. 0x19c60 takes the wrapper's
    # arguments (rsi size, edx align, ecx kind). Nothing calls it statically (no direct or data reference found).
    dict(
        name='PC_POOL_SIMPLE',
        switch='SOTC_PC_POOL',
        group='pc_pool_direct',
        mode='wrap',
        # eboot_re.py sig 0x19c60 (unique): push rbp; mov rbp,rsp; sub rsp,0x10; mov rax,rsi; mov r9d,edx ...
        pattern='55 48 89 e5 48 83 ec 10 48 89 f0 41',
        expected={EU_FINGERPRINT: 0x19c60},
    ),
    dict(
        name='PC_POOL_SIMPLE_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_direct_native',
        mode='wrap',
        pattern='55 48 89 e5 48 83 ec 10 48 89 f0 41',
        expected={EU_FINGERPRINT: 0x19c60},
    ),
    # pool_alloc_region_aligned (0x1a660): called only by mem_allocator_alloc_reuse (0x47a10), reached from sub_8d770,
    # which sits in a function-pointer table (vaddr 0x1597618). Its seventh argument (kind) is read from the stack, so
    # a wrap is refused (Wrap: reads its stack arguments). The pair is an entry (pre snapshot, the arguments) and a site
    # at the normal exit (0x1a7db: mov rax,r14 after the pool lock is released; the node is r14). Both are pre-phase
    # hooks in the engine, so the exit site runs the post phase of the block pair.
    dict(
        name='PC_POOL_ALIGNED',
        switch='SOTC_PC_POOL',
        group='pc_pool_aligned',
        mode='entry',
        # eboot_re.py sig 0x1a660 (unique)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 38 48 8b 05 ?? ?? ?? ?? 44 89 4d bc 44 89 45 c0',
        expected={EU_FINGERPRINT: 0x1a660},
    ),
    dict(
        name='PC_POOL_ALIGNED_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_aligned_native',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 38 48 8b 05 ?? ?? ?? ?? 44 89 4d bc 44 89 45 c0',
        expected={EU_FINGERPRINT: 0x1a660},
    ),
    dict(
        name='PC_POOL_ALIGNED_EXIT',
        switch='SOTC_PC_POOL',
        group='pc_pool_aligned_exit',
        mode='site',
        # eboot_re.py sig 0x1a7db (unique): mov rax,r14; add rsp,0x38; pop rbx ... ret
        pattern='4c 89 f0 48 83 c4 38 5b 41 5c 41 5d 41 5e 41 5f 5d c3 e8 ?? ?? ?? ?? 90 90 90 90 90 90 90 90 90 90 90 90 90 90 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89',
        expected={EU_FINGERPRINT: 0x1a7db},
    ),
    dict(
        name='PC_POOL_ALIGNED_EXIT_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_aligned_exit_native',
        mode='site',
        pattern='4c 89 f0 48 83 c4 38 5b 41 5c 41 5d 41 5e 41 5f 5d c3 e8 ?? ?? ?? ?? 90 90 90 90 90 90 90 90 90 90 90 90 90 90 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89',
        expected={EU_FINGERPRINT: 0x1a7db},
    ),
    # Boot callers. bpe init (0x18aa0, its two pool_node_alloc sites) and pool_init_flexible (0x197d0, called only
    # from bpe init): entry hooks that re-base the verification and name the source. Boot only (cpu_memory_family_init
    # chain), so they fire once per run.
    dict(
        name='PC_POOL_BPE_INIT',
        switch='SOTC_PC_POOL',
        group='pc_pool_boot',
        mode='entry',
        # eboot_re.py sig 0x18aa0 (unique)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 60 01 00 00 48 8b 05 ?? ?? ?? ?? 49 be 00 00 00',
        expected={EU_FINGERPRINT: 0x18aa0},
    ),
    dict(
        name='PC_POOL_BPE_INIT_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_boot_native',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 60 01 00 00 48 8b 05 ?? ?? ?? ?? 49 be 00 00 00',
        expected={EU_FINGERPRINT: 0x18aa0},
    ),
    dict(
        name='PC_POOL_FLEX_INIT',
        switch='SOTC_PC_POOL',
        group='pc_pool_boot_flex',
        mode='entry',
        # eboot_re.py sig 0x197d0 (unique)
        pattern='55 48 89 e5 41 57 41 56 53 50 bb 01',
        expected={EU_FINGERPRINT: 0x197d0},
    ),
    dict(
        name='PC_POOL_FLEX_INIT_NATIVE',
        switch='SOTC_PC_POOL_NATIVE',
        group='pc_pool_boot_flex_native',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 53 50 bb 01',
        expected={EU_FINGERPRINT: 0x197d0},
    ),
]
