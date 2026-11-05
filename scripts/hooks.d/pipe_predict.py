# SPDX-License-Identifier: GPL-2.0-or-later
"""Pipeline prediction from the game's materials (gpu/sotc/pipe_predict.cpp, switch SOTC_PIPE_PREDICT=1).

PIPE_PREDICT_MATERIAL: wrap of the material factory build FUN_000f9390 (0xf9390). It calls the material parser
  0xf6c90, which fills the 14 pass slots (material + 0x60 + 0x48*k), and returns the material in rax on success.
  The post phase queues that pointer; the command processor reads its slots (VS and PS program hashes per pass)
  and builds each pass's pipeline before the material's first draw
  (docs/re-stutters/native/re-pipelines/README.md, step 3c).

Same signature as RE_MATERIAL_FACTORY (scripts/hooks.d/re.py): with SOTC_RE_PROBE on as well, the later one is
refused (the same bytes are displaced).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PIPE_PREDICT_MATERIAL',
        switch='SOTC_PIPE_PREDICT',
        group='pipe_predict_material',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; mov r14,rsi; mov r15,rdi; mov rbx,[r14+8];
        # test rbx,rbx; jne short; mov edi,0x638
        pattern='55 48 89 e5 41 57 41 56 53 50 49 89 f6 49 89 ff 49 8b 5e 08 48 85 db 75 ?? bf 38 06',
        expected={EU_FINGERPRINT: 0xf9390},
    ),
]
