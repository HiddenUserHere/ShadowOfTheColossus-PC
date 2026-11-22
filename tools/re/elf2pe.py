"""Wraps the PS4 eboot (ELF, e_type 0xfe10) in a PE32+ image for REA's Windows Ghidra provider (it accepts PE only).

Same virtual addresses as the ELF (ImageBase 0, section RVA = segment VA). The first page of the text segment (VA 0..0xfff)
is dropped because a PE section cannot start at RVA 0 (the headers live there). R_X86_64_RELATIVE relocations are applied with
base 0 so data pointers and vtables resolve. Analysis copy only: never placed in the repository.
usage: python -I elf2pe.py <eboot.elf> <out.exe>
"""
import struct
import sys

SECT_ALIGN = 0x1000
FILE_ALIGN = 0x200


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    e_entry, e_phoff = struct.unpack_from('<QQ', d, 24)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 54)
    phs = [struct.unpack_from('<IIQQQQQQ', d, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    loads = [p for p in phs if p[0] in (1, 0x61000010)]
    dyn = next((p for p in phs if p[0] == 2), None)
    dynlib = next((p for p in phs if p[0] == 0x61000000), None)

    # Memory image of the loadable segments.
    top = max(p[3] + p[6] for p in loads)
    mem = bytearray(top)
    for p in loads:
        _, _, off, va, _, filesz, memsz, _ = p
        mem[va:va + filesz] = d[off:off + filesz]

    # R_X86_64_RELATIVE (type 8) from the SCE rela table: value = base (0) + addend.
    applied = 0
    if dyn and dynlib:
        tags = {}
        for i in range(dyn[5] // 16):
            tag, val = struct.unpack_from('<qQ', d, dyn[2] + i * 16)
            if tag == 0:
                break
            tags[tag] = val
        rela_off = tags.get(0x6100002f)
        rela_sz = tags.get(0x61000031, 0)
        if rela_off is not None:
            base = dynlib[2] + rela_off
            for i in range(rela_sz // 24):
                r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, base + i * 24)
                if (r_info & 0xffffffff) == 8 and r_offset + 8 <= len(mem):
                    struct.pack_into('<Q', mem, r_offset, r_addend & 0xffffffffffffffff)
                    applied += 1

    sections = []
    for p in loads:
        _, flags, _, va, _, filesz, memsz, _ = p
        start = va if va >= SECT_ALIGN else SECT_ALIGN
        end = va + memsz
        raw_end = va + filesz
        name = b'.text' if flags & 1 else (b'.rdata' if not flags & 2 else b'.data')
        chars = 0x40000040  # initialised data, readable
        if flags & 1:
            chars = 0x60000020  # code, execute, read
        if flags & 2:
            chars |= 0x80000000
        sections.append((name, start, end - start, mem[start:max(start, raw_end)], chars))

    n = len(sections)
    hdr_size = align(0x40 + 4 + 20 + 240 + 40 * n, FILE_ALIGN)
    out = bytearray(hdr_size)
    struct.pack_into('<2s58xI', out, 0, b'MZ', 0x40)
    out[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', out, 0x44, 0x8664, n, 0, 0, 0, 240, 0x22)
    size_image = align(max(s[1] + s[2] for s in sections), SECT_ALIGN)
    opt = 0x58
    struct.pack_into('<HBBIIIII', out, opt, 0x20b, 14, 0, 0, 0, 0, e_entry, SECT_ALIGN)
    struct.pack_into('<QIIHHHHHHIIIIHHQQQQII', out, opt + 24, 0, SECT_ALIGN, FILE_ALIGN, 6, 0, 0, 0, 6, 0, 0,
                     size_image, hdr_size, 0, 3, 0x8160, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    sh = opt + 240
    raw_ptr = hdr_size
    blobs = []
    for i, (name, rva, vsize, raw, chars) in enumerate(sections):
        raw_size = align(len(raw), FILE_ALIGN)
        struct.pack_into('<8sIIIIIIHHI', out, sh + 40 * i, name.ljust(8, b'\0'), vsize, rva, raw_size, raw_ptr, 0, 0, 0, 0,
                         chars)
        blobs.append(bytes(raw) + bytes(raw_size - len(raw)))
        raw_ptr += raw_size
    with open(dst, 'wb') as f:
        f.write(out)
        for b in blobs:
            f.write(b)
    print(f'wrote {dst}: {n} sections, entry {e_entry:#x}, {applied} relative relocations applied')


if __name__ == '__main__':
    main()
