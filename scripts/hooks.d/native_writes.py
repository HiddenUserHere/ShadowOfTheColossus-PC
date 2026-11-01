# SPDX-License-Identifier: GPL-2.0-or-later
"""PC-native write announces (docs/re-stutters/native/PLAN.md, N2; handlers in gpu/sotc/native/native_hooks.cpp).

Every hook here is a Site hook (one instruction inside a function, registers editable) placed right after
the engine finished a write, or right before a release. Switch SOTC_NATIVE (any non-zero value), one group
per write path so a refused hook takes down only its own path.

NATIVE_RES_COPY       0x8bf50, after the initial-data memcpy of the resource buffer wrapper 0x8be60 (the
                      non-zero-copy path: the 0x800 path never reaches this site). Announce Persistent.
NATIVE_TEX_COPY       0x96b59, after the single-block texel copy of the texture build 0x96680 (texture and
                      particle texture). Announce Persistent.
NATIVE_TEX_TAIL_COPY  0x96b1b, after the mip-tail texel copy of 0x96680. Announce Persistent.
NATIVE_MIP_COPY       0x98d6c, after the memcpy of a streamed mip into the pool in 0x98ae0. Announce Persistent.
NATIVE_MIP_RELEASE    0x98be0, before the mode-2 decommit (0x12950) of a dropped mip in 0x98ae0. Release.
                      The site is the 'mov edx,2' before the call at 0x98bf0, which DECOMMIT_BEGIN owns.

Each pattern is the signature from tools/re/eboot_re.py sig (unique in the text segment), checked with
sigfind on the EU 1.01 eboot. The expected offsets apply to that eboot only; any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='NATIVE_RES_COPY',
        switch='SOTC_NATIVE',
        group='native_res',
        mode='site',
        # mov ecx,[rbp-0x34]; mov r9d,[rbp-0x30]; mov r10d,[rbp-0x2c]; ...: after the memcpy call in 0x8be60
        pattern='8b 4d cc 44 8b 4d d0 44 8b 55 d4 4d 89 fe 48 8b',
        expected={EU_FINGERPRINT: 0x8bf50},
    ),
    dict(
        name='NATIVE_TEX_COPY',
        switch='SOTC_NATIVE',
        group='native_tex',
        mode='site',
        # add [r15+0x1b0],ebx: after the single-block texel copy in 0x96680
        pattern='41 01 9f b0 01 00 00 4d',
        expected={EU_FINGERPRINT: 0x96b59},
    ),
    dict(
        name='NATIVE_TEX_TAIL_COPY',
        switch='SOTC_NATIVE',
        group='native_tex',
        mode='site',
        # add [r15+0x1b0],r12d: after the mip-tail texel copy in 0x96680
        pattern='45 01 a7 b0 01 00 00 eb',
        expected={EU_FINGERPRINT: 0x96b1b},
    ),
    dict(
        name='NATIVE_MIP_COPY',
        switch='SOTC_NATIVE',
        group='native_mip',
        mode='site',
        # mov eax,[rbp-0x2c]; mov [r12+0xc0],eax: after the mip memcpy in 0x98ae0
        pattern='8b 45 d4 41 89 84 24 c0',
        expected={EU_FINGERPRINT: 0x98d6c},
    ),
    dict(
        name='NATIVE_MIP_RELEASE',
        switch='SOTC_NATIVE',
        group='native_mip',
        mode='site',
        # mov edx,2; mov r8d,0x100: the mode-2 decommit call in 0x98ae0 follows at 0x98bf0
        pattern='ba 02 00 00 00 41 b8 00',
        expected={EU_FINGERPRINT: 0x98be0},
    ),
]
