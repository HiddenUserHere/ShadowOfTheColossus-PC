#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Packs every _shipping.csdr (RDSC) shader file of the game's PSARC archives into one input file for the port's
extract mode (gpu/sotc/probe_csdr.cpp, SOTC_CSDR_EXTRACT).

    python tools/csdr/pack_csdr.py GAME_DIR OUT.bin [--all]

Record: u32 name_len, name, u32 descriptor (type << 29 | raw size, from the file's trailer), u32 compressed size,
the data without the trailer. Header: b'CSDRIN01', u32 count. --all keeps the non-shipping (debug) files too.
The output is derived from the game's files: keep it outside the repository (e.g. in the user folder).

Then run the game once with the extract mode (it decompresses with the game's own Oodle after the game initialized,
writes OUT, and exits):
    SOTC_CSDR_PROBE=1 SOTC_CSDR_EXTRACT=IN.bin SOTC_CSDR_EXTRACT_OUT=OUT.bin python sotc.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psarc import Archive, trailer  # noqa: E402

ARCHIVES = ('cachedFiles.psarc', 'pak0.psarc', 'pak1.psarc', 'pakN.psarc')


def main():
    game, out_path = sys.argv[1], sys.argv[2]
    shipping_only = '--all' not in sys.argv
    records = []
    for name in ARCHIVES:
        path = os.path.join(game, name)
        if not os.path.isfile(path):
            continue
        archive = Archive(path)
        found = 0
        for file_name, entry in archive.by_name.items():
            if not file_name.endswith(b'.csdr') or (shipping_only and not file_name.endswith(b'_shipping.csdr')):
                continue
            data = archive.read(entry)
            t = trailer(data)
            if not t or t[0] != b'RDSC':
                continue
            records.append((file_name, t[2], data[:-12]))
            found += 1
        print(f'{name}: {found} shader files', flush=True)
    with open(out_path, 'wb') as o:
        o.write(b'CSDRIN01' + struct.pack('<I', len(records)))
        for file_name, desc, data in records:
            o.write(struct.pack('<I', len(file_name)) + file_name + struct.pack('<II', desc, len(data)) + data)
    raw = sum(d & 0x1fffffff for _, d, _ in records)
    print(f'{len(records)} records, {raw / 1e6:.1f} MB raw -> {out_path}')


if __name__ == '__main__':
    main()
