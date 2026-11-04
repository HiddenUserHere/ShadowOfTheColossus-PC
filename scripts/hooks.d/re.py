# SPDX-License-Identifier: GPL-2.0-or-later
"""One-run RE confirmation probe (gpu/sotc/probe_re.cpp). Observe only.

Switch SOTC_RE_PROBE=1 (off by default). Every hook has its own group, so a hook that does not resolve or is refused
turns off only itself. Topics and the docs they close (docs/re-stutters/native/re-pipelines/):
  material     RE_MATERIAL_FACTORY (wrap 0xf9390, return value), RE_MATERIAL_LOADER (wrap 0xf9530, rax),
               RE_MATERIAL_BIND (entry 0xf8c90): texture-binding.md T7.7, T8.9, A.7, T11.6
  srt-chunk    RE_SRT_CHUNK (entry 0x862490): T12.7 item 1, vertex-formats.md T4.10 item 3
  srt-mask     RE_SRT_MASK (entry 0x95320): vertex-formats.md T4.10 item 2
  refresh      RE_REFRESH_F8D10 (wrap 0xf8d10), RE_REFRESH_F9C40 (wrap 0xf9c40): T8.9 item 4, A.7 item 5
  skin         RE_SKIN_PALETTE (0x4938e0), RE_SKIN_QUEUE (0x493bc0), RE_SKIN_SCRATCH (wrap 0x123330),
               RE_SKIN_DISPATCH (entry 0x193810), RE_SKIN_STORE (site 0x10c45f): vertex-formats.md T5.11, T14.8
  pass         RE_PASS_FLIP (entry 0x90860), RE_PASS_MODE (entry 0xa1cd0), RE_PASS_TABLE (entry 0x9fba0),
               RE_PASS_MRT (entry 0x92140): pass-targets.md K6.9 items 2-5 and 7

Signatures: tools/re/eboot_re.py sig ADDR; checked with tools/re/hooks_check.py. The EU 1.01 expected offsets resolve
by the fingerprint; other builds scan the pattern, which must then match exactly once.

RE_SKIN_STORE: the same instruction sequence also sits at 0x10c83f (in 0x10c6c0), so its pattern is not unique on
its own; the EU expected offset resolves it, and a build without that fingerprint leaves it off.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'


def _hook(name, group, mode, pattern, address, **extra):
    """One hook of the probe: switch SOTC_RE_PROBE, its own group, the EU expected offset."""
    record = dict(name=name, switch='SOTC_RE_PROBE', group=group, mode=mode, pattern=pattern,
                  expected={EU_FINGERPRINT: address})
    record.update(extra)
    return record


HOOKS = [
    # Material. The factory build 0xf9390 returns the material (rbx) on success; the others are fallbacks.
    _hook('RE_MATERIAL_FACTORY', 're_factory', 'wrap',
          '55 48 89 e5 41 57 41 56 53 50 49 89 f6 49 89 ff 49 8b 5e 08 48 85 db 75 ?? bf 38 06', 0xf9390),
    _hook('RE_MATERIAL_LOADER', 're_loader', 'wrap',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 4c 8b 35 ?? ?? ?? ?? 49 89 ff 48 8d', 0xf9530),
    _hook('RE_MATERIAL_BIND', 're_bind', 'entry',
          '55 48 89 e5 41 57 41 56 53 50 49 89 f6 48 89 fb 49 bf 00 00', 0xf8c90),

    # SRT chunk and mask.
    _hook('RE_SRT_CHUNK', 're_srt_chunk', 'entry',
          '55 48 89 e5 41 57 41 56 53 50 49 89 ff 48 63 de', 0x862490),
    _hook('RE_SRT_MASK', 're_srt_mask', 'entry',
          '55 48 89 e5 41 56 53 89 fb 89 d0 49', 0x95320),

    # Refresh routines (wrap: the T# words are compared after the call).
    _hook('RE_REFRESH_F8D10', 're_refresh_f8d10', 'wrap',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ?? 49 89 fc 48 8b 00 48 89 '
          '45 d0 48 8b 46 08 48 89', 0xf8d10),
    _hook('RE_REFRESH_F9C40', 're_refresh_f9c40', 'wrap',
          '48 8b 87 b0 05 00 00 85', 0xf9c40),

    # Skinning.
    _hook('RE_SKIN_PALETTE', 're_skin_palette', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 89 75 d0 b9 01 00', 0x4938e0),
    _hook('RE_SKIN_QUEUE', 're_skin_queue', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 4c 8b 3d ?? ?? ?? ?? 48 8d 7d b0 48',
          0x493bc0),
    _hook('RE_SKIN_SCRATCH', 're_skin_scratch', 'wrap',
          '55 41 57 41 56 41 55 41 54 53 48 89 7c 24 f8 48', 0x123330),
    _hook('RE_SKIN_DISPATCH', 're_skin_dispatch', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 48 8b 05 ?? ?? ?? ?? 48 89 8d b8 fe ff ff '
          '48 8d', 0x193810),
    # The store of holder+0x60 to P+0x90 (mov [r14+0x90],rax) in 0x10c2e0. Site: the instruction is the first of the
    # pattern. Expected offset for EU; the same sequence also sits at 0x10c83f, so a scan alone would not be unique.
    _hook('RE_SKIN_STORE', 're_skin_store', 'site',
          '49 89 86 90 00 00 00 e8 ?? ?? ?? ?? 48 8b 7b 20 e8 ?? ?? ?? ?? 48 89 c3', 0x10c45f),

    # Pass targets (K6.9).
    _hook('RE_PASS_FLIP', 're_pass_flip', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 40 01 00 00 4c 8b 3d ??', 0x90860),
    _hook('RE_PASS_MODE', 're_pass_mode', 'entry',
          '55 48 89 e5 53 50 48 89 fb 39 b3 70', 0xa1cd0),
    _hook('RE_PASS_TABLE', 're_pass_table', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 8b 00 48 89 45 d0 '
          '48 c7', 0x9fba0),
    _hook('RE_PASS_MRT', 're_pass_mrt', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ?? 49 89 fc 48 8b 00 48 89 45 d0 '
          '48 8b 06 48 85 c0', 0x92140),

    # Skin V# writers (entry; the index base is in the pattern: add 0x11, 0x21 or 0x41, see probe_re.cpp).
    _hook('RE_SKIN_VSHARP_IN', 're_skin_vsharp_in', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 50 41 89 d5 49 89 ce 49 89 ff 45 85 ed 0f 8e ?? ?? ?? ?? '
          '4d 8b a7 a8 18 00 00 48 63 de 41 ff cd 48 83 c3 11 4c 89 e0 eb ?? 66 2e 0f 1f 84 00 00 00 00 00 '
          '49 8b 87 a8 18 00 00 49 83 c6 10 48', 0x85e8e0),
    _hook('RE_SKIN_VSHARP_OUT', 're_skin_vsharp_out', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 50 41 89 d5 49 89 ce 49 89 ff 45 85 ed 0f 8e ?? ?? ?? ?? '
          '4d 8b a7 a8 18 00 00 48 63 de 41 ff cd 48 83 c3 21 4c 89 e0 eb ?? 66 2e 0f 1f 84 00 00 00 00 00 '
          '49 8b 87 a8 18 00 00 49 83 c6 10 48', 0x85e9a0),
    _hook('RE_SKIN_VSHARP_EXTRA', 're_skin_vsharp_extra', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 50 41 89 d5 49 89 ce 49 89 ff 45 85 ed 0f 8e ?? ?? ?? ?? '
          '4d 8b a7 a8 18 00 00 48 63 de 41 ff cd 48 83 c3 41 4c 89 e0', 0x85e820),

    # K6.9 item 1 (EndFrameViewParams, F = rdi) and item 6 (decal MRT bind 0xc28c0, entry arms the port probe).
    _hook('RE_PASS_VIEW', 're_pass_view', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 20 0f 00 00', 0xcefa0),
    _hook('RE_DECAL_ARM', 're_decal_arm', 'entry',
          '55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec f8 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 fb 4c 8d', 0xc28c0),
]
