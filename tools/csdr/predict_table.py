#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prediction table for SOTC_PREDICT_PROBE (gpu/sotc/predict_probe.cpp), from the extract mode's output pack.

    python tools/csdr/predict_table.py CSDR_OUT.bin TABLE.tsv

One line per GCN program: footer hash (16 hex digits), footer type (0 pixel, 1 vertex, 4 compute; 2, 5-8 the
geometry and tessellation stages, verified from the machine code), pass, number of files holding the program.
pass = the .csdr file stem without the trailing _<n>_shipping (e.g. gbufferbakedambient, depth, emitter_0_update);
a program in several passes takes the most frequent one.

Program layout in a decompressed .csdr: each program starts at the marker dword 0xBEEB03FF (s_mov_b32 vcc_hi, imm)
and ends with a 32-byte "OrbShdr" footer (regs_shader.h BinaryInfo: u32 bitfield at +8 with type in bits 2-5 and the
code length in bits 8-31, u64 hash at +16).
"""
import collections
import re
import struct
import sys


def main():
    data = open(sys.argv[1], 'rb').read()
    if data[:8] != b'CSDROUT1':
        sys.exit(f'{sys.argv[1]}: not a CSDROUT1 pack')
    p = 12
    passes = collections.defaultdict(collections.Counter)
    types = {}
    while p < len(data):
        n = struct.unpack_from('<I', data, p)[0]
        p += 4
        name = data[p:p + n].decode('utf-8', 'replace')
        p += n
        size = struct.unpack_from('<I', data, p)[0]
        p += 4
        raw = data[p:p + size]
        p += size
        stem = name.rsplit('/', 1)[-1]
        stem = re.sub(r'(_[0-9a-f]+)?_shipping\.csdr$', '', stem)
        stem = re.sub(r'_\d+$', '', stem)
        for m in re.finditer(b'OrbShdr', raw):
            h = m.start()
            if h + 32 > len(raw):
                continue
            bits = struct.unpack_from('<I', raw, h + 8)[0]
            program = struct.unpack_from('<Q', raw, h + 16)[0]
            types[program] = (bits >> 2) & 0xF
            passes[program][stem] += 1
    with open(sys.argv[2], 'w', encoding='utf-8') as o:
        for program, counter in passes.items():
            stem, _ = counter.most_common(1)[0]
            o.write('%016x\t%d\t%s\t%d\n' % (program, types[program], stem.replace('\t', ' '), sum(counter.values())))
    print(f'{len(passes)} programs -> {sys.argv[2]}')


if __name__ == '__main__':
    main()
