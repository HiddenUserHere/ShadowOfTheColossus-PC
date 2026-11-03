# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hooks of the draw-op probe (docs/re-backend/14-draw-reduction.md, R4 and R6). Observe only.

OPS_DEPTH_WALK  FUN_000c3a50 (0xc3a50), entry. The "Depth" object walk (list r8: 37, 39 in MainCamera).
OPS_ALPHA_WALK  FUN_000c3c70 (0xc3c70), entry (pre). The "Alpha" object walk, same prologue. It is an entry hook
                and not a wrap: the walk reads its stack argument through rbp (0xc3e12), which Wrap refuses.
OPS_ALPHA_DONE  site 0xc3e48 in FUN_000c3c70, the instruction after the list walk call. It completes the record of
                OPS_ALPHA_WALK on the same thread. Same group as OPS_ALPHA_WALK (a group is all or nothing).
OPS_MAINCAM     FUN_000caed0 (RenderView::MainCamera), entry. Reads the gates of its walk calls per call.
OPS_FOG_<addr>  the family A wrappers of the fog pass, FUN_000a7310, a7470, a75b0, a78a0, a7eb0, a83f0.
                Each is called only from sub_a5e80 (scope "Fog"), one or two sites each (eboot_re callers).

Switch SOTC_OPS_PROBE (gpu/sotc/probe_ops.cpp). The 0xc3a50 and 0xc3c70 prologues are identical except for
a RIP displacement, so their patterns are not unique; the EU 1.01 expected offsets resolve them. The site and
MainCamera patterns come from eboot_re sig (unique in the text segment). The fog patterns are unique (eboot_re
sig, 32 bytes). Any other eboot is scanned and these hooks stay off when a pattern is not unique.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

_WALK_PATTERN = (
    '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 78 48 8b 05 ?? ?? ?? ?? 48 89 d3 89 c9 49 89 f7 '
    '48 8d 75 80 4d 89 cd 45 89 c6 48 8d 0c 89 48 c1 e1 04 48 8b 00 48 89 45 d0 48 8b 43 18 4c 8b 23'
)

# 0xc3e48: mov rsi,[rbp-0x98]; lea rdi,[rbp-0xa0]; call 0x16bc50 (FUN_0016bc50, scope close); lea rdi,[rbp-0x90];
# call Profiler::PopScope; the stack check and the epilogue (sig of eboot_re, unique in the text segment)
_ALPHA_DONE_PATTERN = (
    '48 8b b5 68 ff ff ff 48 8d bd 60 ff ff ff e8 ?? ?? ?? ?? 48 8d bd 70 ff ff ff e8 ?? ?? ?? ?? 48 8b 05 '
    '?? ?? ?? ?? 48 8b 00 48 3b 45 d0 75 ?? 48 83 c4 78 5b 41 5c 41 5d 41 5e 41 5f 5d c3 e8 ?? ?? ?? ?? 90 '
    '90 90 90 90'
)

# 0xcaed0: push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x4d8; mov rax,[rip+..]; ...
_MAINCAM_PATTERN = '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 04 00 00 48 8b 05 ?? ?? ?? ?? 48'

HOOKS = [
    dict(
        name='OPS_DEPTH_WALK',
        switch='SOTC_OPS_PROBE',
        group='ops_depth',
        mode='entry',
        pattern=_WALK_PATTERN,
        expected={EU_FINGERPRINT: 0xc3a50},
    ),
    dict(
        name='OPS_ALPHA_WALK',
        switch='SOTC_OPS_PROBE',
        group='ops_alpha',
        mode='entry',
        pattern=_WALK_PATTERN,
        expected={EU_FINGERPRINT: 0xc3c70},
    ),
    dict(
        name='OPS_ALPHA_DONE',
        switch='SOTC_OPS_PROBE',
        group='ops_alpha',
        mode='site',
        pattern=_ALPHA_DONE_PATTERN,
        expected={EU_FINGERPRINT: 0xc3e48},
    ),
    dict(
        name='OPS_MAINCAM',
        switch='SOTC_OPS_PROBE',
        group='ops_maincam',
        mode='entry',
        pattern=_MAINCAM_PATTERN,
        expected={EU_FINGERPRINT: 0xcaed0},
    ),
    dict(
        name='OPS_FOG_A7310',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 02 00 00 48 8b 05 ?? ?? ?? ?? 49 89 ce 48 8d',
        expected={EU_FINGERPRINT: 0xa7310},
    ),
    dict(
        name='OPS_FOG_A7470',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 18 01 00 00 48 8b 05 ?? ?? ?? ?? 49 89 cd 48 8d',
        expected={EU_FINGERPRINT: 0xa7470},
    ),
    dict(
        name='OPS_FOG_A75B0',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ?? 89 4d ac 48 89 55 a0 48',
        expected={EU_FINGERPRINT: 0xa75b0},
    ),
    dict(
        name='OPS_FOG_A78A0',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 8b 05 ?? ?? ?? ?? 44 89 85 20 ff',
        expected={EU_FINGERPRINT: 0xa78a0},
    ),
    dict(
        name='OPS_FOG_A7EB0',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 00 00 00 48 8b 05 ?? ?? ?? ?? 4c 89 45 a8 48',
        expected={EU_FINGERPRINT: 0xa7eb0},
    ),
    dict(
        name='OPS_FOG_A83F0',
        switch='SOTC_OPS_PROBE',
        group='ops_fog',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ?? 48 89 4d a0 49 89 f4 48',
        expected={EU_FINGERPRINT: 0xa83f0},
    ),
]
