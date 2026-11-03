# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hook of the sun shadow cascade cache probe (docs/re-backend/12-shadow-cache.md).

SHADOW_CASCADE_BODY  RenderShadowCascade body, FUN_000cdbc0 (0xcdbc0), entry. rdi = the cascade
                     render-job object; its cascade index is the int at +0x88. Mode 1 returns
                     without the body for a cascade skipped this frame (HookAction::Return).
SHADOW_VIEW_BUILD    FUN_000c7850 (0xc7850), wrap. rsi = view_state. Pre: mode 1 writes the frozen
                     matrix block of a skipped cascade over its source before the view objects are
                     built; mode 2 only compares. Post: records the skipped view object pointers. Both hooks use the switch SOTC_SHADOW_CACHE
                     (gpu/sotc/probe_shadow_cache.cpp). Neither function has another hook or CPU patch.

The patterns are the prologues, checked with tools/re/eboot_re.py sigfind on the EU 1.01 eboot
(one match each: 0xcdbc0 and 0xc7850). The expected offsets apply to that eboot only; any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='SHADOW_CASCADE_BODY',
        switch='SOTC_SHADOW_CACHE',
        group='shadow_cache',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x78; mov r12,[rip+..] (wildcarded); mov rbx,rdi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 78 4c 8b 25 ?? ?? ?? ?? 48 89 fb 4c',
        expected={EU_FINGERPRINT: 0xcdbc0},
    ),
    dict(
        name='SHADOW_VIEW_BUILD',
        switch='SOTC_SHADOW_CACHE',
        group='shadow_cache',
        mode='wrap',  # pre: freeze (mode 1); post: record the skipped view objects
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0xb8; mov rax,[rip+..]; mov [rbp-0xc0],rdx
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 95 40 ff',
        expected={EU_FINGERPRINT: 0xc7850},
    ),
]
