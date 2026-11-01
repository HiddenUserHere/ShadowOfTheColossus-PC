"""GPU_STATIC class of resource-build allocations (gpu/sotc/static_class.cpp, switch
SOTC_STATIC_CLASS). Two groups, one per allocation path, so a refused hook takes down only its own
path: static_class_mesh (mesh build + buffer allocation) and static_class_tex (texture build +
single-block texture allocation).

SOTC_STATIC_CLASS=2 counts only; =1 applies: every whole page of a fresh GPU allocation made by a
mesh or non-streamed texture build whose tracking region exists is made CPU-clean and write-
protected before the game fills it, so the fill marks only the pages it writes. =3 applies and also
lets clean mesh ranges skip the buffer cache's per-bind page scan (static_class.cpp, Mode 3; needs
SOTC_SYNC_FAST=1). The hooks are the same for =1 and =3.

STATIC_CLASS_MESH_BUILD   wrap at CMesh::MeshFactory (0x1264d0): sets the thread's mesh build depth.
                          Wrap: the displaced span is the first 6 bytes (push rbp; mov rbp,rsp;
                          push r15); no branch targets there, no stack operands at depth >= 0.
STATIC_CLASS_TEX_BUILD    wrap at the texture build (0x98aa0): sets the thread's texture build depth.
                          Wrap: the displaced span is the first 5 bytes (push rbp; mov rbp,rsp;
                          push rbx); no branch targets there, no stack operands at depth >= 0.
STATIC_CLASS_BUFFER_ALLOC site at 0x8bf0b in the GPU buffer wrapper 0x8be60: the call to 0x12820
                          returns the new pointer in rax; the displaced span is mov r15,rax and
                          mov [rbx+0x10],r15 (7 bytes), so the copy has not started. Reached for
                          GPUMemory (r12 == 0) allocations with flags bits 0x800, 0x2 and 0x18
                          clear. rbx = the buffer object (size u32 at +0x20), rax = the new pointer
                          (r15 still holds 0 here). Runs before the wrapper's memcpy of the source,
                          which is the fill. Depends on the mesh build depth only: a texture build
                          with the texture group off is simply not counted.
STATIC_CLASS_TEX_ALLOC    site after the call to 0x12820 in the single-block texture copy 0x96680
                          (0x96b48). rax = the new pointer, rbx = the size. Runs before the texel
                          copy. Only the single-block path; the mip path (0x12950) is not covered.

The factories are the only callers gated on: the buffer wrapper has 106 callers, so the allocation
sites act only while a build is on the thread.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='STATIC_CLASS_MESH_BUILD',
        switch='SOTC_STATIC_CLASS',
        group='static_class_mesh',
        mode='wrap',
        # The entry of CMesh::MeshFactory (the pattern of the blob type tag, reports3/03).
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 78 01 00 00 48 8b 05 ?? ?? ?? ?? 48 89 b5 78 fe',
        expected={EU_FINGERPRINT: 0x1264d0},
    ),
    dict(
        name='STATIC_CLASS_BUFFER_ALLOC',
        switch='SOTC_STATIC_CLASS',
        group='static_class_mesh',
        mode='site',
        # mov r15,rax ; mov [rbx+0x10],r15 ; mov ecx,[rbp-0x34] (the first two bytes of the
        # 0x8bf0b site; the store is 0x8bf0e, the copy's reload of ecx follows at 0x8bf12).
        pattern='49 89 c7 4c 89 7b 10 8b 4d cc',
        deps=['STATIC_CLASS_MESH_BUILD'],
        expected={EU_FINGERPRINT: 0x8bf0b},
    ),
    dict(
        name='STATIC_CLASS_TEX_BUILD',
        switch='SOTC_STATIC_CLASS',
        group='static_class_tex',
        mode='wrap',
        # The entry of the texture build 0x98aa0 (the pattern of the blob type tag, reports3/03).
        pattern='55 48 89 e5 53 50 48 8b 47 08 48 89 f3 48 8b 77',
        expected={EU_FINGERPRINT: 0x98aa0},
    ),
    dict(
        name='STATIC_CLASS_TEX_ALLOC',
        switch='SOTC_STATIC_CLASS',
        group='static_class_tex',
        mode='site',
        # mov r14,rax ; mov rdi,r12 ; mov rdx,rbx (the texel copy's arguments)
        pattern='49 89 c6 4c 89 e7 48 89',
        deps=['STATIC_CLASS_TEX_BUILD'],
        expected={EU_FINGERPRINT: 0x96b48},
    ),
]
