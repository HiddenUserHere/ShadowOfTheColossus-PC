# SPDX-License-Identifier: GPL-2.0-or-later
"""Render pass probe (docs/re-stutters/native/re-pipelines/pass-map.md, sections 3, 6 and 7). Observe only.

PASS_RESOLVE: entry of FUN_000f8990 (0xf8990), the resolver of (material, pass). Entry registers:
  rdi = the material (param_1), rsi = the pass object (param_2). The pass slot is k = *(int*)(pass+0xf4),
  and the slot is B = material + 0x60 + 0x48*k.

MATERIAL_LOADED: wrap of the material parser FUN_000f6c90 (0xf6c90), which the factory build 0xf9390 and the
  outer loader 0xf9530 both call. Entry: rdi = the material. The post phase runs at its return (rax = 1 on
  success), when the slots are filled.

Both hooks share switch SOTC_PASS_PROBE; each has its own group, so one pattern that fails does not turn the
other off. Checks (EU 1.01 eboot, tools/re/eboot_re.py sig):
- 0xf8990: 20-byte pattern, unique (push rbp; mov rbp,rsp; push r15, r14, r12, rbx; mov r12,rsi; mov r14,rdi;
  movsxd rax,[r12+0xf4]).
- 0xf6c90: 32-byte pattern, unique (push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x108; mov rax,[rip+..];
  mov [rbp-0xe0],rsi).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PASS_RESOLVE',
        switch='SOTC_PASS_PROBE',
        group='pass_resolve',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r12,rsi; mov r14,rdi; movsxd rax,[r12+0xf4]
        pattern='55 48 89 e5 41 57 41 56 41 54 53 49 89 f4 49 89 fe 49 63 84',
        expected={EU_FINGERPRINT: 0xf8990},
    ),
    dict(
        name='MATERIAL_LOADED',
        switch='SOTC_PASS_PROBE',
        group='pass_material',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x108; mov rax,[rip+..] (wildcarded); mov [rbp-0xe0],rsi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 01 00 00 48 8b 05 ?? ?? ?? ?? 48 89 b5 20 ff',
        expected={EU_FINGERPRINT: 0xf6c90},
    ),
]
