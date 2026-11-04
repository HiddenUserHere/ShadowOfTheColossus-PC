#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Find how eboot functions with no direct caller are reached (topic T6).

Works on out/eboot.elf with the index built by eboot_re.py (out/re/index.pkl.gz). For each target it reports:
- absolute 8-byte pointers to the target in non-code segments (vtables, callback and function-pointer tables),
  with the run of neighbouring function pointers (vtable base and slot index) and the code that references the base;
- 4-byte field-relative offsets (target = field address + s32), the engine's relative-pointer convention;
- 4-byte table-base offsets (target = base + s32[k]) for tables whose base is the target of a RIP-relative
  reference from code (classic switch / jump tables);
- raw 4-byte matches of the target inside code (absolute immediates; not decoded, so noisy);
- RIP-relative references and direct calls (the index);
- indirect call/jmp sites `call [reg+disp]` whose displacement equals 8*slot of a vtable found above;
- writes `mov [reg+disp], x` to the offsets given in WRITE_DISPS (object fields of the graphics context).

The capstone pass over all functions is cached in out/re/indirect_scan.pkl.gz (delete it to rescan).

usage:
  python tools/re/indirect_refs.py [--rescan] [--extra ADDR ...] [--max N]

Addresses are eboot virtual addresses. Output is plain text on stdout.
"""
import argparse
import bisect
import collections
import gzip
import pickle
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eboot_re as E  # noqa: E402  (Image, Index, ROOT)
import capstone  # noqa: E402

SCAN = E.ROOT / 'out' / 're' / 'indirect_scan.pkl.gz'

TARGETS = {
    # T4: O+0xc130 / O+0xc168 setter and the mirror-layout stubs; item builders
    0x8615f0: 'T4 setter O+0xc130/0xc168',
    0x861730: 'T4 mirror layout (O+0xc0d8 path)',
    0x861880: 'T4 mirror layout (O+0xc0d8 path)',
    0x103ad0: 'T4 item builder (object mesh)',
    0x0db700: 'T4 item builder (indirect draw caller)',
    0x0caed0: 'T4 item builder (pass runner)',
    0x1b0980: 'T4 item builder',
    0x0aea10: 'T4 item builder',
    0x1051f0: 'T4 item builder',
    # D2: VGT_INSTANCE_STEP_RATE writer and generic setters
    0x86bb90: 'D2 VGT_INSTANCE_STEP_RATE writer',
    0x86bb30: 'D2 neighbour of step-rate writer',
    0x869350: 'D2 generic emitter',
    0x8694e0: 'D2 generic setter',
    0x869370: 'D2 generic emitter',
    # K2: GS and HS setters
    0x860c50: 'K2 GS setter',
    0x860ea0: 'K2 GS setter',
    0x8612b0: 'K2 HS setter',
    0x8613e0: 'K2 HS setter',
    # R8/K1: 0x54-byte draw packet builder
    0x862c50: 'R8/K1 draw packet builder (commit)',
}

# object-field offsets of the graphics context O (see slot-stages.md, vertex-formats.md T4)
WRITE_DISPS = {0xc0e8, 0xc108, 0xc130, 0xc168}
# stores in this displacement range are recorded (covers the 8-byte fields and any block copy that starts below them)
WRITE_LO, WRITE_HI = 0xc0c0, 0xc118

MEM = re.compile(r'\[(\w+)(?: ([+-]) 0x([0-9a-f]+))?\]')
PTR_RE = re.compile(r'\[(\w+)(?: ([+-]) 0x([0-9a-f]+))?\]')


def mem_disp(m):
    if not m or m.group(1) == 'rip':
        return None
    d = int(m.group(3), 16) if m.group(3) else 0
    return -d if m.group(2) == '-' else d


def scan_code(img, funcs):
    """One capstone pass: indirect calls/jumps with [reg+disp], writes to WRITE_DISPS, and register calls."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    indirect = []   # (site, func_start, mnem, base_reg, disp)
    writes = []     # (site, func_start, dest_operand, source_operand)
    reg_calls = collections.Counter()  # func_start -> number of "call reg" sites
    absbases = []   # (site, func_start, value, ops): absolute displacement operands (table bases without RIP)
    dispatch = []   # (site, func_start, mnem, ops): call/jmp through an indexed memory operand
    for n, (start, end) in enumerate(funcs):
        if n % 10000 == 0:
            print(f'  scan {n}/{len(funcs)}', file=sys.stderr)
        code = img.read(start, end - start)
        for addr, size, mnem, ops in md.disasm_lite(code, start):
            if '[' in ops and 'rip' not in ops:
                if MEM.search(ops) is None or '*' in ops:
                    for inner in re.findall(r'\[([^\]]*)\]', ops):
                        for hx in re.findall(r'0x([0-9a-f]+)', inner):
                            v = int(hx, 16)
                            if v > 0x10000:
                                absbases.append((addr, start, v, ops))
            if mnem in ('call', 'jmp'):
                if ops.startswith('0x'):
                    continue
                if '*' in ops:
                    dispatch.append((addr, start, mnem, ops))
                m = MEM.search(ops)
                if m:
                    if m.group(1) != 'rip':
                        indirect.append((addr, start, mnem, m.group(1), mem_disp(m)))
                elif mnem == 'call':
                    reg_calls[start] += 1
            elif '[' in ops and mnem not in ('cmp', 'test', 'lea', 'push', 'call', 'jmp', 'nop')                     and not mnem.startswith('j') and ',' in ops and ops.split(',', 1)[0].count('[') == 1                     and ('mov' in mnem or mnem.startswith('v') or mnem in ('add', 'sub', 'or', 'and', 'xor', 'stos')):
                # any store-like instruction whose destination is [reg+disp] (mov, vmovq, movups, add, ...)
                parts = ops.split(',', 1)
                m = MEM.search(parts[0])
                d = mem_disp(m)
                if d is not None and WRITE_LO <= d <= WRITE_HI:
                    writes.append((addr, start, parts[0].strip(), parts[1].strip() if len(parts) > 1 else ''))
    return {'indirect': indirect, 'writes': writes, 'reg_calls': dict(reg_calls),
            'absbases': absbases, 'dispatch': dispatch}


