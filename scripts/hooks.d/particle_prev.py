"""Particle stat readback probe (plan C7, docs/re-stutters/reports2/07-readback-consumers.md, 2.3).

PARTICLE_COUNT_READ: entry of sub_8c1b0 (0x8c1b0), the accessor that turns a frame-ring descriptor
into the guest address of its GPU-written per-system count array. The only two callers are
WaterRendering::ReadQueries (call at 0xe492b) and ParticleSystemPool::ProcessStats (call at
0x1019af); the handler tells them apart by the return address.
Entry registers: rdi = the descriptor (pool + 0xf8 + slot * 0x28); the caller keeps the pool object
in r13 (ProcessStats: mov r13, rdi at 0x1018db, never written again before the call).

Mode is the value of SOTC_PARTICLE_PREV. Only 3 is used: the PC-native serving
(SOTC_NATIVE_PARTICLES=prev, sotc.py sets it), where the accessor may return a completed copy of the
counts instead (gpu/sotc/native/native_particles.cpp). Any other value installs the detour, which then
continues without effect (the observe modes 1 and 2 were removed). Off when the switch is unset: the
detour is not installed.

NATIVE_PARTICLE_VERIFY: the site after the count load mov r15d,[r12+rbx*4] at 0x101a98 in
ParticleSystemPool::ProcessStats. r15d = the exact count, rbx = the system index, r13 = the pool. Only
the verify mode (SOTC_NATIVE=2 or SOTC_VERIFY=1 with mode 3) compares there; otherwise it continues.
Displaced: mov r14d,[r13+0x74]; mov r12,rbx (7 bytes, no RIP or rel32 bytes). The only branch into
the loop body targets 0x101a8d, before the site.

Checks (EU 1.01 eboot, tools/re/eboot_re.py sigfind and llvm disassembly, this session):
- 8b 4f 04 8b 07 is mov ecx,[rdi+4]; mov eax,[rdi]: 5 bytes, a clean instruction boundary before
  48 81 e1 (and rcx, 0xfff). The pattern is unique in the eboot (one match at 0x8c1b0).
- sub_8c1b0 has no branch target and no rel32 or RIP displacement in the displaced bytes.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PARTICLE_COUNT_READ',
        switch='SOTC_PARTICLE_PREV',
        group='particle_prev',
        mode='entry',
        # mov ecx,[rdi+4]; mov eax,[rdi]; and rcx,... (the 44-bit guest address of the count array)
        pattern='8b 4f 04 8b 07 48 81 e1',
        expected={EU_FINGERPRINT: 0x8c1b0},
    ),
    dict(
        name='NATIVE_PARTICLE_VERIFY',
        switch='SOTC_PARTICLE_PREV',
        group='particle_native',
        mode='site',
        # mov r14d,[r13+0x74]; mov r12,rbx (unique with the shl r12,4 that follows)
        pattern='45 8b 75 74 49 89 dc 49 c1 e4 04',
        expected={EU_FINGERPRINT: 0x101a9c},
    ),
]
