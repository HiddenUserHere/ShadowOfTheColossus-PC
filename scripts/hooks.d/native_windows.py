"""PC-native allocator windows (SOTC_NATIVE, docs/re-stutters/native/impl-windows.md, gpu/sotc/native/native_windows_hooks.cpp).

The windows use these two hooks. The other window calls ride on hooks that already exist and need no record here:
  - 0x479a0 (ALLOC_BLOCK, scripts/hooks.d/alloc.py): its pre phase binds the render-target window to the RTAllocator
    block and serves a parked node (SkipOriginal, a wrap). Needs SOTC_HOOK_ALLOC_PROBE.
  - 0x12820 (MEMTYPE_ALLOC, scripts/hooks.d/memtype.py): its pre and post phases set and clear the heap key of window B.
    Needs SOTC_HOOK_MEMTYPE_PROBE.
  - 0x19ae0 (PC_POOL_NATIVE, scripts/hooks.d/pcpool.py): routes the fresh block of a window. Needs SOTC_PC_POOL_NATIVE.

NATIVE_WINDOW_RELEASE  entry at 0x476f0 (release: rdi = owner, rsi = node). When the node is the live block of a window,
                       the handler does the function's stores (WindowRelease) and returns (HookAction::Return), so the
                       VA return 0x1b0e0 does not run. Otherwise the original runs.
NATIVE_WINDOW_SETUP    site at 0x1171f in memory_system_init (0x110a0), after the kind-2 arena store at 0x11718 (the
                       arena pointer is in 0x2dfb2b8). The handler reserves and binds windows A and B once per session.
                       The site is a branch target from 0x115cd too (the arena already existed), so it runs on both paths.

Switch SOTC_NATIVE: any non-zero value installs both hooks. Each one is its own group, so a refused pattern turns off only
that hook. Both handlers are inert unless the window path is on (pc_pool.cpp WindowsOn).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='NATIVE_WINDOW_RELEASE',
        switch='SOTC_NATIVE',
        group='native_windows_release',
        mode='entry',
        # push rbp; mov rbp,rsp; push r14; push rbx; mov r14,[rsi+0x50] (unique; eboot_re.py sig 0x476f0)
        pattern='55 48 89 e5 41 56 53 4c 8b 76 50 48',
        expected={EU_FINGERPRINT: 0x476f0},
    ),
    dict(
        name='NATIVE_WINDOW_SETUP',
        switch='SOTC_NATIVE',
        group='native_windows_setup',
        mode='site',
        # mov edi,0x2e0; call sub_17070 (the GameSingleFrame ring), right after the kind-2 arena store (unique;
        # eboot_re.py sig 0x1171f)
        pattern='bf e0 02 00 00 e8 ?? ??',
        expected={EU_FINGERPRINT: 0x1171f},
    ),
]
