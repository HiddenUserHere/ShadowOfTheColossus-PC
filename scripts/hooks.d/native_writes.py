# SPDX-License-Identifier: GPL-2.0-or-later
"""PC-native write announces (docs/re-stutters/native/PLAN.md, N2; handlers in gpu/sotc/native/native_hooks.cpp).

Every hook is placed where the engine has finished a write (or before a release). Switch SOTC_NATIVE (any
non-zero value), one group per write path so a refused hook takes down only its own path.

Entry  (the handler runs before the function body; the body only reads what the handler saw):
NATIVE_DCB_SUBMIT     0x91f80, the DCB submit thunk (called once per DCB by KickoffSubmitJob 0xd07c0). The
                      DCB context (rdi = [rdi]) gets its DCB range [base, cursor) plus its CCB and ACE ranges
                      announced PerFrame, then the per-frame chunk list (NATIVE_CHUNK) is flushed.
NATIVE_CCB_SUBMIT     0x16f100, the CCB submit (called per CCB by 0xd07c0). [start, cursor) of the CCB object,
                      PerFrame, then the chunk flush.
NATIVE_ZC_CREATE      0x8be60, the resource creator. Zero-copy (flag 0x800) buffers of the per-frame writers
                      (particles 0xffe60, BPXPR 0x19b760 / 0x19c1a0 / 0x19c7e0, MaterialParam-like 0x94fb0 and 0x198bb0)
                      announce [backing, count * elem) PerFrame. The caller filter keeps other creators out.
                      The writers that memcpy before the call (0x94fb0, 0x19c1a0, 0x19c7e0) announce after their
                      writes; the particle and BPXPR builders write after the call, which is safe for PerFrame data
                      (read at the next submit, after the job waits in 0xd07c0).
NATIVE_DESC_865410    0x865410, V# buffer encoder. Wrap: pre stashes desc/base/count, post reads the encoded word1
NATIVE_DESC_8655D0    0x8655d0, constant V#                 (stride) and word3 (format) and calls NoteDescriptor.
NATIVE_DESC_865600    0x865600, image/buffer word encoder
NATIVE_DESC_865500    0x865500, buffer V# with stride and count

Wrap (the body runs; the post phase sees the final state):
NATIVE_CHUNK         0x8c0d0, the per-frame block allocator (0x12820 category 1). Post: the new block is queued in
                     the per-frame chunk list when the caller is one of the chunk writers (heaps.md 7.5, the 27
                     memcpy callers, and the instance writers 0x11c710 and 0x123410). The queue is announced
                     PerFrame at the next DCB or CCB submit (one announce point for all chunk users).
NATIVE_MATPARAM      0xf5cd0, MaterialParam sub-buffer (post: buffer param_1[1], size u32 at +0x38; PerFrame).
NATIVE_FEF40_BLOCK   0xfef40, the 0x17090 block it memcpy's (post: block at +0x48, size u32 of the source header
                     at param_2+4; PerFrame).

Entry (listed here because Wrap is refused for 0x121ed0):
NATIVE_INST_121ED0   0x121ed0 (indirect jump: no Wrap), the persistent instance block at *(this+0x1d8),
                     rewritten with 0x18 bytes each frame. Announced PerFrame at entry: the bytes are read at the next
                     submit, after the job waits, so an announce before the body is safe for PerFrame data.

Site (one instruction inside a function, registers editable, the displaced instructions run next):
NATIVE_PARTICLE_SHIFT 0x101817, ParticleRenderer_ShiftRecords after the record writes of one record: the source
                      record (r14 + r15) and the destination record ([rbp-0x30] + r15), each stride bytes (u32 at
                      this+0x74), Persistent. Shares no site with the other hooks.

Resource and texture hooks of the first batch (NATIVE_RES_COPY, NATIVE_TEX_COPY, NATIVE_TEX_TAIL_COPY,
NATIVE_MIP_COPY, NATIVE_MIP_RELEASE) are unchanged; see docs/re-stutters/native/impl-writes.md.

Not hooked (see impl-writes.md, section 4): the 0x117160 material copy (its memcpy return site 0x11788f has no
unique signature yet), 0xbe1f0 / 0xcedf0 / 0x8b7f0 view writers, 0x18aa20 transform writer.

Each pattern is the signature from tools/re/eboot_re.py sig (unique in the text segment); '??' is a wildcard.
Checked with tools/re/hooks_install_check.py on the EU 1.01 eboot. The expected offsets apply to that eboot only.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    # --- first batch (persistent resource and mip writes) ---
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
    # --- N2 batch: DCB / CCB submit, zero-copy creation, chunk allocator, rewriters, descriptors, records ---
    dict(
        name='NATIVE_DCB_SUBMIT',
        switch='SOTC_NATIVE',
        group='native_submit',
        mode='entry',
        # mov rdi,[rdi]; mov esi,esi; jmp 0x8636e0: the DCB submit thunk, called once per DCB by 0xd07c0
        pattern='48 8b 3f 89 f6 e9 ?? ??',
        expected={EU_FINGERPRINT: 0x91f80},
    ),
    dict(
        name='NATIVE_CCB_SUBMIT',
        switch='SOTC_NATIVE',
        group='native_submit',
        mode='entry',
        # mov rcx,rdi; mov eax,[rcx+0x20]; sub eax,[rcx+0x10]: the CCB submit, called by 0xd07c0
        pattern='48 89 f9 8b 41 20 2b 41',
        expected={EU_FINGERPRINT: 0x16f100},
    ),
    dict(
        name='NATIVE_ZC_CREATE',
        switch='SOTC_NATIVE',
        group='native_zc',
        mode='entry',
        # resource creator 0x8be60 (prologue; rdi = resource, r8 = flags, r9 = backing)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4d 89 ce 41 89 c9 41',
        expected={EU_FINGERPRINT: 0x8be60},
    ),
    dict(
        name='NATIVE_CHUNK',
        switch='SOTC_NATIVE',
        group='native_chunk',
        mode='wrap',
        # per-frame block allocator 0x8c0d0 (0x12820 category 1)
        pattern='55 48 89 e5 53 50 48 89 fb 85 f6 75',
        expected={EU_FINGERPRINT: 0x8c0d0},
    ),
    dict(
        name='NATIVE_MATPARAM',
        switch='SOTC_NATIVE',
        group='native_mat',
        mode='wrap',
        # MaterialParam sub-buffer 0xf5cd0 (the buffer of param_1[1] is written by its memcpy and loop)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ?? 49 89 ce 4c',
        expected={EU_FINGERPRINT: 0xf5cd0},
    ),
    dict(
        name='NATIVE_FEF40_BLOCK',
        switch='SOTC_NATIVE',
        group='native_mat',
        mode='wrap',
        # 0xfef40: the 0x17090 block at +0x48 receives the memcpy of the source header
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 49 89 d6 48 89 f3 49',
        expected={EU_FINGERPRINT: 0xfef40},
    ),
    dict(
        name='NATIVE_INST_121ED0',
        switch='SOTC_NATIVE',
        group='native_inst',
        mode='entry',
        # instance class 0 writer 0x121ed0: writes the persistent block at this+0x1d8 each frame
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 4c 8b 35 ?? ?? ?? ?? 48 89 fb 48',
        expected={EU_FINGERPRINT: 0x121ed0},
    ),
    dict(
        name='NATIVE_DESC_865410',
        switch='SOTC_NATIVE',
        group='native_desc',
        mode='wrap',
        # V# buffer encoder 0x865410 (desc, base, format, count)
        pattern='55 48 89 e5 41 57 41 56 53 48 83 ec 18 4c 8b 3d ?? ?? ?? ?? 48 89 fb 48 8d 7d d8 41',
        expected={EU_FINGERPRINT: 0x865410},
    ),
    dict(
        name='NATIVE_DESC_8655D0',
        switch='SOTC_NATIVE',
        group='native_desc',
        mode='wrap',
        # constant V# encoder 0x8655d0 (desc, base, size)
        pattern='89 37 48 c1 ee 20 83 c2',
        expected={EU_FINGERPRINT: 0x8655d0},
    ),
    dict(
        name='NATIVE_DESC_865600',
        switch='SOTC_NATIVE',
        group='native_desc',
        mode='wrap',
        # image/buffer word encoder 0x865600 (desc, base, size)
        pattern='89 37 48 c1 ee 20 81 e6 ff 0f 00 00 89 77 04 c7',
        expected={EU_FINGERPRINT: 0x865600},
    ),
    dict(
        name='NATIVE_DESC_865500',
        switch='SOTC_NATIVE',
        group='native_desc',
        mode='wrap',
        # buffer V# encoder with stride and count 0x865500
        pattern='55 48 89 e5 41 57 41 56 53 50 48 89 fb c1 e2 10',
        expected={EU_FINGERPRINT: 0x865500},
    ),
    dict(
        name='NATIVE_PARTICLE_SHIFT',
        switch='SOTC_NATIVE',
        group='native_particle',
        mode='site',
        # mov dword ptr [r13+rax*4],ebx: after the record writes of ParticleRenderer_ShiftRecords (0x101720)
        pattern='41 89 5c 85 00 48 89 d8',
        expected={EU_FINGERPRINT: 0x101817},
    ),
    # --- N2 write-side completion (impl-writes3.md): material copy, view blocks, view table, instance transform ---
    dict(
        name='NATIVE_MAT_COPY',
        switch='SOTC_NATIVE',
        group='native_matcopy',
        mode='site',
        # 0x117160 (Material Param Update), after the memcpy of 0x11788a into the block rbx: the next instructions
        # load the GPU DMA source and call the DMA 0x16eb90. Dest rbx, size u32 at [r12] (r12 = 0x2ed2100, not yet
        # reused here). The short signature (up to the lea) also matches CRenderer::UpdateTextureStreamingGPUData
        # (0x118046); the exact rel32 of the DMA call (e8 dd 72 05 00) makes the pattern unique.
        pattern='48 8d 05 ?? ?? ?? ?? 41 8b 14 24 4c 8b 64 24 08 48 89 d9 48 8b 30 c1 ea 02 4c 89 e7 41 89 d0 e8 dd 72 05 00',
        expected={EU_FINGERPRINT: 0x11788f},
    ),
    dict(
        name='NATIVE_VIEW_BLOCK',
        switch='SOTC_NATIVE',
        group='native_view',
        mode='wrap',
        # 0x8c120: the view block allocator (0x12820 category from 0x2ed1f54, size u32 at [rdi+0x20]). Its result is
        # stored into the handle at rsi (low word [rsi], high 12 bits [rsi+4]). Callers: sub_bdfd0 (0xbe055) and
        # SetViewParams (0xbe27b) only. The block is queued in the chunk list (announced PerFrame at the next submit).
        pattern='55 48 89 e5 41 56 53 48 8d 05 ?? ?? ?? ?? 48 89 fb 49 89 f6 ba 10 00 00',
        expected={EU_FINGERPRINT: 0x8c120},
    ),
    dict(
        name='NATIVE_VIEW_TABLE',
        switch='SOTC_NATIVE',
        group='native_view',
        mode='site',
        # 0xcedf0 (EndFrameView_WriteB), the common exit after `mov [r14+rbx+8],rdx`: r14 = the view table of
        # [obj+0x330], rbx = view index * 16. Writes 16 bytes per call, PerFrame. Not a 0x8c120 block: the table is
        # persistent and rewritten each frame.
        pattern='48 83 c4 08 5b 41 5e 41 5f 5d c3 48 8b 12 89 fb',
        expected={EU_FINGERPRINT: 0xcef0d},
    ),
    dict(
        name='NATIVE_INST_18AA20',
        switch='SOTC_NATIVE',
        group='native_inst2',
        mode='entry',
        # instance class 2 writer 0x18aa20 (rdi = the instance object): writes the 0x18-byte block at [this+0xa8]
        # when that pointer is set (checked at 0x18aaaf). Announced at entry, PerFrame (read at the next submit).
        pattern='55 48 89 e5 41 56 53 48 83 ec 60 4c 8b 35 ?? ?? ?? ?? 48 89 fb c5 fa 10',
        expected={EU_FINGERPRINT: 0x18aa20},
    ),
    # --- impl-writes4: the zero-copy buffers of the BuildXPR model-section build (load time) ---
    dict(
        name='NATIVE_BUILDXPR_ZC',
        switch='SOTC_NATIVE',
        group='native_buildxpr',
        mode='site',
        # BuildXPR 0x1b32f0, the only return: `mov rax,r12` at 0x1b3f97 (before the epilogue). r12 is the object
        # (the return value) whose [+0x18, +0x20) is the zero-copy V# backing written by the loop. A wrap is refused
        # ("stack operand at an unknown stack depth": the prologue realigns rsp). Persistent (native_hooks.cpp
        # BuildXprExit). The signature is the tool's `sig 1b3f97` output (unique in the text segment).
        pattern='4c 89 e0 48 8d 65 d8 5b 41 5c 41 5d 41 5e 41 5f 5d c3 e8 ?? ?? ?? ?? 90 90 55 48 89',
        expected={EU_FINGERPRINT: 0x1b3f97},
    ),
]



