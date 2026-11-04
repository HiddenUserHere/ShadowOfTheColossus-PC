#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reader for the game's PSARC archives (PSAR 1.4, zlib blocks), stdlib only.

    python tools/csdr/psarc.py ARCHIVE.psarc [--list]

Layout: a 32-byte big-endian header (b'PSAR', major, minor, compression, toc size, entry size 30, entry count,
block size, flags), the entries (md5 of the name 16, first block index u32, size u40, offset u40), then the block
size table (u16 when the block size is 64 KiB; 0 = a full raw block, equal to the chunk = raw, else zlib).
Entry 0 is the manifest: NUL-separated names. The entries are NOT in manifest order: a name's entry is the one whose
md5 field is md5(name).

Every file of Shadow of the Colossus ends with a 12-byte trailer: a reversed FourCC (b'RDSC' shader, b'STAM'
material, b'RTXT' / b'CTXT' texture, b'HSEM' mesh, b'LDOM' model, ...), a u32 version and a u32 descriptor
(type << 29 | raw size; type 0 = stored, 2-5 = Oodle, decompressed by the game's own OodleLZ_Decompress).
"""
import hashlib
import struct
import sys
import zlib

HEADER_SIZE = 32


class Archive:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(HEADER_SIZE)
        if h[:4] != b'PSAR':
            raise ValueError(f'{path}: not a PSAR archive')
        toc_size, self.entry_size, count, self.block_size, _flags = struct.unpack('>IIIII', h[12:32])
        toc = self.f.read(toc_size - HEADER_SIZE)
        self.entries = []
        for i in range(count):
            e = toc[i * self.entry_size:(i + 1) * self.entry_size]
            self.entries.append(dict(index=i, md5=e[0:16].hex(), zidx=struct.unpack('>I', e[16:20])[0],
                                     size=int.from_bytes(e[20:25], 'big'), offset=int.from_bytes(e[25:30], 'big')))
        table = toc[count * self.entry_size:]
        width = 2 if self.block_size <= 0x10000 else 4
        self.blocks = list(struct.unpack('>%d%s' % (len(table) // width, 'H' if width == 2 else 'I'),
                                         table[:len(table) // width * width]))
        manifest = self.read(self.entries[0])
        sep = b'\n' if manifest.count(b'\n') > manifest.count(b'\0') else b'\0'
        self.names = [n for n in manifest.split(sep) if n]
        by_md5 = {e['md5']: e for e in self.entries[1:]}
        self.by_name = {}
        for n in self.names:
            e = by_md5.get(hashlib.md5(n).hexdigest())
            if e is not None:
                self.by_name[n] = e

    def read(self, entry):
        out = bytearray()
        remaining = entry['size']
        block = entry['zidx']
        self.f.seek(entry['offset'])
        while remaining > 0:
            chunk = min(self.block_size, remaining)
            stored = self.blocks[block] or self.block_size
            block += 1
            raw = self.f.read(stored)
            out += raw[:chunk] if stored == chunk or (stored >= self.block_size and chunk == self.block_size) \
                else zlib.decompress(raw)[:chunk]
            remaining -= chunk
        return bytes(out)

    def read_name(self, name):
        return self.read(self.by_name[name])


def trailer(data):
    """(fourcc, version, descriptor) of a game file, or None when it is too short."""
    if len(data) < 12:
        return None
    return struct.unpack('<4sII', data[-12:])


if __name__ == '__main__':
    archive = Archive(sys.argv[1])
    print(f'{len(archive.entries) - 1} entries, {len(archive.names)} names, {len(archive.by_name)} mapped')
    if '--list' in sys.argv:
        for name, e in archive.by_name.items():
            print(f"{e['size']}\t{name.decode('utf-8', 'replace')}")
