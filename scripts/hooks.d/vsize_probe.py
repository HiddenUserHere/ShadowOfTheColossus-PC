# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hook of the unbounded-V# probe (SOTC_VSIZE_PROBE, gpu/sotc/probe_vsize.cpp). Observe only.

VSIZE_VIEW_SITE  site 0x8c2e2 in FUN_0008c280 (0x8c280, the view creator of the buffer object layout: obj+0x10 base,
                 +0x18 and +0x1c fields, +0x20 size bytes). The site is after the store of obj+0x20 and before the
                 displaced loads; rdi is still the object. The function leaves by tail jumps (no ret), so this is
                 the point where its size is known. The base (obj+0x10) is written later by the tail callee and is
                 read at lookup time, not here.

The creator 0x8be60 is NOT declared here: NATIVE_ZC_CREATE (native_writes.py, switch SOTC_NATIVE) already detours
its entry, and a second detour in the same function is refused. The probe's 0x8be60 logic runs inside that handler
(native_hooks.cpp ZeroCopyCreate calls SotcVsizeProbe::NoteBpeEntry), so it needs SOTC_NATIVE=1 as well.

Switch SOTC_VSIZE_PROBE. The pattern is 14 bytes of 0x8c2e2 (movl 0x4(%r9),%edx; movl (%r9),%ebx; andq $0xfff,%rdx).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='VSIZE_VIEW_SITE',
        switch='SOTC_VSIZE_PROBE',
        group='vsize_view',
        mode='site',
        pattern='41 8b 51 04 41 8b 19 48 81 e2 ff 0f 00 00',
        expected={EU_FINGERPRINT: 0x8c2e2},
    ),
]
