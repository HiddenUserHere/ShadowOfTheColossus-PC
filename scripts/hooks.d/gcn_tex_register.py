# SPDX-License-Identifier: GPL-2.0-or-later
"""S3 T# registration ahead of draws (gpu/sotc/gcn_tex_register.cpp; GCN-NATIVE-PLAN-2026-11-09.md step 2).

Switch SOTC_GCN_TEX_REGISTER=1 (off by default; the handlers do nothing unless SOTC_GCN_TEX is set too). Every hook has
its own group, so a site that is refused turns off only itself. The material source has no hook here: it rides on
PIPE_PREDICT_MATERIAL (pipe_predict.py, wrap of the factory 0xf9390), which the loader turns on for this switch.

GCN_TEX_REG_BASE      0x881e60 wrap: T# base writer; pre rdi = CTexture + 0xc4 (words before), post reads the words
                      after. An in-place rewrite of a T# the table holds is refreshed at the next submit.
GCN_TEX_REG_LEVEL     0x880c46 site inside the level writer 0x880c30 (`and eax,0xfff00fff`): rdi = T#, eax = old
                      dword 3, esi/edx = base/last level. A site, not the entry: TEXLIFE_LEVEL (texlife.py) wraps the
                      entry, and both can be installed (one function, detours only, spans apart).
GCN_TEX_REG_RT        0x97a50 wrap: descriptor / render target factory; post rax = the CTexture (T# at +0xc4).
GCN_TEX_REG_PASS_T22  0x861fe0 entry: T# setter, layout table +0x22 (pass textures, 0x133550 and others);
                      ecx = count, r8 = count T#s of 32 bytes.
GCN_TEX_REG_PASS_T42  0x862160 entry: the same with layout table +0x42 (0xa8c50 and others).

The block walkers 0xf9c40 / 0xf9ef0 and the notify 0xf8d10 are not hooked: they copy payload T#s (CTexture + 0xc4)
into the material blocks, and those words change only through 0x881e60 / 0x880c30, which are hooked; 0xf9c40 also runs
about 50,000 times per second (texture-binding.md RA.3).

Signatures: tools/re/eboot_re.py sig ADDR (the two setters differ only in the table byte and in the 32-byte copy, so
their signatures run to the vmovups ymm), checked with tools/re/hooks_check.py. The EU 1.01 expected offsets resolve by
the fingerprint; the US 1.00 eboot (same code) scans each pattern, which matches once.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

_SWITCH = 'SOTC_GCN_TEX_REGISTER'

# The prologue and loop head shared by the T# setters 0x861fe0 and 0x862160, up to the layout table displacement.
_SETTER_HEAD = ('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 41 89 ce 48 63 c6 4d 89 c7 89 d3 49 89 fc 89 75 d4 '
                '48 89 45 c8 45 85 f6 0f 84 ?? ?? ?? ?? 48 8b 4d c8 89 c8 4d 8b ac cc 80 c1 00 00 c1 e0 09 83 f9 02 '
                '8d 04 40 48 98 49 8d 84 84 90 00 00 00 48 89 45 c0 0f 85 ?? ?? ?? ?? 66 0f 1f 44 00 00 89 db 41 0f '
                'b7 54 5d')
# After the table byte: cmp edx,0xffff; je; mov eax,edx; and edx,0x3fff; test ax,ax; js; vmovups ymm0,[r15] (32 bytes).
_SETTER_TAIL = '81 fa ff ff 00 00 74 ?? 89 d0 81 e2 ff 3f 00 00 66 85 c0 78 ?? c4 c1 7c'

HOOKS = [
    dict(
        name='GCN_TEX_REG_BASE',
        switch=_SWITCH,
        group='gcn_tex_reg_base',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x18; mov r12,[rip+..]; mov rbx,rdi; mov ecx,0x614; mov r14d,esi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 25 ?? ?? ?? ?? 48 89 fb b9 14 06 00 00 '
                '41 89 f6 49',
        expected={EU_FINGERPRINT: 0x881e60},
    ),
    dict(
        name='GCN_TEX_REG_LEVEL',
        switch=_SWITCH,
        group='gcn_tex_reg_level',
        mode='site',
        # and eax,0xfff00fff; shl esi,0xc; shl edx,0x10
        pattern='25 ff 0f f0 ff c1 e6 0c c1 e2 10',
        expected={EU_FINGERPRINT: 0x880c46},
    ),
    dict(
        name='GCN_TEX_REG_RT',
        switch=_SWITCH,
        group='gcn_tex_reg_rt',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0xf8; mov [rbp-0xb4],edi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec f8 00 00 00 89 bd 4c ff',
        expected={EU_FINGERPRINT: 0x97a50},
    ),
    dict(
        name='GCN_TEX_REG_PASS_T22',
        switch=_SWITCH,
        group='gcn_tex_reg_pass_t22',
        mode='entry',
        pattern=_SETTER_HEAD + ' 22 ' + _SETTER_TAIL,
        expected={EU_FINGERPRINT: 0x861fe0},
    ),
    dict(
        name='GCN_TEX_REG_PASS_T42',
        switch=_SWITCH,
        group='gcn_tex_reg_pass_t42',
        mode='entry',
        pattern=_SETTER_HEAD + ' 42 ' + _SETTER_TAIL,
        expected={EU_FINGERPRINT: 0x862160},
    ),
]
