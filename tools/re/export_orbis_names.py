"""Export the eboot's import seam: PLT stub -> JMPREL index -> GOT slot -> NID -> symbol name.

This is the authoritative Orbis symbol map for the render rewrite: it names every import the
guest can reach through the PLT (the seam that carries GNM draw/dispatch traffic into the port).
It reads only the user's own decrypted eboot ELF (out/eboot.elf) and the port's NID table
(src/import_names.inc); it writes a text map and a JSON map under out/re/orbis/.

Usage: python -I tools/re/export_orbis_names.py [eboot.elf] [out_dir]
"""
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # repo root
EBOOT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'out' / 'eboot.elf'
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'out' / 're' / 'orbis'

# Orbis module segment (PT_SCE_MODULE_INFO, type 0x61000000) holds the dynamic tables.
# Its file offset is discovered below; dynamic tag values are relative to this blob.
TAG_STRTAB = 0x61000035
TAG_STRSZ = 0x61000037
TAG_SYMTAB = 0x61000039
TAG_SYMTABSZ = 0x6100003f
TAG_JMPREL = 0x61000029
TAG_PLTRELSZ = 0x6100002d  # 0x6100002b is PLTREL (enum DT_RELA=7), not the size
SYMENT = 24
RELAENT = 24


def load_port_nids():
    """NID -> symbol name from the port's own table (diagnostics + contract names)."""
    text = (ROOT / 'src' / 'import_names.inc').read_text()
    return dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', text))


def main():
    data = EBOOT.read_bytes()
    if data[:4] != b'\x7fELF':
        raise SystemExit('not an ELF')
    e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x36)[0]
    e_phnum = struct.unpack_from('<H', data, 0x38)[0]
    ph = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align) = \
            struct.unpack_from('<IIQQQQQQ', data, o)
        ph.append(dict(type=p_type, offset=p_offset, vaddr=p_vaddr, filesz=p_filesz))
    dyn = next(p for p in ph if p['type'] == 2)
    tags = {}
    for pos in range(dyn['offset'], dyn['offset'] + dyn['filesz'], 16):
        tag, val = struct.unpack_from('<QQ', data, pos)
        if tag == 0:
            break
        tags[tag] = val
    lib = next(p for p in ph if p['type'] == 0x61000000)
    blob = lib['offset']  # dynamic tag values are blob-relative

    strtab = blob + tags[TAG_STRTAB]
    strsz = tags[TAG_STRSZ]
    symtab = blob + tags[TAG_SYMTAB]
    nsyms = tags[TAG_SYMTABSZ] // SYMENT
    jmprel = blob + tags[TAG_JMPREL]
    nplt = tags[TAG_PLTRELSZ] // RELAENT

    def string(off):
        end = data.index(b'\x00', strtab + off)
        return data[strtab + off:end].decode('latin1')

    def sym(i):
        o = symtab + i * SYMENT
        st_name = struct.unpack_from('<I', data, o)[0]
        return string(st_name)

    nids = load_port_nids()
    rows = []
    for idx in range(nplt):
        o = jmprel + idx * RELAENT
        r_offset, r_info = struct.unpack_from('<QQ', data, o)
        symi = r_info >> 32
        scoped = sym(symi)
        nid = scoped.split('#')[0]
        rows.append(dict(plt_index=idx, got=r_offset, sym_index=symi, nid=nid,
                         scoped=scoped, symbol=nids.get(nid)))

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'names_orbis.json').write_text(json.dumps(rows, indent=1))
    with (OUT / 'names_orbis.txt').open('w') as f:
        f.write('# PLT index -> GOT slot -> NID -> symbol (eboot import seam)\n')
        f.write(f'# {len(rows)} entries from {EBOOT.name}\n')
        for r in rows:
            f.write('plt[%#05x] got=%#010x sym=%3d %-11s %s\n' %
                    (r['plt_index'], r['got'], r['sym_index'], r['nid'], r['symbol'] or '(unknown)'))
    named = sum(1 for r in rows if r['symbol'])
    print(f'wrote {OUT / "names_orbis.txt"} ({len(rows)} entries, {named} named, '
          f'{len(rows) - named} unknown)')


if __name__ == '__main__':
    main()
