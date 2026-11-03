# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hook of the frame ring probe (docs/re-backend/10-ring-reuse.md).

RING_INIT  the ring allocator init 0x481f0 (sub_481f0, render-context init from sub_8f6f0), entry.
           It is called for the frame rings A (RenderContextFrameCPU, 130 MiB) and B
           (RenderContextFrameGPU, 20 MiB) at 0x8ff7b and 0x8ffa3, and from four other ring users
           (filtered by the name at ring+0x10). Observe only. Probe: gpu/sotc/probe_frame_rings.cpp.
           Switch SOTC_FRAME_RINGS.

The 8-byte pattern is the unique signature from tools/re/eboot_re.py sig 0x481f0 (mov byte ptr
[rdi+8],1; mov [rdi+0x80],rdx). The expected offset applies to the EU 1.01 eboot only.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='RING_INIT',
        switch='SOTC_FRAME_RINGS',
        group='frame_rings',
        mode='entry',
        pattern='c6 47 08 01 48 89 97 80',
        expected={EU_FINGERPRINT: 0x481f0},
    ),
]
