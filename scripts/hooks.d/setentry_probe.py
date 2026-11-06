# SPDX-License-Identifier: GPL-2.0-or-later
"""Diagnostic: the entries of BPXPR_BuildSetEntry (0x19b400), switch SOTC_SETENTRY_PROBE.

Every call is counted per 5 s with its owner (rdi), whether the owner's set record (owner+0x18) is null (the
function's first gate: it returns at once, 0x19b42f -> 0x19b738) and the set's model count (u16 at set+0x64).
Comparing two sessions (e.g. with and without the opening cinematic) shows whether a set never reaches the call
(BPXPR_BuildPreShaderSlots 0xc6810 filtered it) or reaches it with no set record. Handler in
gpu/sotc/native/cpu_cull_lod.cpp (OnSetEntry).

0x19b400: push rbp / mov rbp,rsp / push r15 / push r14 / push r13 (8 displaced bytes); the 24-byte pattern is
unique (tools/re/eboot_re.py sig 0x19b400). No branch targets the entry bytes.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='SETENTRY_PROBE',
        switch='SOTC_SETENTRY_PROBE',
        group='setentry_probe',
        mode='entry',
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 78 01 00 00 48 8b 0d ??',
        expected={EU_FINGERPRINT: 0x19b400},
    ),
]
