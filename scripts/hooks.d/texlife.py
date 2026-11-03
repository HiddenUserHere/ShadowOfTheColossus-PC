# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine texture lifecycle hooks (docs/re-backend/08-texture-lifecycle.md, section 4; gpu/sotc/texlife.cpp).

Switch SOTC_TEX_LIFECYCLE: 1 = fast validation, 2 = verify (counts only). Every hook has its own group, so a
site that the CPU patch passes refuse does not switch off the others. The registry is used only when the
create, level, mip and destroy hooks are all installed (texlife.cpp, Mode()).

TEXLIFE_CREATE_FILE   0x98ac0 site, after `call 0x96680` in 0x98aa0 (the file-texture factory): rax = CTexture
                      with the T# complete. A Site, not a wrap on 0x96680: the wrap of that function is refused
                      while the NATIVE_TEX_* sites are installed inside it (cpu_patches.cpp, "other CPU patches").
TEXLIFE_LEVEL         0x880c30 wrap: pre rdi = CTexture + 0xc4 (T# base and levels written after this).
TEXLIFE_MIP           0x112d30 wrap: pre reads chunk + 0x78 (CTexture); its tail jump into 0x98ae0 returns
                      through the post phase when the streamed mip copy is done.
TEXLIFE_DESTROY       0x985d0 entry: rdi = CTexture (dtor, before the region release).

Render targets (0x97a50) and aliases (0x97520, 0x96f70) are not hooked: their images have no record, so they
take today's path. The signatures are the shortest unique ones from tools/re/eboot_re.py sig, checked on the EU
1.01 eboot (one match each, tools/re/hooks_check.py). The expected offsets apply to that eboot only.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

_SWITCH = 'SOTC_TEX_LIFECYCLE'

HOOKS = [
    dict(
        name='TEXLIFE_CREATE_FILE',
        switch=_SWITCH,
        group='texlife_create_file',
        mode='site',
        # mov [rbx],rax; add rsp,8; pop rbx; pop rbp; ret (after call 0x96680)
        pattern='48 89 03 48 83 c4 08 5b 5d c3 90 90 90 90 90 90 c3 90 90 90',
        expected={EU_FINGERPRINT: 0x98ac0},
    ),
    dict(
        name='TEXLIFE_LEVEL',
        switch=_SWITCH,
        group='texlife_level',
        mode='wrap',
        # mov eax,[rdi+0xc]; mov ecx,eax; shr ecx,0x1c
        pattern='8b 47 0c 89 c1 c1 e9 1c',
        expected={EU_FINGERPRINT: 0x880c30},
    ),
    dict(
        name='TEXLIFE_MIP',
        switch=_SWITCH,
        group='texlife_mip',
        mode='wrap',
        # mov rax,[rdi+0x78]; mov esi,[rdi+0x80]; (then mov rdi,rax; jmp 0x98ae0)
        pattern='48 8b 47 78 8b b7 80 00 00 00',
        expected={EU_FINGERPRINT: 0x112d30},
    ),
    dict(
        name='TEXLIFE_DESTROY',
        switch=_SWITCH,
        group='texlife_destroy',
        mode='entry',
        pattern='55 48 89 e5 53 50 48 8d 05 ?? ?? ?? ?? 48 89 fb 48 83 c0 10 48 89 03 48 8b b3 a0 00',
        expected={EU_FINGERPRINT: 0x985d0},
    ),
]
