"""PC-native sync (SOTC_NATIVE, docs/re-stutters/native/sync.md section 6, gpu/sotc/native/native_sync.cpp).

NATIVE_FRAME_SLOT_SPIN   site at 0x90f47, the first compare of RenderBackend_Frame's frame-slot spin:
                         `cmp rdx, [r13 + rcx*8 + 0x780]` (49 3b 94 cd 80 07 00 00). The handler waits
                         on the frame ticket the slot needs (FrameWait(closed - 1)) and makes the compare
                         equal, so the spin loop at 0x90f60..0x90f74 is not entered. The EOP label writes
                         stay as they are. Switch SOTC_NATIVE: any non-zero value installs it (0 = today).

Not hooked here (see the report of the sync specialist): the compute drain 0x85d690 / 0x85d6f0 (dormant),
the ring-full retry loops of the gnmx submit (handled by SOTC_ASC_RPTR=2 under SOTC_NATIVE), and the
second frame label spin 0x91090 (writer not known yet).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='NATIVE_FRAME_SLOT_SPIN',
        switch='SOTC_NATIVE',
        group='native_sync',
        mode='site',
        # cmp rdx, qword ptr [r13 + rcx*8 + 0x780] (REX.W+B, SIB index rcx scale 8, disp32 0x780)
        pattern='49 3b 94 cd 80 07 00 00',
        expected={EU_FINGERPRINT: 0x90f47},
    ),
]