def data_segments(img):
    """Non-executable segments (.data, .data.rel.ro)."""
    return [(v, fs, off) for v, fs, off, fl in img.segs if not fl & 1 and fs]


def readonly_segments(img):
    """Every segment with file bytes, including the R+X segment that holds .rodata (scanned as data too)."""
    return [(v, fs, off) for v, fs, off, fl in img.segs if fs]


def code_segments(img):
    return [(v, fs, off) for v, fs, off, fl in img.segs if fl & 1 and fs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rescan', action='store_true', help='ignore the cached capstone pass')
    ap.add_argument('--extra', nargs='*', default=[], help='extra target addresses (hex)')
    ap.add_argument('--max', type=int, default=25, help='max sites printed per list')
    args = ap.parse_args()

    targets = dict(TARGETS)
    for a in args.extra:
        targets[int(a, 16)] = 'extra'

    img = E.Image()
    ix = E.Index()
    funcs = ix.funcs
    fstarts = ix.fstarts
    fstart_set = set(fstarts)

    if SCAN.exists() and not args.rescan:
        with gzip.open(SCAN, 'rb') as f:
            scan = pickle.load(f)
    else:
        print('capstone pass over all functions (cached afterwards)', file=sys.stderr)
        scan = scan_code(img, funcs)
        with gzip.open(SCAN, 'wb') as f:
            pickle.dump(scan, f, protocol=pickle.HIGHEST_PROTOCOL)

    # Writers of the WRITE_DISPS fields become targets too (the brief asks who writes them).
    writer_funcs = {}
    for site, fs, dst, src in scan['writes']:
        writer_funcs.setdefault(fs, []).append((site, dst, src))
    for fs in writer_funcs:
        targets.setdefault(fs, 'writer of O+0xc0e8/0xc108/0xc130/0xc168')

    # Reference indexes
    refs_to = collections.defaultdict(list)           # target -> [site] (RIP-relative data or code)
    for site, t in ix.refs:
        refs_to[t].append(site)
    calls_to = collections.defaultdict(list)
    for site, t, k in ix.calls:
        calls_to[t].append((site, k))

    # Data pointer map: every 8-byte aligned value in data segments that is a function start or target.
    allstarts = fstart_set | set(targets)
    ptr_at = {}        # slot address -> value (only values that are function starts)
    abs_hits = collections.defaultdict(list)   # target -> [slot]
    rel_hits = collections.defaultdict(list)   # target -> [(field, s32)]
    for v, fs, off in readonly_segments(img):
        blob = img.data[off:off + fs]
        for i in range(0, len(blob) - 7, 8):
            val = struct.unpack_from('<Q', blob, i)[0]
            if val in allstarts:
                ptr_at[v + i] = val
                if val in targets:
                    abs_hits[val].append(v + i)
        for i in range(0, len(blob) - 3, 4):
            w = struct.unpack_from('<i', blob, i)[0]
            t = v + i + w
            if t in targets:
                rel_hits[t].append((v + i, w))
    # reloc values (kind 0) are stored as final values in the file; include any that the file scan missed
    for t, slots_ in list(abs_hits.items()):
        abs_hits[t] = sorted(set(slots_))

    # Table-base relative: bases are targets of RIP-relative references from code into data.
    dsegs = readonly_segments(img)
    def in_data(a):
        return any(v <= a < v + fs for v, fs, _ in dsegs)
    abs_bases = {v for _, _, v, _ in scan.get('absbases', []) if in_data(v)}
    bases = sorted({t for _, t in ix.refs if in_data(t)} | abs_bases)
    print(f'table bases: {len(bases)} (RIP-relative {len({t for _, t in ix.refs if in_data(t)})}, '
          f'absolute-operand {len(abs_bases)})')
    tbl_hits = collections.defaultdict(list)   # target -> [(base, k, s32)]
    for b in bases:
        blob = img.read(b, 512 * 4)
        if len(blob) < 8:
            continue
        for k in range(len(blob) // 4):
            w = struct.unpack_from('<i', blob, 4 * k)[0]
            t = b + w
            if t in targets and w != 0:
                tbl_hits[t].append((b, k, w))

    # Raw absolute 4-byte and 8-byte matches in code (not decoded).
    raw_hits = collections.defaultdict(list)
    for v, fs, off in code_segments(img):
        blob = img.data[off:off + fs]
        for t in targets:
            p4 = struct.pack('<I', t)
            p8 = struct.pack('<Q', t)
            for pat, kind in ((p8, 'imm64'), (p4, 'imm32')):
                i = blob.find(pat)
                while i >= 0:
                    raw_hits[t].append((v + i, kind))
                    i = blob.find(pat, i + 1)

    # Vtable runs: contiguous function pointers around each absolute hit.
    def run_of(slot):
        base = slot
        while ptr_at.get(base - 8) is not None:
            base -= 8
        end = slot
        while ptr_at.get(end + 8) is not None:
            end += 8
        return base, (end - base) // 8 + 1

    # Data holders of a table base (a struct member or global that stores the table address).
    run_bases = set()
    for t in targets:
        for slot in abs_hits.get(t, []):
            run_bases.add(run_of(slot)[0])
    holders = collections.defaultdict(list)
    if run_bases:
        for v, fs, off in readonly_segments(img):
            blob = img.data[off:off + fs]
            for i in range(0, len(blob) - 7, 8):
                val = struct.unpack_from('<Q', blob, i)[0]
                if val in run_bases:
                    holders[val].append(v + i)

    # Vtable base -> code that references it (constructors store it at object+0).
    # Indirect call sites by displacement.
    by_disp = collections.defaultdict(list)
    for site, fs, mnem, reg, disp in scan['indirect']:
        by_disp[disp].append((site, fs, mnem, reg))

    def fname(a):
        return ix.name(a)

    def show_sites(sites, limit):
        out = []
        for s in sites[:limit]:
            out.append(f'      {s[0]:#x} in {fname(s[0])}')
        if len(sites) > limit:
            out.append(f'      ... {len(sites) - limit} more')
        return '\n'.join(out) if out else '      (none)'

    print('# indirect_refs.py output')
    print(f'targets: {len(targets)}; functions: {len(funcs)}; abs-ptr functions: {len(ptr_at)}; '
          f'table bases from code: {len(bases)}; indirect call/jmp [reg+disp] sites: {len(scan["indirect"])}; '
          f'writes to {sorted(hex(d) for d in WRITE_DISPS)}: {len(scan["writes"])}')
    print()

    for t in sorted(targets):
        print(f'== {t:#x} {fname(t)}  ({targets[t]})')
        rr = refs_to.get(t, [])
        cc = calls_to.get(t, [])
        print(f'  direct calls and tail jumps: {len(cc)}; RIP-relative refs to it: {len(rr)}')
        for s, k in cc[:args.max]:
            print(f'    {"call" if k == "c" else "tail-jmp"} at {s:#x} in {fname(s)}')
        for s in rr[:args.max]:
            print(f'    ref {s:#x} in {fname(s)}')
        print(f'  absolute 8-byte pointers in data: {len(abs_hits.get(t, []))}')
        for slot in abs_hits.get(t, [])[:args.max]:
            base, n = run_of(slot)
            owners = refs_to.get(base, [])
            kind = 'vtable/table run' if n > 1 else 'single pointer'
            print(f'    slot {slot:#x} run base {base:#x} len {n} index {(slot - base) // 8} ({kind}); '
                  f'code refs to base: {len(owners)}')
            for s in owners[:6]:
                print(f'      base referenced at {s:#x} in {fname(s)}')
            if not owners:
                print(f'      no code reference to the base; data holders: {len(holders.get(base, []))}')
                for h in holders.get(base, [])[:6]:
                    print(f'        data pointer to base at {h:#x}')
        print(f'  field-relative s32 (target = field + s32): {len(rel_hits.get(t, []))}')
        for field, w in rel_hits.get(t, [])[:args.max]:
            print(f'    field {field:#x} s32 {w:#x}')
        print(f'  table-base s32 (target = base + s32[k]): {len(tbl_hits.get(t, []))}')
        for b, k, w in tbl_hits.get(t, [])[:args.max]:
            print(f'    base {b:#x} entry {k} s32 {w:#x}')
        print(f'  raw code matches (undecoded): {len(raw_hits.get(t, []))}')
        for a, kind in raw_hits.get(t, [])[:args.max]:
            print(f'    {a:#x} {kind} in {fname(a)}')
        print()

    # Writers of the object fields
    print(f'== stores with displacement {WRITE_LO:#x}..{WRITE_HI:#x} (object fields of O)')
    for site, fs, dst, src in sorted(scan['writes']):
        print(f'  {site:#x} in {fname(fs)}: {dst} <- {src}')
    print()

    # Indirect call sites whose displacement equals a vtable slot found for a target.
    print('== indirect call/jmp [reg+disp] sites for the vtable slots of the targets')
    seen = set()
    for t in sorted(targets):
        for slot in abs_hits.get(t, []):
            base, n = run_of(slot)
            if n < 2 or base in seen:
                continue
            seen.add(base)
            idx = (slot - base) // 8
            sites = by_disp.get(8 * idx, [])
            print(f'  run {base:#x} len {n}, target {t:#x} at index {idx}: disp {8 * idx:#x} -> {len(sites)} sites')
            print(show_sites(sites, args.max))
    print()
    print(f'== indexed call/jmp dispatch sites: {len(scan.get("dispatch", []))}')
    for site, fs, mnem, ops in scan.get('dispatch', [])[:args.max]:
        print(f'  {site:#x} in {fname(fs)}: {mnem} {ops}')
    print()
    print('== reg-call counts for functions that contain a target (computed calls not resolved)')
    for t in sorted(targets):
        print(f'  {t:#x}: call reg sites in its own function: {scan["reg_calls"].get(t, 0)}')


if __name__ == '__main__':
    main()
