# SPDX-License-Identifier: GPL-2.0-or-later
"""Call probe (docs/re-stutters/native/re-pipelines/indirect-callers.md, topic T6). Observe only.

Switch SOTC_CALL_PROBE=1 (off by default). Each hook is an entry detour (mode entry) whose handler,
gpu/sotc/probe_calls.cpp, counts the calls, logs the caller (the return address at [rsp]) and the first
3 calls' arguments, and always returns HookAction::Continue. One hook group per function, so a hook that
does not resolve or is refused turns off only itself.

Targets (eboot addresses; the signatures come from tools/re/eboot_re.py sig ADDR, -n 200 where needed):
  0x86bb90 VGT_INSTANCE_STEP_RATE writer      0x86bb30 its generic sibling (writes step rate 2)
  0x862c50 R8 draw packet builder (0x54 bytes) 0x8615f0, 0x861730, 0x861880 setters (O+0xc130/c168, O+0xc0d8)
  0x860ea0, 0x8613e0 dead K2 setter copies     0x869350, 0x8694e0, 0x869370 generic setters (D2)
  0x860c50 K2 GS setter, 0x8612b0 K2 HS setter (tail-jumped from the two handlers below)
  0x94a80, 0x949f0 the GS/HS stage handlers (tail jumps to the setters above)
  0x103aa0 entry 0 of the 7-entry function table at 0x15fea00

Checks (EU 1.01 eboot, tools/re/hooks_check.py): each pattern must match exactly once at its expected offset.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'


def _entry(address, pattern):
    """One entry hook of the function at the eboot address: hook CALL_<hex>, its own group, the SOTC_CALL_PROBE switch."""
    return dict(
        name='CALL_%X' % address,
        switch='SOTC_CALL_PROBE',
        group='call_%x' % address,
        mode='entry',
        pattern=pattern,
        expected={EU_FINGERPRINT: address},
    )


HOOKS = [
    _entry(0x86bb90,
           '55 48 89 e5 41 57 41 56 53 50 48 89 fb 41 89 d6 41 89 f7 48 8b 43 10 48 8b 4b 08 48 29 c1 48 c1 e9 02 '
           '83 f9 03 77 ?? 48 8b 53 20 be 04 00 00 00 48 89 df ff 53 18 84 c0 74 ?? 48 8b 43 10 c7 00 00 69 02 c0 '
           'c7 40 04 a8'),
    _entry(0x86bb30,
           '55 48 89 e5 41 56 53 48 89 fb 41 89 f6 48 8b 43 10 48 8b 4b 08 48 29 c1 48 c1 e9 02 83 f9 02 77 ?? '
           '48 8b 53 20 be 03 00 00 00 48 89 df ff 53 18 84 c0 74 ?? 48 8b 43 10 41 0f b6 ce c7 00 00 69 01 c0 '
           'c7 40 04 ad 02 00'),
    _entry(0x862c50,
           '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 89 4d a8 89'),
    _entry(0x8615f0, '89 b7 68 c1 00 00 48 89'),
    _entry(0x861730, '55 41 56 53 0f b7 46 24'),
    _entry(0x861880, 'c6 87 57 c2 00 00 01 48'),
    _entry(0x860ea0,
           '55 48 89 e5 41 57 41 56 41 55 41 54 53 50 48 89 fb 89 55 d4'),
    _entry(0x8613e0,
           '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 0d'),
    _entry(0x869350, '48 8b 47 10 81 c6 00 60'),
    _entry(0x8694e0,
           '55 48 89 e5 41 56 53 49 89 fe 89 cb c1 e1 10 81 c6 00 60 ff'),
    _entry(0x869370,
           '55 48 89 e5 41 57 41 56 41 54 53 89 cb 41 89 d6 41 89 f7 49 89 fc e8 ?? ?? ?? ?? 49 8b 54 24 10 31 c9 '
           '83 f8 01 be 00 00 00 f0 0f 45 f1 c1 e3 1c 41 81 c7 00 60 ff ff 21'),
    _entry(0x860c50,
           '55 48 89 e5 41 57 41 56 53 50 48 89 fb 31 ff c6'),
    _entry(0x8612b0,
           '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 2d ?? ?? ?? ?? 48 89 fb 49 89 f6 41 89'),
    _entry(0x94a80, '48 8b 47 20 8b 40 0c 48'),
    _entry(0x949f0, '48 8b 47 20 8b 40 04 48'),
    _entry(0x103aa0,
           '48 8b 46 10 48 8b 56 08 48 8b 88 c0 0d 00 00 48 85 d2 74 ?? e9 ?? ?? ?? ?? 48 8b 16 e9 ?? ?? ?? ?? '
           '90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec e8 01 00 00'),
]
