"""PC memory model, per-frame upload ring (gpu/sotc/pc_ring.cpp, docs/re-stutters/PC-MEMORY-REWRITE.md).

Three sites for the same declare. Each is its own hook and its own group, so each is installed by
its own switch:

PC_RING_HEAP   (default, switch SOTC_PC_RING) wrap of sub_1d750, the allocator slot +0xa0 of the
               RenderSingleFrameGPU heap (the arena heap of memtype DAT_02ed1f54). The memtype
               dispatcher 0x12820 calls that slot for every allocation of the heap; the heap's
               chunks come from the kind-2 "Default TaggedArena" blocks. Pre saves rsi (size), post
               reads rax (pointer). Its class vtable is shared with the other render heaps, so
               calls of those are counted too; only arena pointers are declared (mode 1) or
               recorded as extents (modes 3 and 4, SOTC_PC_RING=3 or 4: no declare, no protection
               change). Any non-zero SOTC_PC_RING installs this hook.
PC_RING_BUMP   (comparison only, switch SOTC_PC_RING_BUMP) entry of the frame bump allocator
               sub_48440 (rdi = allocator object, rsi = size, edx = align; object +0x98 cursor,
               +0x88 chunk base, +0xa8 chunk end). Not on the RenderSingleFrameGPU path: the arena
               allocations did not land in its chunk (0 arena allocs/s in the first run).
PC_RING_ALLOC  (comparison only, switch SOTC_PC_RING_DISPATCH) wrap at the generic allocator
               0x12820 (size rdi, memtype slot esi, align edx, flags ecx; returns the pointer in rax).
               Same declare, but it runs for every allocation of every family.

Switches are read as integers. SOTC_PC_RING_DISPATCH=1 with SOTC_PC_RING also set installs the
dispatch wrap next to the heap wrap (both declare; the comparison counts both). Neither hook changes
guest registers or memory. All sites are off unless their switch is set. PC_RING_ALLOC uses the
same wrap site as MEMTYPE_ALLOC (SOTC_HOOK_MEMTYPE_PROBE): do not turn that probe on together with
SOTC_PC_RING_DISPATCH.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PC_RING_HEAP',
        switch='SOTC_PC_RING',
        group='pc_ring_heap',
        mode='wrap',
        # sub_1d750: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx;
        # sub rsp,0x18; mov r14d,edx; mov rbx,rsi; mov r15,rdi. Unique (sigfind), no rip-relative
        # bytes, no indirect branch, no branch target in the span, no rsp operand at depth >= 0.
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 41 89 d6 48 89 f3 49',
        expected={EU_FINGERPRINT: 0x1d750},
    ),
    dict(
        name='PC_RING_BUMP',
        switch='SOTC_PC_RING_BUMP',
        group='pc_ring_bump',
        mode='entry',
        # sub_48440: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx;
        # push rax; mov r12,rdi; mov r14d,edx; mov rbx,rsi; ... (no rip-relative bytes, unique)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 50 49 89 fc 41 89 d6 48 89 f3 4d',
        expected={EU_FINGERPRINT: 0x48440},
    ),
    dict(
        name='PC_RING_ALLOC',
        switch='SOTC_PC_RING_DISPATCH',
        group='pc_ring_dispatch',
        mode='wrap',
        # The pattern of MEMTYPE_ALLOC (scripts/hooks.d/memtype.py).
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 4c 8b 35 ?? ?? ?? ?? 41 89 cf 41',
        expected={EU_FINGERPRINT: 0x12820},
    ),
]
