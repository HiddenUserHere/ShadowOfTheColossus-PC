# SPDX-License-Identifier: GPL-2.0-or-later
"""Depth pre-pass probe (docs/re-backend/04-depth-prepass.md section 9). Observe only.

PREPASS_RESOLVE_A  ResolveSunShadows, FUN_000afcc0 (0xafcc0), entry. rsi = pass. T_in =
                   *(*(pass+0x18)+0x2110), T# at T_in+0xc4. Its twin is PREPASS_RESOLVE_B.
PREPASS_RESOLVE_B  the twin, FUN_000b0100 (0xb0100), entry. Same reads as A.
PREPASS_JOB        RenderDepth job body, FUN_000c1860 (0xc1860), wrap. Sets the pre-pass tag for its calls.
PREPASS_LIST_PASS  object pass, FUN_0011d3d0 (0x11d3d0), entry. rsi = container, ecx = list index.
PREPASS_XPR_MASKED BPXPR_RecordMaskedDraws, FUN_0011d7a0 (0x11d7a0), entry. rcx = mask id, rdx = view,
                   rsi = batch vector. Stack realigned (and rsp,-32), so no wrap. Read-only scan of the
                   visibility bits 0x1a6d20 would draw (report 11, section 1.5).
PREPASS_GBUFFER_VERT  RenderGBufferBakedPerVertex, FUN_000c1ce0 (0xc1ce0), wrap. G-buffer tag.
                   The 32-byte prologue of 0xc1ce0 is shared with 0xc4a50, so 72 bytes are used.
PREPASS_GBUFFER_PROBE RenderGBufferBakedPerProbe, FUN_000c1fc0 (0xc1fc0), wrap. G-buffer tag.
PREPASS_GRASS      RenderVirtualGeometry draw, FUN_000db700 (0xdb700), entry. Call count per phase.
PREPASS_PARTICLES  ParticleRenderer thunk, FUN_00010aee0 (0x10aee0), entry. Call count per phase.

Switch SOTC_PREPASS_PROBE (gpu/sotc/probe_prepass.cpp). The patterns are the prologues, checked with
tools/re/eboot_re.py sigfind on the EU 1.01 eboot (one match each). The expected offsets apply to that
eboot only; any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

_TAGS = ['PREPASS_JOB', 'PREPASS_GBUFFER_VERT', 'PREPASS_GBUFFER_PROBE']

HOOKS = [
    dict(
        name='PREPASS_RESOLVE_A',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_resolve_a',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x98; mov rax,[rip+..]; mov [rbp-0xa0],rdi; mov r15,rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 bd 60 ff ff ff 49 89 f7 48 8d 1d',
        expected={EU_FINGERPRINT: 0xafcc0},
    ),
    dict(
        name='PREPASS_RESOLVE_B',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_resolve_b',
        mode='entry',
        # the same prologue as A; mov r14,rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 bd 60 ff ff ff 49 89 f6 4c 8d 3d',
        expected={EU_FINGERPRINT: 0xb0100},
    ),
    dict(
        name='PREPASS_JOB',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_job',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x68; mov rax,[rip+..]; mov rbx,rdi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 8b 00 48 89 45 d0 4c 8b 73 60 4c 8b',
        expected={EU_FINGERPRINT: 0xc1860},
    ),
    dict(
        name='PREPASS_LIST_PASS',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_list_pass',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x58; mov r14,[rip+..]; mov r12,rdx
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 4c 8b 35 ?? ?? ?? ?? 49 89 d4 48',
        expected={EU_FINGERPRINT: 0x11d3d0},
        deps=['PREPASS_JOB'],
    ),
    dict(
        name='PREPASS_XPR_MASKED',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_xpr_masked',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; and rsp,-32; sub rsp,0x160; mov rbx,[rip+..]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 60 01 00 00 48 8b 1d ?? ?? ?? ??',
        expected={EU_FINGERPRINT: 0x11d7a0},
        deps=list(_TAGS),
    ),
    dict(
        name='PREPASS_GBUFFER_VERT',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_gbuffer_vert',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x88; mov rax,[rip+..]; mov rbx,rdi; lea rsi,[rip+..]
        pattern=('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 fb '
                 '48 8d 35 ?? ?? ?? ?? 48 8d bd 70 ff ff ff b9 01 00 00 00 48 89 da 48 8b 00 48 89 45 d0 '
                 '48 8b 43 48 4c 8b 73 60 4c 8b 23 4c 8b'),
        expected={EU_FINGERPRINT: 0xc1ce0},
    ),
    dict(
        name='PREPASS_GBUFFER_PROBE',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_gbuffer_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x98; mov rax,[rip+..]; mov rbx,rdi; lea rsi,[rip+..]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 8d',
        expected={EU_FINGERPRINT: 0xc1fc0},
    ),
    dict(
        name='PREPASS_GRASS',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_grass',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0xc8; mov r14d,ecx
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 41 89 ce 48',
        expected={EU_FINGERPRINT: 0xdb700},
        deps=list(_TAGS),
    ),
    dict(
        name='PREPASS_PARTICLES',
        switch='SOTC_PREPASS_PROBE',
        group='prepass_particles',
        mode='entry',
        # xor ecx,ecx; xor r8d,r8d; jmp 0x10aef0 (the thunk; the 5 displaced bytes are the two xors)
        pattern='31 c9 45 31 c0 e9 ?? ?? ?? ?? 90 90 90 90 90 90 55 48 89 e5',
        expected={EU_FINGERPRINT: 0x10aee0},
        deps=list(_TAGS),
    ),
]
