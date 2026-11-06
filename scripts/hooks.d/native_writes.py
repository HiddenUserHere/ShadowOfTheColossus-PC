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
NATIVE_DCB_SUBMIT_FLIP  Site 0x91f97 inside the flip submit thunk 0x91f90 (the last DCB of each mode-1 job, the
                      frame's end, goes there instead of 0x91f80). Same handler as NATIVE_DCB_SUBMIT: rdi = slot.
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
NATIVE_RECORD_18C390 0x18c390, the per-object binding record written by the render item job 0x34cad0 (rdi = the
                     object; +0x278..+0x298 rewritten twice per frame). PerFrame at entry, group native_rec.

Site (one instruction inside a function, registers editable, the displaced instructions run next):
NATIVE_PARTICLE_SHIFT 0x101817, ParticleRenderer_ShiftRecords after the record writes of one record: the source
                      record (r14 + r15) and the destination record ([rbp-0x30] + r15), each stride bytes (u32 at
                      this+0x74), Persistent. Shares no site with the other hooks.

Flicker brief 2 (SOTC_VERIFY=n1 misses, native_hooks.cpp "Flicker brief 2"):
NATIVE_SUNSHADOW_AFCC0 / _B0100  Site, ResolveSunShadows and its twin: the 0x10-byte zero-copy backing, the 0x18-byte
                      bind block and the 0x60-byte constants chunk, PerFrame (counter sun).
NATIVE_UDATA_85ECD0 / _85E5D0  Wrap, user-data packets written into the command buffer at [rdi+0x90] (the packet
                      writer 0x884240 itself cannot be hooked: jump table). Pre/post cursor delta, PerFrame (pkt).
NATIVE_UDATA_884349    Site 0x884349, the end of the packet writer 0x884240 itself (every path through the jump
                      table falls through here): the whole packet [rcx - 8, rcx + edx * 4), PerFrame (pkt). Covers
                      its 17 other callers (Material_BindGraphicsShaders_A, 0x861ce0 ...), reached after the opening
                      cinematic has played: their packets went to the GPU stale (no grass at the Shrine).
NATIVE_INST_STAGE_F9C40  Wrap, sub_f9c40 (callers 0xfb1e0 / 0xfb2d0): copies 32 bytes (+0xc4..+0xe3) of each dirty
                      object into the instance staging array [rdi+0x580] + ([rdi+0x478] - [rdi+0x470]); count = the
                      high dword of [rdi+0x5b0]. PerFrame (inst). Not announced before: after the opening cinematic
                      the Shrine terrain instances were drawn from stale bytes (no terrain with SOTC_NATIVE=1).
NATIVE_PALETTE_A3EE50 Wrap, cloth skeleton matrix palette (joints * 0x40 bytes from 0x12820), PerFrame (pal).

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
    # NATIVE_RES_COPY (a Site after the initial-data memcpy in 0x8be60) is gone: every guest memcpy is announced by
    # the runtime (src/runtime.c), and its detour made the engine refuse NATIVE_ZC_CREATE on the same function, so
    # no zero-copy buffer was ever announced (1 Nov, n1verify: 0x199c4f, 0xafe13 bound without an announce).
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
        name='NATIVE_DCB_SUBMIT_FLIP',
        switch='SOTC_NATIVE',
        group='native_submit_flip',
        mode='site',
        # 0x91f90 is the flip submit thunk: the last DCB of a mode-1 job (the frame's end job, 0xcefa0) goes
        # through it to 0x863aa0 and never through 0x91f80. Site 0x91f97 is after its lea (RIP-relative, not
        # displaced): mov rdi,[rdi] is still unexecuted, so rdi holds the slot, as at the 0x91f80 entry.
        pattern='48 8b 3f 41 89 f0 48 8b 00',
        expected={EU_FINGERPRINT: 0x91f97},
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
    # --- impl-writes6: the CPU-written user-data block of the cull dispatch, and the 0x117930 material copy ---
    dict(
        name='NATIVE_CULL_DISPATCH_BLK',
        switch='SOTC_NATIVE',
        group='native_cull',
        mode='entry',
        # BPGnm_DispatchWithLabelWait 0x92b50 (args: rdi ctx, rsi buffer, rdx count, rcx block, r8 dword count). The
        # first call 0x928f0 binds rcx as a buffer V# of r8 elements, so the GPU reads the block through user data.
        # Announced only from BPXPR_IndexCullObjects (0x11dbe0), which writes the block just before the call
        # (native_hooks.cpp CullDispatchEntry, caller span kCullDispatchCallers). Unique (sigfind) at 0x92b50.
        pattern='55 48 89 e5 41 56 53 49 89 fe e8 ?? ?? ?? ?? 49 8b 3e be 08',
        expected={EU_FINGERPRINT: 0x92b50},
    ),
    dict(
        name='NATIVE_MAT_COPY_STREAM',
        switch='SOTC_NATIVE',
        group='native_strm',
        mode='site',
        # 0x117930 (the second Material Param Update path, reached when the job's [1] is 0 via 0xd0710 -> 0x117140),
        # after memcpy(rbx, DAT_02ed2108, [r12]) at 0x11805a. The DMA-like call is 0x928f0 (rel32 6d a8 f7 ff here, which
        # makes the pattern unique: the NATIVE_MAT_COPY pattern at 0x11788f has its own call rel32).
        pattern='48 8d 05 ?? ?? ?? ?? 41 8b 14 24 4c 8b 64 24 08 48 89 d9 48 8b 30 c1 ea 02 4c 89 e7 41 89 d0 e8 6d a8 f7 ff',
        expected={EU_FINGERPRINT: 0x11805f},
    ),
    # --- impl-writes7-render: the per-object binding record written by the render item job (0x34cad0) ---
    dict(
        name='NATIVE_RECORD_18C390',
        switch='SOTC_NATIVE',
        group='native_rec',
        mode='entry',
        # sub_18c390 (rdi = the 0x298-byte object of [render item + 0xa0]; writes +0x278..+0x290 twice per frame, the
        # only caller is 0x34ccac in sub_34cad0). The object is persistent (one address per session) and the GPU
        # binds 192 KiB ranges over it; announced PerFrame before the body (read at the next submit).
        pattern='55 48 89 e5 41 57 41 56 53 48 83 ec 18 4c 8b 3d ?? ?? ?? ?? 48 89 fb 49 89 ce',
        expected={EU_FINGERPRINT: 0x18c390},
    ),
    # --- SOTC_VERIFY=n1 misses: plain-store buffers that the GPU binds the same or next frame (flicker brief 2) ---
    dict(
        name='NATIVE_UBUF_198BB0',
        switch='SOTC_NATIVE',
        group='native_ubuf',
        mode='site',
        # sub_198bb0 (BPXPR pass-through builder), right after the fill loop of the 0x804 zero-copy buffer (16 bytes per
        # element, plain stores at 0x199c4f / 0x199c7b / 0x199c80) and the following `call 0x8c0d0`: r12 = end of the buffer
        # (the loop advanced it 0x10 per element), r13 = param_1 (element list [+0x70, +0x78)). The 0x8be60 creator
        # announced the buffer before the loop wrote it. Displaced: mov rcx,[rbp-0x610] (no RIP-relative operand).
        pattern='48 8b 8d f0 f9 ff ff 4c 8d 75 b0 be',
        expected={EU_FINGERPRINT: 0x199d62},
    ),
    dict(
        name='NATIVE_UBUF_1AF960',
        switch='SOTC_NATIVE',
        group='native_ubuf',
        mode='wrap',
        # sub_1af960 (rdi = builder, [rdi+8] = the object; rsi = element vector): fills the 0x12820 buffers at obj+0x48,
        # +0x50, +0x58, +0x60 and +0x70; the +0x60 / +0x70 ones (plain stores at 0x1afb79 / 0x1afbfd) are written after
        # the 0x8be60 creators ran.
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 48 8d 1d ??',
        expected={EU_FINGERPRINT: 0x1af960},
    ),
    dict(
        name='NATIVE_UBUF_1B0440',
        switch='SOTC_NATIVE',
        group='native_ubuf',
        mode='wrap',
        # sub_1b0440 (rdi = builder, [rdi+8] = the object; rsi = element vector): fills obj+0x40 (0x14 bytes per element)
        # and, through sub_1b0080 (plain stores at 0x1b00a9), obj+0x78 (0x40 bytes per element).
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 49 89 f6 4c 8d 05 ??',
        expected={EU_FINGERPRINT: 0x1b0440},
    ),
    # --- foliage displacer per-frame block (impl-grass): the 8-byte 0x12820 block of each displacer job variant ---
    dict(
        name='NATIVE_DISP_BLOCK_D83B0',
        switch='SOTC_NATIVE',
        group='native_chunk',
        mode='site',
        # sub_d83b0 after `call 0x12820` (block size 8): mov rsi,[r14+0xb0]; mov r12,rax. rax = the block, the store of
        # [r14+0xb0] into it follows. The displaced mov has no RIP-relative or rel32 operand; the signature is unique.
        pattern='49 8b b6 b0 00 00 00 49 89 c4 b9 10',
        expected={EU_FINGERPRINT: 0xd852b},
    ),
    dict(
        name='NATIVE_DISP_BLOCK_D8660',
        switch='SOTC_NATIVE',
        group='native_chunk',
        mode='site',
        # sub_d8660 after `call 0x12820`: mov rsi,[r14+0xb0]; mov r13,rax (same contract as NATIVE_DISP_BLOCK_D83B0).
        pattern='49 8b b6 b0 00 00 00 49 89 c5 b9 10',
        expected={EU_FINGERPRINT: 0xd87cf},
    ),
    # --- flicker brief 2 (n1verify misses): plain-store writers that no announce covered ---
    dict(
        name='NATIVE_SUNSHADOW_AFCC0',
        switch='SOTC_NATIVE',
        group='native_sun',
        mode='site',
        # sub_afcc0 (ResolveSunShadows) after the last plain store of its blocks: the 0x10-byte zero-copy backing at
        # [[r15+0x10]+0xf28], the constants chunk at [r15+0xb0] and the 0x18-byte bind block in r12. Site 0xaff60 is
        # `mov rax,[r14+0x1ae0]`, right after `mov [r12+0x10],rax` (the displaced instruction has no RIP-relative
        # or rel32 operand).
        pattern='49 8b 86 e0 1a 00 00 48 85 c0 74 ?? 48 8b 00 80 78 19 02 75 ?? 49 8d 7e 50 48 8d 75 b0 ba 02 00 00 00 4c 89 65 b0 e8 ?? ?? ?? ?? 48',
        expected={EU_FINGERPRINT: 0xaff60},
    ),
    dict(
        name='NATIVE_SUNSHADOW_B0100',
        switch='SOTC_NATIVE',
        group='native_sun',
        mode='site',
        # sub_b0100, the twin of sub_afcc0: after `mov [rbx+0x10],rax` (rbx = the 0x18-byte bind block, r14 = the
        # shadow object). Site 0xb03bd is `mov rax,[r12+0x20]`.
        pattern='49 8b 44 24 20 8b 70 14 49 8b 84 f4 d8 01 00 00 48 85 c0 74 ?? 48 8b 00 80 78 19 02 75 ?? 48 89',
        expected={EU_FINGERPRINT: 0xb03bd},
    ),
    dict(
        name='NATIVE_UDATA_85ECD0',
        switch='SOTC_NATIVE',
        group='native_pkt',
        mode='wrap',
        # sub_85ecd0 (binds a user-data block: a 35-byte wrapper that tail-jumps to the packet writer 0x884240, which
        # cannot be hooked itself: its jump table is an unresolved indirect branch). rdi = the context whose command
        # buffer object is at [rdi+0x90] (cursor at +0x10). Pre/post read the cursor: the packet words in between.
        pattern='48 8b 8f a8 18 00 00 48',
        expected={EU_FINGERPRINT: 0x85ecd0},
    ),
    dict(
        name='NATIVE_UDATA_85E5D0',
        switch='SOTC_NATIVE',
        group='native_pkt',
        mode='wrap',
        # sub_85e5d0 (user-data flush: SET_SH_REG packets and the embedded user-data table into the command buffer
        # at [rdi+0x90]). Same cursor contract as NATIVE_UDATA_85ECD0.
        pattern='55 48 89 e5 41 57 41 56 41 54 53 49 89 fc 41 80 bc 24 b0 18',
        expected={EU_FINGERPRINT: 0x85e5d0},
    ),
    dict(
        name='NATIVE_INST_STAGE_F9C40',
        switch='SOTC_NATIVE',
        group='native_inst_stage',
        mode='wrap',
        # sub_f9c40 (76 bytes, leaf): mov rax,[rdi+0x5b0]; test eax,eax ... vmovups [rdx],ymm0 per object. Pre reads
        # the destination and the count, post announces the 32-byte records written.
        pattern='48 8b 87 b0 05 00 00 85',
        expected={EU_FINGERPRINT: 0xf9c40},
    ),
    dict(
        name='NATIVE_UDATA_884349',
        switch='SOTC_NATIVE',
        group='native_pkt_end',
        mode='site',
        # mov eax,edx; lea rax,[rcx+rax*4]; mov [r14+0x10],rax: the cursor update at the end of the packet writer
        # 0x884240, after every word of the packet is written. rcx = packet start + 8, edx = the words after those 8
        # bytes (count + 2). Its own group: a refusal leaves NATIVE_UDATA_85ECD0 / _85E5D0 on.
        pattern='89 d0 48 8d 04 81 49 89 46 10 5b 41',
        expected={EU_FINGERPRINT: 0x884349},
    ),
    dict(
        name='NATIVE_PALETTE_A3EE50',
        switch='SOTC_NATIVE',
        group='native_pal',
        mode='wrap',
        # sub_a3ee50 (cloth skeleton matrix palette builder, callers sub_a3e170 and sub_a3fa30): returns a 0x12820
        # block of (bytes of the index vector at param_1+0x90..0x98) * 16 filled with plain stores.
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 4c 8d 05',
        expected={EU_FINGERPRINT: 0xa3ee50},
    ),
]



